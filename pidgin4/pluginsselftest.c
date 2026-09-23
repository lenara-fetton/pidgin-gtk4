/*
 * pidgin4: the UI plugins selftest (PIDGIN4_PLUGINS_SELFTEST=1, M7).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Loads every ported plugin from <prefix>/lib/pidgin4 (or the directory
 * in PIDGIN4_PLUGINS_SELFTEST_DIR, e.g. the build tree's plugins/),
 * drives conversations on the in-process selftest protocol
 * (tests/selftest-prpl.c) and checks what each plugin does, unloads them
 * all and quits with status 0 on success. It never signs a real account
 * in; the XMPP windows are opened with whatever (disabled) XMPP accounts
 * the profile has. Run it on a scratch copy of a profile: it writes logs,
 * index rows and <profile>/pidgin4/cap.db (see pidgin4/TESTING.md).
 *
 * Plugins that /pidgin4/plugins/loaded listed are already loaded when it
 * starts; it reports them, which is how the saved list is checked.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include <sqlite3.h>

#include "account.h"
#include "blist.h"
#include "connection.h"
#include "conversation.h"
#include "debug.h"
#include "plugin.h"
#include "pluginpref.h"
#include "prefs.h"
#include "request.h"
#include "server.h"
#include "signals.h"
#include "util.h"

#include "gtkblist.h"
#include "gtkconv.h"
#include "gtkconvwin.h"
#include "pidgincomposeentry.h"
#include "pidginmessage.h"
#include "pidginmessageview.h"
#include "pluginsselftest.h"
#include "tests/selftest-prpl.h"

#define ST_USER "plugins-selftest@example.invalid"

static int failures = 0;
static int checks = 0;
static PurpleAccount *st_account = NULL;
static GHashTable *saved_prefs = NULL;   /* pref name -> GValue-ish copy */

#define CHECK(cond, ...) G_STMT_START { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		g_printerr("PIDGIN4_PLUGINS_SELFTEST: FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
		g_printerr(__VA_ARGS__); \
		g_printerr("\n"); \
	} \
} G_STMT_END

#define SAY(...) G_STMT_START { \
	g_print("PIDGIN4_PLUGINS_SELFTEST: "); \
	g_print(__VA_ARGS__); \
	g_print("\n"); \
} G_STMT_END

/* The ported plugins: file name (without .so) and plugin id. */
static const struct {
	const char *file;
	const char *id;
} ported[] = {
	{ "history", "gtk-history" },
	{ "markerline", "gtk-plugin_pack-markerline" },
	{ "timestamp_format", "core-timestamp_format" },
	{ "notify", "gtk-x11-notify" },
	{ "cap", "gtk-g-off_-cap" },
	{ "convcolors", "gtk-plugin_pack-convcolors" },
	{ "spellchk", "gtk-spellcheck" },
	{ "sendbutton", "gtksendbutton" },
	{ "gtkbuddynote", "gtkbuddynote" },
	{ "iconaway", "gtk-iconaway" },
	{ "relnot", "gtk-relnot" },
	{ "timestamp", "gtk-timestamp" },
	{ "xmppconsole", "gtk-xmpp" },
	{ "xmppdisco", "gtk-xmppdisco" },
};

static PurplePlugin *loaded[G_N_ELEMENTS(ported)];

/**************************************************************************
 * Helpers
 **************************************************************************/

static void
spin(guint ms)
{
	pidgin_selftest_spin(ms);
}

/* Prefs are restored at the end (a bool, int or string each). */
typedef struct {
	PurplePrefType type;
	gboolean b;
	int i;
	char *s;
} SavedPref;

static void
saved_pref_free(gpointer data)
{
	SavedPref *sp = data;

	g_free(sp->s);
	g_free(sp);
}

static void
save_pref(const char *name)
{
	SavedPref *sp;

	if (g_hash_table_contains(saved_prefs, name) || !purple_prefs_exists(name))
		return;
	sp = g_new0(SavedPref, 1);
	sp->type = purple_prefs_get_type(name);
	if (sp->type == PURPLE_PREF_BOOLEAN)
		sp->b = purple_prefs_get_bool(name);
	else if (sp->type == PURPLE_PREF_INT)
		sp->i = purple_prefs_get_int(name);
	else if (sp->type == PURPLE_PREF_STRING)
		sp->s = g_strdup(purple_prefs_get_string(name));
	g_hash_table_insert(saved_prefs, g_strdup(name), sp);
}

static void
set_bool(const char *name, gboolean value)
{
	save_pref(name);
	purple_prefs_set_bool(name, value);
}

