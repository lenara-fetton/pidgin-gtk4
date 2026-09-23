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
#include "pidgin-internal.h"
#include "pidgin.h"

#include "account.h"
#include "blist.h"
#include "connection.h"
#include "conversation.h"
#include "debug.h"
#include "log.h"
#include "plugin.h"
#include "prefs.h"
#include "prpl.h"
#include "signals.h"
#include "util.h"

#include "pidginconvmeta.h"
#include "pidginimageloader.h"
#include "pidginmarkup.h"

#define META_PREFS PIDGIN4_PREFS_ROOT "/conversations"
/* How many older messages one scroll-back step loads. */
#define OLDER_PAGE 50
/* A server that never answers a scroll-back query. */
#define OLDER_TIMEOUT_S 30
/* purple_conversation_set_data() keys */
#define DISPLAYED_KEY "pidgin4-displayed-id"
#define PUBLISHED_KEY "pidgin4-mds-id"
#define MARKABLE_KEY "pidgin4-markable"

typedef struct
{
	GHashTable *meta;
	gboolean fuzzy_checked;
} Pending;

typedef struct
{
	PurpleAccount *account;
	char *conv_key;          /* pending key of the conversation */
	GPtrArray *messages;     /* PidginMessage, oldest first */
	guint timeout;
	gboolean fetching;       /* a mam-fetch-older is outstanding */
} OlderBatch;

static int handle;
static PidginConvMetaUiOps ui_ops;
static gboolean initialized = FALSE;
static char *logger_path(PurpleLogCommonLoggerData *ld);
static GHashTable *pending_recv;     /* key -> Pending */
static GHashTable *pending_send;     /* key -> Pending */
static GHashTable *older_batches;    /* key -> OlderBatch */
static guint pending_idle;
static PurplePlugin *mam_plugin;     /* where mam-query-done is connected */
/* The sfs-url of the message being written (from take()): its card
 * (pidgin_message_apply_meta()) replaces the inline image or media card
 * the body URL would get. */
static char *writing_share_url = NULL;
static void share_meta_taken(PurpleConversation *conv, GHashTable *meta);
/* "<account key> <bare jid> <eme namespace>" seen this session */
static GHashTable *seen_encryption = NULL;

static char *
encryption_key(PurpleAccount *account, const char *jid, const char *ns)
{
	char *akey = pidgin_message_index_account_key(account);
	char *bare = pidgin_conv_meta_bare_jid(jid);
	char *key = g_strdup_printf("%s %s %s", akey, purple_normalize(account, bare), ns);

	g_free(akey);
	g_free(bare);
	return key;
}

void
pidgin_conv_meta_note_encryption(PurpleAccount *account, const char *jid, const char *ns)
{
	g_return_if_fail(account != NULL && jid != NULL && ns != NULL);
	if (seen_encryption == NULL)
		seen_encryption = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_hash_table_add(seen_encryption, encryption_key(account, jid, ns));
}

gboolean
pidgin_conv_meta_saw_encryption(PurpleAccount *account, const char *jid, const char *ns)
{
	char *key;
	gboolean ret;

	if (seen_encryption == NULL || account == NULL || jid == NULL || ns == NULL)
		return FALSE;
	key = encryption_key(account, jid, ns);
	ret = g_hash_table_contains(seen_encryption, key);
	g_free(key);
	return ret;
}

/* Where the last line libpurple is about to log was going to be written
 * (from "writing-*-msg", before the log write). */
static struct
{
	PurpleConversation *conv;
	char *path;              /* absolute; NULL: the log wasn't open yet */
	gint64 offset;
} last_write;

/**************************************************************************
 * Helpers
 **************************************************************************/

char *
pidgin_conv_meta_bare_jid(const char *jid)
{
	const char *slash;

	if (jid == NULL)
		return NULL;
	slash = strchr(jid, '/');
	return slash ? g_strndup(jid, slash - jid) : g_strdup(jid);
}

static gboolean
account_is_jabber(PurpleAccount *account)
{
	return account != NULL &&
	       purple_strequal(purple_account_get_protocol_id(account), "prpl-jabber");
}

/* The normalized identity of @who on @account: bare JID for XMPP. */
static char *
identity(PurpleAccount *account, const char *who)
{
	char *bare, *ret;

	if (who == NULL)
		return NULL;
	bare = account_is_jabber(account) ? pidgin_conv_meta_bare_jid(who) : g_strdup(who);
	ret = g_strdup(purple_normalize(account, bare));
	g_free(bare);
	return ret;
}

static char *
conv_pending_key(PurpleAccount *account, const char *name)
{
	return g_strdup_printf("%p:%s", (void *)account, purple_normalize(account, name));
}

static PurpleConversation *
find_conv(PurpleAccount *account, const char *name)
{
	PurpleConversation *conv;

	if (account == NULL || name == NULL)
		return NULL;
	conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, name, account);
	if (conv == NULL)
		conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, name, account);
	return conv;
}

static PidginMessageView *
conv_view(PurpleConversation *conv)
{
	return (conv != NULL && ui_ops.get_view != NULL) ? ui_ops.get_view(conv) : NULL;
}

char *
pidgin_conv_meta_snippet(const char *text, guint max)
{
	char *cut, *ret;
	glong len;

	if (text == NULL)
		return g_strdup("");
	len = g_utf8_strlen(text, -1);
	cut = g_utf8_substring(text, 0, MIN((glong)max, len));
	g_strdelimit(cut, "\r\n\t", ' ');
	ret = len > (glong)max ? g_strconcat(cut, "\xe2\x80\xa6", NULL) : g_strdup(cut);
	g_free(cut);
	return ret;
}

char *
pidgin_conv_meta_text_edited(const char *who, const char *new_text)
{
	return g_strdup_printf(_("%s edited: %s"), who ? who : "", new_text ? new_text : "");
}

char *
pidgin_conv_meta_text_reaction(const char *who, const char *emoji, gboolean add,
                               const char *target_text)
{
	char *snip = pidgin_conv_meta_snippet(target_text, 40);
	char *ret;

	if (add)
		ret = g_strdup_printf(_("%s reacted %s to: %s"), who ? who : "", emoji, snip);
	else
		ret = g_strdup_printf(_("%s removed the reaction %s from: %s"),
		                      who ? who : "", emoji, snip);
	g_free(snip);
	return ret;
}

char *
pidgin_conv_meta_text_retracted(const char *who, gboolean moderated, const char *reason)
{
	if (!moderated)
		return g_strdup_printf(_("%s retracted a message"), who ? who : "");
	if (reason != NULL && *reason != '\0')
		return g_strdup_printf(_("%s removed a message: %s"), who ? who : "", reason);
	return g_strdup_printf(_("%s removed a message"), who ? who : "");
}

gboolean
pidgin_conv_meta_is_image_url(const char *url)
{
	static const char *const exts[] = { ".png", ".jpg", ".jpeg", ".gif", ".webp",
		".bmp", ".avif", ".heic", NULL };
	GUri *uri;
	const char *path, *scheme;
	char *lower;
	gboolean ret = FALSE;
	int i;

	if (url == NULL || strpbrk(url, " \t\r\n") != NULL)
		return FALSE;
	uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
	if (uri == NULL)
		return FALSE;
	scheme = g_uri_get_scheme(uri);
	path = g_uri_get_path(uri);
	if (scheme != NULL && path != NULL && g_uri_get_host(uri) != NULL &&
	    (!g_ascii_strcasecmp(scheme, "https") || !g_ascii_strcasecmp(scheme, "http") ||
	     !g_ascii_strcasecmp(scheme, "aesgcm"))) {
		lower = g_ascii_strdown(path, -1);
		for (i = 0; exts[i] != NULL && !ret; i++)
			ret = g_str_has_suffix(lower, exts[i]);
		g_free(lower);
	}
	g_uri_unref(uri);
	return ret;
}

PidginMessage *
pidgin_conv_meta_message_from_index(const PidginIndexedMessage *row)
{
	PidginMessage *msg;
	char *html;
	PurpleMessageFlags flags = row->flags;

	if (!(flags & (PURPLE_MESSAGE_SEND | PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_SYSTEM |
	               PURPLE_MESSAGE_ERROR)))
		flags |= PURPLE_MESSAGE_RECV;
	flags |= PURPLE_MESSAGE_DELAYED;
	html = g_markup_escape_text(row->body ? row->body : "", -1);
	msg = pidgin_message_new(row->sender, row->sender, html, flags, (time_t)row->time);
	g_free(html);
	g_object_freeze_notify(G_OBJECT(msg));
	if (row->stanza_id)
		pidgin_message_set_stanza_id(msg, row->stanza_id);
	if (row->origin_id)
		pidgin_message_set_origin_id(msg, row->origin_id);
	if (row->server_id)
		pidgin_message_set_server_id(msg, row->server_id);
	if (row->occupant_id)
		pidgin_message_set_occupant_id(msg, row->occupant_id);
	if (row->reply_to)
		pidgin_message_set_reply(msg, row->reply_to, NULL, NULL);
	pidgin_message_set_index_id(msg, row->id);
	g_object_thaw_notify(G_OBJECT(msg));
	return msg;
}

