/*
 * pidgin4: desktop notifications (GNotification) for new messages.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * See pidginnotify.h.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "account.h"
#include "blist.h"
#include "buddyicon.h"
#include "conversation.h"
#include "debug.h"
#include "prefs.h"
#include "signals.h"
#include "util.h"

#include "pidginnotify.h"

#define NOTIFY4_PREFS PIDGIN4_PREFS_ROOT "/notifications"
/* At most one notification per conversation in this time. */
#define RATE_LIMIT_US (10 * G_USEC_PER_SEC)
#define BODY_MAX_CHARS 200

typedef struct
{
	char *id;
	PurpleAccount *account;
	PurpleConversationType type;
	char *conv_name;
	gint64 last_sent;     /* monotonic µs */
	guint held;           /* messages since the last notification */
	char *title;          /* the latest message, for a held notification */
	char *body;
	GIcon *icon;
	guint timer;
} NotifyState;

static GHashTable *states = NULL;   /* id -> NotifyState */
static int notify_handle;

/**************************************************************************
 * Helpers
 **************************************************************************/

GVariant *
pidgin_notification_conversation_target(PurpleAccount *account,
                                        PurpleConversationType type,
                                        const char *conv_name)
{
	return g_variant_new("(ssis)", purple_account_get_protocol_id(account),
	                     purple_account_get_username(account), (gint)type,
	                     conv_name);
}

static char *
notification_id(PurpleAccount *account, PurpleConversationType type,
                const char *conv_name)
{
	char *normalized = g_strdup(purple_normalize(account, conv_name));
	char *id = g_strdup_printf("conv/%s/%s/%d/%s",
		purple_account_get_protocol_id(account),
		purple_account_get_username(account), (int)type,
		normalized ? normalized : conv_name);

	g_free(normalized);
	return id;
}

static void
state_free(NotifyState *state)
{
	if (state->timer != 0)
		g_source_remove(state->timer);
	g_free(state->id);
	g_free(state->conv_name);
	g_free(state->title);
	g_free(state->body);
	g_clear_object(&state->icon);
	g_free(state);
}

static GApplication *
get_app(void)
{
	GApplication *app = G_APPLICATION(pidgin_application_get());

	if (app == NULL || !g_application_get_is_registered(app))
		return NULL;
	return app;
}

/* The message as plain text, shortened. */
static char *
plain_body(const char *html)
{
	char *stripped, *text;

	if (html == NULL)
		return g_strdup("");

	stripped = purple_markup_strip_html(html);
	text = g_strstrip(stripped);
	if (g_utf8_strlen(text, -1) > BODY_MAX_CHARS) {
		char *cut = g_utf8_substring(text, 0, BODY_MAX_CHARS);

		text = g_strconcat(cut, "…", NULL);
		g_free(cut);
		g_free(stripped);
		return text;
	}
	text = g_strdup(text);
	g_free(stripped);
	return text;
}

static GIcon *
sender_icon(PurpleAccount *account, const char *sender)
{
	PurpleBuddyIcon *buddy_icon;
	GIcon *icon = NULL;

	buddy_icon = purple_buddy_icons_find(account, sender);
	if (buddy_icon != NULL) {
		size_t len = 0;
		gconstpointer data = purple_buddy_icon_get_data(buddy_icon, &len);

		if (data != NULL && len > 0) {
			GBytes *bytes = g_bytes_new(data, len);

			icon = g_bytes_icon_new(bytes);
			g_bytes_unref(bytes);
		}
		purple_buddy_icon_unref(buddy_icon);
	}

	if (icon == NULL)
		icon = g_themed_icon_new(PIDGIN4_APP_ID);
	return icon;
}

static char *
sender_name(PurpleAccount *account, const char *sender)
{
	PurpleBuddy *buddy = purple_find_buddy(account, sender);

	if (buddy != NULL)
		return g_strdup(purple_buddy_get_contact_alias(buddy));
	return g_strdup(sender);
}

/**************************************************************************
 * Sending and withdrawing
 **************************************************************************/

static void
send_notification(NotifyState *state)
{
	GApplication *app = get_app();
	GNotification *notification;
	char *body;

	if (app == NULL)
		return;

	if (state->held > 1)
		body = g_strdup_printf(ngettext("%u new message\n%s",
		                                "%u new messages\n%s", state->held),
		                       state->held, state->body);
	else
		body = g_strdup(state->body);

	notification = g_notification_new(state->title);
	g_notification_set_body(notification, body);
	if (state->icon != NULL)
		g_notification_set_icon(notification, state->icon);
	g_notification_set_category(notification, "im.received");
	g_notification_set_default_action_and_target_value(notification,
		"app.present-conversation",
		pidgin_notification_conversation_target(state->account, state->type,
		                                        state->conv_name));

	g_application_send_notification(app, state->id, notification);
	g_object_unref(notification);
	g_free(body);

	state->last_sent = g_get_monotonic_time();
	state->held = 0;
}

