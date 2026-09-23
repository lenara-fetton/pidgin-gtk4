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

#include "debug.h"
#include "signals.h"
#include "util.h"
#include "value.h"

#include "chat.h"
#include "iq.h"
#include "kvstore.h"
#include "mam.h"
#include "message.h"

/*
 * XEP-0313 Message Archive Management.
 *
 * - Account catch-up: after connecting, the own archive is read forward
 *   from the last archive id we showed ("mam/last-id" in the UI's
 *   key/value store, see kvstore.h).  Without one, a UI that indexes
 *   messages ("message-meta") gets the last JABBER_MAM_CATCHUP_SECONDS;
 *   other UIs only learn the current position, so that a reconnect in
 *   the same session can catch up without replaying history into logs.
 * - Room catch-up: the same per room ("mam/last-id/<room@server>") after
 *   (re)joining a room that advertises urn:xmpp:mam:2.
 * - Scroll-back: the "mam-fetch-older" IPC command fetches one page before
 *   a given id.
 *
 * Results are unwrapped and parsed as ordinary messages (so they are
 * logged through serv_got_*), flagged PURPLE_MESSAGE_DELAYED with the
 * archive's timestamp, and described by receiving-message-meta with
 * "mam" = "1".  Each query ends with the "mam-query-done" signal.
 *
 * A bounded per-account set of ids seen in this session keeps a message
 * that was shown live from being shown again from the archive.
 */

static PurplePlugin *mam_plugin = NULL;

/**************************************************************************
 * Session state (per account, lives as long as the process)
 **************************************************************************/

typedef struct {
	GHashTable *seen;     /* key -> key (a set) */
	GQueue order;         /* insertion order, for eviction */
	GHashTable *last_ids; /* kv key -> archive id */
} MamSession;

static GHashTable *sessions = NULL; /* PurpleAccount* -> MamSession* */

static void
mam_session_free(MamSession *session)
{
	g_hash_table_destroy(session->seen);
	g_queue_clear(&session->order);
	g_hash_table_destroy(session->last_ids);
	g_free(session);
}

static MamSession *
mam_session_get(PurpleAccount *account)
{
	MamSession *session;

	if (sessions == NULL)
		sessions = g_hash_table_new_full(g_direct_hash, g_direct_equal,
				NULL, (GDestroyNotify)mam_session_free);

	session = g_hash_table_lookup(sessions, account);
	if (session == NULL) {
		session = g_new0(MamSession, 1);
		session->seen = g_hash_table_new_full(g_str_hash, g_str_equal,
				g_free, NULL);
		g_queue_init(&session->order);
		session->last_ids = g_hash_table_new_full(g_str_hash, g_str_equal,
				g_free, g_free);
		g_hash_table_insert(sessions, account, session);
	}

	return session;
}

gboolean
jabber_mam_seen(PurpleAccount *account, const char *key, gboolean add)
{
	MamSession *session;
	char *copy;

	g_return_val_if_fail(key != NULL, FALSE);

	session = mam_session_get(account);
	if (g_hash_table_lookup(session->seen, key))
		return TRUE;

	if (!add)
		return FALSE;

	copy = g_strdup(key);
	g_hash_table_insert(session->seen, copy, copy);
	g_queue_push_tail(&session->order, copy);

	while (g_queue_get_length(&session->order) > JABBER_MAM_SEEN_MAX) {
		char *old = g_queue_pop_head(&session->order);
		g_hash_table_remove(session->seen, old); /* frees old */
	}

	return FALSE;
}

static char *
mam_kv_key(const char *room_jid)
{
	return room_jid ? g_strdup_printf("mam/last-id/%s", room_jid)
	                : g_strdup("mam/last-id");
}

char *
jabber_mam_get_last_id(PurpleAccount *account, const char *room_jid)
{
	char *key = mam_kv_key(room_jid);
	char *id = jabber_kv_load(account, key);

	if (id == NULL)
		id = g_strdup(g_hash_table_lookup(mam_session_get(account)->last_ids, key));

	g_free(key);
	return id;
}

static void
mam_store_last_id(PurpleAccount *account, const char *key, const char *id)
{
	if (key == NULL || id == NULL)
		return;

	g_hash_table_replace(mam_session_get(account)->last_ids,
	                     g_strdup(key), g_strdup(id));
	jabber_kv_store(account, key, id);
}

