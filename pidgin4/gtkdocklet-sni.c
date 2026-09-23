/*
 * pidgin4: the docklet UI ops on a StatusNotifierItem (replaces
 * pidgin/gtkdocklet-gtk.c, the GtkStatusIcon version).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "debug.h"

#include "gtkblist.h"
#include "gtkdocklet.h"
#include "pidgindbusmenu.h"
#include "pidginsni.h"

/*
 * The item registers with whatever StatusNotifierWatcher is on the session
 * bus (Waybar's tray under Sway; the AppIndicator extension under GNOME).
 * pidgin_docklet_embedded() runs when the watcher accepts it, and
 * pidgin_docklet_remove() when the watcher goes away, so the buddy list
 * can only hide into a tray that exists.
 *
 * Icons: IconName is com.minowick.Pidgin4-<status> (installed into
 * <prefix>/share/icons/hicolor, which is also the item's IconThemePath,
 * and linked into ~/.local/share/icons by build-pidgin4.sh
 * --desktop-integration), and IconPixmap has the same icons from the
 * GResource, for hosts that find neither.
 *
 * Clicks: Activate (left) is pidgin_docklet_clicked(1): the next unread
 * conversation, else toggle the buddy list. SecondaryActivate (middle) is
 * button 2, which does nothing, as in Pidgin 2. Hosts show the Menu
 * themselves on right click.
 */

#define ICON_RESOURCE PIDGIN4_RESOURCE_PATH "/icons/%s/apps/" PIDGIN4_APP_ID "-%s.png"

static PidginSni *sni = NULL;

static void
set_icon(gboolean attention, const char *variant)
{
	static const char *const sizes[] = { "16x16", "22x22", "32x32", "48x48" };
	char *resources[G_N_ELEMENTS(sizes) + 1];
	char *name = g_strdup_printf(PIDGIN4_APP_ID "-%s", variant);
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(sizes); i++)
		resources[i] = g_strdup_printf(ICON_RESOURCE, sizes[i], variant);
	resources[i] = NULL;

	if (attention)
		pidgin_sni_set_attention_icon(sni, name, (const char * const *)resources);
	else
		pidgin_sni_set_icon(sni, name, (const char * const *)resources);

	for (i = 0; resources[i] != NULL; i++)
		g_free(resources[i]);
	g_free(name);
}

static void
docklet_sni_update_icon(PurpleStatusPrimitive status, gboolean connecting,
                        gboolean pending)
{
	const char *variant;

	if (sni == NULL)
		return;

	switch (status) {
		case PURPLE_STATUS_OFFLINE:
			variant = "offline";
			break;
		case PURPLE_STATUS_AWAY:
			variant = "away";
			break;
		case PURPLE_STATUS_UNAVAILABLE:
			variant = "busy";
			break;
		case PURPLE_STATUS_EXTENDED_AWAY:
			variant = "extended-away";
			break;
		case PURPLE_STATUS_INVISIBLE:
			variant = "invisible";
			break;
		default:
			variant = "available";
			break;
	}

	if (connecting)
		variant = "connecting";
	if (pending)
		variant = "pending";

	set_icon(FALSE, variant);

	/* Replaces blinking: hosts show the attention icon (Waybar) or
	 * animate the item (KDE, AppIndicator). */
	pidgin_sni_set_status(sni, pending ? PIDGIN_SNI_STATUS_NEEDS_ATTENTION
	                                   : PIDGIN_SNI_STATUS_ACTIVE);
}

static void
docklet_sni_set_tooltip(gchar *tooltip)
{
	guint unread;
	char *title, *body;

	if (sni == NULL)
		return;

	/* The count replaces Pidgin 2's X11 _PurpleUnseenCount property. */
	unread = pidgin_docklet_get_unread_count();
	if (unread > 0)
		title = g_strdup_printf(ngettext("%s (%u unread message)",
			"%s (%u unread messages)", unread), PIDGIN_NAME, unread);
	else
		title = g_strdup(PIDGIN_NAME);

	body = g_markup_escape_text(tooltip != NULL ? tooltip : "", -1);
	pidgin_sni_set_title(sni, title);
	pidgin_sni_set_tooltip(sni, title, body);
	g_free(body);
	g_free(title);
}

