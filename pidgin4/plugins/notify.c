/*
 * pidgin4: Message Notification (M7 port of pidgin/plugins/notify.c).
 *
 * Pidgin - New message notification plugin
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
 * What changed from Pidgin 2:
 *
 *  - The methods: the title string and count are as before. There is no
 *    X property (_PIDGIN_UNSEEN_COUNT; the tray's tooltip has the count)
 *    and no urgency hint on Wayland: "Set attention" marks the window with
 *    the CSS class "notify-attention" and refreshes the tray, whose icon
 *    already turns to "pending" while a conversation has unseen text.
 *    Raise and present are gtk_window_present() (xdg-activation: Sway
 *    marks the window urgent instead of focusing it). New: a desktop
 *    notification through pidginnotify.c's GNotification
 *    (/pidgin4/plugins/notify/method_notification, default off, since
 *    pidgin4's own /pidgin4/notifications/new_message already sends one).
 *  - Removal: event controllers instead of GTK 2 event signals. "Gains
 *    focus" is focus entering the conversation (GtkEventControllerFocus
 *    on the tab page) or its window becoming active, "click" a
 *    GtkGestureClick (capture phase) on the page, "typing" a key press in
 *    the compose entry.
 *  - The prefs are Pidgin 2's /plugins/gtk/X11/notify/... (same meaning)
 *    as a PurplePluginPrefFrame, so the M5 plugins dialog shows them.
 */
#include "pidgin4-plugin.h"

#include "gtkdocklet.h"
#include "pidginnotify.h"

#define NOTIFY_PLUGIN_ID "gtk-x11-notify"

#define PREFS "/plugins/gtk/X11/notify"
#define PREFS4 PIDGIN4_PREFS_ROOT "/plugins/notify"

#define COUNT_KEY "notify-message-count"
#define CONTROLLERS_KEY "notify-controllers"

static PurplePlugin *my_plugin = NULL;
static gboolean setting_title = FALSE;

static void notify_win(PidginWindow *purplewin, PurpleConversation *conv);
static void unnotify(PurpleConversation *conv, gboolean reset);

static int
conv_count(PurpleConversation *conv)
{
	return GPOINTER_TO_INT(purple_conversation_get_data(conv, COUNT_KEY));
}

static guint
count_messages(PidginWindow *purplewin)
{
	guint count = 0;
	GList *l;

	for (l = pidgin_conv_window_get_gtkconvs(purplewin); l != NULL; l = l->next) {
		PidginConversation *gtkconv = l->data;

		count += conv_count(gtkconv->active_conv);
	}
	return count;
}

static gboolean
is_notifiable_window(PidginWindow *win)
{
	return win != NULL && !pidgin_conv_window_is_hidden(win);
}

/* The window title: the active conversation's title, with our prefixes. */
static void
apply_title(PidginWindow *purplewin, gboolean prefixes)
{
	PurpleConversation *active = pidgin_conv_window_get_active_conversation(purplewin);
	GString *title;
	guint count = count_messages(purplewin);

	if (active == NULL)
		return;

	title = g_string_new(NULL);
	if (prefixes && count > 0) {
		if (purple_prefs_get_bool(PREFS "/method_string"))
			g_string_append(title, purple_prefs_get_string(PREFS "/title_string"));
		if (purple_prefs_get_bool(PREFS "/method_count"))
			g_string_append_printf(title, "[%u] ", count);
	}
	g_string_append(title, purple_conversation_get_title(active));

	if (!purple_strequal(gtk_window_get_title(GTK_WINDOW(pidgin_conv_window_get_window(purplewin))),
	                     title->str)) {
		setting_title = TRUE;
		gtk_window_set_title(GTK_WINDOW(pidgin_conv_window_get_window(purplewin)), title->str);
		setting_title = FALSE;
	}
	g_string_free(title, TRUE);
}

static void
handle_urgent(PidginWindow *purplewin, gboolean set)
{
	GtkWidget *window;

	g_return_if_fail(purplewin != NULL);

	window = pidgin_conv_window_get_window(purplewin);
	if (set)
		gtk_widget_add_css_class(window, "notify-attention");
	else
		gtk_widget_remove_css_class(window, "notify-attention");
	/* The tray shows "pending" while there is unseen text. */
	pidgin_docklet_update_icon();
}

static void
handle_raise(PidginWindow *purplewin)
{
	pidgin_conv_window_raise(purplewin);
}