static void
set_int(const char *name, int value)
{
	save_pref(name);
	purple_prefs_set_int(name, value);
}

static void
set_string(const char *name, const char *value)
{
	save_pref(name);
	purple_prefs_set_string(name, value);
}

static void
restore_prefs(void)
{
	GHashTableIter iter;
	gpointer key, value;

	g_hash_table_iter_init(&iter, saved_prefs);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		SavedPref *sp = value;

		if (!purple_prefs_exists(key))
			continue;
		if (sp->type == PURPLE_PREF_BOOLEAN)
			purple_prefs_set_bool(key, sp->b);
		else if (sp->type == PURPLE_PREF_INT)
			purple_prefs_set_int(key, sp->i);
		else if (sp->type == PURPLE_PREF_STRING)
			purple_prefs_set_string(key, sp->s);
	}
}

static PurplePlugin *
plugin(const char *file)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(ported); i++)
		if (g_str_equal(ported[i].file, file))
			return loaded[i];
	return NULL;
}

static PidginMessageView *
view_of(PurpleConversation *conv)
{
	return PIDGIN_MESSAGE_VIEW(pidgin_conv_get_message_view(PIDGIN_CONVERSATION(conv)));
}

static guint
n_messages(PurpleConversation *conv)
{
	return g_list_model_get_n_items(pidgin_message_view_get_model(view_of(conv)));
}

static PidginMessage *
nth_message(PurpleConversation *conv, guint n)
{
	PidginMessage *m = g_list_model_get_item(pidgin_message_view_get_model(view_of(conv)), n);

	if (m != NULL)
		g_object_unref(m);  /* the store keeps it */
	return m;
}

static guint
count_with_class(PurpleConversation *conv, const char *css_class, GString *text)
{
	guint i, n = n_messages(conv), count = 0;

	for (i = 0; i < n; i++) {
		PidginMessage *m = nth_message(conv, i);

		if (pidgin_message_has_css_class(m, css_class)) {
			count++;
			if (text != NULL && pidgin_message_get_kind(m) == PIDGIN_MESSAGE_KIND_NORMAL) {
				g_string_append(text, pidgin_message_get_plain_text(m));
				g_string_append_c(text, '\n');
			}
		}
	}
	return count;
}

static PurpleConversation *
new_im(const char *who)
{
	PurpleConversation *conv = purple_conversation_new(PURPLE_CONV_TYPE_IM, st_account, who);

	spin(50);
	return conv;
}

static void
recv_im(const char *who, const char *text)
{
	serv_got_im(purple_account_get_connection(st_account), who, text,
	            PURPLE_MESSAGE_RECV, time(NULL));
	spin(20);
}

/* A visible label with @css_class under @widget, depth first. */
static GtkWidget *
find_label(GtkWidget *widget, const char *css_class)
{
	GtkWidget *child;

	if (GTK_IS_LABEL(widget) && gtk_widget_has_css_class(widget, css_class) &&
	    gtk_widget_get_visible(widget) && *gtk_label_get_text(GTK_LABEL(widget)) != '\0')
		return widget;
	for (child = gtk_widget_get_first_child(widget); child != NULL;
	     child = gtk_widget_get_next_sibling(child)) {
		GtkWidget *found = find_label(child, css_class);

		if (found != NULL)
			return found;
	}
	return NULL;
}

static GtkWindow *
find_window(const char *title)
{
	GListModel *toplevels = gtk_window_get_toplevels();
	guint i, n = g_list_model_get_n_items(toplevels);

	for (i = 0; i < n; i++) {
		GtkWindow *w = g_list_model_get_item(toplevels, i);
		const char *t = gtk_window_get_title(w);

		g_object_unref(w);
		if (t != NULL && g_str_equal(t, title))
			return w;
	}
	return NULL;
}

static gboolean
run_plugin_action(PurplePlugin *p, const char *label)
{
	GList *actions, *l;
	gboolean ran = FALSE;

	if (p == NULL || p->info->actions == NULL)
		return FALSE;
	actions = p->info->actions(p, NULL);
	for (l = actions; l != NULL; l = l->next) {
		PurplePluginAction *action = l->data;

		if (action == NULL)
			continue;
		if (!ran && purple_strequal(action->label, label)) {
			action->plugin = p;
			action->context = NULL;
			action->callback(action);
			ran = TRUE;
		}
		purple_plugin_action_free(action);
	}
	g_list_free(actions);
	return ran;
}

