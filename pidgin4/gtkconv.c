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
 *
 */

/*
 * pidgin4 (M4b): pidgin/gtkconv.c on GTK 4. The conversation pane (one
 * notebook page): the infopane, the PidginMessageView, the chat topic and
 * user list, the typing line, the reply/edit banner and the compose entry
 * with its toolbar; plus the PurpleConversationUiOps, the commands, the
 * unseen state and the menu actions. The window, tabs, menubar and
 * placement are in gtkconvwin.c, the M8 metadata glue in pidginconvmeta.c.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include <glib/gstdio.h>

#include "account.h"
#include "blist.h"
#include "buddyicon.h"
#include "cmds.h"
#include "core.h"
#include "debug.h"
#include "ft.h"
#include "idle.h"
#include "imgstore.h"
#include "log.h"
#include "notify.h"
#include "prefs.h"
#include "privacy.h"
#include "prpl.h"
#include "request.h"
#include "server.h"
#include "util.h"

#include "gtkblist.h"
#include "gtkconv.h"
#include "gtkconvwin.h"
#include "gtkdialogs.h"
#include "gtkutils.h"
#include "pidginanimation.h"
#include "pidginblistmodel.h"
#include "pidgincomposeentry.h"
#include "pidginimageencode.h"
#include "pidginconvmeta.h"
#include "pidginformattoolbar.h"
#include "pidginmarkup.h"
#include "pidginmenu.h"
#include "pidginmessageview.h"
#include "pidginsmileytheme.h"

#define CONV_PREFS PIDGIN_PREFS_ROOT "/conversations"
#define CONV4_PREFS PIDGIN4_PREFS_ROOT "/conversations"
#define AUTO_RESPONSE "&lt;AUTO-REPLY&gt; :"
#define BUDDY_ICON_SIZE 32          /* the infopane's, as Pidgin 2's */

static void update_tab_and_infopane(PidginConversation *gtkconv);
typedef enum { IMAGE_OFFER_NONE, IMAGE_OFFER_INLINE, IMAGE_OFFER_FILE } ImageOffer;
static ImageOffer image_offer(PidginConversation *gtkconv);
static void update_typing(PidginConversation *gtkconv);
static void chat_users_update_count(PidginConversation *gtkconv);
static void cancel_banner(PidginConversation *gtkconv);

/**************************************************************************
 * Small helpers
 **************************************************************************/

static PurplePluginProtocolInfo *
conv_prpl_info(PurpleConversation *conv)
{
	PurpleAccount *account = purple_conversation_get_account(conv);
	PurplePlugin *prpl = account ? purple_find_prpl(purple_account_get_protocol_id(account)) : NULL;

	return prpl ? PURPLE_PLUGIN_PROTOCOL_INFO(prpl) : NULL;
}

static PurpleBlistNode *
get_conversation_blist_node(PurpleConversation *conv)
{
	PurpleBlistNode *node = NULL;

	switch (purple_conversation_get_type(conv)) {
		case PURPLE_CONV_TYPE_IM:
			node = (PurpleBlistNode *)purple_find_buddy(conv->account, conv->name);
			node = node ? node->parent : NULL;
			break;
		case PURPLE_CONV_TYPE_CHAT:
			node = (PurpleBlistNode *)purple_blist_find_chat(conv->account, conv->name);
			break;
		default:
			break;
	}
	return node;
}

static gboolean
is_chat(PidginConversation *gtkconv)
{
	return purple_conversation_get_type(gtkconv->active_conv) == PURPLE_CONV_TYPE_CHAT;
}

static PidginMessageView *
conv_view(PidginConversation *gtkconv)
{
	return PIDGIN_MESSAGE_VIEW(gtkconv->imhtml);
}

static PidginComposeEntry *
conv_entry(PidginConversation *gtkconv)
{
	return PIDGIN_COMPOSE_ENTRY(gtkconv->entry);
}

static gboolean
account_is_jabber(PurpleAccount *account)
{
	return purple_strequal(purple_account_get_protocol_id(account), "prpl-jabber");
}

GtkWidget *
pidgin_conv_get_message_view(PidginConversation *gtkconv)
{
	return gtkconv->imhtml;
}

GtkWidget *
pidgin_conv_get_entry(PidginConversation *gtkconv)
{
	return gtkconv->entry;
}

/* M7: the names the plugin README uses. */
GtkWidget *
pidgin_conv_get_compose_entry(PidginConversation *gtkconv)
{
	g_return_val_if_fail(gtkconv != NULL, NULL);
	return gtkconv->entry;
}

GtkWidget *
pidgin_conv_get_send_button(PidginConversation *gtkconv)
{
	g_return_val_if_fail(gtkconv != NULL, NULL);
	return gtkconv->send_button;
}

GtkWidget *
pidgin_conv_get_toolbar(PidginConversation *gtkconv)
{
	return gtkconv->toolbar;
}

GtkWidget *
pidgin_conv_get_tab_container(PidginConversation *gtkconv)
{
	return gtkconv->tab_cont;
}

PurpleConversation *
pidgin_conv_get_conversation(PidginConversation *gtkconv)
{
	return gtkconv->active_conv;
}

PidginWindow *
pidgin_conv_get_window(PidginConversation *gtkconv)
{
	return gtkconv->win;
}

void
pidgin_conv_set_window(PidginConversation *gtkconv, PidginWindow *win)
{
	gtkconv->win = win;
}

gboolean
pidgin_conv_is_hidden(PidginConversation *gtkconv)
{
	g_return_val_if_fail(gtkconv != NULL, FALSE);
	return gtkconv->win == NULL || pidgin_conv_window_is_hidden(gtkconv->win);
}

void *
pidgin_conversations_get_handle(void)
{
	/* The message view registered the timestamp and displaying/displayed
	 * signals on it; the others are registered here. */
	return pidgin_message_view_get_conv_handle();
}

/**************************************************************************
 * Unseen state
 **************************************************************************/

void
pidgin_conv_set_unseen(PurpleConversation *conv, PidginUnseenState state)
{
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);
	guint count;
	PidginUnseenState cur;

	if (gtkconv == NULL)
		return;

	if (state == PIDGIN_UNSEEN_NONE) {
		if (gtkconv->unseen_state == PIDGIN_UNSEEN_NONE && gtkconv->unseen_count == 0)
			return;
		count = 0;
		cur = PIDGIN_UNSEEN_NONE;
	} else {
		count = gtkconv->unseen_count + (state >= PIDGIN_UNSEEN_TEXT ? 1 : 0);
		cur = MAX(state, gtkconv->unseen_state);
	}
	gtkconv->unseen_count = count;
	gtkconv->unseen_state = cur;

	/* Pidgin 2 keeps these on the conversation too (the docklet reads them). */
	purple_conversation_set_data(conv, "unseen-count", GINT_TO_POINTER(count));
	purple_conversation_set_data(conv, "unseen-state", GINT_TO_POINTER(cur));
	purple_conversation_update(conv, PURPLE_CONV_UPDATE_UNSEEN);
}

static PidginUnseenState
unseen_for_flags(PurpleMessageFlags flags)
{
	if ((flags & PURPLE_MESSAGE_NICK) == PURPLE_MESSAGE_NICK)
		return PIDGIN_UNSEEN_NICK;
	if (flags & (PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_ERROR))
		return PIDGIN_UNSEEN_EVENT;
	if (flags & PURPLE_MESSAGE_NO_LOG)
		return PIDGIN_UNSEEN_NO_LOG;
	return PIDGIN_UNSEEN_TEXT;
}

void
pidgin_conv_seen(PidginConversation *gtkconv)
{
	if (gtkconv == NULL || pidgin_conv_is_hidden(gtkconv))
		return;
	pidgin_conv_set_unseen(gtkconv->active_conv, PIDGIN_UNSEEN_NONE);
	pidgin_conv_meta_mark_displayed(gtkconv->active_conv);
}

GList *
pidgin_conversations_find_unseen_list(PurpleConversationType type,
                                      PidginUnseenState min_state,
                                      gboolean hidden_only, guint max_count)
{
	GList *l, *r = NULL;
	guint c = 0;

	if (type == PURPLE_CONV_TYPE_IM)
		l = purple_get_ims();
	else if (type == PURPLE_CONV_TYPE_CHAT)
		l = purple_get_chats();
	else
		l = purple_get_conversations();

	for (; l != NULL && (max_count == 0 || c < max_count); l = l->next) {
		PurpleConversation *conv = l->data;
		PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);

		if (gtkconv == NULL || gtkconv->active_conv != conv)
			continue;
		if (gtkconv->unseen_state >= min_state && gtkconv->unseen_state > PIDGIN_UNSEEN_NONE &&
		    (!hidden_only || pidgin_conv_is_hidden(gtkconv))) {
			r = g_list_prepend(r, conv);
			c++;
		}
	}
	return r;
}

guint
pidgin_conversations_get_unseen_count(PurpleConversationType type,
                                      PidginUnseenState min_state)
{
	GList *list = pidgin_conversations_find_unseen_list(type, min_state, FALSE, 0), *l;
	guint n = 0;

	for (l = list; l != NULL; l = l->next)
		n += PIDGIN_CONVERSATION((PurpleConversation *)l->data)->unseen_count;
	g_list_free(list);
	return n;
}

guint
pidgin_conversations_fill_menu(GMenu *menu, GList *convs)
{
	GList *l;
	guint n = 0;

	for (l = convs; l != NULL; l = l->next) {
		PurpleConversation *conv = l->data;
		PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);
		GMenuItem *item;
		char *label;

		if (gtkconv == NULL)
			continue;
		label = g_strdup_printf("%s (%u)", purple_conversation_get_title(conv),
		                        gtkconv->unseen_count);
		item = g_menu_item_new(label, NULL);
		g_menu_item_set_action_and_target(item, "app.pidgin4-present-conv", "t",
		                                  (guint64)GPOINTER_TO_SIZE(conv));
		g_menu_append_item(menu, item);
		g_object_unref(item);
		g_free(label);
		n++;
	}
	if (n > 1)
		g_menu_append(menu, _("Show All"), "app.pidgin4-present-unseen");
	return n;
}

/**************************************************************************
 * Commands
 **************************************************************************/

static PurpleCmdId cmd_ids[6];

static PurpleCmdRet
say_command_cb(PurpleConversation *conv, const char *cmd, char **args, char **error,
               void *data)
{
	if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_IM)
		purple_conv_im_send(PURPLE_CONV_IM(conv), args[0]);
	else if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_CHAT)
		purple_conv_chat_send(PURPLE_CONV_CHAT(conv), args[0]);
	return PURPLE_CMD_RET_OK;
}

static PurpleCmdRet
me_command_cb(PurpleConversation *conv, const char *cmd, char **args, char **error,
              void *data)
{
	char *tmp = g_strdup_printf("/me %s", args[0]);

	if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_IM)
		purple_conv_im_send(PURPLE_CONV_IM(conv), tmp);
	else if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_CHAT)
		purple_conv_chat_send(PURPLE_CONV_CHAT(conv), tmp);
	g_free(tmp);
	return PURPLE_CMD_RET_OK;
}

static PurpleCmdRet
debug_command_cb(PurpleConversation *conv, const char *cmd, char **args, char **error,
                 void *data)
{
	char *tmp, *markup;

	if (!g_ascii_strcasecmp(args[0], "version")) {
		tmp = g_strdup_printf("Using Pidgin 4 v%s with libpurple v%s.",
		                      DISPLAY_VERSION, purple_core_get_version());
	} else if (!g_ascii_strcasecmp(args[0], "plugins")) {
		GString *str = g_string_new("Loaded Plugins: ");
		const GList *plugins = purple_plugins_get_loaded();

		if (plugins != NULL) {
			for (; plugins; plugins = plugins->next) {
				g_string_append(str, purple_plugin_get_name(plugins->data));
				if (plugins->next)
					g_string_append(str, ", ");
			}
		} else {
			g_string_append(str, "(none)");
		}
		tmp = g_string_free(str, FALSE);
	} else {
		purple_conversation_write(conv, NULL, _("Supported debug options are: plugins, version"),
		                          PURPLE_MESSAGE_NO_LOG | PURPLE_MESSAGE_ERROR, time(NULL));
		return PURPLE_CMD_RET_OK;
	}

	markup = g_markup_escape_text(tmp, -1);
	if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_IM)
		purple_conv_im_send(PURPLE_CONV_IM(conv), markup);
	else if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_CHAT)
		purple_conv_chat_send(PURPLE_CONV_CHAT(conv), markup);
	g_free(tmp);
	g_free(markup);
	return PURPLE_CMD_RET_OK;
}

static PurpleCmdRet
clear_command_cb(PurpleConversation *conv, const char *cmd, char **args, char **error,
                 void *data)
{
	purple_conversation_clear_message_history(conv);
	return PURPLE_CMD_RET_OK;
}

static PurpleCmdRet
clearall_command_cb(PurpleConversation *conv, const char *cmd, char **args, char **error,
                    void *data)
{
	purple_conversation_foreach(purple_conversation_clear_message_history);
	return PURPLE_CMD_RET_OK;
}

static PurpleCmdRet
help_command_cb(PurpleConversation *conv, const char *cmd, char **args, char **error,
                void *data)
{
	GList *l, *text;
	GString *s;

	if (args[0] != NULL) {
		s = g_string_new("");
		text = purple_cmd_help(conv, args[0]);
		if (text) {
			for (l = text; l; l = l->next)
				g_string_append_printf(s, l->next ? "%s\n" : "%s", (char *)l->data);
		} else {
			g_string_append(s, _("No such command (in this context)."));
		}
	} else {
		s = g_string_new(_("Use \"/help &lt;command&gt;\" for help on a specific command.\n"
		                   "The following commands are available in this context:\n"));
		text = purple_cmd_list(conv);
		for (l = text; l; l = l->next)
			g_string_append_printf(s, l->next ? "%s, " : "%s.", (char *)l->data);
		g_list_free(text);
	}

	purple_conversation_write(conv, NULL, s->str, PURPLE_MESSAGE_NO_LOG, time(NULL));
	g_string_free(s, TRUE);
	return PURPLE_CMD_RET_OK;
}

static void
clear_conversation_scrollback_cb(PurpleConversation *conv, void *data)
{
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);

	if (gtkconv != NULL)
		pidgin_message_view_clear(conv_view(gtkconv));
}

/* Returns TRUE if @text was a command (done, or an error shown). */
static gboolean
do_command(PurpleConversation *conv, const char *text, const char *markup)
{
	PurpleCmdStatus status;
	char *error = NULL, *args_markup;
	const char *cmdline;
	gboolean retval = FALSE;

	if (text == NULL || text[0] != '/')
		return FALSE;
	cmdline = text + 1;

	if (purple_strequal(cmdline, "xyzzy")) {
		purple_conversation_write(conv, "", "Nothing happens", PURPLE_MESSAGE_NO_LOG,
		                          time(NULL));
		return TRUE;
	}

	/* The arguments keep their formatting when the markup starts with the
	 * slash itself; otherwise the plain text will do. */
	if (markup != NULL && markup[0] == '/')
		args_markup = g_strdup(markup + 1);
	else
		args_markup = g_markup_escape_text(cmdline, -1);

	status = purple_cmd_do_command(conv, cmdline, args_markup, &error);
	g_free(args_markup);

	switch (status) {
		case PURPLE_CMD_STATUS_OK:
			retval = TRUE;
			break;
		case PURPLE_CMD_STATUS_NOT_FOUND: {
			PurplePluginProtocolInfo *prpl_info = conv_prpl_info(conv);

			if (prpl_info != NULL && (prpl_info->options & OPT_PROTO_SLASH_COMMANDS_NATIVE)) {
				const char *spaceslash = cmdline;

				/* A '/' in the first word: probably not meant as a command. */
				while (*spaceslash && *spaceslash != ' ' && *spaceslash != '/')
					spaceslash++;
				if (*spaceslash != '/') {
					purple_conversation_write(conv, "", _("Unknown command."),
					                          PURPLE_MESSAGE_NO_LOG, time(NULL));
					retval = TRUE;
				}
			}
			break;
		}
		case PURPLE_CMD_STATUS_WRONG_ARGS:
			purple_conversation_write(conv, "", _("Syntax Error:  You typed the wrong "
				"number of arguments to that command."), PURPLE_MESSAGE_NO_LOG, time(NULL));
			retval = TRUE;
			break;
		case PURPLE_CMD_STATUS_FAILED:
			purple_conversation_write(conv, "", error ? error :
				_("Your command failed for an unknown reason."), PURPLE_MESSAGE_NO_LOG,
				time(NULL));
			retval = TRUE;
			break;
		case PURPLE_CMD_STATUS_WRONG_TYPE:
			purple_conversation_write(conv, "",
				purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_IM
					? _("That command only works in chats, not IMs.")
					: _("That command only works in IMs, not chats."),
				PURPLE_MESSAGE_NO_LOG, time(NULL));
			retval = TRUE;
			break;
		case PURPLE_CMD_STATUS_WRONG_PRPL:
			purple_conversation_write(conv, "", _("That command doesn't work on this protocol."),
			                          PURPLE_MESSAGE_NO_LOG, time(NULL));
			retval = TRUE;
			break;
	}
	g_free(error);
	return retval;
}

static void
register_commands(void)
{
	cmd_ids[0] = purple_cmd_register("say", "S", PURPLE_CMD_P_DEFAULT,
		PURPLE_CMD_FLAG_CHAT | PURPLE_CMD_FLAG_IM, NULL, say_command_cb,
		_("say &lt;message&gt;:  Send a message normally as if you weren't using a command."), NULL);
	cmd_ids[1] = purple_cmd_register("me", "S", PURPLE_CMD_P_DEFAULT,
		PURPLE_CMD_FLAG_CHAT | PURPLE_CMD_FLAG_IM, NULL, me_command_cb,
		_("me &lt;action&gt;:  Send an IRC style action to a buddy or chat."), NULL);
	cmd_ids[2] = purple_cmd_register("debug", "w", PURPLE_CMD_P_DEFAULT,
		PURPLE_CMD_FLAG_CHAT | PURPLE_CMD_FLAG_IM, NULL, debug_command_cb,
		_("debug &lt;option&gt;:  Send various debug information to the current conversation."), NULL);
	cmd_ids[3] = purple_cmd_register("clear", "", PURPLE_CMD_P_DEFAULT,
		PURPLE_CMD_FLAG_CHAT | PURPLE_CMD_FLAG_IM, NULL, clear_command_cb,
		_("clear: Clears the conversation scrollback."), NULL);
	cmd_ids[4] = purple_cmd_register("clearall", "", PURPLE_CMD_P_DEFAULT,
		PURPLE_CMD_FLAG_CHAT | PURPLE_CMD_FLAG_IM, NULL, clearall_command_cb,
		_("clear: Clears all conversation scrollbacks."), NULL);
	cmd_ids[5] = purple_cmd_register("help", "w", PURPLE_CMD_P_DEFAULT,
		PURPLE_CMD_FLAG_CHAT | PURPLE_CMD_FLAG_IM | PURPLE_CMD_FLAG_ALLOW_WRONG_ARGS, NULL,
		help_command_cb, _("help &lt;command&gt;:  Help on a specific command."), NULL);
}

