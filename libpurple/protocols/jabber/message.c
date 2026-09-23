/*
 * purple - Jabber Protocol Plugin
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
 *
 */
#include "internal.h"

#include "core.h"
#include "debug.h"
#include "notify.h"
#include "server.h"
#include "signals.h"
#include "util.h"
#include "adhoccommands.h"
#include "buddy.h"
#include "carbons.h"
#include "chat.h"
#include "data.h"
#include "google/google.h"
#include "message.h"
#include "xmlnode.h"
#include "pep.h"
#include "smiley.h"
#include "iq.h"
#include "oob.h"
#include "mam.h"
#include "correction.h"
#include "displayed.h"
#include "reactions.h"
#include "receipts.h"
#include "retraction.h"
#include "styling.h"

#include <string.h>

/**************************************************************************
 * UI capability: "message-meta" (see doc/PIDGIN-UPGRADE.md, M8)
 **************************************************************************/

static int ui_message_meta = -1; /* -1 unknown, 0 no, 1 yes */

gboolean
jabber_ui_supports_message_meta(void)
{
	if (ui_message_meta < 0) {
		GHashTable *ui_info = purple_core_get_ui_info();
		const char *value = ui_info ?
			g_hash_table_lookup(ui_info, "message-meta") : NULL;

		ui_message_meta = purple_strequal(value, "1") ? 1 : 0;
		purple_debug_info("jabber", "UI %s message metadata signals\n",
		                  ui_message_meta ? "handles" : "does not handle");
	}

	return ui_message_meta == 1;
}

void
jabber_ui_message_meta_reset(void)
{
	ui_message_meta = -1;
}

/**************************************************************************
 * Pure helpers: XEP-0359 ids, XEP-0428 fallback ranges
 **************************************************************************/

/* Namespaces whose fallback body text is stripped when the UI renders the
 * feature natively (it sees reply-to/reply-to-sender in the metadata, and
 * reactions arrive as message-reaction). */
const char * const jabber_native_fallback_namespaces[] = {
	NS_REPLY,
	NS_REACTIONS,
	NULL
};

static gboolean
jid_equal_bare(const char *a, const char *b)
{
	/* The "by" of a stanza-id is a bare JID; compare case-insensitively,
	 * which is what node/domain preparation amounts to for ASCII JIDs. */
	return a != NULL && b != NULL && g_ascii_strcasecmp(a, b) == 0;
}

char *
jabber_message_get_stanza_id(xmlnode *message, const char *by)
{
	xmlnode *child;

	if (message == NULL || by == NULL)
		return NULL;

	for (child = xmlnode_get_child_with_namespace(message, "stanza-id", NS_SID);
	     child; child = xmlnode_get_next_twin(child)) {
		const char *id = xmlnode_get_attrib(child, "id");

		if (id && *id && jid_equal_bare(xmlnode_get_attrib(child, "by"), by))
			return g_strdup(id);
	}

	return NULL;
}

char *
jabber_message_get_origin_id(xmlnode *message)
{
	xmlnode *child;
	const char *id;

	if (message == NULL)
		return NULL;

	child = xmlnode_get_child_with_namespace(message, "origin-id", NS_SID);
	id = child ? xmlnode_get_attrib(child, "id") : NULL;

	return (id && *id) ? g_strdup(id) : NULL;
}

static gboolean
parse_offset(const char *str, int *out)
{
	char *end = NULL;
	gint64 value;

	if (str == NULL || *str == '\0')
		return FALSE;

	value = g_ascii_strtoll(str, &end, 10);
	if (end == NULL || *end != '\0' || value < 0 || value > G_MAXINT)
		return FALSE;

	*out = (int)value;
	return TRUE;
}

GList *
jabber_fallback_parse(xmlnode *message)
{
	GList *list = NULL;
	xmlnode *fallback;

	if (message == NULL)
		return NULL;

	for (fallback = xmlnode_get_child_with_namespace(message, "fallback", NS_FALLBACK);
	     fallback; fallback = xmlnode_get_next_twin(fallback)) {
		const char *ns = xmlnode_get_attrib(fallback, "for");
		xmlnode *body;
		gboolean any_body = FALSE;

		for (body = xmlnode_get_child(fallback, "body"); body;
		     body = xmlnode_get_next_twin(body)) {
			JabberFallback *fb;
			int start, end;

			any_body = TRUE;
			if (!parse_offset(xmlnode_get_attrib(body, "start"), &start) ||
			    !parse_offset(xmlnode_get_attrib(body, "end"), &end) ||
			    end < start) {
				purple_debug_warning("jabber", "Ignoring invalid "
				                     "fallback range\n");
				continue;
			}

			fb = g_new0(JabberFallback, 1);
			fb->ns = g_strdup(ns);
			fb->start = start;
			fb->end = end;
			list = g_list_append(list, fb);
		}

		/* A <fallback/> without <body/> (or only <subject/>) covers the
		 * whole body. */
		if (!any_body && xmlnode_get_child(fallback, "subject") == NULL) {
			JabberFallback *fb = g_new0(JabberFallback, 1);
			fb->ns = g_strdup(ns);
			fb->start = -1;
			fb->end = -1;
			list = g_list_append(list, fb);
		}
	}

	return list;
}

void
jabber_fallback_free(GList *fallbacks)
{
	while (fallbacks) {
		JabberFallback *fb = fallbacks->data;
		g_free(fb->ns);
		g_free(fb);
		fallbacks = g_list_delete_link(fallbacks, fallbacks);
	}
}

char *
jabber_fallback_ranges_to_string(GList *fallbacks)
{
	GString *str;

	if (fallbacks == NULL)
		return NULL;

	str = g_string_new(NULL);
	for (; fallbacks; fallbacks = fallbacks->next) {
		JabberFallback *fb = fallbacks->data;

		if (str->len)
			g_string_append_c(str, ';');
		if (fb->start < 0)
			g_string_append_printf(str, "%s:", fb->ns ? fb->ns : "");
		else
			g_string_append_printf(str, "%s:%d-%d", fb->ns ? fb->ns : "",
			                       fb->start, fb->end);
	}

	return g_string_free(str, FALSE);
}

static gboolean
ns_in_list(const char *ns, const char * const *namespaces)
{
	for (; namespaces && *namespaces; namespaces++)
		if (purple_strequal(ns, *namespaces))
			return TRUE;
	return FALSE;
}

char *
jabber_fallback_strip(const char *body, GList *fallbacks,
                      const char * const *namespaces)
{
	glong len, i;
	gboolean *drop;
	gboolean dropped_head = FALSE, dropped_tail = FALSE;
	GString *out;
	const char *p;

	if (body == NULL)
		return NULL;

	if (!g_utf8_validate(body, -1, NULL))
		return g_strdup(body);

	len = g_utf8_strlen(body, -1);
	drop = g_new0(gboolean, len + 1);

	for (; fallbacks; fallbacks = fallbacks->next) {
		JabberFallback *fb = fallbacks->data;
		glong start, end;

		if (!ns_in_list(fb->ns, namespaces))
			continue;

		if (fb->start < 0) {
			start = 0;
			end = len;
		} else {
			start = MIN(fb->start, len);
			end = MIN(fb->end, len);
		}

		for (i = start; i < end; i++)
			drop[i] = TRUE;
		if (start == 0 && end > 0)
			dropped_head = TRUE;
		if (end == len && end > start)
			dropped_tail = TRUE;
	}

	out = g_string_sized_new(strlen(body));
	for (i = 0, p = body; i < len; i++, p = g_utf8_next_char(p)) {
		if (!drop[i])
			g_string_append_len(out, p, g_utf8_next_char(p) - p);
	}
	g_free(drop);

	if (dropped_head) {
		gsize n = 0;
		while (n < out->len && g_ascii_isspace(out->str[n]))
			n++;
		g_string_erase(out, 0, n);
	}
	if (dropped_tail) {
		while (out->len && g_ascii_isspace(out->str[out->len - 1]))
			g_string_truncate(out, out->len - 1);
	}

	return g_string_free(out, FALSE);
}

/**************************************************************************
 * receiving-message-meta
 **************************************************************************/

static void
meta_set(GHashTable *meta, const char *key, const char *value)
{
	if (value != NULL)
		g_hash_table_insert(meta, g_strdup(key), g_strdup(value));
}

/*
 * Emits receiving-message-meta for @jm, if the UI wants it.  Returns TRUE
 * if a handler asked for the message to be discarded.
 */
static gboolean
jabber_message_emit_meta(JabberMessage *jm, const char *conv_name,
                         const char *conv_type, const char *sender)
{
	GHashTable *meta;
	PurpleAccount *account;
	char *tmp;
	gboolean discard;

	if (!jabber_ui_supports_message_meta())
		return FALSE;

	account = purple_connection_get_account(jm->js->gc);
	meta = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	meta_set(meta, "conv-type", conv_type);
	meta_set(meta, "sender", sender);
	if (jm->outgoing)
		meta_set(meta, "outgoing", "1");
	tmp = g_strdup_printf("%" G_GINT64_FORMAT, (gint64)jm->sent);
	meta_set(meta, "timestamp", tmp);
	g_free(tmp);

	meta_set(meta, "stanza-id", jm->id);
	meta_set(meta, "origin-id", jm->origin_id);
	meta_set(meta, "server-id", jm->server_id);
	meta_set(meta, "server-id-by", jm->server_id_by);
	meta_set(meta, "occupant-id", jm->occupant_id);
	meta_set(meta, "correction-of", jm->replace_id);
	meta_set(meta, "reply-to", jm->reply_to_id);
	meta_set(meta, "reply-to-sender", jm->reply_to_jid);
	if (jm->markable)
		meta_set(meta, "markable", "1");
	if (jm->unstyled)
		meta_set(meta, "unstyled", "1");

	tmp = jabber_fallback_ranges_to_string(jm->fallbacks);
	meta_set(meta, "fallback-ranges", tmp);
	g_free(tmp);
	if (jm->fallback_stripped)
		meta_set(meta, "fallback-stripped", "1");

	switch (jm->origin) {
		case JABBER_MESSAGE_ORIGIN_CARBON_RECEIVED:
			meta_set(meta, "carbon", "received");
			break;
		case JABBER_MESSAGE_ORIGIN_CARBON_SENT:
			meta_set(meta, "carbon", "sent");
			break;
		case JABBER_MESSAGE_ORIGIN_MAM:
			meta_set(meta, "mam", "1");
			if (jm->ctx)
				meta_set(meta, "mam-query", jm->ctx->mam_kind);
			break;
		case JABBER_MESSAGE_ORIGIN_LIVE:
			break;
	}

	purple_signal_emit(purple_conversations_get_handle(),
	                   "receiving-message-meta", account, conv_name, meta);

	discard = purple_strequal(g_hash_table_lookup(meta, "discard"), "1");
	g_hash_table_destroy(meta);

	if (discard)
		purple_debug_info("jabber", "UI discarded message %s in %s\n",
		                  jm->server_id ? jm->server_id :
		                  (jm->id ? jm->id : "(no id)"), conv_name);

	return discard;
}

