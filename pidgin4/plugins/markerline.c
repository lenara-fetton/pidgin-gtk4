/*
 * pidgin4: Markerline (M7 port of pidgin/plugins/markerline.c).
 *
 * Copyright (C) 2006 Sadrul Habib Chowdhury <sadrul@users.sourceforge.net>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301
 * USA.
 */

/*
 * Pidgin 2 drew a red line over the GtkIMHtml in its expose handler. The
 * PidginMessageView has a marker item instead (a separator row, at most
 * one per view): when a conversation window loses the focus, or its tab
 * is switched away from, the marker moves after the last message of the
 * conversation that was showing. Everything after it arrived while you
 * were away. The prefs (IMs, chats) are shared with Pidgin 2.
 */
#include "pidgin4-plugin.h"

#define PLUGIN_ID           "gtk-plugin_pack-markerline"
#define PLUGIN_NAME         N_("Markerline")
#define PLUGIN_STATIC_NAME  Markerline
#define PLUGIN_SUMMARY      N_("Draw a line to indicate new messages in a conversation.")
#define PLUGIN_DESCRIPTION  N_("Draw a line to indicate new messages in a conversation.")
#define PLUGIN_AUTHOR       "Sadrul H Chowdhury <sadrul@users.sourceforge.net>"

#define PREF_PREFIX     "/plugins/gtk/" PLUGIN_ID
#define PREF_IMS        PREF_PREFIX "/ims"
#define PREF_CHATS      PREF_PREFIX "/chats"

/* g_object data on the GtkWindow: the PidginWindow we connected to. */
#define WIN_KEY "pidgin4-markerline-win"

static gboolean
enabled_for(PurpleConversation *conv)
{
	PurpleConversationType type = purple_conversation_get_type(conv);

	return (type == PURPLE_CONV_TYPE_CHAT && purple_prefs_get_bool(PREF_CHATS)) ||
	       (type == PURPLE_CONV_TYPE_IM && purple_prefs_get_bool(PREF_IMS));
}

static void
update_marker_for_gtkconv(PidginConversation *gtkconv)
{
	PidginMessageView *view;
	GListModel *model;
	guint n;

	g_return_if_fail(gtkconv != NULL);

	if (!enabled_for(gtkconv->active_conv))
		return;
	view = PIDGIN_MESSAGE_VIEW(pidgin_conv_get_message_view(gtkconv));
	model = pidgin_message_view_get_model(view);
	n = g_list_model_get_n_items(model);
	if (n == 0)
		return;
	/* Already after the last message: leave it (and the scroll) alone. */
	if (pidgin_message_view_get_marker(view) != NULL) {
		PidginMessage *last = g_list_model_get_item(model, n - 1);
		gboolean at_end = last == pidgin_message_view_get_marker(view);

		g_object_unref(last);
		if (at_end)
			return;
	}
	pidgin_message_view_set_marker(view);
}

static void
mark_active(PidginWindow *win)
{
	PidginConversation *gtkconv = pidgin_conv_window_get_active_gtkconv(win);

	if (gtkconv != NULL)
		update_marker_for_gtkconv(gtkconv);
}

static void
active_changed_cb(GtkWindow *window, GParamSpec *pspec, PidginWindow *win)
{
	/* focus-out */
	if (!gtk_window_is_active(window))
		mark_active(win);
}

static void
switch_page_cb(GtkNotebook *notebook, GtkWidget *page, guint num, PidginWindow *win)
{
	/* Still the old page: mark the conversation we are leaving. */
	mark_active(win);
}

static void
detach_from_pidgin_window(PidginWindow *win, gpointer null)
{
	GtkWidget *window = pidgin_conv_window_get_window(win);

	g_signal_handlers_disconnect_by_func(pidgin_conv_window_get_notebook(win),
	                                     switch_page_cb, win);
	g_signal_handlers_disconnect_by_func(window, active_changed_cb, win);
	g_object_set_data(G_OBJECT(window), WIN_KEY, NULL);
}

static void
attach_to_pidgin_window(PidginWindow *win, gpointer null)
{
	GtkWidget *window = pidgin_conv_window_get_window(win);

	if (g_object_get_data(G_OBJECT(window), WIN_KEY) != NULL)
		return;
	g_object_set_data(G_OBJECT(window), WIN_KEY, win);
	g_signal_connect(window, "notify::is-active", G_CALLBACK(active_changed_cb), win);
	g_signal_connect(pidgin_conv_window_get_notebook(win), "switch-page",
	                 G_CALLBACK(switch_page_cb), win);
}

