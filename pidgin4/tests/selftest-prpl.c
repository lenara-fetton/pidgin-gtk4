/*
 * pidgin4: the in-process selftest protocol plugin, shared by the
 * conversation selftest (PIDGIN4_CONV_SELFTEST, M4b) and the plugins
 * selftest (PIDGIN4_PLUGINS_SELFTEST, M7).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * No real account may sign in during tests, and libpurple needs a
 * connection to create a conversation. "prpl-pidgin4-selftest" logs in at
 * once without a network, reflects chat messages like an XMPP MUC, and
 * registers the M8 IPC commands, which only record their calls.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "account.h"
#include "cmds.h"
#include "connection.h"
#include "conversation.h"
#include "ft.h"
#include "plugin.h"
#include "prpl.h"
#include "server.h"
#include "signals.h"
#include "status.h"
#include "util.h"
#include "version.h"

#include "selftest-prpl.h"

#define ST_PRPL_ID PIDGIN_SELFTEST_PRPL_ID

static PurplePlugin *st_plugin = NULL;
static GHashTable *st_calls = NULL;     /* command -> last args (string) */
static int sent_counter = 0;

static const char *
st_list_icon(PurpleAccount *account, PurpleBuddy *buddy)
{
	return "selftest";
}

static GList *
st_status_types(PurpleAccount *account)
{
	GList *types = NULL;

	types = g_list_append(types, purple_status_type_new(PURPLE_STATUS_AVAILABLE,
		"available", NULL, TRUE));
	types = g_list_append(types, purple_status_type_new(PURPLE_STATUS_OFFLINE,
		"offline", NULL, TRUE));
	return types;
}

static void
st_login(PurpleAccount *account)
{
	PurpleConnection *gc = purple_account_get_connection(account);

	gc->flags |= PURPLE_CONNECTION_HTML;
	purple_connection_set_state(gc, PURPLE_CONNECTED);
}

static void
st_close(PurpleConnection *gc)
{
}

static void record(const char *command, const char *a, const char *b, const char *c);

static void
emit_sending_meta(PurpleConnection *gc, const char *who, const char *type)
{
	GHashTable *meta = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	char *id = g_strdup_printf("sent-%d", ++sent_counter);

	g_hash_table_insert(meta, g_strdup("conv-type"), g_strdup(type));
	g_hash_table_insert(meta, g_strdup("stanza-id"), g_strdup(id));
	g_hash_table_insert(meta, g_strdup("origin-id"), id);
	purple_signal_emit(purple_conversations_get_handle(), "sending-message-meta",
	                   purple_connection_get_account(gc), who, meta);
	g_hash_table_unref(meta);
}

static int
st_send_im(PurpleConnection *gc, const char *who, const char *message,
           PurpleMessageFlags flags)
{
	record("send-im", who, message, NULL);
	emit_sending_meta(gc, who, "im");
	return 1;
}

static int
st_send_chat(PurpleConnection *gc, int id, const char *message, PurpleMessageFlags flags)
{
	PurpleConversation *conv = purple_find_chat(gc, id);

	/* The room reflects it (with the same ids, like an XMPP MUC). */
	emit_sending_meta(gc, purple_conversation_get_name(conv), "chat");
	serv_got_chat_in(gc, id, purple_conv_chat_get_nick(PURPLE_CONV_CHAT(conv)),
	                 PURPLE_MESSAGE_SEND, message, time(NULL));
	return 0;
}

static GList *
st_chat_info(PurpleConnection *gc)
{
	return NULL;
}

static void
st_join_chat(PurpleConnection *gc, GHashTable *components)
{
}

static PurpleCmdRet
st_op_cmd(PurpleConversation *conv, const char *cmd, char **args, char **error, void *data)
{
	g_hash_table_replace(st_calls, g_strdup("cmd-op"), g_strdup(args[0]));
	return PURPLE_CMD_RET_OK;
}

static void
record(const char *command, const char *a, const char *b, const char *c)
{
	g_hash_table_replace(st_calls, g_strdup(command),
	                     g_strdup_printf("%s|%s|%s", a ? a : "", b ? b : "", c ? c : ""));
}

static gboolean
ipc_send_reaction(PurpleAccount *account, const char *conv, const char *id, const char *list)
{
	record("send-reaction", conv, id, list);
	return TRUE;
}

static gboolean
ipc_send_correction(PurpleAccount *account, const char *conv, const char *id, const char *body)
{
	record("send-correction", conv, id, body);
	return TRUE;
}

static gboolean
ipc_send_reply(PurpleAccount *account, const char *conv, const char *id, const char *jid,
               const char *quote, const char *body)
{
	record("send-reply", id, quote, body);
	return TRUE;
}

static gboolean
ipc_send_retraction(PurpleAccount *account, const char *conv, const char *id)
{
	record("send-retraction", conv, id, NULL);
	return TRUE;
}

