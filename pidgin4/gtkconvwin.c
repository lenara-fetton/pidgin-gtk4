/*
 * pidgin
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
 * pidgin4 (M4b): conversation windows (the window half of
 * pidgin/gtkconv.c): a GtkApplicationWindow with a GtkPopoverMenuBar built
 * from Pidgin 2's menu_items[] and a GtkNotebook of conversations.
 *
 * The window's gtkconvs list follows the notebook: page-added, -removed
 * and -reordered keep it in step, so tabs dragged between windows (the
 * notebooks share a group) or out of one ("create-window") need no other
 * bookkeeping. Conversations created hidden (/pidgin/conversations/im/
 * hide_new) live in a window that is never shown.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "account.h"
#include "blist.h"
#include "debug.h"
#include "prefs.h"

#include "gtkblist.h"
#include "gtkconv.h"
#include "gtkconvwin.h"
#include "gtkdialogs.h"

#define CONV_PREFS PIDGIN_PREFS_ROOT "/conversations"
#define CONV4_PREFS PIDGIN4_PREFS_ROOT "/conversations"
#define NOTEBOOK_GROUP "pidgin4-conversations"

static GList *window_list = NULL;
static PidginWindow *hidden_win = NULL;

static void update_tabs_visible(PidginWindow *win);

/**************************************************************************
 * Window list
 **************************************************************************/

GList *
pidgin_conv_windows_get_list(void)
{
	return window_list;
}

PidginWindow *
pidgin_conv_window_get_hidden(void)
{
	return hidden_win;
}

gboolean
pidgin_conv_window_is_hidden(const PidginWindow *win)
{
	return win != NULL && win == hidden_win;
}

static PidginConversation *
page_gtkconv(GtkWidget *page)
{
	return page ? g_object_get_data(G_OBJECT(page), "PidginConversation") : NULL;
}

PidginConversation *
pidgin_conv_window_get_gtkconv_at_index(const PidginWindow *win, int index)
{
	GtkWidget *page;

	if (index == -1)
		index = 0;
	page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(win->notebook), index);
	return page_gtkconv(page);
}

PidginConversation *
pidgin_conv_window_get_active_gtkconv(const PidginWindow *win)
{
	int index;

	g_return_val_if_fail(win != NULL, NULL);
	index = gtk_notebook_get_current_page(GTK_NOTEBOOK(win->notebook));
	if (index < 0)
		return NULL;
	return page_gtkconv(gtk_notebook_get_nth_page(GTK_NOTEBOOK(win->notebook), index));
}

PurpleConversation *
pidgin_conv_window_get_active_conversation(const PidginWindow *win)
{
	PidginConversation *gtkconv = pidgin_conv_window_get_active_gtkconv(win);

	return gtkconv ? gtkconv->active_conv : NULL;
}

gboolean
pidgin_conv_window_is_active_conversation(const PurpleConversation *conv)
{
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);

	return gtkconv != NULL && gtkconv->win != NULL &&
	       pidgin_conv_window_get_active_gtkconv(gtkconv->win) == gtkconv;
}

gboolean
pidgin_conv_window_has_focus(PidginWindow *win)
{
	return win != NULL && win != hidden_win && gtk_window_is_active(GTK_WINDOW(win->window));
}

GList *
pidgin_conv_window_get_gtkconvs(PidginWindow *win)
{
	return win->gtkconvs;
}

/* M7: accessor for plugins (notify, markerline, iconaway). */
GtkWidget *
pidgin_conv_window_get_window(PidginWindow *win)
{
	g_return_val_if_fail(win != NULL, NULL);
	return win->window;
}

GtkWidget *
pidgin_conv_window_get_notebook(PidginWindow *win)
{
	g_return_val_if_fail(win != NULL, NULL);
	return win->notebook;
}

guint
pidgin_conv_window_get_gtkconv_count(PidginWindow *win)
{
	return g_list_length(win->gtkconvs);
}

static gboolean
window_has_type(PidginWindow *win, PurpleConversationType type)
{
	GList *l;

	for (l = win->gtkconvs; l != NULL; l = l->next) {
		PidginConversation *gtkconv = l->data;

		if (purple_conversation_get_type(gtkconv->active_conv) == type)
			return TRUE;
	}
	return FALSE;
}

PidginWindow *
pidgin_conv_window_first_with_type(PurpleConversationType type)
{
	GList *l;

	for (l = window_list; l != NULL; l = l->next)
		if (window_has_type(l->data, type))
			return l->data;
	return NULL;
}

PidginWindow *
pidgin_conv_window_last_with_type(PurpleConversationType type)
{
	GList *l;

	for (l = g_list_last(window_list); l != NULL; l = l->prev)
		if (window_has_type(l->data, type))
			return l->data;
	return NULL;
}

/**************************************************************************
 * Menubar and actions
 **************************************************************************/

/* Actions on the active conversation, forwarded to gtkconv.c. */
static const char *const conv_action_names[] = {
	"find", "view-log", "save-as", "clear", "send-file", "get-attention",
	"add-pounce", "get-info", "invite", "alias", "block", "unblock", "add",
	"remove", "insert-link", "insert-image", "close", NULL
};

