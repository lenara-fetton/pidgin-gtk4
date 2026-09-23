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
#include "pidgin-internal.h"
#include "pidgin.h"
#include "package_revision.h"

#include "blist.h"
#include "connection.h"
#include "core.h"
#include "debug.h"
#include "request.h"
#include "server.h"

#include "gtkdialogs.h"
#include "gtklog.h"
#include "gtkutils.h"
#include "pidginabout.h"

/* Handle of the New IM / Get Info / View Log requests. */
static int dialogs_handle;

/* The GTK 2 version destroyed its dialog windows when the last connection
 * went away. Here those are the account-based requests (New IM, Get Info,
 * View Log), whose account list would be empty. */
void
pidgin_dialogs_destroy_all(void)
{
	purple_request_close_with_handle(&dialogs_handle);
}

void
pidgin_dialogs_about(void)
{
	pidgin_about_show();
}

/**************************************************************************
 * Buddy list dialogs (M3), from pidgin/gtkdialogs.c
 **************************************************************************/

static PurpleRequestFields *
user_request_fields(gboolean all_accounts)
{
	PurpleRequestFields *fields;
	PurpleRequestFieldGroup *group;
	PurpleRequestField *field;

	fields = purple_request_fields_new();

	group = purple_request_field_group_new(NULL);
	purple_request_fields_add_group(fields, group);

	field = purple_request_field_string_new("screenname", _("_Name"), NULL, FALSE);
	purple_request_field_set_type_hint(field,
		all_accounts ? "screenname-all" : "screenname");
	purple_request_field_set_required(field, TRUE);
	purple_request_field_group_add_field(group, field);

	field = purple_request_field_account_new("account", _("_Account"), NULL);
	if (all_accounts && purple_accounts_get_all() != NULL) {
		/* Only connected accounts get a default otherwise. */
		purple_request_field_account_set_default_value(field, purple_accounts_get_all()->data);
		purple_request_field_account_set_value(field, purple_accounts_get_all()->data);
		purple_request_field_account_set_show_all(field, TRUE);
		purple_request_field_set_visible(field,
			purple_accounts_get_all()->next != NULL);
	} else {
		purple_request_field_set_visible(field,
			(purple_connections_get_all() != NULL &&
			 purple_connections_get_all()->next != NULL));
	}
	purple_request_field_set_type_hint(field, "account");
	purple_request_field_set_required(field, TRUE);
	purple_request_field_group_add_field(group, field);

	return fields;
}

static void
pidgin_dialogs_im_cb(gpointer data, PurpleRequestFields *fields)
{
	PurpleAccount *account;
	const char *username;

	account  = purple_request_fields_get_account(fields, "account");
	username = purple_request_fields_get_string(fields,  "screenname");

	if (account != NULL && username != NULL && *username != '\0')
		pidgin_dialogs_im_with_user(account, username);
}

void
pidgin_dialogs_im(void)
{
	purple_request_fields(&dialogs_handle, _("New Instant Message"),
						NULL,
						_("Please enter the username or alias of the person "
						  "you would like to IM."),
						user_request_fields(FALSE),
						_("OK"), G_CALLBACK(pidgin_dialogs_im_cb),
						_("Cancel"), NULL,
						NULL, NULL, NULL,
						NULL);
}

void
pidgin_dialogs_im_with_user(PurpleAccount *account, const char *username)
{
	PurpleConversation *conv;

	g_return_if_fail(account != NULL);
	g_return_if_fail(username != NULL);

	conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, username, account);

	if (conv == NULL)
		conv = purple_conversation_new(PURPLE_CONV_TYPE_IM, account, username);

	/* Opens (or attaches a hidden) conversation window: gtkconv.c. */
	purple_conversation_present(conv);
}

static void
pidgin_dialogs_info_cb(gpointer data, PurpleRequestFields *fields)
{
	char *username;
	PurpleAccount *account;

	account  = purple_request_fields_get_account(fields, "account");

	username = g_strdup(purple_normalize(account,
		purple_request_fields_get_string(fields,  "screenname")));

	if (username != NULL && *username != '\0' && account != NULL &&
	    purple_account_get_connection(account) != NULL)
		serv_get_info(purple_account_get_connection(account), username);

	g_free(username);
}

