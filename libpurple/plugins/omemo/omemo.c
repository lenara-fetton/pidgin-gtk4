/*
 * OMEMO for libpurple (legacy OMEMO 0.3, eu.siacs.conversations.axolotl)
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
/*
 * Plugin glue. The plugin never links against the jabber prpl; it only uses
 * what the prpl exposes to every plugin:
 *
 *  - "jabber-receiving-xmlnode": decrypt incoming messages in place (also
 *    inside carbons and MAM results), track MUC occupants, watch device
 *    list PEP events;
 *  - "jabber-sending-xmlnode": encrypt outgoing <message/>s, and send our
 *    own IQs by emitting it (that is all jabber_send() does);
 *  - "jabber-receiving-iq": consume the results of our own IQs;
 *  - the "add_feature" IPC command for the devicelist+notify feature.
 *
 * The libpurple core "sending-im-msg"/"sending-chat-msg" signals hold a
 * message back while device lists and bundles are fetched, and send it
 * once the sessions exist.
 *
 * UI interface: see the IPC commands and signals registered in
 * omemo_register_ipc() below, and doc/PIDGIN-UPGRADE.md (M8).
 */
#include "internal.h"

#include <string.h>

#include "account.h"
#include "blist.h"
#include "cmds.h"
#include "connection.h"
#include "conversation.h"
#include "debug.h"
#include "notify.h"
#include "plugin.h"
#include "pluginpref.h"
#include "prefs.h"
#include "request.h"
#include "server.h"
#include "signals.h"
#include "util.h"
#include "value.h"
#include "version.h"

#include "omemo.h"

#define OMEMO_PLUGIN_ID       "core-omemo"
#define PREF_ROOT             "/plugins/core/omemo"
#define PREF_TOFU             PREF_ROOT "/tofu"
#define PREF_DEFAULT_ON       PREF_ROOT "/encrypt_by_default"
#define BLIST_SETTING         "omemo-enabled"

#define NS_PUBSUB             "http://jabber.org/protocol/pubsub"
#define NS_PUBSUB_EVENT       "http://jabber.org/protocol/pubsub#event"
#define NS_DISCO_INFO         "http://jabber.org/protocol/disco#info"
#define NS_MUC_USER           "http://jabber.org/protocol/muc#user"
#define NS_CARBONS            "urn:xmpp:carbons:2"
#define NS_FORWARD            "urn:xmpp:forward:0"
#define NS_SID                "urn:xmpp:sid:0"

#define SENT_CACHE_SIZE       200

typedef void (*OmemoIqFunc)(PurpleConnection *gc, const char *type,
                            xmlnode *iq, gpointer data);
typedef void (*OmemoDoneFunc)(gpointer data);

typedef struct {
	PurpleConnection *gc;
	OmemoIqFunc cb;
	gpointer data;
} OmemoIq;

typedef struct {
	gboolean disco_requested;
	gboolean features_known;
	gboolean membersonly;
	gboolean nonanonymous;
	char *self_nick;
	GHashTable *occupants;   /* nick -> real bare JID */
} OmemoMuc;

/* A conversation waiting for device lists / bundles before it can send. */
typedef struct {
	PurpleAccount *account;
	char *name;
	char *key;
	gboolean is_chat;
	GList *messages;         /* char *, in order */
	int pending;
	gboolean cancelled;      /* account went offline: just wait and free */
} OmemoPrep;

typedef struct {
	OmemoPrep *prep;         /* may be NULL for background fetches */
	char *jid;
	guint32 device_id;
	OmemoDoneFunc done;
	gpointer done_data;
} OmemoFetch;

typedef struct {
	char *node;
	xmlnode *payload;
	gboolean with_options;
} OmemoPublish;

static PurplePlugin *omemo_plugin = NULL;
static PurplePlugin *jabber_prpl = NULL;
static OmemoStore *omemo_store = NULL;
static GHashTable *omemo_accounts = NULL;   /* PurpleAccount* -> OmemoAccount* */
static GHashTable *omemo_iqs = NULL;        /* id -> OmemoIq* */
static GHashTable *omemo_overrides = NULL;  /* conv key -> 1 / -1 */
static GHashTable *omemo_mucs = NULL;       /* conv key -> OmemoMuc* */
static GHashTable *omemo_preps = NULL;      /* conv key -> OmemoPrep* */
static GHashTable *omemo_failed = NULL;     /* device key -> 1 */
static GHashTable *omemo_sent = NULL;       /* msg key -> plaintext */
static GQueue *omemo_sent_order = NULL;
static GHashTable *omemo_replenish = NULL;  /* PurpleConnection* -> timeout */
static PurpleCmdId omemo_cmd = 0;
static guint omemo_iq_counter = 0;

static void omemo_publish_bundle(PurpleConnection *gc);
static void omemo_fetch_devicelist(PurpleConnection *gc, const char *jid,
                                   OmemoDoneFunc done, gpointer data);
static void omemo_emit_state(PurpleAccount *account, const char *name,
                             gboolean is_chat);

/**************************************************************************
 * Small helpers
 **************************************************************************/

static char *
omemo_bare_jid(const char *jid)
{
	const char *slash;
	char *bare, *ret;

	if (jid == NULL)
		return NULL;
	slash = strchr(jid, '/');
	bare = slash ? g_strndup(jid, slash - jid) : g_strdup(jid);
	ret = g_utf8_strdown(bare, -1);
	g_free(bare);
	return ret;
}

static const char *
omemo_resource(const char *jid)
{
	const char *slash = jid ? strchr(jid, '/') : NULL;

	return slash ? slash + 1 : NULL;
}

static char *
omemo_conv_key(PurpleAccount *account, const char *name, gboolean is_chat)
{
	char *bare = omemo_bare_jid(name);
	char *key = g_strdup_printf("%p\n%c\n%s", (void *)account,
	                            is_chat ? 'c' : 'i', bare);

	g_free(bare);
	return key;
}

static char *
omemo_device_key(OmemoAccount *oa, const char *jid, guint32 device_id)
{
	return g_strdup_printf("%p\n%s\n%u", (void *)oa, jid, device_id);
}

static gboolean
omemo_is_xmpp(PurpleAccount *account)
{
	return account != NULL &&
	       purple_strequal(purple_account_get_protocol_id(account),
	                       "prpl-jabber");
}

static void
omemo_new_identity_cb(OmemoAccount *oa, const char *jid, guint32 device_id,
                      const char *fingerprint, gpointer data)
{
	purple_debug_info("omemo", "new identity %s:%u %s\n", jid, device_id,
	                  fingerprint);
	if (oa->account == NULL)
		return;
	purple_signal_emit(omemo_plugin, "omemo-new-device", oa->account, jid,
	                   device_id, fingerprint);
	if (!purple_strequal(jid, oa->jid))
		omemo_emit_state(oa->account, jid, FALSE);
}

static OmemoAccount *
omemo_account_get(PurpleAccount *account)
{
	OmemoAccount *oa;
	GError *error = NULL;
	char *jid;

	if (!omemo_is_xmpp(account) || omemo_store == NULL)
		return NULL;

	oa = g_hash_table_lookup(omemo_accounts, account);
	if (oa != NULL) {
		oa->tofu = purple_prefs_get_bool(PREF_TOFU);
		return oa;
	}

	jid = omemo_bare_jid(purple_account_get_username(account));
	oa = omemo_account_new(omemo_store, jid, account, &error);
	g_free(jid);
	if (oa == NULL) {
		purple_debug_error("omemo", "cannot set up %s: %s\n",
		                   purple_account_get_username(account),
		                   error ? error->message : "?");
		g_clear_error(&error);
		return NULL;
	}
	oa->tofu = purple_prefs_get_bool(PREF_TOFU);
	oa->new_identity_cb = omemo_new_identity_cb;
	g_hash_table_insert(omemo_accounts, account, oa);
	return oa;
}

static PurpleConversation *
omemo_find_conv(PurpleAccount *account, const char *name, gboolean is_chat)
{
	PurpleConversation *conv;
	char *bare;

	conv = purple_find_conversation_with_account(
		is_chat ? PURPLE_CONV_TYPE_CHAT : PURPLE_CONV_TYPE_IM, name, account);
	if (conv != NULL || is_chat)
		return conv;
	bare = omemo_bare_jid(name);
	conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, bare,
	                                             account);
	g_free(bare);
	return conv;
}

static void
omemo_conv_notice(PurpleAccount *account, const char *name, gboolean is_chat,
                  const char *text, gboolean error)
{
	PurpleConversation *conv = omemo_find_conv(account, name, is_chat);

	if (conv == NULL) {
		purple_debug_info("omemo", "%s: %s\n", name, text);
		return;
	}
	purple_conversation_write(conv, NULL, text,
	                          PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG |
	                          (error ? PURPLE_MESSAGE_ERROR : 0),
	                          time(NULL));
}

/**************************************************************************
 * Sending our own IQs
 **************************************************************************/

static void
omemo_send_stanza(PurpleConnection *gc, xmlnode *stanza)
{
	xmlnode *tmp = stanza;

	/* jabber_send() is exactly this emission. */
	purple_signal_emit(jabber_prpl, "jabber-sending-xmlnode", gc, &tmp);
	xmlnode_free(stanza);
}

static void
omemo_send_iq(PurpleConnection *gc, const char *type, const char *to,
              xmlnode *child, OmemoIqFunc cb, gpointer data)
{
	xmlnode *iq = xmlnode_new("iq");
	OmemoIq *pending;
	guint32 rnd;
	char *id;

	omemo_random_bytes((guint8 *)&rnd, sizeof(rnd));
	id = g_strdup_printf("omemo%u-%08x", ++omemo_iq_counter, rnd);

	xmlnode_set_attrib(iq, "type", type);
	xmlnode_set_attrib(iq, "id", id);
	if (to)
		xmlnode_set_attrib(iq, "to", to);
	xmlnode_insert_child(iq, child);

	pending = g_new0(OmemoIq, 1);
	pending->gc = gc;
	pending->cb = cb;
	pending->data = data;
	g_hash_table_insert(omemo_iqs, id, pending);

	omemo_send_stanza(gc, iq);
}

static gboolean
omemo_receiving_iq_cb(PurpleConnection *gc, const char *type, const char *id,
                      const char *from, xmlnode *iq, gpointer unused)
{
	OmemoIq *pending;

	if (id == NULL || !g_str_has_prefix(id, "omemo") ||
	    !(purple_strequal(type, "result") || purple_strequal(type, "error")))
		return FALSE;

	pending = g_hash_table_lookup(omemo_iqs, id);
	if (pending == NULL || pending->gc != gc)
		return FALSE;

	{
		gpointer stolen_key = NULL;

		g_hash_table_steal_extended(omemo_iqs, id, &stolen_key, NULL);
		g_free(stolen_key);
	}
	if (pending->cb)
		pending->cb(gc, type, iq, pending->data);
	g_free(pending);
	return TRUE;
}

/* Cancel our IQs on a connection that goes away. */
static void
omemo_cancel_iqs(PurpleConnection *gc)
{
	GHashTableIter iter;
	gpointer key, value;
	GList *cancelled = NULL, *l;

	g_hash_table_iter_init(&iter, omemo_iqs);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		OmemoIq *pending = value;

		if (pending->gc == gc) {
			cancelled = g_list_prepend(cancelled, pending);
			g_hash_table_iter_steal(&iter);
			g_free(key);
		}
	}
	for (l = cancelled; l; l = l->next) {
		OmemoIq *pending = l->data;

		if (pending->cb)
			pending->cb(gc, "cancel", NULL, pending->data);
		g_free(pending);
	}
	g_list_free(cancelled);
}