static GString *jm_body_with_oob(JabberMessage *jm) {
	GList *etc;
	GString *body = g_string_new("");

	if(jm->xhtml)
		g_string_append(body, jm->xhtml);
	else if(jm->body)
		g_string_append(body, jm->body);

	for(etc = jm->etc; etc; etc = etc->next) {
		xmlnode *x = etc->data;
		const char *xmlns = xmlnode_get_namespace(x);
		/* M8 (HTTP upload): OOB handling lives in oob.c. */
		if(purple_strequal(xmlns, NS_OOB_X_DATA))
			jabber_oob_x_append_to_body(body, x);
	}

	return body;
}

void jabber_message_free(JabberMessage *jm)
{
	g_free(jm->from);
	g_free(jm->to);
	g_free(jm->id);
	g_free(jm->subject);
	g_free(jm->body);
	g_free(jm->xhtml);
	g_free(jm->password);
	g_free(jm->error);
	g_free(jm->thread_id);
	g_list_free(jm->etc);
	g_list_free(jm->eventitems);

	g_free(jm->body_raw);
	g_free(jm->origin_id);
	g_free(jm->server_id);
	g_free(jm->server_id_by);
	g_free(jm->occupant_id);
	g_free(jm->replace_id);
	g_free(jm->reply_to_id);
	g_free(jm->reply_to_jid);
	jabber_fallback_free(jm->fallbacks);

	g_free(jm->receipt_id);
	g_free(jm->marker_id);
	g_free(jm->retract_id);
	g_free(jm->moderated_by);
	g_free(jm->retract_reason);

	g_free(jm);
}

/*
 * A message this account sent from another device: a XEP-0280 sent carbon,
 * or an outgoing message from the archive.  Written into the conversation
 * with the counterpart as a sent message; never goes through serv_got_im(),
 * which would treat it as received (and could send an auto-response).
 */
static void handle_outgoing_chat(JabberMessage *jm)
{
	PurpleAccount *account = purple_connection_get_account(jm->js->gc);
	PurpleConversation *conv;
	PurpleMessageFlags flags;
	GString *body;
	char *own;

	if (jm->to == NULL)
		return;

	body = jm_body_with_oob(jm);
	if (body->len == 0) {
		/* Our own markers, receipts, reactions and retractions sent
		 * from another device have no body; jabber_message_semantics()
		 * has already handled them. */
		g_string_free(body, TRUE);
		return;
	}

	own = jabber_id_get_full_jid(jm->js->user);
	if (jabber_message_emit_meta(jm, jm->to, "im", own)) {
		g_free(own);
		g_string_free(body, TRUE);
		return;
	}
	g_free(own);

	conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM,
	                                             jm->to, account);
	if (conv == NULL) {
		char *bare = jabber_get_bare_jid(jm->to);
		conv = purple_conversation_new(PURPLE_CONV_TYPE_IM, account,
		                               bare ? bare : jm->to);
		g_free(bare);
	}

	flags = PURPLE_MESSAGE_SEND | PURPLE_MESSAGE_REMOTE_SEND;
	if (jm->origin == JABBER_MESSAGE_ORIGIN_MAM)
		flags |= PURPLE_MESSAGE_DELAYED;

	purple_conv_im_write(PURPLE_CONV_IM(conv), NULL, body->str, flags,
	                     jm->sent);
	g_string_free(body, TRUE);
}

static void handle_chat(JabberMessage *jm)
{
	JabberID *jid;

	PurpleConnection *gc;
	PurpleAccount *account;
	JabberBuddy *jb;
	JabberBuddyResource *jbr;
	GString *body;
	gboolean archived = (jm->origin == JABBER_MESSAGE_ORIGIN_MAM);

	if (jm->outgoing) {
		handle_outgoing_chat(jm);
		return;
	}

	jid = jabber_id_new(jm->from);
	if(!jid)
		return;

	gc = jm->js->gc;
	account = purple_connection_get_account(gc);

	jb = jabber_buddy_find(jm->js, jm->from, TRUE);
	jbr = jabber_buddy_find_resource(jb, jid->resource);

	if (jbr && jm->chat_state != JM_STATE_NONE && !archived)
		jbr->chat_states = JABBER_CHAT_STATES_SUPPORTED;

	/* Archived messages say nothing about the current typing state. */
	if (!archived)
	switch(jm->chat_state) {
		case JM_STATE_COMPOSING:
			serv_got_typing(gc, jm->from, 0, PURPLE_TYPING);
			break;
		case JM_STATE_PAUSED:
			serv_got_typing(gc, jm->from, 0, PURPLE_TYPED);
			break;
		case JM_STATE_GONE: {
			PurpleConversation *conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM,
					jm->from, account);
			if (conv && jid->node && jid->domain) {
				char buf[256];
				PurpleBuddy *buddy;

				g_snprintf(buf, sizeof(buf), "%s@%s", jid->node, jid->domain);

				if ((buddy = purple_find_buddy(account, buf))) {
					const char *who;
					char *escaped;

					who = purple_buddy_get_alias(buddy);
					escaped = g_markup_escape_text(who, -1);

					g_snprintf(buf, sizeof(buf),
					           _("%s has left the conversation."), escaped);
					g_free(escaped);

					/* At some point when we restructure PurpleConversation,
					 * this should be able to be implemented by removing the
					 * user from the conversation like we do with chats now. */
					purple_conversation_write(conv, "", buf,
					                        PURPLE_MESSAGE_SYSTEM, time(NULL));
				}
			}
			serv_got_typing_stopped(gc, jm->from);
			break;
		}
		default:
			serv_got_typing_stopped(gc, jm->from);
	}

	if (jm->js->googletalk && jm->body && jm->xhtml == NULL) {
		char *tmp = jm->body;
		jm->body = jabber_google_format_to_html(jm->body);
		g_free(tmp);
	}

	body = jm_body_with_oob(jm);

	if(body && body->len) {
		if (jid->resource && !archived) {
			/*
			 * We received a message from a specific resource, so
			 * we probably want a reply to go to this specific
			 * resource (i.e. bind/lock the conversation to this
			 * resource).
			 *
			 * This works because purple_conv_im_send gets the name
			 * from purple_conversation_get_name()
			 */
			PurpleConversation *conv;

			conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM,
			                                             jm->from, account);
			if (conv && !purple_strequal(jm->from,
			                         purple_conversation_get_name(conv))) {
				purple_debug_info("jabber", "Binding conversation to %s\n",
				                  jm->from);
				purple_conversation_set_name(conv, jm->from);
			}
		}

		if(jbr && !archived) {
			/* Treat SUPPORTED as a terminal with no escape :) */
			if (jbr->chat_states != JABBER_CHAT_STATES_SUPPORTED) {
				if (jm->chat_state != JM_STATE_NONE)
					jbr->chat_states = JABBER_CHAT_STATES_SUPPORTED;
				else
					jbr->chat_states = JABBER_CHAT_STATES_UNSUPPORTED;
			}

			if(jbr->thread_id)
				g_free(jbr->thread_id);
			jbr->thread_id = g_strdup(jm->thread_id);
		}

		if (!jabber_message_emit_meta(jm, jm->from, "im", jm->from))
			serv_got_im(gc, jm->from, body->str,
			            archived ? PURPLE_MESSAGE_DELAYED : 0, jm->sent);
	}

	jabber_id_free(jid);

	if(body)
		g_string_free(body, TRUE);
}

static void handle_headline(JabberMessage *jm)
{
	char *title;
	GString *body;

	if(!jm->xhtml && !jm->body)
		return; /* ignore headlines without any content */

	body = jm_body_with_oob(jm);
	title = g_strdup_printf(_("Message from %s"), jm->from);

	purple_notify_formatted(jm->js->gc, title, jm->subject ? jm->subject : title,
			NULL, body->str, NULL, NULL);

	g_free(title);
	g_string_free(body, TRUE);
}