/**************************************************************************
 * Sending
 **************************************************************************/

/* The entry's markup as plain text: XMPP entries give escaped XEP-0393
 * text with <br> newlines. */
static char *
markup_to_plain(const char *markup)
{
	return purple_unescape_html(markup);
}

static void
send_markup(PidginConversation *gtkconv, const char *markup)
{
	PurpleConversation *conv = gtkconv->active_conv;
	PurpleMessageFlags flags = 0;

	if (purple_strcasestr(markup, "<img id=") != NULL)
		flags |= PURPLE_MESSAGE_IMAGES;

	if (conv->features & PURPLE_CONNECTION_NO_NEWLINES) {
		char *tmp = purple_strcasereplace(markup, "<br/>", "<br>");
		char **lines = NULL;
		int i;

		/* One message per line, as gtk_imhtml_get_markup_lines() did. */
		{
			char *norm = purple_strcasereplace(tmp, "<br>", "\n");
			lines = g_strsplit(norm, "\n", -1);
			g_free(norm);
		}
		for (i = 0; lines[i] != NULL; i++) {
			if (*lines[i] == '\0')
				continue;
			if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_IM)
				purple_conv_im_send_with_flags(PURPLE_CONV_IM(conv), lines[i], flags);
			else
				purple_conv_chat_send_with_flags(PURPLE_CONV_CHAT(conv), lines[i], flags);
		}
		g_strfreev(lines);
		g_free(tmp);
	} else if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_IM) {
		purple_conv_im_send_with_flags(PURPLE_CONV_IM(conv), markup, flags);
	} else {
		purple_conv_chat_send_with_flags(PURPLE_CONV_CHAT(conv), markup, flags);
	}
}

static gboolean
entry_send_cb(PidginComposeEntry *entry, const char *markup, PidginConversation *gtkconv)
{
	PurpleConversation *conv = gtkconv->active_conv;
	PurpleAccount *account = purple_conversation_get_account(conv);
	char *text, *plain;
	gboolean sent = FALSE;

	text = pidgin_compose_entry_get_text(entry);
	if (text != NULL && text[0] == '/' && do_command(conv, text, markup)) {
		g_free(text);
		return TRUE;
	}

	if ((purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_CHAT &&
	     purple_conv_chat_has_left(PURPLE_CONV_CHAT(conv))) ||
	    !purple_account_is_connected(account) || text == NULL ||
	    *g_strstrip(text) == '\0') {
		g_free(text);
		return FALSE;
	}
	g_free(text);

	purple_idle_touch();
	plain = markup_to_plain(markup);

	if (gtkconv->editing != NULL) {
		sent = pidgin_conv_meta_send_correction(conv, PIDGIN_MESSAGE(gtkconv->editing), plain);
		if (!sent)
			purple_conversation_write(conv, NULL, _("The message could not be corrected."),
			                          PURPLE_MESSAGE_ERROR | PURPLE_MESSAGE_NO_LOG, time(NULL));
		cancel_banner(gtkconv);
		g_free(plain);
		return sent;
	}
	if (gtkconv->replying != NULL) {
		sent = pidgin_conv_meta_send_reply(conv, PIDGIN_MESSAGE(gtkconv->replying), plain);
		cancel_banner(gtkconv);
	}
	g_free(plain);

	if (!sent)
		send_markup(gtkconv, markup);

	pidgin_conv_set_unseen(conv, PIDGIN_UNSEEN_NONE);
	return TRUE;
}

static void
send_button_cb(GtkButton *button, PidginConversation *gtkconv)
{
	pidgin_compose_entry_send(conv_entry(gtkconv));
	gtk_widget_grab_focus(gtkconv->entry);
}

static void
typing_changed_cb(PidginComposeEntry *entry, PurpleTypingState state,
                  PidginConversation *gtkconv)
{
	PurpleConversation *conv = gtkconv->active_conv;
	PurpleConvIm *im;
	PurpleConnection *gc;

	if (purple_conversation_get_type(conv) != PURPLE_CONV_TYPE_IM ||
	    !purple_prefs_get_bool("/purple/conversations/im/send_typing") ||
	    (gc = purple_conversation_get_gc(conv)) == NULL)
		return;

	im = PURPLE_CONV_IM(conv);
	if (state == PURPLE_TYPING) {
		/* Resend TYPING only when the prpl's interval has passed. */
		if (time(NULL) > purple_conv_im_get_type_again(im)) {
			unsigned int timeout = serv_send_typing(gc, purple_conversation_get_name(conv),
			                                        PURPLE_TYPING);
			purple_conv_im_set_type_again(im, timeout);
		}
	} else {
		serv_send_typing(gc, purple_conversation_get_name(conv), state);
		purple_conv_im_set_type_again(im, 0);
	}
}

/**************************************************************************
 * Reply / edit banner
 **************************************************************************/

static void
cancel_banner(PidginConversation *gtkconv)
{
	g_clear_object(&gtkconv->replying);
	g_clear_object(&gtkconv->editing);
	if (gtkconv->banner != NULL)
		gtk_widget_set_visible(gtkconv->banner, FALSE);
}

static void
banner_cancel_cb(GtkButton *button, PidginConversation *gtkconv)
{
	gboolean was_editing = gtkconv->editing != NULL;

	cancel_banner(gtkconv);
	if (was_editing)
		pidgin_compose_entry_clear(conv_entry(gtkconv));
	gtk_widget_grab_focus(gtkconv->entry);
}

static void
show_banner(PidginConversation *gtkconv, const char *what, PidginMessage *msg)
{
	char *snip = pidgin_conv_meta_snippet(pidgin_message_get_plain_text(msg), 60);
	char *text = g_strdup_printf("%s %s: %s", what, pidgin_message_get_alias(msg), snip);

	gtk_label_set_text(GTK_LABEL(gtkconv->banner_label), text);
	gtk_widget_set_visible(gtkconv->banner, TRUE);
	g_free(text);
	g_free(snip);
}

static void
start_edit(PidginConversation *gtkconv, PidginMessage *msg)
{
	if (msg == NULL || pidgin_message_get_retracted(msg) ||
	    !pidgin_conv_meta_has_command(gtkconv->active_conv, "send-correction") ||
	    (pidgin_message_get_stanza_id(msg) == NULL && pidgin_message_get_origin_id(msg) == NULL)) {
		gtk_widget_error_bell(gtkconv->entry);
		return;
	}
	cancel_banner(gtkconv);
	gtkconv->editing = g_object_ref(G_OBJECT(msg));
	pidgin_compose_entry_set_markup(conv_entry(gtkconv), pidgin_message_get_html(msg));
	show_banner(gtkconv, _("Editing"), msg);
	gtk_widget_grab_focus(gtkconv->entry);
}

static void
edit_last_cb(PidginComposeEntry *entry, PidginConversation *gtkconv)
{
	start_edit(gtkconv, pidgin_message_view_get_last_sent(conv_view(gtkconv)));
}

/**************************************************************************
 * Message view signals
 **************************************************************************/

static void
view_reaction_cb(PidginMessageView *view, PidginMessage *msg, const char *emoji,
                 gboolean add, PidginConversation *gtkconv)
{
	char *self = pidgin_conv_meta_self_id(gtkconv->active_conv);

	if (!pidgin_conv_meta_send_reaction(gtkconv->active_conv, msg, emoji, add, self))
		gtk_widget_error_bell(GTK_WIDGET(view));
	g_free(self);
}

static void
view_reply_cb(PidginMessageView *view, PidginMessage *msg, PidginConversation *gtkconv)
{
	if (!pidgin_conv_meta_has_command(gtkconv->active_conv, "send-reply")) {
		gtk_widget_error_bell(GTK_WIDGET(view));
		return;
	}
	cancel_banner(gtkconv);
	gtkconv->replying = g_object_ref(G_OBJECT(msg));
	show_banner(gtkconv, _("Replying to"), msg);
	gtk_widget_grab_focus(gtkconv->entry);
}

static void
view_edit_cb(PidginMessageView *view, PidginMessage *msg, PidginConversation *gtkconv)
{
	start_edit(gtkconv, msg);
}

static void
view_retract_cb(PidginMessageView *view, PidginMessage *msg, PidginConversation *gtkconv)
{
	if (!pidgin_conv_meta_send_retraction(gtkconv->active_conv, msg))
		gtk_widget_error_bell(GTK_WIDGET(view));
}

/**************************************************************************
 * Icons, tab label, infopane
 **************************************************************************/

static const char *
presence_icon_name(PurplePresence *p)
{
	if (p == NULL || !purple_presence_is_online(p))
		return "pidgin-status-offline";
	if (purple_presence_is_status_primitive_active(p, PURPLE_STATUS_UNAVAILABLE))
		return "pidgin-status-busy";
	if (purple_presence_is_status_primitive_active(p, PURPLE_STATUS_AWAY))
		return "pidgin-status-away";
	if (purple_presence_is_status_primitive_active(p, PURPLE_STATUS_EXTENDED_AWAY))
		return "pidgin-status-extended-away";
	if (purple_presence_is_status_primitive_active(p, PURPLE_STATUS_INVISIBLE))
		return "pidgin-status-invisible";
	return "pidgin-status-available";
}

static const char *
conv_icon_name(PurpleConversation *conv)
{
	PurpleAccount *account = purple_conversation_get_account(conv);
	PurpleBuddy *buddy;

	if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_CHAT)
		return purple_account_is_connected(account) ? "pidgin-status-chat"
		                                            : "pidgin-status-offline";
	if (purple_conv_im_get_typing_state(PURPLE_CONV_IM(conv)) == PURPLE_TYPING)
		return "pidgin-mood-typing";
	if (!purple_account_is_connected(account))
		return "pidgin-status-offline";
	buddy = purple_find_buddy(account, purple_conversation_get_name(conv));
	if (buddy == NULL)
		return "pidgin-status-person";
	return presence_icon_name(purple_buddy_get_presence(buddy));
}

GIcon *
pidgin_conv_get_tab_icon(PurpleConversation *conv, gboolean small_icon)
{
	g_return_val_if_fail(conv != NULL, NULL);
	return g_themed_icon_new(conv_icon_name(conv));
}

void
pidgin_conv_update_tab(PidginConversation *gtkconv)
{
	PurpleConversation *conv = gtkconv->active_conv;
	static const char *const classes[] = { "unseen-event", "unseen-no-log", "unseen-text",
		"unseen-nick", "typing", NULL };
	const char *cls = NULL;
	const char *title = purple_conversation_get_title(conv);
	int i;

	if (gtkconv->tab_label == NULL)
		return;

	gtk_label_set_text(GTK_LABEL(gtkconv->tab_label), title);
	gtk_widget_set_tooltip_text(gtkconv->tabby, title);
	gtk_image_set_from_icon_name(GTK_IMAGE(gtkconv->icon), conv_icon_name(conv));

	for (i = 0; classes[i] != NULL; i++)
		gtk_widget_remove_css_class(gtkconv->tab_label, classes[i]);
	switch (gtkconv->unseen_state) {
		case PIDGIN_UNSEEN_EVENT: cls = "unseen-event"; break;
		case PIDGIN_UNSEEN_NO_LOG: cls = "unseen-no-log"; break;
		case PIDGIN_UNSEEN_TEXT: cls = "unseen-text"; break;
		case PIDGIN_UNSEEN_NICK: cls = "unseen-nick"; break;
		default: break;
	}
	if (cls != NULL)
		gtk_widget_add_css_class(gtkconv->tab_label, cls);
	if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_IM &&
	    purple_conv_im_get_typing_state(PURPLE_CONV_IM(conv)) == PURPLE_TYPING)
		gtk_widget_add_css_class(gtkconv->tab_label, "typing");

	gtk_widget_set_visible(gtkconv->close,
	                       purple_prefs_get_bool(CONV_PREFS "/close_on_tabs"));

	/* The window title follows its current tab. */
	if (gtkconv->win != NULL && pidgin_conv_window_get_active_gtkconv(gtkconv->win) == gtkconv)
		gtk_window_set_title(GTK_WINDOW(gtkconv->win->window), title);
}

void
pidgin_conv_update_buddy_icon(PurpleConversation *conv)
{
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);
	PurpleAccount *account;
	PurpleBuddyIcon *icon;
	GdkPaintable *paintable = NULL;
	gconstpointer data;
	size_t len = 0;
	PurpleContact *contact;
	PurpleStoredImage *custom = NULL;

	if (gtkconv == NULL || purple_conversation_get_type(conv) != PURPLE_CONV_TYPE_IM ||
	    gtkconv->u.im->icon == NULL)
		return;

	account = purple_conversation_get_account(conv);
	gtkconv->u.im->show_icon = purple_prefs_get_bool(CONV_PREFS "/im/show_buddy_icons");

	/* The contact's custom icon, then the buddy's. */
	{
		PurpleBuddy *buddy = purple_find_buddy(account, purple_conversation_get_name(conv));

		contact = buddy ? purple_buddy_get_contact(buddy) : NULL;
		if (contact != NULL)
			custom = purple_buddy_icons_node_find_custom_icon((PurpleBlistNode *)contact);
	}
	if (custom != NULL) {
		paintable = pidgin_paintable_new_from_imgstore(custom);
		purple_imgstore_unref(custom);
	} else {
		icon = purple_conv_im_get_icon(PURPLE_CONV_IM(conv));
		if (icon == NULL)
			icon = purple_buddy_icons_find(account, purple_conversation_get_name(conv));
		if (icon != NULL) {
			data = purple_buddy_icon_get_data(icon, &len);
			if (data != NULL && len > 0)
				paintable = pidgin_paintable_new_from_data(data, len);
			if (icon != purple_conv_im_get_icon(PURPLE_CONV_IM(conv)))
				purple_buddy_icon_unref(icon);
		}
	}

	gtk_image_set_from_paintable(GTK_IMAGE(gtkconv->u.im->icon), paintable);
	gtk_widget_set_visible(gtkconv->u.im->icon, paintable != NULL && gtkconv->u.im->show_icon);
	g_clear_object(&paintable);
}

static void
update_infopane(PidginConversation *gtkconv)
{
	PurpleConversation *conv = gtkconv->active_conv;
	PurpleAccount *account = purple_conversation_get_account(conv);
	char *markup, *status = NULL;
	GIcon *prpl_icon;

	markup = g_markup_printf_escaped("<b>%s</b>", purple_conversation_get_title(conv));
	gtk_label_set_markup(GTK_LABEL(gtkconv->infopane_name), markup);
	g_free(markup);

	if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_IM) {
		PurpleBuddy *buddy = purple_find_buddy(account, purple_conversation_get_name(conv));

		if (buddy != NULL && purple_account_is_connected(account)) {
			PurpleStatus *st = purple_presence_get_active_status(purple_buddy_get_presence(buddy));
			const char *msg = purple_status_get_attr_string(st, "message");
			char *plain = msg ? purple_markup_strip_html(msg) : NULL;

			if (plain != NULL)
				g_strdelimit(plain, "\r\n", ' ');
			status = (plain && *plain) ? g_strdup_printf("%s - %s", purple_status_get_name(st), plain)
			                           : g_strdup(purple_status_get_name(st));
			g_free(plain);
		} else if (!purple_account_is_connected(account)) {
			status = g_strdup(_("Account offline"));
		}
		pidgin_conv_update_buddy_icon(conv);
	} else {
		PurpleConvChat *chat = PURPLE_CONV_CHAT(conv);

		if (purple_conv_chat_has_left(chat))
			status = g_strdup(_("You are not in this chat"));
		else if (purple_conv_chat_get_topic(chat) != NULL) {
			status = purple_markup_strip_html(purple_conv_chat_get_topic(chat));
			g_strdelimit(status, "\r\n", ' ');
		}
	}
	gtk_label_set_text(GTK_LABEL(gtkconv->infopane_status), status ? status : "");
	gtk_widget_set_visible(gtkconv->infopane_status, status != NULL && *status != '\0');
	g_free(status);

	prpl_icon = pidgin_create_prpl_gicon(account, NULL);
	gtk_image_set_from_gicon(GTK_IMAGE(gtkconv->infopane_prpl), prpl_icon);
	g_clear_object(&prpl_icon);
	gtk_widget_set_visible(gtkconv->infopane_prpl,
	                       purple_prefs_get_bool(PIDGIN_PREFS_ROOT "/blist/show_protocol_icons"));
}

static void
update_tab_and_infopane(PidginConversation *gtkconv)
{
	pidgin_conv_update_tab(gtkconv);
	update_infopane(gtkconv);
}

static void
update_typing(PidginConversation *gtkconv)
{
	PurpleConversation *conv = gtkconv->active_conv;
	PurpleTypingState state;
	char *text = NULL;

	if (purple_conversation_get_type(conv) != PURPLE_CONV_TYPE_IM)
		return;
	state = purple_conv_im_get_typing_state(PURPLE_CONV_IM(conv));
	if (state == PURPLE_TYPING)
		text = g_strdup_printf(_("%s is typing..."), purple_conversation_get_title(conv));
	else if (state == PURPLE_TYPED)
		text = g_strdup_printf(_("%s has stopped typing"), purple_conversation_get_title(conv));
	gtk_label_set_text(GTK_LABEL(gtkconv->typing_label), text ? text : "");
	gtk_widget_set_visible(gtkconv->typing_label, text != NULL);
	g_free(text);
	pidgin_conv_update_tab(gtkconv);
}

/* Formatting, toolbar and buttons for the connection's features. */
static void
update_features(PidginConversation *gtkconv)
{
	PurpleConversation *conv = gtkconv->active_conv;
	PurpleAccount *account = purple_conversation_get_account(conv);
	PurplePluginProtocolInfo *prpl_info = conv_prpl_info(conv);
	PidginComposeEntry *entry = conv_entry(gtkconv);
	PidginMessageView *view = conv_view(gtkconv);
	gboolean meta, moderate = FALSE;

	pidgin_compose_entry_set_caps(entry, pidgin_format_caps_for_account(account));
	pidgin_compose_entry_set_markup_flags(entry,
		(prpl_info && (prpl_info->options & OPT_PROTO_USE_POINTSIZE))
			? PIDGIN_MARKUP_USE_POINTSIZE : 0);
	pidgin_compose_entry_set_smiley_category(entry, purple_account_get_protocol_name(account));
	/* a pasted image goes inline or as a file, if either (see offer_image) */
	pidgin_compose_entry_set_paste_images(entry, image_offer(gtkconv) != IMAGE_OFFER_NONE);
	if (gtkconv->toolbar != NULL) {
		pidgin_format_toolbar_update(PIDGIN_FORMAT_TOOLBAR(gtkconv->toolbar));
		/* Pidgin 2's toolbar "Attention!" button, for IMs */
		pidgin_format_toolbar_set_show_attention(PIDGIN_FORMAT_TOOLBAR(gtkconv->toolbar),
			!is_chat(gtkconv) && prpl_info != NULL && prpl_info->send_attention != NULL);
	}

	pidgin_message_view_set_nick_color_scheme(view, account_is_jabber(account)
		? PIDGIN_NICK_COLOR_XEP0392 : PIDGIN_NICK_COLOR_PIDGIN);
	{
		/* Reaction senders are bare JIDs in IMs and nicks in rooms (M8). */
		char *self = pidgin_conv_meta_self_id(conv);

		pidgin_message_view_set_self_id(view, self);
		g_free(self);
	}

	meta = pidgin_conv_meta_has_command(conv, "send-reaction");
	if (meta && is_chat(gtkconv)) {
		PurpleConvChat *chat = PURPLE_CONV_CHAT(conv);
		PurpleConvChatBuddy *me = purple_conv_chat_cb_find(chat, purple_conv_chat_get_nick(chat));

		moderate = me != NULL && (me->flags & (PURPLE_CBFLAGS_OP | PURPLE_CBFLAGS_FOUNDER |
		                                       PURPLE_CBFLAGS_HALFOP)) &&
		           pidgin_conv_meta_has_command(conv, "send-moderation");
	}
	pidgin_message_view_set_message_actions(view, meta, moderate);

	if (gtkconv->win != NULL && pidgin_conv_window_get_active_gtkconv(gtkconv->win) == gtkconv)
		pidgin_conv_window_update_menu(gtkconv->win);
}

