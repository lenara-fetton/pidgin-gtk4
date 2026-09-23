/*
 * purple - Jabber Protocol Plugin
 *
 * XEP-0191 extras (privacy modes), XEP-0186 Invisible Command and
 * XEP-0377 Spam Reporting.  See blocking.h.
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

#include "account.h"
#include "debug.h"
#include "plugin.h"
#include "privacy.h"

#include "blocking.h"
#include "iq.h"
#include "jabber.h"
#include "jutil.h"
#include "namespaces.h"
#include "presence.h"

static PurplePlugin *blocking_plugin = NULL;

void
jabber_blocking_server_feature(JabberStream *js, const char *var)
{
	if (purple_strequal(var, NS_INVISIBLE))
		js->invisible_supported = TRUE;
	else if (purple_strequal(var, NS_REPORTING))
		js->reporting_supported = TRUE;
}

/**************************************************************************
 * Stanzas
 **************************************************************************/

xmlnode *
jabber_blocking_build(gboolean block, const char * const *jids,
                      const char *report_reason, const char *text)
{
	xmlnode *node = xmlnode_new(block ? "block" : "unblock");

	xmlnode_set_namespace(node, NS_SIMPLE_BLOCKING);
	for (; jids && *jids; jids++) {
		xmlnode *item = xmlnode_new_child(node, "item");

		xmlnode_set_attrib(item, "jid", *jids);
		if (block && report_reason) {
			xmlnode *report = xmlnode_new_child(item, "report");

			xmlnode_set_namespace(report, NS_REPORTING);
			xmlnode_set_attrib(report, "reason", report_reason);
			if (text && *text) {
				xmlnode *t = xmlnode_new_child(report, "text");
				xmlnode_insert_data(t, text, -1);
			}
		}
	}
	return node;
}

xmlnode *
jabber_invisible_build(gboolean invisible)
{
	xmlnode *node = xmlnode_new(invisible ? "invisible" : "visible");

	xmlnode_set_namespace(node, NS_INVISIBLE);
	/* Without probe='true' the server sends no presence probes for us,
	 * and every contact would look offline while we are invisible. */
	if (invisible)
		xmlnode_set_attrib(node, "probe", "true");
	return node;
}

/**************************************************************************
 * XEP-0186
 **************************************************************************/

static void
invisible_result_cb(JabberStream *js, const char *from, JabberIqType type,
                    const char *id, xmlnode *packet, gpointer data)
{
	gboolean invisible = GPOINTER_TO_INT(data);

	if (!invisible) {
		if (type != JABBER_IQ_RESULT)
			purple_debug_warning("jabber", "XEP-0186: <visible/> was refused\n");
		else
			purple_debug_info("jabber", "XEP-0186: now visible\n");
		return;
	}

	js->invisible_pending = FALSE;
	if (type == JABBER_IQ_RESULT) {
		purple_debug_info("jabber", "XEP-0186: now invisible\n");
		/* The held-back presence; the server doesn't broadcast it. */
		jabber_presence_send(js, TRUE);
		return;
	}

	purple_debug_warning("jabber", "XEP-0186: <invisible/> was refused\n");
	js->invisible_active = FALSE;
	js->invisible_refused = TRUE;
	purple_notify_error(js->gc, _("Invisible"),
		_("The server refused to make you invisible."),
		_("No presence is sent until you choose another status, so your "
		  "contacts see you as offline."));
}

JabberInvisibleAction
jabber_invisible_sync(JabberStream *js, gboolean invisible)
{
	JabberIq *iq;

	if (!invisible && js->invisible_refused) {
		/* Nothing went out while refused: send the presence in full. */
		js->invisible_refused = FALSE;
		return JABBER_INVISIBLE_SEND_FORCED;
	}

	if (!js->invisible_supported) {
		if (invisible)
			purple_debug_info("jabber", "XEP-0186: the server has no "
			                  "invisibility; the invisible status is sent "
			                  "as available\n");
		return JABBER_INVISIBLE_SEND;
	}
	if (invisible && js->invisible_refused)
		return JABBER_INVISIBLE_HOLD;
	if (invisible == js->invisible_active)
		return (invisible && js->invisible_pending) ?
			JABBER_INVISIBLE_HOLD : JABBER_INVISIBLE_SEND;

	iq = jabber_iq_new(js, JABBER_IQ_SET);
	xmlnode_insert_child(iq->node, jabber_invisible_build(invisible));
	jabber_iq_set_callback(iq, invisible_result_cb, GINT_TO_POINTER(invisible));
	jabber_iq_send(iq);

	js->invisible_active = invisible;
	js->invisible_pending = invisible;
	return invisible ? JABBER_INVISIBLE_HOLD : JABBER_INVISIBLE_SEND_FORCED;
}

/**************************************************************************
 * Privacy modes
 **************************************************************************/

GList *
jabber_blocking_privacy_modes(JabberStream *js)
{
	GList *modes = NULL;

	modes = g_list_append(modes, g_strdup("allow-all"));
	if (js && (js->server_caps & (JABBER_CAP_BLOCKING | JABBER_CAP_GOOGLE_ROSTER)))
		modes = g_list_append(modes, g_strdup("deny-users"));
	return modes;
}

