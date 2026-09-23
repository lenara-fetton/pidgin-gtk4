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
 * PidginConvMeta: the glue between the M8 message-metadata signals and IPC
 * (doc/PIDGIN-UPGRADE.md, M8 "Landed" notes; doc/conversation-signals.dox)
 * and the conversation UI.
 *
 *  - receiving-message-meta / sending-message-meta: the table is kept for
 *    the conversation until its next write_conv (gtkconv.c takes it with
 *    pidgin_conv_meta_take()); stale tables are dropped on the next idle.
 *    Duplicates are discarded ("discard" = "1") against the message index:
 *    a server-id hit always, a stanza-id/origin-id hit for archive (MAM)
 *    results and for prpls whose ids are server-assigned (IRC msgid);
 *    archive results without an id hit are matched fuzzily (time +-2 min,
 *    sender, text) when libpurple is about to write them
 *    ("writing-im-msg"/"writing-chat-msg", which also cancels the log line).
 *  - message-corrected/-reaction/-receipt/-retracted update the message
 *    in the conversation's view and the index, and write a readable
 *    fallback line into the HTML log without displaying it (profile
 *    contract rule 7). They return TRUE when they rendered the event.
 *  - Every displayed message is written into the index with its log file
 *    and the offset of its line.
 *  - The row actions (reply, react, edit, retract/moderate), read markers
 *    (XEP-0333 send-marker, XEP-0490 mds-publish) and scroll-back
 *    (index history, then mam-fetch-older) go out through the prpl's IPC
 *    commands, which exist for jabber (and, later, Discord: M9).
 *
 * gtkconv.c provides the view lookup and the scroll-back callback through
 * PidginConvMetaUiOps. The pure helpers below are unit-tested
 * (tests/test-convmeta.c).
 */
#ifndef _PIDGIN_CONV_META_H_
#define _PIDGIN_CONV_META_H_

#include <gtk/gtk.h>

#include "conversation.h"

#include "pidginmessage.h"
#include "pidginmessageindex.h"
#include "pidginmessageview.h"

G_BEGIN_DECLS

typedef struct
{
	/** The view showing @conv, or NULL. */
	PidginMessageView *(*get_view)(PurpleConversation *conv);
	/**
	 * Older messages for @conv arrived (oldest first; may be empty).
	 * @complete: nothing older exists. The array is owned by the caller.
	 */
	void (*older_done)(PurpleConversation *conv, GPtrArray *messages,
	                   gboolean complete);
	/** @conv was read on another device (XEP-0490): clear its unseen state. */
	void (*seen_elsewhere)(PurpleConversation *conv);
} PidginConvMetaUiOps;

/** Connects the signals (after the conversations UI is set up). */
void pidgin_conv_meta_init(const PidginConvMetaUiOps *ops);
void pidgin_conv_meta_uninit(void);
void *pidgin_conv_meta_get_handle(void);

/**************************************************************************
 * The write path (gtkconv.c's write_conv)
 **************************************************************************/

/**
 * The pending metadata for a message written to @conv with @flags (only
 * SEND/RECV messages take it), or NULL. Transfer full (unref it).
 */
GHashTable *pidgin_conv_meta_take(PurpleConversation *conv, PurpleMessageFlags flags);

/** TRUE if @meta marks a scroll-back (mam-query = older) message. */
gboolean pidgin_conv_meta_is_older(GHashTable *meta);

/** Holds an older message until its page is complete ("mam-query-done"). */
void pidgin_conv_meta_queue_older(PurpleConversation *conv, PidginMessage *msg);

/**
 * Called once @msg (built from @meta, may be NULL) is displayed: stores
 * reply previews, the markable flag, learns the upload host, and writes
 * the message into the index with its log position.
 */
void pidgin_conv_meta_message_displayed(PurpleConversation *conv, PidginMessage *msg,
                                        GHashTable *meta);

/**
 * If @html (a displayed body) is a lone http(s)/aesgcm image URL on an
 * allowed host, returns HTML that shows it inline (link + IMG), else NULL.
 */
char *pidgin_conv_meta_inline_image_html(PurpleConversation *conv, const char *html);

