/**
 * @file pidginmessage.h A displayed message and its metadata
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
 * PidginMessage is one row of a PidginMessageView (a GListStore of them per
 * conversation): what pidgin_conv_write_conv() used to turn into HTML.
 *
 * The metadata mirrors the M8 message-meta design (doc/PIDGIN-UPGRADE.md,
 * doc/conversation-signals.dox): the ids from "receiving-message-meta" /
 * "sending-message-meta" ("stanza-id", "origin-id", "server-id",
 * "occupant-id", "correction-of", "reply-to", "reply-to-sender",
 * "fallback-ranges"), and the state changed by "message-corrected",
 * "message-reaction", "message-receipt" and "message-retracted".
 *
 * Every property notifies, so bound rows update themselves.
 */
#ifndef _PIDGINMESSAGE_H_
#define _PIDGINMESSAGE_H_

#include <gtk/gtk.h>

#include "conversation.h"

#include "pidginattachment.h"
#include "pidginmarkup.h"

G_BEGIN_DECLS

typedef enum
{
	/** A message (or a system/error line). */
	PIDGIN_MESSAGE_KIND_NORMAL,
	/** The "last read" marker line (markerline). */
	PIDGIN_MESSAGE_KIND_MARKER
} PidginMessageKind;

/** Delivery state of a sent message (XEP-0184 receipts, XEP-0333 markers). */
typedef enum
{
	PIDGIN_RECEIPT_NONE,
	/** Acknowledged by the server (e.g. echo-message, carbons). */
	PIDGIN_RECEIPT_SENT,
	PIDGIN_RECEIPT_DELIVERED,
	PIDGIN_RECEIPT_DISPLAYED
} PidginReceiptState;

#define PIDGIN_TYPE_MESSAGE (pidgin_message_get_type())
G_DECLARE_FINAL_TYPE(PidginMessage, pidgin_message, PIDGIN, MESSAGE, GObject)

/**
 * A message as written to a conversation: @sender is the protocol name
 * (who), @alias what is shown (may be NULL: @sender), @html the body.
 */
PidginMessage *pidgin_message_new(const char *sender, const char *alias,
                                  const char *html, PurpleMessageFlags flags,
                                  time_t when);

/** A marker item (see pidgin_message_view_set_marker()). */
PidginMessage *pidgin_message_new_marker(void);

/**
 * Takes the ids out of a message-meta table (the keys listed above).
 * Unknown keys are ignored; NULL is fine.
 */
void pidgin_message_apply_meta(PidginMessage *msg, GHashTable *meta);

PidginMessageKind pidgin_message_get_kind(PidginMessage *msg);

const char *pidgin_message_get_sender(PidginMessage *msg);
const char *pidgin_message_get_alias(PidginMessage *msg);
void pidgin_message_set_alias(PidginMessage *msg, const char *alias);
PurpleMessageFlags pidgin_message_get_flags(PidginMessage *msg);
void pidgin_message_set_flags(PidginMessage *msg, PurpleMessageFlags flags);
time_t pidgin_message_get_time(PidginMessage *msg);

const char *pidgin_message_get_html(PidginMessage *msg);
/** Replaces the body (drops the parsed form). */
void pidgin_message_set_html(PidginMessage *msg, const char *html);

/** A file shown under the text (display only: the html, the log and the
 * index keep the text). NULL removes it. */
PidginAttachment *pidgin_message_get_attachment(PidginMessage *msg);
void pidgin_message_set_attachment(PidginMessage *msg, PidginAttachment *attachment);

/**
 * How the body is parsed: markup flags (e.g. PIDGIN_MARKUP_STYLING for XMPP,
 * USE_POINTSIZE, NO_INCOMING_FORMATTING), the protocol's smiley category,
 * and an optional custom smiley matcher. Drops the parsed form.
 */
void pidgin_message_set_parse_options(PidginMessage *msg,
                                      const PidginMarkupOptions *options);

/** The parsed body, parsed on first use and cached. Transfer none. */
PidginMarkupResult *pidgin_message_get_markup(PidginMessage *msg);

/** The body as plain text (the parsed text). */
const char *pidgin_message_get_plain_text(PidginMessage *msg);

