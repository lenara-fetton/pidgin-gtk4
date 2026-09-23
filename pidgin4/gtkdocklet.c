/*
 * System tray icon (aka docklet) plugin for Purple
 *
 * Copyright (C) 2002-3 Robert McQueen <robot101@debian.org>
 * Copyright (C) 2003 Herman Bloggs <hermanator12002@yahoo.com>
 * Inspired by a similar plugin by:
 *  John (J5) Palmieri <johnp@martianrock.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02111-1301, USA.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "core.h"
#include "conversation.h"
#include "debug.h"
#include "prefs.h"
#include "savedstatuses.h"
#include "signals.h"
#include "sound.h"

#include "gtkblist.h"
#include "gtkdialogs.h"
#include "gtkdocklet.h"
#include "pidginmenu.h"
#include "stubs.h"

/*
 * The logic of pidgin/gtkdocklet.c. What changed:
 *   - the menu is a GMenuModel + GSimpleActionGroup ("tray." prefix) that
 *     the SNI exports as dbusmenu, rebuilt when the state changes and when
 *     the host is about to show it, instead of a GtkMenu built per click;
 *   - blinking is replaced by the attention status (see gtkdocklet-sni.c);
 *     the "Blink on New Message" item is gone (/pidgin/docklet/blink is
 *     still registered so a shared profile keeps it);
 *   - the buddy list "visibility manager" is pidgin_docklet_is_embedded(),
 *     which gtkblist.c asks when its window is closed.
 */

#ifndef DOCKLET_TOOLTIP_LINE_LIMIT
#define DOCKLET_TOOLTIP_LINE_LIMIT 5
#endif

#define DOCKLET4_PREFS PIDGIN4_PREFS_ROOT "/docklet"
#define STATUS_ICON_RESOURCE "resource://" PIDGIN4_RESOURCE_PATH "/icons/16x16/status/pidgin-status-%s.png"

/* globals */
static struct docklet_ui_ops *ui_ops = NULL;
static PurpleStatusPrimitive status = PURPLE_STATUS_OFFLINE;
static gboolean pending = FALSE;
static gboolean connecting = FALSE;
static gboolean enable_join_chat = FALSE;
static gboolean visible = FALSE;
static gboolean start_hidden = FALSE;
static guint unread_total = 0;

static GMenu *menu = NULL;
static GSimpleActionGroup *actions = NULL;
static guint menu_idle = 0;

static GList *selftest_convs; /* see pidgin_docklet_selftest() */

static void rebuild_menu(void);

/**************************************************************************
 * docklet status and utility functions
 **************************************************************************/

/* /pidgin4/docklet/show when set, else the shared /pidgin/docklet/show. */
static const char *
docklet_show_pref(void)
{
	const char *show = purple_prefs_get_string(DOCKLET4_PREFS "/show");

	if (show == NULL || *show == '\0')
		show = purple_prefs_get_string(PIDGIN_PREFS_ROOT "/docklet/show");
	return show != NULL ? show : "always";
}

static GList *
get_pending_list(guint max)
{
	GList *l_im, *l_chat;

	l_im = pidgin_conversations_find_unseen_list(PURPLE_CONV_TYPE_IM,
						       PIDGIN_UNSEEN_TEXT,
						       FALSE, max);
	if (selftest_convs != NULL)
		l_im = g_list_concat(l_im, g_list_copy(selftest_convs));

	/* Short circuit if we have our information already */
	if (max == 1 && l_im != NULL)
		return l_im;

	l_chat = pidgin_conversations_find_unseen_list(PURPLE_CONV_TYPE_CHAT,
							 PIDGIN_UNSEEN_NICK,
							 FALSE, max);

	if (l_im != NULL && l_chat != NULL)
		return g_list_concat(l_im, l_chat);
	else if (l_im != NULL)
		return l_im;
	else
		return l_chat;
}

static guint
conv_unseen_count(PurpleConversation *conv)
{
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);
	guint count = gtkconv ? gtkconv->unseen_count : 0;

	/* Unseen state without a count still means something unread. */
	return count > 0 ? count : 1;
}

static void
schedule_menu_rebuild(void)
{
	if (menu_idle == 0)
		menu_idle = g_idle_add_once((GSourceOnceFunc)rebuild_menu, NULL);
}