void
pidgin_conv_update_buttons_by_protocol(PurpleConversation *conv)
{
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);

	if (gtkconv != NULL)
		update_features(gtkconv);
}

/**************************************************************************
 * Chat user list
 **************************************************************************/

/* purple_normalize() returns a static buffer: compare copies. */
static gboolean
same_nick(PurpleAccount *account, const char *a, const char *b)
{
	char *na;
	gboolean ret;

	if (a == NULL || b == NULL)
		return FALSE;
	na = g_strdup(purple_normalize(account, a));
	ret = purple_strequal(na, purple_normalize(account, b));
	g_free(na);
	return ret;
}

#define PIDGIN_TYPE_CHAT_USER (pidgin_chat_user_get_type())
G_DECLARE_FINAL_TYPE(PidginChatUser, pidgin_chat_user, PIDGIN, CHAT_USER, GObject)

struct _PidginChatUser
{
	GObject parent;
	char *name;
	char *alias;
	char *key;               /* collation key of the alias */
	PurpleConvChatBuddyFlags flags;
	gboolean buddy;
	gboolean ignored;
};

G_DEFINE_FINAL_TYPE(PidginChatUser, pidgin_chat_user, G_TYPE_OBJECT)

static void
pidgin_chat_user_finalize(GObject *obj)
{
	PidginChatUser *u = PIDGIN_CHAT_USER(obj);

	g_free(u->name);
	g_free(u->alias);
	g_free(u->key);
	G_OBJECT_CLASS(pidgin_chat_user_parent_class)->finalize(obj);
}

static void
pidgin_chat_user_class_init(PidginChatUserClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = pidgin_chat_user_finalize;
}

static void
pidgin_chat_user_init(PidginChatUser *u)
{
}

#define RANK_FLAGS (PURPLE_CBFLAGS_VOICE | PURPLE_CBFLAGS_HALFOP | PURPLE_CBFLAGS_OP | \
                    PURPLE_CBFLAGS_FOUNDER)

/* Pidgin 2's sort_chat_users(): rank, then buddies, then alias. */
static int
chat_user_compare(gconstpointer a, gconstpointer b, gpointer data)
{
	const PidginChatUser *u1 = a, *u2 = b;
	int f1 = u1->flags & RANK_FLAGS, f2 = u2->flags & RANK_FLAGS;

	if (f1 != f2)
		return f1 > f2 ? -1 : 1;
	if (u1->buddy != u2->buddy)
		return u1->buddy ? -1 : 1;
	return g_strcmp0(u1->key, u2->key);
}

static const char *
chat_user_icon(PurpleConvChatBuddyFlags flags)
{
	if (flags & PURPLE_CBFLAGS_FOUNDER)
		return "pidgin-emblem-founder";
	if (flags & PURPLE_CBFLAGS_OP)
		return "pidgin-emblem-operator";
	if (flags & PURPLE_CBFLAGS_HALFOP)
		return "pidgin-emblem-half-operator";
	if (flags & PURPLE_CBFLAGS_VOICE)
		return "pidgin-emblem-voice";
	if (flags & PURPLE_CBFLAGS_TYPING)
		return "pidgin-mood-typing";
	if (flags & PURPLE_CBFLAGS_AWAY)
		return "pidgin-status-away";
	return NULL;
}

static void
chat_user_remove(PidginConversation *gtkconv, const char *name)
{
	PidginChatPane *pane = gtkconv->u.chat;
	PidginChatUser *u;
	guint pos;
	char *norm = g_strdup(purple_normalize(gtkconv->active_conv->account, name));

	u = g_hash_table_lookup(pane->by_name, norm);
	if (u != NULL && g_list_store_find(pane->users, u, &pos))
		g_list_store_remove(pane->users, pos);
	g_hash_table_remove(pane->by_name, norm);
	g_free(norm);
}

static void
chat_user_add(PidginConversation *gtkconv, PurpleConvChatBuddy *cb)
{
	PurpleConversation *conv = gtkconv->active_conv;
	PidginChatPane *pane = gtkconv->u.chat;
	PidginChatUser *u;
	const char *name = purple_conv_chat_cb_get_name(cb);
	const char *alias = cb->alias ? cb->alias : name;

	chat_user_remove(gtkconv, name);

	u = g_object_new(PIDGIN_TYPE_CHAT_USER, NULL);
	u->name = g_strdup(name);
	u->alias = g_strdup(alias);
	u->key = g_utf8_collate_key(alias, -1);
	u->flags = cb->flags;
	u->buddy = cb->buddy;
	u->ignored = purple_conv_chat_is_user_ignored(PURPLE_CONV_CHAT(conv), name);
	g_hash_table_insert(pane->by_name, g_strdup(purple_normalize(conv->account, name)), u);
	g_list_store_insert_sorted(pane->users, u, chat_user_compare, NULL);
	g_object_unref(u);
}

static void
chat_users_update_count(PidginConversation *gtkconv)
{
	guint n = g_list_model_get_n_items(G_LIST_MODEL(gtkconv->u.chat->users));
	char *text = g_strdup_printf(ngettext("%d person in room", "%d people in room", n), n);

	gtk_label_set_text(GTK_LABEL(gtkconv->u.chat->count), text);
	g_free(text);
}

static void
userlist_setup_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	GtkWidget *image = gtk_image_new();
	GtkWidget *label = gtk_label_new(NULL);

	gtk_widget_set_size_request(image, 16, 16);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_box_append(GTK_BOX(box), image);
	gtk_box_append(GTK_BOX(box), label);
	gtk_list_item_set_child(item, box);
}

static void
userlist_bind_cb(GtkSignalListItemFactory *f, GtkListItem *item, PidginConversation *gtkconv)
{
	PidginChatUser *u = gtk_list_item_get_item(item);
	GtkWidget *box = gtk_list_item_get_child(item);
	GtkWidget *image = gtk_widget_get_first_child(box);
	GtkWidget *label = gtk_widget_get_next_sibling(image);
	PurpleConversation *conv = gtkconv->active_conv;
	PangoAttrList *attrs = pango_attr_list_new();
	const char *nick = purple_conv_chat_get_nick(PURPLE_CONV_CHAT(conv));
	gboolean is_me = same_nick(conv->account, u->name, nick);

	gtk_image_set_from_icon_name(GTK_IMAGE(image), chat_user_icon(u->flags));
	gtk_label_set_text(GTK_LABEL(label), u->alias);

	if (!is_me && !u->ignored) {
		/* The message view's nick colours. */
		GdkRGBA fg, bg, color;

		gtk_widget_get_color(label, &fg);
		if (0.3 * fg.red + 0.59 * fg.green + 0.11 * fg.blue > 0.5)
			bg = (GdkRGBA){ 0.14, 0.14, 0.14, 1 };
		else
			bg = (GdkRGBA){ 1, 1, 1, 1 };
		pidgin_nick_color_get(account_is_jabber(conv->account) ? PIDGIN_NICK_COLOR_XEP0392
		                                                       : PIDGIN_NICK_COLOR_PIDGIN,
		                      u->name, &bg, &color);
		pango_attr_list_insert(attrs, pango_attr_foreground_new(color.red * 65535,
		                       color.green * 65535, color.blue * 65535));
	}
	if (u->buddy || is_me)
		pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
	if (u->ignored)
		pango_attr_list_insert(attrs, pango_attr_strikethrough_new(TRUE));
	gtk_label_set_attributes(GTK_LABEL(label), attrs);
	pango_attr_list_unref(attrs);
	gtk_widget_set_tooltip_text(box, purple_strequal(u->name, u->alias) ? NULL : u->name);
}

static char *
chat_real_name(PurpleConversation *conv, const char *who)
{
	PurpleConnection *gc = purple_conversation_get_gc(conv);
	PurplePluginProtocolInfo *prpl_info = conv_prpl_info(conv);
	char *real = NULL;

	if (gc != NULL && prpl_info != NULL && prpl_info->get_cb_real_name)
		real = prpl_info->get_cb_real_name(gc, purple_conv_chat_get_id(PURPLE_CONV_CHAT(conv)), who);
	return real ? real : g_strdup(who);
}

static void
chat_do_im(PidginConversation *gtkconv, const char *who)
{
	PurpleConversation *conv = gtkconv->active_conv;
	char *real;

	if (who == NULL || purple_conversation_get_gc(conv) == NULL)
		return;
	real = chat_real_name(conv, who);
	pidgin_dialogs_im_with_user(purple_conversation_get_account(conv), real);
	g_free(real);
}

static void
userlist_activate_cb(GtkListView *list, guint position, PidginConversation *gtkconv)
{
	PidginChatUser *u = g_list_model_get_item(G_LIST_MODEL(gtkconv->u.chat->users), position);

	if (u == NULL)
		return;
	/* Plugins may handle a click on a nick (Pidgin 2: chat-nick-clicked). */
	if (!purple_signal_emit_return_1(pidgin_conversations_get_handle(), "chat-nick-clicked",
	                                 gtkconv->active_conv, u->name, 1))
		chat_do_im(gtkconv, u->name);
	g_object_unref(u);
}

static void
user_act_im(GSimpleAction *a, GVariant *p, gpointer data)
{
	PidginConversation *gtkconv = data;

	chat_do_im(gtkconv, gtkconv->u.chat->menu_who);
}

static void
user_act_send_file(GSimpleAction *a, GVariant *p, gpointer data)
{
	PidginConversation *gtkconv = data;
	PurpleConnection *gc = purple_conversation_get_gc(gtkconv->active_conv);
	char *real;

	if (gc == NULL || gtkconv->u.chat->menu_who == NULL)
		return;
	real = chat_real_name(gtkconv->active_conv, gtkconv->u.chat->menu_who);
	serv_send_file(gc, real, NULL);
	g_free(real);
}

static void
user_act_ignore(GSimpleAction *a, GVariant *p, gpointer data)
{
	PidginConversation *gtkconv = data;
	PurpleConvChat *chat = PURPLE_CONV_CHAT(gtkconv->active_conv);
	const char *who = gtkconv->u.chat->menu_who;
	PurpleConvChatBuddy *cb;

	if (who == NULL)
		return;
	if (purple_conv_chat_is_user_ignored(chat, who))
		purple_conv_chat_unignore(chat, who);
	else
		purple_conv_chat_ignore(chat, who);
	if ((cb = purple_conv_chat_cb_find(chat, who)) != NULL)
		chat_user_add(gtkconv, cb);
}

static void
user_act_info(GSimpleAction *a, GVariant *p, gpointer data)
{
	PidginConversation *gtkconv = data;
	PurpleConversation *conv = gtkconv->active_conv;
	PurpleConnection *gc = purple_conversation_get_gc(conv);
	PurplePluginProtocolInfo *prpl_info = conv_prpl_info(conv);
	const char *who = gtkconv->u.chat->menu_who;

	if (gc == NULL || who == NULL || prpl_info == NULL)
		return;
	if (prpl_info->get_cb_info != NULL) {
		prpl_info->get_cb_info(gc, purple_conv_chat_get_id(PURPLE_CONV_CHAT(conv)), who);
	} else {
		char *real = chat_real_name(conv, who);
		serv_get_info(gc, real);
		g_free(real);
	}
}

static void
user_act_away(GSimpleAction *a, GVariant *p, gpointer data)
{
	PidginConversation *gtkconv = data;
	PurpleConversation *conv = gtkconv->active_conv;
	PurpleConnection *gc = purple_conversation_get_gc(conv);
	PurplePluginProtocolInfo *prpl_info = conv_prpl_info(conv);

	if (gc != NULL && prpl_info != NULL && prpl_info->get_cb_away != NULL &&
	    gtkconv->u.chat->menu_who != NULL)
		prpl_info->get_cb_away(gc, purple_conv_chat_get_id(PURPLE_CONV_CHAT(conv)),
		                       gtkconv->u.chat->menu_who);
}

static void
user_act_add_remove(GSimpleAction *a, GVariant *p, gpointer data)
{
	PidginConversation *gtkconv = data;
	PurpleAccount *account = purple_conversation_get_account(gtkconv->active_conv);
	const char *who = gtkconv->u.chat->menu_who;
	PurpleBuddy *b;

	if (who == NULL)
		return;
	b = purple_find_buddy(account, who);
	if (b != NULL)
		pidgin_dialogs_remove_buddy(b);
	else if (purple_account_is_connected(account))
		purple_blist_request_add_buddy(account, who, NULL, NULL);
}

static void
user_act_command(GSimpleAction *a, GVariant *p, gpointer data)
{
	PidginConversation *gtkconv = data;
	const char *cmd = g_variant_get_string(p, NULL);
	char *line, *error = NULL;

	if (gtkconv->u.chat->menu_who == NULL)
		return;
	line = g_strdup_printf("%s %s", cmd, gtkconv->u.chat->menu_who);
	if (purple_cmd_do_command(gtkconv->active_conv, line, line, &error) != PURPLE_CMD_STATUS_OK)
		purple_conversation_write(gtkconv->active_conv, NULL,
			error ? error : _("Your command failed for an unknown reason."),
			PURPLE_MESSAGE_ERROR | PURPLE_MESSAGE_NO_LOG, time(NULL));
	g_free(error);
	g_free(line);
}

static const GActionEntry user_actions[] = {
	{ .name = "im", .activate = user_act_im },
	{ .name = "send-file", .activate = user_act_send_file },
	{ .name = "ignore", .activate = user_act_ignore },
	{ .name = "info", .activate = user_act_info },
	{ .name = "away", .activate = user_act_away },
	{ .name = "add-remove", .activate = user_act_add_remove },
	{ .name = "command", .activate = user_act_command, .parameter_type = "s" },
};

static GMenu *
build_user_menu(PidginConversation *gtkconv, const char *who, GSimpleActionGroup **node_group)
{
	PurpleConversation *conv = gtkconv->active_conv;
	PurpleConvChat *chat = PURPLE_CONV_CHAT(conv);
	PurpleConnection *gc = purple_conversation_get_gc(conv);
	PurplePluginProtocolInfo *prpl_info = conv_prpl_info(conv);
	GMenu *menu = g_menu_new(), *section;
	gboolean is_me = same_nick(conv->account, who, chat->nick);
	PurpleBuddy *buddy = purple_find_buddy(conv->account, who);
	static const struct { const char *cmd, *label; } mod_cmds[] = {
		{ "op", N_("_Op") }, { "deop", N_("_Deop") },
		{ "voice", N_("_Voice") }, { "devoice", N_("De_voice") },
		{ "kick", N_("_Kick") }, { "ban", N_("_Ban") },
	};
	GList *cmds;
	gsize i;

	*node_group = NULL;
	section = g_menu_new();
	if (!is_me) {
		g_menu_append(section, _("IM"), "user.im");
		if (prpl_info && prpl_info->send_file) {
			char *real = chat_real_name(conv, who);

			if (gc != NULL && (!prpl_info->can_receive_file || prpl_info->can_receive_file(gc, real)))
				g_menu_append(section, _("Send File"), "user.send-file");
			g_free(real);
		}
		g_menu_append(section, purple_conv_chat_is_user_ignored(chat, who)
		                       ? _("Un-Ignore") : _("Ignore"), "user.ignore");
	}
	if (prpl_info && (prpl_info->get_info || prpl_info->get_cb_info))
		g_menu_append(section, _("Info"), "user.info");
	if (prpl_info && prpl_info->get_cb_away)
		g_menu_append(section, _("Get Away Message"), "user.away");
	if (!is_me && prpl_info && !(prpl_info->options & OPT_PROTO_UNIQUE_CHATNAME) &&
	    prpl_info->add_buddy != NULL)
		g_menu_append(section, (buddy && PURPLE_BLIST_NODE_IS_VISIBLE(buddy))
		                       ? _("Remove") : _("Add"), "user.add-remove");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);

	/* Moderation, through the prpl's own commands where it has them. */
	if (!is_me && gc != NULL) {
		section = g_menu_new();
		cmds = purple_cmd_list(conv);
		for (i = 0; i < G_N_ELEMENTS(mod_cmds); i++) {
			if (g_list_find_custom(cmds, mod_cmds[i].cmd, (GCompareFunc)g_strcmp0)) {
				GMenuItem *item = g_menu_item_new(_(mod_cmds[i].label), NULL);

				g_menu_item_set_action_and_target(item, "user.command", "s", mod_cmds[i].cmd);
				g_menu_append_item(section, item);
				g_object_unref(item);
			}
		}
		g_list_free(cmds);
		if (g_menu_model_get_n_items(G_MENU_MODEL(section)) > 0)
			g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
		g_object_unref(section);
	}

	/* The buddy's own menu (prpl and plugin items), as Pidgin 2. */
	if (buddy != NULL) {
		GMenu *bmenu = g_menu_new();

		*node_group = g_simple_action_group_new();
		pidgin_blist_build_node_menu((PurpleBlistNode *)buddy, bmenu, *node_group);
		g_menu_append_section(menu, NULL, G_MENU_MODEL(bmenu));
		g_object_unref(bmenu);
	}
	return menu;
}