static gint64
cap_rows(void)
{
	char *path = g_build_filename(pidgin_user_dir(), "cap.db", NULL);
	sqlite3 *db = NULL;
	sqlite3_stmt *stmt = NULL;
	gint64 rows = -1;

	if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK &&
	    sqlite3_prepare_v2(db, "SELECT (SELECT count(*) FROM cap_msg_count) + "
	                           "(SELECT count(*) FROM cap_status_count)", -1, &stmt, NULL) == SQLITE_OK &&
	    sqlite3_step(stmt) == SQLITE_ROW)
		rows = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);
	sqlite3_close(db);
	g_free(path);
	return rows;
}

/**************************************************************************
 * Loading
 **************************************************************************/

static void
load_all(void)
{
	const char *dir = g_getenv("PIDGIN4_PLUGINS_SELFTEST_DIR");
	const char *skip_env = g_getenv("PIDGIN4_PLUGINS_SELFTEST_SKIP");
	char **skip = g_strsplit(skip_env ? skip_env : "", ",", -1);
	GString *already = g_string_new(NULL);
	guint i;

	if (dir == NULL || *dir == '\0')
		dir = LIBDIR;

	for (i = 0; i < G_N_ELEMENTS(ported); i++) {
		char *path = g_build_filename(dir, ported[i].file, NULL);
		char *file = g_strconcat(path, "." G_MODULE_SUFFIX, NULL);
		PurplePlugin *p = purple_plugins_find_with_id(ported[i].id);

		/* PIDGIN4_PLUGINS_SELFTEST_SKIP=name,name: leave these out */
		if (g_strv_contains((const char * const *)skip, ported[i].file)) {
			SAY("skipping %s", ported[i].file);
			loaded[i] = NULL;
			g_free(path);
			g_free(file);
			continue;
		}
		if (p != NULL && purple_plugin_is_loaded(p)) {
			g_string_append_printf(already, " %s", ported[i].file);
		} else if (p == NULL || !purple_strequal(p->path, file)) {
			PurplePlugin *probed = purple_plugins_find_with_filename(file);

			if (probed == NULL)
				probed = purple_plugin_probe(file);
			if (probed != NULL)
				p = probed;
		}
		CHECK(p != NULL, "%s not found in %s", ported[i].file, dir);
		if (p != NULL && !purple_plugin_is_loaded(p)) {
			CHECK(purple_plugin_load(p), "%s did not load: %s", ported[i].file,
			      p->error ? p->error : "");
		}
		CHECK(p == NULL || purple_plugin_is_loaded(p), "%s not loaded", ported[i].file);
		CHECK(p == NULL || purple_strequal(purple_plugin_get_id(p), ported[i].id),
		      "%s has id %s", ported[i].file, p ? purple_plugin_get_id(p) : "");
		loaded[i] = (p != NULL && purple_plugin_is_loaded(p)) ? p : NULL;
		g_free(path);
		g_free(file);
	}
	SAY("loaded from /pidgin4/plugins/loaded at startup:%s",
	    already->len ? already->str : " (none)");
	g_string_free(already, TRUE);
	g_strfreev(skip);
}

/**************************************************************************
 * Per-plugin checks
 **************************************************************************/

static void
test_history(void)
{
	char *who = g_strdup_printf("hist-%" G_GINT64_FORMAT "@example.invalid",
	                            g_get_real_time());
	PurpleConversation *conv;
	PidginMessage *last;
	GString *text;
	guint rows, i;

	if (plugin("history") == NULL)
		return;

	/* A first conversation has no log: nothing is added. */
	conv = new_im(who);
	CHECK(count_with_class(conv, "history", NULL) == 0, "history rows without a log");
	purple_conversation_set_logging(conv, TRUE);
	recv_im(who, "history line <b>one</b>");
	purple_conv_im_send(PURPLE_CONV_IM(conv), "history line two");
	spin(20);
	recv_im(who, "history line three");
	purple_conversation_destroy(conv);
	spin(50);

	/* All of the last log */
	set_int(PIDGIN4_PREFS_ROOT "/plugins/history/lines", 100);
	conv = new_im(who);
	text = g_string_new(NULL);
	rows = count_with_class(conv, "history", text);
	/* header + 3 lines + separator */
	CHECK(rows == 5, "%u history rows: %s", rows, text->str);
	CHECK(strstr(text->str, "history line one") != NULL &&
	      strstr(text->str, "history line two") != NULL &&
	      strstr(text->str, "history line three") != NULL, "history text: %s", text->str);
	CHECK(n_messages(conv) == rows, "live rows before the history");
	CHECK(pidgin_message_get_kind(nth_message(conv, rows - 1)) == PIDGIN_MESSAGE_KIND_MARKER,
	      "no separator last");
	CHECK(pidgin_message_get_flags(nth_message(conv, 2)) & PURPLE_MESSAGE_SEND,
	      "the sent line isn't SEND");
	g_string_free(text, TRUE);
	/* live messages come after it */
	recv_im(who, "live after history");
	last = nth_message(conv, n_messages(conv) - 1);
	CHECK(n_messages(conv) > rows && !pidgin_message_has_css_class(last, "history") &&
	      strstr(pidgin_message_get_plain_text(last), "live after history") != NULL,
	      "live message placement");
	for (i = 0; i < rows; i++)
		CHECK(pidgin_message_has_css_class(nth_message(conv, i), "history"),
		      "row %u isn't history", i);
	purple_conversation_destroy(conv);
	spin(50);

	/* The tail only (the index gives the offset of the last lines). The
	 * last log is now the one with "live after history". */
	set_int(PIDGIN4_PREFS_ROOT "/plugins/history/lines", 1);
	conv = new_im(who);
	text = g_string_new(NULL);
	rows = count_with_class(conv, "history", text);
	CHECK(rows == 3 && strstr(text->str, "live after history") != NULL,
	      "%u tail rows: %s", rows, text->str);
	g_string_free(text, TRUE);
	purple_conversation_destroy(conv);
	spin(50);
	g_free(who);
}