/**
 * Writes @text (plain) as a system line into @conv's log without showing
 * it (profile contract rule 7). Nothing if the conversation isn't logged.
 */
void pidgin_conv_meta_log_line(PurpleConversation *conv, const char *text, time_t when);

/** The file @conv's (first) log writes to, or NULL. */
char *pidgin_conv_meta_log_file(PurpleConversation *conv);

/**************************************************************************
 * Actions (prpl IPC)
 **************************************************************************/

/** TRUE if the conversation's prpl implements @command and is connected. */
gboolean pidgin_conv_meta_has_command(PurpleConversation *conv, const char *command);

/** The id to address @msg by in @conv (server-id in rooms, else stanza-id). */
const char *pidgin_conv_meta_target_id(PurpleConversation *conv, PidginMessage *msg);

/** Our identity in reaction lists (bare JID for IMs, nick/occupant-id in rooms). */
char *pidgin_conv_meta_self_id(PurpleConversation *conv);

gboolean pidgin_conv_meta_send_reaction(PurpleConversation *conv, PidginMessage *msg,
                                        const char *emoji, gboolean add,
                                        const char *self_id);
/** @body is plain (0393) text. */
gboolean pidgin_conv_meta_send_reply(PurpleConversation *conv, PidginMessage *msg,
                                     const char *body);
gboolean pidgin_conv_meta_send_correction(PurpleConversation *conv, PidginMessage *msg,
                                          const char *body);
/** Own messages: retraction; others' (rooms): moderation. */
gboolean pidgin_conv_meta_send_retraction(PurpleConversation *conv, PidginMessage *msg);

/**
 * The user has seen @conv: sends a displayed marker for the newest
 * markable incoming message and publishes it (XEP-0490), once per
 * message, if /pidgin4/conversations/send_markers is on.
 */
void pidgin_conv_meta_mark_displayed(PurpleConversation *conv);

/**
 * Loads messages older than the oldest in the view: from the index first
 * (any prpl; returned at once through older_done), else from the server
 * archive (mam-fetch-older; older_done when the page is complete).
 * Returns FALSE if nothing more can be loaded.
 */
gboolean pidgin_conv_meta_load_older(PurpleConversation *conv);

/**************************************************************************
 * Helpers (unit-tested)
 **************************************************************************/

/**
 * The discard decision for an incoming message's @meta: TRUE if the index
 * has it already (see the top of this file). @ids_unique: the prpl's
 * stanza ids are server-assigned (not XMPP). @hit (may be NULL) receives
 * the matching row.
 */
gboolean pidgin_conv_meta_check_discard(PidginMessageIndex *idx, const char *account_key,
                                        const char *conv_key, GHashTable *meta,
                                        gboolean ids_unique, PidginIndexedMessage **hit);

/** TRUE (and @hit) if a line without ids matches (MAM fuzzy dedup). */
gboolean pidgin_conv_meta_fuzzy_duplicate(PidginMessageIndex *idx, const char *account_key,
                                          const char *conv_key, gint64 when,
                                          const char *sender, const char *plain,
                                          PidginIndexedMessage **hit);

/** The first @max characters of @text on one line, with "…" if cut. */
char *pidgin_conv_meta_snippet(const char *text, guint max);

/** The contract rule 7 log lines. */
char *pidgin_conv_meta_text_edited(const char *who, const char *new_text);
char *pidgin_conv_meta_text_reaction(const char *who, const char *emoji, gboolean add,
                                     const char *target_text);
char *pidgin_conv_meta_text_retracted(const char *who, gboolean moderated,
                                      const char *reason);

/** @jid without its resource ("a@b/c" -> "a@b"). */
char *pidgin_conv_meta_bare_jid(const char *jid);

/** TRUE if @url is a lone http(s)/aesgcm URL whose path looks like an image. */
gboolean pidgin_conv_meta_is_image_url(const char *url);

/** A PidginMessage for an index row (plain body, ids, time, sender). */
PidginMessage *pidgin_conv_meta_message_from_index(const PidginIndexedMessage *row);

G_END_DECLS

#endif /* _PIDGIN_CONV_META_H_ */
