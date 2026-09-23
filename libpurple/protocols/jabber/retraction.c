/*
 * purple - Jabber Protocol Plugin
 *
 * XEP-0424 Message Retraction and XEP-0425 Moderated Message Retraction
 * (M8, doc/PIDGIN-UPGRADE.md).  Both the current forms
 * (message-retract:1, message-moderate:1) and the older XEP-0422
 * fastening forms (message-retract:0, message-moderate:0) are understood;
 * outgoing retractions carry both.
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

#include "conversation.h"
#include "debug.h"
#include "signals.h"
#include "util.h"

#include "chat.h"
#include "iq.h"
#include "jabber.h"
#include "message.h"
#include "retraction.h"

#define RETRACT_FALLBACK_BODY \
	"This person attempted to retract a previous message, but it's " \
	"unsupported by your client."

static void
set_moderation(JabberMessage *jm, xmlnode *moderated, xmlnode *reason_parent)
{
	xmlnode *reason = reason_parent ? xmlnode_get_child(reason_parent, "reason") : NULL;

	jm->moderated = TRUE;
	g_free(jm->moderated_by);
	jm->moderated_by = g_strdup(xmlnode_get_attrib(moderated, "by"));
	g_free(jm->retract_reason);
	jm->retract_reason = reason ? xmlnode_get_data(reason) : NULL;
	if (jm->retract_reason && *jm->retract_reason == '\0') {
		g_free(jm->retract_reason);
		jm->retract_reason = NULL;
	}
}

gboolean
jabber_retraction_parse(JabberMessage *jm, xmlnode *child, const char *xmlns)
{
	const char *id = xmlnode_get_attrib(child, "id");

	if (purple_strequal(xmlns, NS_RETRACT)) {
		xmlnode *moderated;

		/* <retracted/> tombstones (archives) are not events. */
		if (!purple_strequal(child->name, "retract") || id == NULL || *id == '\0')
			return TRUE;

		/* The current form wins over a legacy <apply-to/> in the same
		 * stanza. */
		g_free(jm->retract_id);
		jm->retract_id = g_strdup(id);
		jm->moderated = FALSE;
		g_free(jm->moderated_by);
		jm->moderated_by = NULL;
		g_free(jm->retract_reason);
		jm->retract_reason = NULL;

		moderated = xmlnode_get_child_with_namespace(child, "moderated", NS_MODERATE);
		if (moderated)
			set_moderation(jm, moderated, child);
		return TRUE;
	}

	if (purple_strequal(xmlns, NS_FASTEN) &&
	    purple_strequal(child->name, "apply-to")) {
		xmlnode *moderated;

		if (jm->retract_id || id == NULL || *id == '\0')
			return TRUE;

		moderated = xmlnode_get_child_with_namespace(child, "moderated",
		                                             NS_MODERATE_LEGACY);
		if (moderated && xmlnode_get_child_with_namespace(moderated, "retract",
		                                                  NS_RETRACT_LEGACY)) {
			jm->retract_id = g_strdup(id);
			set_moderation(jm, moderated, moderated);
		} else if (xmlnode_get_child_with_namespace(child, "retract",
		                                            NS_RETRACT_LEGACY)) {
			jm->retract_id = g_strdup(id);
		}
		/* Other fastenings are not ours; drop them either way. */
		return TRUE;
	}

	return FALSE;
}

static gboolean
retracted_emit(JabberStream *js, const char *conv_name, const char *target_id,
               const char *sender, const char *reason)
{
	if (!jabber_ui_supports_message_meta())
		return FALSE;

	return GPOINTER_TO_INT(purple_signal_emit_return_1(
			purple_conversations_get_handle(), "message-retracted",
			purple_connection_get_account(js->gc), conv_name, target_id,
			sender, reason));
}