/**************************************************************************
 * Dedup
 **************************************************************************/

gboolean
pidgin_conv_meta_check_discard(PidginMessageIndex *idx, const char *account_key,
                               const char *conv_key, GHashTable *meta,
                               gboolean ids_unique, PidginIndexedMessage **hit)
{
	static const char *const client_ids[] = { "stanza-id", "origin-id", NULL };
	PidginIndexedMessage *row = NULL;
	const char *id;
	gboolean mam;
	int i;

	if (hit)
		*hit = NULL;
	if (idx == NULL || meta == NULL || account_key == NULL || conv_key == NULL)
		return FALSE;

	mam = purple_strequal(g_hash_table_lookup(meta, "mam"), "1");

	/* The archive's own id is unique in its archive. */
	id = g_hash_table_lookup(meta, "server-id");
	if (id != NULL && *id != '\0')
		row = pidgin_message_index_find_by_id(idx, account_key, conv_key, id);

	/* Client-assigned ids can repeat (counters), so they only count for
	 * archive copies, or when the prpl's ids come from the server. */
	for (i = 0; row == NULL && client_ids[i] != NULL && (mam || ids_unique); i++) {
		id = g_hash_table_lookup(meta, client_ids[i]);
		if (id != NULL && *id != '\0')
			row = pidgin_message_index_find_by_id(idx, account_key, conv_key, id);
	}

	if (row == NULL)
		return FALSE;
	if (hit)
		*hit = row;
	else
		pidgin_indexed_message_free(row);
	return TRUE;
}

gboolean
pidgin_conv_meta_fuzzy_duplicate(PidginMessageIndex *idx, const char *account_key,
                                 const char *conv_key, gint64 when, const char *sender,
                                 const char *plain, PidginIndexedMessage **hit)
{
	PidginIndexedMessage *row;

	if (hit)
		*hit = NULL;
	if (idx == NULL || account_key == NULL || conv_key == NULL || plain == NULL)
		return FALSE;
	row = pidgin_message_index_find_fuzzy(idx, account_key, conv_key, when, sender, plain);
	if (row == NULL)
		return FALSE;
	if (hit)
		*hit = row;
	else
		pidgin_indexed_message_free(row);
	return TRUE;
}

/**************************************************************************
 * Pending metadata
 **************************************************************************/

static void
pending_free(Pending *p)
{
	g_hash_table_unref(p->meta);
	g_free(p);
}

static gboolean
pending_idle_cb(gpointer data)
{
	/* The write for a table follows its emission synchronously; anything
	 * left now belongs to a message that was dropped (blocked, cancelled
	 * by a plugin, discarded). */
	pending_idle = 0;
	g_hash_table_remove_all(pending_recv);
	g_hash_table_remove_all(pending_send);
	return G_SOURCE_REMOVE;
}

static void
pending_store(GHashTable *table, PurpleAccount *account, const char *name, GHashTable *meta)
{
	Pending *p = g_new0(Pending, 1);
	GHashTableIter iter;
	gpointer k, v;

	/* A copy: the table is only valid during the emission. The jabber
	 * prpl (and possibly others) frees it with g_hash_table_destroy(),
	 * which empties it even while we hold a reference. */
	p->meta = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_hash_table_iter_init(&iter, meta);
	while (g_hash_table_iter_next(&iter, &k, &v))
		g_hash_table_insert(p->meta, g_strdup(k), g_strdup(v));
	g_hash_table_replace(table, conv_pending_key(account, name), p);
	if (pending_idle == 0)
		pending_idle = g_idle_add(pending_idle_cb, NULL);
}

GHashTable *
pidgin_conv_meta_take(PurpleConversation *conv, PurpleMessageFlags flags)
{
	GHashTable *ret = NULL;
	Pending *p;
	char *key;

	g_clear_pointer(&writing_share_url, g_free);
	if (!initialized || conv == NULL || !(flags & (PURPLE_MESSAGE_SEND | PURPLE_MESSAGE_RECV)))
		return NULL;

	key = conv_pending_key(purple_conversation_get_account(conv),
	                       purple_conversation_get_name(conv));
	p = g_hash_table_lookup(pending_recv, key);
	if (p != NULL) {
		ret = g_hash_table_ref(p->meta);
		g_hash_table_remove(pending_recv, key);
	} else if ((flags & PURPLE_MESSAGE_SEND) &&
	           (p = g_hash_table_lookup(pending_send, key)) != NULL) {
		ret = g_hash_table_ref(p->meta);
		g_hash_table_remove(pending_send, key);
	}
	g_free(key);
	if (ret != NULL)
		share_meta_taken(conv, ret);
	return ret;
}

gboolean
pidgin_conv_meta_is_older(GHashTable *meta)
{
	return meta != NULL && purple_strequal(g_hash_table_lookup(meta, "mam-query"), "older");
}

/**************************************************************************
 * Scroll-back batches
 **************************************************************************/

static void
older_batch_free(OlderBatch *b)
{
	if (b->timeout)
		g_source_remove(b->timeout);
	g_ptr_array_unref(b->messages);
	g_free(b->conv_key);
	g_free(b);
}

static OlderBatch *
older_batch_get(PurpleConversation *conv, gboolean create)
{
	PurpleAccount *account = purple_conversation_get_account(conv);
	char *key = conv_pending_key(account, purple_conversation_get_name(conv));
	OlderBatch *b = g_hash_table_lookup(older_batches, key);

	if (b == NULL && create) {
		b = g_new0(OlderBatch, 1);
		b->account = account;
		b->conv_key = g_strdup(key);
		b->messages = g_ptr_array_new_with_free_func(g_object_unref);
		g_hash_table_insert(older_batches, g_strdup(key), b);
	}
	g_free(key);
	return b;
}

static void
older_finish(PurpleConversation *conv, OlderBatch *b, gboolean complete)
{
	GPtrArray *msgs = g_ptr_array_ref(b->messages);
	char *key = g_strdup(b->conv_key);

	g_hash_table_remove(older_batches, key);   /* frees b */
	g_free(key);
	if (conv != NULL && ui_ops.older_done != NULL)
		ui_ops.older_done(conv, msgs, complete);
	g_ptr_array_unref(msgs);
}

void
pidgin_conv_meta_queue_older(PurpleConversation *conv, PidginMessage *msg)
{
	OlderBatch *b;

	g_return_if_fail(conv != NULL && PIDGIN_IS_MESSAGE(msg));
	b = older_batch_get(conv, TRUE);
	g_ptr_array_add(b->messages, g_object_ref(msg));
}

static gboolean
older_timeout_cb(gpointer data)
{
	char *key = data;
	OlderBatch *b = g_hash_table_lookup(older_batches, key);
	GList *l;

	if (b == NULL)
		return G_SOURCE_REMOVE;
	b->timeout = 0;
	purple_debug_warning("gtkconv", "No answer to the archive query for %s\n", key);
	for (l = purple_get_conversations(); l != NULL; l = l->next) {
		PurpleConversation *conv = l->data;
		char *k = conv_pending_key(purple_conversation_get_account(conv),
		                           purple_conversation_get_name(conv));
		gboolean match = purple_strequal(k, key);

		g_free(k);
		if (match) {
			older_finish(conv, b, FALSE);
			return G_SOURCE_REMOVE;
		}
	}
	g_hash_table_remove(older_batches, key);
	return G_SOURCE_REMOVE;
}

static void
mam_query_done_cb(PurpleAccount *account, const char *conv_name, const char *first_id,
                  const char *last_id, guint complete, gpointer data)
{
	PurpleConversation *conv;
	OlderBatch *b = NULL;

	if (conv_name == NULL)
		return;     /* the account catch-up */
	conv = find_conv(account, conv_name);
	if (conv != NULL)
		b = older_batch_get(conv, FALSE);
	if (b == NULL || !b->fetching)
		return;     /* a room catch-up, or a page we didn't ask for */
	older_finish(conv, b, complete || first_id == NULL);
}

static PurplePlugin *
conv_prpl(PurpleConversation *conv)
{
	PurpleAccount *account = purple_conversation_get_account(conv);

	return account ? purple_find_prpl(purple_account_get_protocol_id(account)) : NULL;
}