void
pidgin_dialogs_info(void)
{
	purple_request_fields(&dialogs_handle, _("Get User Info"),
						NULL,
						_("Please enter the username or alias of the person "
						  "whose info you would like to view."),
						user_request_fields(FALSE),
						_("OK"), G_CALLBACK(pidgin_dialogs_info_cb),
						_("Cancel"), NULL,
						NULL, NULL, NULL,
						NULL);
}

static void
pidgin_dialogs_log_cb(gpointer data, PurpleRequestFields *fields)
{
	PurpleAccount *account = purple_request_fields_get_account(fields, "account");
	char *username;
	GSList *buddies, *cur;

	username = g_strdup(purple_normalize(account,
		purple_request_fields_get_string(fields, "screenname")));

	if (username != NULL && *username != '\0' && account != NULL) {
		/* A buddy that is part of a bigger contact: the contact's logs. */
		buddies = purple_find_buddies(account, username);
		for (cur = buddies; cur != NULL; cur = cur->next) {
			PurpleBlistNode *node = cur->data;

			if (node != NULL && (node->prev != NULL || node->next != NULL)) {
				pidgin_log_show_contact((PurpleContact *)node->parent);
				g_slist_free(buddies);
				g_free(username);
				return;
			}
		}
		g_slist_free(buddies);

		pidgin_log_show(PURPLE_LOG_IM, username, account);
	}

	g_free(username);
}

void
pidgin_dialogs_log(void)
{
	purple_request_fields(&dialogs_handle, _("View User Log"),
						NULL,
						_("Please enter the username or alias of the person "
						  "whose log you would like to view."),
						user_request_fields(TRUE),
						_("OK"), G_CALLBACK(pidgin_dialogs_log_cb),
						_("Cancel"), NULL,
						NULL, NULL, NULL,
						NULL);
}

static void
pidgin_dialogs_alias_contact_cb(PurpleContact *contact, const char *new_alias)
{
	purple_blist_alias_contact(contact, new_alias);
}

void
pidgin_dialogs_alias_contact(PurpleContact *contact)
{
	g_return_if_fail(contact != NULL);

	purple_request_input(contact, _("Alias Contact"), NULL,
					   _("Enter an alias for this contact."),
					   contact->alias, FALSE, FALSE, NULL,
					   _("Alias"), G_CALLBACK(pidgin_dialogs_alias_contact_cb),
					   _("Cancel"), NULL,
					   NULL, purple_contact_get_alias(contact), NULL,
					   contact);
}

static void
pidgin_dialogs_alias_buddy_cb(PurpleBuddy *buddy, const char *new_alias)
{
	purple_blist_alias_buddy(buddy, new_alias);
	serv_alias_buddy(buddy);
}

void
pidgin_dialogs_alias_buddy(PurpleBuddy *buddy)
{
	gchar *secondary;

	g_return_if_fail(buddy != NULL);

	secondary = g_strdup_printf(_("Enter an alias for %s."), buddy->name);

	purple_request_input(buddy, _("Alias Buddy"), NULL,
					   secondary, buddy->alias, FALSE, FALSE, NULL,
					   _("Alias"), G_CALLBACK(pidgin_dialogs_alias_buddy_cb),
					   _("Cancel"), NULL,
					   purple_buddy_get_account(buddy), purple_buddy_get_name(buddy), NULL,
					   buddy);

	g_free(secondary);
}

static void
pidgin_dialogs_alias_chat_cb(PurpleChat *chat, const char *new_alias)
{
	purple_blist_alias_chat(chat, new_alias);
}

void
pidgin_dialogs_alias_chat(PurpleChat *chat)
{
	g_return_if_fail(chat != NULL);

	purple_request_input(chat, _("Alias Chat"), NULL,
					   _("Enter an alias for this chat."),
					   chat->alias, FALSE, FALSE, NULL,
					   _("Alias"), G_CALLBACK(pidgin_dialogs_alias_chat_cb),
					   _("Cancel"), NULL,
					   chat->account, NULL, NULL,
					   chat);
}