static gboolean
docklet_update_status(void)
{
	GList *convs, *all, *l;
	int count;
	PurpleSavedStatus *saved_status;
	PurpleStatusPrimitive newstatus = PURPLE_STATUS_OFFLINE;
	gboolean newpending = FALSE, newconnecting = FALSE;

	/* get the current savedstatus */
	saved_status = purple_savedstatus_get_current();

	/* determine if any ims have unseen messages */
	convs = get_pending_list(DOCKLET_TOOLTIP_LINE_LIMIT);

	if (purple_strequal(docklet_show_pref(), "pending")) {
		if (convs && ui_ops && ui_ops->create && !visible) {
			g_list_free(convs);
			ui_ops->create();
			return FALSE;
		} else if (!convs && ui_ops && ui_ops->destroy && visible) {
			g_list_free(convs);
			ui_ops->destroy();
			return FALSE;
		}
	}

	/* The total, for the SNI title and the Unread Messages menu. */
	all = get_pending_list(0);
	unread_total = 0;
	for (l = all; l != NULL; l = l->next)
		unread_total += conv_unseen_count(l->data);
	g_list_free(all);

	if (!visible) {
		g_list_free(convs);
		return FALSE;
	}

	if (convs != NULL) {
		newpending = TRUE;

		/* set tooltip if messages are pending */
		if (ui_ops->set_tooltip) {
			GString *tooltip_text = g_string_new("");
			for (l = convs, count = 0 ; l != NULL ; l = l->next, count++) {
				PurpleConversation *conv = (PurpleConversation *)l->data;
				guint unseen = conv_unseen_count(conv);

				if (count == DOCKLET_TOOLTIP_LINE_LIMIT - 1) {
					g_string_append(tooltip_text, _("Right-click for more unread messages...\n"));
				} else {
					g_string_append_printf(tooltip_text,
						ngettext("%d unread message from %s\n", "%d unread messages from %s\n", unseen),
						unseen, purple_conversation_get_title(conv));
				}
			}

			/* get rid of the last newline */
			if (tooltip_text->len > 0)
				tooltip_text = g_string_truncate(tooltip_text, tooltip_text->len - 1);

			ui_ops->set_tooltip(tooltip_text->str);

			g_string_free(tooltip_text, TRUE);
		}

		g_list_free(convs);

	} else if (ui_ops->set_tooltip) {
		char *tooltip_text = g_strconcat(PIDGIN_NAME, " - ",
			purple_savedstatus_get_title(saved_status), NULL);
		ui_ops->set_tooltip(tooltip_text);
		g_free(tooltip_text);
	}

	for(l = purple_accounts_get_all(); l != NULL; l = l->next) {

		PurpleAccount *account = (PurpleAccount*)l->data;

		if (!purple_account_get_enabled(account, PIDGIN_UI))
			continue;

		if (purple_account_is_disconnected(account))
			continue;

		if (purple_account_is_connecting(account))
			newconnecting = TRUE;
	}

	newstatus = purple_savedstatus_get_type(saved_status);

	/* update the icon if we changed status */
	if (status != newstatus || pending!=newpending || connecting!=newconnecting) {
		status = newstatus;
		pending = newpending;
		connecting = newconnecting;

		pidgin_docklet_update_icon();
	}

	/* The menu shows the status, the unread list and what is enabled. */
	schedule_menu_rebuild();

	return FALSE; /* for when we're called by the glib idle handler */
}

static gboolean
online_account_supports_chat(void)
{
	GList *c = NULL;
	c = purple_connections_get_all();

	while(c != NULL) {
		PurpleConnection *gc = c->data;
		PurplePluginProtocolInfo *prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(gc->prpl);
		if (prpl_info != NULL && prpl_info->chat_info != NULL)
			return TRUE;
		c = c->next;
	}

	return FALSE;
}

/**************************************************************************
 * callbacks and signal handlers
 **************************************************************************/

static void
docklet_update_status_cb(void *data)
{
	docklet_update_status();
}

static void
docklet_conv_updated_cb(PurpleConversation *conv, PurpleConvUpdateType type)
{
	if (type == PURPLE_CONV_UPDATE_UNSEEN)
		docklet_update_status();
}

static void
docklet_signed_on_cb(PurpleConnection *gc)
{
	if (!enable_join_chat) {
		if (PURPLE_PLUGIN_PROTOCOL_INFO(gc->prpl)->chat_info != NULL)
			enable_join_chat = TRUE;
	}
	docklet_update_status();
}

static void
docklet_signed_off_cb(PurpleConnection *gc)
{
	if (enable_join_chat) {
		if (PURPLE_PLUGIN_PROTOCOL_INFO(gc->prpl)->chat_info != NULL)
			enable_join_chat = online_account_supports_chat();
	}
	docklet_update_status();
}

static void
docklet_show_pref_changed_cb(const char *name, PurplePrefType type,
			     gconstpointer value, gpointer data)
{
	const char *val = docklet_show_pref();

	if (ui_ops == NULL)
		return;

	if (purple_strequal(val, "always")) {
		if (ui_ops->create && !visible)
			ui_ops->create();
	} else if (purple_strequal(val, "never")) {
		if (visible && ui_ops->destroy)
			ui_ops->destroy();
	} else {
		docklet_update_status();
	}
}

