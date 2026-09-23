/*
 * pidgin4: Message Timestamp Formats (M7 port of
 * pidgin/plugins/timestamp_format.c).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The formats come from the "conversation-timestamp" signal (kept by the
 * PidginMessageView, which asks for each row's timestamp label) and
 * libpurple's "log-timestamp". Pidgin 2 added "Timestamp Format Options"
 * to the GtkIMHtml's context menu through a GtkTextView populate-popup
 * emission hook; here the item goes into every message row's menu through
 * the view's "populate-menu" signal, and opens the plugin's prefs. The
 * prefs are Pidgin 2's (/plugins/gtk/timestamp_format/...), same meaning.
 */
#include "pidgin4-plugin.h"

#include <time.h>

#define PREFS "/plugins/gtk/timestamp_format"
#define OPTIONS_ACTION "timestamp-format-options"

static PurplePlugin *my_plugin = NULL;

static const char *
format_12hour_hour(const struct tm *tm)
{
	static char hr[3];
	int hour = tm->tm_hour % 12;
	if (hour == 0)
		hour = 12;

	g_snprintf(hr, sizeof(hr), "%d", hour);
	return hr;
}

static PurplePluginPrefFrame *
get_plugin_pref_frame(PurplePlugin *plugin)
{
	PurplePluginPrefFrame *frame;
	PurplePluginPref *ppref;

	frame = purple_plugin_pref_frame_new();

	ppref = purple_plugin_pref_new_with_label(_("Timestamp Format Options"));
	purple_plugin_pref_frame_add(frame, ppref);

	ppref = purple_plugin_pref_new_with_name_and_label(PREFS "/force",
		_("_Force timestamp format:"));
	purple_plugin_pref_set_type(ppref, PURPLE_PLUGIN_PREF_CHOICE);
	purple_plugin_pref_add_choice(ppref, _("Use system default"), "default");
	purple_plugin_pref_add_choice(ppref, _("12 hour time format"), "force12");
	purple_plugin_pref_add_choice(ppref, _("24 hour time format"), "force24");
	purple_plugin_pref_frame_add(frame, ppref);

	ppref = purple_plugin_pref_new_with_label(_("Show dates in..."));
	purple_plugin_pref_frame_add(frame, ppref);

	ppref = purple_plugin_pref_new_with_name_and_label(PREFS "/use_dates/conversation",
		_("Co_nversations:"));
	purple_plugin_pref_set_type(ppref, PURPLE_PLUGIN_PREF_CHOICE);
	purple_plugin_pref_add_choice(ppref, _("For delayed messages"), "automatic");
	purple_plugin_pref_add_choice(ppref, _("For delayed messages and in chats"), "chats");
	purple_plugin_pref_add_choice(ppref, _("Always"), "always");
	purple_plugin_pref_frame_add(frame, ppref);

	ppref = purple_plugin_pref_new_with_name_and_label(PREFS "/use_dates/log",
		_("_Message Logs:"));
	purple_plugin_pref_set_type(ppref, PURPLE_PLUGIN_PREF_CHOICE);
	purple_plugin_pref_add_choice(ppref, _("For delayed messages"), "automatic");
	purple_plugin_pref_add_choice(ppref, _("For delayed messages and in chats"), "chats");
	purple_plugin_pref_add_choice(ppref, _("Always"), "always");
	purple_plugin_pref_frame_add(frame, ppref);

	return frame;
}