/* Pidgin 2 renamed groups in place in the tree view
 * (gtk_blist_renderer_edited_cb); here it is a request. */
static void
pidgin_dialogs_rename_group_cb(PurpleGroup *group, const char *new_name)
{
	PurpleGroup *dest;

	if (new_name == NULL || *new_name == '\0')
		return;

	dest = purple_find_group(new_name);
	if (dest != NULL && dest != group &&
	    purple_utf8_strcasecmp(new_name, purple_group_get_name(group)) != 0)
		pidgin_dialogs_merge_groups(group, new_name);
	else
		purple_blist_rename_group(group, new_name);
}

void
pidgin_dialogs_rename_group(PurpleGroup *group)
{
	g_return_if_fail(group != NULL);

	purple_request_input(group, _("Rename Group"), NULL,
					   _("Enter a new name for this group."),
					   purple_group_get_name(group), FALSE, FALSE, NULL,
					   _("Rename"), G_CALLBACK(pidgin_dialogs_rename_group_cb),
					   _("Cancel"), NULL,
					   NULL, NULL, NULL,
					   group);
}

static void
pidgin_dialogs_remove_contact_cb(PurpleContact *contact)
{
	PurpleBlistNode *bnode, *cnode;
	PurpleGroup *group;

	cnode = (PurpleBlistNode *)contact;
	group = (PurpleGroup*)cnode->parent;
	for (bnode = cnode->child; bnode; bnode = bnode->next) {
		PurpleBuddy *buddy = (PurpleBuddy*)bnode;
		if (purple_account_is_connected(buddy->account))
			purple_account_remove_buddy(buddy->account, buddy, group);
	}
	purple_blist_remove_contact(contact);
}

void
pidgin_dialogs_remove_contact(PurpleContact *contact)
{
	PurpleBuddy *buddy;

	g_return_if_fail(contact != NULL);

	buddy = purple_contact_get_priority_buddy(contact);
	g_return_if_fail(buddy != NULL);

	if (PURPLE_BLIST_NODE(contact)->child == PURPLE_BLIST_NODE(buddy) &&
	    PURPLE_BLIST_NODE(buddy)->next == NULL) {
		pidgin_dialogs_remove_buddy(buddy);
	} else {
		gchar *text;
		text = g_strdup_printf(
					ngettext(
						"You are about to remove the contact containing %s "
						"and %d other buddy from your buddy list.  Do you "
						"want to continue?",
						"You are about to remove the contact containing %s "
						"and %d other buddies from your buddy list.  Do you "
						"want to continue?", contact->totalsize - 1),
					buddy->name, contact->totalsize - 1);

		purple_request_action(contact, NULL, _("Remove Contact"), text, 0,
				NULL, purple_contact_get_alias(contact), NULL,
				contact, 2,
				_("_Remove Contact"), G_CALLBACK(pidgin_dialogs_remove_contact_cb),
				_("Cancel"),
				NULL);

		g_free(text);
	}
}

typedef struct {
	PurpleGroup *parent;
	char *new_name;
} PidginGroupMergeObject;

static void
free_ggmo(PidginGroupMergeObject *ggp)
{
	g_free(ggp->new_name);
	g_free(ggp);
}

static void
pidgin_dialogs_merge_groups_cb(PidginGroupMergeObject *ggp)
{
	purple_blist_rename_group(ggp->parent, ggp->new_name);
	free_ggmo(ggp);
}