static void
docklet_plugins_changed_cb(PurplePlugin *plugin, gpointer data)
{
	schedule_menu_rebuild();
}

static void
docklet_mute_changed_cb(const char *name, PurplePrefType type,
                        gconstpointer value, gpointer data)
{
	GAction *action = g_action_map_lookup_action(G_ACTION_MAP(actions), "mute");

	if (action != NULL)
		g_simple_action_set_state(G_SIMPLE_ACTION(action),
			g_variant_new_boolean(GPOINTER_TO_INT(value)));
}

/**************************************************************************
 * docklet menu actions
 **************************************************************************/

static PurpleSavedStatus *
create_transient_status(PurpleStatusPrimitive primitive, PurpleStatusType *status_type)
{
	PurpleSavedStatus *saved_status = purple_savedstatus_new(NULL, primitive);

	if(status_type != NULL) {
		GList *tmp, *active_accts = purple_accounts_get_all_active();
		for (tmp = active_accts; tmp != NULL; tmp = tmp->next) {
			purple_savedstatus_set_substatus(saved_status,
				(PurpleAccount*) tmp->data, status_type, NULL);
		}
		g_list_free(active_accts);
	}

	return saved_status;
}

static void
activate_status_primitive_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	PurpleStatusPrimitive primitive;
	PurpleSavedStatus *saved_status;

	primitive = g_variant_get_int32(param);

	/* Try to lookup an already existing transient saved status */
	saved_status = purple_savedstatus_find_transient_by_type_and_message(primitive, NULL);

	/* Create a new transient saved status if we weren't able to find one */
	if (saved_status == NULL)
		saved_status = create_transient_status(primitive, NULL);

	/* Set the status for each account */
	purple_savedstatus_activate(saved_status);
}

static void
activate_saved_status_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	time_t creation_time;
	PurpleSavedStatus *saved_status;

	creation_time = (time_t)g_variant_get_int64(param);
	saved_status = purple_savedstatus_find_by_creation_time(creation_time);
	if (saved_status != NULL)
		purple_savedstatus_activate(saved_status);
}

static void
show_custom_status_editor_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	PurpleSavedStatus *saved_status;
	saved_status = purple_savedstatus_get_current();

	if (purple_savedstatus_get_type(saved_status) == PURPLE_STATUS_AVAILABLE)
		saved_status = purple_savedstatus_new(NULL, PURPLE_STATUS_AWAY);

	pidgin_status_editor_show(FALSE,
		purple_savedstatus_is_transient(saved_status) ? saved_status : NULL);
}

static void
saved_statuses_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	pidgin_status_window_show();
}

static void
toggle_blist_cb(GSimpleAction *action, GVariant *value, gpointer data)
{
	purple_blist_set_visible(g_variant_get_boolean(value));
	g_simple_action_set_state(action, value);
}

static void
toggle_mute_cb(GSimpleAction *action, GVariant *value, gpointer data)
{
	purple_prefs_set_bool(PIDGIN_PREFS_ROOT "/sound/mute", g_variant_get_boolean(value));
	g_simple_action_set_state(action, value);
}

/* Runs a buddy list window action (win.*), so the tray and the menubar do
 * the same thing, including TODO(M5) windows once they exist. */
static void
blist_window_action_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	GtkWidget *window = pidgin_blist_get_window();
	const char *name = data;

	if (window != NULL && g_action_group_has_action(G_ACTION_GROUP(window), name))
		g_action_group_activate_action(G_ACTION_GROUP(window), name, NULL);
	else if (purple_strequal(name, "new-im"))
		pidgin_dialogs_im();
	else if (purple_strequal(name, "join-chat"))
		pidgin_blist_joinchat_show();
}

static void
app_action_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	GApplication *app = G_APPLICATION(pidgin_application_get());

	if (app != NULL)
		g_action_group_activate_action(G_ACTION_GROUP(app), data, NULL);
}

static void
quit_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	pidgin_application_quit();
}

/* (ssis): protocol id, account name, conversation type, conversation name */
static void
present_conversation_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	const char *protocol, *username, *name;
	gint type;
	PurpleAccount *account;
	PurpleConversation *conv;

	g_variant_get(param, "(&s&si&s)", &protocol, &username, &type, &name);
	account = purple_accounts_find(username, protocol);
	if (account == NULL)
		return;
	conv = purple_find_conversation_with_account(type, name, account);
	if (conv != NULL)
		purple_conversation_present(conv);
}

static GVariant *
conversation_target(PurpleConversation *conv)
{
	PurpleAccount *account = purple_conversation_get_account(conv);

	return g_variant_new("(ssis)", purple_account_get_protocol_id(account),
		purple_account_get_username(account),
		(gint)purple_conversation_get_type(conv),
		purple_conversation_get_name(conv));
}