static void
mam_forget_last_id(PurpleAccount *account, const char *key)
{
	g_hash_table_remove(mam_session_get(account)->last_ids, key);
	jabber_kv_store(account, key, NULL);
}

/**************************************************************************
 * Queries: pure parts
 **************************************************************************/

JabberMamQuery *
jabber_mam_query_new(JabberStream *js, JabberMamQueryKind kind,
                     const char *archive, gboolean muc)
{
	JabberMamQuery *query = g_new0(JabberMamQuery, 1);

	query->js = js;
	query->kind = kind;
	query->queryid = g_uuid_string_random();
	query->archive = g_strdup(archive);
	query->muc = muc;
	query->max = (kind == JABBER_MAM_QUERY_POSITION) ? 1 : JABBER_MAM_PAGE_SIZE;
	query->max_pages = (kind == JABBER_MAM_QUERY_CATCHUP) ?
		JABBER_MAM_CATCHUP_MAX_PAGES : 1;

	return query;
}

void
jabber_mam_query_free(JabberMamQuery *query)
{
	if (query == NULL)
		return;

	g_free(query->queryid);
	g_free(query->archive);
	g_free(query->with);
	g_free(query->conv_name);
	g_free(query->kv_key);
	g_free(query->after);
	g_free(query->before);
	g_free(query->first);
	g_free(query->last);
	g_free(query);
}

static void
add_field(xmlnode *x, const char *var, const char *type, const char *value)
{
	xmlnode *field = xmlnode_new_child(x, "field");
	xmlnode *val;

	xmlnode_set_attrib(field, "var", var);
	if (type)
		xmlnode_set_attrib(field, "type", type);
	val = xmlnode_new_child(field, "value");
	xmlnode_insert_data(val, value, -1);
}

xmlnode *
jabber_mam_query_build(JabberMamQuery *query)
{
	xmlnode *node, *x, *set, *child;
	char buf[16];

	node = xmlnode_new("query");
	xmlnode_set_namespace(node, NS_MAM);
	xmlnode_set_attrib(node, "queryid", query->queryid);

	x = xmlnode_new_child(node, "x");
	xmlnode_set_namespace(x, NS_XDATA);
	xmlnode_set_attrib(x, "type", "submit");
	add_field(x, "FORM_TYPE", "hidden", NS_MAM);
	if (query->with)
		add_field(x, "with", NULL, query->with);
	if (query->kind == JABBER_MAM_QUERY_CATCHUP && query->after == NULL &&
	    query->start > 0) {
		struct tm *tm = gmtime(&query->start);
		add_field(x, "start", NULL,
		          purple_utf8_strftime("%Y-%m-%dT%H:%M:%SZ", tm));
	}

	set = xmlnode_new_child(node, "set");
	xmlnode_set_namespace(set, NS_RSM);
	child = xmlnode_new_child(set, "max");
	g_snprintf(buf, sizeof(buf), "%u", query->max);
	xmlnode_insert_data(child, buf, -1);

	switch (query->kind) {
		case JABBER_MAM_QUERY_CATCHUP:
			if (query->after) {
				child = xmlnode_new_child(set, "after");
				xmlnode_insert_data(child, query->after, -1);
			}
			break;
		case JABBER_MAM_QUERY_OLDER:
			child = xmlnode_new_child(set, "before");
			if (query->before && *query->before)
				xmlnode_insert_data(child, query->before, -1);
			break;
		case JABBER_MAM_QUERY_POSITION:
			/* An empty <before/> asks for the last page. */
			xmlnode_new_child(set, "before");
			break;
	}

	return node;
}

static void
replace_str(char **dest, const char *value)
{
	g_free(*dest);
	*dest = g_strdup(value);
}