static void
test_markerline_and_timestamps(void)
{
	PurpleConversation *a, *b;
	PidginConversation *gtka, *gtkb;
	PidginWindow *win;
	GtkWidget *label;
	char *expected, *ts;
	time_t now = time(NULL);

	a = new_im("marker-a@example.invalid");
	b = new_im("marker-b@example.invalid");
	gtka = PIDGIN_CONVERSATION(a);
	gtkb = PIDGIN_CONVERSATION(b);
	if (gtka->win != gtkb->win) {
		pidgin_conv_window_remove_gtkconv(gtkb->win, gtkb);
		pidgin_conv_window_add_gtkconv(gtka->win, gtkb);
	}
	win = gtka->win;
	recv_im("marker-a@example.invalid", "before the marker");
	recv_im("marker-b@example.invalid", "b's message");

	if (plugin("markerline") != NULL) {
		set_bool("/plugins/gtk/gtk-plugin_pack-markerline/ims", TRUE);
		/* Switching away from a marks it */
		pidgin_conv_window_switch_gtkconv(win, gtka);
		spin(20);
		pidgin_message_view_remove_marker(view_of(a));
		pidgin_conv_window_switch_gtkconv(win, gtkb);
		spin(20);
		CHECK(pidgin_message_view_get_marker(view_of(a)) != NULL, "no marker after switch");
		/* A focus change (is-active, as the compositor reports it) marks
		 * the active one */
		pidgin_message_view_remove_marker(view_of(b));
		g_object_notify(G_OBJECT(win->window), "is-active");
		if (!gtk_window_is_active(GTK_WINDOW(win->window)))
			CHECK(pidgin_message_view_get_marker(view_of(b)) != NULL, "no marker on focus-out");
		recv_im("marker-b@example.invalid", "after the marker");
		CHECK(pidgin_message_get_kind(nth_message(b, n_messages(b) - 2)) ==
		      PIDGIN_MESSAGE_KIND_MARKER, "marker isn't before the new message");
	}

	if (plugin("timestamp_format") != NULL) {
		PidginMessage *msg;
		GMenu *section;
		GtkWidget *view_widget = pidgin_conv_get_message_view(gtkb);

		set_bool(PIDGIN_PREFS_ROOT "/conversations/show_timestamps", TRUE);
		set_string("/plugins/gtk/timestamp_format/force", "force24");
		set_string("/plugins/gtk/timestamp_format/use_dates/conversation", "automatic");
		expected = g_strdup_printf("(%s)", purple_utf8_strftime("%H:%M:%S", localtime(&now)));
		ts = pidgin_message_view_format_timestamp(b, now, FALSE);
		CHECK(purple_strequal(ts, expected), "format %s, expected %s", ts, expected);
		g_free(ts);
		g_free(expected);
		ts = pidgin_message_view_format_timestamp(b, now, TRUE);
		expected = g_strdup_printf("(%s)",
			purple_utf8_strftime("%Y-%m-%d %H:%M:%S", localtime(&now)));
		CHECK(purple_strequal(ts, expected), "date format %s, expected %s", ts, expected);
		g_free(ts);
		g_free(expected);

		/* a row's label */
		gtk_widget_set_visible(win->window, TRUE);
		pidgin_conv_window_switch_gtkconv(win, gtkb);
		recv_im("marker-b@example.invalid", "a timestamped message");
		spin(200);
		label = find_label(view_widget, "timestamp");
		CHECK(label != NULL, "no timestamp label");
		if (label != NULL) {
			const char *t = gtk_label_get_text(GTK_LABEL(label));

			CHECK(g_regex_match_simple("^\\(([0-9]{4}-[0-9]{2}-[0-9]{2} )?[0-9]{2}:[0-9]{2}:[0-9]{2}\\)$",
			                           t, 0, 0), "row timestamp \"%s\"", t);
		}
		set_string("/plugins/gtk/timestamp_format/force", "force12");
		spin(50);
		label = find_label(view_widget, "timestamp");
		CHECK(label != NULL && (strstr(gtk_label_get_text(GTK_LABEL(label)), "AM") ||
		                        strstr(gtk_label_get_text(GTK_LABEL(label)), "PM") ||
		                        !g_regex_match_simple("^\\([0-9]{2}:", gtk_label_get_text(GTK_LABEL(label)), 0, 0)),
		      "12 hour format not applied: %s", label ? gtk_label_get_text(GTK_LABEL(label)) : "");

		/* the context menu item */
		msg = nth_message(b, n_messages(b) - 1);
		section = g_menu_new();
		g_signal_emit_by_name(view_widget, "populate-menu", msg, section);
		CHECK(g_menu_model_get_n_items(G_MENU_MODEL(section)) >= 1, "no menu item");
		CHECK(g_action_group_has_action(G_ACTION_GROUP(pidgin_application_get()),
		                                "timestamp-format-options"), "no app action");
		g_object_unref(section);
		g_action_group_activate_action(G_ACTION_GROUP(pidgin_application_get()),
		                               "timestamp-format-options", NULL);
		spin(100);
		{
			GtkWindow *w = find_window(_("Message Timestamp Formats"));

			SAY("timestamp_format options window: %s", w ? "shown" : "not shown (no M5 pref frames)");
			if (w != NULL)
				gtk_window_destroy(w);
		}
	}

	purple_conversation_destroy(a);
	purple_conversation_destroy(b);
	spin(50);
}