/**************************************************************************
 * PEP
 **************************************************************************/

static void
omemo_pep_request(PurpleConnection *gc, const char *to, const char *node,
                  OmemoIqFunc cb, gpointer data)
{
	xmlnode *pubsub = xmlnode_new("pubsub");
	xmlnode *items;

	xmlnode_set_namespace(pubsub, NS_PUBSUB);
	items = xmlnode_new_child(pubsub, "items");
	xmlnode_set_attrib(items, "node", node);
	xmlnode_set_attrib(items, "max_items", "1");
	omemo_send_iq(gc, "get", to, pubsub, cb, data);
}

/* The payload of the first item of 'node' in a pubsub items result. */
static xmlnode *
omemo_pep_payload(xmlnode *iq, const char *node)
{
	xmlnode *pubsub, *items, *item, *child;

	if (iq == NULL)
		return NULL;
	pubsub = xmlnode_get_child_with_namespace(iq, "pubsub", NS_PUBSUB);
	items = pubsub ? xmlnode_get_child(pubsub, "items") : NULL;
	if (items == NULL ||
	    !purple_strequal(xmlnode_get_attrib(items, "node"), node))
		return NULL;
	item = xmlnode_get_child(items, "item");
	if (item == NULL)
		return NULL;
	for (child = item->child; child; child = child->next)
		if (child->type == XMLNODE_TYPE_TAG)
			return child;
	return NULL;
}

/*
 * A device list answer we can rely on: a result (possibly without items),
 * or an item-not-found error (the node does not exist: no devices).
 */
static gboolean
omemo_devicelist_answer_ok(const char *type, xmlnode *iq)
{
	xmlnode *error;

	if (purple_strequal(type, "result"))
		return TRUE;
	if (iq == NULL || !purple_strequal(type, "error"))
		return FALSE;
	error = xmlnode_get_child(iq, "error");
	return error != NULL &&
	       xmlnode_get_child_with_namespace(error, "item-not-found",
	                   "urn:ietf:params:xml:ns:xmpp-stanzas") != NULL;
}

static void omemo_pep_publish_send(PurpleConnection *gc, OmemoPublish *pub);

static void
omemo_pep_publish_cb(PurpleConnection *gc, const char *type, xmlnode *iq,
                     gpointer data)
{
	OmemoPublish *pub = data;

	if (purple_strequal(type, "error") && pub->with_options) {
		/* Server without publish-options (or a node whose config
		 * differs): retry with the node's defaults. */
		purple_debug_info("omemo", "publish of %s with options failed, "
		                  "retrying without\n", pub->node);
		pub->with_options = FALSE;
		omemo_pep_publish_send(gc, pub);
		return;
	}
	if (purple_strequal(type, "error"))
		purple_debug_error("omemo", "cannot publish %s\n", pub->node);
	else if (purple_strequal(type, "result"))
		purple_debug_info("omemo", "published %s\n", pub->node);

	g_free(pub->node);
	xmlnode_free(pub->payload);
	g_free(pub);
}

static void
omemo_pep_publish_send(PurpleConnection *gc, OmemoPublish *pub)
{
	xmlnode *pubsub = xmlnode_new("pubsub");
	xmlnode *publish, *item;

	xmlnode_set_namespace(pubsub, NS_PUBSUB);
	publish = xmlnode_new_child(pubsub, "publish");
	xmlnode_set_attrib(publish, "node", pub->node);
	item = xmlnode_new_child(publish, "item");
	xmlnode_set_attrib(item, "id", "current");
	xmlnode_insert_child(item, xmlnode_copy(pub->payload));

	if (pub->with_options) {
		xmlnode *opts = xmlnode_new_child(pubsub, "publish-options");
		xmlnode *x = xmlnode_new_child(opts, "x");
		xmlnode *field, *value;

		xmlnode_set_namespace(x, "jabber:x:data");
		xmlnode_set_attrib(x, "type", "submit");
		field = xmlnode_new_child(x, "field");
		xmlnode_set_attrib(field, "var", "FORM_TYPE");
		xmlnode_set_attrib(field, "type", "hidden");
		value = xmlnode_new_child(field, "value");
		xmlnode_insert_data(value,
			"http://jabber.org/protocol/pubsub#publish-options", -1);
		field = xmlnode_new_child(x, "field");
		xmlnode_set_attrib(field, "var", "pubsub#access_model");
		value = xmlnode_new_child(field, "value");
		xmlnode_insert_data(value, "open", -1);
	}

	omemo_send_iq(gc, "set", NULL, pubsub, omemo_pep_publish_cb, pub);
}

static void
omemo_pep_publish(PurpleConnection *gc, const char *node, xmlnode *payload)
{
	OmemoPublish *pub = g_new0(OmemoPublish, 1);

	pub->node = g_strdup(node);
	pub->payload = payload;
	pub->with_options = TRUE;
	omemo_pep_publish_send(gc, pub);
}

/**************************************************************************
 * Our own device list and bundle
 **************************************************************************/

static void
omemo_publish_bundle(PurpleConnection *gc)
{
	OmemoAccount *oa = omemo_account_get(purple_connection_get_account(gc));
	xmlnode *bundle;
	char *node;

	if (oa == NULL)
		return;
	omemo_account_replenish_prekeys(oa, OMEMO_PREKEY_MINIMUM,
	                                OMEMO_PREKEY_TARGET);
	oa->prekeys_removed = 0;

	bundle = omemo_xml_build_bundle(oa);
	if (bundle == NULL) {
		purple_debug_error("omemo", "cannot build bundle for %s\n", oa->jid);
		return;
	}
	node = g_strdup_printf("%s%u", OMEMO_NS_BUNDLES, oa->device_id);
	omemo_pep_publish(gc, node, bundle);
	g_free(node);
}

/* Handle a device list of ours: make sure our device id stays in it. */
static void
omemo_own_devicelist(PurpleConnection *gc, OmemoAccount *oa, GArray *ids)
{
	guint i;

	for (i = 0; i < ids->len; i++)
		if (g_array_index(ids, guint32, i) == oa->device_id)
			return;

	purple_debug_info("omemo", "adding device %u to the device list of %s\n",
	                  oa->device_id, oa->jid);
	g_array_append_val(ids, oa->device_id);
	g_list_free(omemo_store_update_device_list(oa, oa->jid, (guint32 *)ids->data,
	                               ids->len));
	omemo_pep_publish(gc, OMEMO_NS_DEVICELIST,
	                  omemo_xml_build_devicelist(ids));
}

static void
omemo_own_devicelist_cb(PurpleConnection *gc, const char *type, xmlnode *iq,
                        gpointer data)
{
	OmemoAccount *oa;
	GArray *ids;
	GList *added;

	if (purple_strequal(type, "cancel"))
		return;
	oa = omemo_account_get(purple_connection_get_account(gc));
	if (oa == NULL)
		return;

	if (!omemo_devicelist_answer_ok(type, iq)) {
		/* Publishing now could drop the ids of our other devices. The
		 * +notify event will bring the list when the server has one. */
		purple_debug_warning("omemo", "cannot fetch the device list of %s; "
		                     "not publishing\n", oa->jid);
		omemo_publish_bundle(gc);
		return;
	}

	/* item-not-found: we are the first OMEMO device of this account. */
	ids = omemo_xml_parse_devicelist(omemo_pep_payload(iq, OMEMO_NS_DEVICELIST));
	added = omemo_store_update_device_list(oa, oa->jid, (guint32 *)ids->data,
	                                       ids->len);
	g_list_free(added);
	omemo_own_devicelist(gc, oa, ids);
	g_array_free(ids, TRUE);

	omemo_publish_bundle(gc);
}

static void
omemo_account_setup(PurpleConnection *gc)
{
	PurpleAccount *account = purple_connection_get_account(gc);
	OmemoAccount *oa = omemo_account_get(account);

	if (oa == NULL)
		return;
	purple_debug_info("omemo", "%s: device id %u\n", oa->jid, oa->device_id);
	omemo_pep_request(gc, NULL, OMEMO_NS_DEVICELIST, omemo_own_devicelist_cb,
	                  NULL);
}

static gboolean
omemo_replenish_timeout(gpointer data)
{
	PurpleConnection *gc = data;

	g_hash_table_remove(omemo_replenish, gc);
	if (g_list_find(purple_connections_get_all(), gc))
		omemo_publish_bundle(gc);
	return FALSE;
}

/* After a pre-key was consumed: republish the bundle, batched. */
static void
omemo_schedule_replenish(PurpleConnection *gc, OmemoAccount *oa)
{
	if (oa->prekeys_removed == 0 ||
	    g_hash_table_lookup(omemo_replenish, gc))
		return;
	g_hash_table_insert(omemo_replenish, gc, GUINT_TO_POINTER(
		purple_timeout_add_seconds(3, omemo_replenish_timeout, gc)));
}

/**************************************************************************
 * Contacts: device lists and bundles
 **************************************************************************/

static void
omemo_fetch_free(OmemoFetch *f)
{
	if (f->done)
		f->done(f->done_data);
	g_free(f->jid);
	g_free(f);
}

static void
omemo_contact_devicelist(PurpleConnection *gc, OmemoAccount *oa,
                         const char *jid, GArray *ids)
{
	GList *added, *l;

	if (purple_strequal(jid, oa->jid)) {
		g_list_free(omemo_store_update_device_list(oa, jid, (guint32 *)ids->data,
		                               ids->len));
		omemo_own_devicelist(gc, oa, ids);
		return;
	}

	added = omemo_store_update_device_list(oa, jid, (guint32 *)ids->data,
	                                       ids->len);
	for (l = added; l; l = l->next)
		purple_debug_info("omemo", "%s announced device %u\n", jid,
		                  GPOINTER_TO_UINT(l->data));
	g_list_free(added);
	omemo_emit_state(oa->account, jid, FALSE);
}

static void
omemo_devicelist_cb(PurpleConnection *gc, const char *type, xmlnode *iq,
                    gpointer data)
{
	OmemoFetch *f = data;
	OmemoAccount *oa;

	if (!purple_strequal(type, "cancel") &&
	    (oa = omemo_account_get(purple_connection_get_account(gc)))) {
		GArray *ids = omemo_xml_parse_devicelist(
			omemo_pep_payload(iq, OMEMO_NS_DEVICELIST));

		/* item-not-found means "no OMEMO": remember the empty list;
		 * other errors (timeouts, forbidden) leave the list unknown. */
		if (omemo_devicelist_answer_ok(type, iq))
			omemo_contact_devicelist(gc, oa, f->jid, ids);
		g_array_free(ids, TRUE);
	}
	omemo_fetch_free(f);
}

static void
omemo_fetch_devicelist(PurpleConnection *gc, const char *jid,
                       OmemoDoneFunc done, gpointer data)
{
	OmemoFetch *f = g_new0(OmemoFetch, 1);

	f->jid = g_strdup(jid);
	f->done = done;
	f->done_data = data;
	omemo_pep_request(gc, jid, OMEMO_NS_DEVICELIST, omemo_devicelist_cb, f);
}