static void
connect_mam_signal(void)
{
	PurplePlugin *prpl = purple_plugins_find_with_id("prpl-jabber");

	if (prpl == NULL || prpl == mam_plugin || !purple_plugin_is_loaded(prpl))
		return;
	purple_signal_connect(prpl, "mam-query-done", &handle,
	                      PURPLE_CALLBACK(mam_query_done_cb), NULL);
	mam_plugin = prpl;
}

static void
plugin_load_cb(PurplePlugin *plugin, gpointer data)
{
	if (purple_strequal(purple_plugin_get_id(plugin), "prpl-jabber"))
		connect_mam_signal();
}

static void
plugin_unload_cb(PurplePlugin *plugin, gpointer data)
{
	/* Unloading unregisters the plugin's signals and their handlers. */
	if (plugin == mam_plugin)
		mam_plugin = NULL;
}

gboolean
pidgin_conv_meta_load_older(PurpleConversation *conv)
{
	PidginMessageIndex *idx = pidgin_message_index_get_default();
	PidginMessageView *view = conv_view(conv);
	PurpleAccount *account;
	GListModel *model;
	PidginMessage *oldest = NULL;
	const char *before_id = NULL;
	gint64 before_time = 0;
	guint i, n;
	OlderBatch *b;
	gboolean ok = FALSE;

	g_return_val_if_fail(conv != NULL, FALSE);
	if (!initialized)
		return FALSE;
	if ((b = older_batch_get(conv, FALSE)) != NULL && b->fetching)
		return TRUE;    /* already loading */

	account = purple_conversation_get_account(conv);
	if (view != NULL) {
		model = pidgin_message_view_get_model(view);
		n = g_list_model_get_n_items(model);
		for (i = 0; i < n; i++) {
			PidginMessage *m = g_list_model_get_item(model, i);

			if (pidgin_message_get_kind(m) == PIDGIN_MESSAGE_KIND_NORMAL) {
				if (oldest == NULL) {
					oldest = m;
					before_time = pidgin_message_get_time(m);
				}
				if (pidgin_message_get_server_id(m) != NULL) {
					before_id = pidgin_message_get_server_id(m);
					g_object_unref(m);
					break;
				}
			}
			if (m != oldest)
				g_object_unref(m);
		}
	}

	/* 1. The index (every prpl, offline too). */
	if (idx != NULL) {
		char *akey = pidgin_message_index_account_key(account);
		char *ckey = pidgin_message_index_conv_key(account, purple_conversation_get_name(conv));
		GPtrArray *rows = pidgin_message_index_get_recent(idx, akey, ckey,
			before_time > 0 ? before_time : (gint64)time(NULL) + 1, OLDER_PAGE);

		g_free(akey);
		g_free(ckey);
		if (rows != NULL && rows->len > 0) {
			b = older_batch_get(conv, TRUE);
			/* newest first -> oldest first */
			for (i = rows->len; i > 0; i--) {
				PidginIndexedMessage *row = g_ptr_array_index(rows, i - 1);

				/* Correction rows only make the new text searchable. */
				if (row->correction_of != NULL)
					continue;
				g_ptr_array_add(b->messages, pidgin_conv_meta_message_from_index(row));
			}
			g_ptr_array_unref(rows);
			if (b->messages->len > 0) {
				g_clear_object(&oldest);
				older_finish(conv, b, FALSE);
				return TRUE;
			}
			g_hash_table_remove(older_batches, b->conv_key);
		} else if (rows != NULL) {
			g_ptr_array_unref(rows);
		}
	}

	/* 2. The server archive. */
	if (pidgin_conv_meta_has_command(conv, "mam-fetch-older")) {
		ok = GPOINTER_TO_INT(purple_plugin_ipc_call(conv_prpl(conv), "mam-fetch-older", NULL,
			account, purple_conversation_get_name(conv), before_id, (guint)OLDER_PAGE));
		if (ok) {
			b = older_batch_get(conv, TRUE);
			b->fetching = TRUE;
			b->timeout = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, OLDER_TIMEOUT_S,
				older_timeout_cb, g_strdup(b->conv_key), g_free);
		}
	}
	g_clear_object(&oldest);
	return ok;
}

/**************************************************************************
 * Signal handlers
 **************************************************************************/

static gboolean
ids_are_unique(PurpleAccount *account)
{
	/* XMPP message ids are chosen by the sender; IRC msgid (and Discord
	 * snowflakes) by the server. */
	return !account_is_jabber(account);
}

static void
receiving_meta_cb(PurpleAccount *account, const char *conv_name, GHashTable *meta,
                  gpointer data)
{
	PidginMessageIndex *idx;
	PidginIndexedMessage *hit = NULL;
	char *akey, *ckey;

	if (account == NULL || conv_name == NULL || meta == NULL)
		return;

	/* XEP-0380: who sends with which scheme we couldn't decrypt */
	if (g_hash_table_lookup(meta, "eme-namespace") != NULL) {
		const char *ns = g_hash_table_lookup(meta, "eme-namespace");
		const char *sender = g_hash_table_lookup(meta, "sender");

		pidgin_conv_meta_note_encryption(account, conv_name, ns);
		if (sender != NULL)
			pidgin_conv_meta_note_encryption(account, sender, ns);
	}

	idx = pidgin_message_index_get_default();
	if (idx != NULL) {
		akey = pidgin_message_index_account_key(account);
		ckey = pidgin_message_index_conv_key(account, conv_name);
		if (pidgin_conv_meta_check_discard(idx, akey, ckey, meta,
		                                   ids_are_unique(account), &hit)) {
			PurpleConversation *conv = find_conv(account, conv_name);

			purple_debug_info("gtkconv", "Discarding a message already in the "
			                  "index (%s, row %" G_GINT64_FORMAT ")\n", conv_name, hit->id);
			g_hash_table_replace(meta, g_strdup("discard"), g_strdup("1"));
			/* A scroll-back page still shows it (from the index). */
			if (conv != NULL && pidgin_conv_meta_is_older(meta)) {
				PidginMessage *m = pidgin_conv_meta_message_from_index(hit);
				pidgin_conv_meta_queue_older(conv, m);
				g_object_unref(m);
			}
			pidgin_indexed_message_free(hit);
			g_free(akey);
			g_free(ckey);
			return;
		}
		g_free(akey);
		g_free(ckey);
	}

	pending_store(pending_recv, account, conv_name, meta);
}

static void
sending_meta_cb(PurpleAccount *account, const char *conv_name, GHashTable *meta,
                gpointer data)
{
	if (account == NULL || conv_name == NULL || meta == NULL)
		return;
	/* A 1:1 correction is reported through message-corrected, not with a
	 * write, so its table must not stick to the next message. */
	if (g_hash_table_lookup(meta, "correction-of") != NULL)
		return;
	pending_store(pending_send, account, conv_name, meta);
}

static gboolean
writing_msg_cb(PurpleAccount *account, const char *who, char **message,
               PurpleConversation *conv, PurpleMessageFlags flags, gpointer data)
{
	PidginMessageIndex *idx;
	Pending *p;
	char *key;

	if (conv == NULL || message == NULL || *message == NULL)
		return FALSE;

	/* Archive results without an id hit: fuzzy dedup against lines that
	 * Pidgin 2 logged (no ids). */
	key = conv_pending_key(account, purple_conversation_get_name(conv));
	p = g_hash_table_lookup(pending_recv, key);
	if (p != NULL && !p->fuzzy_checked && (flags & (PURPLE_MESSAGE_SEND | PURPLE_MESSAGE_RECV)) &&
	    purple_strequal(g_hash_table_lookup(p->meta, "mam"), "1") &&
	    (idx = pidgin_message_index_get_default()) != NULL) {
		PidginIndexedMessage *hit = NULL;
		const char *ts = g_hash_table_lookup(p->meta, "timestamp");
		gint64 when = ts ? g_ascii_strtoll(ts, NULL, 10) : (gint64)time(NULL);
		char *plain = pidgin_markup_html_to_plain(*message);
		char *akey = pidgin_message_index_account_key(account);
		char *ckey = pidgin_message_index_conv_key(account, purple_conversation_get_name(conv));
		/* Logs hold aliases; a room nick usually is one, an IM sender
		 * isn't (any sender will do: it's one conversation). */
		const char *sender = purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_CHAT
			? g_hash_table_lookup(p->meta, "sender") : NULL;

		p->fuzzy_checked = TRUE;
		if (pidgin_conv_meta_fuzzy_duplicate(idx, akey, ckey, when, sender, plain, &hit)) {
			purple_debug_info("gtkconv", "Dropping an archived message already "
			                  "logged (row %" G_GINT64_FORMAT ")\n", hit->id);
			if (pidgin_conv_meta_is_older(p->meta)) {
				PidginMessage *m = pidgin_conv_meta_message_from_index(hit);
				pidgin_conv_meta_queue_older(conv, m);
				g_object_unref(m);
			}
			pidgin_indexed_message_free(hit);
			g_hash_table_remove(pending_recv, key);
			g_free(plain);
			g_free(akey);
			g_free(ckey);
			g_free(key);
			return TRUE;    /* neither written nor logged */
		}
		g_free(plain);
		g_free(akey);
		g_free(ckey);
	}
	g_free(key);

	/* Remember where the log line will go. */
	last_write.conv = conv;
	g_clear_pointer(&last_write.path, g_free);
	last_write.offset = -1;
	if (!(flags & PURPLE_MESSAGE_NO_LOG) && purple_conversation_is_logging(conv) &&
	    conv->logs != NULL) {
		PurpleLog *log = conv->logs->data;
		PurpleLogCommonLoggerData *ld = log ? log->logger_data : NULL;

		if (ld != NULL && ld->file != NULL) {
			last_write.path = logger_path(ld);
			last_write.offset = ftell(ld->file);
		}
	}
	return FALSE;
}

