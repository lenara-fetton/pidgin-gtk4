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
#include "server.h"
#include "util.h"
#include "value.h"

#include "bookmarks.h"
#include "chat.h"
#include "iq.h"
#include "pep.h"

/*
 * Room bookmarks.
 *
 * XEP-0402 keeps one PEP item per room in urn:xmpp:bookmarks:1 (the item
 * id is the room JID).  Older accounts keep a single XEP-0048
 * <storage xmlns='storage:bookmarks'/> item in the storage:bookmarks PEP
 * node; servers with urn:xmpp:bookmarks:1#compat sync the two.  We read
 * the native node first and fall back to the legacy one, and write back in
 * whichever format we found.
 *
 * After connecting, rooms bookmarked with autojoin are joined with the
 * stored nick and password (unless the account setting
 * "bookmarks_autojoin" is FALSE).  Bookmarks added by other clients arrive
 * as PEP notifications and are joined the same way.
 *
 * The UI can add and remove bookmarks through the "bookmark-add" and
 * "bookmark-remove" IPC commands.
 */

typedef enum {
	BOOKMARKS_UNKNOWN,
	BOOKMARKS_NATIVE,  /* XEP-0402 */
	BOOKMARKS_LEGACY   /* XEP-0048 in PEP */
} JabberBookmarksMode;

typedef struct {
	JabberBookmarksMode mode;
	gboolean compat;
	gboolean native_node_missing;
	GList *list;          /* JabberBookmark* */
	guint autojoin_timer;
	guint autojoin_tries;
} JabberBookmarks;

#define AUTOJOIN_RETRY_SECONDS 2
#define AUTOJOIN_MAX_TRIES     30

void
jabber_bookmark_free(JabberBookmark *bookmark)
{
	if (bookmark == NULL)
		return;

	g_free(bookmark->jid);
	g_free(bookmark->name);
	g_free(bookmark->nick);
	g_free(bookmark->password);
	g_free(bookmark);
}

static void
bookmarks_list_free(GList *list)
{
	g_list_free_full(list, (GDestroyNotify)jabber_bookmark_free);
}

/**************************************************************************
 * (De)serialization
 **************************************************************************/

static gboolean
parse_bool(const char *value)
{
	return purple_strequal(value, "true") || purple_strequal(value, "1");
}

static char *
child_data(xmlnode *parent, const char *name)
{
	xmlnode *child = xmlnode_get_child(parent, name);
	char *data = child ? xmlnode_get_data(child) : NULL;

	if (data && *data == '\0') {
		g_free(data);
		data = NULL;
	}
	return data;
}

static char *
normalize_room_jid(const char *jid_str)
{
	JabberID *jid = jid_str ? jabber_id_new(jid_str) : NULL;
	char *bare;

	if (jid == NULL || jid->node == NULL) {
		jabber_id_free(jid);
		return NULL;
	}

	bare = jabber_id_get_bare_jid(jid);
	jabber_id_free(jid);
	return bare;
}

JabberBookmark *
jabber_bookmark_parse_item(xmlnode *item)
{
	JabberBookmark *bookmark;
	xmlnode *conference;
	char *jid;

	if (item == NULL)
		return NULL;

	conference = xmlnode_get_child_with_namespace(item, "conference", NS_BOOKMARKS2);
	jid = normalize_room_jid(xmlnode_get_attrib(item, "id"));
	if (conference == NULL || jid == NULL) {
		g_free(jid);
		return NULL;
	}

	bookmark = g_new0(JabberBookmark, 1);
	bookmark->jid = jid;
	bookmark->name = g_strdup(xmlnode_get_attrib(conference, "name"));
	bookmark->autojoin = parse_bool(xmlnode_get_attrib(conference, "autojoin"));
	bookmark->nick = child_data(conference, "nick");
	bookmark->password = child_data(conference, "password");

	return bookmark;
}

