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
#include "jabber.h"
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
} JabberMessage;

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

/** The namespaces whose fallback we strip when the UI renders them natively. */
extern const char * const jabber_native_fallback_namespaces[];

#endif /* PURPLE_JABBER_MESSAGE_H_ */
