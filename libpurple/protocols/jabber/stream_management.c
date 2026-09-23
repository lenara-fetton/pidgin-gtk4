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

/*
 * XEP-0198 Stream Management: acks, and session resumption.
 *
 * libpurple destroys the JabberStream when a connection drops and its
 * auto-reconnect builds a new one, so everything needed to resume lives in
 * a JabberSmSession kept in a process-lifetime hash keyed by the account's
 * bare JID (never persisted):
 *
 *   - the SM-ID and the bound full JID of the session to resume;
 *   - the h counters (what we processed, what the server acked);
 *   - a bounded queue of copies of the stanzas the server hasn't acked.
 *
 * A clean sign-off closes the stream, which ends the session on the server,
 * so the SM-ID is forgotten.  A non-fatal connection error (network drop,
 * ping timeout) leaves the stream unclosed so the server hibernates the
 * session, and the next connection sends <resume/> instead of binding (or
 * inline in the SASL2 <authenticate/>, see sasl2.c).
 *
 * On <resumed/> the stanzas the server didn't get are re-sent.  libpurple
 * already marked every buddy offline and left every chat when the old
 * connection went away, so the resumed session re-runs the post-bind
 * sequence (disco, roster) after an unavailable presence; the available
 * presence that follows is then "initial" again and the server re-probes
 * the contacts.  The gain over a fresh session is that no stanza in flight
 * is lost in either direction.
 *
 * On <failed/> (or when there is nothing to resume) a new session is bound
 * and the queued <message/>s from the old one are re-sent as new stanzas;
 * queued IQs and presences are dropped.
 */

#include "internal.h"

#include <glib.h>
#include "namespaces.h"
#include "xmlnode.h"
#include "jabber.h"
#include "debug.h"
#include "disco.h"
#include "stream_management.h"

/* Bound on the unacked-stanza queue; the oldest are dropped past this. */
#define MAX_QUEUE_LENGTH 1000

typedef struct {
	GQueue *queue;          /* copies of unacked outbound stanzas */
	guint32 head_seq;       /* sequence number of the queue head */
	GList *pending;         /* stanzas of an old session to re-send */

	gchar *id;              /* SM-ID, NULL if not resumable */
	gchar *full_jid;        /* bound JID of that session */
	guint max;              /* server's max resumption time, 0 unknown */
	gint64 dropped_at;      /* monotonic µs when the stream dropped */

	guint32 inbound_count;  /* saved js->sm_inbound_count */
	guint32 outbound_count; /* saved js->sm_outbound_count */
	guint32 outbound_confirmed;
} JabberSmSession;

static GHashTable *jabber_sm_accounts;

static void
sm_session_free(gpointer p)
{
	JabberSmSession *sess = p;

	g_queue_free_full(sess->queue, (GDestroyNotify)xmlnode_free);
	g_list_free_full(sess->pending, (GDestroyNotify)xmlnode_free);
	g_free(sess->id);
	g_free(sess->full_jid);
	g_free(sess);
}

/* Returns the session state for a JabberStream's account (based on the
   bare JID), creating it if there's none. */
static JabberSmSession *
sm_session_get(JabberStream *js)
{
	JabberSmSession *sess;
	gchar *jid = jabber_id_get_bare_jid(js->user);

	sess = g_hash_table_lookup(jabber_sm_accounts, jid);
	if (sess == NULL) {
		sess = g_new0(JabberSmSession, 1);
		sess->queue = g_queue_new();
		g_hash_table_insert(jabber_sm_accounts, jid, sess);
	} else {
		g_free(jid);
	}

	return sess;
}

static void
sm_forget_resumption(JabberSmSession *sess)
{
	g_free(sess->id);
	sess->id = NULL;
	g_free(sess->full_jid);
	sess->full_jid = NULL;
	sess->dropped_at = 0;
	sess->max = 0;
}