GList *
jabber_bookmarks_parse_storage(xmlnode *storage)
{
	GList *list = NULL;
	xmlnode *conference;

	if (storage == NULL)
		return NULL;

	for (conference = xmlnode_get_child(storage, "conference"); conference;
	     conference = xmlnode_get_next_twin(conference)) {
		JabberBookmark *bookmark;
		char *jid = normalize_room_jid(xmlnode_get_attrib(conference, "jid"));

		if (jid == NULL)
			continue;

		bookmark = g_new0(JabberBookmark, 1);
		bookmark->jid = jid;
		bookmark->name = g_strdup(xmlnode_get_attrib(conference, "name"));
		bookmark->autojoin = parse_bool(xmlnode_get_attrib(conference, "autojoin"));
		bookmark->nick = child_data(conference, "nick");
		bookmark->password = child_data(conference, "password");
		list = g_list_append(list, bookmark);
	}

	return list;
}

static void
add_text_child(xmlnode *parent, const char *name, const char *text)
{
	if (text && *text) {
		xmlnode *child = xmlnode_new_child(parent, name);
		xmlnode_insert_data(child, text, -1);
	}
}

xmlnode *
jabber_bookmark_to_conference(const JabberBookmark *bookmark)
{
	xmlnode *conference = xmlnode_new("conference");

	xmlnode_set_namespace(conference, NS_BOOKMARKS2);
	if (bookmark->name && *bookmark->name)
		xmlnode_set_attrib(conference, "name", bookmark->name);
	xmlnode_set_attrib(conference, "autojoin",
	                   bookmark->autojoin ? "true" : "false");
	add_text_child(conference, "nick", bookmark->nick);
	add_text_child(conference, "password", bookmark->password);

	return conference;
}

xmlnode *
jabber_bookmarks_to_storage(GList *bookmarks)
{
	xmlnode *storage = xmlnode_new("storage");

	xmlnode_set_namespace(storage, NS_BOOKMARKS_LEGACY);
	for (; bookmarks; bookmarks = bookmarks->next) {
		JabberBookmark *bookmark = bookmarks->data;
		xmlnode *conference = xmlnode_new_child(storage, "conference");

		xmlnode_set_attrib(conference, "jid", bookmark->jid);
		if (bookmark->name && *bookmark->name)
			xmlnode_set_attrib(conference, "name", bookmark->name);
		xmlnode_set_attrib(conference, "autojoin",
		                   bookmark->autojoin ? "true" : "false");
		add_text_child(conference, "nick", bookmark->nick);
		add_text_child(conference, "password", bookmark->password);
	}

	return storage;
}

/**************************************************************************
 * State, autojoin
 **************************************************************************/

static JabberBookmarks *
bookmarks_get(JabberStream *js)
{
	if (js->bookmarks == NULL)
		js->bookmarks = g_new0(JabberBookmarks, 1);
	return js->bookmarks;
}

static JabberBookmark *
bookmarks_find(JabberBookmarks *bm, const char *jid)
{
	GList *l;

	for (l = bm->list; l; l = l->next) {
		JabberBookmark *bookmark = l->data;
		if (g_ascii_strcasecmp(bookmark->jid, jid) == 0)
			return bookmark;
	}
	return NULL;
}

static void
bookmark_autojoin(JabberStream *js, JabberBookmark *bookmark)
{
	PurpleAccount *account = purple_connection_get_account(js->gc);
	GHashTable *components;
	JabberID *jid;

	if (!bookmark->autojoin ||
	    !purple_account_get_bool(account, "bookmarks_autojoin", TRUE))
		return;

	jid = jabber_id_new(bookmark->jid);
	if (jid == NULL || jid->node == NULL) {
		jabber_id_free(jid);
		return;
	}

	if (jabber_chat_find(js, jid->node, jid->domain) == NULL) {
		purple_debug_info("jabber", "Joining bookmarked room %s\n",
		                  bookmark->jid);

		components = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
		g_hash_table_insert(components, g_strdup("room"), g_strdup(jid->node));
		g_hash_table_insert(components, g_strdup("server"), g_strdup(jid->domain));
		g_hash_table_insert(components, g_strdup("handle"),
				g_strdup(bookmark->nick ? bookmark->nick : js->user->node));
		if (bookmark->password)
			g_hash_table_insert(components, g_strdup("password"),
					g_strdup(bookmark->password));

		serv_join_chat(js->gc, components);
		g_hash_table_destroy(components);
	}

	jabber_id_free(jid);
}

