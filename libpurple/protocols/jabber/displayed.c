/*
 * purple - Jabber Protocol Plugin
 *
 * XEP-0490 Message Displayed Synchronization (M8, doc/PIDGIN-UPGRADE.md).
 *
 * The last message displayed in each conversation lives in the private
 * PEP node urn:xmpp:mds:displayed:0, one item per conversation (item id =
 * the conversation's bare JID), holding the XEP-0359 stanza-id of that
 * message.  Notifications become message-receipt(..., "displayed", <our
 * bare JID>) so the UI clears unread state; the UI publishes through the
 * mds-publish IPC command.
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
#include "internal.h"

#include "debug.h"
#include "util.h"

#include "displayed.h"
#include "iq.h"
#include "message.h"
#include "pep.h"
#include "receipts.h"

static void
displayed_items(JabberStream *js, const char *from, xmlnode *items)
{
	char *own;
	xmlnode *item;

	if (items == NULL || !jabber_ui_supports_message_meta())
		return;

	own = jabber_message_own_jid(js, TRUE);

	/* Our own PEP node only. */
	if (from && g_ascii_strcasecmp(from, own) != 0) {
		purple_debug_warning("jabber", "Ignoring %s items from %s\n",
		                     NS_MDS, from);
		g_free(own);
		return;
	}

	for (item = xmlnode_get_child(items, "item"); item;
	     item = xmlnode_get_next_twin(item)) {
		const char *conv = xmlnode_get_attrib(item, "id");
		xmlnode *displayed = xmlnode_get_child_with_namespace(item,
				"displayed", NS_MDS);
		xmlnode *sid = displayed ? xmlnode_get_child_with_namespace(displayed,
				"stanza-id", NS_SID) : NULL;
		const char *id = sid ? xmlnode_get_attrib(sid, "id") : NULL;

		if (conv == NULL || *conv == '\0' || id == NULL || *id == '\0')
			continue;

		jabber_receipts_emit(js, conv, id, "displayed", own);
	}

	g_free(own);
}

void
jabber_displayed_pep_init(void)
{
	jabber_pep_register_handler(NS_MDS, displayed_items);
}

static void
fetch_result_cb(JabberStream *js, const char *from, JabberIqType type,
                const char *id, xmlnode *packet, gpointer data)
{
	xmlnode *pubsub, *items;
	char *own;

	if (type != JABBER_IQ_RESULT)
		return; /* no node yet: nothing was displayed anywhere */

	pubsub = xmlnode_get_child_with_namespace(packet, "pubsub",
			"http://jabber.org/protocol/pubsub");
	items = pubsub ? xmlnode_get_child(pubsub, "items") : NULL;
	if (items == NULL || !purple_strequal(xmlnode_get_attrib(items, "node"), NS_MDS))
		return;

	own = jabber_message_own_jid(js, TRUE);
	displayed_items(js, from ? from : own, items);
	g_free(own);
}

void
jabber_displayed_fetch(JabberStream *js)
{
	JabberIq *iq;
	xmlnode *pubsub, *items;

	if (!jabber_ui_supports_message_meta())
		return;

	iq = jabber_iq_new(js, JABBER_IQ_GET);
	pubsub = xmlnode_new_child(iq->node, "pubsub");
	xmlnode_set_namespace(pubsub, "http://jabber.org/protocol/pubsub");
	items = xmlnode_new_child(pubsub, "items");
	xmlnode_set_attrib(items, "node", NS_MDS);
	jabber_iq_set_callback(iq, fetch_result_cb, NULL);
	jabber_iq_send(iq);
}

xmlnode *
jabber_displayed_publish_node(const char *conv_jid, const char *stanza_id,
                              const char *by)
{
	xmlnode *publish, *item, *displayed, *sid;

	publish = xmlnode_new("publish");
	xmlnode_set_attrib(publish, "node", NS_MDS);
	item = xmlnode_new_child(publish, "item");
	xmlnode_set_attrib(item, "id", conv_jid);
	displayed = xmlnode_new_child(item, "displayed");
	xmlnode_set_namespace(displayed, NS_MDS);
	sid = xmlnode_new_child(displayed, "stanza-id");
	xmlnode_set_namespace(sid, NS_SID);
	xmlnode_set_attrib(sid, "id", stanza_id);
	xmlnode_set_attrib(sid, "by", by);

	return publish;
}