static void
omemo_bundle_cb(PurpleConnection *gc, const char *type, xmlnode *iq,
                gpointer data)
{
	OmemoFetch *f = data;
	OmemoAccount *oa;
	GError *error = NULL;
	char *node, *key;

	if (!purple_strequal(type, "cancel") &&
	    (oa = omemo_account_get(purple_connection_get_account(gc)))) {
		xmlnode *bundle;

		node = g_strdup_printf("%s%u", OMEMO_NS_BUNDLES, f->device_id);
		bundle = omemo_pep_payload(iq, node);
		g_free(node);

		if (bundle == NULL ||
		    !omemo_session_from_bundle(oa, f->jid, f->device_id, bundle,
		                               &error)) {
			purple_debug_warning("omemo", "no session with %s:%u: %s\n",
			                     f->jid, f->device_id,
			                     error ? error->message : "no bundle");
			key = omemo_device_key(oa, f->jid, f->device_id);
			g_hash_table_replace(omemo_failed, key, GINT_TO_POINTER(1));
			g_clear_error(&error);
		} else {
			purple_debug_info("omemo", "session with %s:%u built\n",
			                  f->jid, f->device_id);
		}
	}
	omemo_fetch_free(f);
}

static void
omemo_fetch_bundle(PurpleConnection *gc, const char *jid, guint32 device_id,
                   OmemoDoneFunc done, gpointer data)
{
	OmemoFetch *f = g_new0(OmemoFetch, 1);
	char *node;

	f->jid = g_strdup(jid);
	f->device_id = device_id;
	f->done = done;
	f->done_data = data;
	node = g_strdup_printf("%s%u", OMEMO_NS_BUNDLES, device_id);
	omemo_pep_request(gc, jid, node, omemo_bundle_cb, f);
	g_free(node);
}

/* Devices of 'jid' we would encrypt to but have no session with yet. */
static GArray *
omemo_devices_missing(OmemoAccount *oa, const char *jid)
{
	GArray *ids = omemo_store_get_devices(oa, jid, TRUE);
	GArray *missing = g_array_new(FALSE, FALSE, sizeof(guint32));
	guint i;

	for (i = 0; i < ids->len; i++) {
		guint32 id = g_array_index(ids, guint32, i);
		char *key;

		if (purple_strequal(jid, oa->jid) && id == oa->device_id)
			continue;
		if (omemo_store_get_trust(oa, jid, id, NULL) == OMEMO_TRUST_UNTRUSTED)
			continue;
		if (omemo_store_has_session(oa, jid, id))
			continue;
		key = omemo_device_key(oa, jid, id);
		if (!g_hash_table_lookup(omemo_failed, key))
			g_array_append_val(missing, id);
		g_free(key);
	}
	g_array_free(ids, TRUE);
	return missing;
}

/**************************************************************************
 * MUC tracking
 **************************************************************************/

static void
omemo_muc_free(OmemoMuc *muc)
{
	g_hash_table_destroy(muc->occupants);
	g_free(muc->self_nick);
	g_free(muc);
}

static OmemoMuc *
omemo_muc_get(PurpleAccount *account, const char *room, gboolean create)
{
	char *key = omemo_conv_key(account, room, TRUE);
	OmemoMuc *muc = g_hash_table_lookup(omemo_mucs, key);

	if (muc == NULL && create) {
		muc = g_new0(OmemoMuc, 1);
		muc->occupants = g_hash_table_new_full(g_str_hash, g_str_equal,
		                                       g_free, g_free);
		g_hash_table_insert(omemo_mucs, key, muc);
		key = NULL;
	}
	g_free(key);
	return muc;
}

static gboolean
omemo_muc_is_private(OmemoMuc *muc)
{
	return muc && muc->features_known && muc->membersonly &&
	       muc->nonanonymous;
}

typedef struct {
	PurpleAccount *account;
	char *room;
	OmemoDoneFunc done;
	gpointer data;
} OmemoDisco;

static void
omemo_muc_disco_cb(PurpleConnection *gc, const char *type, xmlnode *iq,
                   gpointer data)
{
	OmemoDisco *d = data;
	OmemoMuc *muc = omemo_muc_get(d->account, d->room, FALSE);

	if (muc && iq && purple_strequal(type, "result")) {
		xmlnode *query = xmlnode_get_child_with_namespace(iq, "query",
		                                                  NS_DISCO_INFO);
		xmlnode *feat;

		muc->membersonly = muc->nonanonymous = FALSE;
		for (feat = query ? xmlnode_get_child(query, "feature") : NULL; feat;
		     feat = xmlnode_get_next_twin(feat)) {
			const char *var = xmlnode_get_attrib(feat, "var");

			if (purple_strequal(var, "muc_membersonly"))
				muc->membersonly = TRUE;
			else if (purple_strequal(var, "muc_nonanonymous"))
				muc->nonanonymous = TRUE;
		}
		muc->features_known = TRUE;
		purple_debug_info("omemo", "room %s: members-only %d, "
		                  "non-anonymous %d\n", d->room, muc->membersonly,
		                  muc->nonanonymous);
		omemo_emit_state(d->account, d->room, TRUE);
	} else if (muc && !purple_strequal(type, "cancel")) {
		muc->features_known = TRUE;
		muc->membersonly = muc->nonanonymous = FALSE;
	}
	if (muc && purple_strequal(type, "cancel"))
		muc->disco_requested = FALSE;

	if (d->done)
		d->done(d->data);
	g_free(d->room);
	g_free(d);
}

static void
omemo_muc_disco(PurpleConnection *gc, const char *room, OmemoDoneFunc done,
                gpointer data)
{
	OmemoDisco *d = g_new0(OmemoDisco, 1);
	OmemoMuc *muc;
	xmlnode *query = xmlnode_new("query");

	d->account = purple_connection_get_account(gc);
	d->room = g_strdup(room);
	d->done = done;
	d->data = data;
	muc = omemo_muc_get(d->account, room, TRUE);
	muc->disco_requested = TRUE;

	xmlnode_set_namespace(query, NS_DISCO_INFO);
	omemo_send_iq(gc, "get", room, query, omemo_muc_disco_cb, d);
}

static gboolean omemo_conv_enabled(PurpleAccount *account, const char *name,
                                   gboolean is_chat);

static void
omemo_handle_presence(PurpleConnection *gc, OmemoAccount *oa,
                      xmlnode *presence)
{
	PurpleAccount *account = purple_connection_get_account(gc);
	const char *from = xmlnode_get_attrib(presence, "from");
	const char *nick = omemo_resource(from);
	xmlnode *x, *item, *status;
	gboolean self = FALSE, unavailable;
	OmemoMuc *muc;
	char *room;

	x = xmlnode_get_child_with_namespace(presence, "x", NS_MUC_USER);
	if (x == NULL || nick == NULL)
		return;

	for (status = xmlnode_get_child(x, "status"); status;
	     status = xmlnode_get_next_twin(status))
		if (purple_strequal(xmlnode_get_attrib(status, "code"), "110"))
			self = TRUE;

	room = omemo_bare_jid(from);
	unavailable = purple_strequal(xmlnode_get_attrib(presence, "type"),
	                              "unavailable");

	if (self && unavailable) {
		/* We left: forget the occupants (features stay valid). */
		muc = omemo_muc_get(account, room, FALSE);
		if (muc) {
			g_hash_table_remove_all(muc->occupants);
			g_free(muc->self_nick);
			muc->self_nick = NULL;
		}
		g_free(room);
		return;
	}

	muc = omemo_muc_get(account, room, TRUE);
	item = xmlnode_get_child(x, "item");

	if (unavailable) {
		g_hash_table_remove(muc->occupants, nick);
	} else if (item && xmlnode_get_attrib(item, "jid")) {
		char *real = omemo_bare_jid(xmlnode_get_attrib(item, "jid"));
		gboolean known = omemo_store_device_list_known(oa, real);

		g_hash_table_replace(muc->occupants, g_strdup(nick), g_strdup(real));
		if (!known && !purple_strequal(real, oa->jid) &&
		    omemo_conv_enabled(account, room, TRUE))
			omemo_fetch_devicelist(gc, real, NULL, NULL);
		g_free(real);
	}

	if (self) {
		g_free(muc->self_nick);
		muc->self_nick = g_strdup(nick);
		if (!muc->disco_requested)
			omemo_muc_disco(gc, room, NULL, NULL);
	}
	g_free(room);
}

/* Real bare JIDs of the room's occupants, without ourselves. */
static GList *
omemo_muc_members(OmemoAccount *oa, OmemoMuc *muc)
{
	GList *ret = NULL;
	GHashTableIter iter;
	gpointer value;

	if (muc == NULL)
		return NULL;
	g_hash_table_iter_init(&iter, muc->occupants);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		if (purple_strequal(value, oa->jid) ||
		    g_list_find_custom(ret, value, (GCompareFunc)strcmp))
			continue;
		ret = g_list_prepend(ret, g_strdup(value));
	}
	return ret;
}

/**************************************************************************
 * Per-conversation state
 **************************************************************************/

static int
omemo_conv_explicit(PurpleAccount *account, const char *name, gboolean is_chat)
{
	char *key = omemo_conv_key(account, name, is_chat);
	gpointer v = g_hash_table_lookup(omemo_overrides, key);
	PurpleBlistNode *node;
	char *bare;

	g_free(key);
	if (v != NULL)
		return GPOINTER_TO_INT(v);

	bare = omemo_bare_jid(name);
	node = is_chat ? (PurpleBlistNode *)purple_blist_find_chat(account, bare)
	               : (PurpleBlistNode *)purple_find_buddy(account, bare);
	g_free(bare);
	if (node && node->settings &&
	    g_hash_table_lookup(node->settings, BLIST_SETTING))
		return purple_blist_node_get_bool(node, BLIST_SETTING) ? 1 : -1;
	return 0;
}

static gboolean
omemo_conv_enabled(PurpleAccount *account, const char *name, gboolean is_chat)
{
	OmemoAccount *oa;
	int explicit;
	gboolean ret = FALSE;

	if (!omemo_is_xmpp(account) || name == NULL)
		return FALSE;

	explicit = omemo_conv_explicit(account, name, is_chat);
	if (explicit != 0)
		return explicit > 0;
	if (!purple_prefs_get_bool(PREF_DEFAULT_ON))
		return FALSE;

	oa = omemo_account_get(account);
	if (oa == NULL)
		return FALSE;
	if (is_chat) {
		char *room = omemo_bare_jid(name);

		ret = omemo_muc_is_private(omemo_muc_get(account, room, FALSE));
		g_free(room);
	} else {
		char *bare = omemo_bare_jid(name);
		GArray *ids = omemo_store_get_devices(oa, bare, TRUE);

		ret = ids->len > 0;
		g_array_free(ids, TRUE);
		g_free(bare);
	}
	return ret;
}

/* The JIDs a conversation encrypts to: contact(s) + ourselves. */
static GList *
omemo_conv_targets(OmemoAccount *oa, PurpleAccount *account, const char *name,
                   gboolean is_chat)
{
	GList *targets;
	char *bare = omemo_bare_jid(name);

	if (is_chat) {
		targets = omemo_muc_members(oa, omemo_muc_get(account, bare, FALSE));
		g_free(bare);
	} else {
		targets = g_list_prepend(NULL, bare);
	}
	return g_list_append(targets, g_strdup(oa->jid));
}

static void
omemo_address_array_free(GArray *array)
{
	guint i;

	if (array == NULL)
		return;
	for (i = 0; i < array->len; i++)
		g_free(g_array_index(array, OmemoAddress, i).jid);
	g_array_free(array, TRUE);
}