static gboolean
bookmarks_autojoin_cb(gpointer data)
{
	JabberStream *js = data;
	JabberBookmarks *bm = bookmarks_get(js);
	GList *l;

	/* Join after the initial presence went out. */
	if (js->state != JABBER_STREAM_CONNECTED &&
	    ++bm->autojoin_tries < AUTOJOIN_MAX_TRIES)
		return TRUE;

	bm->autojoin_timer = 0;
	for (l = bm->list; l; l = l->next)
		bookmark_autojoin(js, l->data);

	return FALSE;
}

static void
bookmarks_apply(JabberStream *js, JabberBookmarksMode mode, GList *list)
{
	JabberBookmarks *bm = bookmarks_get(js);

	bookmarks_list_free(bm->list);
	bm->list = list;
	bm->mode = mode;

	purple_debug_info("jabber", "%u bookmark(s) (%s)\n", g_list_length(list),
	                  mode == BOOKMARKS_NATIVE ? "XEP-0402" : "XEP-0048");

	if (js->state == JABBER_STREAM_CONNECTED) {
		if (bm->autojoin_timer)
			purple_timeout_remove(bm->autojoin_timer);
		bookmarks_autojoin_cb(js);
	} else if (bm->autojoin_timer == 0) {
		bm->autojoin_tries = 0;
		bm->autojoin_timer = purple_timeout_add_seconds(AUTOJOIN_RETRY_SECONDS,
				bookmarks_autojoin_cb, js);
	}
}

/**************************************************************************
 * Fetching
 **************************************************************************/

static void
bookmarks_request(JabberStream *js, const char *node, JabberIqCallback *cb)
{
	JabberIq *iq = jabber_iq_new(js, JABBER_IQ_GET);
	xmlnode *pubsub, *items;

	pubsub = xmlnode_new_child(iq->node, "pubsub");
	xmlnode_set_namespace(pubsub, NS_PUBSUB);
	items = xmlnode_new_child(pubsub, "items");
	xmlnode_set_attrib(items, "node", node);

	jabber_iq_set_callback(iq, cb, NULL);
	jabber_iq_send(iq);
}

static xmlnode *
pubsub_items(xmlnode *packet)
{
	xmlnode *pubsub = xmlnode_get_child_with_namespace(packet, "pubsub", NS_PUBSUB);
	return pubsub ? xmlnode_get_child(pubsub, "items") : NULL;
}

static void
bookmarks_legacy_cb(JabberStream *js, const char *from, JabberIqType type,
                    const char *id, xmlnode *packet, gpointer data)
{
	JabberBookmarks *bm = bookmarks_get(js);
	GList *list = NULL;
	xmlnode *items = (type == JABBER_IQ_RESULT) ? pubsub_items(packet) : NULL;
	xmlnode *item;

	for (item = items ? xmlnode_get_child(items, "item") : NULL; item;
	     item = xmlnode_get_next_twin(item)) {
		xmlnode *storage = xmlnode_get_child_with_namespace(item, "storage",
		                                                    NS_BOOKMARKS_LEGACY);
		if (storage) {
			list = jabber_bookmarks_parse_storage(storage);
			break;
		}
	}

	if (list) {
		bookmarks_apply(js, BOOKMARKS_LEGACY, list);
	} else if (bm->native_node_missing) {
		/* No bookmarks anywhere yet: new ones go to the native node. */
		bookmarks_apply(js, BOOKMARKS_NATIVE, NULL);
	} else {
		/* The native node is unusable here. */
		bookmarks_apply(js, BOOKMARKS_LEGACY, NULL);
	}
}