static void
show_user_menu(PidginConversation *gtkconv, PidginChatUser *u, GtkWidget *relative,
               double x, double y)
{
	PidginChatPane *pane = gtkconv->u.chat;
	GSimpleActionGroup *node_group = NULL;
	GMenu *menu;
	GdkRectangle rect = { (int)x, (int)y, 1, 1 };

	g_free(pane->menu_who);
	pane->menu_who = g_strdup(u->name);
	menu = build_user_menu(gtkconv, u->name, &node_group);

	if (pane->menu == NULL) {
		pane->menu = gtk_popover_menu_new_from_model(NULL);
		gtk_widget_set_parent(pane->menu, pane->userlist_box);
		gtk_popover_set_has_arrow(GTK_POPOVER(pane->menu), FALSE);
		gtk_widget_set_halign(pane->menu, GTK_ALIGN_START);
	}
	gtk_popover_menu_set_menu_model(GTK_POPOVER_MENU(pane->menu), G_MENU_MODEL(menu));
	gtk_widget_insert_action_group(pane->menu, "node", G_ACTION_GROUP(node_group));
	if (relative != NULL && relative != pane->userlist_box) {
		graphene_point_t pt;

		if (gtk_widget_compute_point(relative, pane->userlist_box,
		                             &GRAPHENE_POINT_INIT((float)x, (float)y), &pt)) {
			rect.x = (int)pt.x;
			rect.y = (int)pt.y;
		}
	}
	gtk_popover_set_pointing_to(GTK_POPOVER(pane->menu), &rect);
	gtk_popover_popup(GTK_POPOVER(pane->menu));
	g_object_unref(menu);
	g_clear_object(&node_group);
}

static void
userlist_pressed_cb(GtkGestureClick *gesture, int n, double x, double y,
                    PidginConversation *gtkconv)
{
	GtkWidget *list = gtkconv->u.chat->list;
	GtkWidget *picked = gtk_widget_pick(list, x, y, GTK_PICK_DEFAULT);
	GListModel *model = G_LIST_MODEL(gtkconv->u.chat->users);
	guint i, count = g_list_model_get_n_items(model);

	/* Find the row under the pointer: the child whose item is bound. */
	for (; picked != NULL && picked != list; picked = gtk_widget_get_parent(picked)) {
		PidginChatUser *u = g_object_get_data(G_OBJECT(picked), "pidgin-chat-user");

		if (u != NULL) {
			for (i = 0; i < count; i++) {
				PidginChatUser *c = g_list_model_get_item(model, i);
				g_object_unref(c);
				if (c == u) {
					gtk_selection_model_select_item(
						gtk_list_view_get_model(GTK_LIST_VIEW(list)), i, TRUE);
					break;
				}
			}
			show_user_menu(gtkconv, u, list, x, y);
			gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
			return;
		}
	}
}

static void
userlist_bind_data_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data)
{
	g_object_set_data(G_OBJECT(gtk_list_item_get_child(item)), "pidgin-chat-user",
	                  gtk_list_item_get_item(item));
}

static void
userlist_unbind_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data)
{
	g_object_set_data(G_OBJECT(gtk_list_item_get_child(item)), "pidgin-chat-user", NULL);
}

static void
topic_activate_cb(GtkEntry *entry, PidginConversation *gtkconv)
{
	PurpleConversation *conv = gtkconv->active_conv;
	PurpleConnection *gc = purple_conversation_get_gc(conv);
	PurplePluginProtocolInfo *prpl_info = conv_prpl_info(conv);
	const char *topic = gtk_editable_get_text(GTK_EDITABLE(entry));

	if (gc == NULL || prpl_info == NULL || prpl_info->set_chat_topic == NULL)
		return;
	if (purple_strequal(topic, purple_conv_chat_get_topic(PURPLE_CONV_CHAT(conv))))
		return;
	prpl_info->set_chat_topic(gc, purple_conv_chat_get_id(PURPLE_CONV_CHAT(conv)), topic);
	gtk_widget_grab_focus(gtkconv->entry);
}

static void
update_topic(PidginConversation *gtkconv)
{
	PurpleConversation *conv = gtkconv->active_conv;
	PurplePluginProtocolInfo *prpl_info = conv_prpl_info(conv);
	const char *topic = purple_conv_chat_get_topic(PURPLE_CONV_CHAT(conv));
	char *plain = topic ? purple_markup_strip_html(topic) : NULL;
	GtkWidget *entry = gtkconv->u.chat->topic_text;

	if (entry == NULL)
		return;
	gtk_widget_set_visible(entry, prpl_info != NULL &&
	                       (prpl_info->options & OPT_PROTO_CHAT_TOPIC));
	gtk_editable_set_text(GTK_EDITABLE(entry), plain ? plain : "");
	gtk_widget_set_tooltip_text(entry, plain);
	gtk_editable_set_editable(GTK_EDITABLE(entry), prpl_info && prpl_info->set_chat_topic);
	g_free(plain);
	update_infopane(gtkconv);
}

/* Nick completion (Tab), a simpler tab_complete(). */
static gboolean
tab_complete(PidginConversation *gtkconv)
{
	GtkTextBuffer *buffer = gtkconv->entry_buffer;
	GtkTextIter cursor, word_start, start;
	char *entered, *prefix = NULL;
	GListModel *model = G_LIST_MODEL(gtkconv->u.chat->users);
	GPtrArray *matches;
	guint i, n;
	gboolean at_start;

	if (purple_signal_emit_return_1(pidgin_conversations_get_handle(),
	                                "chat-nick-autocomplete", gtkconv->active_conv, FALSE))
		return TRUE;

	gtk_text_buffer_get_iter_at_mark(buffer, &cursor, gtk_text_buffer_get_insert(buffer));
	word_start = cursor;
	while (!gtk_text_iter_starts_line(&word_start)) {
		GtkTextIter prev = word_start;

		gtk_text_iter_backward_char(&prev);
		if (g_unichar_isspace(gtk_text_iter_get_char(&prev)))
			break;
		word_start = prev;
	}
	entered = gtk_text_buffer_get_text(buffer, &word_start, &cursor, FALSE);
	if (*entered == '\0') {
		g_free(entered);
		return FALSE;
	}
	gtk_text_buffer_get_start_iter(buffer, &start);
	at_start = gtk_text_iter_equal(&start, &word_start);

	matches = g_ptr_array_new();
	n = g_list_model_get_n_items(model);
	for (i = 0; i < n; i++) {
		PidginChatUser *u = g_list_model_get_item(model, i);
		const char *nick = u->alias ? u->alias : u->name;

		if (g_ascii_strncasecmp(nick, entered, strlen(entered)) == 0)
			g_ptr_array_add(matches, (gpointer)nick);
		else if (g_ascii_strncasecmp(u->name, entered, strlen(entered)) == 0)
			g_ptr_array_add(matches, u->name);
		g_object_unref(u);   /* the store keeps it */
	}

	if (matches->len == 0) {
		g_ptr_array_free(matches, TRUE);
		g_free(entered);
		return TRUE;
	}

	if (matches->len == 1) {
		prefix = g_strdup_printf("%s%s", (char *)matches->pdata[0], at_start ? ": " : " ");
	} else {
		/* The longest common prefix, and the candidates shown. */
		GString *list = g_string_new(NULL);
		size_t len = strlen(matches->pdata[0]);

		for (i = 1; i < matches->len; i++) {
			size_t j = 0;
			const char *a = matches->pdata[0], *b = matches->pdata[i];

			while (j < len && a[j] && b[j] && g_ascii_tolower(a[j]) == g_ascii_tolower(b[j]))
				j++;
			len = j;
		}
		prefix = g_strndup(matches->pdata[0], len);
		for (i = 0; i < matches->len; i++) {
			char *esc = g_markup_escape_text(matches->pdata[i], -1);
			g_string_append_printf(list, "%s%s", i ? " " : "", esc);
			g_free(esc);
		}
		purple_conversation_write(gtkconv->active_conv, "", list->str, PURPLE_MESSAGE_NO_LOG,
		                          time(NULL));
		g_string_free(list, TRUE);
	}

	if (strlen(prefix) >= strlen(entered)) {
		gtk_text_buffer_delete(buffer, &word_start, &cursor);
		gtk_text_buffer_insert(buffer, &word_start, prefix, -1);
	}
	g_free(prefix);
	g_ptr_array_free(matches, TRUE);
	g_free(entered);
	return TRUE;
}

static gboolean
entry_key_cb(GtkEventControllerKey *ctl, guint keyval, guint keycode,
             GdkModifierType state, PidginConversation *gtkconv)
{
	if ((keyval == GDK_KEY_Tab || keyval == GDK_KEY_KP_Tab) &&
	    !(state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_ALT_MASK)) && is_chat(gtkconv))
		return tab_complete(gtkconv);
	if (keyval == GDK_KEY_Escape && gtk_widget_get_visible(gtkconv->banner)) {
		banner_cancel_cb(NULL, gtkconv);
		return TRUE;
	}
	return FALSE;
}

/**************************************************************************
 * Images: paste and drop (one rule)
 *
 * A pasted image and a dropped image file go the same way
 * (pidgin_conv_offer_image()):
 *  (a) inline in the message, if the conversation takes inline images
 *      (an IM on a prpl with OPT_PROTO_IM_IMAGE whose connection doesn't
 *      say PURPLE_CONNECTION_NO_IMAGES: what enables Insert Image);
 *  (b) else as a file transfer, if the prpl can send this conversation a
 *      file (IM: send_file and can_receive_file; chat: chat_send_file and
 *      chat_can_receive_file: what enables Send File). XMPP sends it by
 *      HTTP upload. A pasted image is saved first as
 *      <profile>/pidgin4/paste/pasted-<time>.png (or .jpg: pasted and
 *      dropped image data is encoded by the /pidgin4/images/paste_format
 *      and paste_jpeg_quality prefs, see pidginimageencode.h), deleted when the
 *      transfer completes or is cancelled (and, for transfers that never
 *      end, at the next start once a day old); a dropped file is sent as
 *      it is;
 *  (c) else it is not taken: a paste pastes the clipboard's text, and a
 *      drop offers, as Pidgin 2 did, to make it the buddy icon (IMs with
 *      the buddy on the list), or does nothing.
 **************************************************************************/

#define PASTE_MAX_AGE (24 * 60 * 60)

static ImageOffer
image_offer(PidginConversation *gtkconv)
{
	PurpleConversation *conv = gtkconv->active_conv;

	if (purple_conversation_get_gc(conv) == NULL)
		return IMAGE_OFFER_NONE;
	if (pidgin_conv_action_enabled(gtkconv, "insert-image") &&
	    !(conv->features & PURPLE_CONNECTION_NO_IMAGES))
		return IMAGE_OFFER_INLINE;
	if (pidgin_conv_action_enabled(gtkconv, "send-file"))
		return IMAGE_OFFER_FILE;
	return IMAGE_OFFER_NONE;
}

static char *
paste_dir(void)
{
	return g_build_filename(purple_user_dir(), "pidgin4", "paste", NULL);
}

static gboolean
is_paste_file(const char *path)
{
	char *dir = paste_dir();
	char *parent = path ? g_path_get_dirname(path) : NULL;
	gboolean ret = parent != NULL && purple_strequal(parent, dir);

	g_free(parent);
	g_free(dir);
	return ret;
}

static gboolean
unlink_paste_file(gpointer data)
{
	if (g_unlink(data) == 0)
		purple_debug_info("gtkconv", "removed the pasted image %s\n", (char *)data);
	return G_SOURCE_REMOVE;
}

/* file-send-complete / file-send-cancel: a pasted image's transfer ended */
static void
paste_xfer_done_cb(PurpleXfer *xfer, gpointer data)
{
	const char *path = purple_xfer_get_local_filename(xfer);

	/* after the prpl's own handlers, which may still close the file */
	if (is_paste_file(path))
		g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, unlink_paste_file, g_strdup(path), g_free);
}

/* Pasted images whose transfer never ended (the prpl never made one, or
 * pidgin4 quit first), once a day old. */
static void
paste_dir_cleanup(void)
{
	char *dir = paste_dir();
	GDir *d = g_dir_open(dir, 0, NULL);
	const char *name;
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;

	while (d != NULL && (name = g_dir_read_name(d)) != NULL) {
		char *path = g_build_filename(dir, name, NULL);
		GStatBuf st;

		if (g_stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
		    now - (gint64)st.st_mtime > PASTE_MAX_AGE)
			g_unlink(path);
		g_free(path);
	}
	if (d != NULL)
		g_dir_close(d);
	g_free(dir);
}

/* <profile>/pidgin4/paste/<filename>, made unique */
static char *
save_paste_file(GBytes *data, const char *filename)
{
	char *dir = paste_dir();
	char *base = g_path_get_basename(filename);
	const char *dot = strrchr(base, '.');
	char *stem = g_strndup(base, dot ? (gsize)(dot - base) : strlen(base));
	char *path = g_build_filename(dir, base, NULL);
	GError *error = NULL;
	int i;

	g_mkdir_with_parents(dir, 0700);
	for (i = 2; g_file_test(path, G_FILE_TEST_EXISTS) && i < 1000; i++) {
		char *name = g_strdup_printf("%s-%d%s", stem, i, dot ? dot : "");

		g_free(path);
		path = g_build_filename(dir, name, NULL);
		g_free(name);
	}
	if (!g_file_set_contents(path, g_bytes_get_data(data, NULL), g_bytes_get_size(data),
	                         &error)) {
		purple_debug_error("gtkconv", "saving the pasted image: %s\n", error->message);
		g_error_free(error);
		g_clear_pointer(&path, g_free);
	}
	g_free(stem);
	g_free(base);
	g_free(dir);
	return path;
}

static void
send_file_now(PidginConversation *gtkconv, const char *path)
{
	PurpleConversation *conv = gtkconv->active_conv;
	PurpleConnection *gc = purple_conversation_get_gc(conv);

	if (gc == NULL)
		return;
	if (is_chat(gtkconv))
		serv_chat_send_file(gc, purple_conv_chat_get_id(PURPLE_CONV_CHAT(conv)), path);
	else
		serv_send_file(gc, purple_conversation_get_name(conv), path);
}

/* The rule above. @source_path: the dropped file (sent as it is, and read
 * only when inlined), or NULL for @data. */
static gboolean
offer_image(PidginConversation *gtkconv, GBytes *data, const char *filename,
            const char *source_path)
{
	PurpleConversation *conv = gtkconv->active_conv;
	ImageOffer offer = image_offer(gtkconv);

	if (offer == IMAGE_OFFER_INLINE) {
		GBytes *bytes = data ? g_bytes_ref(data) : NULL;
		gsize len;
		int id;

		if (bytes == NULL) {
			char *contents = NULL;

			if (!g_file_get_contents(source_path, &contents, &len, NULL))
				return FALSE;
			bytes = g_bytes_new_take(contents, len);
		}
		len = g_bytes_get_size(bytes);
		id = purple_imgstore_add_with_id(g_memdup2(g_bytes_get_data(bytes, NULL), len), len,
		                                 filename);
		g_bytes_unref(bytes);
		if (id == 0)
			return FALSE;
		pidgin_compose_entry_insert_image(conv_entry(gtkconv), id);
		purple_imgstore_unref_by_id(id);    /* the entry holds its own */
		gtk_widget_grab_focus(gtkconv->entry);
		return TRUE;
	}

	if (offer == IMAGE_OFFER_FILE) {
		char *path = source_path ? g_strdup(source_path) : save_paste_file(data, filename);
		char *base, *msg;

		if (path == NULL)
			return FALSE;
		base = g_markup_escape_text(filename, -1);
		msg = g_strdup_printf(_("Sending the image %s as a file."), base);
		purple_conversation_write(conv, NULL, msg,
		                          PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG, time(NULL));
		g_free(msg);
		g_free(base);
		send_file_now(gtkconv, path);
		g_free(path);
		return TRUE;
	}
	return FALSE;
}

gboolean
pidgin_conv_offer_image(PidginConversation *gtkconv, GBytes *png, const char *filename)
{
	g_return_val_if_fail(gtkconv != NULL, FALSE);
	g_return_val_if_fail(png != NULL && filename != NULL, FALSE);

	return offer_image(gtkconv, png, filename, NULL);
}

/* A pasted (or dropped) image's data: encoded by the paste_format and
 * paste_jpeg_quality prefs (pidginimageencode.c), named
 * pasted-<time>.png or .jpg. */
static gboolean
offer_texture(PidginConversation *gtkconv, GdkTexture *texture)
{
	const char *ext = "png";
	GBytes *data;
	GDateTime *now;
	char *stamp, *filename;
	gboolean ret;

	if (image_offer(gtkconv) == IMAGE_OFFER_NONE)
		return FALSE;
	data = pidgin_image_encode_for_paste(texture, &ext);
	now = g_date_time_new_now_local();
	stamp = g_date_time_format(now, "%Y%m%d-%H%M%S");
	filename = g_strdup_printf("pasted-%s.%s", stamp, ext);
	ret = pidgin_conv_offer_image(gtkconv, data, filename);
	g_free(filename);
	g_free(stamp);
	g_date_time_unref(now);
	g_bytes_unref(data);
	return ret;
}

/* The compose entry's "paste-image" */
static gboolean
entry_paste_image_cb(PidginComposeEntry *entry, GdkTexture *texture,
                     PidginConversation *gtkconv)
{
	return offer_texture(gtkconv, texture);
}

/* A dropped image file (its name, not its path, in the imgstore). */
static gboolean
offer_image_path(PidginConversation *gtkconv, const char *path)
{
	char *base = g_path_get_basename(path);
	gboolean ret = offer_image(gtkconv, NULL, base, path);

	g_free(base);
	return ret;
}

/**************************************************************************
 * Drag and drop
 **************************************************************************/

typedef struct
{
	PidginConversation *gtkconv;
	char *path;
} IconDrop;

static void
icon_drop_cb(GObject *source, GAsyncResult *res, gpointer data)
{
	IconDrop *drop = data;
	int button = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), res, NULL);
	PurpleConversation *conv = NULL;
	GList *l;

	/* The conversation may be gone by now. */
	for (l = purple_get_conversations(); l != NULL; l = l->next)
		if (PIDGIN_CONVERSATION((PurpleConversation *)l->data) == drop->gtkconv)
			conv = l->data;

	if (conv != NULL && button == 0) {
		PurpleBuddy *buddy = purple_find_buddy(purple_conversation_get_account(conv),
		                                       purple_conversation_get_name(conv));

		if (buddy != NULL)
			purple_buddy_icons_node_set_custom_icon_from_file(
				(PurpleBlistNode *)purple_buddy_get_contact(buddy), drop->path);
	}
	g_free(drop->path);
	g_free(drop);
}

static void
send_file_to(PidginConversation *gtkconv, const char *path)
{
	PurpleConversation *conv = gtkconv->active_conv;
	PurpleConnection *gc = purple_conversation_get_gc(conv);
	const char *who = purple_conversation_get_name(conv);
	char *type;

	if (gc == NULL || conv_prpl_info(conv) == NULL)
		return;

	type = g_content_type_guess(path, NULL, 0, NULL);
	if (type == NULL || !g_content_type_is_a(type, "image/*")) {
		send_file_now(gtkconv, path);
	} else if (offer_image_path(gtkconv, path)) {
		/* images: as a paste (see "Images: paste and drop") */
	} else if (!is_chat(gtkconv) &&
	           purple_find_buddy(purple_conversation_get_account(conv), who) != NULL) {
		/* Neither inline nor as a file: Pidgin 2's third choice. */
		GtkAlertDialog *dialog = gtk_alert_dialog_new(_("You have dragged an image"));
		IconDrop *drop = g_new0(IconDrop, 1);
		GCancellable *cancel;
		const char *buttons[] = { _("Set as Buddy Icon"), _("Cancel"), NULL };

		gtk_alert_dialog_set_detail(dialog, _("Would you like to set it as the buddy "
			"icon for this user?"));
		gtk_alert_dialog_set_buttons(dialog, buttons);
		gtk_alert_dialog_set_cancel_button(dialog, 1);
		drop->gtkconv = gtkconv;
		drop->path = g_strdup(path);
		cancel = g_cancellable_new();
		/* the latest one, for the selftest */
		g_object_set_data_full(G_OBJECT(gtkconv->tab_cont), "pidgin-image-drop-dialog",
		                       g_object_ref(dialog), g_object_unref);
		g_object_set_data_full(G_OBJECT(gtkconv->tab_cont), "pidgin-image-drop-cancel",
		                       g_object_ref(cancel), g_object_unref);
		gtk_alert_dialog_choose(dialog, GTK_WINDOW(gtk_widget_get_root(gtkconv->tab_cont)),
		                        cancel, icon_drop_cb, drop);
		g_object_unref(cancel);
		g_object_unref(dialog);
	}
	g_free(type);
}