static gboolean
held_timeout_cb(gpointer data)
{
	NotifyState *state = data;

	state->timer = 0;
	if (state->held > 0)
		send_notification(state);
	return G_SOURCE_REMOVE;
}

void
pidgin_notification_new_message(PurpleAccount *account,
                                PurpleConversationType type,
                                const char *conv_name,
                                PurpleConversation *conv,
                                const char *sender, const char *message)
{
	NotifyState *state;
	char *id, *who;
	gint64 now = g_get_monotonic_time();

	g_return_if_fail(account != NULL);
	g_return_if_fail(conv_name != NULL);

	if (states == NULL)
		return;

	id = notification_id(account, type, conv_name);
	state = g_hash_table_lookup(states, id);
	if (state == NULL) {
		state = g_new0(NotifyState, 1);
		state->id = id;
		state->account = account;
		state->type = type;
		state->conv_name = g_strdup(conv_name);
		g_hash_table_insert(states, state->id, state);
	} else {
		g_free(id);
	}

	/* The latest message is what the notification shows. */
	who = sender_name(account, sender != NULL ? sender : conv_name);
	g_free(state->title);
	if (type == PURPLE_CONV_TYPE_CHAT && conv != NULL)
		state->title = g_strdup_printf(_("%s in %s"), who,
		                               purple_conversation_get_title(conv));
	else
		state->title = g_strdup(who);
	g_free(who);
	g_free(state->body);
	if (purple_prefs_get_bool(NOTIFY4_PREFS "/show_message"))
		state->body = plain_body(message);
	else
		state->body = g_strdup(_("New message"));
	g_clear_object(&state->icon);
	state->icon = sender_icon(account, sender != NULL ? sender : conv_name);
	state->held++;

	/* Rate limit: hold it until the conversation's 10 s are over. */
	if (state->last_sent != 0 && now - state->last_sent < RATE_LIMIT_US) {
		if (state->timer == 0)
			state->timer = g_timeout_add((RATE_LIMIT_US - (now - state->last_sent)) / 1000 + 1,
			                             held_timeout_cb, state);
		return;
	}

	send_notification(state);
}

void
pidgin_notification_withdraw(PurpleAccount *account, PurpleConversationType type,
                             const char *conv_name)
{
	GApplication *app;
	char *id;

	if (states == NULL || account == NULL || conv_name == NULL)
		return;

	id = notification_id(account, type, conv_name);
	if (g_hash_table_contains(states, id)) {
		app = get_app();
		if (app != NULL)
			g_application_withdraw_notification(app, id);
		g_hash_table_remove(states, id);
	}
	g_free(id);
}

static void
withdraw_conv(PurpleConversation *conv)
{
	pidgin_notification_withdraw(purple_conversation_get_account(conv),
		purple_conversation_get_type(conv), purple_conversation_get_name(conv));
}

/**************************************************************************
 * app.present-conversation
 **************************************************************************/

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

	pidgin_notification_withdraw(account, type, name);

	conv = purple_find_conversation_with_account(type, name, account);
	if (conv == NULL && type == PURPLE_CONV_TYPE_IM &&
	    purple_account_is_connected(account))
		conv = purple_conversation_new(PURPLE_CONV_TYPE_IM, account, name);

	/* GtkApplication has applied the activation token the notification
	 * server sent (if any), so the window may take the focus. */
	if (conv != NULL)
		purple_conversation_present(conv);
}

/**************************************************************************
 * Signals
 **************************************************************************/

static gboolean
wants_notification(PurpleConversation *conv, PurpleMessageFlags flags)
{
	if (!purple_prefs_get_bool(NOTIFY4_PREFS "/new_message"))
		return FALSE;
	if (flags & (PURPLE_MESSAGE_SEND | PURPLE_MESSAGE_DELAYED |
	             PURPLE_MESSAGE_NOTIFY | PURPLE_MESSAGE_SYSTEM))
		return FALSE;
	if (conv != NULL && purple_conversation_has_focus(conv))
		return FALSE;
	return TRUE;
}

static void
received_im_msg_cb(PurpleAccount *account, char *sender, char *message,
                   PurpleConversation *conv, PurpleMessageFlags flags,
                   gpointer data)
{
	if (!wants_notification(conv, flags))
		return;

	/* A first message has no conversation yet; libpurple creates it
	 * with the sender's name. */
	pidgin_notification_new_message(account, PURPLE_CONV_TYPE_IM,
		conv != NULL ? purple_conversation_get_name(conv) : sender,
		conv, sender, message);
}