static void
handle_present(PurpleConversation *conv)
{
	if (pidgin_conv_is_hidden(PIDGIN_CONVERSATION(conv)))
		return;
	purple_conversation_present(conv);
}

static int
notify(PurpleConversation *conv, gboolean increment)
{
	PidginWindow *purplewin;
	gboolean has_focus;

	if (conv == NULL || PIDGIN_CONVERSATION(conv) == NULL)
		return 0;

	/* We want to remove the notifications, but not reset the counter */
	unnotify(conv, FALSE);

	purplewin = PIDGIN_CONVERSATION(conv)->win;
	if (!is_notifiable_window(purplewin))
		return 0;

	/* If we aren't doing notifications for this type of conversation, return */
	if ((purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_IM &&
	     !purple_prefs_get_bool(PREFS "/type_im")) ||
	    (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_CHAT &&
	     !purple_prefs_get_bool(PREFS "/type_chat")))
		return 0;

	has_focus = gtk_window_is_active(GTK_WINDOW(pidgin_conv_window_get_window(purplewin)));

	if (purple_prefs_get_bool(PREFS "/type_focused") || !has_focus) {
		if (increment)
			purple_conversation_set_data(conv, COUNT_KEY,
			                             GINT_TO_POINTER(conv_count(conv) + 1));
		notify_win(purplewin, conv);
	}

	return 0;
}

static void
notify_win(PidginWindow *purplewin, PurpleConversation *conv)
{
	if (count_messages(purplewin) <= 0)
		return;

	apply_title(purplewin, TRUE);
	if (purple_prefs_get_bool(PREFS "/method_urgent"))
		handle_urgent(purplewin, TRUE);
	if (purple_prefs_get_bool(PREFS "/method_raise"))
		handle_raise(purplewin);
	if (purple_prefs_get_bool(PREFS "/method_present"))
		handle_present(conv);
}

static void
unnotify(PurpleConversation *conv, gboolean reset)
{
	PidginWindow *purplewin;

	g_return_if_fail(conv != NULL);
	if (PIDGIN_CONVERSATION(conv) == NULL)
		return;

	purplewin = PIDGIN_CONVERSATION(conv)->win;
	if (!is_notifiable_window(purplewin))
		return;

	if (reset) {
		handle_urgent(purplewin, FALSE);
		purple_conversation_set_data(conv, COUNT_KEY, GINT_TO_POINTER(0));
		if (purple_prefs_get_bool(PREFS4 "/method_notification"))
			pidgin_notification_withdraw(purple_conversation_get_account(conv),
			                             purple_conversation_get_type(conv),
			                             purple_conversation_get_name(conv));
	}
	/* reset the conversation window title; after a reset, the other
	 * conversations of the window may still have a count to show */
	apply_title(purplewin, reset);
}

static void
unnotify_now(PurpleConversation *conv)
{
	if (g_list_find(purple_get_conversations(), conv) != NULL && conv_count(conv) != 0)
		unnotify(conv, TRUE);
}

/**************************************************************************
 * Event controllers for the removal
 **************************************************************************/

static void
focus_enter_cb(GtkEventControllerFocus *controller, PurpleConversation *conv)
{
	unnotify_now(conv);
}

static void
click_cb(GtkGestureClick *gesture, int n_press, double x, double y, PurpleConversation *conv)
{
	unnotify_now(conv);
}

static gboolean
key_cb(GtkEventControllerKey *controller, guint keyval, guint keycode,
       GdkModifierType state, PurpleConversation *conv)
{
	unnotify_now(conv);
	return FALSE;
}

static void
window_active_cb(GtkWindow *window, GParamSpec *pspec, gpointer data)
{
	PidginWindow *win = data;
	PurpleConversation *conv;

	if (!gtk_window_is_active(window) || !purple_prefs_get_bool(PREFS "/notify_focus"))
		return;
	if (g_list_find(pidgin_conv_windows_get_list(), win) == NULL)
		return;
	conv = pidgin_conv_window_get_active_conversation(win);
	if (conv != NULL)
		unnotify_now(conv);
}

/*
 * The conversation window sets its title from the active conversation on
 * many updates (tab switches, typing, status); put our prefix back.
 */
static gboolean
reapply_title_idle(gpointer data)
{
	GtkWidget *window = data;
	GList *l;

	g_object_steal_data(G_OBJECT(window), "notify-title-idle");
	for (l = pidgin_conv_windows_get_list(); l != NULL; l = l->next) {
		PidginWindow *win = l->data;

		if (pidgin_conv_window_get_window(win) == window && count_messages(win) > 0)
			apply_title(win, TRUE);
	}
	return G_SOURCE_REMOVE;
}