static void handle_groupchat(JabberMessage *jm)
{
	JabberID *jid = jabber_id_new(jm->from);
	JabberChat *chat;

	if(!jid)
		return;

	chat = jabber_chat_find(jm->js, jid->node, jid->domain);

	if(!chat)
		return;

	if (jm->origin == JABBER_MESSAGE_ORIGIN_LIVE)
		chat->selfping_last_activity = time(NULL);

	if(jm->subject && jm->origin != JABBER_MESSAGE_ORIGIN_MAM) {
		purple_conv_chat_set_topic(PURPLE_CONV_CHAT(chat->conv), jid->resource,
				jm->subject);
		if(!jm->xhtml && !jm->body) {
			char *msg, *tmp, *tmp2;
			tmp = g_markup_escape_text(jm->subject, -1);
			tmp2 = purple_markup_linkify(tmp);
			if(jid->resource)
				msg = g_strdup_printf(_("%s has set the topic to: %s"), jid->resource, tmp2);
			else
				msg = g_strdup_printf(_("The topic is: %s"), tmp2);
			purple_conv_chat_write(PURPLE_CONV_CHAT(chat->conv), "", msg, PURPLE_MESSAGE_SYSTEM, jm->sent);
			g_free(tmp);
			g_free(tmp2);
			g_free(msg);
		}
	}

	if(jm->xhtml || jm->body) {
		if(jid->resource) {
			char *room = g_strdup_printf("%s@%s", jid->node, jid->domain);
			gboolean discard = jabber_message_emit_meta(jm,
					chat->conv ? purple_conversation_get_name(chat->conv) : room,
					"chat", jid->resource);
			g_free(room);
			if (!discard)
				serv_got_chat_in(jm->js->gc, chat->id, jid->resource,
								jm->delayed ? PURPLE_MESSAGE_DELAYED : 0,
								jm->xhtml ? jm->xhtml : jm->body, jm->sent);
		}
		else if(chat->muc)
			purple_conv_chat_write(PURPLE_CONV_CHAT(chat->conv), "",
							jm->xhtml ? jm->xhtml : jm->body,
							PURPLE_MESSAGE_SYSTEM, jm->sent);
	}

	jabber_id_free(jid);
}

static void handle_groupchat_invite(JabberMessage *jm)
{
	GHashTable *components;
	JabberID *jid = jabber_id_new(jm->to);

	if(!jid)
		return;

	components = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);

	g_hash_table_replace(components, "room", g_strdup(jid->node));
	g_hash_table_replace(components, "server", g_strdup(jid->domain));
	g_hash_table_replace(components, "handle", g_strdup(jm->js->user->node));
	g_hash_table_replace(components, "password", g_strdup(jm->password));

	jabber_id_free(jid);
	serv_got_chat_invite(jm->js->gc, jm->to, jm->from, jm->body, components);
}

static void handle_error(JabberMessage *jm)
{
	char *buf;

	if(!jm->body)
		return;

	buf = g_strdup_printf(_("Message delivery to %s failed: %s"),
			jm->from, jm->error ? jm->error : "");

	purple_notify_formatted(jm->js->gc, _("XMPP Message Error"), _("XMPP Message Error"), buf,
			jm->xhtml ? jm->xhtml : jm->body, NULL, NULL);

	g_free(buf);
}

static void handle_buzz(JabberMessage *jm) {
	PurpleAccount *account;

	/* Delayed buzz MUST NOT be accepted */
	if(jm->delayed)
		return;

	/* Reject buzz when it's not enabled */
	if(!jm->js->allowBuzz)
		return;

	account = purple_connection_get_account(jm->js->gc);

	if (purple_find_buddy(account, jm->from) == NULL)
		return; /* Do not accept buzzes from unknown people */

	/* xmpp only has 1 attention type, so index is 0 */
	purple_prpl_got_attention(jm->js->gc, jm->from, 0);
}

/* used internally by the functions below */
typedef struct {
	gchar *cid;
	gchar *alt;
} JabberSmileyRef;


static void
jabber_message_get_refs_from_xmlnode_internal(const xmlnode *message,
	GHashTable *table)
{
	xmlnode *child;

	for (child = xmlnode_get_child(message, "img") ; child ;
		 child = xmlnode_get_next_twin(child)) {
		const gchar *src = xmlnode_get_attrib(child, "src");

		if (g_str_has_prefix(src, "cid:")) {
			const gchar *cid = src + 4;

			/* if we haven't "fetched" this yet... */
			if (!g_hash_table_lookup(table, cid)) {
				/* take a copy of the cid and let the SmileyRef own it... */
				gchar *temp_cid = g_strdup(cid);
				JabberSmileyRef *ref = g_new0(JabberSmileyRef, 1);
				const gchar *alt = xmlnode_get_attrib(child, "alt");
				ref->cid = temp_cid;
				/* if there is no "alt" string, use the cid...
				 include the entire src, eg. "cid:.." to avoid linkification */
				if (alt && alt[0] != '\0') {
					/* workaround for when "alt" is set to the value of the
					 CID (which Jabbim seems to do), to avoid it showing up
						 as an mailto: link */
					if (purple_email_is_valid(alt)) {
						ref->alt = g_strdup_printf("smiley:%s", alt);
					} else {
						ref->alt = g_strdup(alt);
					}
				} else {
					ref->alt = g_strdup(src);
				}
				g_hash_table_insert(table, temp_cid, ref);
			}
		}
	}

	for (child = message->child ; child ; child = child->next) {
		jabber_message_get_refs_from_xmlnode_internal(child, table);
	}
}

static gboolean
jabber_message_get_refs_steal(gpointer key, gpointer value, gpointer user_data)
{
	GList **refs = (GList **) user_data;
	JabberSmileyRef *ref = (JabberSmileyRef *) value;

	*refs = g_list_append(*refs, ref);

	return TRUE;
}

static GList *
jabber_message_get_refs_from_xmlnode(const xmlnode *message)
{
	GList *refs = NULL;
	GHashTable *unique_refs = g_hash_table_new(g_str_hash, g_str_equal);

	jabber_message_get_refs_from_xmlnode_internal(message, unique_refs);
	(void) g_hash_table_foreach_steal(unique_refs,
		jabber_message_get_refs_steal, (gpointer) &refs);
	g_hash_table_destroy(unique_refs);
	return refs;
}

static gchar *
jabber_message_xml_to_string_strip_img_smileys(xmlnode *xhtml)
{
	gchar *markup = xmlnode_to_str(xhtml, NULL);
	int len = strlen(markup);
	int pos = 0;
	GString *out = g_string_new(NULL);

	while (pos < len) {
		/* this is a bit cludgy, maybe there is a better way to do this...
		  we need to find all <img> tags within the XHTML and replace those
			tags with the value of their "alt" attributes */
		if (g_str_has_prefix(&(markup[pos]), "<img")) {
			xmlnode *img = NULL;
			int pos2 = pos;
			const gchar *src;

			for (; pos2 < len ; pos2++) {
				if (g_str_has_prefix(&(markup[pos2]), "/>")) {
					pos2 += 2;
					break;
				} else if (g_str_has_prefix(&(markup[pos2]), "</img>")) {
					pos2 += 5;
					break;
				}
			}

			/* note, if the above loop didn't find the end of the <img> tag,
			  it the parsed string will be until the end of the input string,
			  in which case xmlnode_from_str will bail out and return NULL,
			  in this case the "if" statement below doesn't trigger and the
			  text is copied unchanged */
			img = xmlnode_from_str(&(markup[pos]), pos2 - pos);
			src = xmlnode_get_attrib(img, "src");

			if (g_str_has_prefix(src, "cid:")) {
				const gchar *alt = xmlnode_get_attrib(img, "alt");
				/* if the "alt" attribute is empty, put the cid as smiley string */
				if (alt && alt[0] != '\0') {
					/* if the "alt" is the same as the CID, as Jabbim does,
					 this prevents linkification... */
					if (purple_email_is_valid(alt)) {
						gchar *safe_alt = g_strdup_printf("smiley:%s", alt);
						out = g_string_append(out, safe_alt);
						g_free(safe_alt);
					} else {
						out = g_string_append(out, alt);
					}
				} else {
					out = g_string_append(out, src);
				}
				pos += pos2 - pos;
			} else {
				out = g_string_append_c(out, markup[pos]);
				pos++;
			}

			xmlnode_free(img);

		} else {
			out = g_string_append_c(out, markup[pos]);
			pos++;
		}
	}

	g_free(markup);
	return g_string_free(out, FALSE);
}

static void
jabber_message_add_remote_smileys(JabberStream *js, const gchar *who,
    const xmlnode *message)
{
	xmlnode *data_tag;
	for (data_tag = xmlnode_get_child_with_namespace(message, "data", NS_BOB) ;
		 data_tag ;
		 data_tag = xmlnode_get_next_twin(data_tag)) {
		const gchar *cid = xmlnode_get_attrib(data_tag, "cid");
		const JabberData *data = jabber_data_find_remote_by_cid(js, who, cid);

		if (!data && cid != NULL) {
			/* we haven't cached this already, let's add it */
			JabberData *new_data = jabber_data_create_from_xml(data_tag);

			if (new_data) {
				jabber_data_associate_remote(js, who, new_data);
			}
		}
	}
}

static void
jabber_message_request_data_cb(JabberData *data, gchar *alt,
    gpointer userdata)
{
	PurpleConversation *conv = (PurpleConversation *) userdata;

	if (data) {
		purple_conv_custom_smiley_write(conv, alt,
										jabber_data_get_data(data),
										jabber_data_get_size(data));
		purple_conv_custom_smiley_close(conv, alt);
	}

	g_free(alt);
}

/*
 * Children of <message/> handled by the M8 code.  Returns TRUE if @child
 * was consumed.  This is where the message-semantics XEPs slot in; the
 * ones marked TODO are parsed by the follow-up work and must also be
 * emitted as the matching conversation signals (only when
 * jabber_ui_supports_message_meta()), keeping today's text output
 * otherwise.
 */