/* Moves the unacked queue of a finished session to the re-send list. */
static void
sm_steal_queue(JabberSmSession *sess)
{
	xmlnode *stanza;

	while ((stanza = g_queue_pop_head(sess->queue)) != NULL)
		sess->pending = g_list_append(sess->pending, stanza);
	sess->head_seq = 0;
}

/* Sends a stanza we sent before without running the jabber-sending-xmlnode
 * signal again (the queued copy is what went on the wire, after plugins
 * such as OMEMO had their go at it), but counting it for SM. */
static void
sm_resend(JabberStream *js, xmlnode *stanza)
{
	char *txt;
	int len;

	txt = xmlnode_to_str(stanza, &len);
	jabber_send_raw(js, txt, len);
	g_free(txt);

	jabber_sm_outbound(js, stanza);
}

void
jabber_sm_init(void)
{
	jabber_sm_accounts = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                           g_free, sm_session_free);
}

void
jabber_sm_uninit(void)
{
	g_hash_table_destroy(jabber_sm_accounts);
	jabber_sm_accounts = NULL;
}

/* Handles an ack count from <a/> or <resumed/>: drops what the server
 * has, returns FALSE if h makes no sense. */
static gboolean
sm_process_ack(JabberStream *js, const char *ack_h)
{
	JabberSmSession *sess = sm_session_get(js);
	guint32 h;
	xmlnode *stanza;

	if (ack_h == NULL) {
		purple_debug_error("XEP-0198",
		                   "The 'h' attribute is not defined for an answer.\n");
		return FALSE;
	}
	h = strtoul(ack_h, NULL, 10);

	if ((gint32)(h - js->sm_outbound_count) > 0) {
		purple_debug_error("XEP-0198",
		                   "Server acked %u stanzas but only %u were sent\n",
		                   h, js->sm_outbound_count);
		h = js->sm_outbound_count;
	}

	/* Remove stanzas from the queue */
	while ((gint32)(h - sess->head_seq) > 0) {
		stanza = g_queue_pop_head(sess->queue);
		if (stanza == NULL)
			break;
		xmlnode_free(stanza);
		sess->head_seq++;
	}
	if (g_queue_is_empty(sess->queue))
		sess->head_seq = h;

	js->sm_outbound_confirmed = h;
	purple_debug_info("XEP-0198",
	                  "Acknowledged %u out of %u outbound stanzas\n",
	                  js->sm_outbound_confirmed, js->sm_outbound_count);
	return TRUE;
}

static void
sm_handle_enabled(JabberStream *js, xmlnode *packet)
{
	JabberSmSession *sess = sm_session_get(js);
	const char *id = xmlnode_get_attrib(packet, "id");
	const char *resume = xmlnode_get_attrib(packet, "resume");
	const char *max = xmlnode_get_attrib(packet, "max");

	js->sm_inbound_count = 0;
	js->sm_state = SM_ENABLED;

	sm_forget_resumption(sess);
	if (id && *id && (purple_strequal(resume, "true") || purple_strequal(resume, "1"))) {
		sess->id = g_strdup(id);
		sess->full_jid = jabber_id_get_full_jid(js->user);
		sess->max = max ? strtoul(max, NULL, 10) : 0;
	}

	purple_debug_info("XEP-0198", "Stream management is enabled%s\n",
	                  sess->id ? " (resumable)" : "");
}

