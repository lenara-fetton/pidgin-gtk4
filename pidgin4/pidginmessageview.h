/**
 * @file pidginmessageview.h The conversation/history message list
 * @ingroup pidgin
 */

/* pidgin
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
 * PidginMessageView replaces the GtkIMHtml conversation display: a
 * GtkListView over a GListStore of PidginMessage, with a GtkSearchBar for
 * find. Rows show the timestamp (the "conversation-timestamp" signal, so
 * timestamp_format keeps working), the name (CSS classes send, receive,
 * highlight, action, whisper, error, system; chat nick colours), the body
 * (PidginRichLabel), and the M8 decorations: edited marker with the
 * original text, receipt ticks, reactions, reply quote, retraction.
 *
 * M4b (gtkconv.c) builds PidginMessages in its write_conv and appends
 * them; the message actions come back as signals.
 */
#ifndef _PIDGINMESSAGEVIEW_H_
#define _PIDGINMESSAGEVIEW_H_

#include <gtk/gtk.h>

#include "conversation.h"

#include "pidginmessage.h"
#include "pidginnickcolor.h"

G_BEGIN_DECLS

#define PIDGIN_TYPE_MESSAGE_VIEW (pidgin_message_view_get_type())
G_DECLARE_FINAL_TYPE(PidginMessageView, pidgin_message_view, PIDGIN, MESSAGE_VIEW, GtkWidget)

/*
 * Signals (all with the PidginMessage the user acted on):
 *
 *   "reaction-toggled" (PidginMessage *msg, const char *emoji, gboolean add)
 *       A reaction chip was clicked or an emoji picked. The view doesn't
 *       change the message; the caller sends it (M8 "send-reaction") and
 *       updates the message.
 *   "reply-requested"   (PidginMessage *msg)
 *   "edit-requested"    (PidginMessage *msg)    own messages only
 *   "retract-requested" (PidginMessage *msg)    own messages only
 *   "top-reached"       ()
 *       The view was scrolled to its top (M4b loads older history).
 *   "focus-entry-requested" ()
 *       A message action (reply, react, edit, delete, from the row menu or
 *       the hover action bar) is done: the compose entry should take the
 *       keyboard focus back. Emitted from an idle, after the menu or
 *       emoji chooser has closed.
 *   "populate-menu"     (PidginMessage *msg, GMenu *section)
 *       Emitted when a row is bound, to let plugins add items to the
 *       row's context menu (replaces GtkTextView "populate-popup" hooks,
 *       e.g. timestamp_format). Actions must be app.* or win.* ones.
 */

GtkWidget *pidgin_message_view_new(void);

/** The conversation shown (for the timestamp signal). May be NULL. */
void pidgin_message_view_set_conversation(PidginMessageView *view,
                                          PurpleConversation *conv);
PurpleConversation *pidgin_message_view_get_conversation(PidginMessageView *view);

/** Chats colour nicks; IMs don't. */
void pidgin_message_view_set_is_chat(PidginMessageView *view, gboolean is_chat);

/** How chat nicks are coloured (XEP-0392 for XMPP). */
void pidgin_message_view_set_nick_color_scheme(PidginMessageView *view,
                                               PidginNickColorScheme scheme);

/**
 * Our own identity as it appears in reaction sender lists (the account's
 * name, or our nick in a chat), to show our reactions as active.
 */
void pidgin_message_view_set_self_id(PidginMessageView *view, const char *self_id);

/** The store (GListModel of PidginMessage). Transfer none. */
GListModel *pidgin_message_view_get_model(PidginMessageView *view);

/**
 * Appends a message. Scrolls to it if the view was at the bottom, and
 * trims the oldest messages beyond the scrollback limit. A "/me" body
 * (purple_message_meify()) is shown as an action.
 */
void pidgin_message_view_append(PidginMessageView *view, PidginMessage *msg);

/**
 * Inserts older messages (e.g. MAM scroll-back) before the first one,
 * keeping what is on screen where it is. @msg is one message.
 */
void pidgin_message_view_prepend(PidginMessageView *view, PidginMessage *msg);

/** Same for several (oldest first). */
void pidgin_message_view_prepend_many(PidginMessageView *view, GPtrArray *messages);

void pidgin_message_view_clear(PidginMessageView *view);

/** The newest message with @id as its stanza, origin or server id. */
PidginMessage *pidgin_message_view_find_by_id(PidginMessageView *view, const char *id);

