/*
 * pidgin4
 *
 * Pidgin is the legal property of its developers, whose names are too numerous
 * to list here.  Please refer to the COPYRIGHT file distributed with this
 * source distribution.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 */

/*
 * PIDGIN4_WINDOWS_SELFTEST=1: the M5 windows selftest. It runs each
 * module's synchronous selftest in turn, from an idle callback once the
 * buddy list is up, then quits: exit status 0, or 1 if a step failed.
 * With G_DEBUG=fatal-criticals any GTK/GLib/pidgin4 critical aborts it.
 *
 * PIDGIN4_WINDOWS_SELFTEST may also name a comma-separated subset of the
 * modules below (e.g. "prefs,log") instead of "1".
 *
 * Every window mapped while it runs is checked for its transient parent
 * (pidgin_window_set_secondary()): with /pidgin4/windows/secondary_transient
 * on, every window but the buddy list and the conversation windows must
 * have one (so Sway floats it). PIDGIN4_SELFTEST_SECONDARY=off runs with
 * the pref switched off (restored afterwards): then no window may be
 * transient for the buddy list or a conversation window; a window that
 * belongs to another one (a certificate over the certificate manager, a
 * plugin's configuration over Plugins) keeps its owner and is only
 * counted.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "debug.h"
#include "notify.h"
#include "request.h"
#include "prefs.h"
#include "util.h"

#include "gtkaccount.h"
#include "gtkblist.h"
#include "gtkcertmgr.h"
#include "gtkconvwin.h"
#include "gtkdebug.h"
#include "gtkdialogs.h"
#include "gtkutils.h"
#include "gtkft.h"
#include "gtklog.h"
#include "gtkplugin.h"
#include "gtkpounce.h"
#include "gtkprefs.h"
#include "gtkprivacy.h"
#include "gtkroomlist.h"
#include "gtksavedstatuses.h"
#include "gtksmiley.h"
#include "pidginabout.h"
#include "pidginomemo.h"
#include "pidginselftest.h"

static gboolean running = FALSE;
static int failures = 0;

/* The transient-parent check. */
static const char *current_module = "start";
static gboolean secondary_on = TRUE;
static int n_mapped = 0, n_transient = 0, n_owned = 0;
static GPtrArray *module_windows = NULL;   /* mapped during this module */

static void pidgin_secondary_selftest(void);

static const struct {
	const char *name;
	void (*run)(void);
} modules[] = {
	{ "prefs",    pidgin_prefs_selftest },
	{ "pounces",  pidgin_pounces_selftest },
	{ "status",   pidgin_status_selftest },
	{ "log",      pidgin_log_selftest },
	{ "privacy",  pidgin_privacy_selftest },
	{ "roomlist", pidgin_roomlist_selftest },
	{ "certmgr",  pidgin_certmgr_selftest },
	{ "xfers",    pidgin_xfers_selftest },
	{ "smileys",  pidgin_smileys_selftest },
	{ "plugins",  pidgin_plugins_selftest },
	{ "about",    pidgin_about_selftest },
	{ "omemo",    pidgin_omemo_selftest },
	{ "secondary", pidgin_secondary_selftest },
};

gboolean
pidgin_selftest_is_running(void)
{
	return running;
}

void
pidgin_selftest_log(const char *module, const char *format, ...)
{
	va_list args;
	char *msg;

	va_start(args, format);
	msg = g_strdup_vprintf(format, args);
	va_end(args);
	purple_debug_info("selftest", "%s: %s\n", module, msg);
	g_print("selftest: %s: %s\n", module, msg);
	g_free(msg);
}

void
pidgin_selftest_fail(const char *module, const char *format, ...)
{
	va_list args;
	char *msg;

	va_start(args, format);
	msg = g_strdup_vprintf(format, args);
	va_end(args);
	failures++;
	purple_debug_error("selftest", "%s: FAILED: %s\n", module, msg);
	g_printerr("selftest: %s: FAILED: %s\n", module, msg);
	g_free(msg);
}

static gboolean
timeout_cb(gpointer data)
{
	*(gboolean *)data = TRUE;
	return G_SOURCE_REMOVE;
}

void
pidgin_selftest_iterate(guint ms)
{
	gboolean done = FALSE;
	guint id = g_timeout_add(ms, timeout_cb, &done);

	while (!done)
		g_main_context_iteration(NULL, TRUE);
	(void)id;
}

