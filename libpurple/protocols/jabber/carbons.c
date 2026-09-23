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
#include "util.h"

#include "carbons.h"
#include "chat.h"
#include "iq.h"
#include "message.h"

/*
 * XEP-0280 Message Carbons.
 *
 * Carbons are enabled once the server advertises urn:xmpp:carbons:2.  The
 * server then copies our other devices' incoming messages to us wrapped in
 * <received/>, and their outgoing messages in <sent/>:
 *
 *   <message from='me@example.org' to='me@example.org/pidgin'>
 *     <sent xmlns='urn:xmpp:carbons:2'>
 *       <forwarded xmlns='urn:xmpp:forward:0'>
 *         <message from='me@example.org/phone' to='friend@example.net'
 *                  type='chat'>...</message>
 *       </forwarded>
 *     </sent>
 *   </message>
 *
 * The inner message is parsed normally (keeping its own ids); sent ones
 * are written as PURPLE_MESSAGE_SEND | PURPLE_MESSAGE_REMOTE_SEND.
 */

xmlnode *
jabber_carbons_unwrap(xmlnode *packet, JabberCarbonDirection *dir,
                      xmlnode **forwarded_out)
{
	xmlnode *wrapper, *forwarded, *inner;

	if (dir)
		*dir = JABBER_CARBON_NONE;
	if (forwarded_out)
		*forwarded_out = NULL;

	if (packet == NULL)
		return NULL;

	if ((wrapper = xmlnode_get_child_with_namespace(packet, "received", NS_CARBONS))) {
		if (dir)
			*dir = JABBER_CARBON_RECEIVED;
	} else if ((wrapper = xmlnode_get_child_with_namespace(packet, "sent", NS_CARBONS))) {
		if (dir)
			*dir = JABBER_CARBON_SENT;
	} else {
		return NULL;
	}

	forwarded = xmlnode_get_child_with_namespace(wrapper, "forwarded", NS_FORWARD);
	if (forwarded == NULL)
		return NULL;
	if (forwarded_out)
		*forwarded_out = forwarded;

	inner = xmlnode_get_child(forwarded, "message");
	if (inner == NULL)
		return NULL;

	/* The forwarded stanza keeps the client namespace (or none, if the
	 * serializer dropped a redundant declaration). */
	if (xmlnode_get_namespace(inner) &&
	    !purple_strequal(xmlnode_get_namespace(inner), NS_XMPP_CLIENT))
		return NULL;

	return inner;
}

gboolean
jabber_carbons_from_is_valid(const char *from, const char *own_bare)
{
	/* No from: from our own account (RFC 6120 8.1.2.1). */
	if (from == NULL)
		return TRUE;

	return own_bare != NULL && g_ascii_strcasecmp(from, own_bare) == 0;
}

gboolean
jabber_carbons_handle(JabberStream *js, xmlnode *packet)
{
	JabberCarbonDirection dir;
	JabberMessageContext ctx;
	xmlnode *forwarded, *inner, *delay;
	const char *type, *counterpart;
	char *own_bare;
	gboolean valid;

	inner = jabber_carbons_unwrap(packet, &dir, &forwarded);
	if (dir == JABBER_CARBON_NONE)
		return FALSE;

	own_bare = jabber_id_get_bare_jid(js->user);
	valid = jabber_carbons_from_is_valid(xmlnode_get_attrib(packet, "from"),
	                                     own_bare);
	g_free(own_bare);

	if (!valid) {
		purple_debug_warning("jabber", "Ignoring carbon from %s: not from "
		                     "our own account\n",
		                     xmlnode_get_attrib(packet, "from"));
		return TRUE;
	}

	if (inner == NULL) {
		purple_debug_warning("jabber", "Ignoring malformed carbon\n");
		return TRUE;
	}

	/* Servers don't copy groupchat messages, and <private/> ones must not
	 * be copied at all; ignore any that arrive anyway. */
	type = xmlnode_get_attrib(inner, "type");
	if (purple_strequal(type, "groupchat") || purple_strequal(type, "error")) {
		purple_debug_info("jabber", "Ignoring %s carbon\n", type);
		return TRUE;
	}
	if (xmlnode_get_child_with_namespace(inner, "private", NS_CARBONS)) {
		purple_debug_info("jabber", "Ignoring carbon of a private message\n");
		return TRUE;
	}

	/* MUC private messages are only meaningful while we are in the room. */
	counterpart = xmlnode_get_attrib(inner,
			dir == JABBER_CARBON_SENT ? "to" : "from");
	if (xmlnode_get_child_with_namespace(inner, "x", NS_MUC_USER)) {
		JabberID *jid = counterpart ? jabber_id_new(counterpart) : NULL;
		JabberChat *chat = jid ? jabber_chat_find(js, jid->node, jid->domain) : NULL;

		jabber_id_free(jid);
		if (chat == NULL) {
			purple_debug_info("jabber", "Ignoring carbon of a MUC private "
			                  "message for a room we are not in\n");
			return TRUE;
		}
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.origin = (dir == JABBER_CARBON_SENT) ?
		JABBER_MESSAGE_ORIGIN_CARBON_SENT : JABBER_MESSAGE_ORIGIN_CARBON_RECEIVED;

	delay = xmlnode_get_child_with_namespace(forwarded, "delay", NS_DELAYED_DELIVERY);
	if (delay && xmlnode_get_attrib(delay, "stamp")) {
		ctx.has_stamp = TRUE;
		ctx.stamp = purple_str_to_time(xmlnode_get_attrib(delay, "stamp"),
		                               TRUE, NULL, NULL, NULL);
	}

	jabber_message_parse_with_context(js, inner, &ctx);
	return TRUE;
}

static void
jabber_carbons_enable_cb(JabberStream *js, const char *from,
                         JabberIqType type, const char *id,
                         xmlnode *packet, gpointer data)
{
	if (type == JABBER_IQ_RESULT) {
		js->carbons_enabled = TRUE;
		purple_debug_info("jabber", "Message carbons enabled\n");
	} else {
		char *msg = jabber_parse_error(js, packet, NULL);
		purple_debug_warning("jabber", "Enabling message carbons failed: %s\n",
		                     msg ? msg : "(unknown)");
		g_free(msg);
	}
}

void
jabber_carbons_enable(JabberStream *js)
{
	JabberIq *iq;
	xmlnode *enable;

	if (js->carbons_enabled)
		return;

	iq = jabber_iq_new(js, JABBER_IQ_SET);
	enable = xmlnode_new_child(iq->node, "enable");
	xmlnode_set_namespace(enable, NS_CARBONS);
	jabber_iq_set_callback(iq, jabber_carbons_enable_cb, NULL);
	jabber_iq_send(iq);
}