/* The file a log writes to. libpurple's common writer doesn't record the
 * path (only the readers do), so ask the kernel about its FILE. */
static char *
logger_path(PurpleLogCommonLoggerData *ld)
{
	char *link, *ret;

	if (ld == NULL)
		return NULL;
	if (ld->path != NULL)
		return g_strdup(ld->path);
	if (ld->file == NULL)
		return NULL;
	link = g_strdup_printf("/proc/self/fd/%d", fileno(ld->file));
	ret = g_file_read_link(link, NULL);
	g_free(link);
	return ret;
}

char *
pidgin_conv_meta_log_file(PurpleConversation *conv)
{
	PurpleLog *log;

	if (conv == NULL || conv->logs == NULL || (log = conv->logs->data) == NULL)
		return NULL;
	return logger_path(log->logger_data);
}

/* The offset of the last line of @path (a log just created). */
static gint64
last_line_offset(const char *path)
{
	char *contents = NULL;
	gsize len = 0;
	gint64 off = -1;

	if (g_file_get_contents(path, &contents, &len, NULL) && len > 1) {
		gsize i = len - 1;

		while (i > 0 && contents[i - 1] != '\n')
			i--;
		/* The line ends in "\n"; skip back over it first. */
		if (i == len - 1 || contents[len - 1] == '\n') {
			i = len - 1;
			while (i > 0 && contents[i - 1] != '\n')
				i--;
		}
		off = i;
	}
	g_free(contents);
	return off;
}

static char *
log_relative_path(const char *path)
{
	char *logs = g_build_filename(purple_user_dir(), "logs", NULL);
	char *real = NULL, *ret = NULL;

	/* The path from /proc is canonical; the profile path may not be. */
	if (path != NULL && !g_str_has_prefix(path, logs) && (real = realpath(logs, NULL)) != NULL) {
		g_free(logs);
		logs = g_strdup(real);
		free(real);
	}
	if (path != NULL && g_str_has_prefix(path, logs) && path[strlen(logs)] == G_DIR_SEPARATOR)
		ret = g_strdup(path + strlen(logs) + 1);
	g_free(logs);
	return ret;
}

static gint64
index_insert(PurpleConversation *conv, PidginMessage *msg, const char *body,
             const char *sender, PurpleMessageFlags flags, time_t when,
             const char *correction_of, const char *stanza_id, gboolean logged)
{
	PidginMessageIndex *idx = pidgin_message_index_get_default();
	PurpleAccount *account = purple_conversation_get_account(conv);
	PidginIndexedMessage *row;
	GError *error = NULL;
	gint64 id;

	if (idx == NULL)
		return 0;

	row = pidgin_indexed_message_new();
	row->account = pidgin_message_index_account_key(account);
	row->conv = pidgin_message_index_conv_key(account, purple_conversation_get_name(conv));
	row->is_chat = purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_CHAT;
	row->time = when;
	row->sender = g_strdup(sender);
	row->body = g_strdup(body);
	row->flags = flags;
	row->correction_of = g_strdup(correction_of);
	if (msg != NULL) {
		row->stanza_id = g_strdup(pidgin_message_get_stanza_id(msg));
		row->origin_id = g_strdup(pidgin_message_get_origin_id(msg));
		row->server_id = g_strdup(pidgin_message_get_server_id(msg));
		row->occupant_id = g_strdup(pidgin_message_get_occupant_id(msg));
		row->reply_to = g_strdup(pidgin_message_get_reply_to(msg));
		if (row->correction_of == NULL)
			row->correction_of = g_strdup(pidgin_message_get_correction_of(msg));
	} else {
		row->stanza_id = g_strdup(stanza_id);
	}

	id = pidgin_message_index_insert(idx, row, &error);
	if (id == 0) {
		purple_debug_error("gtkconv", "Indexing a message failed: %s\n",
		                   error ? error->message : "?");
		g_clear_error(&error);
	} else if (logged && last_write.conv == conv && conv->logs != NULL) {
		PurpleLog *log = conv->logs->data;
		PurpleLogCommonLoggerData *ld = log ? log->logger_data : NULL;
		char *path = logger_path(ld);
		char *rel = log_relative_path(path);
		gint64 off = last_write.offset;

		if (ld != NULL && ld->file != NULL)
			fflush(ld->file);
		if (rel != NULL && (last_write.path == NULL || off < 0 ||
		                    !purple_strequal(last_write.path, path)))
			off = last_line_offset(path);   /* the log was just opened */
		if (rel != NULL)
			pidgin_message_index_mark_log_position(idx, id, rel, off);
		g_free(rel);
		g_free(path);
	}
	pidgin_indexed_message_free(row);
	return id;
}

static void
learn_upload_host(PurpleConversation *conv, PidginMessage *msg)
{
	char *plain;
	GUri *uri;

	/* Our own XEP-0363 uploads come back as a SEND message whose body is
	 * the GET URL: that host is the account's upload service. */
	if (!(pidgin_message_get_flags(msg) & PURPLE_MESSAGE_SEND) ||
	    !account_is_jabber(purple_conversation_get_account(conv)))
		return;
	plain = g_strstrip(g_strdup(pidgin_message_get_plain_text(msg)));
	if (strpbrk(plain, " \t\r\n") == NULL &&
	    (g_str_has_prefix(plain, "https://") || g_str_has_prefix(plain, "aesgcm://")) &&
	    (uri = g_uri_parse(plain, G_URI_FLAGS_NONE, NULL)) != NULL) {
		const char *host = g_uri_get_host(uri);

		if (host != NULL && pidgin_image_loader_get_default() != NULL &&
		    !pidgin_image_loader_is_allowed(pidgin_image_loader_get_default(), plain)) {
			purple_debug_info("gtkconv", "Allowing inline images from our upload "
			                  "host %s\n", host);
			pidgin_image_loader_allow_host(pidgin_image_loader_get_default(), host);
		}
		g_uri_unref(uri);
	}
	g_free(plain);
}

/* Hosts on the account's own XMPP domain (upload.example.net, ...): our
 * server sees our address anyway, so loading from it leaks nothing. */
static gboolean
host_on_account_domain(PurpleAccount *account, const char *host)
{
	char *bare, *domain, *h;
	const char *at;
	gboolean ret = FALSE;

	if (!account_is_jabber(account) || host == NULL)
		return FALSE;
	bare = pidgin_conv_meta_bare_jid(purple_account_get_username(account));
	at = strchr(bare, '@');
	domain = g_ascii_strdown(at ? at + 1 : bare, -1);
	h = g_ascii_strdown(host, -1);
	if (*domain != '\0') {
		size_t hl = strlen(h), dl = strlen(domain);
		ret = purple_strequal(h, domain) ||
		      (hl > dl && h[hl - dl - 1] == '.' && purple_strequal(h + hl - dl, domain));
	}
	g_free(h);
	g_free(domain);
	g_free(bare);
	return ret;
}

/**************************************************************************
 * XMPP file shares
 *
 * An XMPP message whose body is exactly one URL is a file share (XEP-0066
 * jabber:x:oob, which the prpl passes through as the body, or a client
 * that sends only the upload's GET URL): an attachment someone chose to
 * send, not a link in text. With /pidgin4/images/inline_xmpp_shares (on
 * by default) such a URL is allowed for the image loader whatever its
 * host (only that URI; the size cap and the cache apply as usual) when it
 * is an image: by its extension, or else by the Content-Type of a HEAD
 * request (pidgin_image_loader_probe_async(), https only). An <img src>
 * in HTML from an unknown host, or a URL among other text, stays a link.
 **************************************************************************/