void
jabber_sm_resumed(JabberStream *js, xmlnode *packet)
{
	JabberSmSession *sess = sm_session_get(js);
	GList *resend = NULL, *l;
	xmlnode *stanza;
	gchar *full_jid;

	if (js->sm_state != SM_RESUMING || sess->id == NULL ||
			!purple_strequal(xmlnode_get_attrib(packet, "previd"), sess->id)) {
		purple_debug_error("XEP-0198", "Unexpected <resumed/>\n");
		purple_connection_error_reason(js->gc,
			PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
			_("Invalid response from server"));
		return;
	}

	js->sm_state = SM_ENABLED;
	sess->dropped_at = 0;

	if (!sm_process_ack(js, xmlnode_get_attrib(packet, "h"))) {
		purple_connection_error_reason(js->gc,
			PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
			_("Invalid response from server"));
		return;
	}

	/* The server has everything up to h; what's left in the queue gets
	 * sent (and counted) again. */
	while ((stanza = g_queue_pop_head(sess->queue)) != NULL)
		resend = g_list_append(resend, stanza);
	js->sm_outbound_count = js->sm_outbound_confirmed;
	sess->head_seq = js->sm_outbound_confirmed;

	purple_debug_info("XEP-0198", "Resumed session %s, re-sending %u stanzas\n",
	                  sess->id, g_list_length(resend));

	for (l = resend; l; l = l->next)
		sm_resend(js, l->data);
	g_list_free_full(resend, (GDestroyNotify)xmlnode_free);

	full_jid = g_strdup(sess->full_jid);
	if (!jabber_stream_set_bound_jid(js, full_jid)) {
		g_free(full_jid);
		return;
	}
	g_free(full_jid);

	jabber_stream_set_state(js, JABBER_STREAM_POST_AUTH);

	/*
	 * The old connection took libpurple's view of the roster presence and
	 * our chats with it.  Going unavailable makes the next available
	 * presence (sent once the roster is in) an initial one, so the server
	 * probes the contacts again (RFC 6121 4.2, 4.5).
	 */
	stanza = xmlnode_new("presence");
	xmlnode_set_attrib(stanza, "type", "unavailable");
	jabber_send(js, stanza);
	xmlnode_free(stanza);

	jabber_disco_items_server(js);
}

void
jabber_sm_resume_failed(JabberStream *js, xmlnode *packet)
{
	JabberSmSession *sess = sm_session_get(js);

	purple_debug_info("XEP-0198", "Could not resume session %s, binding a new one\n",
	                  sess->id ? sess->id : "(none)");

	/* The server may tell us how much of the old session it got. */
	if (packet && xmlnode_get_attrib(packet, "h"))
		sm_process_ack(js, xmlnode_get_attrib(packet, "h"));

	sm_forget_resumption(sess);
	sm_steal_queue(sess);
	js->sm_state = SM_DISABLED;
	js->sm_inbound_count = 0;
	js->sm_outbound_count = 0;
	js->sm_outbound_confirmed = 0;
}

/* Processes incoming NS_STREAM_MANAGEMENT packets. */
void
jabber_sm_process_packet(JabberStream *js, xmlnode *packet) {
	const char *name = packet->name;
	if (purple_strequal(name, "enabled")) {
		sm_handle_enabled(js, packet);
	} else if (purple_strequal(name, "resumed")) {
		jabber_sm_resumed(js, packet);
	} else if (purple_strequal(name, "failed")) {
		if (js->sm_state == SM_RESUMING) {
			jabber_sm_resume_failed(js, packet);
			/* Resumption replaced resource binding; do it now. */
			js->sm_state = SM_PLANNED;
			jabber_bind_start(js);
		} else {
			JabberSmSession *sess = sm_session_get(js);
			purple_debug_error("XEP-0198", "Failed to enable stream management\n");
			js->sm_state = SM_DISABLED;
			sm_forget_resumption(sess);
			g_queue_free_full(sess->queue, (GDestroyNotify)xmlnode_free);
			sess->queue = g_queue_new();
			sess->head_seq = 0;
		}
	} else if (purple_strequal(name, "r")) {
		jabber_sm_ack_send(js);
	} else if (purple_strequal(name, "a")) {
		jabber_sm_ack_read(js, packet);
	} else {
		purple_debug_error("XEP-0198", "Unknown packet: %s\n", name);
	}
}

