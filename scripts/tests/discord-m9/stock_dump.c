/*
 * M9: the stock-output gate. Feeds every sample payload of test_discord.c
 * to the plugin on a UI WITHOUT message-meta (stock Pidgin 2) and prints
 * everything it writes (conversation, sender, text, flags), every signal it
 * emits and the buddy statuses it sets. run.sh builds this twice, against
 * the branch's libdiscord.c and against the base commit's (STOCK_BASE), and
 * requires identical output: on a stock UI the plugin's behaviour must not
 * change at all.
 */
#define STOCK_DUMP
#include "test_discord.c"

typedef enum { MSG, EDIT, DISPATCH, HISTORY, SEEN } Kind;

typedef struct {
	Kind kind;
	const char *type;       /* the dispatch event */
	const char **payload;
} Step;

static const Step steps[] = {
	{ MSG, NULL, &msg_create },
	{ MSG, NULL, &msg_reply },
	{ MSG, NULL, &msg_reply_deleted_ref },
	{ EDIT, NULL, &msg_update },
	{ EDIT, NULL, &msg_update_embed_only },
	{ DISPATCH, "MESSAGE_REACTION_ADD", &reaction_add_custom },
	{ DISPATCH, "MESSAGE_REACTION_REMOVE", &reaction_remove_unicode_self },
	{ DISPATCH, "MESSAGE_REACTION_ADD", &reaction_add_dm },
	{ DISPATCH, "MESSAGE_DELETE", &msg_delete },
	{ DISPATCH, "MESSAGE_DELETE", &msg_delete_unknown },
	{ MSG, NULL, &dm_create },
	{ MSG, NULL, &dm_own_other_client },
	{ MSG, NULL, &msg_stickers },
	{ MSG, NULL, &msg_tenor },
	{ MSG, NULL, &msg_link },
	{ MSG, NULL, &msg_link_bare },
	{ DISPATCH, "MESSAGE_UPDATE", &msg_link_update },
	{ DISPATCH, "MESSAGE_ACK", &message_ack },
	{ DISPATCH, "MESSAGE_ACK", &message_ack_dm },
	{ SEEN, "conversation-updated", &msg_create },
#ifdef STOCK_DUMP_STEPS
	STOCK_DUMP_STEPS
#endif
};

static void
dump_status(PurpleAccount *account, const char *who)
{
	PurpleBuddy *buddy = purple_find_buddy(account, who);
	PurplePresence *presence = buddy ? purple_buddy_get_presence(buddy) : NULL;
	GList *l;

	if (presence == NULL) {
		printf("status %s: none\n", who);
		return;
	}
	for (l = purple_presence_get_statuses(presence); l != NULL; l = l->next) {
		PurpleStatus *status = l->data;
		GList *a;

		if (!purple_status_is_active(status))
			continue;
		printf("status %s: %s", who, purple_status_get_id(status));
		for (a = purple_status_type_get_attrs(purple_status_get_type(status)); a != NULL; a = a->next) {
			const char *id = purple_status_attr_get_id(a->data);
			PurpleValue *v = purple_status_get_attr_value(status, id);

			if (v != NULL && purple_value_get_type(v) == PURPLE_TYPE_STRING)
				printf(" %s=%s", id, purple_value_get_string(v) ? purple_value_get_string(v) : "(null)");
		}
		printf("\n");
	}
}