static char *share_prpl_for_tests = NULL;

/* Plain text, or text whose only markup is links and line breaks: what a
 * lone URL looks like once purple_markup_linkify() has run on it. */
static gboolean
markup_is_text_or_links(const char *html)
{
	const char *c;

	if (html == NULL || pidgin_markup_is_plain(html))
		return TRUE;
	for (c = html; (c = strchr(c, '<')) != NULL; c++) {
		if (!g_ascii_strncasecmp(c, "<a ", 3) || !g_ascii_strncasecmp(c, "</a>", 4) ||
		    !g_ascii_strncasecmp(c, "<br", 3))
			continue;
		if (g_ascii_isalpha(c[1]) || c[1] == '/')
			return FALSE;
	}
	return TRUE;
}

static gboolean
account_has_shares(PurpleAccount *account)
{
	return account_is_jabber(account) ||
	       (share_prpl_for_tests != NULL && account != NULL &&
	        purple_strequal(purple_account_get_protocol_id(account), share_prpl_for_tests));
}

void
pidgin_conv_meta_set_share_protocol_for_tests(const char *protocol_id)
{
	g_free(share_prpl_for_tests);
	share_prpl_for_tests = g_strdup(protocol_id);
}

gboolean
pidgin_conv_meta_inline_xmpp_shares(void)
{
	return !purple_prefs_exists(PIDGIN4_PREFS_ROOT "/images/inline_xmpp_shares") ||
	       purple_prefs_get_bool(PIDGIN4_PREFS_ROOT "/images/inline_xmpp_shares");
}

char *
pidgin_conv_meta_share_url(PurpleConversation *conv, const char *html)
{
	if (conv == NULL || !account_has_shares(purple_conversation_get_account(conv)))
		return NULL;
	return pidgin_conv_meta_lone_url(html);
}

/*
 * XEP-0447 stateless file sharing (sfs-* meta keys): the message gets a
 * card from the metadata (pidgin_attachment_new_for_share(), applied by
 * pidgin_message_apply_meta()), so the body URL, normally the share's own
 * URL, is left a link: no inline copy, no HEAD probe, no second card. An
 * image share is allowed for the loader by its exact URI, as a lone URL
 * share is, with /pidgin4/images/inline_xmpp_shares.
 */
static void
share_meta_taken(PurpleConversation *conv, GHashTable *meta)
{
	const char *url = g_hash_table_lookup(meta, "sfs-url");
	PidginImageLoader *loader;

	if (url == NULL || *url == '\0')
		return;
	writing_share_url = g_strdup(url);
	if (account_has_shares(purple_conversation_get_account(conv)) &&
	    pidgin_conv_meta_inline_xmpp_shares() &&
	    pidgin_attachment_classify(g_hash_table_lookup(meta, "sfs-name") ?
	                               g_hash_table_lookup(meta, "sfs-name") : url,
	                               g_hash_table_lookup(meta, "sfs-media-type")) ==
	        PIDGIN_ATTACHMENT_IMAGE &&
	    (loader = pidgin_image_loader_get_default()) != NULL &&
	    !pidgin_image_loader_is_allowed(loader, url))
		pidgin_image_loader_allow_uri(loader, url);
}

/* The body is the URL of the share being written (see above). */
static gboolean
is_writing_share(const char *plain)
{
	return writing_share_url != NULL && plain != NULL &&
	       purple_strequal(plain, writing_share_url);
}

char *
pidgin_conv_meta_lone_url(const char *html)
{
	char *plain;
	GUri *uri;
	gboolean ok = FALSE;

	if (html == NULL || !markup_is_text_or_links(html))
		return NULL;
	plain = g_strstrip(pidgin_markup_plain_from_html(html));
	if (is_writing_share(plain)) {
		g_free(plain);
		return NULL;
	}
	if (*plain != '\0' && strpbrk(plain, " \t\r\n") == NULL &&
	    (g_str_has_prefix(plain, "https://") || g_str_has_prefix(plain, "http://") ||
	     g_str_has_prefix(plain, "aesgcm://")) &&
	    (uri = g_uri_parse(plain, G_URI_FLAGS_NONE, NULL)) != NULL) {
		ok = g_uri_get_host(uri) != NULL && *g_uri_get_host(uri) != '\0' &&
		     g_uri_get_userinfo(uri) == NULL;
		g_uri_unref(uri);
	}
	if (!ok)
		g_clear_pointer(&plain, g_free);
	return plain;
}

char *
pidgin_conv_meta_inline_image_html_for_url(const char *url)
{
	PidginImageLoader *loader = pidgin_image_loader_get_default();
	char *esc, *ret;

	if (url == NULL || loader == NULL || !pidgin_image_loader_is_allowed(loader, url))
		return NULL;
	esc = g_markup_escape_text(url, -1);
	ret = g_strdup_printf("<a href=\"%s\">%s</a><br><img src=\"%s\" alt=\"%s\">",
	                      esc, esc, esc, esc);
	g_free(esc);
	return ret;
}

char *
pidgin_conv_meta_inline_image_html(PurpleConversation *conv, const char *html)
{
	PidginImageLoader *loader;
	char *plain, *ret = NULL;
	GUri *uri;

	/* a lone URL, as typed or as linkified */
	if (conv == NULL || html == NULL || !markup_is_text_or_links(html))
		return NULL;
	plain = g_strstrip(pidgin_markup_plain_from_html(html));
	if (is_writing_share(plain) || !pidgin_conv_meta_is_image_url(plain) ||
	    (loader = pidgin_image_loader_get_default()) == NULL) {
		g_free(plain);
		return NULL;
	}
	if (!pidgin_image_loader_is_allowed(loader, plain) &&
	    (uri = g_uri_parse(plain, G_URI_FLAGS_NONE, NULL)) != NULL) {
		if (host_on_account_domain(purple_conversation_get_account(conv), g_uri_get_host(uri)))
			pidgin_image_loader_allow_host(loader, g_uri_get_host(uri));
		g_uri_unref(uri);
	}
	/* An XMPP file share: this URI only, whatever the host. */
	if (!pidgin_image_loader_is_allowed(loader, plain) && pidgin_conv_meta_inline_xmpp_shares()) {
		char *share = pidgin_conv_meta_share_url(conv, html);

		if (share != NULL)
			pidgin_image_loader_allow_uri(loader, share);
		g_free(share);
	}
	ret = pidgin_conv_meta_inline_image_html_for_url(plain);
	g_free(plain);
	return ret;
}

void
pidgin_conv_meta_message_displayed(PurpleConversation *conv, PidginMessage *msg,
                                   GHashTable *meta)
{
	PurpleMessageFlags flags;
	PidginMessageView *view;
	gboolean logged;
	gint64 id;

	g_return_if_fail(conv != NULL && PIDGIN_IS_MESSAGE(msg));
	flags = pidgin_message_get_flags(msg);
	g_clear_pointer(&writing_share_url, g_free);    /* this write is done */

	if (meta != NULL) {
		const char *reply_to = g_hash_table_lookup(meta, "reply-to");

		if (purple_strequal(g_hash_table_lookup(meta, "markable"), "1"))
			g_object_set_data(G_OBJECT(msg), MARKABLE_KEY, GINT_TO_POINTER(1));
		/* A reply's quote was stripped: find the target for the preview. */
		if (reply_to != NULL && pidgin_message_get_reply_preview(msg) == NULL) {
			PidginMessage *target = NULL;
			PidginMessageIndex *idx = pidgin_message_index_get_default();

			if ((view = conv_view(conv)) != NULL)
				target = pidgin_message_view_find_by_id(view, reply_to);
			if (target == NULL && idx != NULL) {
				PurpleAccount *account = purple_conversation_get_account(conv);
				char *akey = pidgin_message_index_account_key(account);
				char *ckey = pidgin_message_index_conv_key(account,
					purple_conversation_get_name(conv));
				PidginIndexedMessage *row = pidgin_message_index_find_by_id(idx, akey,
					ckey, reply_to);

				if (row != NULL) {
					pidgin_message_set_reply(msg, reply_to,
						pidgin_message_get_reply_to_sender(msg) ?
						pidgin_message_get_reply_to_sender(msg) : row->sender,
						row->body);
					pidgin_indexed_message_free(row);
				}
				g_free(akey);
				g_free(ckey);
			}
			/* M9: not here, but the prpl sent the replied-to text (Discord) */
			if (target == NULL && pidgin_message_get_reply_preview(msg) == NULL &&
			    g_hash_table_lookup(meta, "reply-to-text") != NULL)
				pidgin_message_set_reply(msg, reply_to, pidgin_message_get_reply_to_sender(msg),
				                         g_hash_table_lookup(meta, "reply-to-text"));
		}
	}

	learn_upload_host(conv, msg);

	/* Index what the log got, and anything with ids. */
	logged = !(flags & PURPLE_MESSAGE_NO_LOG) && purple_conversation_is_logging(conv) &&
	         conv->logs != NULL;
	if (logged || pidgin_message_get_stanza_id(msg) != NULL ||
	    pidgin_message_get_server_id(msg) != NULL) {
		id = index_insert(conv, msg, pidgin_message_get_plain_text(msg),
		                  pidgin_message_get_alias(msg), flags, pidgin_message_get_time(msg),
		                  NULL, NULL, logged);
		if (id > 0)
			pidgin_message_set_index_id(msg, id);
	}
	last_write.conv = NULL;
}