JabberMamPageAction
jabber_mam_query_page_done(JabberMamQuery *query, gboolean complete,
                           const char *rsm_first, const char *rsm_last)
{
	query->pages++;
	query->page_count = 0;

	switch (query->kind) {
		case JABBER_MAM_QUERY_CATCHUP:
			if (rsm_first && query->first == NULL)
				replace_str(&query->first, rsm_first);
			if (rsm_last) {
				replace_str(&query->last, rsm_last);
				replace_str(&query->after, rsm_last);
			}
			if (complete || rsm_last == NULL) {
				/* An empty page means we have everything. */
				query->complete = TRUE;
				return JABBER_MAM_PAGE_DONE;
			}
			if (query->pages >= query->max_pages) {
				query->complete = FALSE;
				return JABBER_MAM_PAGE_DONE;
			}
			return JABBER_MAM_PAGE_NEXT;

		case JABBER_MAM_QUERY_OLDER:
			/* One page per request; the UI asks again with before=first. */
			if (rsm_first)
				replace_str(&query->first, rsm_first);
			if (rsm_last && query->last == NULL)
				replace_str(&query->last, rsm_last);
			query->complete = complete || rsm_first == NULL;
			return JABBER_MAM_PAGE_DONE;

		case JABBER_MAM_QUERY_POSITION:
			if (rsm_last) {
				replace_str(&query->first, rsm_last);
				replace_str(&query->last, rsm_last);
			}
			query->complete = TRUE;
			return JABBER_MAM_PAGE_DONE;
	}

	return JABBER_MAM_PAGE_DONE;
}

xmlnode *
jabber_mam_unwrap(xmlnode *packet, gboolean *is_result, const char **queryid,
                  const char **id, gboolean *has_stamp, time_t *stamp)
{
	xmlnode *result, *forwarded, *delay, *inner;

	if (is_result)
		*is_result = FALSE;
	if (queryid)
		*queryid = NULL;
	if (id)
		*id = NULL;
	if (has_stamp)
		*has_stamp = FALSE;
	if (stamp)
		*stamp = 0;

	if (packet == NULL)
		return NULL;

	result = xmlnode_get_child_with_namespace(packet, "result", NS_MAM);
	if (result == NULL)
		return NULL;

	if (is_result)
		*is_result = TRUE;
	if (queryid)
		*queryid = xmlnode_get_attrib(result, "queryid");
	if (id)
		*id = xmlnode_get_attrib(result, "id");

	forwarded = xmlnode_get_child_with_namespace(result, "forwarded", NS_FORWARD);
	if (forwarded == NULL)
		return NULL;

	delay = xmlnode_get_child_with_namespace(forwarded, "delay", NS_DELAYED_DELIVERY);
	if (delay && xmlnode_get_attrib(delay, "stamp")) {
		if (has_stamp)
			*has_stamp = TRUE;
		if (stamp)
			*stamp = purple_str_to_time(xmlnode_get_attrib(delay, "stamp"),
			                            TRUE, NULL, NULL, NULL);
	}

	inner = xmlnode_get_child(forwarded, "message");
	if (inner && xmlnode_get_namespace(inner) &&
	    !purple_strequal(xmlnode_get_namespace(inner), NS_XMPP_CLIENT))
		return NULL;

	return inner;
}

/**************************************************************************
 * Queries: stream side
 **************************************************************************/

static void mam_send(JabberMamQuery *query);

static gboolean
jid_bare_equal(const char *jid, const char *bare)
{
	char *tmp;
	gboolean equal;

	if (jid == NULL || bare == NULL)
		return FALSE;

	tmp = jabber_get_bare_jid(jid);
	equal = tmp && g_ascii_strcasecmp(tmp, bare) == 0;
	g_free(tmp);
	return equal;
}

static JabberChat *
mam_find_chat(JabberStream *js, const char *room_jid)
{
	JabberID *jid = jabber_id_new(room_jid);
	JabberChat *chat = NULL;

	if (jid) {
		chat = jabber_chat_find(js, jid->node, jid->domain);
		jabber_id_free(jid);
	}
	return chat;
}

static void
mam_register(JabberStream *js, JabberMamQuery *query)
{
	if (js->mam_queries == NULL)
		js->mam_queries = g_hash_table_new_full(g_str_hash, g_str_equal,
				NULL, (GDestroyNotify)jabber_mam_query_free);

	g_hash_table_insert(js->mam_queries, query->queryid, query);
}