static void
window_title_cb(GtkWindow *window, GParamSpec *pspec, gpointer data)
{
	guint id;

	if (setting_title || g_object_get_data(G_OBJECT(window), "notify-title-idle") != NULL)
		return;
	/* not from inside the notification: once the window is done */
	id = g_idle_add_full(G_PRIORITY_HIGH_IDLE, reapply_title_idle,
	                     g_object_ref(window), g_object_unref);
	g_object_set_data_full(G_OBJECT(window), "notify-title-idle", GUINT_TO_POINTER(id),
	                       (GDestroyNotify)g_source_remove);
}

static void
add_controller(GtkWidget *widget, GtkEventController *controller, GSList **list)
{
	gtk_widget_add_controller(widget, controller);
	/* the widget owns it; we keep weak track to remove it */
	*list = g_slist_prepend(*list, controller);
	g_object_set_data(G_OBJECT(controller), "notify-widget", widget);
}

static void
detach_signals(PurpleConversation *conv)
{
	GSList *list = purple_conversation_get_data(conv, CONTROLLERS_KEY), *l;

	for (l = list; l != NULL; l = l->next) {
		GtkEventController *controller = l->data;
		GtkWidget *widget = gtk_event_controller_get_widget(controller);

		if (widget != NULL)
			gtk_widget_remove_controller(widget, controller);
		g_object_unref(controller);
	}
	g_slist_free(list);
	purple_conversation_set_data(conv, CONTROLLERS_KEY, NULL);
	purple_conversation_set_data(conv, COUNT_KEY, GINT_TO_POINTER(0));
}

static void
attach_window(PidginWindow *win)
{
	GtkWidget *window;

	if (!is_notifiable_window(win))
		return;
	window = pidgin_conv_window_get_window(win);
	if (g_signal_handler_find(window, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, window_active_cb, NULL))
		return;
	g_signal_connect(window, "notify::is-active", G_CALLBACK(window_active_cb), win);
	g_signal_connect(window, "notify::title", G_CALLBACK(window_title_cb), win);
}

static int
attach_signals(PurpleConversation *conv)
{
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);
	GSList *list = NULL, *l;
	GtkEventController *controller;
	GtkWidget *page, *entry;

	if (gtkconv == NULL || !PIDGIN_IS_PIDGIN_CONVERSATION(conv)) {
		purple_debug_misc("notify", "Failed to find gtkconv\n");
		return 0;
	}
	page = pidgin_conv_get_tab_container(gtkconv);
	entry = pidgin_conv_get_compose_entry(gtkconv);

	if (purple_prefs_get_bool(PREFS "/notify_focus")) {
		controller = gtk_event_controller_focus_new();
		g_signal_connect(controller, "enter", G_CALLBACK(focus_enter_cb), conv);
		add_controller(page, controller, &list);
	}

	if (purple_prefs_get_bool(PREFS "/notify_click")) {
		controller = GTK_EVENT_CONTROLLER(gtk_gesture_click_new());
		gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(controller), 0);
		gtk_event_controller_set_propagation_phase(controller, GTK_PHASE_CAPTURE);
		g_signal_connect(controller, "pressed", G_CALLBACK(click_cb), conv);
		add_controller(page, controller, &list);
	}

	if (purple_prefs_get_bool(PREFS "/notify_type")) {
		controller = gtk_event_controller_key_new();
		gtk_event_controller_set_propagation_phase(controller, GTK_PHASE_CAPTURE);
		g_signal_connect(controller, "key-pressed", G_CALLBACK(key_cb), conv);
		add_controller(entry, controller, &list);
	}

	for (l = list; l != NULL; l = l->next)
		g_object_ref(l->data);
	purple_conversation_set_data(conv, CONTROLLERS_KEY, list);

	if (gtkconv->win != NULL)
		attach_window(gtkconv->win);
	return 0;
}

/**************************************************************************
 * libpurple and conversation signals
 **************************************************************************/