/* Sends an acknowledgement. */
void
jabber_sm_ack_send(JabberStream *js)
{
	xmlnode *ack;
	char *ack_h;
	if (js->sm_state != SM_ENABLED) {
		return;
	}
	ack = xmlnode_new("a");
	ack_h = g_strdup_printf("%u", js->sm_inbound_count);
	xmlnode_set_namespace(ack, NS_STREAM_MANAGEMENT);
	xmlnode_set_attrib(ack, "h", ack_h);
	jabber_send(js, ack);
	xmlnode_free(ack);
	g_free(ack_h);
}

/* Reads acknowledgements, removes queued stanzas. */
void
jabber_sm_ack_read(JabberStream *js, xmlnode *packet)
{
	sm_process_ack(js, xmlnode_get_attrib(packet, "h"));
}

/* Asks a server to enable stream management (after resource binding). */
void
jabber_sm_enable(JabberStream *js)
{
	xmlnode *enable;

	sm_steal_queue(sm_session_get(js));

	js->server_caps |= JABBER_CAP_STREAM_MANAGEMENT;
	purple_debug_info("XEP-0198", "Enabling stream management\n");
	enable = xmlnode_new("enable");
	xmlnode_set_namespace(enable, NS_STREAM_MANAGEMENT);
	xmlnode_set_attrib(enable, "resume", "true");
	jabber_send(js, enable);
	xmlnode_free(enable);
	js->sm_outbound_count = 0;
	js->sm_outbound_confirmed = 0;
	js->sm_state = SM_REQUESTED;
}

xmlnode *
jabber_sm_inline_enable_node(JabberStream *js)
{
	xmlnode *enable;

	sm_steal_queue(sm_session_get(js));

	js->server_caps |= JABBER_CAP_STREAM_MANAGEMENT;
	enable = xmlnode_new("enable");
	xmlnode_set_namespace(enable, NS_STREAM_MANAGEMENT);
	xmlnode_set_attrib(enable, "resume", "true");
	return enable;
}

void
jabber_sm_inline_enable_sent(JabberStream *js)
{
	js->sm_outbound_count = 0;
	js->sm_outbound_confirmed = 0;
	js->sm_state = SM_REQUESTED;
}

gboolean
jabber_sm_can_resume(JabberStream *js)
{
	JabberSmSession *sess;
	gchar *jid;

	if (jabber_sm_accounts == NULL || js->user == NULL)
		return FALSE;

	jid = jabber_id_get_bare_jid(js->user);
	sess = g_hash_table_lookup(jabber_sm_accounts, jid);
	g_free(jid);

	if (sess == NULL || sess->id == NULL || sess->full_jid == NULL ||
			sess->dropped_at == 0)
		return FALSE;

	if (sess->max > 0 &&
			g_get_monotonic_time() - sess->dropped_at > (gint64)sess->max * G_USEC_PER_SEC) {
		purple_debug_info("XEP-0198", "Session %s is too old to resume\n", sess->id);
		sm_forget_resumption(sess);
		return FALSE;
	}

	return TRUE;
}

xmlnode *
jabber_sm_resume_node(JabberStream *js)
{
	JabberSmSession *sess = sm_session_get(js);
	xmlnode *resume;
	gchar *h;

	g_return_val_if_fail(sess->id != NULL, NULL);

	/* Continue counting where the dropped stream stopped. */
	js->sm_inbound_count = sess->inbound_count;
	js->sm_outbound_count = sess->outbound_count;
	js->sm_outbound_confirmed = sess->outbound_confirmed;
	js->sm_state = SM_RESUMING;

	resume = xmlnode_new("resume");
	xmlnode_set_namespace(resume, NS_STREAM_MANAGEMENT);
	h = g_strdup_printf("%u", sess->inbound_count);
	xmlnode_set_attrib(resume, "h", h);
	g_free(h);
	xmlnode_set_attrib(resume, "previd", sess->id);

	purple_debug_info("XEP-0198", "Resuming session %s (h=%u)\n",
	                  sess->id, sess->inbound_count);
	return resume;
}

gboolean
jabber_sm_resume_start(JabberStream *js)
{
	xmlnode *resume;

	if (!jabber_sm_can_resume(js))
		return FALSE;

	resume = jabber_sm_resume_node(js);
	jabber_send(js, resume);
	xmlnode_free(resume);
	return TRUE;
}

