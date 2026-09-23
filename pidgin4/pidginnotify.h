/*
 * pidgin4: desktop notifications (GNotification) for new messages.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef _PIDGIN_NOTIFICATION_H_
#define _PIDGIN_NOTIFICATION_H_

#include <gio/gio.h>

#include "conversation.h"

/*
 * A GNotification per conversation when a message arrives and the
 * conversation does not have the focus (IMs; chats only when your nick is
 * said). Title: the sender (in chats "sender in room"); body: the message
 * as plain text; icon: the buddy icon when there is one, else the app
 * icon; the default action app.present-conversation presents the
 * conversation. It is withdrawn when the conversation gets the focus, is
 * closed or presented. At most one per conversation every 10 s: messages
 * in between are summed up in the next one.
 *
 * Prefs: /pidgin4/notifications/new_message (default TRUE) and
 * /pidgin4/notifications/show_message (default TRUE; FALSE shows only
 * "New message"). The notify plugin's "notification" option (M7) uses
 * pidgin_notification_new_message().
 *
 * On Sway GLib sends the notification to org.freedesktop.Notifications
 * (mako, dunst, ...); on GNOME to the shell (org.gtk.Notifications), which
 * needs the installed com.minowick.Pidgin4.desktop. Clicking it activates
 * app.present-conversation with the server's xdg-activation token, so the
 * window may take the focus.
 */

/** Registers the prefs, the app action and the signal handlers. */
void pidgin_notification_init(void);
void pidgin_notification_uninit(void);

/**
 * Shows (or replaces) the notification of @conv for a message from
 * @sender (@message is HTML). @conv may be NULL for a first IM, then
 * @account and @sender name the conversation.
 */
void pidgin_notification_new_message(PurpleAccount *account,
                                     PurpleConversationType type,
                                     const char *conv_name,
                                     PurpleConversation *conv,
                                     const char *sender,
                                     const char *message);

/** Withdraws the notification of a conversation, if any. */
void pidgin_notification_withdraw(PurpleAccount *account,
                                  PurpleConversationType type,
                                  const char *conv_name);

/**
 * (ssis): protocol id, account name, conversation type, conversation
 * name; the target of app.present-conversation.
 */
GVariant *pidgin_notification_conversation_target(PurpleAccount *account,
                                                  PurpleConversationType type,
                                                  const char *conv_name);

/**
 * Test hook: with PIDGIN4_NOTIFY_SELFTEST set, sends one test notification
 * and withdraws it after 5 s.
 */
void pidgin_notification_selftest(void);

#endif /* _PIDGIN_NOTIFICATION_H_ */