static char *
timestamp_cb_common(PurpleConversation *conv, time_t t, gboolean show_date,
                    const char *force, const char *dates, gboolean parens)
{
	struct tm *tm;

	g_return_val_if_fail(dates != NULL, NULL);

	tm = localtime(&t);

	if (show_date ||
	    purple_strequal(dates, "always") ||
	    (conv != NULL && purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_CHAT &&
	     purple_strequal(dates, "chats")))
	{
		if (purple_strequal(force, "force24"))
			return g_strdup_printf("%s%s%s", parens ? "(" : "",
				purple_utf8_strftime("%Y-%m-%d %H:%M:%S", tm), parens ? ")" : "");
		else if (purple_strequal(force, "force12")) {
			char *date = g_strdup_printf("%s", purple_utf8_strftime("%Y-%m-%d ", tm));
			char *remtime = g_strdup_printf("%s", purple_utf8_strftime(":%M:%S %p", tm));
			const char *hour = format_12hour_hour(tm);
			char *output;

			output = g_strdup_printf("%s%s%s%s%s", parens ? "(" : "", date,
			                         hour, remtime, parens ? ")" : "");

			g_free(date);
			g_free(remtime);

			return output;
		} else
			return g_strdup_printf("%s%s%s", parens ? "(" : "",
				purple_date_format_long(tm), parens ? ")" : "");
	}

	if (purple_strequal(force, "force24"))
		return g_strdup_printf("%s%s%s", parens ? "(" : "",
			purple_utf8_strftime("%H:%M:%S", tm), parens ? ")" : "");
	else if (purple_strequal(force, "force12")) {
		const char *hour = format_12hour_hour(tm);
		char *remtime = g_strdup_printf("%s", purple_utf8_strftime(":%M:%S %p", tm));
		char *output = g_strdup_printf("%s%s%s%s", parens ? "(" : "", hour, remtime,
		                               parens ? ")" : "");

		g_free(remtime);

		return output;
	}

	return NULL;
}

static char *
conversation_timestamp_cb(PurpleConversation *conv, time_t t, gboolean show_date,
                          gpointer data)
{
	const char *force = purple_prefs_get_string(PREFS "/force");
	const char *dates = purple_prefs_get_string(PREFS "/use_dates/conversation");

	/* The view may ask without a conversation (history rows of a view
	 * that isn't a conversation's); that is fine here. */
	return timestamp_cb_common(conv, t, show_date, force, dates, TRUE);
}

static char *
log_timestamp_cb(PurpleLog *log, time_t t, gboolean show_date, gpointer data)
{
	const char *force = purple_prefs_get_string(PREFS "/force");
	const char *dates = purple_prefs_get_string(PREFS "/use_dates/log");

	g_return_val_if_fail(log != NULL, NULL);

	return timestamp_cb_common(log->conv, t, show_date, force, dates, FALSE);
}

/**************************************************************************
 * The context menu item and the options window
 **************************************************************************/

static void
options_activate_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	if (my_plugin != NULL)
		pidgin4_plugin_show_config(my_plugin);
}

static void
populate_menu_cb(PidginMessageView *view, PidginMessage *msg, GMenu *section, gpointer data)
{
	if (pidgin_message_get_kind(msg) != PIDGIN_MESSAGE_KIND_NORMAL)
		return;
	g_menu_append(section, _("Timestamp Format Options"), "app." OPTIONS_ACTION);
}

static void
attach_view(PidginMessageView *view)
{
	if (view == NULL ||
	    g_signal_handler_find(view, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, populate_menu_cb, NULL))
		return;
	g_signal_connect(view, "populate-menu", G_CALLBACK(populate_menu_cb), NULL);
	/* rows already bound build their menus again */
	pidgin_message_view_refresh(view);
}

static void
conv_displayed_cb(PidginConversation *gtkconv, gpointer data)
{
	attach_view(PIDGIN_MESSAGE_VIEW(pidgin_conv_get_message_view(gtkconv)));
}

static void
refresh_all(gboolean detach)
{
	GList *l;

	for (l = purple_get_conversations(); l != NULL; l = l->next) {
		PidginMessageView *view = pidgin4_plugin_conv_view(l->data);

		if (view == NULL)
			continue;
		if (detach) {
			g_signal_handlers_disconnect_by_func(view, populate_menu_cb, NULL);
			pidgin_message_view_refresh(view);
		} else {
			attach_view(view);
		}
	}
}

static void
format_pref_cb(const char *name, PurplePrefType type, gconstpointer val, gpointer data)
{
	GList *l;

	/* Rows ask for their timestamp when they are bound: redo them. */
	for (l = purple_get_conversations(); l != NULL; l = l->next) {
		PidginMessageView *view = pidgin4_plugin_conv_view(l->data);

		if (view != NULL)
			pidgin_message_view_refresh(view);
	}
}