static gboolean
ipc_send_marker(PurpleAccount *account, const char *conv, const char *id, const char *marker)
{
	record("send-marker", conv, id, marker);
	return TRUE;
}

static gboolean
ipc_mds_publish(PurpleAccount *account, const char *conv, const char *id)
{
	record("mds-publish", conv, id, NULL);
	return TRUE;
}

static gboolean
st_send_attention(PurpleConnection *gc, const char *who, guint type)
{
	record("send-attention", who, NULL, NULL);
	return TRUE;
}

/* File transfer (off unless pidgin_selftest_prpl_set_caps() turns it on):
 * records the call; with a file, starts a transfer that never moves, for
 * the test to finish or cancel. */
static PurpleXfer *st_last_xfer = NULL;

static void
st_xfer_init(PurpleXfer *xfer)
{
}

static void
st_xfer_start(PurpleConnection *gc, int chat_id, const char *who, const char *file)
{
	PurpleXfer *xfer;

	if (file == NULL)
		return;     /* a real prpl asks for the file here */
	xfer = purple_xfer_new(purple_connection_get_account(gc), PURPLE_XFER_SEND, who);
	purple_xfer_set_init_fnc(xfer, st_xfer_init);
	st_last_xfer = xfer;
	purple_xfer_request_accepted(xfer, file);
}

static gboolean
st_can_receive_file(PurpleConnection *gc, const char *who)
{
	return TRUE;
}

static void
st_send_file(PurpleConnection *gc, const char *who, const char *file)
{
	record("send-file", who, file, NULL);
	st_xfer_start(gc, -1, who, file);
}

static gboolean
st_chat_can_receive_file(PurpleConnection *gc, int id)
{
	return TRUE;
}

static void
st_chat_send_file(PurpleConnection *gc, int id, const char *file)
{
	PurpleConversation *conv = purple_find_chat(gc, id);
	const char *room = conv ? purple_conversation_get_name(conv) : "";

	record("chat-send-file", room, file, NULL);
	st_xfer_start(gc, id, room, file);
}

static PurpleCmdId st_cmd_id;

static gboolean
st_load(PurplePlugin *plugin)
{
	PurpleValue *acct = purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT);

	purple_plugin_ipc_register(plugin, "send-reaction", PURPLE_CALLBACK(ipc_send_reaction),
		purple_marshal_BOOLEAN__POINTER_POINTER_POINTER_POINTER,
		purple_value_new(PURPLE_TYPE_BOOLEAN), 4, purple_value_dup(acct),
		purple_value_new(PURPLE_TYPE_STRING), purple_value_new(PURPLE_TYPE_STRING),
		purple_value_new(PURPLE_TYPE_STRING));
	purple_plugin_ipc_register(plugin, "send-correction", PURPLE_CALLBACK(ipc_send_correction),
		purple_marshal_BOOLEAN__POINTER_POINTER_POINTER_POINTER,
		purple_value_new(PURPLE_TYPE_BOOLEAN), 4, purple_value_dup(acct),
		purple_value_new(PURPLE_TYPE_STRING), purple_value_new(PURPLE_TYPE_STRING),
		purple_value_new(PURPLE_TYPE_STRING));
	purple_plugin_ipc_register(plugin, "send-reply", PURPLE_CALLBACK(ipc_send_reply),
		purple_marshal_BOOLEAN__POINTER_POINTER_POINTER_POINTER_POINTER_POINTER,
		purple_value_new(PURPLE_TYPE_BOOLEAN), 6, purple_value_dup(acct),
		purple_value_new(PURPLE_TYPE_STRING), purple_value_new(PURPLE_TYPE_STRING),
		purple_value_new(PURPLE_TYPE_STRING), purple_value_new(PURPLE_TYPE_STRING),
		purple_value_new(PURPLE_TYPE_STRING));
	purple_plugin_ipc_register(plugin, "send-retraction", PURPLE_CALLBACK(ipc_send_retraction),
		purple_marshal_BOOLEAN__POINTER_POINTER_POINTER,
		purple_value_new(PURPLE_TYPE_BOOLEAN), 3, purple_value_dup(acct),
		purple_value_new(PURPLE_TYPE_STRING), purple_value_new(PURPLE_TYPE_STRING));
	purple_plugin_ipc_register(plugin, "send-marker", PURPLE_CALLBACK(ipc_send_marker),
		purple_marshal_BOOLEAN__POINTER_POINTER_POINTER_POINTER,
		purple_value_new(PURPLE_TYPE_BOOLEAN), 4, purple_value_dup(acct),
		purple_value_new(PURPLE_TYPE_STRING), purple_value_new(PURPLE_TYPE_STRING),
		purple_value_new(PURPLE_TYPE_STRING));
	purple_plugin_ipc_register(plugin, "mds-publish", PURPLE_CALLBACK(ipc_mds_publish),
		purple_marshal_BOOLEAN__POINTER_POINTER_POINTER,
		purple_value_new(PURPLE_TYPE_BOOLEAN), 3, purple_value_dup(acct),
		purple_value_new(PURPLE_TYPE_STRING), purple_value_new(PURPLE_TYPE_STRING));
	purple_value_destroy(acct);

	st_cmd_id = purple_cmd_register("op", "w", PURPLE_CMD_P_PRPL,
		PURPLE_CMD_FLAG_CHAT | PURPLE_CMD_FLAG_PRPL_ONLY, ST_PRPL_ID, st_op_cmd,
		"op &lt;nick&gt;", NULL);
	return TRUE;
}