/** The newest message sent by us (PURPLE_MESSAGE_SEND), for "edit last". */
PidginMessage *pidgin_message_view_get_last_sent(PidginMessageView *view);

void pidgin_message_view_scroll_to_message(PidginMessageView *view, PidginMessage *msg);
void pidgin_message_view_scroll_to_bottom(PidginMessageView *view);
/** TRUE if the newest message is visible (autoscroll is on). */
gboolean pidgin_message_view_is_at_bottom(PidginMessageView *view);

/**
 * Puts the marker line (markerline) after the last message; there is at
 * most one. remove_marker() takes it away.
 */
void pidgin_message_view_set_marker(PidginMessageView *view);
void pidgin_message_view_remove_marker(PidginMessageView *view);
/** The marker item, or NULL (M7: markerline's "Jump to markerline"). */
PidginMessage *pidgin_message_view_get_marker(PidginMessageView *view);

/** Find: the search bar (hidden until search mode is on). */
GtkSearchBar *pidgin_message_view_get_search_bar(PidginMessageView *view);
void pidgin_message_view_set_search_mode(PidginMessageView *view, gboolean on);
/** Filters to messages containing @text and highlights it; NULL clears. */
void pidgin_message_view_set_search_text(PidginMessageView *view, const char *text);
/** Messages shown with the current filter. */
guint pidgin_message_view_get_n_visible(PidginMessageView *view);

/**
 * Whether the row menu offers Reply, React, Edit and Delete (the
 * conversation's prpl implements the M8 IPC). Default TRUE; with TRUE only
 * messages with an id get them. @can_moderate also offers Delete on
 * others' messages that have a server id (XEP-0425 moderation).
 */
void pidgin_message_view_set_message_actions(PidginMessageView *view, gboolean enabled,
                                             gboolean can_moderate);

/**
 * Test hook: hovers the row showing @msg as the pointer would (without
 * the delay) and moves the hover off every other row; @msg NULL moves it
 * off all. Returns FALSE if no row shows @msg (it is not bound: scroll to
 * it first). *@bar is set to the hover action bar if it is shown, else
 * NULL. Its buttons are named react, reply, edit, delete and more
 * (gtk_widget_get_name()); hidden ones don't apply to the message.
 */
gboolean pidgin_message_view_test_hover(PidginMessageView *view, PidginMessage *msg,
                                        GtkWidget **bar);

/** Updates every visible row (e.g. after the show_timestamps pref changed). */
void pidgin_message_view_refresh(PidginMessageView *view);

/**
 * Maximum number of messages kept (0: unlimited). Defaults to the shared
 * /pidgin/conversations/scrollback_lines pref.
 */
void pidgin_message_view_set_scrollback(PidginMessageView *view, guint max);

/**************************************************************************
 * Conversation UI signals (for M4b's gtkconv.c)
 **************************************************************************/

/**
 * The handle the conversation UI signals are registered on (Pidgin 2's
 * pidgin_conversations_get_handle()): "conversation-timestamp",
 * "displaying-im-msg", "displayed-im-msg", "displaying-chat-msg" and
 * "displayed-chat-msg", with Pidgin 2's signatures. M4b's
 * pidgin_conversations_get_handle() must return this handle.
 */
void *pidgin_message_view_get_conv_handle(void);

/**
 * The timestamp text for @when in @conv (may be NULL): what a
 * "conversation-timestamp" handler returns, else "(time)" or, with
 * @show_date, "(date time)" in the locale's format.
 */
char *pidgin_message_view_format_timestamp(PurpleConversation *conv, time_t when,
                                           gboolean show_date);

/**
 * Emits "displaying-im-msg"/"displaying-chat-msg". @message is in/out
 * (plugins may replace it; free the result with g_free()). Returns TRUE
 * if a plugin cancelled the display.
 */
gboolean pidgin_message_view_emit_displaying(PurpleConversation *conv,
                                             const char *who, char **message,
                                             PurpleMessageFlags flags);

/** Emits "displayed-im-msg"/"displayed-chat-msg". */
void pidgin_message_view_emit_displayed(PurpleConversation *conv, const char *who,
                                        const char *message, PurpleMessageFlags flags);

/** Registers the signals above; idempotent. */
void pidgin_message_view_signals_init(void);
void pidgin_message_view_signals_uninit(void);

G_END_DECLS

#endif /* _PIDGINMESSAGEVIEW_H_ */