static gboolean
message_displayed_cb(PurpleAccount *account, const char *who, char *message,
                     PurpleConversation *conv, PurpleMessageFlags flags)
{
	PurpleConversationType ct = purple_conversation_get_type(conv);

	/* Ignore anything that's not a received message or a system message */
	if (!(flags & (PURPLE_MESSAGE_RECV|PURPLE_MESSAGE_SYSTEM)))
		return FALSE;
	/* Don't highlight for delayed messages */
	if ((flags & PURPLE_MESSAGE_RECV) && (flags & PURPLE_MESSAGE_DELAYED))
		return FALSE;
	/* Check whether to highlight for system message for either chat or IM */
	if (flags & PURPLE_MESSAGE_SYSTEM) {
		switch (ct) {
			case PURPLE_CONV_TYPE_CHAT:
				if (!purple_prefs_get_bool(PREFS "/type_chat_sys"))
					return FALSE;
				break;
			case PURPLE_CONV_TYPE_IM:
				if (!purple_prefs_get_bool(PREFS "/type_im_sys"))
					return FALSE;
				break;
			default:
				/* System message not from chat or IM, ignore */
				return FALSE;
		}
	}

	/* If it's a chat, check if we should only highlight when nick is mentioned */
	if (ct == PURPLE_CONV_TYPE_CHAT &&
	    purple_prefs_get_bool(PREFS "/type_chat_nick") &&
	    !(flags & PURPLE_MESSAGE_NICK))
		return FALSE;

	/* Nothing speaks against notifying, do so */
	notify(conv, TRUE);

	if (purple_prefs_get_bool(PREFS4 "/method_notification") && conv_count(conv) > 0 &&
	    (flags & PURPLE_MESSAGE_RECV))
		pidgin_notification_new_message(account, ct, purple_conversation_get_name(conv),
		                                 conv, who, message);

	return FALSE;
}

static void
im_sent_im(PurpleAccount *account, const char *receiver, const char *message)
{
	PurpleConversation *conv;

	if (purple_prefs_get_bool(PREFS "/notify_send")) {
		conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, receiver, account);
		if (conv != NULL)
			unnotify(conv, TRUE);
	}
}

static void
chat_sent_im(PurpleAccount *account, const char *message, int id)
{
	PurpleConversation *conv;

	if (purple_prefs_get_bool(PREFS "/notify_send")) {
		conv = purple_find_chat(purple_account_get_connection(account), id);
		if (conv != NULL)
			unnotify(conv, TRUE);
	}
}

static void
conv_created(PurpleConversation *conv)
{
	purple_conversation_set_data(conv, COUNT_KEY, GINT_TO_POINTER(0));

	/* always attach the signals, notify() will take care of conversation
	 * type checking */
	attach_signals(conv);
}

static void
conv_switched(PurpleConversation *conv)
{
	/*
	 * If the conversation was switched, then make sure we re-notify
	 * because Pidgin will have overwritten our custom window title.
	 */
	notify(conv, FALSE);
}

static void
conv_displayed(PidginConversation *gtkconv)
{
	/* A conversation that moved to another window (DnD, attach). */
	if (gtkconv->win != NULL)
		attach_window(gtkconv->win);
}

static void
deleting_conv(PurpleConversation *conv)
{
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);

	if (gtkconv == NULL)
		return;

	detach_signals(conv);
	if (is_notifiable_window(gtkconv->win))
		handle_urgent(gtkconv->win, FALSE);
	purple_conversation_set_data(conv, COUNT_KEY, GINT_TO_POINTER(0));
}

static void
apply_method(void)
{
	GList *convs;

	for (convs = purple_get_conversations(); convs != NULL; convs = convs->next) {
		PurpleConversation *conv = convs->data;

		/* remove notifications */
		unnotify(conv, FALSE);

		if (conv_count(conv) != 0)
			/* reattach appropriate notifications */
			notify(conv, FALSE);
	}
}

static void
apply_notify(void)
{
	GList *convs;

	for (convs = purple_get_conversations(); convs != NULL; convs = convs->next) {
		PurpleConversation *conv = convs->data;
		int count = conv_count(conv);

		/* detach signals and reattach appropriate signals */
		detach_signals(conv);
		attach_signals(conv);
		purple_conversation_set_data(conv, COUNT_KEY, GINT_TO_POINTER(count));
	}
}

static void
method_pref_cb(const char *name, PurplePrefType type, gconstpointer val, gpointer data)
{
	apply_method();
}

static void
removal_pref_cb(const char *name, PurplePrefType type, gconstpointer val, gpointer data)
{
	apply_notify();
}

/**************************************************************************
 * Prefs
 **************************************************************************/

static void
add_bool(PurplePluginPrefFrame *frame, const char *pref, const char *label)
{
	purple_plugin_pref_frame_add(frame,
		purple_plugin_pref_new_with_name_and_label(pref, label));
}