int
main(int argc, char **argv)
{
	PurpleAccount *account;
	PurpleConnection *gc;
	DiscordAccount *da;
	DiscordGuild *guild;
	DiscordChannel *channel;
	JsonObject *o;
	PurplePlugin *prpl;
	guint i;

	purple_util_set_user_dir(g_build_filename(argv[1] ? argv[1] : ".", "profile", NULL));
	purple_debug_set_enabled(FALSE);
	purple_eventloop_set_ui_ops(&loop_ops);
	ui_info = g_hash_table_new(g_str_hash, g_str_equal);
	g_hash_table_insert(ui_info, "name", "discord-stock");     /* no message-meta */
	purple_core_set_ui_ops(&core_ops);
	if (!purple_core_init("discord-stock"))
		return 2;
	purple_set_blist(purple_blist_new());
	purple_blist_load();
	prpl = purple_plugin_new(TRUE, NULL);
	purple_init_plugin(prpl);
	purple_plugins_probe(G_MODULE_SUFFIX);
	purple_plugin_load(prpl);
	/* What a message-meta UI would listen to: must stay silent */
	register_meta_signals();
	last_meta = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	seen_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	events = g_string_new(NULL);
	written = g_string_new(NULL);
	requests = g_string_new(NULL);
	capture_requests = TRUE;        /* and print the REST requests too */
	purple_signal_connect(purple_conversations_get_handle(), "receiving-message-meta", &failures, PURPLE_CALLBACK(meta_cb), NULL);
	purple_signal_connect(purple_conversations_get_handle(), "message-corrected", &failures, PURPLE_CALLBACK(corrected_cb), NULL);
	purple_signal_connect(purple_conversations_get_handle(), "message-reaction", &failures, PURPLE_CALLBACK(reaction_cb), NULL);
	purple_signal_connect(purple_conversations_get_handle(), "message-retracted", &failures, PURPLE_CALLBACK(retracted_cb), NULL);
	purple_signal_connect(purple_conversations_get_handle(), "message-receipt", &failures, PURPLE_CALLBACK(receipt_cb), NULL);
	purple_signal_connect(purple_conversations_get_handle(), "writing-chat-msg", &failures, PURPLE_CALLBACK(writing_cb), NULL);
	purple_signal_connect(purple_conversations_get_handle(), "writing-im-msg", &failures, PURPLE_CALLBACK(writing_cb), NULL);

	account = purple_account_new("me@example.com", DISCORD_PLUGIN_ID);
	purple_accounts_add(account);
	gc = g_new0(PurpleConnection, 1);
	gc->account = account;
	gc->prpl = prpl;
	gc->state = PURPLE_CONNECTED;
	purple_account_set_connection(account, gc);

	da = g_new0(DiscordAccount, 1);
	da->account = account;
	da->pc = gc;
	discord_native_init(da);
	gc->proto_data = da;
	da->self_user_id = to_int(SELF_ID);
	da->one_to_ones = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	da->one_to_ones_rev = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	da->sent_message_ids = g_hash_table_new_full(g_str_insensitive_hash, g_str_insensitive_equal, g_free, NULL);
	da->new_users = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, discord_free_user);
	da->new_guilds = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, discord_free_guild);
	da->group_dms = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, discord_free_channel);
	da->last_message_id_dm = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_hash_table_replace(da->one_to_ones, g_strdup(DM_ID), g_strdup("alice"));
	g_hash_table_replace(da->one_to_ones_rev, g_strdup("alice"), g_strdup(DM_ID));
	o = parse("{\"id\":\"" GUILD_ID "\",\"name\":\"Guild\"}");
	guild = discord_upsert_guild(da->new_guilds, o);
	json_object_unref(o);
	o = parse("{\"id\":\"" CHANNEL_ID "\",\"type\":0,\"name\":\"general\"}");
	channel = discord_new_channel(o);
	json_object_unref(o);
	g_hash_table_replace_int64(guild->channels, channel->id, channel);
	purple_account_set_int(account, GUILD_ID "-size", DISCORD_GUILD_SIZE_LARGE);
	o = parse("{\"username\":\"me\",\"discriminator\":\"0\",\"id\":\"" SELF_ID "\"}");
	discord_upsert_user(da->new_users, o);
	json_object_unref(o);
	purple_blist_add_buddy(purple_buddy_new(account, "alice", NULL), NULL, NULL, NULL);
	{
		PurpleConversation *conv = serv_got_joined_chat(gc, discord_chat_hash(channel->id), CHANNEL_ID);

		purple_conversation_set_data(conv, "id", g_memdup2(&channel->id, sizeof(guint64)));
		purple_conv_chat_set_nick(PURPLE_CONV_CHAT(conv), "me");
	}

	for (i = 0; i < G_N_ELEMENTS(steps); i++) {
		reset();
		o = parse(*steps[i].payload);
		switch (steps[i].kind) {
		case MSG:
			discord_process_message(da, o, DISCORD_MESSAGE_NORMAL);
			break;
		case EDIT:
			discord_process_message(da, o, DISCORD_MESSAGE_EDITED);
			break;
		case DISPATCH:
			discord_process_dispatch(da, steps[i].type, o);
			break;
		case SEEN:
			discord_mark_conv_seen(purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, CHANNEL_ID, account),
			                       PURPLE_CONV_UPDATE_UNSEEN);
			break;
		case HISTORY:
			{
				JsonNode *node = json_node_new(JSON_NODE_ARRAY);

				json_node_set_array(node, json_object_get_array_member(o, "messages"));
				discord_got_history_static(da, node, NULL);
				json_node_free(node);
			}
			break;
		}
		json_object_unref(o);
		spin(60);
		printf("step %u %s: meta=%d events=[%s]\n  written=%s\n  requests=%s\n", i,
		       steps[i].type ? steps[i].type : (steps[i].kind == EDIT ? "edit" : steps[i].kind == HISTORY ? "history" : "message"),
		       meta_count, events->str, written->str, requests->str);
		dump_status(account, "alice");
	}
	{
		PurpleStatusType *t;
		GList *types = purple_account_get_status_types(account), *l;

		for (l = types; l != NULL; l = l->next) {
			GList *a;

			t = l->data;
			printf("status type %s:", purple_status_type_get_id(t));
			for (a = purple_status_type_get_attrs(t); a != NULL; a = a->next)
				printf(" %s", purple_status_attr_get_id(a->data));
			printf("\n");
		}
	}
	{
		GList *l;

		for (l = PURPLE_PLUGIN_PROTOCOL_INFO(prpl)->protocol_options; l != NULL; l = l->next)
			printf("option %s\n", purple_account_option_get_setting(l->data));
	}
	/* The plugin's own signal exists only for message-meta UIs */
	{
		gulong id = purple_signal_connect(prpl, "mam-query-done", &failures, PURPLE_CALLBACK(reset), NULL);

		printf("signal mam-query-done: %s\n", id != 0 ? "registered" : "none");
	}
	return 0;
}