static void
mam_finish(JabberMamQuery *query)
{
	JabberStream *js = query->js;
	PurpleAccount *account = purple_connection_get_account(js->gc);

	purple_debug_info("jabber", "MAM query %s on %s done: %u results in %u "
	                  "page(s), %scomplete\n", query->queryid, query->archive,
	                  query->delivered, query->pages,
	                  query->complete ? "" : "in");

	if (query->kind != JABBER_MAM_QUERY_OLDER) {
		if (query->muc) {
			JabberChat *chat = mam_find_chat(js, query->archive);
			if (chat)
				chat->mam_catchup_done = TRUE;
		} else {
			js->mam_catchup_done = TRUE;
		}
	}

	if (mam_plugin)
		purple_signal_emit(mam_plugin, "mam-query-done", account,
		                   query->conv_name, query->first, query->last,
		                   (guint)query->complete);

	g_hash_table_remove(js->mam_queries, query->queryid); /* frees query */
}

static gboolean
mam_error_is(xmlnode *packet, const char *condition)
{
	xmlnode *error = xmlnode_get_child(packet, "error");
	return error && xmlnode_get_child_with_namespace(error, condition,
	                                                  NS_XMPP_STANZAS);
}

static void
mam_fin_cb(JabberStream *js, const char *from, JabberIqType type,
           const char *id, xmlnode *packet, gpointer data)
{
	JabberMamQuery *query = data;
	PurpleAccount *account = purple_connection_get_account(js->gc);
	xmlnode *fin, *set, *node;
	char *first = NULL, *last = NULL;
	gboolean complete;

	/* Make sure the query wasn't dropped in the meantime. */
	if (js->mam_queries == NULL ||
	    g_hash_table_lookup(js->mam_queries, query->queryid) != query)
		return;

	if (type == JABBER_IQ_ERROR) {
		if (query->kind == JABBER_MAM_QUERY_CATCHUP && query->after &&
		    !query->retried && mam_error_is(packet, "item-not-found")) {
			/* The archive no longer has our last id (expired). */
			purple_debug_warning("jabber", "MAM: archive %s doesn't know "
			                     "id %s any more; restarting catch-up\n",
			                     query->archive, query->after);
			mam_forget_last_id(account, query->kv_key);
			query->retried = TRUE;
			g_free(query->after);
			query->after = NULL;
			if (!query->muc && jabber_ui_supports_message_meta()) {
				query->start = time(NULL) - JABBER_MAM_CATCHUP_SECONDS;
			} else {
				query->kind = JABBER_MAM_QUERY_POSITION;
				query->max = 1;
			}
			mam_send(query);
			return;
		} else {
			char *msg = jabber_parse_error(js, packet, NULL);
			purple_debug_warning("jabber", "MAM query on %s failed: %s\n",
			                     query->archive, msg ? msg : "(unknown)");
			g_free(msg);
			query->complete = FALSE;
			mam_finish(query);
			return;
		}
	}

	fin = xmlnode_get_child_with_namespace(packet, "fin", NS_MAM);
	complete = fin && purple_strequal(xmlnode_get_attrib(fin, "complete"), "true");
	set = fin ? xmlnode_get_child_with_namespace(fin, "set", NS_RSM) : NULL;
	if (set) {
		if ((node = xmlnode_get_child(set, "first")))
			first = xmlnode_get_data(node);
		if ((node = xmlnode_get_child(set, "last")))
			last = xmlnode_get_data(node);
	}

	if (jabber_mam_query_page_done(query, complete, first, last) == JABBER_MAM_PAGE_NEXT) {
		if (query->kv_key)
			mam_store_last_id(account, query->kv_key, query->last);
		g_free(first);
		g_free(last);
		mam_send(query);
		return;
	}

	if (query->kv_key && query->kind != JABBER_MAM_QUERY_OLDER)
		mam_store_last_id(account, query->kv_key, query->last);

	g_free(first);
	g_free(last);
	mam_finish(query);
}

static void
mam_send(JabberMamQuery *query)
{
	JabberIq *iq = jabber_iq_new(query->js, JABBER_IQ_SET);

	if (query->muc)
		xmlnode_set_attrib(iq->node, "to", query->archive);
	xmlnode_insert_child(iq->node, jabber_mam_query_build(query));
	jabber_iq_set_callback(iq, mam_fin_cb, query);
	jabber_iq_send(iq);
}