static void
test_notify(void)
{
	const char *who = "notify@example.invalid";
	PurpleConversation *conv;
	PidginWindow *win;
	const char *title;

	if (plugin("notify") == NULL)
		return;

	set_bool("/plugins/gtk/X11/notify/type_im", TRUE);
	set_bool("/plugins/gtk/X11/notify/type_focused", TRUE);
	set_bool("/plugins/gtk/X11/notify/method_string", TRUE);
	set_string("/plugins/gtk/X11/notify/title_string", "(*)");
	set_bool("/plugins/gtk/X11/notify/method_count", TRUE);
	set_bool("/plugins/gtk/X11/notify/notify_send", TRUE);

	conv = new_im(who);
	win = PIDGIN_CONVERSATION(conv)->win;
	pidgin_conv_window_switch_gtkconv(win, PIDGIN_CONVERSATION(conv));
	recv_im(who, "notify me");
	recv_im(who, "and again");
	title = gtk_window_get_title(GTK_WINDOW(win->window));
	CHECK(g_str_has_prefix(title, "(*)[2] "), "title \"%s\"", title);
	/* a status/typing update resets the title in gtkconv; the plugin puts
	 * the prefix back */
	purple_conversation_update(conv, PURPLE_CONV_UPDATE_TYPING);
	spin(50);
	title = gtk_window_get_title(GTK_WINDOW(win->window));
	CHECK(g_str_has_prefix(title, "(*)[2] "), "title after an update \"%s\"", title);
	/* sending removes it */
	purple_conv_im_send(PURPLE_CONV_IM(conv), "read it");
	spin(20);
	title = gtk_window_get_title(GTK_WINDOW(win->window));
	CHECK(!g_str_has_prefix(title, "(*)"), "title after sending \"%s\"", title);
	purple_conversation_destroy(conv);
	spin(50);
}