static gboolean
jabber_message_parse_modern_child(JabberMessage *jm, xmlnode *child,
                                  const char *xmlns)
{
	const char *name = child->name;

	if (purple_strequal(xmlns, NS_SID)) {
		/* XEP-0359 <stanza-id/>, <origin-id/>: read after the loop, since
		 * which "by" to trust depends on the message type. */
		return TRUE;
	} else if (purple_strequal(xmlns, NS_FALLBACK)) {
		/* XEP-0428: parsed after the loop (jabber_fallback_parse). */
		return TRUE;
	} else if (purple_strequal(xmlns, NS_OCCUPANT_ID)) {
		/* XEP-0421: trusted only for rooms that advertise it (checked
		 * after the loop). */
		if (purple_strequal(name, "occupant-id") && !jm->occupant_id) {
			const char *id = xmlnode_get_attrib(child, "id");
			jm->occupant_id = (id && *id) ? g_strdup(id) : NULL;
		}
		return TRUE;
	} else if (purple_strequal(xmlns, NS_REPLY)) {
		/* XEP-0461: reply-to / reply-to-sender in the metadata. */
		if (purple_strequal(name, "reply") && !jm->reply_to_id) {
			jm->reply_to_id = g_strdup(xmlnode_get_attrib(child, "id"));
			jm->reply_to_jid = g_strdup(xmlnode_get_attrib(child, "to"));
		}
		return TRUE;
	} else if (purple_strequal(xmlns, NS_MESSAGE_CORRECT)) {
		/* XEP-0308: correction-of in the metadata; message-corrected is
		 * emitted by jabber_message_semantics() (correction.c). */
		if (purple_strequal(name, "replace") && !jm->replace_id) {
			const char *id = xmlnode_get_attrib(child, "id");
			jm->replace_id = (id && *id) ? g_strdup(id) : NULL;
		}
		return TRUE;
	} else if (purple_strequal(xmlns, NS_REACTIONS)) {
		/* XEP-0444: reactions.c.  The fallback body is stripped when the
		 * UI handles message-meta (jabber_native_fallback_namespaces). */
		if (purple_strequal(name, "reactions") && !jm->reactions)
			jm->reactions = child;
		return TRUE;
	} else if (purple_strequal(xmlns, NS_RECEIPTS)) {
		/* XEP-0184: receipts.c */
		if (purple_strequal(name, "request")) {
			jm->receipt_request = TRUE;
		} else if (purple_strequal(name, "received") && !jm->receipt_id) {
			const char *id = xmlnode_get_attrib(child, "id");
			jm->receipt_id = (id && *id) ? g_strdup(id) : NULL;
		}
		return TRUE;
	} else if (purple_strequal(xmlns, NS_CHAT_MARKERS)) {
		/* XEP-0333: receipts.c; <markable/> goes into the metadata. */
		if (purple_strequal(name, "markable")) {
			jm->markable = TRUE;
		} else if (!jm->marker && (purple_strequal(name, "received") ||
		                           purple_strequal(name, "displayed") ||
		                           purple_strequal(name, "acknowledged"))) {
			const char *id = xmlnode_get_attrib(child, "id");
			if (id && *id) {
				jm->marker = purple_strequal(name, "received") ? "received" :
					purple_strequal(name, "displayed") ? "displayed" :
					"acknowledged";
				jm->marker_id = g_strdup(id);
			}
		}
		return TRUE;
	} else if (jabber_retraction_parse(jm, child, xmlns)) {
		/* XEP-0424/0425 (and the XEP-0422 fastening forms) */
		return TRUE;
	} else if (purple_strequal(xmlns, NS_STYLING)) {
		/* XEP-0393: the UI must not style this body. */
		if (purple_strequal(name, "unstyled"))
			jm->unstyled = TRUE;
		return TRUE;
	} else if (purple_strequal(xmlns, NS_HINTS) ||
	           purple_strequal(xmlns, NS_CARBONS)) {
		/* XEP-0334 hints and a carbons <private/> on a live message:
		 * nothing to do on receipt. */
		return TRUE;
	}

	return FALSE;
}

/* Fills the M8 fields after the child loop.  Returns FALSE if the message
 * must be dropped (a duplicate, or not acceptable in its context). */
static gboolean
jabber_message_post_process(JabberMessage *jm, xmlnode *packet)
{
	JabberStream *js = jm->js;
	PurpleAccount *account = purple_connection_get_account(js->gc);
	const JabberMessageContext *ctx = jm->ctx;
	char *own_bare = jabber_id_get_bare_jid(js->user);
	char *from_bare = jm->from ? jabber_get_bare_jid(jm->from) : NULL;
	gboolean keep = TRUE;

	if (ctx && ctx->has_stamp) {
		/* The <delay/> on <forwarded/> is authoritative for archives. */
		jm->delayed = TRUE;
		jm->sent = ctx->stamp;
	}

	/* XEP-0359: only the stanza-id added by our own archive (1:1) or by
	 * the room (MUC) can be trusted. */
	jm->origin_id = jabber_message_get_origin_id(packet);
	if (ctx && ctx->origin == JABBER_MESSAGE_ORIGIN_MAM && ctx->mam_id) {
		jm->server_id = g_strdup(ctx->mam_id);
		jm->server_id_by = g_strdup(ctx->mam_archive);
	} else {
		const char *by = (jm->type == JABBER_MESSAGE_GROUPCHAT) ?
			from_bare : own_bare;
		jm->server_id = jabber_message_get_stanza_id(packet, by);
		if (jm->server_id)
			jm->server_id_by = g_strdup(by);
	}

	/* Who sent it: us, from another device? */
	if (jm->origin == JABBER_MESSAGE_ORIGIN_CARBON_SENT)
		jm->outgoing = TRUE;
	else if (jm->origin == JABBER_MESSAGE_ORIGIN_MAM &&
	         jm->type != JABBER_MESSAGE_GROUPCHAT &&
	         from_bare && g_ascii_strcasecmp(from_bare, own_bare) == 0)
		jm->outgoing = TRUE;

	/* XEP-0421: only rooms that advertise occupant-id overwrite it. */
	if (jm->occupant_id) {
		JabberID *jid = jabber_id_new(jm->from);
		JabberChat *chat = jid ? jabber_chat_find(js, jid->node, jid->domain) : NULL;

		if (!chat || !chat->occupant_id_supported) {
			g_free(jm->occupant_id);
			jm->occupant_id = NULL;
		}
		jabber_id_free(jid);
	}

	/* Session guard against showing a message twice (live and again from
	 * the archive, or room history after a rejoin).  The UI's index
	 * handles duplicates across restarts. */
	if (jm->server_id) {
		char *key = g_strdup_printf("s:%s:%s", jm->server_id_by, jm->server_id);
		if (jabber_mam_seen(account, key, TRUE)) {
			purple_debug_info("jabber", "Dropping duplicate message %s\n", key);
			keep = FALSE;
		}
		g_free(key);
	}
	if (keep && jm->outgoing && jm->origin_id) {
		/* Something this client itself sent earlier in this session. */
		char *key = g_strdup_printf("o:%s", jm->origin_id);
		if (jabber_mam_seen(account, key, FALSE)) {
			purple_debug_info("jabber", "Dropping own archived message %s\n", key);
			keep = FALSE;
		}
		g_free(key);
	}

	if (keep && jm->server_id && jm->origin != JABBER_MESSAGE_ORIGIN_MAM)
		jabber_mam_note_live_id(js, jm->server_id_by, jm->server_id);

	/* XEP-0428: record the ranges; strip the ones for features the UI
	 * renders natively. */
	jm->fallbacks = jabber_fallback_parse(packet);
	if (keep && jm->fallbacks && jm->body_raw &&
	    jabber_ui_supports_message_meta()) {
		const char *strip[3];
		int n = 0, i;

		for (i = 0; jabber_native_fallback_namespaces[i] && n < 2; i++) {
			const char *ns = jabber_native_fallback_namespaces[i];

			if (purple_strequal(ns, NS_REPLY) && jm->reply_to_id == NULL)
				continue;
			if (purple_strequal(ns, NS_REACTIONS) &&
			    !xmlnode_get_child_with_namespace(packet, "reactions", NS_REACTIONS))
				continue;
			strip[n++] = ns;
		}
		strip[n] = NULL;

		if (n > 0) {
			char *stripped = jabber_fallback_strip(jm->body_raw,
			                                       jm->fallbacks, strip);

			if (!purple_strequal(stripped, jm->body_raw)) {
				g_free(jm->body);
				jm->body = NULL;
				if (*stripped) {
					char *escaped = purple_markup_escape_text(stripped, -1);
					jm->body = purple_strdup_withhtml(escaped);
					g_free(escaped);
				}
				/* The XHTML-IM version still contains the fallback. */
				g_free(jm->xhtml);
				jm->xhtml = NULL;
				jm->fallback_stripped = TRUE;
			}
			g_free(stripped);
		}
	}

	g_free(own_bare);
	g_free(from_bare);
	return keep;
}

/**************************************************************************
 * M8 message semantics: receipts, markers, corrections, reactions,
 * retractions (receipts.c, correction.c, reactions.c, retraction.c)
 **************************************************************************/

static void
jabber_message_target_clear(JabberMessageTarget *t)
{
	g_free(t->conv_name);
	g_free(t->sender);
	g_free(t->identity);
	memset(t, 0, sizeof(*t));
}

/* Where the events in @jm belong; the conv_name/sender pair matches what
 * receiving-message-meta reports for the same message. */
static gboolean
jabber_message_target_init(JabberMessage *jm, JabberMessageTarget *t)
{
	JabberStream *js = jm->js;
	JabberID *jid;

	memset(t, 0, sizeof(*t));

	if (jm->type == JABBER_MESSAGE_GROUPCHAT) {
		JabberChat *chat;

		jid = jabber_id_new(jm->from);
		chat = jid ? jabber_chat_find(js, jid->node, jid->domain) : NULL;
		if (chat == NULL) {
			jabber_id_free(jid);
			return FALSE;
		}

		t->chat = chat;
		t->conv_name = chat->conv ?
			g_strdup(purple_conversation_get_name(chat->conv)) :
			g_strdup_printf("%s@%s", chat->room, chat->server);
		if (jid->resource) {
			t->sender = g_strdup(jid->resource);
			t->identity = jm->occupant_id ?
				g_strdup_printf("o:%s", jm->occupant_id) :
				g_strdup_printf("n:%s", jid->resource);
			t->own = purple_strequal(jid->resource, chat->handle);
		}
		jabber_id_free(jid);
		return TRUE;
	}

	if (jm->outgoing) {
		if (jm->to == NULL)
			return FALSE;
		t->conv_name = g_strdup(jm->to);
		t->sender = jabber_message_own_jid(js, FALSE);
		t->identity = jabber_message_own_jid(js, TRUE);
		t->own = TRUE;
		return TRUE;
	}

	if (jm->from == NULL)
		return FALSE;
	jid = jabber_id_new(jm->from);
	if (jid == NULL)
		return FALSE;

	t->conv_name = g_strdup(jm->from);
	t->sender = g_strdup(jm->from);
	/* Private messages in a room: each occupant is its own sender. */
	if (jabber_chat_find(js, jid->node, jid->domain))
		t->identity = jabber_id_get_full_jid(jid);
	else
		t->identity = jabber_id_get_bare_jid(jid);
	jabber_id_free(jid);
	return TRUE;
}