void
jabber_sm_session_started(JabberStream *js)
{
	JabberSmSession *sess = sm_session_get(js);
	GList *pending, *l;
	guint n = 0;

	sm_steal_queue(sess);
	pending = sess->pending;
	sess->pending = NULL;

	for (l = pending; l; l = l->next) {
		xmlnode *stanza = l->data;
		/* Only messages are worth sending again in a new session; the
		 * old IQs have no one waiting for their results and presence
		 * is sent afresh anyway. */
		if (purple_strequal(stanza->name, "message")) {
			sm_resend(js, stanza);
			n++;
		}
	}
	if (n > 0)
		purple_debug_info("XEP-0198", "Re-sent %u unacknowledged messages\n", n);

	g_list_free_full(pending, (GDestroyNotify)xmlnode_free);
}

gboolean
jabber_sm_stream_closing(JabberStream *js)
{
	JabberSmSession *sess;
	gchar *jid;

	if (jabber_sm_accounts == NULL || js->user == NULL)
		return FALSE;

	jid = jabber_id_get_bare_jid(js->user);
	sess = g_hash_table_lookup(jabber_sm_accounts, jid);
	g_free(jid);
	if (sess == NULL)
		return FALSE;

	if (js->sm_network_drop) {
		if (js->sm_state == SM_ENABLED && sess->id != NULL) {
			sess->inbound_count = js->sm_inbound_count;
			sess->outbound_count = js->sm_outbound_count;
			sess->outbound_confirmed = js->sm_outbound_confirmed;
			sess->dropped_at = g_get_monotonic_time();
			purple_debug_info("XEP-0198",
			                  "Connection lost; keeping session %s resumable "
			                  "(h=%u, %u unacked)\n", sess->id,
			                  sess->inbound_count,
			                  g_queue_get_length(sess->queue));
		}
		/* An earlier dropped session stays as it was (its counters were
		 * saved when it dropped; SM_RESUMING never got an answer). */
		return sess->id != NULL;
	}

	/* A clean close ends the session on the server. */
	sm_forget_resumption(sess);
	return FALSE;
}

/* Tracks outbound stanzas, stores those into a queue, requests
   acknowledgements. */
void
jabber_sm_outbound(JabberStream *js, xmlnode *packet)
{
	if (jabber_is_stanza(packet)
	    && (js->sm_state == SM_REQUESTED || js->sm_state == SM_ENABLED)) {
		/* Counting stanzas even if there's no confirmation that SM is
		   enabled yet, so that we won't miss any. */
		xmlnode *req;
		JabberSmSession *sess = sm_session_get(js);

		if (g_queue_get_length(sess->queue) >= MAX_QUEUE_LENGTH) {
			/* Drop the oldest; it won't be re-sent after a drop. */
			xmlnode_free(g_queue_pop_head(sess->queue));
			sess->head_seq++;
			purple_debug_warning("XEP-0198",
			                     "Unacked stanza queue is full (%u), dropping the oldest\n",
			                     MAX_QUEUE_LENGTH);
		}
		g_queue_push_tail(sess->queue, xmlnode_copy(packet));

		/* Count the stanza */
		js->sm_outbound_count++;

		/* Requesting acknowledgements with either SM_REQUESTED or
		   SM_ENABLED state as well, so that it would be harder to lose
		   stanzas. */
		req = xmlnode_new("r");
		xmlnode_set_namespace(req, NS_STREAM_MANAGEMENT);
		jabber_send(js, req);
		xmlnode_free(req);
	}
}

/* Counts inbound stanzas. */
void
jabber_sm_inbound(JabberStream *js, xmlnode *packet)
{
	/* Count stanzas for XEP-0198, excluding stream management
	   packets. */
	if (jabber_is_stanza(packet)) {
		js->sm_inbound_count++;
	}
}