static gboolean
plugin_load(PurplePlugin *plugin)
{
	GApplication *app = G_APPLICATION(pidgin_application_get());
	GSimpleAction *action;

	my_plugin = plugin;

	purple_signal_connect(pidgin_conversations_get_handle(), "conversation-timestamp",
	                      plugin, PURPLE_CALLBACK(conversation_timestamp_cb), NULL);
	purple_signal_connect(purple_log_get_handle(), "log-timestamp",
	                      plugin, PURPLE_CALLBACK(log_timestamp_cb), NULL);
	purple_signal_connect(pidgin_conversations_get_handle(), "conversation-displayed",
	                      plugin, PURPLE_CALLBACK(conv_displayed_cb), NULL);
	purple_prefs_connect_callback(plugin, PREFS, format_pref_cb, NULL);

	if (app != NULL) {
		action = g_simple_action_new(OPTIONS_ACTION, NULL);
		g_signal_connect(action, "activate", G_CALLBACK(options_activate_cb), NULL);
		g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(action));
		g_object_unref(action);
	}

	refresh_all(FALSE);
	format_pref_cb(NULL, PURPLE_PREF_NONE, NULL, NULL);
	return TRUE;
}

static gboolean
plugin_unload(PurplePlugin *plugin)
{
	GApplication *app = G_APPLICATION(pidgin_application_get());

	if (app != NULL)
		g_action_map_remove_action(G_ACTION_MAP(app), OPTIONS_ACTION);
	/* Disconnect first: the refresh below then uses the default format. */
	purple_signals_disconnect_by_handle(plugin);
	refresh_all(TRUE);
	my_plugin = NULL;
	return TRUE;
}

static PurplePluginUiInfo prefs_info = {
	get_plugin_pref_frame,
	0,   /* page num (Reserved) */
	NULL,/* frame (Reserved) */

	/* padding */
	NULL,
	NULL,
	NULL,
	NULL
};

static PurplePluginInfo info =
{
	PURPLE_PLUGIN_MAGIC,
	PURPLE_MAJOR_VERSION,
	PURPLE_MINOR_VERSION,
	PURPLE_PLUGIN_STANDARD,                           /**< type           */
	PIDGIN_PLUGIN_TYPE,                               /**< ui_requirement */
	0,                                                /**< flags          */
	NULL,                                             /**< dependencies   */
	PURPLE_PRIORITY_DEFAULT,                          /**< priority       */

	"core-timestamp_format",                          /**< id             */
	N_("Message Timestamp Formats"),                  /**< name           */
	DISPLAY_VERSION,                                  /**< version        */
	                                                  /**  summary        */
	N_("Customizes the message timestamp formats."),
	                                                  /**  description    */
	N_("This plugin allows the user to customize "
	   "conversation and logging message timestamp "
	   "formats."),
	"Richard Laager <rlaager@pidgin.im>",             /**< author         */
	PURPLE_WEBSITE,                                   /**< homepage       */

	plugin_load,                                      /**< load           */
	plugin_unload,                                    /**< unload         */
	NULL,                                             /**< destroy        */

	NULL,                                             /**< ui_info        */
	NULL,                                             /**< extra_info     */
	&prefs_info,                                      /**< prefs_info     */
	NULL,                                             /**< actions        */

	/* padding */
	NULL,
	NULL,
	NULL,
	NULL
};

static void
init_plugin(PurplePlugin *plugin)
{
	purple_prefs_add_none("/plugins/gtk");
	purple_prefs_add_none(PREFS);

	if (!purple_prefs_exists(PREFS "/force") &&
	    purple_prefs_exists(PREFS "/force_24hr"))
	{
		if (purple_prefs_get_bool(PREFS "/force_24hr"))
			purple_prefs_add_string(PREFS "/force", "force24");
		else
			purple_prefs_add_string(PREFS "/force", "default");
	}
	else
		purple_prefs_add_string(PREFS "/force", "default");

	purple_prefs_add_none(PREFS "/use_dates");
	purple_prefs_add_string(PREFS "/use_dates/conversation", "automatic");
	purple_prefs_add_string(PREFS "/use_dates/log", "automatic");
}

PURPLE_INIT_PLUGIN(timestamp_format, init_plugin, info)