static gboolean
omemo_trust_usable(OmemoTrust trust)
{
	return trust == OMEMO_TRUST_TRUSTED || trust == OMEMO_TRUST_VERIFIED;
}

/*
 * Collect the recipient devices of a conversation. Returns FALSE with a
 * localized reason when there is no trusted device on the other side.
 */
static gboolean
omemo_conv_recipients(OmemoAccount *oa, PurpleAccount *account,
                      const char *name, gboolean is_chat, GArray **out,
                      char **why)
{
	GArray *recipients = omemo_address_array_new();
	GList *targets, *l;
	guint others = 0, undecided = 0;
	char *bare = omemo_bare_jid(name);

	if (is_chat) {
		OmemoMuc *muc = omemo_muc_get(account, bare, FALSE);

		if (!omemo_muc_is_private(muc)) {
			*why = g_strdup(muc && muc->features_known ?
				_("OMEMO needs a members-only, non-anonymous room") :
				_("the room's configuration is not known yet"));
			g_free(bare);
			g_array_free(recipients, TRUE);
			return FALSE;
		}
	}

	targets = omemo_conv_targets(oa, account, name, is_chat);
	for (l = targets; l; l = l->next) {
		const char *jid = l->data;
		gboolean own = purple_strequal(jid, oa->jid);
		GArray *ids = omemo_store_get_devices(oa, jid, TRUE);
		guint i;

		for (i = 0; i < ids->len; i++) {
			guint32 id = g_array_index(ids, guint32, i);
			OmemoTrust trust;

			if (own && id == oa->device_id)
				continue;
			trust = omemo_store_get_trust(oa, jid, id, NULL);
			if (trust == OMEMO_TRUST_UNDECIDED &&
			    omemo_store_has_session(oa, jid, id))
				undecided++;
			if (!omemo_trust_usable(trust) ||
			    !omemo_store_has_session(oa, jid, id))
				continue;
			omemo_address_array_add(recipients, jid, id);
			if (!own)
				others++;
		}
		g_array_free(ids, TRUE);
	}
	g_list_free_full(targets, g_free);

	if (others == 0) {
		if (undecided > 0)
			*why = g_strdup_printf(_("%s has no trusted OMEMO device; "
			                         "decide on the fingerprints with "
			                         "/omemo trust"), bare);
		else
			*why = g_strdup_printf(_("%s has no usable OMEMO device"), bare);
		g_free(bare);
		omemo_address_array_free(recipients);
		return FALSE;
	}

	g_free(bare);
	*out = recipients;
	return TRUE;
}

static gboolean
omemo_conv_all_trusted(OmemoAccount *oa, PurpleAccount *account,
                       const char *name, gboolean is_chat)
{
	GList *targets = omemo_conv_targets(oa, account, name, is_chat), *l;
	gboolean ret = TRUE, any = FALSE;

	for (l = targets; l; l = l->next) {
		const char *jid = l->data;
		gboolean own = purple_strequal(jid, oa->jid);
		GArray *ids = omemo_store_get_devices(oa, jid, TRUE);
		guint i;

		for (i = 0; i < ids->len; i++) {
			guint32 id = g_array_index(ids, guint32, i);
			gboolean has_key = FALSE;
			OmemoTrust trust = omemo_store_get_trust(oa, jid, id, &has_key);

			/* Devices whose key we never saw are not candidates yet. */
			if ((own && id == oa->device_id) || !has_key)
				continue;
			if (!own && omemo_trust_usable(trust))
				any = TRUE;
			if (trust == OMEMO_TRUST_UNDECIDED)
				ret = FALSE;
		}
		g_array_free(ids, TRUE);
	}
	g_list_free_full(targets, g_free);
	return ret && any;
}

static void
omemo_emit_state(PurpleAccount *account, const char *name, gboolean is_chat)
{
	OmemoAccount *oa = omemo_account_get(account);
	gboolean enabled, trusted;

	if (oa == NULL || name == NULL)
		return;
	enabled = omemo_conv_enabled(account, name, is_chat);
	trusted = omemo_conv_all_trusted(oa, account, name, is_chat);
	purple_signal_emit(omemo_plugin, "omemo-conv-state-changed", account,
	                   name, enabled, trusted);
}

/**************************************************************************
 * Holding messages back until the sessions exist
 **************************************************************************/

static void
omemo_prep_free(OmemoPrep *prep)
{
	g_list_free_full(prep->messages, g_free);
	g_free(prep->name);
	g_free(prep->key);
	g_free(prep);
}

static void
omemo_prep_flush(OmemoPrep *prep)
{
	PurpleConnection *gc = purple_account_get_connection(prep->account);
	PurpleConversation *conv;
	OmemoAccount *oa = omemo_account_get(prep->account);
	GArray *recipients = NULL;
	char *why = NULL;
	GList *l;

	g_hash_table_steal(omemo_preps, prep->key);

	if (prep->messages == NULL) {
		omemo_emit_state(prep->account, prep->name, prep->is_chat);
		omemo_prep_free(prep);
		return;
	}

	if (gc == NULL || oa == NULL ||
	    !omemo_conv_recipients(oa, prep->account, prep->name, prep->is_chat,
	                           &recipients, &why)) {
		char *msg = g_strdup_printf(
			_("OMEMO: %u message(s) not sent: %s"),
			g_list_length(prep->messages),
			why ? why : _("the account is offline"));

		omemo_conv_notice(prep->account, prep->name, prep->is_chat, msg,
		                  TRUE);
		g_free(msg);
		g_free(why);
		omemo_prep_free(prep);
		return;
	}
	omemo_address_array_free(recipients);

	conv = omemo_find_conv(prep->account, prep->name, prep->is_chat);
	for (l = prep->messages; l; l = l->next) {
		const char *text = l->data;

		if (prep->is_chat) {
			if (conv)
				serv_chat_send(gc, purple_conv_chat_get_id(
					PURPLE_CONV_CHAT(conv)), text, PURPLE_MESSAGE_SEND);
		} else {
			int err = serv_send_im(gc, prep->name, text, PURPLE_MESSAGE_SEND);

			if (err > 0 && conv)
				purple_conv_im_write(PURPLE_CONV_IM(conv), NULL, text,
				                     PURPLE_MESSAGE_SEND, time(NULL));
		}
	}
	omemo_emit_state(prep->account, prep->name, prep->is_chat);
	omemo_prep_free(prep);
}

static void
omemo_prep_done(gpointer data)
{
	OmemoPrep *prep = data;

	if (--prep->pending > 0)
		return;
	if (prep->cancelled)
		omemo_prep_free(prep);
	else
		omemo_prep_flush(prep);
}

typedef struct {
	OmemoPrep *prep;
	char *jid;
} OmemoPrepJid;

static void
omemo_prep_fetch_bundles(PurpleConnection *gc, OmemoAccount *oa,
                         OmemoPrep *prep, const char *jid)
{
	GArray *missing = omemo_devices_missing(oa, jid);
	guint i;

	for (i = 0; i < missing->len; i++) {
		prep->pending++;
		omemo_fetch_bundle(gc, jid, g_array_index(missing, guint32, i),
		                   omemo_prep_done, prep);
	}
	g_array_free(missing, TRUE);
}

static void
omemo_prep_devicelist_done(gpointer data)
{
	OmemoPrepJid *pj = data;
	PurpleConnection *gc = purple_account_get_connection(pj->prep->account);
	OmemoAccount *oa = omemo_account_get(pj->prep->account);

	if (gc && oa && !pj->prep->cancelled)
		omemo_prep_fetch_bundles(gc, oa, pj->prep, pj->jid);
	omemo_prep_done(pj->prep);
	g_free(pj->jid);
	g_free(pj);
}

static void omemo_prep_start_targets(OmemoPrep *prep);

static void
omemo_prep_disco_done(gpointer data)
{
	OmemoPrep *prep = data;

	if (!prep->cancelled)
		omemo_prep_start_targets(prep);
	omemo_prep_done(prep);
}

static void
omemo_prep_start_targets(OmemoPrep *prep)
{
	PurpleConnection *gc = purple_account_get_connection(prep->account);
	OmemoAccount *oa = omemo_account_get(prep->account);
	GList *targets, *l;

	if (gc == NULL || oa == NULL)
		return;
	targets = omemo_conv_targets(oa, prep->account, prep->name, prep->is_chat);
	for (l = targets; l; l = l->next) {
		const char *jid = l->data;

		if (!omemo_store_device_list_known(oa, jid)) {
			OmemoPrepJid *pj = g_new0(OmemoPrepJid, 1);

			pj->prep = prep;
			pj->jid = g_strdup(jid);
			prep->pending++;
			omemo_fetch_devicelist(gc, jid, omemo_prep_devicelist_done, pj);
		} else {
			omemo_prep_fetch_bundles(gc, oa, prep, jid);
		}
	}
	g_list_free_full(targets, g_free);
}

/* Does the conversation need network round trips before it can encrypt? */
static gboolean
omemo_conv_needs_fetch(OmemoAccount *oa, PurpleAccount *account,
                       const char *name, gboolean is_chat)
{
	GList *targets, *l;
	gboolean ret = FALSE;

	if (is_chat) {
		char *room = omemo_bare_jid(name);
		OmemoMuc *muc = omemo_muc_get(account, room, FALSE);

		g_free(room);
		if (muc == NULL || !muc->features_known)
			return TRUE;
	}

	targets = omemo_conv_targets(oa, account, name, is_chat);
	for (l = targets; l && !ret; l = l->next) {
		GArray *missing;

		if (!omemo_store_device_list_known(oa, l->data)) {
			ret = TRUE;
			break;
		}
		missing = omemo_devices_missing(oa, l->data);
		ret = missing->len > 0;
		g_array_free(missing, TRUE);
	}
	g_list_free_full(targets, g_free);
	return ret;
}

/*
 * Start (or join) fetching what a conversation needs. 'message' (may be
 * NULL) is queued and sent when done. Returns the prep.
 */
static OmemoPrep *
omemo_prep_start(PurpleAccount *account, const char *name, gboolean is_chat,
                 const char *message)
{
	char *key = omemo_conv_key(account, name, is_chat);
	OmemoPrep *prep = g_hash_table_lookup(omemo_preps, key);
	PurpleConnection *gc;

	if (prep != NULL) {
		g_free(key);
		if (message)
			prep->messages = g_list_append(prep->messages, g_strdup(message));
		return prep;
	}

	prep = g_new0(OmemoPrep, 1);
	prep->account = account;
	prep->name = g_strdup(name);
	prep->key = key;
	prep->is_chat = is_chat;
	if (message)
		prep->messages = g_list_append(NULL, g_strdup(message));
	g_hash_table_insert(omemo_preps, prep->key, prep);

	/* Hold one reference while starting, so nothing flushes early. */
	prep->pending = 1;
	gc = purple_account_get_connection(account);
	if (gc && is_chat) {
		char *room = omemo_bare_jid(name);
		OmemoMuc *muc = omemo_muc_get(account, room, FALSE);

		if (muc == NULL || !muc->features_known) {
			prep->pending++;
			omemo_muc_disco(gc, room, omemo_prep_disco_done, prep);
		} else {
			omemo_prep_start_targets(prep);
		}
		g_free(room);
	} else if (gc) {
		omemo_prep_start_targets(prep);
	}
	omemo_prep_done(prep);
	return prep;
}