gboolean
jabber_retraction_handle(JabberMessage *jm, const JabberMessageTarget *t)
{
	JabberStream *js = jm->js;
	char *text;

	if (jm->moderated) {
		const char *by = jm->moderated_by;
		char *moderator = NULL, *escaped;

		/* XEP-0425: only the room itself announces moderation. */
		if (t->chat == NULL || t->sender != NULL) {
			purple_debug_warning("jabber", "Ignoring moderation of %s not "
			                     "sent by the room\n", jm->retract_id);
			return TRUE;
		}

		if (by) {
			JabberID *jid = jabber_id_new(by);
			if (jid && jid->resource)
				moderator = g_strdup(jid->resource);
			else
				moderator = g_strdup(by);
			jabber_id_free(jid);
		}

		if (!retracted_emit(js, t->conv_name, jm->retract_id, moderator,
		                    jm->retract_reason)) {
			escaped = moderator ? g_markup_escape_text(moderator, -1)
			                    : g_strdup(_("A moderator"));
			if (jm->retract_reason) {
				char *reason = g_markup_escape_text(jm->retract_reason, -1);
				text = g_strdup_printf(_("%s removed a message: %s"),
				                       escaped, reason);
				g_free(reason);
			} else {
				text = g_strdup_printf(_("%s removed a message"), escaped);
			}
			jabber_message_write_event(js, t, text, jm->sent, jm->delayed);
			g_free(text);
			g_free(escaped);
		}
		g_free(moderator);
		return TRUE;
	}

	if (t->sender == NULL) {
		purple_debug_warning("jabber", "Ignoring retraction without a sender\n");
		return TRUE;
	}

	/* In rooms, only the occupant who sent a message may retract it. */
	if (t->chat && jm->occupant_id &&
	    jabber_chat_occupant_mismatch(t->chat, jm->retract_id, jm->occupant_id)) {
		purple_debug_warning("jabber", "Ignoring retraction of %s in %s: "
		                     "not sent by the same occupant\n",
		                     jm->retract_id, t->conv_name);
		return TRUE;
	}

	if (!retracted_emit(js, t->conv_name, jm->retract_id, t->sender, NULL)) {
		char *who = jabber_message_display_name(js, t);
		text = g_strdup_printf(_("%s retracted a message"), who);
		jabber_message_write_event(js, t, text, jm->sent, jm->delayed);
		g_free(text);
		g_free(who);
	}

	return TRUE;
}

/**************************************************************************
 * IPC: send-retraction
 **************************************************************************/

static gboolean
jabber_retraction_ipc_send(PurpleAccount *account, const char *conv_name,
                           const char *target_id)
{
	JabberIpcTarget t;
	xmlnode *message, *child, *apply;
	gboolean ok = FALSE;

	if (target_id == NULL || *target_id == '\0')
		return FALSE;

	if (!jabber_ipc_target_init(&t, account, conv_name))
		goto out;

	message = jabber_message_stanza_new(t.js, t.to, t.groupchat);

	child = xmlnode_new_child(message, "retract");
	xmlnode_set_namespace(child, NS_RETRACT);
	xmlnode_set_attrib(child, "id", target_id);

	/* message-retract:0 for clients that implement the older version */
	apply = xmlnode_new_child(message, "apply-to");
	xmlnode_set_namespace(apply, NS_FASTEN);
	xmlnode_set_attrib(apply, "id", target_id);
	child = xmlnode_new_child(apply, "retract");
	xmlnode_set_namespace(child, NS_RETRACT_LEGACY);

	child = xmlnode_new_child(message, "fallback");
	xmlnode_set_namespace(child, NS_FALLBACK);
	xmlnode_set_attrib(child, "for", NS_RETRACT);
	child = xmlnode_new_child(message, "body");
	xmlnode_insert_data(child, RETRACT_FALLBACK_BODY, -1);
	child = xmlnode_new_child(message, "store");
	xmlnode_set_namespace(child, NS_HINTS);

	jabber_send(t.js, message);
	xmlnode_free(message);

	if (!t.groupchat) {
		/* Reported like a retraction from another device; in rooms the
		 * reflected message does that. */
		char *own = jabber_message_own_jid(t.js, FALSE);

		if (!retracted_emit(t.js, conv_name, target_id, own, NULL)) {
			JabberMessageTarget own_t = { NULL, NULL, NULL, NULL, TRUE };
			char *who, *text;

			own_t.conv_name = (char *)conv_name;
			own_t.sender = own;
			who = jabber_message_display_name(t.js, &own_t);
			text = g_strdup_printf(_("%s retracted a message"), who);
			jabber_message_write_event(t.js, &own_t, text, time(NULL), FALSE);
			g_free(text);
			g_free(who);
		}
		g_free(own);
	}
	ok = TRUE;

out:
	jabber_ipc_target_clear(&t);
	return ok;
}