static void
remove_markers(void)
{
	GList *l;

	for (l = purple_get_conversations(); l != NULL; l = l->next) {
		PidginMessageView *view = pidgin4_plugin_conv_view(l->data);

		if (view != NULL)
			pidgin_message_view_remove_marker(view);
	}
}

static void
conv_displayed(PidginConversation *gtkconv, gpointer null)
{
	PidginWindow *win = pidgin_conv_get_window(gtkconv);

	if (win != NULL && !pidgin_conv_window_is_hidden(win))
		attach_to_pidgin_window(win, NULL);
}

static void
jump_to_markerline(PurpleConversation *conv, gpointer null)
{
	PidginMessageView *view = pidgin4_plugin_conv_view(conv);
	PidginMessage *marker;

	if (view == NULL)
		return;
	marker = pidgin_message_view_get_marker(view);
	if (marker != NULL)
		pidgin_message_view_scroll_to_message(view, marker);
}

static void
conv_menu_cb(PurpleConversation *conv, GList **list)
{
	PidginMessageView *view = pidgin4_plugin_conv_view(conv);
	gboolean enabled = enabled_for(conv) && view != NULL &&
	                   pidgin_message_view_get_marker(view) != NULL;
	PurpleMenuAction *action = purple_menu_action_new(_("Jump to markerline"),
		enabled ? PURPLE_CALLBACK(jump_to_markerline) : NULL, NULL, NULL);

	*list = g_list_append(*list, action);
}

static void
pref_cb(const char *name, PurplePrefType type, gconstpointer val, gpointer data)
{
	GList *l;

	if (GPOINTER_TO_INT(val))
		return;
	/* Switched off: take the markers of that conversation type away. */
	for (l = purple_get_conversations(); l != NULL; l = l->next) {
		PidginMessageView *view = pidgin4_plugin_conv_view(l->data);

		if (view != NULL && !enabled_for(l->data))
			pidgin_message_view_remove_marker(view);
	}
}

static gboolean
plugin_load(PurplePlugin *plugin)
{
	g_list_foreach(pidgin_conv_windows_get_list(), (GFunc)attach_to_pidgin_window, NULL);
	purple_signal_connect(pidgin_conversations_get_handle(), "conversation-displayed",
	                      plugin, PURPLE_CALLBACK(conv_displayed), NULL);
	purple_signal_connect(purple_conversations_get_handle(), "conversation-extended-menu",
	                      plugin, PURPLE_CALLBACK(conv_menu_cb), NULL);
	purple_prefs_connect_callback(plugin, PREF_IMS, pref_cb, NULL);
	purple_prefs_connect_callback(plugin, PREF_CHATS, pref_cb, NULL);
	return TRUE;
}

static gboolean
plugin_unload(PurplePlugin *plugin)
{
	g_list_foreach(pidgin_conv_windows_get_list(), (GFunc)detach_from_pidgin_window, NULL);
	remove_markers();
	return TRUE;
}

static PurplePluginPrefFrame *
get_plugin_pref_frame(PurplePlugin *plugin)
{
	PurplePluginPrefFrame *frame;
	PurplePluginPref *pref;

	frame = purple_plugin_pref_frame_new();

	pref = purple_plugin_pref_new_with_label(_("Draw Markerline in "));
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(PREF_IMS, _("_IM windows"));
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(PREF_CHATS, _("C_hat windows"));
	purple_plugin_pref_frame_add(frame, pref);

	return frame;
}

static PurplePluginUiInfo prefs_info = {
	get_plugin_pref_frame,
	0,
	NULL,

	/* padding */
	NULL,
	NULL,
	NULL,
	NULL
};

static PurplePluginInfo info = {
	PURPLE_PLUGIN_MAGIC,
	PURPLE_MAJOR_VERSION,
	PURPLE_MINOR_VERSION,
	PURPLE_PLUGIN_STANDARD,
	PIDGIN_PLUGIN_TYPE,
	0,
	NULL,
	PURPLE_PRIORITY_DEFAULT,
	PLUGIN_ID,
	PLUGIN_NAME,
	DISPLAY_VERSION,
	PLUGIN_SUMMARY,
	PLUGIN_DESCRIPTION,
	PLUGIN_AUTHOR,
	PURPLE_WEBSITE,
	plugin_load,
	plugin_unload,
	NULL,
	NULL,
	NULL,
	&prefs_info,
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
	purple_prefs_add_none(PREF_PREFIX);
	purple_prefs_add_bool(PREF_IMS, FALSE);
	purple_prefs_add_bool(PREF_CHATS, TRUE);
}

PURPLE_INIT_PLUGIN(PLUGIN_STATIC_NAME, init_plugin, info)