static void
bookmarks_native_cb(JabberStream *js, const char *from, JabberIqType type,
                    const char *id, xmlnode *packet, gpointer data)
{
	JabberBookmarks *bm = bookmarks_get(js);
	GList *list = NULL;
	xmlnode *items, *item;

	if (type == JABBER_IQ_ERROR) {
		xmlnode *error = xmlnode_get_child(packet, "error");
		bm->native_node_missing = error && xmlnode_get_child_with_namespace(
				error, "item-not-found", NS_XMPP_STANZAS);
		bookmarks_request(js, NS_BOOKMARKS_LEGACY, bookmarks_legacy_cb);
		return;
	}

	items = pubsub_items(packet);
	for (item = items ? xmlnode_get_child(items, "item") : NULL; item;
	     item = xmlnode_get_next_twin(item)) {
		JabberBookmark *bookmark = jabber_bookmark_parse_item(item);
		if (bookmark)
			list = g_list_append(list, bookmark);
	}

	if (list == NULL && !bm->compat) {
		/* Maybe this account still keeps them the old way. */
		bm->native_node_missing = TRUE;
		bookmarks_request(js, NS_BOOKMARKS_LEGACY, bookmarks_legacy_cb);
		return;
	}

	bookmarks_apply(js, BOOKMARKS_NATIVE, list);
}

void
jabber_bookmarks_fetch(JabberStream *js)
{
	if (!js->pep) {
		purple_debug_info("jabber", "No PEP, so no bookmarks\n");
		return;
	}

	bookmarks_request(js, NS_BOOKMARKS2, bookmarks_native_cb);
}

void
jabber_bookmarks_set_compat(JabberStream *js, gboolean compat)
{
	bookmarks_get(js)->compat = compat;
}

void
jabber_bookmarks_close(JabberStream *js)
{
	JabberBookmarks *bm = js->bookmarks;

	if (bm == NULL)
		return;

	if (bm->autojoin_timer)
		purple_timeout_remove(bm->autojoin_timer);
	bookmarks_list_free(bm->list);
	g_free(bm);
	js->bookmarks = NULL;
}

/**************************************************************************
 * PEP notifications (other clients changing bookmarks)
 **************************************************************************/

static void
bookmarks_native_event(JabberStream *js, const char *from, xmlnode *items)
{
	JabberBookmarks *bm;
	xmlnode *child;

	if (!jabber_is_own_account(js, from))
		return;

	bm = bookmarks_get(js);
	if (bm->mode == BOOKMARKS_UNKNOWN)
		bm->mode = BOOKMARKS_NATIVE;

	for (child = items ? items->child : NULL; child; child = child->next) {
		if (child->type != XMLNODE_TYPE_TAG)
			continue;

		if (purple_strequal(child->name, "item")) {
			JabberBookmark *bookmark = jabber_bookmark_parse_item(child);
			JabberBookmark *old;

			if (bookmark == NULL)
				continue;

			old = bookmarks_find(bm, bookmark->jid);
			if (old) {
				bm->list = g_list_remove(bm->list, old);
				jabber_bookmark_free(old);
			}
			bm->list = g_list_append(bm->list, bookmark);
			if (js->state == JABBER_STREAM_CONNECTED)
				bookmark_autojoin(js, bookmark);
		} else if (purple_strequal(child->name, "retract")) {
			char *jid = normalize_room_jid(xmlnode_get_attrib(child, "id"));
			JabberBookmark *old = jid ? bookmarks_find(bm, jid) : NULL;

			if (old) {
				bm->list = g_list_remove(bm->list, old);
				jabber_bookmark_free(old);
			}
			g_free(jid);
		}
	}
}