gboolean
pidgin_selftest_wait(gboolean (*cond)(gpointer data), gpointer data,
                     guint timeout_ms)
{
	gboolean timed_out = FALSE;
	guint id = g_timeout_add(timeout_ms, timeout_cb, &timed_out);
	gboolean result;

	while (!(result = cond(data)) && !timed_out)
		g_main_context_iteration(NULL, TRUE);
	if (!timed_out)
		g_source_remove(id);
	return result;
}

void
pidgin_selftest_screenshot(GtkWidget *widget, const char *name)
{
	const char *dir = g_getenv("PIDGIN4_SELFTEST_SHOTS");
	GdkPaintable *paintable;
	GtkSnapshot *snapshot;
	GskRenderNode *node;
	GskRenderer *renderer;
	GdkTexture *texture;
	GtkNative *native;
	char *path;
	int w, h;

	if (dir == NULL || widget == NULL || !gtk_widget_get_realized(widget))
		return;
	w = gtk_widget_get_width(widget);
	h = gtk_widget_get_height(widget);
	native = gtk_widget_get_native(widget);
	if (w <= 0 || h <= 0 || native == NULL)
		return;

	paintable = gtk_widget_paintable_new(widget);
	snapshot = gtk_snapshot_new();
	gdk_paintable_snapshot(paintable, snapshot, w, h);
	node = gtk_snapshot_free_to_node(snapshot);
	renderer = gtk_native_get_renderer(native);
	if (node != NULL && renderer != NULL) {
		texture = gsk_renderer_render_texture(renderer, node,
			&GRAPHENE_RECT_INIT(0, 0, w, h));
		g_mkdir_with_parents(dir, 0700);
		path = g_strdup_printf("%s/%s.png", dir, name);
		gdk_texture_save_to_png(texture, path);
		g_free(path);
		g_object_unref(texture);
	}
	if (node != NULL)
		gsk_render_node_unref(node);
	g_object_unref(paintable);
}

/**************************************************************************
 * Transient parents (pidgin_window_set_secondary)
 **************************************************************************/

static gboolean
is_primary_window(GtkWindow *win)
{
	GList *l;

	if (GTK_WIDGET(win) == pidgin_blist_get_window())
		return TRUE;
	for (l = pidgin_conv_windows_get_list(); l != NULL; l = l->next)
		if (GTK_WIDGET(win) == pidgin_conv_window_get_window(l->data))
			return TRUE;
	return FALSE;
}

static void
window_mapped_cb(GtkWidget *widget, gpointer data)
{
	GtkWindow *win = GTK_WINDOW(widget);
	GtkWindow *parent = gtk_window_get_transient_for(win);
	const char *title = gtk_window_get_title(win);
	const char *name = gtk_widget_get_name(widget);

	if (!running || g_object_get_data(G_OBJECT(win), "pidgin-selftest-checked"))
		return;
	g_object_set_data(G_OBJECT(win), "pidgin-selftest-checked", GINT_TO_POINTER(1));
	if (title == NULL)
		title = "(untitled)";
	if (module_windows != NULL)
		g_ptr_array_add(module_windows, g_object_ref(win));

	if (is_primary_window(win)) {
		if (parent != NULL)
			pidgin_selftest_fail(current_module,
				"primary window \"%s\" is transient (it would float)", title);
		return;
	}

	n_mapped++;
	if (parent != NULL)
		n_transient++;

	if (secondary_on) {
		if (parent == NULL)
			pidgin_selftest_fail(current_module,
				"window \"%s\" (%s, %s) has no transient parent (it would tile)",
				title, name, G_OBJECT_TYPE_NAME(win));
		else
			pidgin_selftest_log(current_module, "window \"%s\" transient for \"%s\"",
				title, gtk_window_get_title(parent) ? gtk_window_get_title(parent) : "?");
		return;
	}

	/* The pref is off: nothing may be a dialog of the buddy list or of a
	 * conversation window just because it is a secondary window. The
	 * windows open at startup were made before the pref was switched off
	 * (it applies to windows opened afterwards). */
	if (purple_strequal(current_module, "start"))
		pidgin_selftest_log(current_module, "window \"%s\" was opened before "
			"the pref was switched off", title);
	else if (parent != NULL && is_primary_window(parent) && pidgin_window_is_secondary(win))
		pidgin_selftest_fail(current_module,
			"window \"%s\" is transient for \"%s\" with the pref off",
			title, gtk_window_get_title(parent) ? gtk_window_get_title(parent) : "?");
	else if (parent != NULL) {
		n_owned++;
		pidgin_selftest_log(current_module, "window \"%s\" keeps its owner \"%s\"",
			title, gtk_window_get_title(parent) ? gtk_window_get_title(parent) : "?");
	}
}

