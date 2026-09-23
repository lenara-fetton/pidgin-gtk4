/*
 * purple - Jabber Protocol Plugin
 *
 * XEP-0308 Last Message Correction (M8, doc/PIDGIN-UPGRADE.md).
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

#include "correction.h"
#include "jabber.h"
#include "message.h"

static gboolean
correction_emit(JabberStream *js, const char *conv_name, const char *target_id,
                const char *new_id, const char *body, const char *sender)
{
	if (!jabber_ui_supports_message_meta())
		return FALSE;

	return GPOINTER_TO_INT(purple_signal_emit_return_1(
			purple_conversations_get_handle(), "message-corrected",
			purple_connection_get_account(js->gc), conv_name, target_id,
			new_id, body, sender));
}

void
jabber_correction_prefix_body(JabberMessage *jm)
{
	char *prefix = g_markup_escape_text(JABBER_CORRECTION_PREFIX, -1);
	char *tmp;

	if (jm->body) {
		tmp = g_strconcat(prefix, jm->body, NULL);
		g_free(jm->body);
		jm->body = tmp;
	}
	if (jm->xhtml) {
		/* Before <html><body>: unknown elements are ignored by the
		 * markup parsers, so the prefix shows up in front of the text. */
		tmp = g_strconcat(prefix, jm->xhtml, NULL);
		g_free(jm->xhtml);
		jm->xhtml = tmp;
	}
	g_free(prefix);
}

gboolean
jabber_correction_handle(JabberMessage *jm, const JabberMessageTarget *t,
                         const char *body)
{
	/* XEP-0308 §5: in rooms, a correction must come from the occupant who
	 * sent the original.  With occupant-ids that survives nick changes
	 * and can't be faked by taking over a nick. */
	if (t->chat && jm->occupant_id &&
	    jabber_chat_occupant_mismatch(t->chat, jm->replace_id, jm->occupant_id)) {
		purple_debug_warning("jabber", "Ignoring correction of %s in %s: "
		                     "not sent by the same occupant\n",
		                     jm->replace_id, t->conv_name);
		return TRUE;
	}

	if (correction_emit(jm->js, t->conv_name, jm->replace_id, jm->id, body,
	                    t->sender))
		return TRUE;

	jabber_correction_prefix_body(jm);
	return FALSE;
}

/**************************************************************************
 * IPC: send-correction
 **************************************************************************/

static gboolean
jabber_correction_ipc_send(PurpleAccount *account, const char *conv_name,
                           const char *target_id, const char *new_body)
{
	JabberIpcTarget t;
	JabberMessage *jm;
	gboolean ok = FALSE;

	if (target_id == NULL || *target_id == '\0' ||
	    new_body == NULL || *new_body == '\0')
		return FALSE;

	if (!jabber_ipc_target_init(&t, account, conv_name))
		goto out;

	jm = jabber_message_new_outgoing(&t, new_body);
	jm->replace_id = g_strdup(target_id);
	jabber_message_send_ipc(jm, conv_name, "correction-of", target_id, NULL);

	/* 1:1: nothing echoes the correction back, so it is reported the way
	 * a correction from another device would be.  In rooms the reflected
	 * message does that. */
	if (!t.groupchat) {
		char *markup = jabber_message_plain_to_markup(jm->body);
		char *own = jabber_message_own_jid(t.js, FALSE);

		if (!correction_emit(t.js, conv_name, target_id, jm->id, markup, own)) {
			PurpleConversation *conv = jabber_message_im_conv(account, conv_name);
			char *prefix = g_markup_escape_text(JABBER_CORRECTION_PREFIX, -1);
			char *text = g_strconcat(prefix, markup, NULL);

			purple_conv_im_write(PURPLE_CONV_IM(conv), NULL, text,
			                     PURPLE_MESSAGE_SEND, time(NULL));
			g_free(text);
			g_free(prefix);
		}
		g_free(own);
		g_free(markup);
	}

	jabber_message_free(jm);
	ok = TRUE;

out:
	jabber_ipc_target_clear(&t);
	return ok;
}

void
jabber_correction_init(PurplePlugin *plugin)
{
	/* gboolean (account, conv name, id of the corrected message, new body) */
	purple_plugin_ipc_register(plugin, "send-correction",
			PURPLE_CALLBACK(jabber_correction_ipc_send),
			purple_marshal_BOOLEAN__POINTER_POINTER_POINTER_POINTER,
			purple_value_new(PURPLE_TYPE_BOOLEAN), 4,
			purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING));
}