static const GActionEntry action_entries[] = {
	{ .name = "show-blist", .state = "true", .change_state = toggle_blist_cb },
	{ .name = "mute", .state = "false", .change_state = toggle_mute_cb },
	{ .name = "status-primitive", .parameter_type = "i", .state = "0",
	  .activate = activate_status_primitive_cb },
	{ .name = "saved-status", .parameter_type = "x",
	  .activate = activate_saved_status_cb },
	{ .name = "new-status", .activate = show_custom_status_editor_cb },
	{ .name = "saved-statuses", .activate = saved_statuses_cb },
	{ .name = "present-conversation", .parameter_type = "(ssis)",
	  .activate = present_conversation_cb },
	{ .name = "quit", .activate = quit_cb },
};

/* Actions that forward to a buddy list window action or an app action. */
static const struct {
	const char *name;
	const char *target;
	gboolean app;
} forward_actions[] = {
	{ "new-im", "new-im", FALSE },
	{ "join-chat", "join-chat", FALSE },
	{ "plugins", "plugins", FALSE },
	{ "preferences", "preferences", FALSE },
	{ "transfers", "transfers", FALSE },
	{ "accounts", "accounts", TRUE },
};

static void
create_actions(void)
{
	gsize i;

	actions = g_simple_action_group_new();
	g_action_map_add_action_entries(G_ACTION_MAP(actions), action_entries,
	                                G_N_ELEMENTS(action_entries), NULL);

	for (i = 0; i < G_N_ELEMENTS(forward_actions); i++) {
		GSimpleAction *action = g_simple_action_new(forward_actions[i].name, NULL);

		g_signal_connect(action, "activate",
			forward_actions[i].app ? G_CALLBACK(app_action_cb)
			                       : G_CALLBACK(blist_window_action_cb),
			(gpointer)forward_actions[i].target);
		g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(action));
		g_object_unref(action);
	}
}

static void
set_enabled(const char *name, gboolean enabled)
{
	GAction *action = g_action_map_lookup_action(G_ACTION_MAP(actions), name);

	if (action != NULL)
		g_simple_action_set_enabled(G_SIMPLE_ACTION(action), enabled);
}

static void
set_state(const char *name, GVariant *state)
{
	GAction *action = g_action_map_lookup_action(G_ACTION_MAP(actions), name);

	if (action != NULL)
		g_simple_action_set_state(G_SIMPLE_ACTION(action), state);
	else
		g_variant_unref(g_variant_ref_sink(state));
}

/**************************************************************************
 * docklet menu
 **************************************************************************/

static const char *
primitive_icon(PurpleStatusPrimitive primitive)
{
	switch (primitive) {
	case PURPLE_STATUS_AWAY:
		return "away";
	case PURPLE_STATUS_UNAVAILABLE:
		return "busy";
	case PURPLE_STATUS_EXTENDED_AWAY:
		return "extended-away";
	case PURPLE_STATUS_INVISIBLE:
		return "invisible";
	case PURPLE_STATUS_OFFLINE:
		return "offline";
	default:
		return "available";
	}
}

static void
set_status_icon(GMenuItem *item, PurpleStatusPrimitive primitive)
{
	char *uri = g_strdup_printf(STATUS_ICON_RESOURCE, primitive_icon(primitive));
	GFile *file = g_file_new_for_uri(uri);
	GIcon *icon = g_file_icon_new(file);

	g_menu_item_set_icon(item, icon);
	g_object_unref(icon);
	g_object_unref(file);
	g_free(uri);
}