void
pidgin_conv_meta_log_line(PurpleConversation *conv, const char *text, time_t when)
{
	char *esc;
	GList *l;

	g_return_if_fail(conv != NULL && text != NULL);

	if (!purple_conversation_is_logging(conv))
		return;
	/* libpurple opens the log on the first write (open_log() is private);
	 * this is the same. It closes conv->logs on destroy. */
	if (conv->logs == NULL)
		conv->logs = g_list_append(NULL, purple_log_new(
			purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_CHAT
				? PURPLE_LOG_CHAT : PURPLE_LOG_IM,
			purple_conversation_get_name(conv), purple_conversation_get_account(conv),
			conv, time(NULL), NULL));
	esc = g_markup_escape_text(text, -1);
	for (l = conv->logs; l != NULL; l = l->next)
		purple_log_write(l->data, PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LINKIFY,
		                 "", when, esc);
	g_free(esc);
}

/* Writes a native event's text fallback: log only. */
static void
event_fallback(PurpleConversation *conv, const char *text)
{
	pidgin_conv_meta_log_line(conv, text, time(NULL));
}

static const char *
display_name(PurpleConversation *conv, const char *sender)
{
	PurpleAccount *account = purple_conversation_get_account(conv);
	PurpleBuddy *buddy;
	char *id, *self;
	const char *ret = sender;

	if (sender == NULL)
		return "";
	if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_CHAT)
		return sender;
	id = identity(account, sender);
	self = identity(account, purple_account_get_username(account));
	if (purple_strequal(id, self))
		ret = purple_account_get_alias(account) ? purple_account_get_alias(account)
		                                        : purple_account_get_username(account);
	else if ((buddy = purple_find_buddy(account, id)) != NULL)
		ret = purple_buddy_get_contact_alias(buddy);
	g_free(id);
	g_free(self);
	return ret;
}

/* May @sender change @msg (corrections, retractions)? */
static gboolean
same_sender(PurpleConversation *conv, PidginMessage *msg, const char *sender)
{
	PurpleAccount *account = purple_conversation_get_account(conv);
	gboolean own = (pidgin_message_get_flags(msg) & PURPLE_MESSAGE_SEND) != 0;
	char *a, *b;
	gboolean ret;

	if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_CHAT) {
		/* The prpl compared occupant-ids already where the room has
		 * them; without, compare nicks. */
		if (purple_strequal(pidgin_message_get_sender(msg), sender))
			return TRUE;
		return own && purple_strequal(sender,
			purple_conv_chat_get_nick(PURPLE_CONV_CHAT(conv)));
	}

	a = identity(account, sender);
	b = identity(account, own ? purple_account_get_username(account)
	                          : (pidgin_message_get_sender(msg) ? pidgin_message_get_sender(msg)
	                                                            : purple_conversation_get_name(conv)));
	ret = purple_strequal(a, b);
	g_free(a);
	g_free(b);
	return ret;
}

static PidginIndexedMessage *
index_find(PurpleConversation *conv, PurpleAccount *account, const char *conv_name,
           const char *id)
{
	PidginMessageIndex *idx = pidgin_message_index_get_default();
	PidginIndexedMessage *row;
	char *akey, *ckey;

	if (idx == NULL || id == NULL)
		return NULL;
	akey = pidgin_message_index_account_key(account);
	ckey = pidgin_message_index_conv_key(account,
		conv ? purple_conversation_get_name(conv) : conv_name);
	row = pidgin_message_index_find_by_id(idx, akey, ckey, id);
	g_free(akey);
	g_free(ckey);
	return row;
}

static gboolean
corrected_cb(PurpleAccount *account, const char *conv_name, const char *target_id,
             const char *new_id, const char *new_body, const char *sender, gpointer data)
{
	PurpleConversation *conv = find_conv(account, conv_name);
	PidginMessageView *view = conv_view(conv);
	PidginMessage *msg;
	char *plain, *text;

	if (view == NULL || target_id == NULL || new_body == NULL)
		return FALSE;
	msg = pidgin_message_view_find_by_id(view, target_id);
	if (msg == NULL)
		return FALSE;   /* the prpl writes "edit: …" instead */
	if (!same_sender(conv, msg, sender)) {
		purple_debug_warning("gtkconv", "Ignoring a correction of %s's message by %s\n",
		                     pidgin_message_get_sender(msg), sender);
		return FALSE;
	}

	pidgin_message_apply_correction(msg, new_body, new_id);

	plain = pidgin_markup_html_to_plain(new_body);
	text = pidgin_conv_meta_text_edited(pidgin_message_get_alias(msg), plain);
	event_fallback(conv, text);
	/* The corrected text is searchable; new_id resolves to it. */
	index_insert(conv, NULL, plain, pidgin_message_get_alias(msg),
	             pidgin_message_get_flags(msg), time(NULL), target_id, new_id, FALSE);
	g_free(text);
	g_free(plain);
	return TRUE;
}

static gboolean
reaction_cb(PurpleAccount *account, const char *conv_name, const char *target_id,
            const char *emoji, const char *sender, gpointer add_p, gpointer data)
{
	gboolean add = GPOINTER_TO_INT(add_p) != 0;
	PurpleConversation *conv = find_conv(account, conv_name);
	PidginMessageView *view = conv_view(conv);
	PidginMessageIndex *idx = pidgin_message_index_get_default();
	PidginMessage *msg = NULL;
	PidginIndexedMessage *row;
	char *who, *text;
	gboolean changed;

	if (target_id == NULL || emoji == NULL || sender == NULL)
		return FALSE;

	/* Reaction lists hold identities: bare JIDs in IMs, nicks in rooms. */
	who = (conv != NULL && purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_CHAT)
		? g_strdup(sender) : identity(account, sender);

	if (view != NULL)
		msg = pidgin_message_view_find_by_id(view, target_id);
	if (msg == NULL) {
		/* Not shown: keep the index current; the prpl writes its line. */
		if ((row = index_find(conv, account, conv_name, target_id)) != NULL) {
			if (add)
				pidgin_message_index_add_reaction(idx, row->id, emoji, who);
			else
				pidgin_message_index_remove_reaction(idx, row->id, emoji, who);
			pidgin_indexed_message_free(row);
		}
		g_free(who);
		return FALSE;
	}

	/* The prpl's diffs are per process: an add we have is a no-op. */
	changed = add ? pidgin_message_add_reaction(msg, emoji, who)
	              : pidgin_message_remove_reaction(msg, emoji, who);
	if (changed) {
		if (idx != NULL && pidgin_message_get_index_id(msg) > 0) {
			if (add)
				pidgin_message_index_add_reaction(idx, pidgin_message_get_index_id(msg),
				                                  emoji, who);
			else
				pidgin_message_index_remove_reaction(idx,
					pidgin_message_get_index_id(msg), emoji, who);
		}
		text = pidgin_conv_meta_text_reaction(display_name(conv, sender), emoji, add,
		                                      pidgin_message_get_plain_text(msg));
		event_fallback(conv, text);
		g_free(text);
	}
	g_free(who);
	return TRUE;
}