static void
conv_action_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	PidginWindow *win = data;

	pidgin_conv_action(pidgin_conv_window_get_active_gtkconv(win),
	                   g_action_get_name(G_ACTION(action)));
}

static void
toggle_conv_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	PidginWindow *win = data;
	PidginConversation *gtkconv = pidgin_conv_window_get_active_gtkconv(win);
	const char *name = g_action_get_name(G_ACTION(action));

	pidgin_conv_action(gtkconv, name);
	g_simple_action_set_state(action,
		g_variant_new_boolean(pidgin_conv_action_enabled(gtkconv, name)));
}

static void
toggle_pref_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	const char *name = g_action_get_name(G_ACTION(action));
	const char *pref = purple_strequal(name, "toolbar")
		? CONV_PREFS "/show_formatting_toolbar" : CONV_PREFS "/show_timestamps";
	gboolean value = !purple_prefs_get_bool(pref);

	purple_prefs_set_bool(pref, value);
	g_simple_action_set_state(action, g_variant_new_boolean(value));
}

static void
new_im_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	pidgin_dialogs_im();
}

static void
join_chat_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	pidgin_blist_joinchat_show();
}

static void
switch_relative(PidginWindow *win, int delta)
{
	GtkNotebook *nb = GTK_NOTEBOOK(win->notebook);
	int n = gtk_notebook_get_n_pages(nb), cur = gtk_notebook_get_current_page(nb);

	if (n > 0)
		gtk_notebook_set_current_page(nb, ((cur + delta) % n + n) % n);
}

static void
next_tab_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	switch_relative(data, 1);
}

static void
prev_tab_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	switch_relative(data, -1);
}

/* Pidgin 2's move_to_next_unread_tab(): the next tab with unseen text, else
 * the next tab. */
static void
unread_tab(PidginWindow *win, gboolean forward)
{
	GtkNotebook *nb = GTK_NOTEBOOK(win->notebook);
	int n = gtk_notebook_get_n_pages(nb), cur = gtk_notebook_get_current_page(nb), i;
	PidginUnseenState state;

	for (state = PIDGIN_UNSEEN_NICK; state >= PIDGIN_UNSEEN_TEXT; state--) {
		for (i = 1; i < n; i++) {
			int idx = ((cur + (forward ? i : -i)) % n + n) % n;
			PidginConversation *gtkconv = pidgin_conv_window_get_gtkconv_at_index(win, idx);

			if (gtkconv != NULL && gtkconv->unseen_state >= state) {
				gtk_notebook_set_current_page(nb, idx);
				return;
			}
		}
	}
	switch_relative(win, forward ? 1 : -1);
}

static void
next_unread_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	unread_tab(data, TRUE);
}

static void
prev_unread_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	unread_tab(data, FALSE);
}

static void
tab_n_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	PidginWindow *win = data;
	int n = g_variant_get_int32(param);

	if (n >= 1 && n <= gtk_notebook_get_n_pages(GTK_NOTEBOOK(win->notebook)))
		gtk_notebook_set_current_page(GTK_NOTEBOOK(win->notebook), n - 1);
}

void
pidgin_conv_window_move_tab(PidginWindow *win, int index, int new_index)
{
	GtkNotebook *nb = GTK_NOTEBOOK(win->notebook);
	GtkWidget *page = gtk_notebook_get_nth_page(nb, index);
	int n = gtk_notebook_get_n_pages(nb);

	if (page == NULL || n < 2)
		return;
	gtk_notebook_reorder_child(nb, page, ((new_index % n) + n) % n);
}

static void
move_left_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	PidginWindow *win = data;
	int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(win->notebook));

	pidgin_conv_window_move_tab(win, cur, cur - 1);
}

static void
move_right_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	PidginWindow *win = data;
	int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(win->notebook));

	pidgin_conv_window_move_tab(win, cur, cur + 1);
}

static const GActionEntry window_actions[] = {
	{ .name = "new-im", .activate = new_im_cb },
	{ .name = "join-chat", .activate = join_chat_cb },
	{ .name = "logging", .activate = toggle_conv_cb, .state = "false" },
	{ .name = "sounds", .activate = toggle_conv_cb, .state = "true" },
	{ .name = "toolbar", .activate = toggle_pref_cb, .state = "true" },
	{ .name = "timestamps", .activate = toggle_pref_cb, .state = "true" },
	{ .name = "next-tab", .activate = next_tab_cb },
	{ .name = "prev-tab", .activate = prev_tab_cb },
	{ .name = "next-unread", .activate = next_unread_cb },
	{ .name = "prev-unread", .activate = prev_unread_cb },
	{ .name = "tab", .activate = tab_n_cb, .parameter_type = "i" },
	{ .name = "move-tab-left", .activate = move_left_cb },
	{ .name = "move-tab-right", .activate = move_right_cb },
};

