/**
 * @file message.h Message handlers
 *
 * purple
 *
 * Purple is the legal property of its developers, whose names are too numerous
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
#ifndef PURPLE_JABBER_MESSAGE_H_
#define PURPLE_JABBER_MESSAGE_H_

#include "buddy.h"
#include "chat.h"
#include "jabber.h"
#include "sfs.h"
#include "xmlnode.h"

/** Where a parsed message came from (M8: carbons and MAM unwrap to these). */
typedef enum {
	JABBER_MESSAGE_ORIGIN_LIVE = 0,
	JABBER_MESSAGE_ORIGIN_CARBON_RECEIVED, /**< XEP-0280 <received/> */
	JABBER_MESSAGE_ORIGIN_CARBON_SENT,     /**< XEP-0280 <sent/> */
	JABBER_MESSAGE_ORIGIN_MAM              /**< XEP-0313 <result/> */
} JabberMessageOrigin;

/** A XEP-0428 fallback range.  start == -1 means the whole body. */
typedef struct {
	char *ns;   /**< the "for" namespace (may be NULL) */
	int start;  /**< in Unicode code points */
	int end;    /**< exclusive */
} JabberFallback;

/**
 * Context for a message unwrapped from a carbon or an archive result.
 * NULL for live messages.
 */
typedef struct {
	JabberMessageOrigin origin;
	const char *mam_id;      /**< <result id=''>: the archive id */
	const char *mam_archive; /**< bare JID of the archive */
	const char *mam_kind;    /**< "catchup" or "older" */
	gboolean has_stamp;      /**< a <delay/> was on the <forwarded/> */
	time_t stamp;
} JabberMessageContext;

typedef struct _JabberMessage {
	JabberStream *js;
	enum {
		JABBER_MESSAGE_NORMAL,
		JABBER_MESSAGE_CHAT,
		JABBER_MESSAGE_GROUPCHAT,
		JABBER_MESSAGE_HEADLINE,
		JABBER_MESSAGE_ERROR,
		JABBER_MESSAGE_GROUPCHAT_INVITE,
		JABBER_MESSAGE_EVENT,
		JABBER_MESSAGE_OTHER
	} type;
	time_t sent;
	gboolean delayed;
	gboolean hasBuzz;
	char *id;
	char *from;
	char *to;
	char *subject;
	char *body;
	char *xhtml;
	char *password;
	char *error;
	char *thread_id;
	enum {
		JM_STATE_NONE,
		JM_STATE_ACTIVE,
		JM_STATE_COMPOSING,
		JM_STATE_PAUSED,
		JM_STATE_INACTIVE,
		JM_STATE_GONE
	} chat_state;
	GList *etc;
	GList *eventitems;

	/* M8 protocol core */
	JabberMessageOrigin origin;
	const JabberMessageContext *ctx; /**< NULL for live messages */
	gboolean outgoing;       /**< sent by this account from another device */
	char *body_raw;          /**< unescaped <body/> text */
	char *origin_id;         /**< XEP-0359 <origin-id/> */
	char *server_id;         /**< XEP-0359 <stanza-id/> by the archive */
	char *server_id_by;
	char *occupant_id;       /**< XEP-0421 */
	char *replace_id;        /**< XEP-0308 <replace id=''/> */
	char *reply_to_id;       /**< XEP-0461 <reply id=''/> */
	char *reply_to_jid;      /**< XEP-0461 <reply to=''/> */
	GList *fallbacks;        /**< XEP-0428, JabberFallback* */
	gboolean fallback_stripped;

	/* M8 message semantics.  On incoming messages these are parsed from
	 * the stanza; on outgoing ones (jabber_message_send) the flags and
	 * replace_id, reply_to_*, fallbacks add the matching elements. */
	gboolean receipt_request; /**< XEP-0184 <request/> */
	char *receipt_id;         /**< XEP-0184 <received id=''/> */
	const char *marker;       /**< XEP-0333 "received", "displayed", "acknowledged" */
	char *marker_id;
	gboolean markable;        /**< XEP-0333 <markable/> */
	xmlnode *reactions;       /**< XEP-0444 <reactions/> (points into the stanza) */
	char *retract_id;         /**< XEP-0424 target, or XEP-0425 target */
	gboolean moderated;       /**< XEP-0425: retract_id was moderated */
	char *moderated_by;       /**< XEP-0425 <moderated by=''/> */
	char *retract_reason;     /**< XEP-0425 <reason/> */
	gboolean unstyled;        /**< XEP-0393 <unstyled/> */
	gboolean store_hint;      /**< outgoing: XEP-0334 <store/> */

	/* M8 server features round 2 */
	JabberSfsFile *sfs;       /**< XEP-0447 (or XEP-0385) file metadata */
	char *eme_ns;             /**< XEP-0380 namespace we couldn't decrypt */
	char *eme_name;           /**< its display name */
} JabberMessage;