/*
 * Handles the message-semantics elements of a conversation message.
 * Returns TRUE if the message was consumed: a reaction or retraction (the
 * body is only their fallback), or a correction the UI rendered.
 */
static gboolean
jabber_message_semantics(JabberMessage *jm)
{
	JabberMessageTarget t;
	gboolean consumed = FALSE;

	switch (jm->type) {
		case JABBER_MESSAGE_NORMAL:
		case JABBER_MESSAGE_CHAT:
		case JABBER_MESSAGE_GROUPCHAT:
		case JABBER_MESSAGE_OTHER:
			break;
		default:
			return FALSE;
	}

	if (!jabber_message_target_init(jm, &t))
		return FALSE;

	if (t.chat && jm->occupant_id)
		jabber_chat_note_occupant(t.chat, jm->occupant_id, jm->id,
		                          jm->origin_id, jm->server_id);

	jabber_receipts_handle(jm, &t);

	if (jm->retract_id) {
		consumed = jabber_retraction_handle(jm, &t);
	} else if (jm->reactions) {
		consumed = jabber_reactions_handle(jm, &t);
	} else if (jm->replace_id && (jm->body || jm->xhtml) && t.sender) {
		GString *body = jm_body_with_oob(jm);
		consumed = jabber_correction_handle(jm, &t, body->str);
		g_string_free(body, TRUE);
	}

	jabber_message_target_clear(&t);
	return consumed;
}

void jabber_message_parse(JabberStream *js, xmlnode *packet)
{
	const char *id, *from, *to, *type;
	gboolean signal_return;

	from = xmlnode_get_attrib(packet, "from");
	id   = xmlnode_get_attrib(packet, "id");
	to   = xmlnode_get_attrib(packet, "to");
	type = xmlnode_get_attrib(packet, "type");

	signal_return = GPOINTER_TO_INT(purple_signal_emit_return_1(purple_connection_get_prpl(js->gc),
			"jabber-receiving-message", js->gc, type, id, from, to, packet));
	if (signal_return)
		return;

	/* XEP-0280: carbon copies are unwrapped and parsed as the inner
	 * message (or dropped if they are not acceptable). */
	if (jabber_carbons_handle(js, packet))
		return;

	/* XEP-0313: archive query results. */
	if (jabber_mam_handle_result(js, packet))
		return;

	jabber_message_parse_with_context(js, packet, NULL);
}

void jabber_message_parse_with_context(JabberStream *js, xmlnode *packet,
                                       const JabberMessageContext *ctx)
{
	JabberMessage *jm;
	const char *id, *from, *to, *type;
	xmlnode *child;

	from = xmlnode_get_attrib(packet, "from");
	id   = xmlnode_get_attrib(packet, "id");
	to   = xmlnode_get_attrib(packet, "to");
	type = xmlnode_get_attrib(packet, "type");

	jm = g_new0(JabberMessage, 1);
	jm->js = js;
	jm->sent = time(NULL);
	jm->delayed = FALSE;
	jm->chat_state = JM_STATE_NONE;
	jm->ctx = ctx;
	jm->origin = ctx ? ctx->origin : JABBER_MESSAGE_ORIGIN_LIVE;

	if(type) {
		if(purple_strequal(type, "normal"))
			jm->type = JABBER_MESSAGE_NORMAL;
		else if(purple_strequal(type, "chat"))
			jm->type = JABBER_MESSAGE_CHAT;
		else if(purple_strequal(type, "groupchat"))
			jm->type = JABBER_MESSAGE_GROUPCHAT;
		else if(purple_strequal(type, "headline"))
			jm->type = JABBER_MESSAGE_HEADLINE;
		else if(purple_strequal(type, "error"))
			jm->type = JABBER_MESSAGE_ERROR;
		else
			jm->type = JABBER_MESSAGE_OTHER;
	} else {
		jm->type = JABBER_MESSAGE_NORMAL;
	}

	jm->from = g_strdup(from);
	jm->to   = g_strdup(to);
	jm->id   = g_strdup(id);

	for(child = packet->child; child; child = child->next) {
		const char *xmlns = xmlnode_get_namespace(child);
		if(child->type != XMLNODE_TYPE_TAG)
			continue;

		if (xmlns && jabber_message_parse_modern_child(jm, child, xmlns))
			continue;

		if(purple_strequal(child->name, "error")) {
			const char *code = xmlnode_get_attrib(child, "code");
			char *code_txt = NULL;
			char *text = xmlnode_get_data(child);
			if (!text) {
				xmlnode *enclosed_text_node;

				if ((enclosed_text_node = xmlnode_get_child(child, "text")))
					text = xmlnode_get_data(enclosed_text_node);
			}

			if(code)
				code_txt = g_strdup_printf(_("(Code %s)"), code);

			if(!jm->error)
				jm->error = g_strdup_printf("%s%s%s",
						text ? text : "",
						text && code_txt ? " " : "",
						code_txt ? code_txt : "");

			g_free(code_txt);
			g_free(text);
		} else if (xmlns == NULL) {
			/* QuLogic: Not certain this is correct, but it would have happened
			   with the previous code. */
			if(purple_strequal(child->name, "x"))
				jm->etc = g_list_append(jm->etc, child);
			/* The following tests expect xmlns != NULL */
			continue;
		} else if(purple_strequal(child->name, "subject") && purple_strequal(xmlns, NS_XMPP_CLIENT)) {
			if(!jm->subject) {
				jm->subject = xmlnode_get_data(child);
				if(!jm->subject)
					jm->subject = g_strdup("");
			}
		} else if(purple_strequal(child->name, "thread") && purple_strequal(xmlns, NS_XMPP_CLIENT)) {
			if(!jm->thread_id)
				jm->thread_id = xmlnode_get_data(child);
		} else if(purple_strequal(child->name, "body") && purple_strequal(xmlns, NS_XMPP_CLIENT)) {
			if(!jm->body) {
				char *msg = xmlnode_get_data(child);
				char *escaped = purple_markup_escape_text(msg, -1);
				jm->body = purple_strdup_withhtml(escaped);
				g_free(escaped);
				jm->body_raw = msg;
			}
		} else if(purple_strequal(child->name, "html") && purple_strequal(xmlns, NS_XHTML_IM)) {
			if(!jm->xhtml && xmlnode_get_child(child, "body")) {
				char *c;

				const PurpleConnection *gc = js->gc;
				PurpleAccount *account = purple_connection_get_account(gc);
				PurpleConversation *conv = NULL;
				GList *smiley_refs = NULL;
				gchar *reformatted_xhtml;

				if (purple_account_get_bool(account, "custom_smileys", TRUE)) {
					/* find a list of smileys ("cid" and "alt" text pairs)
					  occuring in the message */
					smiley_refs = jabber_message_get_refs_from_xmlnode(child);
					purple_debug_info("jabber", "found %d smileys\n",
						g_list_length(smiley_refs));

					if (smiley_refs) {
						if (jm->type == JABBER_MESSAGE_GROUPCHAT) {
							JabberID *jid = jabber_id_new(jm->from);
							JabberChat *chat = NULL;

							if (jid) {
								chat = jabber_chat_find(js, jid->node, jid->domain);
								if (chat)
									conv = chat->conv;
								jabber_id_free(jid);
							}
						} else if (jm->type == JABBER_MESSAGE_NORMAL ||
						           jm->type == JABBER_MESSAGE_CHAT) {
							conv =
								purple_find_conversation_with_account(PURPLE_CONV_TYPE_ANY,
									from, account);
							if (!conv) {
								/* we need to create the conversation here */
								conv =
									purple_conversation_new(PURPLE_CONV_TYPE_IM,
									account, from);
							}
						}
					}

					/* process any newly provided smileys */
					jabber_message_add_remote_smileys(js, to, packet);
				}

				/* reformat xhtml so that img tags with a "cid:" src gets
				  translated to the bare text of the emoticon (the "alt" attrib) */
				/* this is done also when custom smiley retrieval is turned off,
				  this way the receiver always sees the shortcut instead */
				reformatted_xhtml =
					jabber_message_xml_to_string_strip_img_smileys(child);

				jm->xhtml = reformatted_xhtml;

				/* add known custom emoticons to the conversation */
				/* note: if there were no smileys in the incoming message, or
				  	if receiving custom smileys is turned off, smiley_refs will
					be NULL */
				for (; conv && smiley_refs ; smiley_refs = g_list_delete_link(smiley_refs, smiley_refs)) {
					JabberSmileyRef *ref = (JabberSmileyRef *) smiley_refs->data;
					const gchar *cid = ref->cid;
					gchar *alt = g_strdup(ref->alt);

					purple_debug_info("jabber",
						"about to add custom smiley %s to the conv\n", alt);
					if (purple_conv_custom_smiley_add(conv, alt, "cid", cid,
						    TRUE)) {
						const JabberData *data =
								jabber_data_find_remote_by_cid(js, from, cid);
						/* if data is already known, we write it immediatly */
						if (data) {
							purple_debug_info("jabber",
								"data is already known\n");
							purple_conv_custom_smiley_write(conv, alt,
								jabber_data_get_data(data),
								jabber_data_get_size(data));
							purple_conv_custom_smiley_close(conv, alt);
						} else {
							/* we need to request the smiley (data) */
							purple_debug_info("jabber",
								"data is unknown, need to request it\n");
							jabber_data_request(js, cid, from, alt, FALSE,
							    jabber_message_request_data_cb, conv);
						}
					}
					g_free(ref->cid);
					g_free(ref->alt);
					g_free(ref);
				}

			    /* Convert all newlines to whitespace. Technically, even regular, non-XML HTML is supposed to ignore newlines, but Pidgin has, as convention
				 * treated \n as a newline for compatibility with other protocols
				 */
				for (c = jm->xhtml; *c != '\0'; c++) {
					if (*c == '\n')
						*c = ' ';
				}
			}
		} else if(purple_strequal(child->name, "active") && purple_strequal(xmlns,"http://jabber.org/protocol/chatstates")) {
			jm->chat_state = JM_STATE_ACTIVE;
		} else if(purple_strequal(child->name, "composing") && purple_strequal(xmlns,"http://jabber.org/protocol/chatstates")) {
			jm->chat_state = JM_STATE_COMPOSING;
		} else if(purple_strequal(child->name, "paused") && purple_strequal(xmlns,"http://jabber.org/protocol/chatstates")) {
			jm->chat_state = JM_STATE_PAUSED;
		} else if(purple_strequal(child->name, "inactive") && purple_strequal(xmlns,"http://jabber.org/protocol/chatstates")) {
			jm->chat_state = JM_STATE_INACTIVE;
		} else if(purple_strequal(child->name, "gone") && purple_strequal(xmlns,"http://jabber.org/protocol/chatstates")) {
			jm->chat_state = JM_STATE_GONE;
		} else if(purple_strequal(child->name, "event") && purple_strequal(xmlns,"http://jabber.org/protocol/pubsub#event")) {
			xmlnode *items;
			jm->type = JABBER_MESSAGE_EVENT;
			for(items = xmlnode_get_child(child,"items"); items; items = items->next)
				jm->eventitems = g_list_append(jm->eventitems, items);
		} else if(purple_strequal(child->name, "attention") && purple_strequal(xmlns, NS_ATTENTION)) {
			jm->hasBuzz = TRUE;
		} else if(purple_strequal(child->name, "delay") && purple_strequal(xmlns, NS_DELAYED_DELIVERY)) {
			const char *timestamp = xmlnode_get_attrib(child, "stamp");
			jm->delayed = TRUE;
			if(timestamp)
				jm->sent = purple_str_to_time(timestamp, TRUE, NULL, NULL, NULL);
		} else if(purple_strequal(child->name, "x")) {
			if(purple_strequal(xmlns, NS_DELAYED_DELIVERY_LEGACY)) {
				const char *timestamp = xmlnode_get_attrib(child, "stamp");
				jm->delayed = TRUE;
				if(timestamp)
					jm->sent = purple_str_to_time(timestamp, TRUE, NULL, NULL, NULL);
			} else if(purple_strequal(xmlns, "jabber:x:conference") &&
					jm->type != JABBER_MESSAGE_GROUPCHAT_INVITE &&
					jm->type != JABBER_MESSAGE_ERROR) {
				const char *jid = xmlnode_get_attrib(child, "jid");
				if(jid) {
					const char *reason = xmlnode_get_attrib(child, "reason");
					const char *password = xmlnode_get_attrib(child, "password");

					jm->type = JABBER_MESSAGE_GROUPCHAT_INVITE;
					g_free(jm->to);
					jm->to = g_strdup(jid);

					if (reason) {
						g_free(jm->body);
						jm->body = g_strdup(reason);
					}

					if (password) {
						g_free(jm->password);
						jm->password = g_strdup(password);
					}
				}
			} else if(purple_strequal(xmlns, "http://jabber.org/protocol/muc#user") &&
					jm->type != JABBER_MESSAGE_ERROR) {
				xmlnode *invite = xmlnode_get_child(child, "invite");
				if(invite) {
					xmlnode *reason, *password;
					const char *jid = xmlnode_get_attrib(invite, "from");
					g_free(jm->to);
					jm->to = jm->from;
					jm->from = g_strdup(jid);
					if((reason = xmlnode_get_child(invite, "reason"))) {
						g_free(jm->body);
						jm->body = xmlnode_get_data(reason);
					}
					if((password = xmlnode_get_child(child, "password"))) {
						g_free(jm->password);
						jm->password = xmlnode_get_data(password);
					}

					jm->type = JABBER_MESSAGE_GROUPCHAT_INVITE;
				}
			} else {
				jm->etc = g_list_append(jm->etc, child);
			}
		} else if (purple_strequal(child->name, "query")) {
			const char *node = xmlnode_get_attrib(child, "node");
			if (purple_strequal(xmlns, NS_DISCO_ITEMS)
					&& purple_strequal(node, "http://jabber.org/protocol/commands")) {
				jabber_adhoc_got_list(js, jm->from, child);
			}
		}
	}

	if (!jabber_message_post_process(jm, packet)) {
		jabber_message_free(jm);
		return;
	}

	if (jm->origin == JABBER_MESSAGE_ORIGIN_MAM ||
	    jm->origin == JABBER_MESSAGE_ORIGIN_CARBON_SENT) {
		/* Only conversation messages are replayed from archives or copied
		 * from our other devices; no invites, notifications or buzzes. */
		switch (jm->type) {
			case JABBER_MESSAGE_NORMAL:
			case JABBER_MESSAGE_CHAT:
			case JABBER_MESSAGE_GROUPCHAT:
			case JABBER_MESSAGE_OTHER:
				break;
			default:
				jabber_message_free(jm);
				return;
		}
		jm->hasBuzz = FALSE;
	}

	if (jabber_message_semantics(jm)) {
		jabber_message_free(jm);
		return;
	}

	if(jm->hasBuzz)
		handle_buzz(jm);

	switch(jm->type) {
		case JABBER_MESSAGE_OTHER:
			purple_debug_info("jabber",
					"Received message of unknown type: %s\n", type);
			/* FALL-THROUGH */
		case JABBER_MESSAGE_NORMAL:
		case JABBER_MESSAGE_CHAT:
			handle_chat(jm);
			break;
		case JABBER_MESSAGE_HEADLINE:
			handle_headline(jm);
			break;
		case JABBER_MESSAGE_GROUPCHAT:
			handle_groupchat(jm);
			break;
		case JABBER_MESSAGE_GROUPCHAT_INVITE:
			handle_groupchat_invite(jm);
			break;
		case JABBER_MESSAGE_EVENT:
			jabber_handle_event(jm);
			break;
		case JABBER_MESSAGE_ERROR:
			handle_error(jm);
			break;
	}
	jabber_message_free(jm);
}