static GMenu *
docklet_status_submenu(void)
{
	static const struct {
		const char *label;
		PurpleStatusPrimitive primitive;
	} primitives[] = {
		{ N_("Available"), PURPLE_STATUS_AVAILABLE },
		{ N_("Away"), PURPLE_STATUS_AWAY },
		{ N_("Do not disturb"), PURPLE_STATUS_UNAVAILABLE },
		{ N_("Invisible"), PURPLE_STATUS_INVISIBLE },
		{ N_("Offline"), PURPLE_STATUS_OFFLINE },
	};
	GMenu *submenu = g_menu_new();
	GMenu *section;
	GList *popular_statuses, *cur;
	gsize i;

	/* The primitives are radio items: the action's state is the current
	 * primitive. (Pidgin 2's per-account statuses of the status box's
	 * account are left out: the pidgin4 status box has no per-account
	 * mode.) */
	section = g_menu_new();
	for (i = 0; i < G_N_ELEMENTS(primitives); i++) {
		GMenuItem *item = g_menu_item_new(_(primitives[i].label), NULL);

		g_menu_item_set_action_and_target_value(item, "tray.status-primitive",
			g_variant_new_int32(primitives[i].primitive));
		set_status_icon(item, primitives[i].primitive);
		g_menu_append_item(section, item);
		g_object_unref(item);
	}
	g_menu_append_section(submenu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);

	popular_statuses = purple_savedstatuses_get_popular(6);
	if (popular_statuses != NULL) {
		section = g_menu_new();
		for (cur = popular_statuses; cur != NULL; cur = cur->next) {
			PurpleSavedStatus *saved_status = cur->data;
			char *label = pidgin_menu_escape_label(purple_savedstatus_get_title(saved_status));
			GMenuItem *item = g_menu_item_new(label, NULL);

			g_menu_item_set_action_and_target_value(item, "tray.saved-status",
				g_variant_new_int64(purple_savedstatus_get_creation_time(saved_status)));
			set_status_icon(item, purple_savedstatus_get_type(saved_status));
			g_menu_append_item(section, item);
			g_object_unref(item);
			g_free(label);
		}
		g_menu_append_section(submenu, NULL, G_MENU_MODEL(section));
		g_object_unref(section);
	}
	g_list_free(popular_statuses);

	section = g_menu_new();
	g_menu_append(section, _("New..."), "tray.new-status");
	g_menu_append(section, _("Saved..."), "tray.saved-statuses");
	g_menu_append_section(submenu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);

	return submenu;
}

static GMenu *
docklet_unread_submenu(void)
{
	GMenu *submenu;
	GList *convs, *l;

	convs = get_pending_list(0);
	if (convs == NULL)
		return NULL;

	submenu = g_menu_new();
	for (l = convs; l != NULL; l = l->next) {
		PurpleConversation *conv = l->data;
		char *title = pidgin_menu_escape_label(purple_conversation_get_title(conv));
		char *label = g_strdup_printf("%s (%u)", title, conv_unseen_count(conv));
		GMenuItem *item = g_menu_item_new(label, NULL);

		g_menu_item_set_action_and_target_value(item, "tray.present-conversation",
			conversation_target(conv));
		g_menu_append_item(submenu, item);
		g_object_unref(item);
		g_free(label);
		g_free(title);
	}
	g_list_free(convs);

	return submenu;
}

static void
remove_plugin_actions(void)
{
	char **names = g_action_group_list_actions(G_ACTION_GROUP(actions));
	char **name;

	for (name = names; *name != NULL; name++)
		if (g_str_has_prefix(*name, "plugin-action-"))
			g_action_map_remove_action(G_ACTION_MAP(actions), *name);
	g_strfreev(names);
}

static GMenu *
docklet_plugin_actions(void)
{
	GMenu *section = NULL;
	GList *l;

	/* Add a submenu for each plugin with custom actions */
	for (l = purple_plugins_get_loaded(); l; l = l->next) {
		PurplePlugin *plugin = (PurplePlugin *) l->data;
		GMenu *submenu;
		char *label;

		if (PURPLE_IS_PROTOCOL_PLUGIN(plugin))
			continue;

		if (!PURPLE_PLUGIN_HAS_ACTIONS(plugin))
			continue;

		submenu = pidgin_menu_from_plugin_actions(plugin, NULL, actions, "tray");
		if (submenu == NULL)
			continue;

		if (section == NULL)
			section = g_menu_new();
		label = pidgin_menu_escape_label(_(plugin->info->name));
		g_menu_append_submenu(section, label, G_MENU_MODEL(submenu));
		g_free(label);
		g_object_unref(submenu);
	}

	return section;
}

static void
update_action_states(void)
{
	GtkWidget *window = pidgin_blist_get_window();
	gboolean online = (status != PURPLE_STATUS_OFFLINE);

	set_state("show-blist", g_variant_new_boolean(window != NULL &&
		gtk_widget_get_visible(window)));
	set_state("mute", g_variant_new_boolean(
		purple_prefs_get_bool(PIDGIN_PREFS_ROOT "/sound/mute")));
	set_state("status-primitive", g_variant_new_int32(
		purple_savedstatus_get_type(purple_savedstatus_get_current())));
	set_enabled("mute", !purple_strequal(
		purple_prefs_get_string(PIDGIN4_PREFS_ROOT "/sound/method"), "none"));
	set_enabled("new-im", online);
	set_enabled("join-chat", online && enable_join_chat);
}