static void
test_cap_and_buddynote(void)
{
	const char *who = "cap-buddy@example.invalid";
	PurpleBuddy *buddy;
	PurpleGroup *group;
	PurpleConversation *conv;
	char *tip, *path;
	gint64 before;

	group = purple_group_new("Plugins Selftest");
	purple_blist_add_group(group, NULL);
	buddy = purple_buddy_new(st_account, who, "Cap Buddy");
	purple_blist_add_buddy(buddy, NULL, group, NULL);

	if (plugin("cap") != NULL) {
		path = g_build_filename(pidgin_user_dir(), "cap.db", NULL);
		CHECK(g_file_test(path, G_FILE_TEST_IS_REGULAR), "no %s", path);
		g_free(path);
		before = cap_rows();
		conv = new_im(who);
		purple_conv_im_send(PURPLE_CONV_IM(conv), "are you there");
		spin(20);
		recv_im(who, "yes");
		CHECK(cap_rows() > before && before >= 0, "cap rows %" G_GINT64_FORMAT " -> %"
		      G_GINT64_FORMAT, before, cap_rows());
		tip = pidgin_blist_get_tooltip_text((PurpleBlistNode *)buddy, TRUE);
		CHECK(tip != NULL && strstr(tip, _("Response Probability:")) != NULL, "tooltip %s", tip);
		g_free(tip);
		purple_conversation_destroy(conv);
		spin(20);
	}

	if (plugin("gtkbuddynote") != NULL) {
		GList *menu = NULL, *l;
		gboolean found = FALSE;

		CHECK(purple_plugin_is_loaded(purple_plugins_find_with_id("core-plugin_pack-buddynote")),
		      "core buddynote not loaded");
		purple_signal_emit(purple_blist_get_handle(), "blist-node-extended-menu",
		                   (PurpleBlistNode *)buddy, &menu);
		for (l = menu; l != NULL; l = l->next) {
			PurpleMenuAction *act = l->data;

			if (act != NULL && purple_strequal(act->label, _("Edit Notes...")))
				found = TRUE;
			purple_menu_action_free(act);
		}
		g_list_free(menu);
		CHECK(found, "no Edit Notes... item");
		purple_blist_node_set_string((PurpleBlistNode *)buddy, "notes", "likes <tea>");
		tip = pidgin_blist_get_tooltip_text((PurpleBlistNode *)buddy, TRUE);
		CHECK(tip != NULL && strstr(tip, "Buddy Note") && strstr(tip, "likes &lt;tea&gt;"),
		      "tooltip %s", tip);
		g_free(tip);
	}

	purple_blist_remove_buddy(buddy);
	purple_blist_remove_group(group);
}

static void
test_compose_plugins(void)
{
	const char *who = "compose@example.invalid";
	PurpleConversation *conv = new_im(who);
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);
	PidginComposeEntry *entry = PIDGIN_COMPOSE_ENTRY(pidgin_conv_get_compose_entry(gtkconv));
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(entry));
	GtkTextIter end;
	PidginMessage *last;
	char *text;

	if (plugin("sendbutton") != NULL) {
		CHECK(gtk_widget_get_visible(pidgin_conv_get_send_button(gtkconv)),
		      "send button hidden");
		CHECK(!gtk_widget_get_sensitive(pidgin_conv_get_send_button(gtkconv)),
		      "send button sensitive with an empty entry");
	}

	if (plugin("spellchk") != NULL) {
		/* typing a space after a word corrects it */
		gtk_text_buffer_set_text(buffer, "", -1);
		gtk_text_buffer_get_end_iter(buffer, &end);
		gtk_text_buffer_insert_interactive(buffer, &end, "i think teh", -1, TRUE);
		gtk_text_buffer_get_end_iter(buffer, &end);
		gtk_text_buffer_insert_interactive(buffer, &end, " ", -1, TRUE);
		text = pidgin_compose_entry_get_text(entry);
		CHECK(purple_strequal(text, "i think the "), "typed: \"%s\"", text);
		g_free(text);

		if (plugin("sendbutton") != NULL)
			CHECK(gtk_widget_get_sensitive(pidgin_conv_get_send_button(gtkconv)),
			      "send button not sensitive");

		/* the last word is corrected on send, which is held back once */
		gtk_text_buffer_get_end_iter(buffer, &end);
		gtk_text_buffer_insert_interactive(buffer, &end, "adn", -1, TRUE);
		CHECK(!pidgin_compose_entry_send(entry), "sent without the correction");
		text = pidgin_compose_entry_get_text(entry);
		CHECK(purple_strequal(text, "i think the and"), "corrected: \"%s\"", text);
		g_free(text);
		CHECK(pidgin_compose_entry_send(entry), "second send");
		spin(20);
		last = nth_message(conv, n_messages(conv) - 1);
		CHECK(last != NULL && strstr(pidgin_message_get_plain_text(last), "i think the and"),
		      "sent \"%s\"", last ? pidgin_message_get_plain_text(last) : "");
	}

	if (plugin("convcolors") != NULL) {
		GtkWidget *view = pidgin_conv_get_message_view(gtkconv);

		CHECK(gtk_widget_has_css_class(view, "convcolors-ims"), "no convcolors class");
		set_bool("/plugins/gtk/gtk-plugin_pack-convcolors/ims", FALSE);
		CHECK(!gtk_widget_has_css_class(view, "convcolors-ims"), "convcolors class stays");
		set_bool("/plugins/gtk/gtk-plugin_pack-convcolors/ims", TRUE);
		recv_im(who, "<font color=\"#00ff00\">coloured</font>");
		last = nth_message(conv, n_messages(conv) - 1);
		CHECK(strstr(pidgin_message_get_html(last), "00ff00") == NULL ||
		      !purple_prefs_get_bool("/plugins/gtk/gtk-plugin_pack-convcolors/ignore_incoming"),
		      "incoming formatting kept: %s", pidgin_message_get_html(last));
	}

	if (plugin("timestamp") != NULL) {
		set_int("/plugins/gtk/timestamp/interval", 60 * 1000);
		/* a new conversation starts with a time row */
		purple_conversation_destroy(conv);
		spin(20);
		conv = new_im(who);
		recv_im(who, "first message");
		CHECK(count_with_class(conv, "timestamp-plugin", NULL) == 1, "%u time rows",
		      count_with_class(conv, "timestamp-plugin", NULL));
		recv_im(who, "second message");
		/* (two if a minute boundary passed in between) */
		CHECK(count_with_class(conv, "timestamp-plugin", NULL) <= 2, "time row repeated");
	}

	purple_conversation_destroy(conv);
	spin(50);
}

