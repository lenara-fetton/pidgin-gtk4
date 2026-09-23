/*
 * purple - Jabber Protocol Plugin
 *
 * XEP-0352 Client State Indication
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
#include "signals.h"
#include "status.h"

#include "csi.h"
#include "jabber.h"
#include "namespaces.h"

void
jabber_csi_update(JabberStream *js)
{
	PurpleAccount *account;
	PurpleStatus *status;
	PurpleStatusPrimitive prim;
	gboolean inactive;
	xmlnode *node;

	if (js == NULL || !js->csi_supported || js->state != JABBER_STREAM_CONNECTED)
		return;

	account = purple_connection_get_account(js->gc);
	status = purple_account_get_active_status(account);
	prim = purple_status_type_get_primitive(purple_status_get_type(status));

	inactive = js->csi_ui_inactive || js->idle != 0 ||
	           prim == PURPLE_STATUS_AWAY || prim == PURPLE_STATUS_EXTENDED_AWAY;

	if (inactive == js->csi_sent_inactive)
		return;

	purple_debug_info("jabber", "XEP-0352: client is %s\n",
	                  inactive ? "inactive" : "active");

	/* A nonza: not counted by stream management. */
	node = xmlnode_new(inactive ? "inactive" : "active");
	xmlnode_set_namespace(node, NS_CSI);
	jabber_send(js, node);
	xmlnode_free(node);

	js->csi_sent_inactive = inactive;
}

void
jabber_csi_set_active(JabberStream *js, gboolean active)
{
	g_return_if_fail(js != NULL);

	js->csi_ui_inactive = !active;
	jabber_csi_update(js);
}

static PurplePlugin *csi_plugin = NULL;

static JabberStream *
csi_account_stream(PurpleAccount *account, PurplePlugin *plugin)
{
	PurpleConnection *gc;

	if (account == NULL || !purple_account_is_connected(account))
		return NULL;

	gc = purple_account_get_connection(account);
	if (gc == NULL || purple_connection_get_prpl(gc) != plugin)
		return NULL;

	return purple_connection_get_protocol_data(gc);
}

static void
csi_account_status_changed_cb(PurpleAccount *account, PurpleStatus *old,
                              PurpleStatus *new, PurplePlugin *plugin)
{
	jabber_csi_update(csi_account_stream(account, plugin));
}

/* IPC "csi-set-active" */
static gboolean
csi_ipc_set_active(PurpleAccount *account, gboolean active)
{
	JabberStream *js = csi_account_stream(account, csi_plugin);

	if (js == NULL)
		return FALSE;

	jabber_csi_set_active(js, active);
	return TRUE;
}

void
jabber_csi_init(PurplePlugin *plugin)
{
	csi_plugin = plugin;

	/* The UI (pidgin4, M6) reports window focus through this. */
	purple_plugin_ipc_register(plugin, "csi-set-active",
	                           PURPLE_CALLBACK(csi_ipc_set_active),
	                           purple_marshal_BOOLEAN__POINTER_BOOLEAN,
	                           purple_value_new(PURPLE_TYPE_BOOLEAN), 2,
	                           purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
	                           purple_value_new(PURPLE_TYPE_BOOLEAN));
}

void
jabber_csi_hook_signals(PurplePlugin *plugin)
{
	/* Status changes don't reach the prpl as a callback when only the
	 * primitive's meaning matters, so watch the signal.  Idle comes in
	 * through jabber_idle_set(). */
	purple_signal_connect(purple_accounts_get_handle(), "account-status-changed",
	                      plugin, PURPLE_CALLBACK(csi_account_status_changed_cb),
	                      plugin);
}