static PurplePluginPrefFrame *
get_plugin_pref_frame(PurplePlugin *plugin)
{
	PurplePluginPrefFrame *frame = purple_plugin_pref_frame_new();
	PurplePluginPref *pref;

	purple_plugin_pref_frame_add(frame, purple_plugin_pref_new_with_label(_("Notify For")));
	add_bool(frame, PREFS "/type_im", _("_IM windows"));
	add_bool(frame, PREFS "/type_im_sys", _("\tS_ystem messages"));
	add_bool(frame, PREFS "/type_chat", _("C_hat windows"));
	add_bool(frame, PREFS "/type_chat_nick", _("\t_Only when someone says your username"));
	add_bool(frame, PREFS "/type_chat_sys", _("\tS_ystem messages"));
	add_bool(frame, PREFS "/type_focused", _("_Focused windows"));

	purple_plugin_pref_frame_add(frame,
		purple_plugin_pref_new_with_label(_("Notification Methods")));
	add_bool(frame, PREFS "/method_string", _("Prepend _string into window title:"));
	pref = purple_plugin_pref_new_with_name_and_label(PREFS "/title_string", _("Title string"));
	purple_plugin_pref_set_max_length(pref, 10);
	purple_plugin_pref_frame_add(frame, pref);
	add_bool(frame, PREFS "/method_count", _("Insert c_ount of new messages into window title"));
	add_bool(frame, PREFS "/method_urgent", _("Set window _attention (tray icon)"));
	add_bool(frame, PREFS "/method_raise", _("R_aise conversation window"));
	/* Translators: "Present" as used here is a verb. The plugin presents
	 * the window to the user. */
	add_bool(frame, PREFS "/method_present", _("_Present conversation window"));
	add_bool(frame, PREFS4 "/method_notification", _("Show a desktop _notification"));

	purple_plugin_pref_frame_add(frame,
		purple_plugin_pref_new_with_label(_("Notification Removal")));
	add_bool(frame, PREFS "/notify_focus", _("Remove when conversation window _gains focus"));
	add_bool(frame, PREFS "/notify_click", _("Remove when conversation window _receives click"));
	add_bool(frame, PREFS "/notify_type", _("Remove when _typing in conversation window"));
	add_bool(frame, PREFS "/notify_send", _("Remove when a _message gets sent"));

	return frame;
}

static gboolean
plugin_load(PurplePlugin *plugin)
{
	GList *convs;
	void *conv_handle = purple_conversations_get_handle();
	void *gtk_conv_handle = pidgin_conversations_get_handle();
	static const char *const methods[] = { "method_string", "title_string",
		"method_count", "method_urgent", "method_raise", "method_present", NULL };
	static const char *const removals[] = { "notify_focus", "notify_click",
		"notify_type", NULL };
	int i;

	my_plugin = plugin;

	purple_signal_connect(gtk_conv_handle, "displayed-im-msg", plugin,
	                      PURPLE_CALLBACK(message_displayed_cb), NULL);
	purple_signal_connect(gtk_conv_handle, "displayed-chat-msg", plugin,
	                      PURPLE_CALLBACK(message_displayed_cb), NULL);
	purple_signal_connect(gtk_conv_handle, "conversation-switched", plugin,
	                      PURPLE_CALLBACK(conv_switched), NULL);
	purple_signal_connect(gtk_conv_handle, "conversation-displayed", plugin,
	                      PURPLE_CALLBACK(conv_displayed), NULL);
	purple_signal_connect(conv_handle, "sent-im-msg", plugin,
	                      PURPLE_CALLBACK(im_sent_im), NULL);
	purple_signal_connect(conv_handle, "sent-chat-msg", plugin,
	                      PURPLE_CALLBACK(chat_sent_im), NULL);
	purple_signal_connect(conv_handle, "conversation-created", plugin,
	                      PURPLE_CALLBACK(conv_created), NULL);
	purple_signal_connect(conv_handle, "chat-joined", plugin,
	                      PURPLE_CALLBACK(conv_created), NULL);
	purple_signal_connect(conv_handle, "deleting-conversation", plugin,
	                      PURPLE_CALLBACK(deleting_conv), NULL);

	for (i = 0; methods[i] != NULL; i++) {
		char *pref = g_strdup_printf(PREFS "/%s", methods[i]);
		purple_prefs_connect_callback(plugin, pref, method_pref_cb, NULL);
		g_free(pref);
	}
	for (i = 0; removals[i] != NULL; i++) {
		char *pref = g_strdup_printf(PREFS "/%s", removals[i]);
		purple_prefs_connect_callback(plugin, pref, removal_pref_cb, NULL);
		g_free(pref);
	}

	for (convs = purple_get_conversations(); convs != NULL; convs = convs->next) {
		PurpleConversation *conv = convs->data;

		/* attach signals */
		if (PIDGIN_IS_PIDGIN_CONVERSATION(conv))
			attach_signals(conv);
	}

	return TRUE;
}