void jabber_message_send(JabberMessage *jm)
{
	xmlnode *message, *child;
	const char *type = NULL;

	message = xmlnode_new("message");

	switch(jm->type) {
		case JABBER_MESSAGE_NORMAL:
			type = "normal";
			break;
		case JABBER_MESSAGE_CHAT:
		case JABBER_MESSAGE_GROUPCHAT_INVITE:
			type = "chat";
			break;
		case JABBER_MESSAGE_HEADLINE:
			type = "headline";
			break;
		case JABBER_MESSAGE_GROUPCHAT:
			type = "groupchat";
			break;
		case JABBER_MESSAGE_ERROR:
			type = "error";
			break;
		case JABBER_MESSAGE_OTHER:
		default:
			type = NULL;
			break;
	}

	if(type)
		xmlnode_set_attrib(message, "type", type);

	if (jm->id)
		xmlnode_set_attrib(message, "id", jm->id);

	xmlnode_set_attrib(message, "to", jm->to);

	if(jm->thread_id) {
		child = xmlnode_new_child(message, "thread");
		xmlnode_insert_data(child, jm->thread_id, -1);
	}

	child = NULL;
	switch(jm->chat_state)
	{
		case JM_STATE_ACTIVE:
			child = xmlnode_new_child(message, "active");
			break;
		case JM_STATE_COMPOSING:
			child = xmlnode_new_child(message, "composing");
			break;
		case JM_STATE_PAUSED:
			child = xmlnode_new_child(message, "paused");
			break;
		case JM_STATE_INACTIVE:
			child = xmlnode_new_child(message, "inactive");
			break;
		case JM_STATE_GONE:
			child = xmlnode_new_child(message, "gone");
			break;
		case JM_STATE_NONE:
			/* yep, nothing */
			break;
	}
	if(child)
		xmlnode_set_namespace(child, "http://jabber.org/protocol/chatstates");

	if(jm->subject) {
		child = xmlnode_new_child(message, "subject");
		xmlnode_insert_data(child, jm->subject, -1);
	}

	if(jm->body) {
		child = xmlnode_new_child(message, "body");
		xmlnode_insert_data(child, jm->body, -1);
	}

	if(jm->xhtml) {
		if ((child = xmlnode_from_str(jm->xhtml, -1))) {
			xmlnode_insert_child(message, child);
		} else {
			purple_debug_error("jabber",
					"XHTML translation/validation failed, returning: %s\n",
					jm->xhtml);
		}
	}

	/* XEP-0359 */
	if (jm->origin_id) {
		child = xmlnode_new_child(message, "origin-id");
		xmlnode_set_namespace(child, NS_SID);
		xmlnode_set_attrib(child, "id", jm->origin_id);
	}

	/* M8 message semantics */
	if (jm->replace_id) {
		child = xmlnode_new_child(message, "replace");
		xmlnode_set_namespace(child, NS_MESSAGE_CORRECT);
		xmlnode_set_attrib(child, "id", jm->replace_id);
	}
	if (jm->reply_to_id) {
		child = xmlnode_new_child(message, "reply");
		xmlnode_set_namespace(child, NS_REPLY);
		if (jm->reply_to_jid && *jm->reply_to_jid)
			xmlnode_set_attrib(child, "to", jm->reply_to_jid);
		xmlnode_set_attrib(child, "id", jm->reply_to_id);
	}
	{
		GList *l;

		for (l = jm->fallbacks; l; l = l->next) {
			JabberFallback *fb = l->data;

			child = xmlnode_new_child(message, "fallback");
			xmlnode_set_namespace(child, NS_FALLBACK);
			if (fb->ns)
				xmlnode_set_attrib(child, "for", fb->ns);
			if (fb->start >= 0) {
				xmlnode *range = xmlnode_new_child(child, "body");
				char *num = g_strdup_printf("%d", fb->start);
				xmlnode_set_attrib(range, "start", num);
				g_free(num);
				num = g_strdup_printf("%d", fb->end);
				xmlnode_set_attrib(range, "end", num);
				g_free(num);
			}
		}
	}
	if (jm->receipt_request) {
		child = xmlnode_new_child(message, "request");
		xmlnode_set_namespace(child, NS_RECEIPTS);
	}
	if (jm->markable) {
		child = xmlnode_new_child(message, "markable");
		xmlnode_set_namespace(child, NS_CHAT_MARKERS);
	}
	if (jm->store_hint) {
		child = xmlnode_new_child(message, "store");
		xmlnode_set_namespace(child, NS_HINTS);
	}

	jabber_send(jm->js, message);

	xmlnode_free(message);
}