void
jabber_set_permit_deny(PurpleConnection *gc)
{
	JabberStream *js = purple_connection_get_protocol_data(gc);
	PurpleAccount *account = purple_connection_get_account(gc);

	if (js == NULL)
		return;

	switch (account->perm_deny) {
		case PURPLE_PRIVACY_ALLOW_ALL:
		case PURPLE_PRIVACY_DENY_USERS:
			/* The server's blocklist (XEP-0191) is the deny list; it
			 * applies in both modes, so there is nothing to send. */
			purple_debug_info("jabber", "privacy mode %d: the server "
			                  "blocklist applies\n", account->perm_deny);
			break;
		default:
			/* ALLOW_USERS, ALLOW_BUDDYLIST and DENY_ALL need XEP-0016
			 * privacy lists, which this prpl doesn't implement.  libpurple
			 * still drops IMs from disallowed senders locally. */
			purple_debug_warning("jabber", "privacy mode %d can't be "
			                     "enforced by the server (no XEP-0016); only "
			                     "incoming IMs are filtered locally\n",
			                     account->perm_deny);
			break;
	}
}

/**************************************************************************
 * IPC
 **************************************************************************/

static JabberStream *
blocking_account_stream(PurpleAccount *account)
{
	PurpleConnection *gc;

	if (account == NULL || !purple_account_is_connected(account))
		return NULL;
	gc = purple_account_get_connection(account);
	if (gc == NULL || purple_connection_get_prpl(gc) != blocking_plugin)
		return NULL;
	return purple_connection_get_protocol_data(gc);
}

/* GList *privacy-modes(PurpleAccount *account) */
static GList *
ipc_privacy_modes(PurpleAccount *account)
{
	JabberStream *js = blocking_account_stream(account);

	return js ? jabber_blocking_privacy_modes(js) : NULL;
}

/* gboolean status-invisible-supported(PurpleAccount *account) */
static gboolean
ipc_invisible_supported(PurpleAccount *account)
{
	JabberStream *js = blocking_account_stream(account);

	return js != NULL && js->invisible_supported;
}

/* gboolean report-spam-supported(PurpleAccount *account) */
static gboolean
ipc_report_supported(PurpleAccount *account)
{
	JabberStream *js = blocking_account_stream(account);

	return js != NULL && js->reporting_supported &&
	       (js->server_caps & JABBER_CAP_BLOCKING);
}

/* gboolean report-spam(PurpleAccount *account, const char *jid,
 *                      const char *reason, gboolean abuse) */
static gboolean
ipc_report_spam(PurpleAccount *account, const char *jid, const char *reason,
                gboolean abuse)
{
	JabberStream *js = blocking_account_stream(account);
	JabberIq *iq;
	const char *norm;
	const char *jids[2];
	gboolean report;

	if (js == NULL || jid == NULL || *jid == '\0')
		return FALSE;
	if (!(js->server_caps & JABBER_CAP_BLOCKING)) {
		purple_debug_info("jabber", "report-spam: no XEP-0191 on the server\n");
		return FALSE;
	}
	norm = jabber_normalize(account, jid);
	if (norm == NULL)
		return FALSE;

	report = js->reporting_supported;
	if (!report)
		purple_debug_info("jabber", "report-spam: the server has no "
		                  "XEP-0377; blocking %s without a report\n", norm);

	jids[0] = norm;
	jids[1] = NULL;
	iq = jabber_iq_new(js, JABBER_IQ_SET);
	xmlnode_insert_child(iq->node, jabber_blocking_build(TRUE, jids,
			report ? (abuse ? JABBER_REPORTING_ABUSE : JABBER_REPORTING_SPAM)
			       : NULL,
			reason));
	jabber_iq_send(iq);
	/* The server's block push mirrors it into the deny list. */
	return TRUE;
}

void
jabber_blocking_init(PurplePlugin *plugin)
{
	blocking_plugin = plugin;

	purple_plugin_ipc_register(plugin, "privacy-modes",
	                           PURPLE_CALLBACK(ipc_privacy_modes),
	                           purple_marshal_POINTER__POINTER,
	                           purple_value_new(PURPLE_TYPE_POINTER), 1,
	                           purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT));
	purple_plugin_ipc_register(plugin, "status-invisible-supported",
	                           PURPLE_CALLBACK(ipc_invisible_supported),
	                           purple_marshal_BOOLEAN__POINTER,
	                           purple_value_new(PURPLE_TYPE_BOOLEAN), 1,
	                           purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT));
	purple_plugin_ipc_register(plugin, "report-spam-supported",
	                           PURPLE_CALLBACK(ipc_report_supported),
	                           purple_marshal_BOOLEAN__POINTER,
	                           purple_value_new(PURPLE_TYPE_BOOLEAN), 1,
	                           purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT));
	purple_plugin_ipc_register(plugin, "report-spam",
	                           PURPLE_CALLBACK(ipc_report_spam),
	                           purple_marshal_BOOLEAN__POINTER_POINTER_POINTER_UINT,
	                           purple_value_new(PURPLE_TYPE_BOOLEAN), 4,
	                           purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
	                           purple_value_new(PURPLE_TYPE_STRING),
	                           purple_value_new(PURPLE_TYPE_STRING),
	                           purple_value_new(PURPLE_TYPE_UINT));
}