void
pidgin_dialogs_merge_groups(PurpleGroup *source, const char *new_name)
{
	gchar *text;
	PidginGroupMergeObject *ggp;

	g_return_if_fail(source != NULL);
	g_return_if_fail(new_name != NULL);

	text = g_strdup_printf(
				_("You are about to merge the group called %s into the group "
				"called %s. Do you want to continue?"), source->name, new_name);

	ggp = g_new(PidginGroupMergeObject, 1);
	ggp->parent = source;
	ggp->new_name = g_strdup(new_name);

	purple_request_action(source, NULL, _("Merge Groups"), text, 0,
			NULL, NULL, NULL,
			ggp, 2,
			_("_Merge Groups"), G_CALLBACK(pidgin_dialogs_merge_groups_cb),
			_("Cancel"), G_CALLBACK(free_ggmo));

	g_free(text);
}

static void
pidgin_dialogs_remove_group_cb(PurpleGroup *group)
{
	PurpleBlistNode *cnode, *bnode;

	cnode = ((PurpleBlistNode*)group)->child;

	while (cnode) {
		if (PURPLE_BLIST_NODE_IS_CONTACT(cnode)) {
			bnode = cnode->child;
			cnode = cnode->next;
			while (bnode) {
				PurpleBuddy *buddy;
				if (PURPLE_BLIST_NODE_IS_BUDDY(bnode)) {
					buddy = (PurpleBuddy*)bnode;
					bnode = bnode->next;
					if (purple_account_is_connected(buddy->account)) {
						purple_account_remove_buddy(buddy->account, buddy, group);
						purple_blist_remove_buddy(buddy);
					}
				} else {
					bnode = bnode->next;
				}
			}
		} else if (PURPLE_BLIST_NODE_IS_CHAT(cnode)) {
			PurpleChat *chat = (PurpleChat *)cnode;
			cnode = cnode->next;
			if (purple_account_is_connected(chat->account))
				purple_blist_remove_chat(chat);
		} else {
			cnode = cnode->next;
		}
	}

	purple_blist_remove_group(group);
}

void
pidgin_dialogs_remove_group(PurpleGroup *group)
{
	gchar *text;

	g_return_if_fail(group != NULL);

	text = g_strdup_printf(_("You are about to remove the group %s and all its members from your buddy list.  Do you want to continue?"),
						   group->name);

	purple_request_action(group, NULL, _("Remove Group"), text, 0,
						NULL, NULL, NULL,
						group, 2,
						_("_Remove Group"), G_CALLBACK(pidgin_dialogs_remove_group_cb),
						_("Cancel"), NULL);

	g_free(text);
}

static void
pidgin_dialogs_remove_buddy_cb(PurpleBuddy *buddy)
{
	PurpleGroup *group = purple_buddy_get_group(buddy);
	PurpleAccount *account = buddy->account;

	purple_debug_info("blist", "Removing '%s' from buddy list.\n", buddy->name);
	purple_account_remove_buddy(account, buddy, group);
	purple_blist_remove_buddy(buddy);
}

void
pidgin_dialogs_remove_buddy(PurpleBuddy *buddy)
{
	gchar *text;

	g_return_if_fail(buddy != NULL);

	text = g_strdup_printf(_("You are about to remove %s from your buddy list.  Do you want to continue?"),
						   buddy->name);

	purple_request_action(buddy, NULL, _("Remove Buddy"), text, 0,
						purple_buddy_get_account(buddy), purple_buddy_get_name(buddy), NULL,
						buddy, 2,
						_("_Remove Buddy"), G_CALLBACK(pidgin_dialogs_remove_buddy_cb),
						_("Cancel"), NULL);

	g_free(text);
}

static void
pidgin_dialogs_remove_chat_cb(PurpleChat *chat)
{
	purple_blist_remove_chat(chat);
}

void
pidgin_dialogs_remove_chat(PurpleChat *chat)
{
	const gchar *name;
	gchar *text;

	g_return_if_fail(chat != NULL);

	name = purple_chat_get_name(chat);
	text = g_strdup_printf(_("You are about to remove the chat %s from your buddy list.  Do you want to continue?"),
			name ? name : "");

	purple_request_action(chat, NULL, _("Remove Chat"), text, 0,
						chat->account, NULL, NULL,
						chat, 2,
						_("_Remove Chat"), G_CALLBACK(pidgin_dialogs_remove_chat_cb),
						_("Cancel"), NULL);

	g_free(text);
}