/* Ids (XEP-0359 and friends; Discord uses stanza-id for its ids). */
const char *pidgin_message_get_stanza_id(PidginMessage *msg);
void pidgin_message_set_stanza_id(PidginMessage *msg, const char *id);
const char *pidgin_message_get_origin_id(PidginMessage *msg);
void pidgin_message_set_origin_id(PidginMessage *msg, const char *id);
const char *pidgin_message_get_server_id(PidginMessage *msg);
void pidgin_message_set_server_id(PidginMessage *msg, const char *id);
const char *pidgin_message_get_occupant_id(PidginMessage *msg);
void pidgin_message_set_occupant_id(PidginMessage *msg, const char *id);

/** TRUE if @id is this message's stanza, origin or server id. */
gboolean pidgin_message_has_id(PidginMessage *msg, const char *id);

/** The id this message corrects (it arrived as a correction). */
const char *pidgin_message_get_correction_of(PidginMessage *msg);
void pidgin_message_set_correction_of(PidginMessage *msg, const char *id);

/** Reply target: its id, sender and (optional) a preview of its text. */
const char *pidgin_message_get_reply_to(PidginMessage *msg);
const char *pidgin_message_get_reply_to_sender(PidginMessage *msg);
const char *pidgin_message_get_reply_preview(PidginMessage *msg);
void pidgin_message_set_reply(PidginMessage *msg, const char *id,
                              const char *sender, const char *preview);

/**
 * Applies a correction (XEP-0308, a Discord edit): the body becomes
 * @new_html, the old body is kept for "show original", "edited" becomes
 * TRUE. @new_id (may be NULL) is the correction's own id.
 */
void pidgin_message_apply_correction(PidginMessage *msg, const char *new_html,
                                     const char *new_id);
gboolean pidgin_message_get_edited(PidginMessage *msg);
/** Earlier bodies, oldest first (const char *). Transfer none. */
GPtrArray *pidgin_message_get_history(PidginMessage *msg);

PidginReceiptState pidgin_message_get_receipt(PidginMessage *msg);
/** Only ever moves forward (DISPLAYED implies DELIVERED). */
void pidgin_message_set_receipt(PidginMessage *msg, PidginReceiptState state);
/** "delivered" / "displayed" / "sent" as in the message-receipt signal. */
PidginReceiptState pidgin_receipt_state_from_string(const char *state);

/**
 * Reactions: emoji (char*) -> GList of senders (char*). Transfer none.
 * "reactions-changed" is emitted after each change.
 */
GHashTable *pidgin_message_get_reactions(PidginMessage *msg);
/** Returns FALSE if nothing changed. */
gboolean pidgin_message_add_reaction(PidginMessage *msg, const char *emoji,
                                     const char *sender);
gboolean pidgin_message_remove_reaction(PidginMessage *msg, const char *emoji,
                                        const char *sender);
gboolean pidgin_message_has_reaction(PidginMessage *msg, const char *emoji,
                                     const char *sender);
/** Emoji in first-reaction order (transfer container). */
GList *pidgin_message_get_reaction_emojis(PidginMessage *msg);

/** Retraction (XEP-0424, a Discord delete): the view shows a placeholder. */
gboolean pidgin_message_get_retracted(PidginMessage *msg);
void pidgin_message_set_retracted(PidginMessage *msg, gboolean retracted);

/** The row id in the message index (0: not indexed). */
gint64 pidgin_message_get_index_id(PidginMessage *msg);
void pidgin_message_set_index_id(PidginMessage *msg, gint64 id);

/**
 * Extra CSS classes for the message's row (M7: plugins, e.g. "history"
 * for the history plugin's rows). The view adds them to the row widget;
 * "notify::css-classes" follows changes. get_css_classes() returns a
 * NULL-terminated array (or NULL), owned by the message.
 */
void pidgin_message_add_css_class(PidginMessage *msg, const char *css_class);
void pidgin_message_remove_css_class(PidginMessage *msg, const char *css_class);
gboolean pidgin_message_has_css_class(PidginMessage *msg, const char *css_class);
const char * const *pidgin_message_get_css_classes(PidginMessage *msg);

G_END_DECLS

#endif /* _PIDGINMESSAGE_H_ */