/**
 * Where the events carried by an incoming message belong (M8 message
 * semantics), with the same conv_name and sender as its
 * receiving-message-meta would carry.
 */
typedef struct {
	char *conv_name; /**< full JID (IM), room JID (MUC), counterpart (own) */
	char *sender;    /**< full JID (IM), nick (MUC), own full JID (own IM);
	                      NULL for the room itself */
	char *identity;  /**< stable sender key: bare JID (IM), occupant-id or
	                      nick (MUC) */
	JabberChat *chat;/**< the room, for groupchat messages */
	gboolean own;    /**< sent by this account (another device, or our own
	                      nick in a room) */
} JabberMessageTarget;

/**
 * The destination of a UI -> prpl IPC command (send-correction etc.).
 */
typedef struct {
	JabberStream *js;
	JabberChat *chat;    /**< set when conv_name is a joined room */
	char *to;            /**< the room's bare JID, or the peer's (full) JID */
	gboolean groupchat;  /**< send type='groupchat' */
} JabberIpcTarget;

void jabber_message_free(JabberMessage *jm);

void jabber_message_send(JabberMessage *jm);

void jabber_message_parse(JabberStream *js, xmlnode *packet);
int jabber_message_send_im(PurpleConnection *gc, const char *who, const char *msg,
		PurpleMessageFlags flags);
int jabber_message_send_chat(PurpleConnection *gc, int id, const char *message, PurpleMessageFlags flags);

unsigned int jabber_send_typing(PurpleConnection *gc, const char *who, PurpleTypingState state);

gboolean jabber_buzz_isenabled(JabberStream *js, const gchar *namespace);

gboolean jabber_custom_smileys_isenabled(JabberStream *js, const gchar *namespace);

/**
 * Parses a message unwrapped from a carbon or archive result.  @a ctx
 * says where it came from.  The jabber-receiving-message signal is not
 * emitted again for the inner message.
 */
void jabber_message_parse_with_context(JabberStream *js, xmlnode *packet,
		const JabberMessageContext *ctx);

/**
 * TRUE if the UI set "message-meta" = "1" in its ui_info, i.e. it handles
 * the receiving-message-meta etc. conversation signals.  Checked once and
 * cached.  Prpl code emits those signals only when this is TRUE.
 */
gboolean jabber_ui_supports_message_meta(void);

/** Test hook: forget the cached jabber_ui_supports_message_meta() answer. */
void jabber_ui_message_meta_reset(void);

/*
 * Pure helpers (xmlnode in, strings out), unit-tested.
 */

/** XEP-0359: the <stanza-id/> whose by equals @a by (a bare JID), or NULL. */
char *jabber_message_get_stanza_id(xmlnode *message, const char *by);

/** XEP-0359: the <origin-id/>, or NULL. */
char *jabber_message_get_origin_id(xmlnode *message);

/** XEP-0428: parses all <fallback/> children, returns a list of JabberFallback. */
GList *jabber_fallback_parse(xmlnode *message);
void jabber_fallback_free(GList *fallbacks);