gboolean
jabber_mam_handle_result(JabberStream *js, xmlnode *packet)
{
	JabberMamQuery *query = NULL;
	JabberMessageContext ctx;
	gboolean is_result, has_stamp;
	const char *queryid, *id, *from, *type;
	time_t stamp;
	xmlnode *inner;

	inner = jabber_mam_unwrap(packet, &is_result, &queryid, &id,
	                          &has_stamp, &stamp);
	if (!is_result)
		return FALSE;

	if (queryid && js->mam_queries)
		query = g_hash_table_lookup(js->mam_queries, queryid);
	if (query == NULL) {
		purple_debug_warning("jabber", "Ignoring MAM result for unknown "
		                     "query %s\n", queryid ? queryid : "(none)");
		return TRUE;
	}

	/* Only the archive we asked may answer. */
	from = xmlnode_get_attrib(packet, "from");
	if (query->muc ? !jid_bare_equal(from, query->archive)
	               : (from != NULL && !jid_bare_equal(from, query->archive))) {
		purple_debug_warning("jabber", "Ignoring MAM result from %s for a "
		                     "query to %s\n", from ? from : "(none)",
		                     query->archive);
		return TRUE;
	}

	if (inner == NULL || id == NULL) {
		purple_debug_warning("jabber", "Ignoring malformed MAM result\n");
		return TRUE;
	}

	query->page_count++;
	query->delivered++;

	if (query->kind == JABBER_MAM_QUERY_POSITION)
		return TRUE;

	type = xmlnode_get_attrib(inner, "type");
	if (query->muc) {
		/* A room archive only holds the room's own messages. */
		if (!jid_bare_equal(xmlnode_get_attrib(inner, "from"), query->archive))
			return TRUE;
	} else if (purple_strequal(type, "groupchat")) {
		return TRUE;
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.origin = JABBER_MESSAGE_ORIGIN_MAM;
	ctx.mam_id = id;
	ctx.mam_archive = query->archive;
	ctx.mam_kind = (query->kind == JABBER_MAM_QUERY_OLDER) ? "older" : "catchup";
	ctx.has_stamp = has_stamp;
	ctx.stamp = stamp;

	jabber_message_parse_with_context(js, inner, &ctx);
	return TRUE;
}

static gboolean
mam_query_running(JabberStream *js, const char *archive,
                  JabberMamQueryKind kind)
{
	GHashTableIter iter;
	gpointer value;

	if (js->mam_queries == NULL)
		return FALSE;

	g_hash_table_iter_init(&iter, js->mam_queries);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		JabberMamQuery *query = value;
		if (query->kind != JABBER_MAM_QUERY_OLDER &&
		    kind != JABBER_MAM_QUERY_OLDER &&
		    purple_strequal(query->archive, archive))
			return TRUE;
	}
	return FALSE;
}

/**************************************************************************
 * Preferences
 **************************************************************************/

#define MAM_PREFS_KV "mam/prefs-set"

const char *
jabber_mam_prefs_default(xmlnode *prefs)
{
	return prefs ? xmlnode_get_attrib(prefs, "default") : NULL;
}

xmlnode *
jabber_mam_prefs_build_always(xmlnode *current)
{
	xmlnode *prefs = xmlnode_new("prefs");
	xmlnode *child;

	xmlnode_set_namespace(prefs, NS_MAM);
	xmlnode_set_attrib(prefs, "default", "always");
	/* The lists are part of the preferences: a set replaces them, so
	 * send back what the server has. */
	for (child = current ? current->child : NULL; child; child = child->next) {
		if (child->type == XMLNODE_TYPE_TAG &&
		    (purple_strequal(child->name, "always") ||
		     purple_strequal(child->name, "never")))
			xmlnode_insert_child(prefs, xmlnode_copy(child));
	}
	return prefs;
}

static void
mam_prefs_set_cb(JabberStream *js, const char *from, JabberIqType type,
                 const char *id, xmlnode *packet, gpointer data)
{
	if (type != JABBER_IQ_RESULT) {
		purple_debug_warning("jabber", "MAM prefs: setting default='always' "
		                     "failed\n");
		return;
	}
	purple_debug_info("jabber", "MAM prefs: default is now 'always'\n");
	jabber_kv_store(purple_connection_get_account(js->gc), MAM_PREFS_KV, "1");
}