static void
sni_registered_cb(PidginSni *item, gpointer data)
{
	purple_debug_info("docklet", "tray icon registered as %s\n",
	                  pidgin_sni_get_bus_name(item));
	pidgin_docklet_embedded();
}

static void
sni_unregistered_cb(PidginSni *item, gpointer data)
{
	purple_debug_info("docklet", "tray icon unregistered (no "
	                  "StatusNotifierWatcher)\n");
	pidgin_docklet_remove();
}

static void
sni_activate_cb(PidginSni *item, gint x, gint y, gpointer data)
{
	GtkWidget *window = pidgin_blist_get_window();
	char *token = pidgin_sni_take_activation_token(item);

	/* A host that sent ProvideXdgActivationToken first: the buddy list
	 * presents with that token, so the compositor lets it take focus. */
	if (token != NULL && window != NULL)
		gtk_window_set_startup_id(GTK_WINDOW(window), token);
	g_free(token);

	pidgin_docklet_clicked(1);
}

static void
sni_secondary_activate_cb(PidginSni *item, gint x, gint y, gpointer data)
{
	pidgin_docklet_clicked(2);
}

static void
sni_context_menu_cb(PidginSni *item, gint x, gint y, gpointer data)
{
	pidgin_docklet_clicked(3);
}

static gboolean
menu_about_to_show_cb(PidginDBusMenu *menu, gint id, gpointer data)
{
	/* The unread list and the plugin actions are current when shown. */
	if (id == 0) {
		pidgin_docklet_refresh_menu();
		return TRUE;
	}
	return FALSE;
}

static void
docklet_sni_destroy(void)
{
	if (sni == NULL)
		return;

	pidgin_docklet_remove();
	g_signal_handlers_disconnect_by_data(sni, NULL);
	pidgin_sni_stop(sni);
	g_clear_object(&sni);
}

static void
docklet_sni_create(void)
{
	GDBusConnection *connection;
	GError *error = NULL;
	char *theme_path;

	if (sni != NULL)
		return;

	connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
	if (connection == NULL) {
		purple_debug_warning("docklet", "no session bus, no tray icon: %s\n",
		                     error->message);
		g_error_free(error);
		return;
	}

	sni = pidgin_sni_new(PIDGIN4_APP_ID, "Communications");
	pidgin_sni_set_title(sni, PIDGIN_NAME);
	theme_path = g_build_filename(DATADIR, "icons", NULL);
	pidgin_sni_set_icon_theme_path(sni, theme_path);
	g_free(theme_path);
	set_icon(TRUE, "pending");
	set_icon(FALSE, "offline");
	pidgin_sni_set_menu(sni, pidgin_docklet_get_menu(),
	                    pidgin_docklet_get_actions(), "tray");

	g_signal_connect(sni, "registered", G_CALLBACK(sni_registered_cb), NULL);
	g_signal_connect(sni, "unregistered", G_CALLBACK(sni_unregistered_cb), NULL);
	g_signal_connect(sni, "activate", G_CALLBACK(sni_activate_cb), NULL);
	g_signal_connect(sni, "secondary-activate",
	                 G_CALLBACK(sni_secondary_activate_cb), NULL);
	g_signal_connect(sni, "context-menu", G_CALLBACK(sni_context_menu_cb), NULL);
	g_signal_connect(pidgin_sni_get_menu_exporter(sni), "about-to-show",
	                 G_CALLBACK(menu_about_to_show_cb), NULL);

	if (!pidgin_sni_start(sni, connection, &error)) {
		purple_debug_warning("docklet", "cannot export the tray icon: %s\n",
		                     error->message);
		g_error_free(error);
		g_clear_object(&sni);
	} else {
		purple_debug_info("docklet", "tray icon exported as %s; waiting "
		                  "for a StatusNotifierWatcher\n",
		                  pidgin_sni_get_bus_name(sni));
	}
	g_object_unref(connection);
}

static struct docklet_ui_ops ui_ops =
{
	docklet_sni_create,
	docklet_sni_destroy,
	docklet_sni_update_icon,
	NULL, /* blank_icon: no blinking */
	docklet_sni_set_tooltip,
	NULL
};

void
docklet_ui_init(void)
{
	pidgin_docklet_set_ui_ops(&ui_ops);
}
