/*
 * pidgin4: Timestamp (M7 port of pidgin/plugins/timestamp.c).
 *
 * iChat-style timestamps
 *
 * Copyright (C) 2002-2003, Sean Egan
 * Copyright (C) 2003, Chris J. Friesen <Darth_Sebulba04@yahoo.com>
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
 * Before a message is displayed, if the last time row of the conversation
 * is at least the interval old, a time row ("12:35") goes into the
 * PidginMessageView: a system row that is never logged, with the CSS
 * class "timestamp-plugin" (bold and dimmed, as Pidgin 2's grey
 * centred text). The interval pref is Pidgin 2's
 * /plugins/gtk/timestamp/interval (in milliseconds).
 */
#include "pidgin4-plugin.h"

#define TIMESTAMP_PLUGIN_ID "gtk-timestamp"

#define PREF_INTERVAL "/plugins/gtk/timestamp/interval"

/* Set the default to 5 minutes. */
static int interval = 5 * 60;

static void
timestamp_display(PurpleConversation *conv, time_t then, time_t now)
{
	PidginMessageView *view = pidgin4_plugin_conv_view(conv);
	PidginMessage *msg;
	char *text;

	if (view == NULL)
		return;

	text = g_markup_escape_text(purple_utf8_strftime("%H:%M", localtime(&now)), -1);
	msg = pidgin_message_new(NULL, NULL, text,
	                         PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG, now);
	pidgin_message_add_css_class(msg, "timestamp-plugin");
	pidgin_message_view_append(view, msg);
	g_object_unref(msg);
	g_free(text);
}

static gboolean
timestamp_displaying_conv_msg(PurpleAccount *account, const char *who,
                              char **buffer, PurpleConversation *conv,
                              PurpleMessageFlags flags, void *data)
{
	time_t now = time(NULL) / interval * interval;
	time_t then;

	if (!g_list_find(purple_get_conversations(), conv))
		return FALSE;

	then = GPOINTER_TO_INT(purple_conversation_get_data(conv, "timestamp-last"));

	if (now - then >= interval) {
		timestamp_display(conv, then, now);
		purple_conversation_set_data(conv, "timestamp-last", GINT_TO_POINTER(now));
	}

	return FALSE;
}

static void
timestamp_new_convo(PurpleConversation *conv)
{
	if (!g_list_find(purple_get_conversations(), conv))
		return;

	purple_conversation_set_data(conv, "timestamp-last", GINT_TO_POINTER(0));
}

static void
set_timestamp(GtkSpinButton *spinner, void *null)
{
	int tm;

	tm = gtk_spin_button_get_value_as_int(spinner);
	purple_debug(PURPLE_DEBUG_MISC, "timestamp", "setting interval to %d minutes\n", tm);

	interval = tm * 60;
	purple_prefs_set_int(PREF_INTERVAL, interval * 1000);
}

static void
interval_pref_cb(const char *name, PurplePrefType type, gconstpointer val, gpointer data)
{
	interval = MAX(GPOINTER_TO_INT(val) / 1000, 60);
}

static GtkWidget *
get_config_frame(PurplePlugin *plugin)
{
	GtkWidget *ret;
	GtkWidget *frame, *label;
	GtkWidget *hbox;
	GtkWidget *spinner;

	ret = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
	gtk_widget_set_margin_top(ret, 12);
	gtk_widget_set_margin_bottom(ret, 12);
	gtk_widget_set_margin_start(ret, 12);
	gtk_widget_set_margin_end(ret, 12);

	frame = pidgin_make_frame(ret, _("Display Timestamps Every"));

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
	gtk_box_append(GTK_BOX(frame), hbox);

	spinner = gtk_spin_button_new_with_range(1, 60, 1);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(spinner), interval / 60);
	gtk_widget_set_hexpand(spinner, TRUE);
	gtk_box_append(GTK_BOX(hbox), spinner);
	g_signal_connect(spinner, "value-changed", G_CALLBACK(set_timestamp), NULL);
	label = gtk_label_new(_("minutes"));
	gtk_box_append(GTK_BOX(hbox), label);

	return ret;
}

static gboolean
plugin_load(PurplePlugin *plugin)
{
	void *conv_handle = purple_conversations_get_handle();
	void *gtkconv_handle = pidgin_conversations_get_handle();
	GList *l;

	/* lower priority to display initial timestamp after logged messages */
	purple_signal_connect_priority(conv_handle, "conversation-created",
		plugin, PURPLE_CALLBACK(timestamp_new_convo), NULL,
		PURPLE_SIGNAL_PRIORITY_DEFAULT + 1);

	purple_signal_connect(gtkconv_handle, "displaying-chat-msg",
		plugin, PURPLE_CALLBACK(timestamp_displaying_conv_msg), NULL);
	purple_signal_connect(gtkconv_handle, "displaying-im-msg",
		plugin, PURPLE_CALLBACK(timestamp_displaying_conv_msg), NULL);

	purple_prefs_connect_callback(plugin, PREF_INTERVAL, interval_pref_cb, NULL);
	interval = MAX(purple_prefs_get_int(PREF_INTERVAL) / 1000, 60);

	/* conversations that are already open start again */
	for (l = purple_get_conversations(); l != NULL; l = l->next)
		timestamp_new_convo(l->data);

	return TRUE;
}

static PidginPluginUiInfo ui_info =
{
	get_config_frame,
	0, /* page_num (Reserved) */

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

	TIMESTAMP_PLUGIN_ID,                              /**< id             */
	N_("Timestamp"),                                  /**< name           */
	DISPLAY_VERSION,                                  /**< version        */
	                                                  /**  summary        */
	N_("Display iChat-style timestamps"),
	                                                  /**  description    */
	N_("Display iChat-style timestamps every N minutes."),
	"Sean Egan <seanegan@gmail.com>",                 /**< author         */
	PURPLE_WEBSITE,                                   /**< homepage       */

	plugin_load,                                      /**< load           */
	NULL,                                             /**< unload         */
	NULL,                                             /**< destroy        */

	&ui_info,                                         /**< ui_info        */
	NULL,                                             /**< extra_info     */
	NULL,
	NULL,

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
	purple_prefs_add_none("/plugins/gtk/timestamp");
	purple_prefs_add_int(PREF_INTERVAL, interval * 1000);
}

PURPLE_INIT_PLUGIN(timestamp, init_plugin, info)