static gboolean
conv_drop_cb(GtkDropTarget *target, const GValue *value, double x, double y,
             PidginConversation *gtkconv)
{
	PurpleConversation *conv = gtkconv->active_conv;

	if (G_VALUE_HOLDS(value, PIDGIN_TYPE_BLIST_NODE_ITEM)) {
		PurpleBlistNode *node = pidgin_blist_node_item_get_node(g_value_get_object(value));
		PurpleBuddy *buddy = NULL;

		if (PURPLE_BLIST_NODE_IS_CONTACT(node))
			buddy = purple_contact_get_priority_buddy((PurpleContact *)node);
		else if (PURPLE_BLIST_NODE_IS_BUDDY(node))
			buddy = (PurpleBuddy *)node;
		if (buddy == NULL)
			return FALSE;
		/* A buddy dropped on a chat is invited; on an IM, opens one. */
		if (is_chat(gtkconv) && purple_buddy_get_account(buddy) == conv->account)
			purple_conv_chat_invite_user(PURPLE_CONV_CHAT(conv), purple_buddy_get_name(buddy),
			                             NULL, TRUE);
		else
			pidgin_dialogs_im_with_user(purple_buddy_get_account(buddy),
			                            purple_buddy_get_name(buddy));
		return TRUE;
	}

	/* Image data (dragged out of a browser or an image viewer without a
	 * file): as a paste. */
	if (G_VALUE_HOLDS(value, GDK_TYPE_TEXTURE))
		return offer_texture(gtkconv, g_value_get_object(value));

	if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
		GSList *files = g_value_get_boxed(value), *l;

		for (l = files; l != NULL; l = l->next) {
			char *path = g_file_get_path(l->data);

			if (path != NULL)
				send_file_to(gtkconv, path);
			g_free(path);
		}
		return TRUE;
	}
	return FALSE;
}

/**************************************************************************
 * Creating and destroying
 **************************************************************************/

static PidginMessageView *
meta_get_view(PurpleConversation *conv)
{
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);

	return (gtkconv != NULL && PIDGIN_IS_PIDGIN_CONVERSATION(conv)) ? conv_view(gtkconv) : NULL;
}

static void
meta_older_done(PurpleConversation *conv, GPtrArray *messages, gboolean complete)
{
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);

	if (gtkconv == NULL || !PIDGIN_IS_PIDGIN_CONVERSATION(conv))
		return;
	gtkconv->history_busy = FALSE;
	gtk_widget_set_visible(gtkconv->history_spinner, FALSE);
	gtk_spinner_set_spinning(GTK_SPINNER(gtkconv->history_spinner), FALSE);
	if (messages->len > 0)
		pidgin_message_view_prepend_many(conv_view(gtkconv), messages);
	if (complete && messages->len == 0)
		gtkconv->history_exhausted = TRUE;
}

static void
meta_seen_elsewhere(PurpleConversation *conv)
{
	pidgin_conv_set_unseen(conv, PIDGIN_UNSEEN_NONE);
}

static const PidginConvMetaUiOps meta_ui_ops = {
	meta_get_view,
	meta_older_done,
	meta_seen_elsewhere,
};

static void
load_older(PidginConversation *gtkconv)
{
	if (gtkconv->history_busy || gtkconv->history_exhausted)
		return;
	gtkconv->history_busy = TRUE;
	gtk_widget_set_visible(gtkconv->history_spinner, TRUE);
	gtk_spinner_set_spinning(GTK_SPINNER(gtkconv->history_spinner), TRUE);
	/* older_done() may run before this returns (the index). */
	if (!pidgin_conv_meta_load_older(gtkconv->active_conv)) {
		gtkconv->history_busy = FALSE;
		gtkconv->history_exhausted = TRUE;
		gtk_widget_set_visible(gtkconv->history_spinner, FALSE);
		gtk_spinner_set_spinning(GTK_SPINNER(gtkconv->history_spinner), FALSE);
	}
}

static void
top_reached_cb(PidginMessageView *view, PidginConversation *gtkconv)
{
	load_older(gtkconv);
}

static GtkWidget *
setup_infopane(PidginConversation *gtkconv)
{
	GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

	gtk_widget_add_css_class(hbox, "pidgin-conv-infopane");
	if (!is_chat(gtkconv)) {
		/* A GtkImage, not a GtkPicture: a picture asks for the icon's own
		 * size (a Steam avatar is 184 px) and a size request is only a
		 * minimum. The image draws any paintable, animated ones too,
		 * within its pixel size, keeping the aspect. */
		GtkWidget *pic = gtk_image_new();

		gtk_image_set_pixel_size(GTK_IMAGE(pic), BUDDY_ICON_SIZE);
		gtk_widget_set_valign(pic, GTK_ALIGN_CENTER);
		gtk_widget_add_css_class(pic, "pidgin-conv-buddy-icon");
		gtk_widget_set_visible(pic, FALSE);
		gtkconv->u.im->icon = pic;
		gtkconv->u.im->icon_container = pic;
		gtk_box_append(GTK_BOX(hbox), pic);
	}

	gtkconv->infopane_name = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(gtkconv->infopane_name), 0);
	gtk_label_set_ellipsize(GTK_LABEL(gtkconv->infopane_name), PANGO_ELLIPSIZE_END);
	gtk_label_set_selectable(GTK_LABEL(gtkconv->infopane_name), TRUE);
	gtkconv->infopane_status = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(gtkconv->infopane_status), 0);
	gtk_label_set_ellipsize(GTK_LABEL(gtkconv->infopane_status), PANGO_ELLIPSIZE_END);
	gtk_widget_add_css_class(gtkconv->infopane_status, "pidgin-conv-status");
	gtk_box_append(GTK_BOX(vbox), gtkconv->infopane_name);
	gtk_box_append(GTK_BOX(vbox), gtkconv->infopane_status);
	gtk_widget_set_hexpand(vbox, TRUE);
	gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(hbox), vbox);
	gtkconv->infopane = vbox;

	gtkconv->history_spinner = gtk_spinner_new();
	gtk_widget_set_visible(gtkconv->history_spinner, FALSE);
	gtk_widget_set_tooltip_text(gtkconv->history_spinner, _("Loading older messages"));
	gtk_box_append(GTK_BOX(hbox), gtkconv->history_spinner);

	gtkconv->infopane_prpl = gtk_image_new();
	gtk_box_append(GTK_BOX(hbox), gtkconv->infopane_prpl);
	gtkconv->infopane_hbox = hbox;
	return hbox;
}

static GtkWidget *
setup_chat_side(PidginConversation *gtkconv)
{
	PidginChatPane *pane = gtkconv->u.chat;
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BOX_SPACE);
	GtkListItemFactory *factory;
	GtkSingleSelection *sel;
	GtkGesture *click;
	GtkWidget *sw;

	pane->users = g_list_store_new(PIDGIN_TYPE_CHAT_USER);
	pane->by_name = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	pane->count = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(pane->count), 0);
	gtk_widget_add_css_class(pane->count, "pidgin-conv-user-count");
	gtk_box_append(GTK_BOX(box), pane->count);

	factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(userlist_setup_cb), NULL);
	g_signal_connect(factory, "bind", G_CALLBACK(userlist_bind_cb), gtkconv);
	g_signal_connect_after(factory, "bind", G_CALLBACK(userlist_bind_data_cb), NULL);
	g_signal_connect(factory, "unbind", G_CALLBACK(userlist_unbind_cb), NULL);
	sel = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(pane->users)));
	gtk_single_selection_set_autoselect(sel, FALSE);
	gtk_single_selection_set_can_unselect(sel, TRUE);
	pane->list = gtk_list_view_new(GTK_SELECTION_MODEL(sel), factory);
	gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(pane->list), FALSE);
	gtk_widget_add_css_class(pane->list, "pidgin-conv-userlist");
	g_signal_connect(pane->list, "activate", G_CALLBACK(userlist_activate_cb), gtkconv);

	click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
	g_signal_connect(click, "pressed", G_CALLBACK(userlist_pressed_cb), gtkconv);
	gtk_widget_add_controller(pane->list, GTK_EVENT_CONTROLLER(click));

	sw = pidgin_make_scrollable(pane->list, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC, -1, -1);
	gtk_widget_set_vexpand(sw, TRUE);
	gtk_box_append(GTK_BOX(box), sw);
	gtk_widget_set_size_request(box,
		MAX(purple_prefs_get_int(CONV_PREFS "/chat/userlist_width"), 120), -1);

	{
		GSimpleActionGroup *group = g_simple_action_group_new();

		g_action_map_add_action_entries(G_ACTION_MAP(group), user_actions,
		                                G_N_ELEMENTS(user_actions), gtkconv);
		gtk_widget_insert_action_group(box, "user", G_ACTION_GROUP(group));
		g_object_unref(group);
	}
	pane->userlist_box = box;
	chat_users_update_count(gtkconv);
	return box;
}

static GtkWidget *
setup_common_pane(PidginConversation *gtkconv)
{
	PurpleConversation *conv = gtkconv->active_conv;
	PurpleAccount *account = purple_conversation_get_account(conv);
	PurpleConnection *gc = purple_account_get_connection(account);
	GtkWidget *vbox, *paned, *compose, *entry = NULL, *toolbar = NULL, *top, *hbox, *button;
	GtkEventController *keys;
	PidginMessageView *view;

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BOX_SPACE);

	gtk_box_append(GTK_BOX(vbox), setup_infopane(gtkconv));

	if (is_chat(gtkconv)) {
		GtkWidget *topic_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
		GtkWidget *label = gtk_label_new(_("Topic:"));

		gtkconv->u.chat->topic_text = gtk_entry_new();
		gtk_widget_set_hexpand(gtkconv->u.chat->topic_text, TRUE);
		g_signal_connect(gtkconv->u.chat->topic_text, "activate",
		                 G_CALLBACK(topic_activate_cb), gtkconv);
		gtk_box_append(GTK_BOX(topic_box), label);
		gtk_box_append(GTK_BOX(topic_box), gtkconv->u.chat->topic_text);
		g_object_bind_property(gtkconv->u.chat->topic_text, "visible", topic_box, "visible",
		                       G_BINDING_SYNC_CREATE);
		gtk_box_append(GTK_BOX(vbox), topic_box);
	}

	/* The message view (with its own find bar) */
	gtkconv->imhtml = pidgin_create_message_view();
	view = conv_view(gtkconv);
	pidgin_message_view_set_conversation(view, conv);
	pidgin_message_view_set_is_chat(view, is_chat(gtkconv));
	g_signal_connect(view, "reaction-toggled", G_CALLBACK(view_reaction_cb), gtkconv);
	g_signal_connect(view, "reply-requested", G_CALLBACK(view_reply_cb), gtkconv);
	g_signal_connect(view, "edit-requested", G_CALLBACK(view_edit_cb), gtkconv);
	g_signal_connect(view, "retract-requested", G_CALLBACK(view_retract_cb), gtkconv);
	g_signal_connect(view, "top-reached", G_CALLBACK(top_reached_cb), gtkconv);

	if (is_chat(gtkconv)) {
		paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
		gtk_paned_set_start_child(GTK_PANED(paned), gtkconv->imhtml);
		gtk_paned_set_resize_start_child(GTK_PANED(paned), TRUE);
		gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
		gtk_paned_set_end_child(GTK_PANED(paned), setup_chat_side(gtkconv));
		gtk_paned_set_resize_end_child(GTK_PANED(paned), FALSE);
		gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
		top = paned;
	} else {
		top = gtkconv->imhtml;
	}
	gtk_widget_set_vexpand(top, TRUE);

	/* The compose area */
	compose = pidgin_create_compose_entry(gc ? gc->flags : 0, TRUE, &entry, &toolbar);
	gtkconv->entry = entry;
	gtkconv->toolbar = toolbar;
	gtkconv->entry_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(entry));
	pidgin_compose_entry_set_return_inserts_newline(conv_entry(gtkconv), FALSE);
	g_signal_connect(entry, "message-send", G_CALLBACK(entry_send_cb), gtkconv);
	g_signal_connect(entry, "typing-changed", G_CALLBACK(typing_changed_cb), gtkconv);
	g_signal_connect(entry, "edit-last-requested", G_CALLBACK(edit_last_cb), gtkconv);
	g_signal_connect(entry, "paste-image", G_CALLBACK(entry_paste_image_cb), gtkconv);
	keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(entry_key_cb), gtkconv);
	gtk_widget_add_controller(entry, keys);
	gtk_widget_set_size_request(compose, -1,
		MAX(purple_prefs_get_int(is_chat(gtkconv) ? CONV_PREFS "/chat/entry_height"
		                                          : CONV_PREFS "/im/entry_height"), 40));
	if (toolbar != NULL)
		gtk_widget_set_visible(toolbar,
			purple_prefs_get_bool(CONV_PREFS "/show_formatting_toolbar"));

	/* Above the entry: the typing line and the reply/edit banner */
	gtkconv->typing_label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(gtkconv->typing_label), 0);
	gtk_widget_add_css_class(gtkconv->typing_label, "pidgin-conv-typing");
	gtk_widget_set_visible(gtkconv->typing_label, FALSE);

	gtkconv->banner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	gtk_widget_add_css_class(gtkconv->banner, "pidgin-conv-banner");
	gtkconv->banner_label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(gtkconv->banner_label), 0);
	gtk_label_set_ellipsize(GTK_LABEL(gtkconv->banner_label), PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand(gtkconv->banner_label, TRUE);
	button = gtk_button_new_from_icon_name("window-close-symbolic");
	gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
	gtk_widget_set_tooltip_text(button, _("Cancel"));
	g_signal_connect(button, "clicked", G_CALLBACK(banner_cancel_cb), gtkconv);
	gtk_box_append(GTK_BOX(gtkconv->banner), gtkconv->banner_label);
	gtk_box_append(GTK_BOX(gtkconv->banner), button);
	gtk_widget_set_visible(gtkconv->banner, FALSE);

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	gtk_widget_set_hexpand(compose, TRUE);
	gtk_box_append(GTK_BOX(hbox), compose);
	gtkconv->send_button = gtk_button_new_with_mnemonic(_("_Send"));
	gtk_widget_set_valign(gtkconv->send_button, GTK_ALIGN_END);
	g_signal_connect(gtkconv->send_button, "clicked", G_CALLBACK(send_button_cb), gtkconv);
	gtk_widget_set_visible(gtkconv->send_button,
	                       purple_prefs_get_bool(CONV4_PREFS "/send_button"));
	gtk_box_append(GTK_BOX(hbox), gtkconv->send_button);
	gtkconv->lower_hbox = hbox;

	/* The view and the compose area share a vertical paned. */
	{
		GtkWidget *lower = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
		GtkWidget *vpaned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);

		gtk_box_append(GTK_BOX(lower), gtkconv->typing_label);
		gtk_box_append(GTK_BOX(lower), gtkconv->banner);
		gtk_box_append(GTK_BOX(lower), hbox);
		gtk_paned_set_start_child(GTK_PANED(vpaned), top);
		gtk_paned_set_end_child(GTK_PANED(vpaned), lower);
		gtk_paned_set_resize_end_child(GTK_PANED(vpaned), FALSE);
		gtk_paned_set_shrink_end_child(GTK_PANED(vpaned), FALSE);
		gtk_paned_set_shrink_start_child(GTK_PANED(vpaned), FALSE);
		gtk_widget_set_vexpand(vpaned, TRUE);
		gtk_box_append(GTK_BOX(vbox), vpaned);
	}

	return vbox;
}

static void
tab_close_clicked_cb(GtkButton *button, PidginConversation *gtkconv)
{
	pidgin_conv_close(gtkconv);
}

static void
tab_middle_click_cb(GtkGestureClick *gesture, int n, double x, double y,
                    PidginConversation *gtkconv)
{
	pidgin_conv_close(gtkconv);
}

static GtkWidget *
setup_tab_label(PidginConversation *gtkconv)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	GtkGesture *middle;

	gtkconv->icon = gtk_image_new();
	gtkconv->tab_label = gtk_label_new(NULL);
	gtk_label_set_ellipsize(GTK_LABEL(gtkconv->tab_label), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(gtkconv->tab_label), 20);
	gtk_widget_set_hexpand(gtkconv->tab_label, TRUE);
	gtk_widget_add_css_class(gtkconv->tab_label, "pidgin-tab-label");
	gtkconv->close = gtk_button_new_from_icon_name("window-close-symbolic");
	gtk_button_set_has_frame(GTK_BUTTON(gtkconv->close), FALSE);
	gtk_widget_set_focus_on_click(gtkconv->close, FALSE);
	gtk_widget_add_css_class(gtkconv->close, "pidgin-tab-close");
	gtk_widget_set_tooltip_text(gtkconv->close, _("Close conversation"));
	g_signal_connect(gtkconv->close, "clicked", G_CALLBACK(tab_close_clicked_cb), gtkconv);
	gtk_box_append(GTK_BOX(box), gtkconv->icon);
	gtk_box_append(GTK_BOX(box), gtkconv->tab_label);
	gtk_box_append(GTK_BOX(box), gtkconv->close);

	middle = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(middle), GDK_BUTTON_MIDDLE);
	g_signal_connect(middle, "released", G_CALLBACK(tab_middle_click_cb), gtkconv);
	gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(middle));
	return box;
}