static void
omemo_drop_preps(PurpleAccount *account)
{
	GHashTableIter iter;
	gpointer value;
	GList *drop = NULL, *l;

	g_hash_table_iter_init(&iter, omemo_preps);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		OmemoPrep *prep = value;

		if (prep->account == account) {
			drop = g_list_prepend(drop, prep);
			g_hash_table_iter_steal(&iter);
		}
	}
	for (l = drop; l; l = l->next) {
		OmemoPrep *prep = l->data;

		if (prep->messages)
			omemo_conv_notice(account, prep->name, prep->is_chat,
			                  _("OMEMO: queued messages were not sent "
			                    "(disconnected)"), TRUE);
		/* Its IQs are cancelled next; the last one frees it. */
		prep->cancelled = TRUE;
		if (prep->pending <= 0)
			omemo_prep_free(prep);
	}
	g_list_free(drop);
}

/**************************************************************************
 * libpurple conversation hooks (send side)
 **************************************************************************/

static void
omemo_sending_common(PurpleAccount *account, const char *name,
                     gboolean is_chat, char **message)
{
	OmemoAccount *oa;
	char *key, *why = NULL;
	GArray *recipients = NULL;

	if (message == NULL || *message == NULL || **message == '\0')
		return;
	if (!omemo_conv_enabled(account, name, is_chat))
		return;
	oa = omemo_account_get(account);
	if (oa == NULL)
		return;

	key = omemo_conv_key(account, name, is_chat);
	if (g_hash_table_lookup(omemo_preps, key) ||
	    omemo_conv_needs_fetch(oa, account, name, is_chat)) {
		g_free(key);
		omemo_prep_start(account, name, is_chat, *message);
		g_free(*message);
		*message = NULL;
		return;
	}
	g_free(key);

	if (!omemo_conv_recipients(oa, account, name, is_chat, &recipients,
	                           &why)) {
		char *msg = g_strdup_printf(_("OMEMO: message not sent: %s"), why);

		omemo_conv_notice(account, name, is_chat, msg, TRUE);
		g_free(msg);
		g_free(why);
		g_free(*message);
		*message = NULL;
		return;
	}
	omemo_address_array_free(recipients);
	/* Ready: jabber-sending-xmlnode encrypts it. */
}

static void
omemo_sending_im_cb(PurpleAccount *account, const char *who, char **message,
                    gpointer data)
{
	omemo_sending_common(account, who, FALSE, message);
}

static void
omemo_sending_chat_cb(PurpleAccount *account, char **message, int id,
                      gpointer data)
{
	PurpleConnection *gc = purple_account_get_connection(account);
	PurpleConversation *conv = gc ? purple_find_chat(gc, id) : NULL;

	if (conv)
		omemo_sending_common(account, purple_conversation_get_name(conv),
		                     TRUE, message);
}

static void
omemo_conversation_created_cb(PurpleConversation *conv, gpointer data)
{
	PurpleAccount *account = purple_conversation_get_account(conv);
	PurpleConnection *gc = purple_conversation_get_gc(conv);
	OmemoAccount *oa;
	char *bare;

	if (gc == NULL || purple_conversation_get_type(conv) != PURPLE_CONV_TYPE_IM)
		return;
	oa = omemo_account_get(account);
	if (oa == NULL)
		return;
	bare = omemo_bare_jid(purple_conversation_get_name(conv));
	if (!omemo_store_device_list_known(oa, bare))
		omemo_fetch_devicelist(gc, bare, NULL, NULL);
	else
		omemo_emit_state(account, purple_conversation_get_name(conv), FALSE);
	g_free(bare);
}

/**************************************************************************
 * Stanza hooks
 **************************************************************************/

static char *
omemo_sent_key(OmemoAccount *oa, const char *id)
{
	return g_strdup_printf("%p\n%s", (void *)oa, id);
}

static void
omemo_sent_remember(OmemoAccount *oa, const char *id, const char *plaintext)
{
	char *key;

	if (id == NULL)
		return;
	key = omemo_sent_key(oa, id);
	if (g_hash_table_lookup(omemo_sent, key)) {
		g_free(key);
		return;
	}
	g_hash_table_insert(omemo_sent, key, g_strdup(plaintext));
	g_queue_push_tail(omemo_sent_order, key);
	while (g_queue_get_length(omemo_sent_order) > SENT_CACHE_SIZE)
		g_hash_table_remove(omemo_sent, g_queue_pop_head(omemo_sent_order));
}

static const char *
omemo_sent_lookup(OmemoAccount *oa, xmlnode *message)
{
	const char *ids[2];
	xmlnode *origin;
	int i;

	ids[0] = xmlnode_get_attrib(message, "id");
	origin = xmlnode_get_child_with_namespace(message, "origin-id", NS_SID);
	ids[1] = origin ? xmlnode_get_attrib(origin, "id") : NULL;

	for (i = 0; i < 2; i++) {
		const char *text;
		char *key;

		if (ids[i] == NULL)
			continue;
		key = omemo_sent_key(oa, ids[i]);
		text = g_hash_table_lookup(omemo_sent, key);
		g_free(key);
		if (text)
			return text;
	}
	return NULL;
}

static void
omemo_sending_xmlnode_cb(PurpleConnection *gc, xmlnode **packet, gpointer data)
{
	PurpleAccount *account = purple_connection_get_account(gc);
	xmlnode *message = packet ? *packet : NULL;
	xmlnode *body, *encrypted;
	const char *type, *to;
	gboolean is_chat;
	OmemoAccount *oa;
	GArray *recipients = NULL;
	GError *error = NULL;
	char *name, *text, *why = NULL;
	guint n_keys = 0;

	if (message == NULL || !purple_strequal(message->name, "message"))
		return;
	body = xmlnode_get_child(message, "body");
	to = xmlnode_get_attrib(message, "to");
	if (body == NULL || to == NULL ||
	    xmlnode_get_child_with_namespace(message, "encrypted", OMEMO_NS))
		return;

	type = xmlnode_get_attrib(message, "type");
	if (purple_strequal(type, "groupchat"))
		is_chat = TRUE;
	else if (type == NULL || purple_strequal(type, "chat") ||
	         purple_strequal(type, "normal"))
		is_chat = FALSE;
	else
		return;

	name = omemo_bare_jid(to);
	if (!omemo_conv_enabled(account, name, is_chat) ||
	    (oa = omemo_account_get(account)) == NULL) {
		g_free(name);
		return;
	}

	text = xmlnode_get_data(body);
	if (text == NULL || *text == '\0') {
		g_free(text);
		g_free(name);
		return;
	}

	if (!omemo_conv_recipients(oa, account, name, is_chat, &recipients, &why) ||
	    (encrypted = omemo_encrypt(oa, text, recipients, &n_keys,
	                               &error)) == NULL) {
		char *msg = g_strdup_printf(_("OMEMO: the message was NOT sent: %s"),
		                            why ? why : error ? error->message : "?");

		/* Never let the plaintext out; the stanza leaves without it. */
		omemo_message_strip_plaintext(message);
		omemo_conv_notice(account, name, is_chat, msg, TRUE);
		g_free(msg);
		g_free(why);
		g_clear_error(&error);
		omemo_address_array_free(recipients);
		g_free(text);
		g_free(name);
		return;
	}
	omemo_address_array_free(recipients);

	if (xmlnode_get_attrib(message, "id") == NULL) {
		guint32 rnd;
		char *id;

		omemo_random_bytes((guint8 *)&rnd, sizeof(rnd));
		id = g_strdup_printf("omemo-msg-%08x%u", rnd, ++omemo_iq_counter);
		xmlnode_set_attrib(message, "id", id);
		g_free(id);
	}

	omemo_message_apply_encrypted(message, encrypted);
	omemo_sent_remember(oa, xmlnode_get_attrib(message, "id"), text);
	{
		xmlnode *origin = xmlnode_get_child_with_namespace(message,
		                                                   "origin-id", NS_SID);

		if (origin)
			omemo_sent_remember(oa, xmlnode_get_attrib(origin, "id"), text);
	}
	purple_debug_info("omemo", "encrypted message to %s for %u device(s)\n",
	                  name, n_keys);

	memset(text, 0, strlen(text));
	g_free(text);
	g_free(name);
}

/* Decrypt one <message/> (top level, or forwarded inside a carbon/MAM). */
static void
omemo_decrypt_one(PurpleConnection *gc, OmemoAccount *oa, xmlnode *message)
{
	PurpleAccount *account = purple_connection_get_account(gc);
	const char *from = xmlnode_get_attrib(message, "from");
	OmemoDecryptStatus status;
	char *sender = NULL;

	if (xmlnode_get_child_with_namespace(message, "encrypted", OMEMO_NS) == NULL)
		return;

	if (purple_strequal(xmlnode_get_attrib(message, "type"), "groupchat")) {
		xmlnode *x = xmlnode_get_child_with_namespace(message, "x",
		                                              NS_MUC_USER);
		xmlnode *item = x ? xmlnode_get_child(x, "item") : NULL;
		const char *nick = omemo_resource(from);

		if (item && xmlnode_get_attrib(item, "jid")) {
			sender = omemo_bare_jid(xmlnode_get_attrib(item, "jid"));
		} else if (nick) {
			char *room = omemo_bare_jid(from);
			OmemoMuc *muc = omemo_muc_get(account, room, FALSE);
			const char *real = muc ? g_hash_table_lookup(muc->occupants, nick)
			                       : NULL;

			if (real)
				sender = g_strdup(real);
			else if (muc && purple_strequal(nick, muc->self_nick))
				sender = g_strdup(oa->jid);
			g_free(room);
		}
		if (sender == NULL) {
			omemo_message_set_body(message,
				_("[OMEMO: could not decrypt (the sender's real JID is "
				  "unknown)]"));
			xmlnode_free(xmlnode_get_child_with_namespace(message, "encrypted",
			                                              OMEMO_NS));
			return;
		}
	} else {
		sender = from ? omemo_bare_jid(from) : g_strdup(oa->jid);
	}

	status = omemo_message_decrypt(oa, message, sender);

	if (status == OMEMO_DECRYPT_OWN_DEVICE) {
		const char *text = omemo_sent_lookup(oa, message);

		omemo_message_set_body(message, text ? text :
			_("[OMEMO: sent from this device; the text is in the local log]"));
		xmlnode_free(xmlnode_get_child_with_namespace(message, "encrypted",
		                                              OMEMO_NS));
	} else if (status == OMEMO_DECRYPT_OK || status == OMEMO_DECRYPT_KEY_ONLY) {
		purple_debug_info("omemo", "decrypted %s from %s\n",
		                  status == OMEMO_DECRYPT_OK ? "message" : "key",
		                  sender);
		omemo_schedule_replenish(gc, oa);
	}
	g_free(sender);
}

static xmlnode *
omemo_forwarded_message(xmlnode *wrapper)
{
	xmlnode *fwd = xmlnode_get_child_with_namespace(wrapper, "forwarded",
	                                                NS_FORWARD);

	return fwd ? xmlnode_get_child(fwd, "message") : NULL;
}