/**************************************************************************
 * IPC: send-moderation
 **************************************************************************/

static void
moderation_result_cb(JabberStream *js, const char *from, JabberIqType type,
                     const char *id, xmlnode *packet, gpointer data)
{
	char *room = data;

	if (type == JABBER_IQ_ERROR) {
		JabberID *jid = jabber_id_new(room);
		JabberChat *chat = jid ? jabber_chat_find(js, jid->node, jid->domain) : NULL;
		char *msg = jabber_parse_error(js, packet, NULL);

		purple_debug_warning("jabber", "Moderation in %s failed: %s\n", room,
		                     msg ? msg : "(unknown)");
		if (chat && chat->conv) {
			char *escaped = g_markup_escape_text(msg ? msg : _("Unknown error"), -1);
			char *text = g_strdup_printf(_("Could not remove the message: %s"),
			                             escaped);
			purple_conversation_write(chat->conv, "", text,
			                          PURPLE_MESSAGE_ERROR, time(NULL));
			g_free(text);
			g_free(escaped);
		}
		g_free(msg);
		jabber_id_free(jid);
	}

	g_free(room);
}

static gboolean
jabber_retraction_ipc_moderate(PurpleAccount *account, const char *conv_name,
                               const char *target_id, const char *reason)
{
	JabberIpcTarget t;
	JabberIq *iq;
	xmlnode *moderate, *child, *parent;
	gboolean ok = FALSE;

	if (target_id == NULL || *target_id == '\0')
		return FALSE;

	if (!jabber_ipc_target_init(&t, account, conv_name) || !t.groupchat)
		goto out;

	iq = jabber_iq_new(t.js, JABBER_IQ_SET);
	xmlnode_set_attrib(iq->node, "to", t.to);

	if (purple_strequal(t.chat->moderation_ns, NS_MODERATE_LEGACY)) {
		/* XEP-0425 0.2: fastened to the message */
		parent = xmlnode_new_child(iq->node, "apply-to");
		xmlnode_set_namespace(parent, NS_FASTEN);
		xmlnode_set_attrib(parent, "id", target_id);
		moderate = xmlnode_new_child(parent, "moderate");
		xmlnode_set_namespace(moderate, NS_MODERATE_LEGACY);
		child = xmlnode_new_child(moderate, "retract");
		xmlnode_set_namespace(child, NS_RETRACT_LEGACY);
	} else {
		moderate = xmlnode_new_child(iq->node, "moderate");
		xmlnode_set_namespace(moderate, NS_MODERATE);
		xmlnode_set_attrib(moderate, "id", target_id);
		child = xmlnode_new_child(moderate, "retract");
		xmlnode_set_namespace(child, NS_RETRACT);
	}
	if (reason && *reason) {
		child = xmlnode_new_child(moderate, "reason");
		xmlnode_insert_data(child, reason, -1);
	}

	jabber_iq_set_callback(iq, moderation_result_cb, g_strdup(t.to));
	jabber_iq_send(iq);
	ok = TRUE;

out:
	jabber_ipc_target_clear(&t);
	return ok;
}

void
jabber_retraction_init(PurplePlugin *plugin)
{
	/* gboolean (account, conv name, target id) */
	purple_plugin_ipc_register(plugin, "send-retraction",
			PURPLE_CALLBACK(jabber_retraction_ipc_send),
			purple_marshal_BOOLEAN__POINTER_POINTER_POINTER,
			purple_value_new(PURPLE_TYPE_BOOLEAN), 3,
			purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING));

	/* gboolean (account, room JID, target stanza-id, reason or NULL) */
	purple_plugin_ipc_register(plugin, "send-moderation",
			PURPLE_CALLBACK(jabber_retraction_ipc_moderate),
			purple_marshal_BOOLEAN__POINTER_POINTER_POINTER_POINTER,
			purple_value_new(PURPLE_TYPE_BOOLEAN), 4,
			purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING));
}