/*
 * Gives an outgoing message with a body a XEP-0359 origin-id, also used
 * as its id (as other modern clients do), and remembers it so the same
 * message coming back from the archive in this session is not shown
 * twice.
 */
static void
jabber_message_assign_ids(JabberMessage *jm)
{
	PurpleAccount *account = purple_connection_get_account(jm->js->gc);
	char *key;

	g_free(jm->id);
	jm->id = g_uuid_string_random();
	g_free(jm->origin_id);
	jm->origin_id = g_strdup(jm->id);

	key = g_strdup_printf("o:%s", jm->origin_id);
	jabber_mam_seen(account, key, TRUE);
	g_free(key);
}

/* sending-message-meta, if the UI wants it: the usual keys plus @extra
 * (key/value pairs, NULL-terminated; NULL values skipped). */
static void
jabber_message_emit_sending_meta_va(JabberMessage *jm, const char *conv_name,
                                    const char *conv_type, const char *first_key,
                                    va_list args)
{
	GHashTable *meta;
	const char *key;

	if (!jabber_ui_supports_message_meta())
		return;

	meta = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	meta_set(meta, "conv-type", conv_type);
	meta_set(meta, "stanza-id", jm->id);
	meta_set(meta, "origin-id", jm->origin_id);

	for (key = first_key; key; key = va_arg(args, const char *)) {
		const char *value = va_arg(args, const char *);
		if (value && *value)
			meta_set(meta, key, value);
	}

	purple_signal_emit(purple_conversations_get_handle(),
	                   "sending-message-meta",
	                   purple_connection_get_account(jm->js->gc),
	                   conv_name, meta);
	g_hash_table_destroy(meta);
}

static void
jabber_message_emit_sending_meta(JabberMessage *jm, const char *conv_name,
                                 const char *conv_type, const char *first_key, ...)
{
	va_list args;

	va_start(args, first_key);
	jabber_message_emit_sending_meta_va(jm, conv_name, conv_type, first_key, args);
	va_end(args);
}

/* XEP-0184 §5.1: ask for a receipt unless the resource is known not to
 * support them. */
static gboolean
jabber_message_wants_receipt(JabberStream *js, const char *who)
{
	JabberBuddy *jb;
	JabberBuddyResource *jbr;
	char *resource;

	if (!jabber_ui_supports_message_meta())
		return FALSE;

	resource = jabber_get_resource(who);
	jb = jabber_buddy_find(js, who, FALSE);
	jbr = jb ? jabber_buddy_find_resource(jb, resource) : NULL;
	g_free(resource);

	return jbr == NULL || jbr->caps.info == NULL ||
		jabber_resource_has_capability(jbr, NS_RECEIPTS);
}

int jabber_message_send_im(PurpleConnection *gc, const char *who, const char *msg,
		PurpleMessageFlags flags)
{
	JabberMessage *jm;
	JabberBuddy *jb;
	JabberBuddyResource *jbr;
	char *tmp;
	char *resource;

	if(!who || !msg)
		return 0;

	if (purple_debug_is_verbose()) {
		/* TODO: Maybe we need purple_debug_is_really_verbose? :) */
		purple_debug_misc("jabber", "jabber_message_send_im: who='%s'\n"
		                            "\tmsg='%s'\n", who, msg);
	}

	resource = jabber_get_resource(who);

	jb = jabber_buddy_find(gc->proto_data, who, TRUE);
	jbr = jabber_buddy_find_resource(jb, resource);

	g_free(resource);

	jm = g_new0(JabberMessage, 1);
	jm->js = gc->proto_data;
	jm->type = JABBER_MESSAGE_CHAT;
	jm->chat_state = JM_STATE_ACTIVE;
	jm->to = g_strdup(who);
	jm->id = jabber_get_next_id(jm->js);

	if(jbr) {
		if(jbr->thread_id) {
			jm->thread_id = g_strdup(jbr->thread_id);
		}

		if (jbr->chat_states == JABBER_CHAT_STATES_UNSUPPORTED) {
			jm->chat_state = JM_STATE_NONE;
		} else {
			/* if(JABBER_CHAT_STATES_UNKNOWN == jbr->chat_states)
			   jbr->chat_states = JABBER_CHAT_STATES_UNSUPPORTED; */
		}
	}

	/*
	 * M8: no XHTML-IM (XEP-0071) any more.  The UI's markup becomes a
	 * plain body with XEP-0393 styling markers.
	 */
	tmp = purple_utf8_strip_unprintables(msg);
	jm->body = jabber_styling_html_to_text(tmp);
	g_free(tmp);

	jm->receipt_request = jabber_message_wants_receipt(jm->js, who);
	jm->markable = jabber_ui_supports_message_meta();

	jabber_message_assign_ids(jm);
	jabber_message_send(jm);
	jabber_message_emit_sending_meta(jm, who, "im", NULL);
	jabber_message_free(jm);
	return 1;
}

int jabber_message_send_chat(PurpleConnection *gc, int id, const char *msg, PurpleMessageFlags flags)
{
	JabberChat *chat;
	JabberMessage *jm;
	JabberStream *js;
	char *tmp;

	if(!msg || !gc)
		return 0;

	js = gc->proto_data;
	chat = jabber_chat_find_by_id(js, id);

	if(!chat)
		return 0;

	jm = g_new0(JabberMessage, 1);
	jm->js = gc->proto_data;
	jm->type = JABBER_MESSAGE_GROUPCHAT;
	jm->to = g_strdup_printf("%s@%s", chat->room, chat->server);
	jm->id = jabber_get_next_id(jm->js);

	/* M8: XEP-0393 plain body instead of XHTML-IM, as for IMs. */
	tmp = purple_utf8_strip_unprintables(msg);
	jm->body = jabber_styling_html_to_text(tmp);
	g_free(tmp);

	jm->markable = jabber_ui_supports_message_meta();

	jabber_message_assign_ids(jm);
	jabber_message_send(jm);
	jabber_message_emit_sending_meta(jm,
			chat->conv ? purple_conversation_get_name(chat->conv) : jm->to,
			"chat", NULL);
	jabber_message_free(jm);

	return 1;
}

unsigned int jabber_send_typing(PurpleConnection *gc, const char *who, PurpleTypingState state)
{
	JabberStream *js;
	JabberMessage *jm;
	JabberBuddy *jb;
	JabberBuddyResource *jbr;
	char *resource;

	js = purple_connection_get_protocol_data(gc);
	jb = jabber_buddy_find(js, who, TRUE);
	if (!jb)
		return 0;

	resource = jabber_get_resource(who);
	jbr = jabber_buddy_find_resource(jb, resource);
	g_free(resource);

	/* We know this entity doesn't support chat states */
	if (jbr && jbr->chat_states == JABBER_CHAT_STATES_UNSUPPORTED)
		return 0;

	/* *If* we don't have presence /and/ the buddy can't see our
	 * presence, don't send typing notifications.
	 */
	if (!jbr && !(jb->subscription & JABBER_SUB_FROM))
		return 0;

	/* TODO: figure out threading */
	jm = g_new0(JabberMessage, 1);
	jm->js = js;
	jm->type = JABBER_MESSAGE_CHAT;
	jm->to = g_strdup(who);
	jm->id = jabber_get_next_id(jm->js);

	if(PURPLE_TYPING == state)
		jm->chat_state = JM_STATE_COMPOSING;
	else if(PURPLE_TYPED == state)
		jm->chat_state = JM_STATE_PAUSED;
	else
		jm->chat_state = JM_STATE_ACTIVE;

	/* if(JABBER_CHAT_STATES_UNKNOWN == jbr->chat_states)
		jbr->chat_states = JABBER_CHAT_STATES_UNSUPPORTED; */

	jabber_message_send(jm);
	jabber_message_free(jm);

	return 0;
}

gboolean jabber_buzz_isenabled(JabberStream *js, const gchar *namespace) {
	return js->allowBuzz;
}

gboolean jabber_custom_smileys_isenabled(JabberStream *js, const gchar *namespace)
{
	const PurpleConnection *gc = js->gc;
	PurpleAccount *account = purple_connection_get_account(gc);

	return purple_account_get_bool(account, "custom_smileys", TRUE);
}

/**************************************************************************
 * M8 message semantics: shared helpers
 **************************************************************************/

char *
jabber_message_own_jid(JabberStream *js, gboolean bare)
{
	return bare ? jabber_id_get_bare_jid(js->user)
	            : jabber_id_get_full_jid(js->user);
}