static void
mam_prefs_get_cb(JabberStream *js, const char *from, JabberIqType type,
                 const char *id, xmlnode *packet, gpointer data)
{
	xmlnode *prefs = xmlnode_get_child_with_namespace(packet, "prefs", NS_MAM);
	const char *def = jabber_mam_prefs_default(prefs);
	JabberIq *iq;

	if (type != JABBER_IQ_RESULT || prefs == NULL) {
		purple_debug_info("jabber", "MAM prefs: the server has none\n");
		return;
	}

	if (purple_strequal(def, "always")) {
		purple_debug_info("jabber", "MAM prefs: default is already 'always'\n");
		jabber_kv_store(purple_connection_get_account(js->gc), MAM_PREFS_KV, "1");
		return;
	}

	purple_debug_info("jabber", "MAM prefs: default is '%s', setting 'always'\n",
	                  def ? def : "(none)");
	iq = jabber_iq_new(js, JABBER_IQ_SET);
	xmlnode_insert_child(iq->node, jabber_mam_prefs_build_always(prefs));
	jabber_iq_set_callback(iq, mam_prefs_set_cb, NULL);
	jabber_iq_send(iq);
}

void
jabber_mam_prefs_sync(JabberStream *js)
{
	PurpleAccount *account = purple_connection_get_account(js->gc);
	JabberIq *iq;
	char *done;
	xmlnode *prefs;

	if (!js->mam_supported)
		return;
	if (!purple_account_get_bool(account, "mam_prefs_always", TRUE)) {
		purple_debug_info("jabber", "MAM prefs: left alone (mam_prefs_always "
		                  "is off)\n");
		return;
	}
	done = jabber_kv_load(account, MAM_PREFS_KV);
	if (purple_strequal(done, "1")) {
		purple_debug_info("jabber", "MAM prefs: already set once\n");
		g_free(done);
		return;
	}
	g_free(done);

	iq = jabber_iq_new(js, JABBER_IQ_GET);
	prefs = xmlnode_new_child(iq->node, "prefs");
	xmlnode_set_namespace(prefs, NS_MAM);
	jabber_iq_set_callback(iq, mam_prefs_get_cb, NULL);
	jabber_iq_send(iq);
}

void
jabber_mam_catchup(JabberStream *js)
{
	PurpleAccount *account = purple_connection_get_account(js->gc);
	JabberMamQuery *query;
	char *own, *last;

	if (!js->mam_supported)
		return;

	own = jabber_id_get_bare_jid(js->user);
	if (mam_query_running(js, own, JABBER_MAM_QUERY_CATCHUP)) {
		g_free(own);
		return;
	}

	js->mam_catchup_done = FALSE;
	last = jabber_mam_get_last_id(account, NULL);

	if (last) {
		query = jabber_mam_query_new(js, JABBER_MAM_QUERY_CATCHUP, own, FALSE);
		query->after = last;
		last = NULL;
	} else if (jabber_ui_supports_message_meta()) {
		query = jabber_mam_query_new(js, JABBER_MAM_QUERY_CATCHUP, own, FALSE);
		query->start = time(NULL) - JABBER_MAM_CATCHUP_SECONDS;
	} else {
		/* No index to deduplicate against: don't replay history into the
		 * logs, only remember where the archive is now. */
		query = jabber_mam_query_new(js, JABBER_MAM_QUERY_POSITION, own, FALSE);
	}
	query->kv_key = mam_kv_key(NULL);

	purple_debug_info("jabber", "MAM: account catch-up (%s)\n",
	                  query->after ? query->after :
	                  (query->kind == JABBER_MAM_QUERY_POSITION ? "position only"
	                                                             : "by time"));

	mam_register(js, query);
	mam_send(query);
	g_free(own);
}

void
jabber_mam_muc_catchup(JabberChat *chat)
{
	JabberStream *js = chat->js;
	PurpleAccount *account = purple_connection_get_account(js->gc);
	JabberMamQuery *query;
	char *room, *last;

	if (!chat->mam_supported || chat->mam_catchup_started)
		return;

	room = g_strdup_printf("%s@%s", chat->room, chat->server);
	if (mam_query_running(js, room, JABBER_MAM_QUERY_CATCHUP)) {
		g_free(room);
		return;
	}

	chat->mam_catchup_started = TRUE;
	chat->mam_catchup_done = FALSE;
	last = jabber_mam_get_last_id(account, room);

	if (last) {
		query = jabber_mam_query_new(js, JABBER_MAM_QUERY_CATCHUP, room, TRUE);
		query->after = last;
	} else {
		/* The room's own history covers the first join. */
		query = jabber_mam_query_new(js, JABBER_MAM_QUERY_POSITION, room, TRUE);
	}
	query->conv_name = g_strdup(room);
	query->kv_key = mam_kv_key(room);

	purple_debug_info("jabber", "MAM: catch-up for %s (%s)\n", room,
	                  query->after ? query->after : "position only");

	mam_register(js, query);
	mam_send(query);
	g_free(room);
}