static void
toplevels_changed_cb(GListModel *model, guint position, guint removed,
                     guint added, gpointer data)
{
	guint i;

	for (i = position; i < position + added; i++) {
		GtkWindow *win = g_list_model_get_item(model, i);

		if (win == NULL)
			continue;
		if (!g_object_get_data(G_OBJECT(win), "pidgin-selftest-watched")) {
			g_object_set_data(G_OBJECT(win), "pidgin-selftest-watched", GINT_TO_POINTER(1));
			g_signal_connect(win, "map", G_CALLBACK(window_mapped_cb), NULL);
		}
		g_object_unref(win);
	}
}

/* The windows no other module opens: accounts, the debug window, the
 * buddy list's mini dialogs, the gtkdialogs.c requests, a notification;
 * and a dialog that stays usable while the buddy list is hidden into the
 * tray. (A conversation's request, which stays with the conversation
 * window, needs a connected account: PIDGIN4_PLUGINS_SELFTEST checks it.) */
static void request_cb(void *data, const char *text) { }

/* PIDGIN4_SELFTEST_HOLD=N: pause N seconds (e.g. for swaymsg -t get_tree). */
static void
hold(const char *what)
{
	const char *env = g_getenv("PIDGIN4_SELFTEST_HOLD");
	guint secs = env ? (guint)g_ascii_strtoull(env, NULL, 10) : 0;

	if (secs == 0)
		return;
	pidgin_selftest_log("secondary", "holding %u s: %s", secs, what);
	pidgin_selftest_iterate(secs * 1000);
}

static gboolean
window_visible_cond(gpointer data)
{
	return gtk_widget_get_mapped(GTK_WIDGET(data));
}

static void
pidgin_secondary_selftest(void)
{
	static const char *M = "secondary";
	static int handle;
	GtkWidget *blist = pidgin_blist_get_window();
	GtkWindow *prefs = NULL;
	guint i;

	module_windows = g_ptr_array_new_with_free_func(g_object_unref);

	pidgin_accounts_window_show();
	pidgin_account_dialog_show(PIDGIN_ADD_ACCOUNT_DIALOG, NULL);
	pidgin_debug_window_show();
	purple_blist_request_add_buddy(NULL, NULL, NULL, NULL);
	purple_blist_request_add_group();
	pidgin_dialogs_im();
	pidgin_dialogs_info();
	pidgin_dialogs_log();
	purple_request_input(&handle, "Selftest", "A request", NULL, NULL,
		FALSE, FALSE, NULL, "_OK", G_CALLBACK(request_cb),
		"_Cancel", G_CALLBACK(request_cb), NULL, NULL, NULL, NULL);
	purple_notify_formatted(&handle, "Selftest", "A notification", NULL,
		"<b>formatted</b>", NULL, NULL);
	pidgin_selftest_iterate(300);

	/* The buddy list hidden into the tray: open dialogs stay open and can
	 * be presented again; a new dialog still gets a parent. */
	pidgin_prefs_show();
	pidgin_selftest_iterate(300);
	hold("everything open");
	for (i = 0; i < module_windows->len; i++)
		if (purple_strequal(gtk_widget_get_name(module_windows->pdata[i]), "preferences"))
			prefs = module_windows->pdata[i];
	if (prefs == NULL) {
		pidgin_selftest_fail(M, "Preferences did not open");
	} else if (blist != NULL) {
		gtk_widget_set_visible(blist, FALSE);
		pidgin_selftest_iterate(300);
		if (!gtk_widget_get_visible(GTK_WIDGET(prefs)))
			pidgin_selftest_fail(M, "hiding the buddy list hid Preferences");
		gtk_window_present(prefs);
		if (!pidgin_selftest_wait(window_visible_cond, prefs, 2000))
			pidgin_selftest_fail(M, "Preferences is not mapped after presenting "
			                     "it with the buddy list hidden");
		else
			pidgin_selftest_log(M, "Preferences stays usable with the buddy list hidden");
		pidgin_about_show();
		pidgin_selftest_iterate(300);
		hold("buddy list hidden, Preferences presented again, About opened");
		gtk_window_present(GTK_WINDOW(blist));
		pidgin_selftest_iterate(300);
	}

	/* Close everything this module opened (newest first). */
	purple_request_close_with_handle(&handle);
	purple_notify_close_with_handle(&handle);
	pidgin_accounts_window_hide();
	pidgin_selftest_iterate(100);
	for (i = module_windows->len; i > 0; i--) {
		GtkWindow *w = module_windows->pdata[i - 1];

		if (GTK_WIDGET(w) != blist && gtk_widget_get_visible(GTK_WIDGET(w)))
			gtk_window_close(w);
	}
	pidgin_selftest_iterate(300);
	g_clear_pointer(&module_windows, g_ptr_array_unref);
}