static void
append_item(GMenu *menu, const char *label, const char *action, gboolean hide_disabled)
{
	GMenuItem *item = g_menu_item_new(label, action);

	if (hide_disabled)
		g_menu_item_set_attribute(item, "hidden-when", "s", "action-disabled");
	g_menu_append_item(menu, item);
	g_object_unref(item);
}

static GMenu *
build_menubar(PidginWindow *win)
{
	GMenu *bar = g_menu_new(), *menu, *section;

	/* Pidgin 2's menu_items[] (pidgin/gtkconv.c) */
	menu = g_menu_new();
	section = g_menu_new();
	append_item(section, _("New Instant _Message..."), "conv.new-im", FALSE);
	append_item(section, _("Join a _Chat..."), "conv.join-chat", FALSE);
	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);

	section = g_menu_new();
	append_item(section, _("_Find..."), "conv.find", FALSE);
	append_item(section, _("View _Log"), "conv.view-log", FALSE);
	append_item(section, _("_Save As..."), "conv.save-as", FALSE);
	append_item(section, _("Clea_r Scrollback"), "conv.clear", FALSE);
	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);

	section = g_menu_new();
	append_item(section, _("Se_nd File..."), "conv.send-file", FALSE);
	append_item(section, _("Get _Attention"), "conv.get-attention", FALSE);
	append_item(section, _("Add Buddy _Pounce..."), "conv.add-pounce", FALSE);
	append_item(section, _("_Get Info"), "conv.get-info", FALSE);
	append_item(section, _("In_vite..."), "conv.invite", TRUE);
	win->menu.more = g_menu_new();
	g_menu_append_submenu(section, _("M_ore"), G_MENU_MODEL(win->menu.more));
	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);

	section = g_menu_new();
	append_item(section, _("Al_ias..."), "conv.alias", FALSE);
	append_item(section, _("_Block..."), "conv.block", TRUE);
	append_item(section, _("_Unblock..."), "conv.unblock", TRUE);
	append_item(section, _("_Add..."), "conv.add", TRUE);
	append_item(section, _("_Remove..."), "conv.remove", TRUE);
	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);

	section = g_menu_new();
	append_item(section, _("Insert Lin_k..."), "conv.insert-link", FALSE);
	append_item(section, _("Insert Imag_e..."), "conv.insert-image", FALSE);
	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);

	section = g_menu_new();
	append_item(section, _("_Close"), "conv.close", FALSE);
	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);
	g_menu_append_submenu(bar, _("_Conversation"), G_MENU_MODEL(menu));
	g_object_unref(menu);

	menu = g_menu_new();
	section = g_menu_new();
	append_item(section, _("Enable _Logging"), "conv.logging", FALSE);
	append_item(section, _("Enable _Sounds"), "conv.sounds", FALSE);
	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);
	section = g_menu_new();
	append_item(section, _("Show Formatting _Toolbars"), "conv.toolbar", FALSE);
	append_item(section, _("Show Ti_mestamps"), "conv.timestamps", FALSE);
	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);
	g_menu_append_submenu(bar, _("_Options"), G_MENU_MODEL(menu));
	g_object_unref(menu);

	return bar;
}

static void
set_enabled(PidginWindow *win, const char *name, gboolean enabled)
{
	GAction *a = g_action_map_lookup_action(G_ACTION_MAP(win->actions), name);

	if (a != NULL)
		g_simple_action_set_enabled(G_SIMPLE_ACTION(a), enabled);
}

static void
set_state(PidginWindow *win, const char *name, gboolean state)
{
	GAction *a = g_action_map_lookup_action(G_ACTION_MAP(win->actions), name);

	if (a != NULL)
		g_simple_action_set_state(G_SIMPLE_ACTION(a), g_variant_new_boolean(state));
}

void
pidgin_conv_window_update_menu(PidginWindow *win)
{
	PidginConversation *gtkconv;
	const char *method;
	int i;

	if (win == NULL || win == hidden_win || win->closing)
		return;
	gtkconv = pidgin_conv_window_get_active_gtkconv(win);

	for (i = 0; conv_action_names[i] != NULL; i++)
		set_enabled(win, conv_action_names[i],
		            gtkconv != NULL && pidgin_conv_action_enabled(gtkconv, conv_action_names[i]));

	set_enabled(win, "logging", gtkconv != NULL);
	set_state(win, "logging", pidgin_conv_action_enabled(gtkconv, "logging"));
	/* Sounds are M6; the per-conversation mute is kept (gtk-mute-sound). */
	method = purple_prefs_exists(PIDGIN_PREFS_ROOT "/sound/method")
		? purple_prefs_get_string(PIDGIN_PREFS_ROOT "/sound/method") : NULL;
	set_enabled(win, "sounds", gtkconv != NULL && !purple_strequal(method, "none"));
	set_state(win, "sounds", pidgin_conv_action_enabled(gtkconv, "sounds"));
	set_state(win, "toolbar", purple_prefs_get_bool(CONV_PREFS "/show_formatting_toolbar"));
	set_state(win, "timestamps", purple_prefs_get_bool(CONV_PREFS "/show_timestamps"));

	/* Conversation → More: the prpl's extended menu. */
	g_clear_object(&win->more_actions);
	win->more_actions = g_simple_action_group_new();
	pidgin_conv_fill_more_menu(gtkconv, win->menu.more, win->more_actions);
	gtk_widget_insert_action_group(win->window, "more", G_ACTION_GROUP(win->more_actions));
}