static void
received_chat_msg_cb(PurpleAccount *account, char *sender, char *message,
                     PurpleConversation *conv, PurpleMessageFlags flags,
                     gpointer data)
{
	PurpleConvChat *chat;

	if (conv == NULL || !wants_notification(conv, flags))
		return;

	chat = purple_conversation_get_chat_data(conv);
	if (chat == NULL || purple_conv_chat_is_user_ignored(chat, sender))
		return;
	if (purple_strequal(purple_normalize(account, sender),
	                    purple_normalize(account, purple_conv_chat_get_nick(chat))))
		return;

	/* Chats: only when you are addressed. */
	if (!(flags & PURPLE_MESSAGE_NICK) &&
	    !purple_utf8_has_word(message, purple_conv_chat_get_nick(chat)))
		return;

	pidgin_notification_new_message(account, PURPLE_CONV_TYPE_CHAT,
		purple_conversation_get_name(conv), conv, sender, message);
}

static void
conversation_updated_cb(PurpleConversation *conv, PurpleConvUpdateType type,
                        gpointer data)
{
	/* The user looked at it. */
	if (type == PURPLE_CONV_UPDATE_UNSEEN && purple_conversation_has_focus(conv))
		withdraw_conv(conv);
}

static void
deleting_conversation_cb(PurpleConversation *conv, gpointer data)
{
	withdraw_conv(conv);
}

/**************************************************************************
 * Setup
 **************************************************************************/

void
pidgin_notification_init(void)
{
	void *conv_handle = purple_conversations_get_handle();
	GtkApplication *app = pidgin_application_get();
	static const GActionEntry entries[] = {
		{ .name = "present-conversation", .parameter_type = "(ssis)",
		  .activate = present_conversation_cb },
	};

	purple_prefs_add_none(NOTIFY4_PREFS);
	purple_prefs_add_bool(NOTIFY4_PREFS "/new_message", TRUE);
	purple_prefs_add_bool(NOTIFY4_PREFS "/show_message", TRUE);

	states = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
	                               (GDestroyNotify)state_free);

	if (app != NULL)
		g_action_map_add_action_entries(G_ACTION_MAP(app), entries,
		                                G_N_ELEMENTS(entries), NULL);

	purple_signal_connect(conv_handle, "received-im-msg", &notify_handle,
	                      PURPLE_CALLBACK(received_im_msg_cb), NULL);
	purple_signal_connect(conv_handle, "received-chat-msg", &notify_handle,
	                      PURPLE_CALLBACK(received_chat_msg_cb), NULL);
	purple_signal_connect(conv_handle, "conversation-updated", &notify_handle,
	                      PURPLE_CALLBACK(conversation_updated_cb), NULL);
	purple_signal_connect(conv_handle, "deleting-conversation", &notify_handle,
	                      PURPLE_CALLBACK(deleting_conversation_cb), NULL);
}

void
pidgin_notification_uninit(void)
{
	GApplication *app = get_app();
	GHashTableIter iter;
	gpointer id;

	purple_signals_disconnect_by_handle(&notify_handle);

	if (states == NULL)
		return;

	/* Nothing may stay behind in the notification server that would
	 * activate a pidgin4 that is gone. */
	g_hash_table_iter_init(&iter, states);
	while (g_hash_table_iter_next(&iter, &id, NULL))
		if (app != NULL)
			g_application_withdraw_notification(app, id);
	g_clear_pointer(&states, g_hash_table_destroy);
}

/**************************************************************************
 * Selftest
 **************************************************************************/

static gboolean
selftest_withdraw_cb(gpointer data)
{
	GApplication *app = get_app();

	if (app != NULL) {
		g_application_withdraw_notification(app, "selftest");
		purple_debug_info("notify", "selftest: withdrew the notification\n");
	}
	return G_SOURCE_REMOVE;
}

void
pidgin_notification_selftest(void)
{
	GApplication *app;
	GNotification *notification;
	GIcon *icon;

	if (g_getenv("PIDGIN4_NOTIFY_SELFTEST") == NULL)
		return;

	app = get_app();
	if (app == NULL) {
		purple_debug_warning("notify", "selftest: the application is not "
		                     "registered\n");
		return;
	}

	notification = g_notification_new(_("Pidgin 4 notification test"));
	g_notification_set_body(notification,
		_("A test from PIDGIN4_NOTIFY_SELFTEST; it goes away in 5 seconds."));
	icon = g_themed_icon_new(PIDGIN4_APP_ID);
	g_notification_set_icon(notification, icon);
	g_object_unref(icon);
	g_notification_set_category(notification, "im.received");
	/* No default action: clicking it activates the app (raises the
	 * buddy list). */
	g_application_send_notification(app, "selftest", notification);
	g_object_unref(notification);
	purple_debug_info("notify", "selftest: sent a notification\n");

	g_timeout_add_seconds(5, selftest_withdraw_cb, NULL);
}