static void
test_xmpp_windows(void)
{
	GtkWindow *w;

	if (plugin("xmppconsole") != NULL) {
		CHECK(run_plugin_action(plugin("xmppconsole"), _("XMPP Console")), "no console action");
		spin(100);
		w = find_window(_("XMPP Console"));
		CHECK(w != NULL, "no console window");
		/* A dialog of the buddy list, so Sway floats it. */
		CHECK(w == NULL || gtk_window_get_transient_for(w) != NULL,
		      "the console window has no transient parent");
		if (w != NULL)
			gtk_window_destroy(w);
		spin(50);
	}
	if (plugin("xmppdisco") != NULL) {
		CHECK(run_plugin_action(plugin("xmppdisco"), _("XMPP Service Discovery")),
		      "no disco action");
		spin(100);
		w = find_window(_("Service Discovery"));
		CHECK(w != NULL, "no disco window");
		CHECK(w == NULL || gtk_window_get_transient_for(w) != NULL,
		      "the disco window has no transient parent");
		if (w != NULL)
			gtk_window_destroy(w);
		spin(50);
	}
}

/* pidgin_window_set_secondary(): a request that belongs to a conversation
 * is transient for the conversation window, the others for the buddy
 * list. */
static void request_cb(void *data, const char *text) { }

static void
test_request_parents(void)
{
	static int handle;
	PurpleConversation *conv = new_im("request@example.invalid");
	PidginWindow *win;
	GtkWindow *w;

	purple_conversation_present(conv);
	spin(100);
	win = pidgin_conv_get_window(PIDGIN_CONVERSATION(conv));
	purple_request_input(&handle, "Conversation request", NULL, NULL, NULL,
		FALSE, FALSE, NULL, "_OK", G_CALLBACK(request_cb), "_Cancel",
		G_CALLBACK(request_cb), st_account, "request@example.invalid", conv, NULL);
	purple_request_input(&handle, "Other request", NULL, NULL, NULL,
		FALSE, FALSE, NULL, "_OK", G_CALLBACK(request_cb), "_Cancel",
		G_CALLBACK(request_cb), st_account, NULL, NULL, NULL);
	spin(100);

	w = find_window("Conversation request");
	CHECK(w != NULL && win != NULL &&
	      GTK_WIDGET(gtk_window_get_transient_for(w)) == pidgin_conv_window_get_window(win),
	      "a conversation's request is not transient for its conversation window");
	w = find_window("Other request");
	CHECK(w != NULL &&
	      GTK_WIDGET(gtk_window_get_transient_for(w)) == pidgin_blist_get_window(),
	      "a request without a conversation is not transient for the buddy list");

	purple_request_close_with_handle(&handle);
	purple_conversation_destroy(conv);
	spin(50);
}

/* Every plugin's configuration: the GTK frames in a window, the
 * PurplePluginPrefFrames built and freed (M5 turns those into widgets). */