static void
omemo_handle_message(PurpleConnection *gc, OmemoAccount *oa, xmlnode *message)
{
	const char *from = xmlnode_get_attrib(message, "from");
	char *from_bare = from ? omemo_bare_jid(from) : g_strdup(oa->jid);
	gboolean from_self = purple_strequal(from_bare, oa->jid);
	xmlnode *child;

	/* Device list notifications (PEP +notify). */
	child = xmlnode_get_child_with_namespace(message, "event", NS_PUBSUB_EVENT);
	if (child) {
		xmlnode *items = xmlnode_get_child(child, "items");

		if (items && purple_strequal(xmlnode_get_attrib(items, "node"),
		                             OMEMO_NS_DEVICELIST)) {
			xmlnode *item = xmlnode_get_child(items, "item");
			xmlnode *list = item ? xmlnode_get_child_with_namespace(item,
			                           "list", OMEMO_NS) : NULL;
			GArray *ids = omemo_xml_parse_devicelist(list);

			omemo_contact_devicelist(gc, oa, from_bare, ids);
			g_array_free(ids, TRUE);
		}
	}

	omemo_decrypt_one(gc, oa, message);

	for (child = message->child; child; child = child->next) {
		const char *xmlns;

		if (child->type != XMLNODE_TYPE_TAG)
			continue;
		xmlns = xmlnode_get_namespace(child);

		if (purple_strequal(xmlns, NS_CARBONS) &&
		    (purple_strequal(child->name, "sent") ||
		     purple_strequal(child->name, "received"))) {
			/* Carbons are only valid from our own bare JID. */
			xmlnode *inner = omemo_forwarded_message(child);

			if (inner && from_self)
				omemo_decrypt_one(gc, oa, inner);
		} else if (purple_strequal(child->name, "result") && xmlns &&
		           g_str_has_prefix(xmlns, "urn:xmpp:mam:")) {
			xmlnode *inner = omemo_forwarded_message(child);

			if (inner)
				omemo_decrypt_one(gc, oa, inner);
		}
	}
	g_free(from_bare);
}

static void
omemo_receiving_xmlnode_cb(PurpleConnection *gc, xmlnode **packet,
                           gpointer data)
{
	xmlnode *node = packet ? *packet : NULL;
	OmemoAccount *oa;

	if (node == NULL)
		return;
	if (!purple_strequal(node->name, "message") &&
	    !purple_strequal(node->name, "presence"))
		return;
	oa = omemo_account_get(purple_connection_get_account(gc));
	if (oa == NULL)
		return;

	if (purple_strequal(node->name, "presence"))
		omemo_handle_presence(gc, oa, node);
	else
		omemo_handle_message(gc, oa, node);
}

/**************************************************************************
 * Connection hooks
 **************************************************************************/

static void
omemo_signed_on_cb(PurpleConnection *gc, gpointer data)
{
	if (omemo_is_xmpp(purple_connection_get_account(gc)))
		omemo_account_setup(gc);
}

static void
omemo_signing_off_cb(PurpleConnection *gc, gpointer data)
{
	PurpleAccount *account = purple_connection_get_account(gc);
	gpointer timeout = g_hash_table_lookup(omemo_replenish, gc);
	GHashTableIter iter;
	gpointer key, value;

	if (!omemo_is_xmpp(account))
		return;
	if (timeout) {
		purple_timeout_remove(GPOINTER_TO_UINT(timeout));
		g_hash_table_remove(omemo_replenish, gc);
	}
	omemo_drop_preps(account);
	omemo_cancel_iqs(gc);

	/* Occupant lists are rebuilt from presence on the next join. */
	g_hash_table_iter_init(&iter, omemo_mucs);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		char *prefix = g_strdup_printf("%p\n", (void *)account);

		if (g_str_has_prefix(key, prefix))
			g_hash_table_iter_remove(&iter);
		g_free(prefix);
	}
}

static void
omemo_account_removed_cb(PurpleAccount *account, gpointer data)
{
	g_hash_table_remove(omemo_accounts, account);
}

/**************************************************************************
 * IPC for the UI
 **************************************************************************/

static void
omemo_marshal_BOOLEAN__POINTER_POINTER_UINT_POINTER(PurpleCallback cb,
		va_list args, void *data, void **return_val)
{
	gboolean ret;
	void *a1 = va_arg(args, void *);
	void *a2 = va_arg(args, void *);
	guint a3 = va_arg(args, guint);
	void *a4 = va_arg(args, void *);

	ret = ((gboolean (*)(void *, void *, guint, void *, void *))cb)
		(a1, a2, a3, a4, data);
	if (return_val != NULL)
		*return_val = GINT_TO_POINTER(ret);
}

static void
omemo_marshal_VOID__POINTER_POINTER_UINT_POINTER(PurpleCallback cb,
		va_list args, void *data, void **return_val)
{
	void *a1 = va_arg(args, void *);
	void *a2 = va_arg(args, void *);
	guint a3 = va_arg(args, guint);
	void *a4 = va_arg(args, void *);

	((void (*)(void *, void *, guint, void *, void *))cb)(a1, a2, a3, a4, data);
}

/*
 * omemo-list-devices(PurpleAccount *, const char *jid) -> GList * of
 * GHashTable * (string -> string): "device-id", "fingerprint" (may be ""),
 * "trust", "active" ("1"/"0"), "session" ("1"/"0"), "own" ("1" for this
 * device). Free with g_list_free_full(list, (GDestroyNotify)g_hash_table_unref).
 */
static GList *
omemo_ipc_list_devices(PurpleAccount *account, const char *jid)
{
	OmemoAccount *oa = omemo_account_get(account);
	GList *devices, *l, *ret = NULL;
	char *bare;

	if (oa == NULL)
		return NULL;
	bare = jid ? omemo_bare_jid(jid) : g_strdup(oa->jid);
	devices = omemo_store_list_devices(oa, bare);
	for (l = devices; l; l = l->next) {
		OmemoDeviceInfo *info = l->data;
		GHashTable *h = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
		                                      g_free);
		gboolean own = purple_strequal(bare, oa->jid) &&
		               info->device_id == oa->device_id;

		g_hash_table_insert(h, "device-id",
		                    g_strdup_printf("%u", info->device_id));
		g_hash_table_insert(h, "fingerprint", own ?
		                    omemo_account_fingerprint(oa) :
		                    g_strdup(info->fingerprint ? info->fingerprint : ""));
		g_hash_table_insert(h, "trust", g_strdup(own ? "verified" :
		                    omemo_trust_to_string(info->trust)));
		g_hash_table_insert(h, "active", g_strdup(info->active ? "1" : "0"));
		g_hash_table_insert(h, "session",
		                    g_strdup(info->has_session ? "1" : "0"));
		g_hash_table_insert(h, "own", g_strdup(own ? "1" : "0"));
		ret = g_list_prepend(ret, h);
	}
	g_list_free_full(devices, (GDestroyNotify)omemo_device_info_free);
	g_free(bare);
	return g_list_reverse(ret);
}

static void
omemo_emit_state_for_jid(PurpleAccount *account, const char *jid)
{
	GList *convs;

	for (convs = purple_get_conversations(); convs; convs = convs->next) {
		PurpleConversation *conv = convs->data;
		char *bare;

		if (purple_conversation_get_account(conv) != account)
			continue;
		if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_CHAT) {
			omemo_emit_state(account, purple_conversation_get_name(conv),
			                 TRUE);
			continue;
		}
		bare = omemo_bare_jid(purple_conversation_get_name(conv));
		if (purple_strequal(bare, jid))
			omemo_emit_state(account, purple_conversation_get_name(conv),
			                 FALSE);
		g_free(bare);
	}
}

/* omemo-set-trust(account, jid, device id, "trusted"|...) -> gboolean */
static gboolean
omemo_ipc_set_trust(PurpleAccount *account, const char *jid, guint device_id,
                    const char *trust_str)
{
	OmemoAccount *oa = omemo_account_get(account);
	OmemoTrust trust;
	gboolean ok;
	char *bare;

	if (oa == NULL || jid == NULL ||
	    !omemo_trust_from_string(trust_str, &trust))
		return FALSE;
	bare = omemo_bare_jid(jid);
	ok = omemo_store_set_trust(oa, bare, device_id, trust);
	if (ok) {
		purple_debug_info("omemo", "%s:%u is now %s\n", bare, device_id,
		                  trust_str);
		omemo_emit_state_for_jid(account, bare);
	}
	g_free(bare);
	return ok;
}

/* omemo-own-fingerprint(account) -> newly allocated string */
static char *
omemo_ipc_own_fingerprint(PurpleAccount *account)
{
	OmemoAccount *oa = omemo_account_get(account);

	return oa ? omemo_account_fingerprint(oa) : NULL;
}

static gboolean
omemo_name_is_chat(PurpleAccount *account, const char *name)
{
	return purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, name,
	                                             account) != NULL ||
	       purple_blist_find_chat(account, name) != NULL;
}

/* omemo-conv-enabled(account, conversation name) -> gboolean */
static gboolean
omemo_ipc_conv_enabled(PurpleAccount *account, const char *name)
{
	return omemo_conv_enabled(account, name,
	                          omemo_name_is_chat(account, name));
}

static void
omemo_conv_set_enabled(PurpleAccount *account, const char *name,
                       gboolean is_chat, gboolean enabled)
{
	char *key = omemo_conv_key(account, name, is_chat);
	PurpleBlistNode *node;
	char *bare = omemo_bare_jid(name);

	node = is_chat ? (PurpleBlistNode *)purple_blist_find_chat(account, bare)
	               : (PurpleBlistNode *)purple_find_buddy(account, bare);
	if (node) {
		purple_blist_node_set_bool(node, BLIST_SETTING, enabled);
		g_hash_table_remove(omemo_overrides, key);
		g_free(key);
	} else {
		g_hash_table_replace(omemo_overrides, key,
		                     GINT_TO_POINTER(enabled ? 1 : -1));
	}
	g_free(bare);

	if (enabled && purple_account_is_connected(account))
		omemo_prep_start(account, name, is_chat, NULL);
	omemo_conv_notice(account, name, is_chat, enabled ?
	                  _("OMEMO encryption enabled") :
	                  _("OMEMO encryption disabled"), FALSE);
	omemo_emit_state(account, name, is_chat);
}

/* omemo-conv-set-enabled(account, conversation name, gboolean) */
static void
omemo_ipc_conv_set_enabled(PurpleAccount *account, const char *name,
                           gboolean enabled)
{
	if (!omemo_is_xmpp(account) || name == NULL)
		return;
	omemo_conv_set_enabled(account, name, omemo_name_is_chat(account, name),
	                       enabled);
}

/* omemo-decrypt-url(const char *aesgcm_url, GBytes *ciphertext) ->
 * GByteArray * plaintext, or NULL */
static GByteArray *
omemo_ipc_decrypt_url(const char *url, GBytes *data)
{
	GError *error = NULL;
	GByteArray *ret;
	gsize len = 0;
	const guint8 *bytes;

	if (url == NULL || data == NULL)
		return NULL;
	bytes = g_bytes_get_data(data, &len);
	ret = omemo_aesgcm_decrypt(url, bytes, len, &error);
	if (ret == NULL) {
		purple_debug_warning("omemo", "aesgcm: %s\n", error->message);
		g_error_free(error);
	}
	return ret;
}

/* omemo-encrypt-file(const char *path, char **fragment) -> path of the
 * ciphertext (a temporary file the caller uploads and unlinks); *fragment
 * gets the hex "<iv><key>" to put after '#' in the aesgcm:// URL. */
static char *
omemo_ipc_encrypt_file(const char *path, char **fragment)
{
	GError *error = NULL;
	char *ret;

	if (path == NULL)
		return NULL;
	ret = omemo_aesgcm_encrypt_file(path, fragment, &error);
	if (ret == NULL) {
		purple_debug_warning("omemo", "aesgcm: %s\n", error->message);
		g_error_free(error);
	}
	return ret;
}

