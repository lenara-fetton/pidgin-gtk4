/*
 * purple - Jabber Protocol Plugin
 *
 * XEP-0184 Message Delivery Receipts and XEP-0333 Chat Markers (M8,
 * doc/PIDGIN-UPGRADE.md).
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

#include "blist.h"
#include "conversation.h"
#include "debug.h"
#include "signals.h"
#include "util.h"

#include "jabber.h"
#include "message.h"
#include "receipts.h"

gboolean
jabber_receipts_emit(JabberStream *js, const char *conv_name, const char *id,
                     const char *state, const char *sender)
{
	PurpleAccount *account;

	if (!jabber_ui_supports_message_meta() || conv_name == NULL || id == NULL)
		return FALSE;

	account = purple_connection_get_account(js->gc);
	return GPOINTER_TO_INT(purple_signal_emit_return_1(
			purple_conversations_get_handle(), "message-receipt",
			account, conv_name, id, state, sender));
}

/*
 * XEP-0184 §5: answer a request on a live message, but not in rooms, not
 * for our own messages or copies of them, and only to contacts in the
 * buddy list (the receipt tells the sender that we are online).
 */
static void
receipts_answer(JabberMessage *jm)
{
	JabberStream *js = jm->js;
	PurpleAccount *account = purple_connection_get_account(js->gc);
	xmlnode *message, *received;
	char *bare;
	gboolean known;

	if (jm->origin != JABBER_MESSAGE_ORIGIN_LIVE || jm->outgoing ||
	    jm->id == NULL || *jm->id == '\0' || jm->from == NULL)
		return;
	if (jm->type != JABBER_MESSAGE_CHAT && jm->type != JABBER_MESSAGE_NORMAL)
		return;

	bare = jabber_get_bare_jid(jm->from);
	known = bare && purple_find_buddy(account, bare) != NULL;
	g_free(bare);
	if (!known) {
		purple_debug_info("jabber", "Not sending a receipt to %s "
		                  "(not in the buddy list)\n", jm->from);
		return;
	}

	message = jabber_message_stanza_new(js, jm->from, FALSE);
	if (jm->type == JABBER_MESSAGE_NORMAL)
		xmlnode_remove_attrib(message, "type");
	received = xmlnode_new_child(message, "received");
	xmlnode_set_namespace(received, NS_RECEIPTS);
	xmlnode_set_attrib(received, "id", jm->id);
	received = xmlnode_new_child(message, "store");
	xmlnode_set_namespace(received, NS_HINTS);

	jabber_send(js, message);
	xmlnode_free(message);
}

void
jabber_receipts_handle(JabberMessage *jm, const JabberMessageTarget *t)
{
	JabberStream *js = jm->js;

	if (jm->receipt_request)
		receipts_answer(jm);

	/* XEP-0184 <received/>: from the peer, not in rooms.  A received
	 * sent by our other device is its own business. */
	if (jm->receipt_id && !t->own && jm->type != JABBER_MESSAGE_GROUPCHAT)
		jabber_receipts_emit(js, t->conv_name, jm->receipt_id, "delivered",
		                     t->sender);

	/* XEP-0333 markers */
	if (jm->marker && jm->marker_id && t->sender) {
		const char *state = purple_strequal(jm->marker, "received") ?
			"delivered" : "displayed";

		if (t->own) {
			/* Read on another of our devices: clears unread here. */
			if (purple_strequal(state, "displayed")) {
				char *own = jabber_message_own_jid(js, TRUE);
				jabber_receipts_emit(js, t->conv_name, jm->marker_id,
				                     state, own);
				g_free(own);
			}
		} else {
			jabber_receipts_emit(js, t->conv_name, jm->marker_id, state,
			                     t->sender);
		}
	}
}

/**************************************************************************
 * IPC: send-marker
 **************************************************************************/

static gboolean
jabber_receipts_ipc_send_marker(PurpleAccount *account, const char *conv_name,
                                const char *message_id, const char *marker)
{
	JabberIpcTarget t;
	xmlnode *message, *child;
	gboolean ok = FALSE;

	if (message_id == NULL || *message_id == '\0')
		return FALSE;
	if (marker == NULL || *marker == '\0')
		marker = "displayed";
	if (!purple_strequal(marker, "displayed") &&
	    !purple_strequal(marker, "received") &&
	    !purple_strequal(marker, "acknowledged"))
		return FALSE;

	if (!jabber_ipc_target_init(&t, account, conv_name))
		goto out;

	/* XEP-0333 §5.1: markers in anonymous rooms would tell everybody
	 * which messages we have read; only send them in non-anonymous
	 * (private) rooms. */
	if (t.groupchat && !t.chat->nonanonymous) {
		purple_debug_info("jabber", "Not sending a %s marker to %s "
		                  "(room is not non-anonymous)\n", marker, t.to);
		goto out;
	}

	message = jabber_message_stanza_new(t.js, t.to, t.groupchat);
	child = xmlnode_new_child(message, marker);
	xmlnode_set_namespace(child, NS_CHAT_MARKERS);
	xmlnode_set_attrib(child, "id", message_id);
	child = xmlnode_new_child(message, "store");
	xmlnode_set_namespace(child, NS_HINTS);

	jabber_send(t.js, message);
	xmlnode_free(message);
	ok = TRUE;

out:
	jabber_ipc_target_clear(&t);
	return ok;
}

void
jabber_receipts_init(PurplePlugin *plugin)
{
	/* gboolean (account, conv name, message id, marker) */
	purple_plugin_ipc_register(plugin, "send-marker",
			PURPLE_CALLBACK(jabber_receipts_ipc_send_marker),
			purple_marshal_BOOLEAN__POINTER_POINTER_POINTER_POINTER,
			purple_value_new(PURPLE_TYPE_BOOLEAN), 4,
			purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING));
}