static void
bookmarks_legacy_event(JabberStream *js, const char *from, xmlnode *items)
{
	JabberBookmarks *bm;
	xmlnode *item, *storage;
	GList *l;

	if (!jabber_is_own_account(js, from))
		return;

	bm = bookmarks_get(js);
	/* With #compat the native notification carries the same change. */
	if (bm->mode == BOOKMARKS_NATIVE)
		return;

	item = items ? xmlnode_get_child(items, "item") : NULL;
	storage = item ? xmlnode_get_child_with_namespace(item, "storage",
	                                                  NS_BOOKMARKS_LEGACY) : NULL;
	if (storage == NULL)
		return;

	bookmarks_list_free(bm->list);
	bm->list = jabber_bookmarks_parse_storage(storage);
	bm->mode = BOOKMARKS_LEGACY;

	if (js->state == JABBER_STREAM_CONNECTED)
		for (l = bm->list; l; l = l->next)
			bookmark_autojoin(js, l->data);
}

void
jabber_bookmarks_pep_init(void)
{
	jabber_pep_register_handler(NS_BOOKMARKS2, bookmarks_native_event);
	jabber_pep_register_handler(NS_BOOKMARKS_LEGACY, bookmarks_legacy_event);
}

/**************************************************************************
 * Publishing, IPC
 **************************************************************************/

static xmlnode *
publish_options(gboolean native)
{
	xmlnode *x = xmlnode_new("x");
	const char *fields[][2] = {
		{ "FORM_TYPE", NS_PUBSUB_PUBLISH_OPTIONS },
		{ "pubsub#persist_items", "true" },
		{ "pubsub#access_model", "whitelist" },
		{ "pubsub#send_last_published_item", "never" },
		{ "pubsub#max_items", "max" },
	};
	gsize i, n = native ? G_N_ELEMENTS(fields) : G_N_ELEMENTS(fields) - 1;

	xmlnode_set_namespace(x, NS_XDATA);
	xmlnode_set_attrib(x, "type", "submit");
	for (i = 0; i < n; i++) {
		xmlnode *field = xmlnode_new_child(x, "field");
		xmlnode *value = xmlnode_new_child(field, "value");

		xmlnode_set_attrib(field, "var", fields[i][0]);
		if (i == 0)
			xmlnode_set_attrib(field, "type", "hidden");
		xmlnode_insert_data(value, fields[i][1], -1);
	}

	return x;
}

static void
bookmarks_publish_legacy(JabberStream *js, JabberBookmarks *bm)
{
	xmlnode *publish = xmlnode_new("publish");
	xmlnode *item;

	xmlnode_set_attrib(publish, "node", NS_BOOKMARKS_LEGACY);
	item = xmlnode_new_child(publish, "item");
	xmlnode_set_attrib(item, "id", "current");
	xmlnode_insert_child(item, jabber_bookmarks_to_storage(bm->list));

	jabber_pep_publish_with_options(js, publish, publish_options(FALSE));
}

static void
bookmarks_publish_native(JabberStream *js, JabberBookmark *bookmark)
{
	xmlnode *publish = xmlnode_new("publish");
	xmlnode *item;

	xmlnode_set_attrib(publish, "node", NS_BOOKMARKS2);
	item = xmlnode_new_child(publish, "item");
	xmlnode_set_attrib(item, "id", bookmark->jid);
	xmlnode_insert_child(item, jabber_bookmark_to_conference(bookmark));

	jabber_pep_publish_with_options(js, publish, publish_options(TRUE));
}