static void
omemo_register_ipc(PurplePlugin *plugin)
{
	purple_plugin_ipc_register(plugin, "omemo-list-devices",
		PURPLE_CALLBACK(omemo_ipc_list_devices),
		purple_marshal_POINTER__POINTER_POINTER,
		purple_value_new(PURPLE_TYPE_POINTER), 2,
		purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
		purple_value_new(PURPLE_TYPE_STRING));

	purple_plugin_ipc_register(plugin, "omemo-set-trust",
		PURPLE_CALLBACK(omemo_ipc_set_trust),
		omemo_marshal_BOOLEAN__POINTER_POINTER_UINT_POINTER,
		purple_value_new(PURPLE_TYPE_BOOLEAN), 4,
		purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
		purple_value_new(PURPLE_TYPE_STRING),
		purple_value_new(PURPLE_TYPE_UINT),
		purple_value_new(PURPLE_TYPE_STRING));

	purple_plugin_ipc_register(plugin, "omemo-own-fingerprint",
		PURPLE_CALLBACK(omemo_ipc_own_fingerprint),
		purple_marshal_POINTER__POINTER,
		purple_value_new(PURPLE_TYPE_STRING), 1,
		purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT));

	purple_plugin_ipc_register(plugin, "omemo-conv-enabled",
		PURPLE_CALLBACK(omemo_ipc_conv_enabled),
		purple_marshal_BOOLEAN__POINTER_POINTER,
		purple_value_new(PURPLE_TYPE_BOOLEAN), 2,
		purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
		purple_value_new(PURPLE_TYPE_STRING));

	purple_plugin_ipc_register(plugin, "omemo-conv-set-enabled",
		PURPLE_CALLBACK(omemo_ipc_conv_set_enabled),
		purple_marshal_VOID__POINTER_POINTER_UINT,
		NULL, 3,
		purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
		purple_value_new(PURPLE_TYPE_STRING),
		purple_value_new(PURPLE_TYPE_BOOLEAN));

	purple_plugin_ipc_register(plugin, "omemo-decrypt-url",
		PURPLE_CALLBACK(omemo_ipc_decrypt_url),
		purple_marshal_POINTER__POINTER_POINTER,
		purple_value_new(PURPLE_TYPE_POINTER), 2,
		purple_value_new(PURPLE_TYPE_STRING),
		purple_value_new(PURPLE_TYPE_POINTER));

	purple_plugin_ipc_register(plugin, "omemo-encrypt-file",
		PURPLE_CALLBACK(omemo_ipc_encrypt_file),
		purple_marshal_POINTER__POINTER_POINTER,
		purple_value_new(PURPLE_TYPE_STRING), 2,
		purple_value_new(PURPLE_TYPE_STRING),
		purple_value_new_outgoing(PURPLE_TYPE_STRING));

	/* (account, bare JID, device id, fingerprint) */
	purple_signal_register(plugin, "omemo-new-device",
		omemo_marshal_VOID__POINTER_POINTER_UINT_POINTER, NULL, 4,
		purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
		purple_value_new(PURPLE_TYPE_STRING),
		purple_value_new(PURPLE_TYPE_UINT),
		purple_value_new(PURPLE_TYPE_STRING));

	/* (account, conversation name, enabled, all devices trusted) */
	purple_signal_register(plugin, "omemo-conv-state-changed",
		purple_marshal_VOID__POINTER_POINTER_UINT_UINT, NULL, 4,
		purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
		purple_value_new(PURPLE_TYPE_STRING),
		purple_value_new(PURPLE_TYPE_BOOLEAN),
		purple_value_new(PURPLE_TYPE_BOOLEAN));
}

/**************************************************************************
 * GTK 2 era UI: /omemo command and the "OMEMO fingerprints..." action
 **************************************************************************/

static char *
omemo_describe_devices(PurpleAccount *account, const char *jid, gboolean html)
{
	GList *devices = omemo_ipc_list_devices(account, jid), *l;
	GString *str = g_string_new(NULL);

	if (devices == NULL) {
		char *esc = html ? g_markup_escape_text(jid, -1) : g_strdup(jid);

		g_string_append_printf(str, _("No OMEMO devices known for %s."), esc);
		g_free(esc);
	}
	for (l = devices; l; l = l->next) {
		GHashTable *h = l->data;
		const char *fp = g_hash_table_lookup(h, "fingerprint");

		g_string_append_printf(str, html ?
			"%s<b>%s</b> <tt>%s</tt> %s%s%s" : "%s%s %s %s%s%s",
			l == devices ? "" : (html ? "<br>" : "\n"),
			(const char *)g_hash_table_lookup(h, "device-id"),
			(fp && *fp) ? fp : _("(fingerprint not known yet)"),
			(const char *)g_hash_table_lookup(h, "trust"),
			purple_strequal(g_hash_table_lookup(h, "active"), "1") ? "" :
				_(" (inactive)"),
			purple_strequal(g_hash_table_lookup(h, "own"), "1") ?
				_(" (this device)") : "");
	}
	g_list_free_full(devices, (GDestroyNotify)g_hash_table_unref);
	return g_string_free(str, FALSE);
}

static PurpleCmdRet
omemo_cmd_cb(PurpleConversation *conv, const gchar *cmd, gchar **args,
             gchar **error, void *data)
{
	PurpleAccount *account = purple_conversation_get_account(conv);
	gboolean is_chat = purple_conversation_get_type(conv) ==
	                   PURPLE_CONV_TYPE_CHAT;
	const char *name = purple_conversation_get_name(conv);
	OmemoAccount *oa = omemo_account_get(account);
	char **argv;
	int argc;
	char *msg = NULL;

	if (oa == NULL) {
		*error = g_strdup(_("OMEMO is not available for this account."));
		return PURPLE_CMD_RET_FAILED;
	}

	argv = g_strsplit_set(args && args[0] ? args[0] : "", " \t", -1);
	/* drop empty tokens */
	{
		int i, j = 0;

		for (i = 0; argv[i]; i++) {
			if (*argv[i])
				argv[j++] = argv[i];
			else
				g_free(argv[i]);
		}
		argv[j] = NULL;
		argc = j;
	}

	if (argc == 0 || purple_strequal(argv[0], "status")) {
		char *own = omemo_account_fingerprint(oa);
		char *bare = omemo_bare_jid(name);
		char *devs = is_chat ? NULL : omemo_describe_devices(account, bare, FALSE);

		msg = g_strdup_printf(_("OMEMO is %s for this conversation.\n"
		                        "This device: %u, fingerprint %s\n%s"),
		                      omemo_conv_enabled(account, name, is_chat) ?
		                      _("enabled") : _("disabled"),
		                      oa->device_id, own ? own : "?",
		                      devs ? devs : "");
		g_free(own);
		g_free(bare);
		g_free(devs);
	} else if (purple_strequal(argv[0], "on") ||
	           purple_strequal(argv[0], "off")) {
		omemo_conv_set_enabled(account, name, is_chat,
		                       purple_strequal(argv[0], "on"));
	} else if (purple_strequal(argv[0], "own")) {
		char *devs = omemo_describe_devices(account, oa->jid, FALSE);

		msg = g_strdup_printf(_("Devices of %s:\n%s"), oa->jid, devs);
		g_free(devs);
	} else if (purple_strequal(argv[0], "refresh")) {
		PurpleConnection *gc = purple_conversation_get_gc(conv);
		GList *targets = omemo_conv_targets(oa, account, name, is_chat), *l;

		g_hash_table_remove_all(omemo_failed);
		for (l = targets; gc && l; l = l->next)
			omemo_fetch_devicelist(gc, l->data, NULL, NULL);
		g_list_free_full(targets, g_free);
		msg = g_strdup(_("Refreshing OMEMO device lists."));
	} else if (purple_strequal(argv[0], "trust") && (argc == 3 || argc == 4)) {
		const char *jid = argc == 4 ? argv[1] : name;
		const char *dev = argv[argc - 2], *state = argv[argc - 1];
		guint64 id = g_ascii_strtoull(dev, NULL, 10);
		char *bare = omemo_bare_jid(jid);

		if (argc == 3 && is_chat) {
			msg = g_strdup(_("In a room, use /omemo trust <jid> <device> "
			                 "<state>."));
		} else if (!omemo_ipc_set_trust(account, bare, (guint)id, state)) {
			msg = g_strdup_printf(_("Cannot set %s:%s to '%s' (unknown device, "
			                        "or state not one of undecided, trusted, "
			                        "verified, untrusted)."), bare, dev, state);
		} else {
			msg = g_strdup_printf(_("%s:%s is now %s."), bare, dev, state);
		}
		g_free(bare);
	} else {
		g_strfreev(argv);
		*error = g_strdup(_("Usage: /omemo [status|on|off|own|refresh|"
		                    "trust [jid] <device> <undecided|trusted|verified|"
		                    "untrusted>]"));
		return PURPLE_CMD_RET_FAILED;
	}

	if (msg) {
		char *escaped = g_markup_escape_text(msg, -1);
		char *html = purple_strdup_withhtml(escaped);

		purple_conversation_write(conv, NULL, html,
		                          PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG |
		                          PURPLE_MESSAGE_NO_LINKIFY, time(NULL));
		g_free(html);
		g_free(escaped);
		g_free(msg);
	}
	g_strfreev(argv);
	return PURPLE_CMD_RET_OK;
}

typedef struct {
	PurpleAccount *account;
	char *jid;
} OmemoTrustRequest;

static void
omemo_trust_request_free(OmemoTrustRequest *req)
{
	g_free(req->jid);
	g_free(req);
}

static const char *omemo_trust_choices[] = {
	"undecided", "trusted", "verified", "untrusted"
};

static void
omemo_trust_request_ok(OmemoTrustRequest *req, PurpleRequestFields *fields)
{
	const char *dev = purple_request_fields_get_string(fields, "device");
	int choice = purple_request_fields_get_choice(fields, "trust");
	guint64 id = dev ? g_ascii_strtoull(dev, NULL, 10) : 0;

	if (choice >= 0 && choice < (int)G_N_ELEMENTS(omemo_trust_choices) &&
	    !omemo_ipc_set_trust(req->account, req->jid, (guint)id,
	                         omemo_trust_choices[choice]))
		purple_notify_error(omemo_plugin, _("OMEMO"),
		                    _("Cannot change the trust of that device."),
		                    _("Enter one of the device ids listed."));
	omemo_trust_request_free(req);
}

static void
omemo_trust_request_cancel(OmemoTrustRequest *req, PurpleRequestFields *fields)
{
	omemo_trust_request_free(req);
}