PurpleConversation *
jabber_message_im_conv(PurpleAccount *account, const char *name)
{
	PurpleConversation *conv;

	conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM,
	                                             name, account);
	if (conv == NULL) {
		char *bare = jabber_get_bare_jid(name);
		conv = purple_conversation_new(PURPLE_CONV_TYPE_IM, account,
		                               bare ? bare : name);
		g_free(bare);
	}

	return conv;
}

char *
jabber_message_display_name(JabberStream *js, const JabberMessageTarget *t)
{
	PurpleAccount *account = purple_connection_get_account(js->gc);
	const char *name = NULL;
	char *bare = NULL, *escaped;

	if (t->chat) {
		name = t->sender ? t->sender : t->conv_name;
	} else if (t->own) {
		name = purple_account_get_alias(account);
		if (name == NULL || *name == '\0')
			name = bare = jabber_message_own_jid(js, TRUE);
	} else {
		PurpleBuddy *buddy;

		bare = jabber_get_bare_jid(t->sender ? t->sender : t->conv_name);
		buddy = bare ? purple_find_buddy(account, bare) : NULL;
		name = buddy ? purple_buddy_get_alias(buddy) : bare;
		if (name == NULL)
			name = t->sender ? t->sender : t->conv_name;
	}

	escaped = g_markup_escape_text(name ? name : "", -1);
	g_free(bare);
	return escaped;
}

void
jabber_message_write_event(JabberStream *js, const JabberMessageTarget *t,
                           const char *markup, time_t when, gboolean delayed)
{
	PurpleAccount *account = purple_connection_get_account(js->gc);
	PurpleMessageFlags flags = PURPLE_MESSAGE_SYSTEM;

	if (delayed)
		flags |= PURPLE_MESSAGE_DELAYED;

	if (t->chat) {
		if (t->chat->conv)
			purple_conv_chat_write(PURPLE_CONV_CHAT(t->chat->conv), "",
			                       markup, flags, when);
		return;
	}

	purple_conversation_write(jabber_message_im_conv(account, t->conv_name),
	                          NULL, markup, flags, when);
}

char *
jabber_message_plain_to_markup(const char *plain)
{
	char *escaped, *markup;

	if (plain == NULL)
		return NULL;

	escaped = purple_markup_escape_text(plain, -1);
	markup = purple_strdup_withhtml(escaped);
	g_free(escaped);
	return markup;
}

gboolean
jabber_ipc_target_init(JabberIpcTarget *t, PurpleAccount *account,
                       const char *conv_name)
{
	PurpleConnection *gc;
	JabberStream *js;
	JabberID *jid;
	JabberChat *chat;

	memset(t, 0, sizeof(*t));

	if (account == NULL || conv_name == NULL || *conv_name == '\0')
		return FALSE;
	if (!purple_strequal(purple_account_get_protocol_id(account), "prpl-jabber"))
		return FALSE;

	gc = purple_account_get_connection(account);
	if (gc == NULL || purple_connection_get_state(gc) != PURPLE_CONNECTED)
		return FALSE;
	js = purple_connection_get_protocol_data(gc);
	if (js == NULL)
		return FALSE;

	jid = jabber_id_new(conv_name);
	if (jid == NULL)
		return FALSE;

	t->js = js;
	chat = jabber_chat_find(js, jid->node, jid->domain);
	if (chat && jid->resource == NULL) {
		t->chat = chat;
		t->groupchat = TRUE;
		t->to = g_strdup_printf("%s@%s", chat->room, chat->server);
	} else {
		t->to = g_strdup(conv_name);
	}
	jabber_id_free(jid);

	return TRUE;
}

void
jabber_ipc_target_clear(JabberIpcTarget *t)
{
	g_free(t->to);
	memset(t, 0, sizeof(*t));
}

xmlnode *
jabber_message_stanza_new(JabberStream *js, const char *to, gboolean groupchat)
{
	PurpleAccount *account = purple_connection_get_account(js->gc);
	xmlnode *message, *origin;
	char *id, *key;

	id = g_uuid_string_random();
	message = xmlnode_new("message");
	xmlnode_set_attrib(message, "type", groupchat ? "groupchat" : "chat");
	xmlnode_set_attrib(message, "to", to);
	xmlnode_set_attrib(message, "id", id);

	origin = xmlnode_new_child(message, "origin-id");
	xmlnode_set_namespace(origin, NS_SID);
	xmlnode_set_attrib(origin, "id", id);

	key = g_strdup_printf("o:%s", id);
	jabber_mam_seen(account, key, TRUE);
	g_free(key);
	g_free(id);

	return message;
}

JabberMessage *
jabber_message_new_outgoing(const JabberIpcTarget *t, const char *body)
{
	JabberMessage *jm = g_new0(JabberMessage, 1);

	jm->js = t->js;
	jm->type = t->groupchat ? JABBER_MESSAGE_GROUPCHAT : JABBER_MESSAGE_CHAT;
	jm->chat_state = t->groupchat ? JM_STATE_NONE : JM_STATE_ACTIVE;
	jm->to = g_strdup(t->to);
	jm->body = purple_utf8_strip_unprintables(body);

	return jm;
}

void
jabber_message_send_ipc(JabberMessage *jm, const char *conv_name,
                        const char *first_key, ...)
{
	va_list args;
	gboolean groupchat = (jm->type == JABBER_MESSAGE_GROUPCHAT);

	jabber_message_assign_ids(jm);
	if (!groupchat)
		jm->receipt_request = jabber_message_wants_receipt(jm->js, jm->to);
	jm->markable = jabber_ui_supports_message_meta();

	jabber_message_send(jm);

	va_start(args, first_key);
	jabber_message_emit_sending_meta_va(jm, conv_name,
			groupchat ? "chat" : "im", first_key, args);
	va_end(args);
}

/**************************************************************************
 * IPC: send-reply (XEP-0461)
 **************************************************************************/

static gboolean
jabber_message_ipc_send_reply(PurpleAccount *account, const char *conv_name,
                              const char *reply_to_id, const char *reply_to_jid,
                              const char *quoted, const char *body)
{
	JabberIpcTarget t;
	JabberMessage *jm;
	GString *full;
	char *clean;
	glong quote_len;
	gboolean ok = FALSE;

	if (reply_to_id == NULL || *reply_to_id == '\0' ||
	    body == NULL || *body == '\0')
		return FALSE;

	if (!jabber_ipc_target_init(&t, account, conv_name))
		goto out;

	/* XEP-0461 §3.2 fallback: the quoted text, each line prefixed with
	 * "> ", then the reply; the quote is marked with XEP-0428. */
	full = g_string_new(NULL);
	if (quoted && *quoted) {
		char **lines, **l;

		clean = purple_utf8_strip_unprintables(quoted);
		lines = g_strsplit(clean, "\n", -1);
		for (l = lines; *l; l++) {
			/* no empty trailing line from a final newline */
			if (**l == '\0' && l[1] == NULL && l != lines)
				break;
			g_string_append(full, "> ");
			g_string_append(full, *l);
			g_string_append_c(full, '\n');
		}
		g_strfreev(lines);
		g_free(clean);
	}
	quote_len = g_utf8_strlen(full->str, -1);
	clean = purple_utf8_strip_unprintables(body);
	g_string_append(full, clean);

	jm = jabber_message_new_outgoing(&t, full->str);
	g_string_free(full, TRUE);
	jm->reply_to_id = g_strdup(reply_to_id);
	jm->reply_to_jid = (reply_to_jid && *reply_to_jid) ? g_strdup(reply_to_jid) : NULL;
	if (quote_len > 0) {
		JabberFallback *fb = g_new0(JabberFallback, 1);

		fb->ns = g_strdup(NS_REPLY);
		fb->start = 0;
		fb->end = (int)quote_len;
		jm->fallbacks = g_list_append(NULL, fb);
	}

	jabber_message_send_ipc(jm, conv_name, "reply-to", reply_to_id,
	                        "reply-to-sender", reply_to_jid, NULL);

	/* 1:1: written like any sent message, without the quote (the UI has
	 * reply-to from sending-message-meta).  Rooms reflect it. */
	if (!t.groupchat) {
		PurpleConversation *conv = jabber_message_im_conv(account, conv_name);
		char *markup = jabber_message_plain_to_markup(clean);

		purple_conv_im_write(PURPLE_CONV_IM(conv), NULL, markup,
		                     PURPLE_MESSAGE_SEND, time(NULL));
		g_free(markup);
	}

	g_free(clean);
	jabber_message_free(jm);
	ok = TRUE;

out:
	jabber_ipc_target_clear(&t);
	return ok;
}

void
jabber_message_semantics_init(PurplePlugin *plugin)
{
	jabber_receipts_init(plugin);
	jabber_correction_init(plugin);
	jabber_reactions_init(plugin);
	jabber_retraction_init(plugin);
	jabber_displayed_init(plugin);

	/* gboolean (account, conv name, reply-to id, reply-to JID or NULL,
	 *           quoted text or NULL, body) */
	purple_plugin_ipc_register(plugin, "send-reply",
			PURPLE_CALLBACK(jabber_message_ipc_send_reply),
			purple_marshal_BOOLEAN__POINTER_POINTER_POINTER_POINTER_POINTER_POINTER,
			purple_value_new(PURPLE_TYPE_BOOLEAN), 6,
			purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING));

	/* disco#info features.  Every one of them has a readable fallback
	 * when the UI doesn't render it natively, so they are advertised
	 * regardless of the UI. */
	jabber_add_feature(NS_RECEIPTS, NULL);
	jabber_add_feature(NS_CHAT_MARKERS, NULL);
	jabber_add_feature(NS_MESSAGE_CORRECT, NULL);
	jabber_add_feature(NS_REACTIONS, NULL);
	jabber_add_feature(NS_RETRACT, NULL);
	jabber_add_feature(NS_RETRACT_LEGACY, NULL);
	jabber_add_feature(NS_REPLY, NULL);
	jabber_add_feature(NS_STYLING, NULL);
}

void
jabber_message_semantics_uninit(void)
{
	jabber_displayed_uninit();
	jabber_reactions_reset();
}