static gboolean
module_selected(const char *spec, const char *name)
{
	char **names;
	gboolean found = FALSE;
	int i;

	if (spec == NULL || *spec == '\0' || purple_strequal(spec, "1"))
		return TRUE;
	names = g_strsplit(spec, ",", -1);
	for (i = 0; names[i] != NULL; i++)
		if (purple_strequal(g_strstrip(names[i]), name))
			found = TRUE;
	g_strfreev(names);
	return found;
}

static gboolean
run_cb(gpointer data)
{
	const char *spec = g_getenv("PIDGIN4_WINDOWS_SELFTEST");
	gsize i;
	gint64 start;

	GListModel *toplevels = gtk_window_get_toplevels();
	gulong toplevels_id;
	gboolean saved_secondary = purple_prefs_get_bool(PIDGIN_PREF_SECONDARY_TRANSIENT);

	running = TRUE;
	g_print("selftest: start\n");

	secondary_on = !purple_strequal(g_getenv("PIDGIN4_SELFTEST_SECONDARY"), "off");
	purple_prefs_set_bool(PIDGIN_PREF_SECONDARY_TRANSIENT, secondary_on);
	pidgin_selftest_log("secondary", "secondary windows transient: %s",
	                    secondary_on ? "on" : "off");
	toplevels_id = g_signal_connect(toplevels, "items-changed",
	                                G_CALLBACK(toplevels_changed_cb), NULL);
	/* The windows open at startup (buddy list, debug window, accounts). */
	for (i = 0; i < g_list_model_get_n_items(toplevels); i++) {
		GtkWidget *win = g_list_model_get_item(toplevels, i);

		if (gtk_widget_get_mapped(win))
			window_mapped_cb(win, NULL);
		g_object_unref(win);
	}
	toplevels_changed_cb(toplevels, 0, 0, g_list_model_get_n_items(toplevels), NULL);

	for (i = 0; i < G_N_ELEMENTS(modules); i++) {
		int before = failures;

		if (!module_selected(spec, modules[i].name))
			continue;
		current_module = modules[i].name;
		start = g_get_monotonic_time();
		pidgin_selftest_log(modules[i].name, "begin");
		modules[i].run();
		/* Let closed windows finish going away. */
		pidgin_selftest_iterate(50);
		pidgin_selftest_log(modules[i].name, "%s (%.1f s)",
			failures == before ? "ok" : "FAILED",
			(g_get_monotonic_time() - start) / 1e6);
	}
	current_module = "end";

	g_signal_handler_disconnect(toplevels, toplevels_id);
	purple_prefs_set_bool(PIDGIN_PREF_SECONDARY_TRANSIENT, saved_secondary);
	if (n_mapped == 0)
		pidgin_selftest_fail("secondary", "no secondary window was mapped");
	pidgin_selftest_log("secondary", "%d secondary window(s) mapped, %d transient "
		"(pref %s; %d kept an owner of their own)", n_mapped, n_transient,
		secondary_on ? "on" : "off", n_owned);
	running = FALSE;

	g_print("selftest: %s, %d failure(s)\n",
	        failures == 0 ? "PASSED" : "FAILED", failures);
	if (failures != 0)
		pidgin_application_set_exit_status(1);
	pidgin_application_quit();

	return G_SOURCE_REMOVE;
}

void
pidgin_windows_selftest_start(void)
{
	/* After the startup code has finished (blist shown, plugins loaded). */
	g_timeout_add(500, run_cb, NULL);
}