static void
private_gtkconv_new(PurpleConversation *conv, gboolean hidden)
{
	PidginConversation *gtkconv;
	PurpleBlistNode *node;
	PurpleValue *value;
	GtkWidget *pane;
	GtkDropTarget *drop;
	/* in order of preference: a file is sent as it is */
	GType drop_types[] = { PIDGIN_TYPE_BLIST_NODE_ITEM, GDK_TYPE_FILE_LIST, GDK_TYPE_TEXTURE };

	gtkconv = g_new0(PidginConversation, 1);
	conv->ui_data = gtkconv;
	gtkconv->active_conv = conv;
	gtkconv->convs = g_list_prepend(NULL, conv);
	gtkconv->unseen_state = PIDGIN_UNSEEN_NONE;

	if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_IM)
		gtkconv->u.im = g_new0(PidginImPane, 1);
	else
		gtkconv->u.chat = g_new0(PidginChatPane, 1);

	pane = setup_common_pane(gtkconv);

	gtkconv->tab_cont = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(gtkconv->tab_cont, "pidgin-conv-pane");
	gtk_box_append(GTK_BOX(gtkconv->tab_cont), pane);
	g_object_set_data(G_OBJECT(gtkconv->tab_cont), "PidginConversation", gtkconv);
	g_object_ref_sink(gtkconv->tab_cont);
	gtkconv->tabby = g_object_ref_sink(setup_tab_label(gtkconv));

	drop = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_COPY | GDK_ACTION_MOVE);
	gtk_drop_target_set_gtypes(drop, drop_types, G_N_ELEMENTS(drop_types));
	g_signal_connect(drop, "drop", G_CALLBACK(conv_drop_cb), gtkconv);
	gtk_widget_add_controller(gtkconv->tab_cont, GTK_EVENT_CONTROLLER(drop));

	node = get_conversation_blist_node(conv);
	gtkconv->make_sound = node == NULL || !purple_blist_node_get_bool(node, "gtk-mute-sound");
	if (node != NULL && (value = g_hash_table_lookup(node->settings, "enable-logging")) &&
	    purple_value_get_type(value) == PURPLE_TYPE_BOOLEAN)
		purple_conversation_set_logging(conv, purple_value_get_boolean(value));

	update_features(gtkconv);
	update_tab_and_infopane(gtkconv);
	if (is_chat(gtkconv))
		update_topic(gtkconv);

	if (hidden)
		pidgin_conv_window_add_gtkconv(pidgin_conv_window_get_hidden(), gtkconv);
	else
		pidgin_conv_placement_place(gtkconv);
}

static void
pidgin_conv_new_hidden(PurpleConversation *conv)
{
	private_gtkconv_new(conv, TRUE);
}

void
pidgin_conv_new(PurpleConversation *conv)
{
	private_gtkconv_new(conv, FALSE);
	if (PIDGIN_IS_PIDGIN_CONVERSATION(conv))
		purple_signal_emit(pidgin_conversations_get_handle(), "conversation-displayed",
		                   PIDGIN_CONVERSATION(conv));
}

static gboolean
hide_new_for(PurpleAccount *account)
{
	const char *hide = purple_prefs_get_string(CONV_PREFS "/im/hide_new");

	if (purple_strequal(hide, "always"))
		return TRUE;
	return purple_strequal(hide, "away") &&
	       !purple_status_is_available(purple_account_get_active_status(account));
}

static void
received_im_msg_cb(PurpleAccount *account, char *sender, char *message,
                   PurpleConversation *conv, PurpleMessageFlags flags)
{
	PurpleConversationUiOps *ops = pidgin_conversations_get_conv_ui_ops();
	gboolean hide = hide_new_for(account);

	if (conv != NULL && PIDGIN_IS_PIDGIN_CONVERSATION(conv) && !hide) {
		PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);

		if (pidgin_conv_is_hidden(gtkconv))
			pidgin_conv_attach_to_conversation(conv);
		return;
	}
	/* A new conversation: hidden if hide_new says so (the tray lists it). */
	if (conv == NULL && hide) {
		ops->create_conversation = pidgin_conv_new_hidden;
		purple_conversation_new(PURPLE_CONV_TYPE_IM, account, sender);
		ops->create_conversation = pidgin_conv_new;
	}
}

static void
pidgin_conv_destroy(PurpleConversation *conv)
{
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);

	if (gtkconv == NULL)
		return;

	gtkconv->closing = TRUE;
	cancel_banner(gtkconv);
	if (gtkconv->win != NULL)
		pidgin_conv_window_remove_gtkconv(gtkconv->win, gtkconv);

	purple_request_close_with_handle(gtkconv);
	purple_notify_close_with_handle(gtkconv);

	if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_IM) {
		if (gtkconv->u.im->typing_timer != 0)
			g_source_remove(gtkconv->u.im->typing_timer);
		g_free(gtkconv->u.im);
	} else {
		PidginChatPane *pane = gtkconv->u.chat;

		if (pane->menu != NULL)
			gtk_widget_unparent(pane->menu);
		g_clear_object(&pane->users);
		g_clear_pointer(&pane->by_name, g_hash_table_destroy);
		g_free(pane->menu_who);
		g_free(pane->self_occupant_id);
		g_free(pane);
	}

	g_object_set_data(G_OBJECT(gtkconv->tab_cont), "PidginConversation", NULL);
	g_clear_object(&gtkconv->tab_cont);
	g_clear_object(&gtkconv->tabby);
	g_list_free(gtkconv->convs);
	conv->ui_data = NULL;
	g_free(gtkconv);
}

void
pidgin_conv_close(PidginConversation *gtkconv)
{
	PurpleConversation *conv = gtkconv->active_conv;

	if (gtkconv->closing)
		return;
	purple_signal_emit(pidgin_conversations_get_handle(), "conversation-hiding", gtkconv);
	/* Leaves the chat / ends the IM; destroy() takes the tab away. */
	purple_conversation_destroy(conv);
}

gboolean
pidgin_conv_attach_to_conversation(PurpleConversation *conv)
{
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);

	if (gtkconv == NULL || !PIDGIN_IS_PIDGIN_CONVERSATION(conv))
		return FALSE;
	if (pidgin_conv_is_hidden(gtkconv)) {
		pidgin_conv_window_remove_gtkconv(gtkconv->win, gtkconv);
		pidgin_conv_placement_place(gtkconv);
		purple_signal_emit(pidgin_conversations_get_handle(), "conversation-displayed",
		                   gtkconv);
	}
	return TRUE;
}

void
pidgin_conv_switch_active_conversation(PurpleConversation *conv)
{
	/* One PurpleConversation per tab in pidgin4 (Send To re-targets it). */
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);

	if (gtkconv != NULL && gtkconv->active_conv != conv)
		gtkconv->active_conv = conv;
}

/**************************************************************************
 * Writing
 **************************************************************************/

static gboolean
pidgin_conv_has_focus(PurpleConversation *conv)
{
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);

	if (gtkconv == NULL || gtkconv->win == NULL || pidgin_conv_is_hidden(gtkconv))
		return FALSE;
	return gtk_window_is_active(GTK_WINDOW(gtkconv->win->window)) &&
	       pidgin_conv_window_get_active_gtkconv(gtkconv->win) == gtkconv;
}

static void
pidgin_conv_write_conv(PurpleConversation *conv, const char *name, const char *alias,
                       const char *message, PurpleMessageFlags flags, time_t mtime)
{
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);
	PurpleAccount *account;
	PurplePluginProtocolInfo *prpl_info;
	PidginMarkupOptions opts = { 0 };
	PidginMessage *msg;
	GHashTable *meta;
	char *displaying, *inline_html;

	g_return_if_fail(conv != NULL);
	if (gtkconv == NULL)
		return;

	account = purple_conversation_get_account(conv);
	meta = pidgin_conv_meta_take(conv, flags);

	displaying = (flags & PURPLE_MESSAGE_NO_LINKIFY) ? g_strdup(message)
	                                                 : purple_markup_linkify(message);
	if (pidgin_message_view_emit_displaying(conv, name, &displaying, flags)) {
		g_free(displaying);
		if (meta)
			g_hash_table_unref(meta);
		return;
	}

	inline_html = pidgin_conv_meta_inline_image_html(conv, displaying);
	if (flags & (PURPLE_MESSAGE_SEND | PURPLE_MESSAGE_RECV)) {
		msg = pidgin_message_new(name, alias, inline_html ? inline_html : displaying,
		                         flags, mtime);
	} else {
		/* Status lines (NO_LOG notices, /help, ...) have no name, as
		 * Pidgin 2 showed them: styled like system lines. */
		msg = pidgin_message_new(NULL, NULL, inline_html ? inline_html : displaying,
		                         flags | (flags & PURPLE_MESSAGE_ERROR ? 0 : PURPLE_MESSAGE_SYSTEM),
		                         mtime);
	}
	g_free(inline_html);

	prpl_info = conv_prpl_info(conv);
	if ((flags & PURPLE_MESSAGE_RECV) &&
	    !purple_prefs_get_bool(CONV_PREFS "/show_incoming_formatting"))
		opts.flags |= PIDGIN_MARKUP_NO_INCOMING_FORMATTING;
	if (account_is_jabber(account) &&
	    !(meta != NULL && purple_strequal(g_hash_table_lookup(meta, "unstyled"), "1")))
		opts.flags |= PIDGIN_MARKUP_STYLING;
	if (prpl_info != NULL && (prpl_info->options & OPT_PROTO_USE_POINTSIZE))
		opts.flags |= PIDGIN_MARKUP_USE_POINTSIZE;
	if (!(flags & (PURPLE_MESSAGE_SEND | PURPLE_MESSAGE_RECV)))
		opts.flags |= PIDGIN_MARKUP_KEEP_NEWLINES;
	opts.protocol_sml = purple_account_get_protocol_name(account);
	/* Our own custom smileys show in what we sent. */
	if ((flags & PURPLE_MESSAGE_SEND) && (conv->features & PURPLE_CONNECTION_ALLOW_CUSTOM_SMILEY))
		opts.smiley_match = pidgin_custom_smiley_match_func;
	pidgin_message_set_parse_options(msg, &opts);

	if (meta != NULL) {
		pidgin_message_apply_meta(msg, meta);
		/* Our occupant-id, from our own room messages (for plugins). */
		if (is_chat(gtkconv) && (flags & PURPLE_MESSAGE_SEND) &&
		    g_hash_table_lookup(meta, "occupant-id") != NULL &&
		    !purple_strequal(gtkconv->u.chat->self_occupant_id,
		                     g_hash_table_lookup(meta, "occupant-id"))) {
			g_free(gtkconv->u.chat->self_occupant_id);
			gtkconv->u.chat->self_occupant_id = g_strdup(g_hash_table_lookup(meta, "occupant-id"));
		}
		if (!(flags & PURPLE_MESSAGE_REMOTE_SEND) && (flags & PURPLE_MESSAGE_SEND))
			pidgin_message_set_receipt(msg, PIDGIN_RECEIPT_SENT);
	}

	if (pidgin_conv_meta_is_older(meta)) {
		/* A scroll-back page: shown when complete (prepended). */
		pidgin_conv_meta_message_displayed(conv, msg, meta);
		pidgin_conv_meta_queue_older(conv, msg);
	} else {
		pidgin_message_view_append(conv_view(gtkconv), msg);
		pidgin_conv_meta_message_displayed(conv, msg, meta);

		if (!(flags & PURPLE_MESSAGE_SEND)) {
			if (!pidgin_conv_has_focus(conv))
				pidgin_conv_set_unseen(conv, unseen_for_flags(flags));
			else if (flags & PURPLE_MESSAGE_RECV)
				pidgin_conv_meta_mark_displayed(conv);
		}
	}

	pidgin_message_view_emit_displayed(conv, name, displaying, flags);

	if ((flags & PURPLE_MESSAGE_RECV) && purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_IM)
		update_typing(gtkconv);

	g_object_unref(msg);
	g_free(displaying);
	if (meta)
		g_hash_table_unref(meta);
}

static void
pidgin_conv_write_im(PurpleConversation *conv, const char *who, const char *message,
                     PurpleMessageFlags flags, time_t mtime)
{
	purple_conversation_write(conv, who, message, flags, mtime);
}

static void
pidgin_conv_write_chat(PurpleConversation *conv, const char *who, const char *message,
                       PurpleMessageFlags flags, time_t mtime)
{
	purple_conversation_write(conv, who, message, flags, mtime);
}

/**************************************************************************
 * Chat users
 **************************************************************************/

static void
pidgin_conv_chat_add_users(PurpleConversation *conv, GList *cbuddies, gboolean new_arrivals)
{
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);
	GList *l;

	if (gtkconv == NULL)
		return;
	for (l = cbuddies; l != NULL; l = l->next)
		chat_user_add(gtkconv, l->data);
	chat_users_update_count(gtkconv);
	/* Our own flags may have changed (moderation). */
	update_features(gtkconv);
}

static void
pidgin_conv_chat_rename_user(PurpleConversation *conv, const char *old_name,
                             const char *new_name, const char *new_alias)
{
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);
	PurpleConvChatBuddy *cb;

	if (gtkconv == NULL)
		return;
	chat_user_remove(gtkconv, old_name);
	cb = purple_conv_chat_cb_find(PURPLE_CONV_CHAT(conv), new_name);
	if (cb != NULL)
		chat_user_add(gtkconv, cb);
	chat_users_update_count(gtkconv);
	if (purple_strequal(new_name, purple_conv_chat_get_nick(PURPLE_CONV_CHAT(conv))))
		update_features(gtkconv);
}

static void
pidgin_conv_chat_remove_users(PurpleConversation *conv, GList *users)
{
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);
	GList *l;

	if (gtkconv == NULL)
		return;
	for (l = users; l != NULL; l = l->next)
		chat_user_remove(gtkconv, l->data);
	chat_users_update_count(gtkconv);
}

static void
pidgin_conv_chat_update_user(PurpleConversation *conv, const char *user)
{
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);
	PurpleConvChatBuddy *cb;

	if (gtkconv == NULL)
		return;
	cb = purple_conv_chat_cb_find(PURPLE_CONV_CHAT(conv), user);
	if (cb != NULL)
		chat_user_add(gtkconv, cb);
	else
		chat_user_remove(gtkconv, user);
	chat_users_update_count(gtkconv);
	if (purple_strequal(user, purple_conv_chat_get_nick(PURPLE_CONV_CHAT(conv))))
		update_features(gtkconv);
}

/**************************************************************************
 * The other ops
 **************************************************************************/

void
pidgin_conv_present_conversation(PurpleConversation *conv)
{
	PidginConversation *gtkconv;

	pidgin_conv_attach_to_conversation(conv);
	gtkconv = PIDGIN_CONVERSATION(conv);
	if (gtkconv == NULL || gtkconv->win == NULL)
		return;
	pidgin_conv_window_switch_gtkconv(gtkconv->win, gtkconv);
	/* gtk_window_present() uses an xdg-activation token when GTK has one;
	 * Sway marks the window urgent otherwise. */
	pidgin_conv_window_raise(gtkconv->win);
	gtk_widget_grab_focus(gtkconv->entry);
}

/*
 * Remote custom smileys (XHTML-IM BoB, MSN): not supported. Returning FALSE
 * tells the prpl not to fetch them; their shortcut stays text.
 */
static gboolean
pidgin_conv_custom_smiley_add(PurpleConversation *conv, const char *smile, gboolean remote)
{
	return FALSE;
}

static void
pidgin_conv_custom_smiley_write(PurpleConversation *conv, const char *smile,
                                const guchar *data, gsize size)
{
}

static void
pidgin_conv_custom_smiley_close(PurpleConversation *conv, const char *smile)
{
}

static void
pidgin_conv_send_confirm(PurpleConversation *conv, const char *message)
{
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);

	/* As Pidgin 2: put it back in the entry for the user to send. */
	if (gtkconv != NULL)
		pidgin_compose_entry_set_markup(conv_entry(gtkconv), message);
}

static PurpleConversationUiOps conversation_ui_ops =
{
	pidgin_conv_new,
	pidgin_conv_destroy,
	pidgin_conv_write_chat,
	pidgin_conv_write_im,
	pidgin_conv_write_conv,
	pidgin_conv_chat_add_users,
	pidgin_conv_chat_rename_user,
	pidgin_conv_chat_remove_users,
	pidgin_conv_chat_update_user,
	pidgin_conv_present_conversation,
	pidgin_conv_has_focus,
	pidgin_conv_custom_smiley_add,
	pidgin_conv_custom_smiley_write,
	pidgin_conv_custom_smiley_close,
	pidgin_conv_send_confirm,
	NULL,
	NULL,
	NULL,
	NULL
};

PurpleConversationUiOps *
pidgin_conversations_get_conv_ui_ops(void)
{
	return &conversation_ui_ops;
}

/**************************************************************************
 * Menu actions (the window calls these for its active conversation)
 **************************************************************************/

static void
insert_link_cb(PidginConversation *gtkconv, PurpleRequestFields *fields)
{
	const char *url = purple_request_fields_get_string(fields, "url");
	const char *desc = purple_request_fields_get_string(fields, "description");

	if (url != NULL && *url != '\0')
		pidgin_compose_entry_insert_link(conv_entry(gtkconv), url,
		                                 (desc && *desc) ? desc : NULL);
}

static void
insert_link(PidginConversation *gtkconv)
{
	PurpleRequestFields *fields = purple_request_fields_new();
	PurpleRequestFieldGroup *group = purple_request_field_group_new(NULL);
	PurpleRequestField *field;

	purple_request_fields_add_group(fields, group);
	field = purple_request_field_string_new("url", _("_URL"), NULL, FALSE);
	purple_request_field_set_required(field, TRUE);
	purple_request_field_group_add_field(group, field);
	if (pidgin_compose_entry_get_caps(conv_entry(gtkconv)) & PIDGIN_FORMAT_LINKDESC)
		purple_request_field_group_add_field(group,
			purple_request_field_string_new("description", _("_Description"), NULL, FALSE));
	purple_request_fields(gtkconv, _("Insert Link"), NULL,
		_("Please enter the URL and description of the link that you want to insert."),
		fields, _("_Insert"), G_CALLBACK(insert_link_cb), _("Cancel"), NULL,
		purple_conversation_get_account(gtkconv->active_conv), NULL, gtkconv->active_conv,
		gtkconv);
}

static PidginConversation *
live_gtkconv(PidginConversation *gtkconv)
{
	GList *l;

	for (l = purple_get_conversations(); l != NULL; l = l->next)
		if (PIDGIN_CONVERSATION((PurpleConversation *)l->data) == gtkconv)
			return gtkconv;
	return NULL;
}

static void
insert_image_chosen_cb(GObject *source, GAsyncResult *res, gpointer data)
{
	PidginConversation *gtkconv = live_gtkconv(data);
	GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), res, NULL);
	char *contents = NULL;
	gsize len = 0;

	if (file != NULL && gtkconv != NULL && g_file_load_contents(file, NULL, &contents, &len,
	                                                           NULL, NULL)) {
		char *base = g_file_get_basename(file);
		int id = purple_imgstore_add_with_id(contents, len, base);

		pidgin_compose_entry_insert_image(conv_entry(gtkconv), id);
		purple_imgstore_unref_by_id(id);
		g_free(base);
	}
	g_clear_object(&file);
}

static void
send_file_chosen_cb(GObject *source, GAsyncResult *res, gpointer data)
{
	PidginConversation *gtkconv = live_gtkconv(data);
	GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), res, NULL);
	char *path = file ? g_file_get_path(file) : NULL;

	if (path != NULL && gtkconv != NULL)
		send_file_to(gtkconv, path);
	g_free(path);
	g_clear_object(&file);
}

typedef struct
{
	PidginConversation *gtkconv;
	char *html;
} SaveAs;