static void
bookmarks_retract_native(JabberStream *js, const char *jid)
{
	JabberIq *iq = jabber_iq_new(js, JABBER_IQ_SET);
	xmlnode *pubsub, *retract, *item;

	pubsub = xmlnode_new_child(iq->node, "pubsub");
	xmlnode_set_namespace(pubsub, NS_PUBSUB);
	retract = xmlnode_new_child(pubsub, "retract");
	xmlnode_set_attrib(retract, "node", NS_BOOKMARKS2);
	xmlnode_set_attrib(retract, "notify", "true");
	item = xmlnode_new_child(retract, "item");
	xmlnode_set_attrib(item, "id", jid);
	jabber_iq_send(iq);
}

static JabberStream *
bookmarks_stream_for(PurpleAccount *account)
{
	PurpleConnection *gc;
	JabberStream *js;

	if (account == NULL ||
	    !purple_strequal(purple_account_get_protocol_id(account), "prpl-jabber"))
		return NULL;

	gc = purple_account_get_connection(account);
	if (gc == NULL || purple_connection_get_state(gc) != PURPLE_CONNECTED)
		return NULL;

	js = purple_connection_get_protocol_data(gc);
	return (js && js->pep) ? js : NULL;
}

static gboolean
jabber_bookmarks_ipc_add(PurpleAccount *account, const char *jid_str,
                         const char *nick, guint autojoin)
{
	JabberStream *js = bookmarks_stream_for(account);
	JabberBookmarks *bm;
	JabberBookmark *bookmark;
	char *jid;

	if (js == NULL || (jid = normalize_room_jid(jid_str)) == NULL)
		return FALSE;

	bm = bookmarks_get(js);
	bookmark = bookmarks_find(bm, jid);
	if (bookmark == NULL) {
		bookmark = g_new0(JabberBookmark, 1);
		bookmark->jid = jid;
		bm->list = g_list_append(bm->list, bookmark);
	} else {
		g_free(jid);
	}

	if (nick && *nick) {
		g_free(bookmark->nick);
		bookmark->nick = g_strdup(nick);
	}
	bookmark->autojoin = autojoin ? TRUE : FALSE;

	if (bm->mode == BOOKMARKS_LEGACY)
		bookmarks_publish_legacy(js, bm);
	else
		bookmarks_publish_native(js, bookmark);

	bookmark_autojoin(js, bookmark);
	return TRUE;
}

static gboolean
jabber_bookmarks_ipc_remove(PurpleAccount *account, const char *jid_str)
{
	JabberStream *js = bookmarks_stream_for(account);
	JabberBookmarks *bm;
	JabberBookmark *bookmark;
	char *jid;

	if (js == NULL || (jid = normalize_room_jid(jid_str)) == NULL)
		return FALSE;

	bm = bookmarks_get(js);
	bookmark = bookmarks_find(bm, jid);
	if (bookmark) {
		bm->list = g_list_remove(bm->list, bookmark);
		jabber_bookmark_free(bookmark);
	}

	if (bm->mode == BOOKMARKS_LEGACY)
		bookmarks_publish_legacy(js, bm);
	else
		bookmarks_retract_native(js, jid);

	g_free(jid);
	return TRUE;
}

void
jabber_bookmarks_init(PurplePlugin *plugin)
{
	/* gboolean (account, room jid, nick or NULL, autojoin) */
	purple_plugin_ipc_register(plugin, "bookmark-add",
			PURPLE_CALLBACK(jabber_bookmarks_ipc_add),
			purple_marshal_BOOLEAN__POINTER_POINTER_POINTER_UINT,
			purple_value_new(PURPLE_TYPE_BOOLEAN), 4,
			purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_BOOLEAN));

	/* gboolean (account, room jid) */
	purple_plugin_ipc_register(plugin, "bookmark-remove",
			PURPLE_CALLBACK(jabber_bookmarks_ipc_remove),
			purple_marshal_BOOLEAN__POINTER_POINTER,
			purple_value_new(PURPLE_TYPE_BOOLEAN), 2,
			purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
			purple_value_new(PURPLE_TYPE_STRING));
}