static gboolean
receipt_cb(PurpleAccount *account, const char *conv_name, const char *id,
           const char *state, const char *sender, gpointer data)
{
	PurpleConversation *conv = find_conv(account, conv_name);
	PidginMessageView *view = conv_view(conv);
	PidginReceiptState rs = pidgin_receipt_state_from_string(state);
	PidginMessage *msg;
	PidginMessageIndex *idx = pidgin_message_index_get_default();
	char *who, *self;
	gboolean own;

	who = identity(account, sender);
	self = identity(account, purple_account_get_username(account));
	own = sender != NULL && purple_strequal(who, self);
	g_free(who);
	g_free(self);

	/* Read on another device of ours (XEP-0333 carbon, XEP-0490). */
	if (own) {
		if (conv != NULL && rs == PIDGIN_RECEIPT_DISPLAYED) {
			purple_conversation_set_data(conv, DISPLAYED_KEY, NULL);
			if (ui_ops.seen_elsewhere != NULL)
				ui_ops.seen_elsewhere(conv);
		}
		return conv != NULL;
	}

	if (view == NULL || id == NULL ||
	    (msg = pidgin_message_view_find_by_id(view, id)) == NULL) {
		PidginIndexedMessage *row = index_find(conv, account, conv_name, id);

		if (row != NULL) {
			pidgin_message_index_set_receipt(idx, row->id, state, sender);
			pidgin_indexed_message_free(row);
		}
		return FALSE;
	}

	pidgin_message_set_receipt(msg, rs);
	if (idx != NULL && pidgin_message_get_index_id(msg) > 0)
		pidgin_message_index_set_receipt(idx, pidgin_message_get_index_id(msg), state, sender);

	/* "displayed" covers everything we sent before it. */
	if (rs == PIDGIN_RECEIPT_DISPLAYED) {
		GListModel *model = pidgin_message_view_get_model(view);
		guint n = g_list_model_get_n_items(model), i;
		gboolean found = FALSE;

		for (i = n; i > 0; i--) {
			PidginMessage *m = g_list_model_get_item(model, i - 1);
			gboolean stop = FALSE;

			if (m == msg)
				found = TRUE;
			else if (found && (pidgin_message_get_flags(m) & PURPLE_MESSAGE_SEND)) {
				if (pidgin_message_get_receipt(m) == PIDGIN_RECEIPT_DISPLAYED)
					stop = TRUE;
				else
					pidgin_message_set_receipt(m, PIDGIN_RECEIPT_DISPLAYED);
			}
			g_object_unref(m);
			if (stop)
				break;
		}
	}
	return TRUE;
}

static gboolean
retracted_cb(PurpleAccount *account, const char *conv_name, const char *target_id,
             const char *sender, const char *reason, gpointer data)
{
	PurpleConversation *conv = find_conv(account, conv_name);
	PidginMessageView *view = conv_view(conv);
	PidginMessage *msg;
	gboolean moderated = FALSE;
	char *text;

	if (view == NULL || target_id == NULL ||
	    (msg = pidgin_message_view_find_by_id(view, target_id)) == NULL)
		return FALSE;

	if (!same_sender(conv, msg, sender)) {
		/* Moderation comes only from the room (the prpl checked). */
		if (purple_conversation_get_type(conv) != PURPLE_CONV_TYPE_CHAT) {
			purple_debug_warning("gtkconv", "Ignoring a retraction of %s's message "
			                     "by %s\n", pidgin_message_get_sender(msg), sender);
			return FALSE;
		}
		moderated = TRUE;
	}

	if (pidgin_message_get_retracted(msg))
		return TRUE;
	pidgin_message_set_retracted(msg, TRUE);
	text = pidgin_conv_meta_text_retracted(moderated ? (sender ? sender : _("A moderator"))
	                                                 : pidgin_message_get_alias(msg),
	                                       moderated, reason);
	event_fallback(conv, text);
	g_free(text);
	return TRUE;
}

/**************************************************************************
 * Actions
 **************************************************************************/

static GHashTable *ipc_cache;   /* "<plugin ptr>:command" -> 1 or 2 */

/* purple_plugin_ipc_get_params() logs an error for each miss (IRC has no
 * M8 commands), so ask once per plugin and command. */
static gboolean
prpl_has_command(PurplePlugin *prpl, const char *command)
{
	char *key = g_strdup_printf("%p:%s", (void *)prpl, command);
	int v;

	if (ipc_cache == NULL)
		ipc_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	v = GPOINTER_TO_INT(g_hash_table_lookup(ipc_cache, key));
	if (v == 0) {
		v = purple_plugin_ipc_get_params(prpl, command, NULL, NULL, NULL) ? 1 : 2;
		g_hash_table_insert(ipc_cache, key, GINT_TO_POINTER(v));
	} else {
		g_free(key);
	}
	return v == 1;
}

gboolean
pidgin_conv_meta_has_command(PurpleConversation *conv, const char *command)
{
	PurplePlugin *prpl;
	PurpleAccount *account;

	if (conv == NULL || !initialized)
		return FALSE;
	account = purple_conversation_get_account(conv);
	if (account == NULL || !purple_account_is_connected(account))
		return FALSE;
	prpl = conv_prpl(conv);
	return prpl != NULL && prpl_has_command(prpl, command);
}

const char *
pidgin_conv_meta_target_id(PurpleConversation *conv, PidginMessage *msg)
{
	const char *id;

	if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_CHAT &&
	    (id = pidgin_message_get_server_id(msg)) != NULL)
		return id;
	if ((id = pidgin_message_get_stanza_id(msg)) != NULL)
		return id;
	if ((id = pidgin_message_get_origin_id(msg)) != NULL)
		return id;
	return pidgin_message_get_server_id(msg);
}

char *
pidgin_conv_meta_self_id(PurpleConversation *conv)
{
	PurpleAccount *account = purple_conversation_get_account(conv);

	if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_CHAT)
		return g_strdup(purple_conv_chat_get_nick(PURPLE_CONV_CHAT(conv)));
	return identity(account, purple_account_get_username(account));
}

static gboolean
ipc_bool(PurpleConversation *conv, const char *command, gboolean result, gboolean ok)
{
	if (!ok || !result)
		purple_debug_warning("gtkconv", "%s in %s failed\n", command,
		                     purple_conversation_get_name(conv));
	return ok && result;
}

gboolean
pidgin_conv_meta_send_reaction(PurpleConversation *conv, PidginMessage *msg,
                               const char *emoji, gboolean add, const char *self_id)
{
	GString *set = g_string_new(NULL);
	GList *emojis, *l;
	const char *target = pidgin_conv_meta_target_id(conv, msg);
	gboolean ok = FALSE, res;

	if (target == NULL || !pidgin_conv_meta_has_command(conv, "send-reaction")) {
		g_string_free(set, TRUE);
		return FALSE;
	}
	/* The IPC takes our complete new set. */
	emojis = pidgin_message_get_reaction_emojis(msg);
	for (l = emojis; l != NULL; l = l->next) {
		const char *e = l->data;

		if (purple_strequal(e, emoji) || !pidgin_message_has_reaction(msg, e, self_id))
			continue;
		if (set->len)
			g_string_append_c(set, ' ');
		g_string_append(set, e);
	}
	g_list_free(emojis);
	if (add) {
		if (set->len)
			g_string_append_c(set, ' ');
		g_string_append(set, emoji);
	}

	res = GPOINTER_TO_INT(purple_plugin_ipc_call(conv_prpl(conv), "send-reaction", &ok,
		purple_conversation_get_account(conv), purple_conversation_get_name(conv),
		target, set->str));
	g_string_free(set, TRUE);
	return ipc_bool(conv, "send-reaction", res, ok);
}

gboolean
pidgin_conv_meta_send_reply(PurpleConversation *conv, PidginMessage *msg, const char *body)
{
	const char *target = pidgin_conv_meta_target_id(conv, msg);
	char *jid = NULL;
	gboolean ok = FALSE, res;

	if (target == NULL || !pidgin_conv_meta_has_command(conv, "send-reply"))
		return FALSE;
	if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_CHAT) {
		if (pidgin_message_get_sender(msg) != NULL)
			jid = g_strdup_printf("%s/%s", purple_conversation_get_name(conv),
			                      pidgin_message_get_sender(msg));
	} else if (pidgin_message_get_flags(msg) & PURPLE_MESSAGE_SEND) {
		jid = pidgin_conv_meta_bare_jid(purple_account_get_username(
			purple_conversation_get_account(conv)));
	} else {
		jid = pidgin_conv_meta_bare_jid(purple_conversation_get_name(conv));
	}
	res = GPOINTER_TO_INT(purple_plugin_ipc_call(conv_prpl(conv), "send-reply", &ok,
		purple_conversation_get_account(conv), purple_conversation_get_name(conv),
		target, jid, pidgin_message_get_plain_text(msg), body));
	g_free(jid);
	return ipc_bool(conv, "send-reply", res, ok);
}