/**************************************************************************
 * Tab context menu
 **************************************************************************/

static void
tab_close_others_cb(GSimpleAction *a, GVariant *p, gpointer data)
{
	PidginWindow *win = data;
	GList *l, *copy = g_list_copy(win->gtkconvs);

	for (l = copy; l != NULL; l = l->next)
		if (l->data != win->tab_menu_conv)
			pidgin_conv_close(l->data);
	g_list_free(copy);
}

static void
tab_detach_cb(GSimpleAction *a, GVariant *p, gpointer data)
{
	PidginWindow *win = data, *new_win;
	PidginConversation *gtkconv = win->tab_menu_conv;

	if (gtkconv == NULL || g_list_length(win->gtkconvs) < 2)
		return;
	new_win = pidgin_conv_window_new();
	pidgin_conv_window_remove_gtkconv(win, gtkconv);
	pidgin_conv_window_add_gtkconv(new_win, gtkconv);
	pidgin_conv_window_show(new_win);
}

static void
tab_close_cb(GSimpleAction *a, GVariant *p, gpointer data)
{
	PidginWindow *win = data;

	if (win->tab_menu_conv != NULL)
		pidgin_conv_close(win->tab_menu_conv);
}

static const GActionEntry tab_actions[] = {
	{ .name = "close-others", .activate = tab_close_others_cb },
	{ .name = "detach", .activate = tab_detach_cb },
	{ .name = "close", .activate = tab_close_cb },
};