static gboolean
plugin_unload(PurplePlugin *plugin)
{
	GList *convs;
	GList *l;

	for (convs = purple_get_conversations(); convs != NULL; convs = convs->next) {
		PurpleConversation *conv = convs->data;

		if (!PIDGIN_IS_PIDGIN_CONVERSATION(conv))
			continue;
		/* kill signals */
		detach_signals(conv);
		if (PIDGIN_CONVERSATION(conv) != NULL && is_notifiable_window(PIDGIN_CONVERSATION(conv)->win))
			handle_urgent(PIDGIN_CONVERSATION(conv)->win, FALSE);
	}
	for (l = pidgin_conv_windows_get_list(); l != NULL; l = l->next) {
		g_signal_handlers_disconnect_by_func(pidgin_conv_window_get_window(l->data),
		                                     window_active_cb, l->data);
		g_signal_handlers_disconnect_by_func(pidgin_conv_window_get_window(l->data),
		                                     window_title_cb, l->data);
		apply_title(l->data, FALSE);
	}
	my_plugin = NULL;
	return TRUE;
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

	NOTIFY_PLUGIN_ID,                                 /**< id             */
	N_("Message Notification"),                       /**< name           */
	DISPLAY_VERSION,                                  /**< version        */
	                                                  /**  summary        */
	N_("Provides a variety of ways of notifying you of unread messages."),
	                                                  /**  description    */
	N_("Provides a variety of ways of notifying you of unread messages."),
	                                                  /**< author         */
	"Etan Reisner <deryni@eden.rutgers.edu>,\nBrian Tarricone <bjt23@cornell.edu>",
	PURPLE_WEBSITE,                                   /**< homepage       */

	plugin_load,                                      /**< load           */
	plugin_unload,                                    /**< unload         */
	NULL,                                             /**< destroy        */

	NULL,                                             /**< ui_info        */
	NULL,                                             /**< extra_info     */
	&prefs_info,                                      /**< prefs_info     */
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
	purple_prefs_add_none("/plugins/gtk/X11");
	purple_prefs_add_none(PREFS);

	purple_prefs_add_bool(PREFS "/type_im", TRUE);
	purple_prefs_add_bool(PREFS "/type_im_sys", FALSE);
	purple_prefs_add_bool(PREFS "/type_chat", FALSE);
	purple_prefs_add_bool(PREFS "/type_chat_nick", FALSE);
	purple_prefs_add_bool(PREFS "/type_chat_sys", FALSE);
	purple_prefs_add_bool(PREFS "/type_focused", FALSE);
	purple_prefs_add_bool(PREFS "/method_string", FALSE);
	purple_prefs_add_string(PREFS "/title_string", "(*)");
	purple_prefs_add_bool(PREFS "/method_urgent", FALSE);
	purple_prefs_add_bool(PREFS "/method_count", FALSE);
	/* Pidgin 2's X property method: registered so the shared key keeps
	 * its type, not used (no X11). */
	purple_prefs_add_bool(PREFS "/method_count_xprop", FALSE);
	purple_prefs_add_bool(PREFS "/method_raise", FALSE);
	purple_prefs_add_bool(PREFS "/method_present", FALSE);
	purple_prefs_add_bool(PREFS "/notify_focus", TRUE);
	purple_prefs_add_bool(PREFS "/notify_click", FALSE);
	purple_prefs_add_bool(PREFS "/notify_type", TRUE);
	purple_prefs_add_bool(PREFS "/notify_send", TRUE);
	purple_prefs_add_bool(PREFS "/notify_switch", TRUE);

	purple_prefs_add_none(PIDGIN4_PREFS_ROOT "/plugins");
	purple_prefs_add_none(PREFS4);
	purple_prefs_add_bool(PREFS4 "/method_notification", FALSE);
}

PURPLE_INIT_PLUGIN(notify, init_plugin, info)