static void
test_config_frames(void)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(ported); i++) {
		PurplePlugin *p = loaded[i];

		if (p == NULL)
			continue;
		if (p->info->ui_info != NULL) {
			/* PidginPluginUiInfo starts with get_config_frame */
			GtkWidget *(**get_frame)(PurplePlugin *) = p->info->ui_info;
			GtkWidget *frame = *get_frame ? (*get_frame)(p) : NULL;
			GtkWidget *window;

			CHECK(frame != NULL, "%s: no config frame", ported[i].file);
			if (frame == NULL)
				continue;
			window = gtk_window_new();
			gtk_window_set_child(GTK_WINDOW(window), frame);
			gtk_window_present(GTK_WINDOW(window));
			spin(100);
			gtk_window_destroy(GTK_WINDOW(window));
			spin(20);
		}
		if (p->info->prefs_info != NULL && p->info->prefs_info->get_plugin_pref_frame != NULL) {
			PurplePluginPrefFrame *frame = p->info->prefs_info->get_plugin_pref_frame(p);

			CHECK(frame != NULL && purple_plugin_pref_frame_get_prefs(frame) != NULL,
			      "%s: empty pref frame", ported[i].file);
			if (frame != NULL)
				purple_plugin_pref_frame_destroy(frame);
		}
	}
}

static void
unload_all(void)
{
	guint i;

	for (i = G_N_ELEMENTS(ported); i-- > 0; ) {
		if (loaded[i] == NULL || !purple_plugin_is_loaded(loaded[i]))
			continue;
		CHECK(purple_plugin_unload(loaded[i]), "%s did not unload", ported[i].file);
	}
	spin(50);
}

static gboolean
selftest_run(gpointer data)
{
	PurpleConversation *conv;
	GList *before_loaded2;
	GList *after_loaded2;

	saved_prefs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, saved_pref_free);
	before_loaded2 = purple_prefs_get_path_list(PIDGIN_PREFS_ROOT "/plugins/loaded");

	CHECK(pidgin_selftest_prpl_register() != NULL, "selftest prpl didn't load");
	st_account = pidgin_selftest_account_new(ST_USER);
	CHECK(purple_account_is_connected(st_account), "the selftest account didn't connect");
	if (!purple_account_is_connected(st_account))
		goto done;

	/* Before loading: logging on in HTML (history), relnot not due,
	 * placement in one window. */
	set_bool("/purple/logging/log_ims", TRUE);
	set_bool("/purple/logging/log_chats", TRUE);
	set_string("/purple/logging/format", "html");
	set_string(PIDGIN_PREFS_ROOT "/conversations/im/hide_new", "never");
	if (purple_prefs_exists("/plugins/gtk/relnot/last_check"))
		set_int("/plugins/gtk/relnot/last_check", (int)time(NULL));
	else {
		purple_prefs_add_none("/plugins/gtk/relnot");
		purple_prefs_add_int("/plugins/gtk/relnot/last_check", (int)time(NULL));
	}

	load_all();
	spin(100);

	test_history();
	test_markerline_and_timestamps();
	test_notify();
	test_cap_and_buddynote();
	test_compose_plugins();
	test_xmpp_windows();
	test_request_parents();
	test_config_frames();

	/* A conversation open while everything unloads */
	conv = new_im("unload@example.invalid");
	recv_im("unload@example.invalid", "still here");
	unload_all();
	if (plugin("sendbutton") != NULL)
		CHECK(!gtk_widget_get_visible(pidgin_conv_get_send_button(PIDGIN_CONVERSATION(conv))),
		      "send button stays after unload");
	recv_im("unload@example.invalid", "after unloading");
	purple_conversation_destroy(conv);
	spin(50);

done:
	while (purple_get_conversations() != NULL)
		purple_conversation_destroy(purple_get_conversations()->data);
	spin(50);
	pidgin_selftest_account_remove(st_account);
	st_account = NULL;
	pidgin_selftest_prpl_unregister();
	restore_prefs();

	/* Pidgin 2's plugin list is never touched (profile contract, rule 3) */
	after_loaded2 = purple_prefs_get_path_list(PIDGIN_PREFS_ROOT "/plugins/loaded");
	CHECK(g_list_length(before_loaded2) == g_list_length(after_loaded2),
	      "/pidgin/plugins/loaded changed");
	g_list_free_full(before_loaded2, g_free);
	g_list_free_full(after_loaded2, g_free);
	g_hash_table_destroy(saved_prefs);

	if (failures == 0)
		SAY("PASS (%d checks)", checks);
	else
		SAY("%d of %d checks FAILED", failures, checks);
	pidgin_application_set_exit_status(failures == 0 ? 0 : 1);
	pidgin_application_quit();
	return G_SOURCE_REMOVE;
}

void
pidgin_ported_plugins_selftest(void)
{
	if (g_getenv("PIDGIN4_PLUGINS_SELFTEST") == NULL)
		return;
	/* After startup settles (the buddy list is shown). */
	g_timeout_add(500, selftest_run, NULL);
}