static void
tab_pressed_cb(GtkGestureClick *gesture, int n, double x, double y, PidginConversation *gtkconv)
{
	PidginWindow *win = gtkconv->win;
	GtkWidget *tabby = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	GdkRectangle rect;
	graphene_point_t pt;

	if (win == NULL || win == hidden_win)
		return;
	if (win->tab_menu == NULL) {
		GMenu *menu = g_menu_new();

		g_menu_append(menu, _("Close other tabs"), "tab.close-others");
		g_menu_append(menu, _("Detach this tab"), "tab.detach");
		g_menu_append(menu, _("Close this tab"), "tab.close");
		win->tab_menu = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
		gtk_popover_set_has_arrow(GTK_POPOVER(win->tab_menu), FALSE);
		gtk_widget_set_parent(win->tab_menu, win->notebook);
		g_object_unref(menu);
	}
	win->tab_menu_conv = gtkconv;
	if (!gtk_widget_compute_point(tabby, win->notebook, &GRAPHENE_POINT_INIT((float)x, (float)y),
	                              &pt))
		pt = GRAPHENE_POINT_INIT(0, 0);
	rect = (GdkRectangle){ (int)pt.x, (int)pt.y, 1, 1 };
	gtk_popover_set_pointing_to(GTK_POPOVER(win->tab_menu), &rect);
	gtk_popover_popup(GTK_POPOVER(win->tab_menu));
	gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

/**************************************************************************
 * Notebook
 **************************************************************************/

static void
rebuild_list(PidginWindow *win)
{
	GtkNotebook *nb = GTK_NOTEBOOK(win->notebook);
	int i, n = gtk_notebook_get_n_pages(nb);

	g_list_free(win->gtkconvs);
	win->gtkconvs = NULL;
	for (i = n - 1; i >= 0; i--) {
		PidginConversation *gtkconv = page_gtkconv(gtk_notebook_get_nth_page(nb, i));

		if (gtkconv != NULL)
			win->gtkconvs = g_list_prepend(win->gtkconvs, gtkconv);
	}
}

static gboolean
destroy_if_empty_cb(gpointer data)
{
	PidginWindow *win = data;

	win->destroy_idle = 0;
	if (win->gtkconvs == NULL)
		pidgin_conv_window_destroy(win);
	return G_SOURCE_REMOVE;
}

static void
page_added_cb(GtkNotebook *nb, GtkWidget *page, guint num, PidginWindow *win)
{
	PidginConversation *gtkconv = page_gtkconv(page);

	if (gtkconv == NULL)
		return;
	if (gtkconv->win != NULL && gtkconv->win != win) {
		/* Dragged here from another window (that one had page-removed). */
		gtkconv->win->gtkconvs = g_list_remove(gtkconv->win->gtkconvs, gtkconv);
	}
	pidgin_conv_set_window(gtkconv, win);
	rebuild_list(win);
	gtk_notebook_set_tab_reorderable(nb, page, win != hidden_win);
	gtk_notebook_set_tab_detachable(nb, page, win != hidden_win);
	update_tabs_visible(win);
	pidgin_conv_update_tab(gtkconv);
}

static void
page_removed_cb(GtkNotebook *nb, GtkWidget *page, guint num, PidginWindow *win)
{
	PidginConversation *gtkconv = page_gtkconv(page);

	rebuild_list(win);
	if (gtkconv != NULL && gtkconv->win == win)
		pidgin_conv_set_window(gtkconv, NULL);
	update_tabs_visible(win);
	if (win->gtkconvs == NULL && win != hidden_win && !win->closing && win->destroy_idle == 0)
		win->destroy_idle = g_idle_add(destroy_if_empty_cb, win);
	else
		pidgin_conv_window_update_menu(win);
}

static void
page_reordered_cb(GtkNotebook *nb, GtkWidget *page, guint num, PidginWindow *win)
{
	rebuild_list(win);
}

static void
switch_page_cb(GtkNotebook *nb, GtkWidget *page, guint num, PidginWindow *win)
{
	PidginConversation *gtkconv = page_gtkconv(page);

	if (gtkconv == NULL || win == hidden_win)
		return;
	gtk_window_set_title(GTK_WINDOW(win->window),
	                     purple_conversation_get_title(gtkconv->active_conv));
	pidgin_conv_window_update_menu(win);
	if (gtk_window_is_active(GTK_WINDOW(win->window)))
		pidgin_conv_seen(gtkconv);
	pidgin_conv_update_tab(gtkconv);
	if (!gtk_widget_has_focus(gtkconv->entry))
		gtk_widget_grab_focus(gtkconv->entry);
	purple_signal_emit(pidgin_conversations_get_handle(), "conversation-switched",
	                   gtkconv->active_conv);
}

static GtkNotebook *
create_window_cb(GtkNotebook *nb, GtkWidget *page, PidginWindow *win)
{
	PidginWindow *new_win = pidgin_conv_window_new();

	pidgin_conv_window_show(new_win);
	return GTK_NOTEBOOK(new_win->notebook);
}

static void
update_tabs_visible(PidginWindow *win)
{
	int side = purple_prefs_get_int(CONV_PREFS "/tab_side") & 3;

	gtk_notebook_set_tab_pos(GTK_NOTEBOOK(win->notebook), (GtkPositionType)side);
	/* One conversation: no tab bar, as Pidgin 2. */
	gtk_notebook_set_show_tabs(GTK_NOTEBOOK(win->notebook),
	                           gtk_notebook_get_n_pages(GTK_NOTEBOOK(win->notebook)) > 1);
}

static void
active_changed_cb(GObject *obj, GParamSpec *pspec, PidginWindow *win)
{
	if (gtk_window_is_active(GTK_WINDOW(win->window)))
		pidgin_conv_seen(pidgin_conv_window_get_active_gtkconv(win));
}

static void
save_size(PidginWindow *win)
{
	int w = 0, h = 0;

	gtk_window_get_default_size(GTK_WINDOW(win->window), &w, &h);
	if (w > 0 && h > 0) {
		purple_prefs_set_int(CONV4_PREFS "/width", w);
		purple_prefs_set_int(CONV4_PREFS "/height", h);
	}
}

static gboolean
close_request_cb(GtkWindow *window, PidginWindow *win)
{
	GList *l, *copy;

	save_size(win);
	/* Closing the window closes its conversations; the last page removed
	 * destroys the window. */
	copy = g_list_copy(win->gtkconvs);
	for (l = copy; l != NULL; l = l->next)
		pidgin_conv_close(l->data);
	g_list_free(copy);
	if (win->gtkconvs == NULL)
		pidgin_conv_window_destroy(win);
	return TRUE;
}

static void
tab_side_pref_cb(const char *name, PurplePrefType type, gconstpointer value, gpointer data)
{
	GList *l;

	for (l = window_list; l != NULL; l = l->next)
		update_tabs_visible(l->data);
}

static PidginWindow *
window_new(gboolean hidden)
{
	PidginWindow *win = g_new0(PidginWindow, 1);
	GtkApplication *app = pidgin_application_get();
	GtkWidget *vbox;
	GMenu *model;

	if (!hidden && app != NULL)
		win->window = gtk_application_window_new(app);
	else
		win->window = gtk_window_new();
	gtk_window_set_title(GTK_WINDOW(win->window), _("Conversations"));
	gtk_window_set_icon_name(GTK_WINDOW(win->window), PIDGIN4_APP_ID);
	gtk_window_set_default_size(GTK_WINDOW(win->window),
		MAX(purple_prefs_get_int(CONV4_PREFS "/width"), 200),
		MAX(purple_prefs_get_int(CONV4_PREFS "/height"), 150));
	gtk_widget_add_css_class(win->window, "pidgin-conv-window");
	g_object_set_data(G_OBJECT(win->window), "PidginWindow", win);

	win->actions = g_simple_action_group_new();
	g_action_map_add_action_entries(G_ACTION_MAP(win->actions), window_actions,
	                                G_N_ELEMENTS(window_actions), win);
	{
		int i;

		for (i = 0; conv_action_names[i] != NULL; i++) {
			GSimpleAction *a = g_simple_action_new(conv_action_names[i], NULL);

			g_signal_connect(a, "activate", G_CALLBACK(conv_action_cb), win);
			g_action_map_add_action(G_ACTION_MAP(win->actions), G_ACTION(a));
			g_object_unref(a);
		}
	}
	gtk_widget_insert_action_group(win->window, "conv", G_ACTION_GROUP(win->actions));
	{
		GSimpleActionGroup *tab = g_simple_action_group_new();

		g_action_map_add_action_entries(G_ACTION_MAP(tab), tab_actions,
		                                G_N_ELEMENTS(tab_actions), win);
		gtk_widget_insert_action_group(win->window, "tab", G_ACTION_GROUP(tab));
		g_object_unref(tab);
	}

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	model = build_menubar(win);
	win->menu.model = model;
	win->menu.menubar = gtk_popover_menu_bar_new_from_model(G_MENU_MODEL(model));
	gtk_box_append(GTK_BOX(vbox), win->menu.menubar);

	win->notebook = gtk_notebook_new();
	gtk_notebook_set_scrollable(GTK_NOTEBOOK(win->notebook), TRUE);
	gtk_notebook_set_show_border(GTK_NOTEBOOK(win->notebook), FALSE);
	gtk_notebook_set_group_name(GTK_NOTEBOOK(win->notebook), hidden ? NULL : NOTEBOOK_GROUP);
	gtk_widget_set_vexpand(win->notebook, TRUE);
	g_signal_connect(win->notebook, "page-added", G_CALLBACK(page_added_cb), win);
	g_signal_connect(win->notebook, "page-removed", G_CALLBACK(page_removed_cb), win);
	g_signal_connect(win->notebook, "page-reordered", G_CALLBACK(page_reordered_cb), win);
	g_signal_connect_after(win->notebook, "switch-page", G_CALLBACK(switch_page_cb), win);
	g_signal_connect(win->notebook, "create-window", G_CALLBACK(create_window_cb), win);
	gtk_box_append(GTK_BOX(vbox), win->notebook);
	gtk_window_set_child(GTK_WINDOW(win->window), vbox);
	update_tabs_visible(win);

	g_signal_connect(win->window, "close-request", G_CALLBACK(close_request_cb), win);
	g_signal_connect(win->window, "notify::is-active", G_CALLBACK(active_changed_cb), win);

	pidgin_conv_window_update_menu(win);
	return win;
}

PidginWindow *
pidgin_conv_window_new(void)
{
	PidginWindow *win = window_new(FALSE);

	window_list = g_list_append(window_list, win);
	return win;
}

void
pidgin_conv_window_destroy(PidginWindow *win)
{
	GList *l, *copy;

	if (win == NULL || win->closing)
		return;
	win->closing = TRUE;
	if (win->destroy_idle != 0) {
		g_source_remove(win->destroy_idle);
		win->destroy_idle = 0;
	}
	if (win != hidden_win)
		save_size(win);

	/* Any conversations still here go with it. */
	copy = g_list_copy(win->gtkconvs);
	for (l = copy; l != NULL; l = l->next)
		purple_conversation_destroy(((PidginConversation *)l->data)->active_conv);
	g_list_free(copy);

	window_list = g_list_remove(window_list, win);
	if (win == hidden_win)
		hidden_win = NULL;
	if (win->tab_menu != NULL)
		gtk_widget_unparent(win->tab_menu);
	gtk_window_destroy(GTK_WINDOW(win->window));
	g_clear_object(&win->menu.model);
	g_clear_object(&win->menu.more);
	g_clear_object(&win->more_actions);
	g_clear_object(&win->actions);
	g_list_free(win->gtkconvs);
	g_free(win);
}

void
pidgin_conv_window_show(PidginWindow *win)
{
	if (win != NULL && win != hidden_win)
		gtk_window_present(GTK_WINDOW(win->window));
}

void
pidgin_conv_window_hide(PidginWindow *win)
{
	if (win != NULL)
		gtk_widget_set_visible(win->window, FALSE);
}

void
pidgin_conv_window_raise(PidginWindow *win)
{
	pidgin_conv_window_show(win);
}

void
pidgin_conv_window_switch_gtkconv(PidginWindow *win, PidginConversation *gtkconv)
{
	int page = gtk_notebook_page_num(GTK_NOTEBOOK(win->notebook), gtkconv->tab_cont);

	if (page >= 0)
		gtk_notebook_set_current_page(GTK_NOTEBOOK(win->notebook), page);
}

void
pidgin_conv_window_add_gtkconv(PidginWindow *win, PidginConversation *gtkconv)
{
	GtkNotebook *nb;

	g_return_if_fail(win != NULL && gtkconv != NULL);
	nb = GTK_NOTEBOOK(win->notebook);

	if (g_object_get_data(G_OBJECT(gtkconv->tabby), "pidgin4-tab-menu") == NULL) {
		GtkGesture *click = gtk_gesture_click_new();

		gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
		g_signal_connect(click, "pressed", G_CALLBACK(tab_pressed_cb), gtkconv);
		gtk_widget_add_controller(gtkconv->tabby, GTK_EVENT_CONTROLLER(click));
		g_object_set_data(G_OBJECT(gtkconv->tabby), "pidgin4-tab-menu", GINT_TO_POINTER(1));
	}

	gtk_notebook_append_page(nb, gtkconv->tab_cont, gtkconv->tabby);
	gtk_notebook_set_menu_label_text(nb, gtkconv->tab_cont,
	                                 purple_conversation_get_title(gtkconv->active_conv));
	/* (The first page becomes current with a switch-page of its own.) */
}

void
pidgin_conv_window_remove_gtkconv(PidginWindow *win, PidginConversation *gtkconv)
{
	int page;

	g_return_if_fail(win != NULL && gtkconv != NULL);
	page = gtk_notebook_page_num(GTK_NOTEBOOK(win->notebook), gtkconv->tab_cont);
	if (page >= 0)
		gtk_notebook_remove_page(GTK_NOTEBOOK(win->notebook), page);
	win->gtkconvs = g_list_remove(win->gtkconvs, gtkconv);
	if (gtkconv->win == win)
		pidgin_conv_set_window(gtkconv, NULL);
}

/**************************************************************************
 * Placement (pidgin/gtkconv.c)
 **************************************************************************/

typedef struct
{
	char *id;
	char *name;
	PidginConvPlacementFunc fnc;
} ConvPlacementData;

static GList *conv_placement_fncs = NULL;
static PidginConvPlacementFunc place_conv = NULL;

static void
place_in(PidginWindow *win, PidginConversation *gtkconv)
{
	if (win == NULL)
		win = pidgin_conv_window_new();
	pidgin_conv_window_add_gtkconv(win, gtkconv);
	if (!gtk_widget_get_visible(win->window))
		pidgin_conv_window_show(win);
}

static void
conv_placement_last_created_win(PidginConversation *gtkconv)
{
	GList *last = g_list_last(window_list);

	place_in(last ? last->data : NULL, gtkconv);
}

static void
conv_placement_last_created_win_type(PidginConversation *gtkconv)
{
	place_in(pidgin_conv_window_last_with_type(
		purple_conversation_get_type(gtkconv->active_conv)), gtkconv);
}

static void
conv_placement_new_window(PidginConversation *gtkconv)
{
	place_in(NULL, gtkconv);
}

static PurpleGroup *
conv_get_group(PidginConversation *gtkconv)
{
	PurpleConversation *conv = gtkconv->active_conv;
	PurpleAccount *account = purple_conversation_get_account(conv);

	if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_IM) {
		PurpleBuddy *buddy = purple_find_buddy(account, purple_conversation_get_name(conv));
		return buddy ? purple_buddy_get_group(buddy) : NULL;
	} else {
		PurpleChat *chat = purple_blist_find_chat(account, purple_conversation_get_name(conv));
		return chat ? purple_chat_get_group(chat) : NULL;
	}
}