static xmlnode *
publish_options(void)
{
	/* XEP-0490 §4: private, persistent, one item per conversation */
	const char *fields[][2] = {
		{ "FORM_TYPE", NS_PUBSUB_PUBLISH_OPTIONS },
		{ "pubsub#persist_items", "true" },
		{ "pubsub#max_items", "max" },
		{ "pubsub#send_last_published_item", "never" },
		{ "pubsub#access_model", "whitelist" },
	};
	xmlnode *x = xmlnode_new("x");
	gsize i;

	xmlnode_set_namespace(x, NS_XDATA);
	xmlnode_set_attrib(x, "type", "submit");
	for (i = 0; i < G_N_ELEMENTS(fields); i++) {
		xmlnode *field = xmlnode_new_child(x, "field");
		xmlnode *value = xmlnode_new_child(field, "value");

		xmlnode_set_attrib(field, "var", fields[i][0]);
		if (i == 0)
			xmlnode_set_attrib(field, "type", "hidden");
		xmlnode_insert_data(value, fields[i][1], -1);
	}

	return x;
}

/**************************************************************************
 * IPC: mds-publish
 **************************************************************************/

static gboolean
jabber_displayed_ipc_publish(PurpleAccount *account, const char *conv_name,
                             const char *stanza_id)
{
	JabberIpcTarget t;
	char *conv = NULL, *by = NULL;
	gboolean ok = FALSE;

	if (stanza_id == NULL || *stanza_id == '\0')
		return FALSE;

	if (!jabber_ipc_target_init(&t, account, conv_name) || !t.js->pep)
		goto out;

	if (t.groupchat) {
		/* The room's archive assigned the stanza-id. */
		conv = g_strdup(t.to);
		by = g_strdup(t.to);
	} else {
		conv = jabber_get_bare_jid(t.to);
		by = jabber_message_own_jid(t.js, TRUE);
	}
	if (conv == NULL || by == NULL)
		goto out;

	jabber_pep_publish_with_options(t.js,
			jabber_displayed_publish_node(conv, stanza_id, by),
			publish_options());
	ok = TRUE;

out:
	g_free(conv);
	g_free(by);
	jabber_ipc_target_clear(&t);
	return ok;
}

/* The connection signals are registered after prpls are probed (see
 * httpupload.c); connect from the event loop. */
static guint connect_timer = 0;
static int handle;

static void
signed_on_cb(PurpleConnection *gc, gpointer data)
{
	PurpleAccount *account = purple_connection_get_account(gc);

	if (!purple_strequal(purple_account_get_protocol_id(account), "prpl-jabber"))
		return;

	jabber_displayed_fetch(purple_connection_get_protocol_data(gc));
}

static gboolean
connect_signals_cb(gpointer data)
{
	connect_timer = 0;
	purple_signal_connect(purple_connections_get_handle(), "signed-on",
	                      &handle, PURPLE_CALLBACK(signed_on_cb), NULL);
	return FALSE;
}

void
jabber_displayed_init(PurplePlugin *plugin)
{
	/* gboolean (account, conv name, stanza-id of the last displayed message) */
	purple_plugin_ipc_register(plugin, "mds-publish",
			PURPLE_CALLBACK(jabber_displayed_ipc_publish),
			purple_marshal_BOOLEAN__POINTER_POINTER_POINTER,
			purple_value_new(PURPLE_TYPE_BOOLEAN), 3,
			purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING));

	if (connect_timer == 0)
		connect_timer = purple_timeout_add(0, connect_signals_cb, NULL);
}

void
jabber_displayed_uninit(void)
{
	if (connect_timer) {
		purple_timeout_remove(connect_timer);
		connect_timer = 0;
	}
	purple_signals_disconnect_by_handle(&handle);
}