static void
save_as_cb(GObject *source, GAsyncResult *res, gpointer data)
{
	SaveAs *sa = data;
	GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), res, NULL);
	GError *error = NULL;

	if (file != NULL && !g_file_replace_contents(file, sa->html, strlen(sa->html), NULL,
	                                             FALSE, G_FILE_CREATE_NONE, NULL, NULL,
	                                             &error)) {
		purple_notify_error(NULL, _("Save Conversation"), _("Unable to save the conversation"),
		                    error->message);
		g_clear_error(&error);
	}
	g_clear_object(&file);
	g_free(sa->html);
	g_free(sa);
}

static char *
conversation_as_html(PidginConversation *gtkconv)
{
	GListModel *model = pidgin_message_view_get_model(conv_view(gtkconv));
	GString *s = g_string_new(NULL);
	char *title = g_markup_escape_text(purple_conversation_get_title(gtkconv->active_conv), -1);
	guint i, n = g_list_model_get_n_items(model);

	g_string_append_printf(s, "<html><head><meta http-equiv=\"content-type\" content=\"text/html; "
		"charset=UTF-8\"><title>%s</title></head><body><h3>%s</h3>\n", title, title);
	for (i = 0; i < n; i++) {
		PidginMessage *m = g_list_model_get_item(model, i);

		if (pidgin_message_get_kind(m) == PIDGIN_MESSAGE_KIND_NORMAL) {
			time_t t = pidgin_message_get_time(m);
			char *who = g_markup_escape_text(pidgin_message_get_alias(m) ?
			                                 pidgin_message_get_alias(m) : "", -1);

			g_string_append_printf(s, "<font size=\"2\">(%s)</font> <b>%s:</b> %s<br>\n",
				purple_date_format_long(localtime(&t)), who, pidgin_message_get_html(m));
			g_free(who);
		}
		g_object_unref(m);
	}
	g_string_append(s, "</body></html>\n");
	g_free(title);
	return g_string_free(s, FALSE);
}

static void
block_confirm_cb(GObject *source, GAsyncResult *res, gpointer data)
{
	PidginConversation *gtkconv = live_gtkconv(data);

	if (gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), res, NULL) == 1 &&
	    gtkconv != NULL)
		purple_privacy_deny(purple_conversation_get_account(gtkconv->active_conv),
		                    purple_conversation_get_name(gtkconv->active_conv), FALSE, FALSE);
}

static GtkWindow *
conv_parent(PidginConversation *gtkconv)
{
	GtkRoot *root = gtk_widget_get_root(gtkconv->tab_cont);

	return GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;
}

static gboolean
toggle_logging(PidginConversation *gtkconv)
{
	PurpleConversation *conv = gtkconv->active_conv;
	gboolean logging = !purple_conversation_is_logging(conv);
	PurpleBlistNode *node = get_conversation_blist_node(conv);

	if (logging) {
		purple_conversation_set_logging(conv, TRUE);
		purple_conversation_write(conv, NULL,
			_("Logging started. Future messages in this conversation will be logged."),
			conv->logs ? PURPLE_MESSAGE_SYSTEM : (PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG),
			time(NULL));
	} else {
		purple_conversation_write(conv, NULL,
			_("Logging stopped. Future messages in this conversation will not be logged."),
			conv->logs ? PURPLE_MESSAGE_SYSTEM : (PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG),
			time(NULL));
		purple_conversation_set_logging(conv, FALSE);
	}

	/* Saved only if it differs from the pref (Pidgin 2). */
	if (node != NULL) {
		gboolean def = purple_prefs_get_bool(is_chat(gtkconv) ? "/purple/logging/log_chats"
		                                                      : "/purple/logging/log_ims");
		if (logging == def)
			purple_blist_node_remove_setting(node, "enable-logging");
		else
			purple_blist_node_set_bool(node, "enable-logging", logging);
	}
	return logging;
}

gboolean
pidgin_conv_action_enabled(PidginConversation *gtkconv, const char *action)
{
	PurpleConversation *conv;
	PurpleAccount *account;
	PurpleConnection *gc;
	PurplePluginProtocolInfo *prpl_info;
	gboolean chat, connected;
	const char *name;

	if (gtkconv == NULL)
		return FALSE;
	conv = gtkconv->active_conv;
	account = purple_conversation_get_account(conv);
	gc = purple_conversation_get_gc(conv);
	prpl_info = conv_prpl_info(conv);
	chat = is_chat(gtkconv);
	connected = gc != NULL && prpl_info != NULL &&
	            !(chat && purple_conv_chat_has_left(PURPLE_CONV_CHAT(conv)));
	name = purple_conversation_get_name(conv);

	if (purple_strequal(action, "send-file"))
		return connected && (chat ? prpl_info->chat_send_file != NULL &&
		                            (prpl_info->chat_can_receive_file == NULL ||
		                             prpl_info->chat_can_receive_file(gc,
		                                 purple_conv_chat_get_id(PURPLE_CONV_CHAT(conv))))
		                          : prpl_info->send_file != NULL &&
		                            (prpl_info->can_receive_file == NULL ||
		                             prpl_info->can_receive_file(gc, name)));
	if (purple_strequal(action, "get-attention"))
		return connected && !chat && prpl_info->send_attention != NULL;
	if (purple_strequal(action, "get-info"))
		return connected && !chat && prpl_info->get_info != NULL;
	if (purple_strequal(action, "invite"))
		return connected && chat && prpl_info->chat_invite != NULL;
	if (purple_strequal(action, "block"))
		return connected && !chat && purple_privacy_check(account, name);
	if (purple_strequal(action, "unblock"))
		return connected && !chat && !purple_privacy_check(account, name);
	if (purple_strequal(action, "add"))
		return connected && (chat ? purple_blist_find_chat(account, name) == NULL
		                          : purple_find_buddy(account, name) == NULL);
	if (purple_strequal(action, "remove"))
		return chat ? purple_blist_find_chat(account, name) != NULL
		            : purple_find_buddy(account, name) != NULL;
	if (purple_strequal(action, "alias"))
		return chat ? purple_blist_find_chat(account, name) != NULL
		            : purple_find_buddy(account, name) != NULL;
	if (purple_strequal(action, "insert-link"))
		return (pidgin_compose_entry_get_caps(conv_entry(gtkconv)) & PIDGIN_FORMAT_LINK) != 0;
	if (purple_strequal(action, "insert-image"))
		return !chat && (pidgin_compose_entry_get_caps(conv_entry(gtkconv)) & PIDGIN_FORMAT_IMAGE);
	/* TODO(M5): the log viewer and the pounce editor. */
	if (purple_strequal(action, "view-log") || purple_strequal(action, "add-pounce"))
		return FALSE;
	if (purple_strequal(action, "logging"))
		return purple_conversation_is_logging(conv);
	if (purple_strequal(action, "sounds"))
		return gtkconv->make_sound;
	return TRUE;
}

void
pidgin_conv_action(PidginConversation *gtkconv, const char *action)
{
	PurpleConversation *conv;
	PurpleAccount *account;
	PurpleConnection *gc;
	const char *name;

	if (gtkconv == NULL)
		return;
	conv = gtkconv->active_conv;
	account = purple_conversation_get_account(conv);
	gc = purple_conversation_get_gc(conv);
	name = purple_conversation_get_name(conv);

	if (purple_strequal(action, "find")) {
		pidgin_message_view_set_search_mode(conv_view(gtkconv), TRUE);
	} else if (purple_strequal(action, "clear")) {
		purple_conversation_clear_message_history(conv);
	} else if (purple_strequal(action, "save-as")) {
		GtkFileDialog *dialog = gtk_file_dialog_new();
		SaveAs *sa = g_new0(SaveAs, 1);
		char *fname = g_strdup_printf("%s.html", purple_normalize(account, name));

		sa->gtkconv = gtkconv;
		sa->html = conversation_as_html(gtkconv);
		gtk_file_dialog_set_title(dialog, _("Save Conversation"));
		gtk_file_dialog_set_initial_name(dialog, fname);
		gtk_file_dialog_save(dialog, conv_parent(gtkconv), NULL, save_as_cb, sa);
		g_object_unref(dialog);
		g_free(fname);
	} else if (purple_strequal(action, "send-file")) {
		if (is_chat(gtkconv)) {
			GtkFileDialog *dialog = gtk_file_dialog_new();

			gtk_file_dialog_set_title(dialog, _("Send File"));
			gtk_file_dialog_open(dialog, conv_parent(gtkconv), NULL, send_file_chosen_cb, gtkconv);
			g_object_unref(dialog);
		} else if (gc != NULL) {
			serv_send_file(gc, name, NULL);
		}
	} else if (purple_strequal(action, "get-attention")) {
		if (gc != NULL)
			purple_prpl_send_attention(gc, name, 0);
	} else if (purple_strequal(action, "get-info")) {
		if (gc != NULL)
			serv_get_info(gc, purple_normalize(account, name));
	} else if (purple_strequal(action, "invite")) {
		purple_conv_chat_invite_user(PURPLE_CONV_CHAT(conv), NULL, NULL, TRUE);
	} else if (purple_strequal(action, "alias")) {
		if (is_chat(gtkconv)) {
			PurpleChat *chat = purple_blist_find_chat(account, name);
			if (chat)
				pidgin_dialogs_alias_chat(chat);
		} else {
			PurpleBuddy *buddy = purple_find_buddy(account, name);
			if (buddy)
				pidgin_dialogs_alias_buddy(buddy);
		}
	} else if (purple_strequal(action, "block")) {
		GtkAlertDialog *dialog = gtk_alert_dialog_new(_("Block %s?"), name);
		const char *buttons[] = { _("Cancel"), _("_Block"), NULL };

		gtk_alert_dialog_set_detail(dialog, _("You will not be able to see or send messages "
		                                      "to this contact while blocked."));
		gtk_alert_dialog_set_buttons(dialog, buttons);
		gtk_alert_dialog_set_cancel_button(dialog, 0);
		gtk_alert_dialog_choose(dialog, conv_parent(gtkconv), NULL, block_confirm_cb, gtkconv);
		g_object_unref(dialog);
	} else if (purple_strequal(action, "unblock")) {
		purple_privacy_allow(account, name, FALSE, FALSE);
	} else if (purple_strequal(action, "add")) {
		if (is_chat(gtkconv))
			purple_blist_request_add_chat(account, NULL, NULL, name);
		else
			purple_blist_request_add_buddy(account, name, NULL, NULL);
	} else if (purple_strequal(action, "remove")) {
		if (is_chat(gtkconv)) {
			PurpleChat *chat = purple_blist_find_chat(account, name);
			if (chat)
				pidgin_dialogs_remove_chat(chat);
		} else {
			PurpleBuddy *buddy = purple_find_buddy(account, name);
			if (buddy)
				pidgin_dialogs_remove_buddy(buddy);
		}
	} else if (purple_strequal(action, "insert-link")) {
		insert_link(gtkconv);
	} else if (purple_strequal(action, "insert-image")) {
		GtkFileDialog *dialog = gtk_file_dialog_new();
		GtkFileFilter *filter = gtk_file_filter_new();
		GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);

		gtk_file_filter_add_mime_type(filter, "image/*");
		gtk_file_filter_set_name(filter, _("Images"));
		g_list_store_append(filters, filter);
		gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
		gtk_file_dialog_set_title(dialog, _("Insert Image"));
		gtk_file_dialog_open(dialog, conv_parent(gtkconv), NULL, insert_image_chosen_cb, gtkconv);
		g_object_unref(filters);
		g_object_unref(filter);
		g_object_unref(dialog);
	} else if (purple_strequal(action, "close")) {
		pidgin_conv_close(gtkconv);
	} else if (purple_strequal(action, "logging")) {
		toggle_logging(gtkconv);
	} else if (purple_strequal(action, "sounds")) {
		PurpleBlistNode *node = get_conversation_blist_node(conv);

		gtkconv->make_sound = !gtkconv->make_sound;
		if (node != NULL)
			purple_blist_node_set_bool(node, "gtk-mute-sound", !gtkconv->make_sound);
	} else if (purple_strequal(action, "load-older")) {
		load_older(gtkconv);
	} else if (purple_strequal(action, "view-log") || purple_strequal(action, "add-pounce")) {
		purple_debug_info("gtkconv", "TODO(M5): %s\n", action);
	} else {
		purple_debug_warning("gtkconv", "Unknown conversation action %s\n", action);
	}
}

void
pidgin_conv_fill_more_menu(PidginConversation *gtkconv, GMenu *menu, GSimpleActionGroup *group)
{
	GList *list = NULL;

	g_menu_remove_all(menu);
	if (gtkconv == NULL)
		return;
	/* The prpl's (and plugins') extended conversation menu. */
	purple_signal_emit(purple_conversations_get_handle(), "conversation-extended-menu",
	                   gtkconv->active_conv, &list);
	/* Takes the list. */
	pidgin_menu_append_menu_actions(menu, list, gtkconv->active_conv, group, "more");
}

/**************************************************************************
 * Send To (Pidgin 2's generate_send_to_items())
 **************************************************************************/

static GVariant *
send_to_target(PurpleAccount *account, const char *name)
{
	return g_variant_new("(sss)", purple_account_get_protocol_id(account),
	                     purple_account_get_username(account), name);
}

/* Pidgin 2's compare_buddy_presence(): one item per account and name */
static gint
compare_buddy_presence(gconstpointer a, gconstpointer b)
{
	PurpleBuddy *b1 = purple_presence_get_buddy((PurplePresence *)a);
	PurpleBuddy *b2 = purple_presence_get_buddy((PurplePresence *)b);

	if (purple_buddy_get_account(b1) == purple_buddy_get_account(b2) &&
	    purple_strequal(purple_buddy_get_name(b1), purple_buddy_get_name(b2)))
		return 0;
	return 1;
}

guint
pidgin_conv_fill_send_to_menu(PidginConversation *gtkconv, GMenu *menu, GVariant **current)
{
	PurpleConversation *conv;
	PurpleBuddy *self;
	GSList *buds, *l;
	GList *list = NULL, *iter;
	guint n = 0;

	g_menu_remove_all(menu);
	if (current != NULL)
		*current = NULL;
	if (gtkconv == NULL || is_chat(gtkconv))
		return 0;
	conv = gtkconv->active_conv;

	buds = purple_find_buddies(conv->account, conv->name);
	for (l = buds; l != NULL; l = l->next) {
		PurpleBlistNode *node = (PurpleBlistNode *)purple_buddy_get_contact(l->data);

		for (node = node->child; node != NULL; node = node->next) {
			PurpleBuddy *buddy = (PurpleBuddy *)node;
			PurpleAccount *account;
			PurplePresence *presence;

			if (!PURPLE_BLIST_NODE_IS_BUDDY(node))
				continue;
			account = purple_buddy_get_account(buddy);
			if (!purple_account_is_connected(account) && account != conv->account)
				continue;
			presence = purple_buddy_get_presence(buddy);
			if (g_list_find_custom(list, presence, compare_buddy_presence) == NULL)
				list = g_list_prepend(list, presence);
		}
	}
	g_slist_free(buds);

	/* Only with more than one to choose from. */
	if (list != NULL && list->next != NULL) {
		for (iter = g_list_last(list); iter != NULL; iter = iter->prev) {
			PurpleBuddy *buddy = purple_presence_get_buddy(iter->data);
			PurpleAccount *account = purple_buddy_get_account(buddy);
			GIcon *icon = pidgin_create_prpl_gicon(account, NULL);
			GMenuItem *item;
			char *label;

			if (PURPLE_BUDDY_IS_ONLINE(buddy))
				label = g_strdup_printf("%s (%s)", purple_buddy_get_name(buddy),
				                        purple_account_get_name_for_display(account));
			else
				/* Pidgin 2 greyed these out */
				label = g_strdup_printf(_("%s (%s) - Offline"), purple_buddy_get_name(buddy),
				                        purple_account_get_name_for_display(account));
			item = g_menu_item_new(label, NULL);
			g_menu_item_set_action_and_target_value(item, "conv.send-to",
				send_to_target(account, purple_buddy_get_name(buddy)));
			if (icon != NULL)
				g_menu_item_set_icon(item, icon);
			g_menu_append_item(menu, item);
			g_object_unref(item);
			g_clear_object(&icon);
			g_free(label);
			n++;
		}
	}
	g_list_free(list);

	if (n > 0 && current != NULL) {
		self = purple_find_buddy(conv->account, conv->name);
		*current = g_variant_ref_sink(send_to_target(conv->account,
			self ? purple_buddy_get_name(self) : conv->name));
	}
	return n;
}

void
pidgin_conv_send_to(PidginConversation *gtkconv, PurpleAccount *account, const char *name)
{
	PurpleConversation *conv, *other;
	PurpleBuddyIcon *icon;
	gboolean logging;

	g_return_if_fail(gtkconv != NULL && account != NULL && name != NULL);

	conv = gtkconv->active_conv;
	if (is_chat(gtkconv))
		return;
	if (account == conv->account) {
		/* purple_normalize() returns a static buffer */
		char *norm = g_strdup(purple_normalize(account, name));
		gboolean same = purple_strequal(norm, purple_normalize(account, conv->name));

		g_free(norm);
		if (same)
			return;
	}

	/* Pidgin 2 kept one PurpleConversation per buddy under the tab. pidgin4
	 * has one per tab: a buddy with a tab of its own is shown there. */
	other = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, name, account);
	if (other != NULL && other != conv) {
		pidgin_conv_present_conversation(other);
		return;
	}

	purple_debug_info("gtkconv", "Send To: %s -> %s\n", conv->name, name);

	/* The log is per buddy: the next write opens the new buddy's. */
	logging = purple_conversation_is_logging(conv);
	purple_conversation_close_logs(conv);

	if (account != conv->account) {
		/* libpurple's conversation cache is keyed by (account, name), and
		 * purple_conversation_set_account() leaves it alone. Drop the old
		 * key by renaming under the old account first; the placeholder
		 * key that remains is never looked up. */
		char *placeholder = g_strdup_printf("\x1bpidgin4-send-to-%p", (void *)conv);

		purple_conversation_set_name(conv, placeholder);
		g_free(placeholder);
		conv->account = account;
	}
	purple_conversation_set_name(conv, name);
	purple_conversation_set_logging(conv, logging);

	purple_conv_im_set_typing_state(PURPLE_CONV_IM(conv), PURPLE_NOT_TYPING);
	icon = purple_buddy_icons_find(account, name);
	purple_conv_im_set_icon(PURPLE_CONV_IM(conv), icon);
	if (icon != NULL)
		purple_buddy_icon_unref(icon);
	{
		PurpleConnection *gc = purple_account_get_connection(account);

		purple_conversation_set_features(conv, gc ? gc->flags : 0);
	}
	/* The UI (features, infopane, tab, menus) follows the account. */
	purple_conversation_update(conv, PURPLE_CONV_UPDATE_ACCOUNT);
	pidgin_conv_update_buddy_icon(conv);
}

/**************************************************************************
 * Updates from libpurple
 **************************************************************************/