static void
conv_placement_by_group(PidginConversation *gtkconv)
{
	PurpleGroup *group = conv_get_group(gtkconv);
	GList *wl, *cl;

	if (group == NULL) {
		conv_placement_new_window(gtkconv);
		return;
	}
	for (wl = window_list; wl != NULL; wl = wl->next)
		for (cl = ((PidginWindow *)wl->data)->gtkconvs; cl != NULL; cl = cl->next)
			if (conv_get_group(cl->data) == group) {
				place_in(wl->data, gtkconv);
				return;
			}
	conv_placement_new_window(gtkconv);
}

static void
conv_placement_by_account(PidginConversation *gtkconv)
{
	PurpleAccount *account = purple_conversation_get_account(gtkconv->active_conv);
	GList *wl, *cl;

	for (wl = window_list; wl != NULL; wl = wl->next)
		for (cl = ((PidginWindow *)wl->data)->gtkconvs; cl != NULL; cl = cl->next)
			if (purple_conversation_get_account(
			        ((PidginConversation *)cl->data)->active_conv) == account) {
				place_in(wl->data, gtkconv);
				return;
			}
	conv_placement_new_window(gtkconv);
}

static ConvPlacementData *
get_conv_placement_data(const char *id)
{
	GList *n;

	for (n = conv_placement_fncs; n != NULL; n = n->next)
		if (purple_strequal(((ConvPlacementData *)n->data)->id, id))
			return n->data;
	return NULL;
}