static void
rebuild_menu(void)
{
	GMenu *section, *submenu;

	if (menu_idle != 0) {
		g_source_remove(menu_idle);
		menu_idle = 0;
	}
	if (menu == NULL)
		return;

	update_action_states();
	remove_plugin_actions();
	g_menu_remove_all(menu);

	section = g_menu_new();
	g_menu_append(section, _("Show Buddy _List"), "tray.show-blist");
	submenu = docklet_unread_submenu();
	if (submenu != NULL) {
		g_menu_append_submenu(section, _("_Unread Messages"), G_MENU_MODEL(submenu));
		g_object_unref(submenu);
	} else {
		/* No action: shown disabled, as in Pidgin 2. */
		g_menu_append(section, _("_Unread Messages"), NULL);
	}
	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);

	section = g_menu_new();
	g_menu_append(section, _("New _Message..."), "tray.new-im");
	g_menu_append(section, _("Join Chat..."), "tray.join-chat");
	submenu = docklet_status_submenu();
	g_menu_append_submenu(section, _("_Change Status"), G_MENU_MODEL(submenu));
	g_object_unref(submenu);
	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);

	section = g_menu_new();
	g_menu_append(section, _("_Accounts"), "tray.accounts");
	g_menu_append(section, _("Plu_gins"), "tray.plugins");
	g_menu_append(section, _("Pr_eferences"), "tray.preferences");
	g_menu_append(section, _("File _Transfers"), "tray.transfers");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);

	section = g_menu_new();
	g_menu_append(section, _("Mute _Sounds"), "tray.mute");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);

	/* add plugin actions */
	section = docklet_plugin_actions();
	if (section != NULL) {
		g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
		g_object_unref(section);
	}

	section = g_menu_new();
	g_menu_append(section, _("_Quit"), "tray.quit");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);
}

static void
docklet_menu(void)
{
	/* Pidgin 2 popped up a GtkMenu here. The tray host shows the exported
	 * menu itself; a host that calls ContextMenu instead (none known) gets
	 * a fresh menu for its next request. */
	purple_debug_info("docklet", "context menu requested; the host shows "
	                  "the exported menu\n");
	rebuild_menu();
}

/**************************************************************************
 * public api for ui_ops
 **************************************************************************/
void
pidgin_docklet_update_icon()
{
	if (ui_ops && ui_ops->update_icon)
		ui_ops->update_icon(status, connecting, pending);
}

gboolean
pidgin_docklet_present_pending(void)
{
	GList *l = get_pending_list(1);

	if (l == NULL)
		return FALSE;
	purple_conversation_present((PurpleConversation *)l->data);
	g_list_free(l);
	return TRUE;
}

void
pidgin_docklet_clicked(int button_type)
{
	purple_debug_info("docklet", "clicked (button %d, pending %d)\n",
	                  button_type, pending);
	switch (button_type) {
		case 1:
			if (!pending || !pidgin_docklet_present_pending())
				pidgin_blist_toggle_visibility();
			break;
		case 3:
			docklet_menu();
			break;
	}
}

void
pidgin_docklet_embedded()
{
	visible = TRUE;
	docklet_update_status();
	pidgin_docklet_update_icon();
	rebuild_menu();

	/* The list was hidden in the tray at the last quit: keep it there. */
	if (start_hidden && pidgin_docklet_is_embedded()) {
		purple_debug_info("docklet", "tray icon embedded; the buddy list "
		                  "starts hidden\n");
		purple_blist_set_visible(FALSE);
	}
	start_hidden = FALSE;
}

void
pidgin_docklet_remove()
{
	if (visible) {
		GtkWidget *window = pidgin_blist_get_window();

		visible = FALSE;
		status = PURPLE_STATUS_OFFLINE;

		/* Pidgin 2's visibility manager: without a tray the buddy list
		 * must not stay hidden. */
		if (window != NULL && !gtk_widget_get_visible(window) &&
		    !purple_prefs_get_bool(PIDGIN4_PREFS_ROOT "/blist/close_hides")) {
			purple_debug_info("docklet", "tray icon gone; showing the "
			                  "buddy list\n");
			purple_blist_set_visible(TRUE);
		}
	}
	start_hidden = FALSE;
}

gboolean
pidgin_docklet_is_embedded(void)
{
	return visible && purple_strequal(docklet_show_pref(), "always");
}

gboolean
pidgin_docklet_start_hidden(void)
{
	return start_hidden;
}

void
pidgin_docklet_set_ui_ops(struct docklet_ui_ops *ops)
{
	ui_ops = ops;
}

void*
pidgin_docklet_get_handle()
{
	static int i;
	return &i;
}

GMenuModel *
pidgin_docklet_get_menu(void)
{
	return G_MENU_MODEL(menu);
}

GActionGroup *
pidgin_docklet_get_actions(void)
{
	return G_ACTION_GROUP(actions);
}

void
pidgin_docklet_refresh_menu(void)
{
	rebuild_menu();
}

guint
pidgin_docklet_get_unread_count(void)
{
	return unread_total;
}

static void
blist_visibility_cb(PurpleBuddyList *list, gpointer data)
{
	schedule_menu_rebuild();
}