gboolean
pidgin_conv_meta_send_correction(PurpleConversation *conv, PidginMessage *msg,
                                 const char *body)
{
	const char *target;
	gboolean ok = FALSE, res;

	/* Our own message: its id is the one we sent (stanza-id = origin-id). */
	target = pidgin_message_get_stanza_id(msg) ? pidgin_message_get_stanza_id(msg)
	                                           : pidgin_message_get_origin_id(msg);
	if (target == NULL || !pidgin_conv_meta_has_command(conv, "send-correction"))
		return FALSE;
	res = GPOINTER_TO_INT(purple_plugin_ipc_call(conv_prpl(conv), "send-correction", &ok,
		purple_conversation_get_account(conv), purple_conversation_get_name(conv),
		target, body));
	return ipc_bool(conv, "send-correction", res, ok);
}

gboolean
pidgin_conv_meta_send_retraction(PurpleConversation *conv, PidginMessage *msg)
{
	gboolean ok = FALSE, res;
	const char *target;

	if (pidgin_message_get_flags(msg) & PURPLE_MESSAGE_SEND) {
		target = pidgin_message_get_stanza_id(msg) ? pidgin_message_get_stanza_id(msg)
		                                           : pidgin_message_get_origin_id(msg);
		if (target == NULL || !pidgin_conv_meta_has_command(conv, "send-retraction"))
			return FALSE;
		res = GPOINTER_TO_INT(purple_plugin_ipc_call(conv_prpl(conv), "send-retraction", &ok,
			purple_conversation_get_account(conv), purple_conversation_get_name(conv),
			target));
		return ipc_bool(conv, "send-retraction", res, ok);
	}

	target = pidgin_message_get_server_id(msg);
	if (purple_conversation_get_type(conv) != PURPLE_CONV_TYPE_CHAT || target == NULL ||
	    !pidgin_conv_meta_has_command(conv, "send-moderation"))
		return FALSE;
	res = GPOINTER_TO_INT(purple_plugin_ipc_call(conv_prpl(conv), "send-moderation", &ok,
		purple_conversation_get_account(conv), purple_conversation_get_name(conv),
		target, NULL));
	return ipc_bool(conv, "send-moderation", res, ok);
}

void
pidgin_conv_meta_mark_displayed(PurpleConversation *conv)
{
	PidginMessageView *view = conv_view(conv);
	GListModel *model;
	guint n, i;
	PidginMessage *newest = NULL;
	gboolean is_chat;
	const char *marker_id, *server_id;
	PurpleAccount *account;

	if (view == NULL || !purple_prefs_get_bool(META_PREFS "/send_markers") ||
	    !pidgin_conv_meta_has_command(conv, "send-marker"))
		return;

	model = pidgin_message_view_get_model(view);
	n = g_list_model_get_n_items(model);
	for (i = n; i > 0 && newest == NULL; i--) {
		PidginMessage *m = g_list_model_get_item(model, i - 1);

		if ((pidgin_message_get_flags(m) & PURPLE_MESSAGE_RECV) &&
		    !(pidgin_message_get_flags(m) & PURPLE_MESSAGE_SEND))
			newest = m;
		else
			g_object_unref(m);
	}
	if (newest == NULL)
		return;

	account = purple_conversation_get_account(conv);
	is_chat = purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_CHAT;
	server_id = pidgin_message_get_server_id(newest);
	marker_id = is_chat ? server_id : pidgin_message_get_stanza_id(newest);

	if (marker_id != NULL && g_object_get_data(G_OBJECT(newest), MARKABLE_KEY) &&
	    !purple_strequal(purple_conversation_get_data(conv, DISPLAYED_KEY), marker_id)) {
		gboolean ok = FALSE;

		purple_plugin_ipc_call(conv_prpl(conv), "send-marker", &ok, account,
		                       purple_conversation_get_name(conv), marker_id, "displayed");
		g_free(purple_conversation_get_data(conv, DISPLAYED_KEY));
		purple_conversation_set_data(conv, DISPLAYED_KEY, g_strdup(marker_id));
	}
	if (server_id != NULL && pidgin_conv_meta_has_command(conv, "mds-publish") &&
	    !purple_strequal(purple_conversation_get_data(conv, PUBLISHED_KEY), server_id)) {
		gboolean ok = FALSE;

		purple_plugin_ipc_call(conv_prpl(conv), "mds-publish", &ok, account,
		                       purple_conversation_get_name(conv), server_id);
		g_free(purple_conversation_get_data(conv, PUBLISHED_KEY));
		purple_conversation_set_data(conv, PUBLISHED_KEY, g_strdup(server_id));
	}
	g_object_unref(newest);
}

static void
deleting_conv_cb(PurpleConversation *conv, gpointer data)
{
	OlderBatch *b = older_batch_get(conv, FALSE);

	if (b != NULL)
		g_hash_table_remove(older_batches, b->conv_key);
	g_free(purple_conversation_get_data(conv, DISPLAYED_KEY));
	purple_conversation_set_data(conv, DISPLAYED_KEY, NULL);
	g_free(purple_conversation_get_data(conv, PUBLISHED_KEY));
	purple_conversation_set_data(conv, PUBLISHED_KEY, NULL);
	if (last_write.conv == conv)
		last_write.conv = NULL;
}

/**************************************************************************
 * Setup
 **************************************************************************/

void *
pidgin_conv_meta_get_handle(void)
{
	return &handle;
}

void
pidgin_conv_meta_init(const PidginConvMetaUiOps *ops)
{
	void *conv_handle = purple_conversations_get_handle();

	if (initialized)
		return;
	initialized = TRUE;
	if (ops != NULL && ops != &ui_ops)
		ui_ops = *ops;

	purple_prefs_add_none(META_PREFS);
	purple_prefs_add_bool(META_PREFS "/send_markers", TRUE);

	pending_recv = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                     (GDestroyNotify)pending_free);
	pending_send = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                     (GDestroyNotify)pending_free);
	older_batches = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                      (GDestroyNotify)older_batch_free);

	/* pidgin4 always runs on the prefix's libpurple, which registers
	 * the M8 signals (conversation.c). */
	purple_signal_connect(conv_handle, "receiving-message-meta", &handle,
	                      PURPLE_CALLBACK(receiving_meta_cb), NULL);
	purple_signal_connect(conv_handle, "sending-message-meta", &handle,
	                      PURPLE_CALLBACK(sending_meta_cb), NULL);
	purple_signal_connect(conv_handle, "message-corrected", &handle,
	                      PURPLE_CALLBACK(corrected_cb), NULL);
	purple_signal_connect(conv_handle, "message-reaction", &handle,
	                      PURPLE_CALLBACK(reaction_cb), NULL);
	purple_signal_connect(conv_handle, "message-receipt", &handle,
	                      PURPLE_CALLBACK(receipt_cb), NULL);
	purple_signal_connect(conv_handle, "message-retracted", &handle,
	                      PURPLE_CALLBACK(retracted_cb), NULL);
	purple_signal_connect_priority(conv_handle, "writing-im-msg", &handle,
		PURPLE_CALLBACK(writing_msg_cb), NULL, PURPLE_SIGNAL_PRIORITY_HIGHEST);
	purple_signal_connect_priority(conv_handle, "writing-chat-msg", &handle,
		PURPLE_CALLBACK(writing_msg_cb), NULL, PURPLE_SIGNAL_PRIORITY_HIGHEST);
	purple_signal_connect(conv_handle, "deleting-conversation", &handle,
	                      PURPLE_CALLBACK(deleting_conv_cb), NULL);
	purple_signal_connect(purple_plugins_get_handle(), "plugin-load", &handle,
	                      PURPLE_CALLBACK(plugin_load_cb), NULL);
	purple_signal_connect(purple_plugins_get_handle(), "plugin-unload", &handle,
	                      PURPLE_CALLBACK(plugin_unload_cb), NULL);
	mam_plugin = NULL;
	connect_mam_signal();
}

void
pidgin_conv_meta_uninit(void)
{
	if (!initialized)
		return;
	initialized = FALSE;
	purple_signals_disconnect_by_handle(&handle);
	mam_plugin = NULL;
	if (pending_idle) {
		g_source_remove(pending_idle);
		pending_idle = 0;
	}
	g_clear_pointer(&pending_recv, g_hash_table_destroy);
	g_clear_pointer(&pending_send, g_hash_table_destroy);
	g_clear_pointer(&older_batches, g_hash_table_destroy);
	g_clear_pointer(&ipc_cache, g_hash_table_destroy);
	g_clear_pointer(&last_write.path, g_free);
	last_write.conv = NULL;
	g_clear_pointer(&writing_share_url, g_free);
	g_clear_pointer(&seen_encryption, g_hash_table_destroy);
}