static void
add_conv_placement_fnc(const char *id, const char *name, PidginConvPlacementFunc fnc)
{
	ConvPlacementData *data = g_new(ConvPlacementData, 1);

	data->id = g_strdup(id);
	data->name = g_strdup(name);
	data->fnc = fnc;
	conv_placement_fncs = g_list_append(conv_placement_fncs, data);
}

static void
ensure_default_funcs(void)
{
	if (conv_placement_fncs != NULL)
		return;
	add_conv_placement_fnc("last", _("Last created window"), conv_placement_last_created_win);
	add_conv_placement_fnc("im_chat", _("Separate IM and Chat windows"),
	                       conv_placement_last_created_win_type);
	add_conv_placement_fnc("new", _("New window"), conv_placement_new_window);
	add_conv_placement_fnc("group", _("By group"), conv_placement_by_group);
	add_conv_placement_fnc("account", _("By account"), conv_placement_by_account);
}

GList *
pidgin_conv_placement_get_options(void)
{
	GList *n, *list = NULL;

	ensure_default_funcs();
	for (n = conv_placement_fncs; n != NULL; n = n->next) {
		ConvPlacementData *data = n->data;
		list = g_list_append(list, data->name);
		list = g_list_append(list, data->id);
	}
	return list;
}

void
pidgin_conv_placement_add_fnc(const char *id, const char *name, PidginConvPlacementFunc fnc)
{
	g_return_if_fail(id != NULL && name != NULL && fnc != NULL);
	ensure_default_funcs();
	add_conv_placement_fnc(id, name, fnc);
}