static void
pidgin_conv_updated(PurpleConversation *conv, PurpleConvUpdateType type)
{
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);

	if (gtkconv == NULL || !PIDGIN_IS_PIDGIN_CONVERSATION(conv) || gtkconv->closing)
		return;

	switch (type) {
		case PURPLE_CONV_UPDATE_TYPING:
			update_typing(gtkconv);
			break;
		case PURPLE_CONV_UPDATE_FEATURES:
		case PURPLE_CONV_UPDATE_ACCOUNT:
			update_features(gtkconv);
			update_tab_and_infopane(gtkconv);
			break;
		case PURPLE_CONV_UPDATE_TOPIC:
			if (is_chat(gtkconv))
				update_topic(gtkconv);
			break;
		case PURPLE_CONV_UPDATE_ICON:
			pidgin_conv_update_buddy_icon(conv);
			break;
		case PURPLE_CONV_UPDATE_CHATLEFT:
			update_features(gtkconv);
			update_tab_and_infopane(gtkconv);
			break;
		case PURPLE_CONV_UPDATE_LOGGING:
			if (gtkconv->win != NULL)
				pidgin_conv_window_update_menu(gtkconv->win);
			break;
		default:
			/* TITLE, UNSEEN, AWAY, ... */
			update_tab_and_infopane(gtkconv);
			break;
	}
}

static void
update_for_buddy(PurpleBuddy *buddy)
{
	PurpleConversation *conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM,
		purple_buddy_get_name(buddy), purple_buddy_get_account(buddy));

	if (conv != NULL && PIDGIN_CONVERSATION(conv) != NULL)
		update_tab_and_infopane(PIDGIN_CONVERSATION(conv));
}

static void
buddy_status_changed_cb(PurpleBuddy *buddy, PurpleStatus *old, PurpleStatus *new_status)
{
	update_for_buddy(buddy);
}

/* The Send To menus list the contacts' buddies that are online. */
static void
update_window_menus(void)
{
	GList *l;

	for (l = pidgin_conv_windows_get_list(); l != NULL; l = l->next)
		pidgin_conv_window_update_menu(l->data);
}

static void
buddy_signed_cb(PurpleBuddy *buddy)
{
	update_for_buddy(buddy);
	update_window_menus();
}

static void
buddy_icon_changed_cb(PurpleBuddy *buddy)
{
	PurpleConversation *conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM,
		purple_buddy_get_name(buddy), purple_buddy_get_account(buddy));

	if (conv != NULL)
		pidgin_conv_update_buddy_icon(conv);
}

static void
blist_node_changed_cb(PurpleBlistNode *node)
{
	GList *l;

	/* Aliases, added/removed buddies: the titles and menus follow. */
	for (l = purple_get_conversations(); l != NULL; l = l->next) {
		PidginConversation *gtkconv = PIDGIN_CONVERSATION((PurpleConversation *)l->data);

		if (gtkconv != NULL && !gtkconv->closing) {
			update_tab_and_infopane(gtkconv);
			if (gtkconv->win != NULL &&
			    pidgin_conv_window_get_active_gtkconv(gtkconv->win) == gtkconv)
				pidgin_conv_window_update_menu(gtkconv->win);
		}
	}
}

static void
account_signed_cb(PurpleConnection *gc, gpointer data)
{
	PurpleAccount *account = purple_connection_get_account(gc);
	GList *l;

	for (l = purple_get_conversations(); l != NULL; l = l->next) {
		PurpleConversation *conv = l->data;
		PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);

		if (gtkconv != NULL && purple_conversation_get_account(conv) == account) {
			update_features(gtkconv);
			update_tab_and_infopane(gtkconv);
		}
	}
	update_window_menus();
}

static void
chat_joined_left_cb(PurpleConversation *conv)
{
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);

	if (gtkconv != NULL && PIDGIN_IS_PIDGIN_CONVERSATION(conv)) {
		update_features(gtkconv);
		update_tab_and_infopane(gtkconv);
	}
}

static void
chat_topic_changed_cb(PurpleConversation *conv, const char *old, const char *topic)
{
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);

	if (gtkconv != NULL && PIDGIN_IS_PIDGIN_CONVERSATION(conv))
		update_topic(gtkconv);
}

static void
foreach_gtkconv(void (*func)(PidginConversation *gtkconv))
{
	GList *l;

	for (l = purple_get_conversations(); l != NULL; l = l->next) {
		PidginConversation *gtkconv = PIDGIN_CONVERSATION((PurpleConversation *)l->data);

		if (gtkconv != NULL && !gtkconv->closing)
			func(gtkconv);
	}
}

static void
apply_toolbar_pref(PidginConversation *gtkconv)
{
	if (gtkconv->toolbar != NULL)
		gtk_widget_set_visible(gtkconv->toolbar,
			purple_prefs_get_bool(CONV_PREFS "/show_formatting_toolbar"));
}

static void
apply_view_refresh(PidginConversation *gtkconv)
{
	pidgin_message_view_refresh(conv_view(gtkconv));
}

static void
apply_send_button(PidginConversation *gtkconv)
{
	gtk_widget_set_visible(gtkconv->send_button, purple_prefs_get_bool(CONV4_PREFS "/send_button"));
}

static void
apply_spellcheck(PidginConversation *gtkconv)
{
	pidgin_compose_entry_set_spellcheck(conv_entry(gtkconv),
	                                    purple_prefs_get_bool(CONV_PREFS "/spellcheck"));
}

static void
apply_icons(PidginConversation *gtkconv)
{
	update_tab_and_infopane(gtkconv);
}

static void
apply_scrollback(PidginConversation *gtkconv)
{
	int n = purple_prefs_get_int(CONV_PREFS "/scrollback_lines");

	pidgin_message_view_set_scrollback(conv_view(gtkconv), n > 0 ? (guint)n : 0);
}

static void
conv_pref_cb(const char *name, PurplePrefType type, gconstpointer value, gpointer data)
{
	GList *l;

	if (g_str_has_suffix(name, "/show_formatting_toolbar"))
		foreach_gtkconv(apply_toolbar_pref);
	else if (g_str_has_suffix(name, "/show_timestamps"))
		foreach_gtkconv(apply_view_refresh);
	else if (g_str_has_suffix(name, "/send_button"))
		foreach_gtkconv(apply_send_button);
	else if (g_str_has_suffix(name, "/spellcheck"))
		foreach_gtkconv(apply_spellcheck);
	else if (g_str_has_suffix(name, "/show_buddy_icons") ||
	         g_str_has_suffix(name, "/show_protocol_icons") ||
	         g_str_has_suffix(name, "/close_on_tabs"))
		foreach_gtkconv(apply_icons);
	else if (g_str_has_suffix(name, "/scrollback_lines"))
		foreach_gtkconv(apply_scrollback);

	for (l = pidgin_conv_windows_get_list(); l != NULL; l = l->next)
		pidgin_conv_window_update_menu(l->data);
}

static void
hide_new_pref_cb(const char *name, PurplePrefType type, gconstpointer value, gpointer data)
{
	PidginWindow *hidden = pidgin_conv_window_get_hidden();
	GList *l, *copy;

	if (hidden == NULL || purple_strequal(value, "always"))
		return;
	copy = g_list_copy(hidden->gtkconvs);
	for (l = copy; l != NULL; l = l->next) {
		PidginConversation *gtkconv = l->data;
		PurpleConversation *conv = gtkconv->active_conv;

		if (is_chat(gtkconv) || gtkconv->unseen_count == 0 ||
		    hide_new_for(purple_conversation_get_account(conv)))
			continue;
		pidgin_conv_attach_to_conversation(conv);
	}
	g_list_free(copy);
}

static void
account_status_changed_cb(PurpleAccount *account, PurpleStatus *old, PurpleStatus *new_status)
{
	/* Back from away: show conversations hidden while away. */
	if (purple_strequal(purple_prefs_get_string(CONV_PREFS "/im/hide_new"), "away") &&
	    purple_status_is_available(new_status))
		hide_new_pref_cb(NULL, PURPLE_PREF_STRING, "away", NULL);
}

/**************************************************************************
 * Setup
 **************************************************************************/

void
pidgin_conversations_prefs_init(void)
{
	/* Pidgin 2's keys, types and defaults (pidgin/gtkconv.c): a profile
	 * Pidgin 2 has used gains nothing. */
	purple_prefs_add_none(CONV_PREFS);
	purple_prefs_add_bool(CONV_PREFS "/use_smooth_scrolling", TRUE);
	purple_prefs_add_bool(CONV_PREFS "/close_on_tabs", TRUE);
	purple_prefs_add_bool(CONV_PREFS "/send_bold", FALSE);
	purple_prefs_add_bool(CONV_PREFS "/send_italic", FALSE);
	purple_prefs_add_bool(CONV_PREFS "/send_underline", FALSE);
	purple_prefs_add_bool(CONV_PREFS "/spellcheck", TRUE);
	purple_prefs_add_bool(CONV_PREFS "/show_incoming_formatting", TRUE);
	purple_prefs_add_bool(CONV_PREFS "/resize_custom_smileys", TRUE);
	purple_prefs_add_int(CONV_PREFS "/custom_smileys_size", 96);
	purple_prefs_add_int(CONV_PREFS "/minimum_entry_lines", 2);
	purple_prefs_add_bool(CONV_PREFS "/show_timestamps", TRUE);
	purple_prefs_add_bool(CONV_PREFS "/show_formatting_toolbar", TRUE);
	purple_prefs_add_string(CONV_PREFS "/placement", "last");
	purple_prefs_add_int(CONV_PREFS "/placement_number", 1);
	purple_prefs_add_string(CONV_PREFS "/bgcolor", "");
	purple_prefs_add_string(CONV_PREFS "/fgcolor", "");
	purple_prefs_add_string(CONV_PREFS "/font_face", "");
	purple_prefs_add_int(CONV_PREFS "/font_size", 3);
	purple_prefs_add_bool(CONV_PREFS "/tabs", TRUE);
	purple_prefs_add_int(CONV_PREFS "/tab_side", GTK_POS_TOP);
	purple_prefs_add_int(CONV_PREFS "/scrollback_lines", 4000);

	purple_prefs_add_none(CONV_PREFS "/chat");
	purple_prefs_add_int(CONV_PREFS "/chat/entry_height", 54);
	purple_prefs_add_int(CONV_PREFS "/chat/userlist_width", 80);
	purple_prefs_add_int(CONV_PREFS "/chat/x", 0);
	purple_prefs_add_int(CONV_PREFS "/chat/y", 0);
	purple_prefs_add_int(CONV_PREFS "/chat/width", 340);
	purple_prefs_add_int(CONV_PREFS "/chat/height", 390);

	purple_prefs_add_none(CONV_PREFS "/im");
	purple_prefs_add_int(CONV_PREFS "/im/x", 0);
	purple_prefs_add_int(CONV_PREFS "/im/y", 0);
	purple_prefs_add_int(CONV_PREFS "/im/width", 340);
	purple_prefs_add_int(CONV_PREFS "/im/height", 390);
	purple_prefs_add_bool(CONV_PREFS "/im/animate_buddy_icons", TRUE);
	purple_prefs_add_int(CONV_PREFS "/im/entry_height", 54);
	purple_prefs_add_bool(CONV_PREFS "/im/show_buddy_icons", TRUE);
	purple_prefs_add_string(CONV_PREFS "/im/hide_new", "never");
	purple_prefs_add_bool(CONV_PREFS "/im/close_immediately", TRUE);

	/* pidgin4's own: window geometry (Wayland gives no position), the
	 * send button (the sendbutton plugin's job in Pidgin 2) and read
	 * markers (M8). */
	purple_prefs_add_none(CONV4_PREFS);
	purple_prefs_add_int(CONV4_PREFS "/width", 640);
	purple_prefs_add_int(CONV4_PREFS "/height", 480);
	purple_prefs_add_bool(CONV4_PREFS "/send_button", FALSE);
	purple_prefs_add_bool(CONV4_PREFS "/send_markers", TRUE);
}

static void
present_conv_action_cb(GSimpleAction *a, GVariant *p, gpointer data)
{
	PurpleConversation *conv = GSIZE_TO_POINTER((gsize)g_variant_get_uint64(p));

	if (g_list_find(purple_get_conversations(), conv) != NULL)
		pidgin_conv_present_conversation(conv);
}

static void
present_unseen_action_cb(GSimpleAction *a, GVariant *p, gpointer data)
{
	GList *list = pidgin_conversations_find_unseen_list(PURPLE_CONV_TYPE_ANY,
		PIDGIN_UNSEEN_TEXT, FALSE, 0), *l;

	for (l = list; l != NULL; l = l->next)
		pidgin_conv_present_conversation(l->data);
	g_list_free(list);
}

static const GActionEntry app_conv_actions[] = {
	{ .name = "pidgin4-present-conv", .activate = present_conv_action_cb, .parameter_type = "t" },
	{ .name = "pidgin4-present-unseen", .activate = present_unseen_action_cb },
};

void
pidgin_conversations_init(void)
{
	void *handle = pidgin_conversations_get_handle();
	void *conv_handle = purple_conversations_get_handle();
	void *blist_handle = purple_blist_get_handle();

	pidgin_conversations_prefs_init();
	pidgin_message_view_signals_init();

	purple_prefs_connect_callback(handle, CONV_PREFS, conv_pref_cb, NULL);
	purple_prefs_connect_callback(handle, CONV4_PREFS "/send_button", conv_pref_cb, NULL);
	purple_prefs_connect_callback(handle, PIDGIN_PREFS_ROOT "/blist/show_protocol_icons",
	                              conv_pref_cb, NULL);
	purple_prefs_connect_callback(handle, CONV_PREFS "/im/hide_new", hide_new_pref_cb, NULL);

	/* The rest of Pidgin 2's signals (the message view registered
	 * conversation-timestamp and displaying-/displayed-*-msg). */
	purple_signal_register(handle, "conversation-switched",
		purple_marshal_VOID__POINTER, NULL, 1,
		purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_CONVERSATION));
	purple_signal_register(handle, "conversation-hiding",
		purple_marshal_VOID__POINTER, NULL, 1,
		purple_value_new(PURPLE_TYPE_BOXED, "PidginConversation *"));
	purple_signal_register(handle, "conversation-displayed",
		purple_marshal_VOID__POINTER, NULL, 1,
		purple_value_new(PURPLE_TYPE_BOXED, "PidginConversation *"));
	purple_signal_register(handle, "chat-nick-autocomplete",
		purple_marshal_BOOLEAN__POINTER_BOOLEAN, purple_value_new(PURPLE_TYPE_BOOLEAN), 1,
		purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_CONVERSATION));
	purple_signal_register(handle, "chat-nick-clicked",
		purple_marshal_BOOLEAN__POINTER_POINTER_UINT, purple_value_new(PURPLE_TYPE_BOOLEAN), 3,
		purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_CONVERSATION),
		purple_value_new(PURPLE_TYPE_STRING), purple_value_new(PURPLE_TYPE_UINT));

	register_commands();

	pidgin_conv_windows_init();
	purple_conversations_set_ui_ops(&conversation_ui_ops);
	pidgin_conv_meta_init(&meta_ui_ops);

	if (pidgin_application_get() != NULL)
		g_action_map_add_action_entries(G_ACTION_MAP(pidgin_application_get()),
		                                app_conv_actions, G_N_ELEMENTS(app_conv_actions), NULL);

	purple_signal_connect(purple_connections_get_handle(), "signed-on", handle,
	                      PURPLE_CALLBACK(account_signed_cb), NULL);
	purple_signal_connect(purple_connections_get_handle(), "signed-off", handle,
	                      PURPLE_CALLBACK(account_signed_cb), NULL);
	purple_signal_connect(conv_handle, "received-im-msg", handle,
	                      PURPLE_CALLBACK(received_im_msg_cb), NULL);
	purple_signal_connect(conv_handle, "cleared-message-history", handle,
	                      PURPLE_CALLBACK(clear_conversation_scrollback_cb), NULL);
	purple_signal_connect(purple_accounts_get_handle(), "account-status-changed", handle,
	                      PURPLE_CALLBACK(account_status_changed_cb), NULL);
	purple_signal_connect(blist_handle, "blist-node-added", handle,
	                      PURPLE_CALLBACK(blist_node_changed_cb), NULL);
	purple_signal_connect(blist_handle, "blist-node-removed", handle,
	                      PURPLE_CALLBACK(blist_node_changed_cb), NULL);
	purple_signal_connect(blist_handle, "blist-node-aliased", handle,
	                      PURPLE_CALLBACK(blist_node_changed_cb), NULL);
	purple_signal_connect(blist_handle, "buddy-privacy-changed", handle,
	                      PURPLE_CALLBACK(blist_node_changed_cb), NULL);
	purple_signal_connect(blist_handle, "buddy-signed-on", handle,
	                      PURPLE_CALLBACK(buddy_signed_cb), NULL);
	purple_signal_connect(blist_handle, "buddy-signed-off", handle,
	                      PURPLE_CALLBACK(buddy_signed_cb), NULL);
	purple_signal_connect(blist_handle, "buddy-status-changed", handle,
	                      PURPLE_CALLBACK(buddy_status_changed_cb), NULL);
	purple_signal_connect(blist_handle, "buddy-idle-changed", handle,
	                      PURPLE_CALLBACK(buddy_signed_cb), NULL);
	purple_signal_connect(blist_handle, "buddy-icon-changed", handle,
	                      PURPLE_CALLBACK(buddy_icon_changed_cb), NULL);
	purple_signal_connect(conv_handle, "chat-left", handle,
	                      PURPLE_CALLBACK(chat_joined_left_cb), NULL);
	purple_signal_connect(conv_handle, "chat-joined", handle,
	                      PURPLE_CALLBACK(chat_joined_left_cb), NULL);
	purple_signal_connect(conv_handle, "chat-topic-changed", handle,
	                      PURPLE_CALLBACK(chat_topic_changed_cb), NULL);
	purple_signal_connect_priority(conv_handle, "conversation-updated", handle,
	                               PURPLE_CALLBACK(pidgin_conv_updated), NULL,
	                               PURPLE_SIGNAL_PRIORITY_LOWEST);

	/* pasted images sent as files (see "Images: paste and drop") */
	purple_signal_connect(purple_xfers_get_handle(), "file-send-complete", handle,
	                      PURPLE_CALLBACK(paste_xfer_done_cb), NULL);
	purple_signal_connect(purple_xfers_get_handle(), "file-send-cancel", handle,
	                      PURPLE_CALLBACK(paste_xfer_done_cb), NULL);
	paste_dir_cleanup();
}

void
pidgin_conversations_uninit(void)
{
	gsize i;

	pidgin_conv_meta_uninit();
	for (i = 0; i < G_N_ELEMENTS(cmd_ids); i++)
		if (cmd_ids[i] != 0)
			purple_cmd_unregister(cmd_ids[i]);
	memset(cmd_ids, 0, sizeof(cmd_ids));
	purple_prefs_disconnect_by_handle(pidgin_conversations_get_handle());
	purple_signals_disconnect_by_handle(pidgin_conversations_get_handle());
	pidgin_conv_windows_uninit();
}