static void
omemo_fingerprints_ok(gpointer unused, PurpleRequestFields *fields)
{
	PurpleAccount *account = purple_request_fields_get_account(fields,
	                                                           "account");
	const char *jid = purple_request_fields_get_string(fields, "jid");
	OmemoAccount *oa = omemo_account_get(account);
	PurpleRequestFields *tfields;
	PurpleRequestFieldGroup *group;
	PurpleRequestField *field;
	OmemoTrustRequest *req;
	char *bare, *devs, *own, *html, *title;

	if (oa == NULL)
		return;
	bare = (jid && *jid) ? omemo_bare_jid(jid) : g_strdup(oa->jid);
	devs = omemo_describe_devices(account, bare, TRUE);
	own = omemo_account_fingerprint(oa);
	{
		char *esc_own = g_markup_escape_text(oa->jid, -1);
		char *esc_bare = g_markup_escape_text(bare, -1);

		html = g_strdup_printf(_("<b>This device</b> (%s): %u<br><tt>%s</tt>"
		                         "<br><br><b>%s</b>:<br>%s"), esc_own,
		                       oa->device_id, own ? own : "?", esc_bare, devs);
		g_free(esc_own);
		g_free(esc_bare);
	}
	title = g_strdup_printf(_("OMEMO fingerprints of %s"), bare);
	purple_notify_formatted(omemo_plugin, _("OMEMO"), title, NULL, html,
	                        NULL, NULL);

	req = g_new0(OmemoTrustRequest, 1);
	req->account = account;
	req->jid = g_strdup(bare);

	tfields = purple_request_fields_new();
	group = purple_request_field_group_new(NULL);
	purple_request_fields_add_group(tfields, group);
	field = purple_request_field_string_new("device", _("Device id"), NULL,
	                                        FALSE);
	purple_request_field_set_required(field, TRUE);
	purple_request_field_group_add_field(group, field);
	field = purple_request_field_choice_new("trust", _("Trust"), 1);
	purple_request_field_choice_add(field, _("Undecided"));
	purple_request_field_choice_add(field, _("Trusted"));
	purple_request_field_choice_add(field, _("Verified"));
	purple_request_field_choice_add(field, _("Untrusted (never encrypt to it)"));
	purple_request_field_group_add_field(group, field);

	purple_request_fields(omemo_plugin, _("OMEMO"), title,
	                      _("Compare the fingerprint with the one the contact's "
	                        "device shows before marking it verified."),
	                      tfields, _("Set trust"),
	                      G_CALLBACK(omemo_trust_request_ok), _("Close"),
	                      G_CALLBACK(omemo_trust_request_cancel),
	                      account, bare, NULL, req);

	g_free(title);
	g_free(html);
	g_free(own);
	g_free(devs);
	g_free(bare);
}

static gboolean
omemo_account_filter(PurpleAccount *account)
{
	return omemo_is_xmpp(account);
}

static void
omemo_action_fingerprints(PurplePluginAction *action)
{
	PurpleRequestFields *fields = purple_request_fields_new();
	PurpleRequestFieldGroup *group = purple_request_field_group_new(NULL);
	PurpleRequestField *field;

	purple_request_fields_add_group(fields, group);
	field = purple_request_field_account_new("account", _("Account"), NULL);
	purple_request_field_account_set_show_all(field, TRUE);
	purple_request_field_account_set_filter(field, omemo_account_filter);
	purple_request_field_set_required(field, TRUE);
	purple_request_field_group_add_field(group, field);
	field = purple_request_field_string_new("jid",
		_("Contact JID (empty for your own devices)"), NULL, FALSE);
	purple_request_field_group_add_field(group, field);

	purple_request_fields(omemo_plugin, _("OMEMO fingerprints"),
	                      _("Show OMEMO fingerprints"), NULL, fields,
	                      _("Show"), G_CALLBACK(omemo_fingerprints_ok),
	                      _("Cancel"), NULL, NULL, NULL, NULL, NULL);
}

static GList *
omemo_actions(PurplePlugin *plugin, gpointer context)
{
	return g_list_append(NULL, purple_plugin_action_new(
		_("OMEMO fingerprints..."), omemo_action_fingerprints));
}

/**************************************************************************
 * Plugin
 **************************************************************************/

static gboolean
plugin_load(PurplePlugin *plugin)
{
	GError *error = NULL;
	char *path;
	GList *l;
	gboolean ok;

	jabber_prpl = purple_find_prpl("prpl-jabber");
	if (jabber_prpl == NULL) {
		purple_debug_error("omemo", "the XMPP protocol plugin is not loaded\n");
		return FALSE;
	}
	if (!omemo_crypto_init(&error)) {
		purple_debug_error("omemo", "%s\n", error->message);
		g_error_free(error);
		return FALSE;
	}

	path = g_build_filename(purple_user_dir(), "pidgin4", "omemo.db", NULL);
	omemo_store = omemo_store_open(path, &error);
	g_free(path);
	if (omemo_store == NULL) {
		purple_debug_error("omemo", "%s\n", error->message);
		g_error_free(error);
		omemo_crypto_uninit();
		return FALSE;
	}

	omemo_plugin = plugin;
	omemo_accounts = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
	                                       (GDestroyNotify)omemo_account_free);
	omemo_iqs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	omemo_overrides = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                        NULL);
	omemo_mucs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                   (GDestroyNotify)omemo_muc_free);
	omemo_preps = g_hash_table_new(g_str_hash, g_str_equal);
	omemo_failed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	omemo_sent = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	omemo_sent_order = g_queue_new();
	omemo_replenish = g_hash_table_new(g_direct_hash, g_direct_equal);

	omemo_register_ipc(plugin);

	purple_signal_connect(jabber_prpl, "jabber-receiving-xmlnode", plugin,
	                      PURPLE_CALLBACK(omemo_receiving_xmlnode_cb), NULL);
	purple_signal_connect(jabber_prpl, "jabber-sending-xmlnode", plugin,
	                      PURPLE_CALLBACK(omemo_sending_xmlnode_cb), NULL);
	purple_signal_connect(jabber_prpl, "jabber-receiving-iq", plugin,
	                      PURPLE_CALLBACK(omemo_receiving_iq_cb), NULL);
	purple_signal_connect(purple_connections_get_handle(), "signed-on", plugin,
	                      PURPLE_CALLBACK(omemo_signed_on_cb), NULL);
	purple_signal_connect(purple_connections_get_handle(), "signing-off",
	                      plugin, PURPLE_CALLBACK(omemo_signing_off_cb), NULL);
	purple_signal_connect(purple_accounts_get_handle(), "account-removed",
	                      plugin, PURPLE_CALLBACK(omemo_account_removed_cb),
	                      NULL);
	/* Last, so we hold back the final text other plugins produced. */
	purple_signal_connect_priority(purple_conversations_get_handle(),
	                               "sending-im-msg", plugin,
	                               PURPLE_CALLBACK(omemo_sending_im_cb), NULL,
	                               PURPLE_SIGNAL_PRIORITY_HIGHEST);
	purple_signal_connect_priority(purple_conversations_get_handle(),
	                               "sending-chat-msg", plugin,
	                               PURPLE_CALLBACK(omemo_sending_chat_cb), NULL,
	                               PURPLE_SIGNAL_PRIORITY_HIGHEST);
	purple_signal_connect(purple_conversations_get_handle(),
	                      "conversation-created", plugin,
	                      PURPLE_CALLBACK(omemo_conversation_created_cb), NULL);

	/* Subscribe to device lists through entity caps (XEP-0163 +notify). */
	purple_plugin_ipc_call(jabber_prpl, "add_feature", &ok, OMEMO_NS_NOTIFY);
	if (!ok)
		purple_debug_warning("omemo", "cannot add the +notify feature\n");

	omemo_cmd = purple_cmd_register("omemo", "s", PURPLE_CMD_P_PLUGIN,
		PURPLE_CMD_FLAG_IM | PURPLE_CMD_FLAG_CHAT | PURPLE_CMD_FLAG_PRPL_ONLY |
		PURPLE_CMD_FLAG_ALLOW_WRONG_ARGS, "prpl-jabber", omemo_cmd_cb,
		_("omemo [status|on|off|own|refresh|trust [jid] &lt;device&gt; "
		  "&lt;state&gt;]: OMEMO encryption for this conversation."),
		NULL);

	for (l = purple_connections_get_all(); l; l = l->next) {
		PurpleConnection *gc = l->data;

		if (purple_connection_get_state(gc) == PURPLE_CONNECTED &&
		    omemo_is_xmpp(purple_connection_get_account(gc)))
			omemo_account_setup(gc);
	}

	return TRUE;
}

static gboolean
plugin_unload(PurplePlugin *plugin)
{
	GList *l;

	for (l = purple_connections_get_all(); l; l = l->next)
		omemo_signing_off_cb(l->data, NULL);

	purple_cmd_unregister(omemo_cmd);
	omemo_cmd = 0;
	purple_signals_disconnect_by_handle(plugin);
	purple_signals_unregister_by_instance(plugin);
	purple_plugin_ipc_unregister_all(plugin);

	g_hash_table_destroy(omemo_replenish);
	g_hash_table_destroy(omemo_iqs);
	g_hash_table_destroy(omemo_overrides);
	g_hash_table_destroy(omemo_mucs);
	g_hash_table_destroy(omemo_preps);
	g_hash_table_destroy(omemo_failed);
	g_hash_table_destroy(omemo_sent);
	g_queue_free(omemo_sent_order);
	g_hash_table_destroy(omemo_accounts);
	omemo_replenish = omemo_iqs = omemo_overrides = omemo_mucs = NULL;
	omemo_preps = omemo_failed = omemo_sent = omemo_accounts = NULL;
	omemo_sent_order = NULL;

	omemo_store_close(omemo_store);
	omemo_store = NULL;
	omemo_crypto_uninit();
	omemo_plugin = NULL;
	jabber_prpl = NULL;
	return TRUE;
}

static PurplePluginPrefFrame *
get_plugin_pref_frame(PurplePlugin *plugin)
{
	PurplePluginPrefFrame *frame = purple_plugin_pref_frame_new();
	PurplePluginPref *pref;

	pref = purple_plugin_pref_new_with_name_and_label(PREF_DEFAULT_ON,
		_("Encrypt new conversations by default when the contact supports "
		  "OMEMO"));
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(PREF_TOFU,
		_("Trust new devices on first use (until a device of that contact "
		  "has been verified)"));
	purple_plugin_pref_frame_add(frame, pref);

	return frame;
}

static PurplePluginUiInfo prefs_info = {
	get_plugin_pref_frame,
	0,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};

static PurplePluginInfo info = {
	PURPLE_PLUGIN_MAGIC,
	PURPLE_MAJOR_VERSION,
	PURPLE_MINOR_VERSION,
	PURPLE_PLUGIN_STANDARD,
	NULL,
	0,
	NULL,
	PURPLE_PRIORITY_DEFAULT,

	OMEMO_PLUGIN_ID,
	N_("OMEMO"),
	DISPLAY_VERSION,
	N_("End-to-end encryption for XMPP (OMEMO)."),
	N_("OMEMO (XEP-0384, legacy eu.siacs.conversations.axolotl namespace) "
	   "end-to-end encryption for one-to-one chats and members-only, "
	   "non-anonymous rooms, compatible with Conversations, Dino and Gajim. "
	   "State is kept in pidgin4/omemo.db in the profile. Use /omemo in a "
	   "conversation."),
	"Pidgin4",
	PURPLE_WEBSITE,

	plugin_load,
	plugin_unload,
	NULL,

	NULL,
	NULL,
	&prefs_info,
	omemo_actions,

	NULL,
	NULL,
	NULL,
	NULL
};

static void
init_plugin(PurplePlugin *plugin)
{
	info.dependencies = g_list_append(info.dependencies, "prpl-jabber");

	purple_prefs_add_none("/plugins/core");
	purple_prefs_add_none(PREF_ROOT);
	purple_prefs_add_bool(PREF_TOFU, TRUE);
	purple_prefs_add_bool(PREF_DEFAULT_ON, FALSE);
}

PURPLE_INIT_PLUGIN(omemo, init_plugin, info)