void
pidgin_conv_placement_remove_fnc(const char *id)
{
	ConvPlacementData *data = get_conv_placement_data(id);

	if (data == NULL)
		return;
	conv_placement_fncs = g_list_remove(conv_placement_fncs, data);
	g_free(data->id);
	g_free(data->name);
	g_free(data);
}

const char *
pidgin_conv_placement_get_name(const char *id)
{
	ConvPlacementData *data;

	ensure_default_funcs();
	data = get_conv_placement_data(id);
	return data ? data->name : NULL;
}

PidginConvPlacementFunc
pidgin_conv_placement_get_fnc(const char *id)
{
	ConvPlacementData *data;

	ensure_default_funcs();
	data = get_conv_placement_data(id);
	return data ? data->fnc : NULL;
}

void
pidgin_conv_placement_set_current_func(PidginConvPlacementFunc func)
{
	g_return_if_fail(func != NULL);
	place_conv = func;
}

PidginConvPlacementFunc
pidgin_conv_placement_get_current_func(void)
{
	return place_conv;
}

void
pidgin_conv_placement_place(PidginConversation *gtkconv)
{
	/* No tabs: every conversation gets its own window. */
	if (!purple_prefs_get_bool(CONV_PREFS "/tabs"))
		conv_placement_new_window(gtkconv);
	else if (place_conv != NULL)
		place_conv(gtkconv);
	else
		conv_placement_last_created_win(gtkconv);
}

static void
placement_pref_cb(const char *name, PurplePrefType type, gconstpointer value, gpointer data)
{
	PidginConvPlacementFunc func = pidgin_conv_placement_get_fnc(value);

	if (func != NULL)
		pidgin_conv_placement_set_current_func(func);
}

/**************************************************************************
 * Setup
 **************************************************************************/

static int handle;

void
pidgin_conv_windows_init(void)
{
	GtkApplication *app = pidgin_application_get();
	static const struct { const char *action; const char *accels[3]; } accels[] = {
		{ "conv.new-im", { "<Control>m", NULL } },
		{ "conv.find", { "<Control>f", NULL } },
		{ "conv.clear", { "<Control>l", NULL } },
		{ "conv.get-info", { "<Control>o", NULL } },
		{ "conv.close", { "<Control>w", NULL } },
		{ "conv.next-unread", { "<Control>Tab", NULL } },
		{ "conv.prev-unread", { "<Control><Shift>Tab", "<Control><Shift>ISO_Left_Tab", NULL } },
		{ "conv.next-tab", { "<Control>Page_Down", "<Control>bracketright", NULL } },
		{ "conv.prev-tab", { "<Control>Page_Up", "<Control>bracketleft", NULL } },
		{ "conv.move-tab-left", { "<Control>comma", NULL } },
		{ "conv.move-tab-right", { "<Control>period", NULL } },
	};
	gsize i;

	ensure_default_funcs();
	purple_prefs_connect_callback(&handle, CONV_PREFS "/placement", placement_pref_cb, NULL);
	purple_prefs_trigger_callback(CONV_PREFS "/placement");
	purple_prefs_connect_callback(&handle, CONV_PREFS "/tab_side", tab_side_pref_cb, NULL);

	if (app != NULL) {
		for (i = 0; i < G_N_ELEMENTS(accels); i++)
			gtk_application_set_accels_for_action(app, accels[i].action, accels[i].accels);
		for (i = 1; i <= 9; i++) {
			char *action = g_strdup_printf("conv.tab(%" G_GSIZE_FORMAT ")", i);
			char *accel = g_strdup_printf("<Alt>%" G_GSIZE_FORMAT, i);
			const char *list[] = { accel, NULL };

			gtk_application_set_accels_for_action(app, action, list);
			g_free(action);
			g_free(accel);
		}
	}

	hidden_win = window_new(TRUE);
}

void
pidgin_conv_windows_uninit(void)
{
	purple_prefs_disconnect_by_handle(&handle);
	while (window_list != NULL)
		pidgin_conv_window_destroy(window_list->data);
	if (hidden_win != NULL)
		pidgin_conv_window_destroy(hidden_win);
	while (conv_placement_fncs != NULL)
		pidgin_conv_placement_remove_fnc(((ConvPlacementData *)conv_placement_fncs->data)->id);
}