/** "for:start-end;..." ("for:" alone for the whole body), or NULL for none. */
char *jabber_fallback_ranges_to_string(GList *fallbacks);

/**
 * Removes the fallback ranges whose namespace is in @a namespaces (NULL
 * terminated) from @a body.  Offsets are code points; invalid or
 * overlapping ranges are clamped.  Returns a new string; the result may
 * be empty.  Leading/trailing whitespace left by a stripped quote at the
 * start is removed.
 */
char *jabber_fallback_strip(const char *body, GList *fallbacks,
		const char * const *namespaces);

/**
 * XEP-0380: a display name for the encryption @a ns (@a name, the <encryption
 * name=''/> attribute, if given; else a known name; else @a ns itself).
 */
const char *jabber_eme_name(const char *ns, const char *name);

/**
 * XEP-0380: TRUE if @a body is (or looks like) the standard "your client
 * can't decrypt this" text that clients put next to encrypted payloads.
 */
gboolean jabber_eme_is_fallback_body(const char *body, const char *name);

/** The namespaces whose fallback we strip when the UI renders them natively. */
extern const char * const jabber_native_fallback_namespaces[];

/*
 * M8 message semantics (receipts.c, correction.c, reactions.c,
 * retraction.c, displayed.c, styling.c)
 */

/** Registers the send-* IPC commands and the disco features. */
void jabber_message_semantics_init(PurplePlugin *plugin);
void jabber_message_semantics_uninit(void);

/**
 * Resolves the account and conversation name an IPC command received.
 * FALSE if the account is not a connected XMPP account.  Clear @a t with
 * jabber_ipc_target_clear() either way.
 */
gboolean jabber_ipc_target_init(JabberIpcTarget *t, PurpleAccount *account,
		const char *conv_name);
void jabber_ipc_target_clear(JabberIpcTarget *t);

/**
 * A new <message/> to @a to (type chat or groupchat) with a fresh UUID id
 * that is also its XEP-0359 origin-id.  The id is remembered so the same
 * stanza coming back from the archive is not shown twice.
 */
xmlnode *jabber_message_stanza_new(JabberStream *js, const char *to,
		gboolean groupchat);

/** Our own full JID (newly allocated). */
char *jabber_message_own_jid(JabberStream *js, gboolean bare);

/**
 * Escaped, human-readable name for the sender of @a t: the nick in rooms,
 * the buddy alias or bare JID in IMs, our own alias for own messages.
 */
char *jabber_message_display_name(JabberStream *js, const JabberMessageTarget *t);

/**
 * Writes a readable text line (already escaped markup) for an event that
 * no UI rendered natively (contract rule 7: logs stay self-contained).
 */
void jabber_message_write_event(JabberStream *js, const JabberMessageTarget *t,
		const char *markup, time_t when, gboolean delayed);

/**
 * A new outgoing message with a body (plain text, as it goes on the wire)
 * for an IPC command: type chat or groupchat, chat state active for IMs.
 * Send it with jabber_message_send_ipc().
 */
JabberMessage *jabber_message_new_outgoing(const JabberIpcTarget *t,
		const char *body);

/**
 * Assigns the ids (UUID id = origin-id), adds the receipt request (IMs)
 * and <markable/> when the UI handles message metadata, sends @a jm and
 * emits sending-message-meta for @a conv_name with the usual keys plus
 * @a extra (key, value, ..., NULL; NULL values are skipped).
 */
void jabber_message_send_ipc(JabberMessage *jm, const char *conv_name,
		const char *first_key, ...) G_GNUC_NULL_TERMINATED;

/** Plain text -> libpurple markup (escaped, newlines as <br>). */
char *jabber_message_plain_to_markup(const char *plain);

/** The IM conversation for @a name (full or bare JID), created if missing. */
PurpleConversation *jabber_message_im_conv(PurpleAccount *account,
		const char *name);

#endif /* PURPLE_JABBER_MESSAGE_H_ */