static gboolean
st_unload(PurplePlugin *plugin)
{
	purple_cmd_unregister(st_cmd_id);
	purple_plugin_ipc_unregister_all(plugin);
	return TRUE;
}

static PurplePluginProtocolInfo st_prpl_info = {
	.options = OPT_PROTO_NO_PASSWORD | OPT_PROTO_CHAT_TOPIC | OPT_PROTO_IM_IMAGE,
	.list_icon = st_list_icon,
	.status_types = st_status_types,
	.login = st_login,
	.close = st_close,
	.send_im = st_send_im,
	.chat_info = st_chat_info,
	.join_chat = st_join_chat,
	.chat_send = st_send_chat,
	.send_attention = st_send_attention,
	.struct_size = sizeof(PurplePluginProtocolInfo),
};

static PurplePluginInfo st_info = {
	.magic = PURPLE_PLUGIN_MAGIC,
	.major_version = PURPLE_MAJOR_VERSION,
	.minor_version = PURPLE_MINOR_VERSION,
	.type = PURPLE_PLUGIN_PROTOCOL,
	.priority = PURPLE_PRIORITY_DEFAULT,
	.id = ST_PRPL_ID,
	.name = "Selftest",
	.version = VERSION,
	.summary = "pidgin4 conversation selftest protocol",
	.description = "Logs in without a network; records the M8 IPC calls.",
	.load = st_load,
	.unload = st_unload,
	.extra_info = &st_prpl_info,
};

/**************************************************************************
 * API
 **************************************************************************/

PurplePlugin *
pidgin_selftest_prpl_register(void)
{
	if (st_plugin != NULL)
		return st_plugin;

	st_calls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	st_plugin = purple_plugin_new(TRUE, NULL);
	st_plugin->info = &st_info;
	purple_plugin_register(st_plugin);
	/* Registered plugins wait in the load queue; probing loads prpls
	 * (the plugin dialog re-probes the same way). */
	purple_plugins_probe(G_MODULE_SUFFIX);
	if (purple_find_prpl(ST_PRPL_ID) != st_plugin || !purple_plugin_is_loaded(st_plugin))
		return NULL;
	return st_plugin;
}

void
pidgin_selftest_prpl_unregister(void)
{
	if (st_plugin != NULL) {
		purple_plugin_unload(st_plugin);
		purple_plugin_destroy(st_plugin);
		st_plugin = NULL;
	}
	g_clear_pointer(&st_calls, g_hash_table_destroy);
}

PurpleAccount *
pidgin_selftest_account_new(const char *username)
{
	PurpleAccount *account = purple_account_new(username, ST_PRPL_ID);

	purple_accounts_add(account);
	purple_account_set_enabled(account, PIDGIN_UI, TRUE);
	purple_account_connect(account);
	pidgin_selftest_spin(50);
	return account;
}

void
pidgin_selftest_account_remove(PurpleAccount *account)
{
	if (account == NULL)
		return;
	purple_account_set_enabled(account, PIDGIN_UI, FALSE);
	purple_accounts_delete(account);
}

const char *
pidgin_selftest_prpl_get_call(const char *command)
{
	return st_calls ? g_hash_table_lookup(st_calls, command) : NULL;
}

void
pidgin_selftest_prpl_set_caps(gboolean im_images, gboolean files)
{
	if (im_images)
		st_prpl_info.options |= OPT_PROTO_IM_IMAGE;
	else
		st_prpl_info.options &= ~OPT_PROTO_IM_IMAGE;
	st_prpl_info.send_file = files ? st_send_file : NULL;
	st_prpl_info.can_receive_file = files ? st_can_receive_file : NULL;
	st_prpl_info.chat_send_file = files ? st_chat_send_file : NULL;
	st_prpl_info.chat_can_receive_file = files ? st_chat_can_receive_file : NULL;
}

PurpleXfer *
pidgin_selftest_prpl_get_last_xfer(void)
{
	return st_last_xfer;
}

void
pidgin_selftest_prpl_forget_xfer(void)
{
	st_last_xfer = NULL;
}

void
pidgin_selftest_spin(guint ms)
{
	gint64 end = g_get_monotonic_time() + ms * 1000;

	while (g_get_monotonic_time() < end)
		g_main_context_iteration(NULL, FALSE);
}
