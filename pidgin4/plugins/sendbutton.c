/*
 * pidgin4: Send Button (M7 port of pidgin/plugins/sendbutton.c).
 *
 * Copyright (C) 2008 Etan Reisner <deryni@pidgin.im>
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
 * pidgin4's conversations already have a Send button, shown by the
 * /pidgin4/conversations/send_button pref (M4b). Pidgin 2 had no such pref
 * and added its own button. The port turns the pref on while the plugin is
 * loaded and puts the old value back on unload, and, as before, makes the
 * button insensitive while the entry is empty.
 */
#include "pidgin4-plugin.h"

#define SEND_BUTTON_PREF PIDGIN4_PREFS_ROOT "/conversations/send_button"
#define SAVED_PREF PIDGIN4_PREFS_ROOT "/plugins/sendbutton/previous"
#define HANDLER_KEY "pidgin4-sendbutton-handler"

static void
input_buffer_changed(GtkTextBuffer *text_buffer, GtkWidget *send_button)
{
	gtk_widget_set_sensitive(send_button, gtk_text_buffer_get_char_count(text_buffer) != 0);
}

static void
attach_gtkconv(PidginConversation *gtkconv)
{
	GtkWidget *button = pidgin_conv_get_send_button(gtkconv);
	GtkTextBuffer *buf;
	gulong id;

	if (button == NULL || g_object_get_data(G_OBJECT(button), HANDLER_KEY) != NULL)
		return;
	buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(pidgin_conv_get_compose_entry(gtkconv)));
	id = g_signal_connect(buf, "changed", G_CALLBACK(input_buffer_changed), button);
	g_object_set_data(G_OBJECT(button), HANDLER_KEY, GSIZE_TO_POINTER(id));
	input_buffer_changed(buf, button);
}

static void
detach_gtkconv(PidginConversation *gtkconv)
{
	GtkWidget *button = pidgin_conv_get_send_button(gtkconv);
	GtkTextBuffer *buf;
	gulong id;

	if (button == NULL)
		return;
	id = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(button), HANDLER_KEY));
	if (id != 0) {
		buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(pidgin_conv_get_compose_entry(gtkconv)));
		g_signal_handler_disconnect(buf, id);
		g_object_set_data(G_OBJECT(button), HANDLER_KEY, NULL);
	}
	gtk_widget_set_sensitive(button, TRUE);
}

static void
conversation_displayed_cb(PidginConversation *gtkconv)
{
	attach_gtkconv(gtkconv);
}

static void
foreach_gtkconv(void (*func)(PidginConversation *))
{
	GList *convs;

	for (convs = purple_get_conversations(); convs != NULL; convs = convs->next) {
		PurpleConversation *conv = convs->data;

		if (PIDGIN_IS_PIDGIN_CONVERSATION(conv) && PIDGIN_CONVERSATION(conv) != NULL)
			func(PIDGIN_CONVERSATION(conv));
	}
}

static gboolean
plugin_load(PurplePlugin *plugin)
{
	purple_signal_connect(pidgin_conversations_get_handle(), "conversation-displayed", plugin,
	                      PURPLE_CALLBACK(conversation_displayed_cb), NULL);

	purple_prefs_set_bool(SAVED_PREF, purple_prefs_get_bool(SEND_BUTTON_PREF));
	purple_prefs_set_bool(SEND_BUTTON_PREF, TRUE);
	foreach_gtkconv(attach_gtkconv);
	return TRUE;
}

static gboolean
plugin_unload(PurplePlugin *plugin)
{
	foreach_gtkconv(detach_gtkconv);
	purple_prefs_set_bool(SEND_BUTTON_PREF, purple_prefs_get_bool(SAVED_PREF));
	return TRUE;
}

static PurplePluginInfo info =
{
	PURPLE_PLUGIN_MAGIC,
	PURPLE_MAJOR_VERSION,                           /**< major version */
	PURPLE_MINOR_VERSION,                           /**< minor version */
	PURPLE_PLUGIN_STANDARD,                         /**< type */
	PIDGIN_PLUGIN_TYPE,                             /**< ui_requirement */
	0,                                              /**< flags */
	NULL,                                           /**< dependencies */
	PURPLE_PRIORITY_DEFAULT,                        /**< priority */

	"gtksendbutton",                                /**< id */
	N_("Send Button"),                              /**< name */
	DISPLAY_VERSION,                                /**< version */
	N_("Conversation Window Send Button."),         /**< summary */
	N_("Adds a Send button to the entry area of "
	   "the conversation window. Intended for use "
	   "when no physical keyboard is present."),    /**< description */
	"Etan Reisner <deryni@pidgin.im>",              /**< author */
	PURPLE_WEBSITE,                                 /**< homepage */
	plugin_load,                                    /**< load */
	plugin_unload,                                  /**< unload */
	NULL,                                           /**< destroy */
	NULL,                                           /**< ui_info */
	NULL,                                           /**< extra_info */
	NULL,                                           /**< prefs_info */
	NULL,                                           /**< actions */

	/* padding */
	NULL,
	NULL,
	NULL,
	NULL
};

static void
init_plugin(PurplePlugin *plugin)
{
	purple_prefs_add_none(PIDGIN4_PREFS_ROOT "/plugins");
	purple_prefs_add_none(PIDGIN4_PREFS_ROOT "/plugins/sendbutton");
	purple_prefs_add_bool(SAVED_PREF, FALSE);
}

PURPLE_INIT_PLUGIN(sendbutton, init_plugin, info)