/* Is a StatusNotifierWatcher (a tray host) on the session bus right now?
 * Only asked once at startup, to decide whether the buddy list may start
 * hidden; the tray itself follows the watcher with g_bus_watch_name. */
static gboolean
watcher_running(void)
{
	GDBusConnection *connection;
	GVariant *reply;
	gboolean owned = FALSE;

	connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
	if (connection == NULL)
		return FALSE;

	reply = g_dbus_connection_call_sync(connection, "org.freedesktop.DBus",
		"/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
		g_variant_new("(s)", "org.kde.StatusNotifierWatcher"),
		G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
	if (reply != NULL) {
		g_variant_get(reply, "(b)", &owned);
		g_variant_unref(reply);
	}
	g_object_unref(connection);
	return owned;
}

/* The tray did not come up after all: show the buddy list. */
static void
start_hidden_timeout_cb(gpointer data)
{
	if (!start_hidden)
		return;

	start_hidden = FALSE;
	if (!pidgin_docklet_is_embedded()) {
		purple_debug_info("docklet", "no tray icon; showing the buddy list\n");
		purple_blist_set_visible(TRUE);
	}
}

void
pidgin_docklet_init()
{
	void *conn_handle = purple_connections_get_handle();
	void *conv_handle = purple_conversations_get_handle();
	void *accounts_handle = purple_accounts_get_handle();
	void *status_handle = purple_savedstatuses_get_handle();
	void *docklet_handle = pidgin_docklet_get_handle();

	/* Shared with Pidgin 2: the same keys, types and defaults. */
	purple_prefs_add_none(PIDGIN_PREFS_ROOT "/docklet");
	purple_prefs_add_bool(PIDGIN_PREFS_ROOT "/docklet/blink", FALSE);
	purple_prefs_add_string(PIDGIN_PREFS_ROOT "/docklet/show", "always");
	/* pidgin4's own: overrides the shared "show" when not empty. */
	purple_prefs_add_none(DOCKLET4_PREFS);
	purple_prefs_add_string(DOCKLET4_PREFS "/show", "");
	purple_prefs_connect_callback(docklet_handle, PIDGIN_PREFS_ROOT "/docklet/show",
				    docklet_show_pref_changed_cb, NULL);
	purple_prefs_connect_callback(docklet_handle, DOCKLET4_PREFS "/show",
				    docklet_show_pref_changed_cb, NULL);
	purple_prefs_connect_callback(docklet_handle, PIDGIN_PREFS_ROOT "/sound/mute",
				    docklet_mute_changed_cb, NULL);

	/* Read before the buddy list is shown (which sets it TRUE): the list
	 * was hidden in the tray when pidgin4 last quit. Only honoured if a
	 * tray host accepts the icon (pidgin_docklet_embedded()). */
	start_hidden = purple_prefs_exists(PIDGIN4_PREFS_ROOT "/blist/list_visible") &&
	               !purple_prefs_get_bool(PIDGIN4_PREFS_ROOT "/blist/list_visible") &&
	               purple_strequal(docklet_show_pref(), "always") &&
	               watcher_running();
	if (start_hidden)
		g_timeout_add_seconds_once(5, start_hidden_timeout_cb, NULL);

	create_actions();
	menu = g_menu_new();

	docklet_ui_init();
	if (purple_strequal(docklet_show_pref(), "always") && ui_ops && ui_ops->create)
		ui_ops->create();

	purple_signal_connect(conn_handle, "signed-on",
			    docklet_handle, PURPLE_CALLBACK(docklet_signed_on_cb), NULL);
	purple_signal_connect(conn_handle, "signed-off",
			    docklet_handle, PURPLE_CALLBACK(docklet_signed_off_cb), NULL);
	purple_signal_connect(accounts_handle, "account-connecting",
			    docklet_handle, PURPLE_CALLBACK(docklet_update_status_cb), NULL);
	purple_signal_connect(conv_handle, "received-im-msg",
			    docklet_handle, PURPLE_CALLBACK(docklet_update_status_cb), NULL);
	purple_signal_connect(conv_handle, "conversation-created",
			    docklet_handle, PURPLE_CALLBACK(docklet_update_status_cb), NULL);
	purple_signal_connect(conv_handle, "deleting-conversation",
			    docklet_handle, PURPLE_CALLBACK(docklet_update_status_cb), NULL);
	purple_signal_connect(conv_handle, "conversation-updated",
			    docklet_handle, PURPLE_CALLBACK(docklet_conv_updated_cb), NULL);
	purple_signal_connect(status_handle, "savedstatus-changed",
			    docklet_handle, PURPLE_CALLBACK(docklet_update_status_cb), NULL);
	purple_signal_connect(purple_plugins_get_handle(), "plugin-load",
			    docklet_handle, PURPLE_CALLBACK(docklet_plugins_changed_cb), NULL);
	purple_signal_connect(purple_plugins_get_handle(), "plugin-unload",
			    docklet_handle, PURPLE_CALLBACK(docklet_plugins_changed_cb), NULL);
	purple_signal_connect(pidgin_blist_get_handle(), "gtkblist-hiding",
			    docklet_handle, PURPLE_CALLBACK(blist_visibility_cb), NULL);
	purple_signal_connect(pidgin_blist_get_handle(), "gtkblist-unhiding",
			    docklet_handle, PURPLE_CALLBACK(blist_visibility_cb), NULL);
	purple_signal_connect(pidgin_blist_get_handle(), "gtkblist-created",
			    docklet_handle, PURPLE_CALLBACK(blist_visibility_cb), NULL);

	enable_join_chat = online_account_supports_chat();
	rebuild_menu();
}

void
pidgin_docklet_uninit()
{
	/* Quitting: the buddy list stays as it is (hidden in the tray, if it
	 * was), so /pidgin4/blist/list_visible is saved as the user left it. */
	visible = FALSE;
	start_hidden = FALSE;
	if (ui_ops && ui_ops->destroy)
		ui_ops->destroy();

	purple_signals_disconnect_by_handle(pidgin_docklet_get_handle());
	purple_prefs_disconnect_by_handle(pidgin_docklet_get_handle());
	if (menu_idle != 0) {
		g_source_remove(menu_idle);
		menu_idle = 0;
	}
	g_clear_object(&menu);
	g_clear_object(&actions);
}

/**************************************************************************
 * Selftest
 **************************************************************************/

/*
 * libpurple creates no conversation without a connection, and the
 * selftest runs with -n. So it builds a stand-alone PurpleConversation
 * (the struct is public in 2.x) that only the docklet sees: it is added to
 * the pending list here, never to libpurple's lists, and no signal is
 * emitted for it.
 */
static GList *selftest_convs = NULL;

static void
selftest_close_blist(void)
{
	GtkWidget *window = pidgin_blist_get_window();

	if (g_getenv("PIDGIN4_DOCKLET_CLOSE_BLIST") == NULL || window == NULL)
		return;

	/* Close the buddy list like the user would: it hides into the tray
	 * when there is one, else pidgin4 quits. */
	purple_debug_info("docklet", "selftest: closing the buddy list "
	                  "(embedded=%d)\n", pidgin_docklet_is_embedded());
	gtk_window_close(GTK_WINDOW(window));
	window = pidgin_blist_get_window();
	if (window != NULL)
		purple_debug_info("docklet", "selftest: buddy list %s\n",
			gtk_widget_get_visible(window) ? "still shown"
			                               : "hidden into the tray");
}

static gboolean
selftest_finish_cb(gpointer data)
{
	PurpleConversation *conv = data;

	selftest_convs = g_list_remove(selftest_convs, conv);
	docklet_update_status();
	rebuild_menu();
	purple_debug_info("docklet", "selftest: cleared, pending=%d unread=%u\n",
	                  pending, unread_total);

	g_hash_table_destroy(conv->data);
	g_free(conv->name);
	g_free(conv->title);
	g_free(conv);

	selftest_close_blist();
	return G_SOURCE_REMOVE;
}

void
pidgin_docklet_selftest(void)
{
	PurpleConversation *conv;
	GList *accounts;
	const char *seconds = g_getenv("PIDGIN4_DOCKLET_SELFTEST");

	if (seconds == NULL)
		return;

	accounts = purple_accounts_get_all();
	if (accounts == NULL) {
		purple_debug_warning("docklet", "selftest: no account\n");
		return;
	}

	conv = g_new0(PurpleConversation, 1);
	conv->type = PURPLE_CONV_TYPE_IM;
	conv->account = accounts->data;
	conv->name = g_strdup("docklet-selftest@example.invalid");
	conv->title = g_strdup("Docklet Selftest");
	conv->data = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	/* Marked the way gtkconv.c marks a conversation without a window. */
	purple_conversation_set_data(conv, "unseen-count", GINT_TO_POINTER(3));
	purple_conversation_set_data(conv, "unseen-state",
	                             GINT_TO_POINTER(PIDGIN_UNSEEN_TEXT));
	selftest_convs = g_list_append(selftest_convs, conv);

	docklet_update_status();
	rebuild_menu();

	purple_debug_info("docklet", "selftest: marked %s unread: pending=%d "
	                  "unread=%u embedded=%d, menu items=%d\n",
	                  purple_conversation_get_name(conv), pending, unread_total,
	                  pidgin_docklet_is_embedded(),
	                  g_menu_model_get_n_items(G_MENU_MODEL(menu)));

	g_timeout_add_seconds(MAX(1, atoi(seconds)), selftest_finish_cb, conv);
}