void
jabber_mam_note_live_id(JabberStream *js, const char *by, const char *id)
{
	PurpleAccount *account = purple_connection_get_account(js->gc);
	char *own, *key;

	if (by == NULL || id == NULL)
		return;

	own = jabber_id_get_bare_jid(js->user);
	if (g_ascii_strcasecmp(by, own) == 0) {
		/* Only move forward once the catch-up closed the gap. */
		if (js->mam_supported && js->mam_catchup_done) {
			key = mam_kv_key(NULL);
			mam_store_last_id(account, key, id);
			g_free(key);
		}
	} else {
		JabberChat *chat = mam_find_chat(js, by);

		if (chat && chat->mam_supported && chat->mam_catchup_done) {
			char *room = g_strdup_printf("%s@%s", chat->room, chat->server);
			key = mam_kv_key(room);
			mam_store_last_id(account, key, id);
			g_free(key);
			g_free(room);
		}
	}
	g_free(own);
}

void
jabber_mam_close(JabberStream *js)
{
	if (js->mam_queries) {
		g_hash_table_destroy(js->mam_queries);
		js->mam_queries = NULL;
	}
}

/**************************************************************************
 * IPC: mam-fetch-older
 **************************************************************************/

static gboolean
jabber_mam_ipc_fetch_older(PurpleAccount *account, const char *conv_name,
                           const char *before_id, guint count)
{
	PurpleConnection *gc;
	JabberStream *js;
	JabberMamQuery *query;
	JabberID *jid;
	JabberChat *chat;

	if (account == NULL || conv_name == NULL)
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
	chat = jabber_chat_find(js, jid->node, jid->domain);

	if (chat && jid->resource == NULL) {
		char *room;

		if (!chat->mam_supported) {
			jabber_id_free(jid);
			return FALSE;
		}
		room = g_strdup_printf("%s@%s", chat->room, chat->server);
		query = jabber_mam_query_new(js, JABBER_MAM_QUERY_OLDER, room, TRUE);
		g_free(room);
	} else {
		char *own;

		if (!js->mam_supported) {
			jabber_id_free(jid);
			return FALSE;
		}
		own = jabber_id_get_bare_jid(js->user);
		query = jabber_mam_query_new(js, JABBER_MAM_QUERY_OLDER, own, FALSE);
		/* Private messages in a room are addressed to the full JID. */
		query->with = chat ? jabber_id_get_full_jid(jid)
		                   : jabber_id_get_bare_jid(jid);
		g_free(own);
	}
	jabber_id_free(jid);

	query->conv_name = g_strdup(conv_name);
	query->before = g_strdup(before_id ? before_id : "");
	query->max = CLAMP(count, 1, JABBER_MAM_OLDER_MAX);

	purple_debug_info("jabber", "MAM: %u messages before %s in %s\n",
	                  query->max, *query->before ? query->before : "(now)",
	                  conv_name);

	mam_register(js, query);
	mam_send(query);
	return TRUE;
}

void
jabber_mam_init(PurplePlugin *plugin)
{
	mam_plugin = plugin;

	/* (account, conv name or NULL for the account catch-up, first id,
	 *  last id, complete) */
	purple_signal_register(plugin, "mam-query-done",
			purple_marshal_VOID__POINTER_POINTER_POINTER_POINTER_UINT,
			NULL, 5,
			purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_BOOLEAN));

	/* gboolean (account, conv name, before id or NULL, count) */
	purple_plugin_ipc_register(plugin, "mam-fetch-older",
			PURPLE_CALLBACK(jabber_mam_ipc_fetch_older),
			purple_marshal_BOOLEAN__POINTER_POINTER_POINTER_UINT,
			purple_value_new(PURPLE_TYPE_BOOLEAN), 4,
			purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_UINT));
}

void
jabber_mam_uninit(void)
{
	if (sessions) {
		g_hash_table_destroy(sessions);
		sessions = NULL;
	}
	mam_plugin = NULL;
}
