/*
 * M9: tests for the native message metadata of libdiscord.c (see run.sh).
 * Includes the plugin source, runs a real (system) libpurple core with a
 * null UI whose ui_info has message-meta = 1, registers the M8 conversation
 * signals itself (as pidgin4's libpurple does), and drives the plugin's
 * message processing with Discord gateway payloads. No network: the fake
 * connection isn't in purple_connections_get_all(), so every REST call
 * fails immediately (its callback gets NULL). With capture_requests set,
 * REST calls are recorded instead (requests), and their callbacks wait in
 * pending_requests for the test to answer them (respond()).
 */
#include <purple.h>
#include "purple2compat/http.h"

static gboolean capture_requests;
static GString *requests;               /* "METHOD URL [body]\n" */
static GQueue pending_requests = G_QUEUE_INIT;  /* DiscordProxyConnection */

#undef PURPLE_CONNECTION_IS_VALID
#define PURPLE_CONNECTION_IS_VALID(gc) \
	(capture_requests || g_list_find(purple_connections_get_all(), (gc)) != NULL)

static PurpleHttpConnection *
test_http_request(PurpleConnection *gc, PurpleHttpRequest *request, PurpleHttpCallback callback, gpointer user_data)
{
	const gchar *body = purple_http_request_get_contents(request);

	g_string_append_printf(requests, "%s %s%s%s\n", purple_http_request_get_method(request),
	                       purple_http_request_get_url(request), body ? " " : "", body ? body : "");
	g_queue_push_tail(&pending_requests, user_data);
	return NULL;
}

#define purple_http_request test_http_request
#include "libdiscord.c"
#undef purple_http_request

/* purple_compat.h maps these onto libpurple's event loop, which is us */
#undef g_timeout_add_seconds
#undef g_timeout_add
#undef g_source_remove

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(a, b) do { const char *_a = (a), *_b = (b); checks++; \
	if (g_strcmp0(_a, _b) != 0) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s = \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #a, _a ? _a : "(null)", _b ? _b : "(null)"); } } while (0)

/* ---- event loop / core ---- */

typedef struct { PurpleInputFunction f; gpointer data; guint result; } IoClosure;

static gboolean
io_invoke(GIOChannel *source, GIOCondition cond, gpointer data)
{
	IoClosure *c = data;
	int cond2 = 0;

	if (cond & (G_IO_IN | G_IO_HUP | G_IO_ERR)) cond2 |= PURPLE_INPUT_READ;
	if (cond & (G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL)) cond2 |= PURPLE_INPUT_WRITE;
	c->f(c->data, g_io_channel_unix_get_fd(source), cond2);
	return TRUE;
}

static guint
input_add(gint fd, PurpleInputCondition cond, PurpleInputFunction f, gpointer data)
{
	IoClosure *c = g_new0(IoClosure, 1);
	GIOChannel *ch = g_io_channel_unix_new(fd);
	int gc = 0;

	c->f = f;
	c->data = data;
	if (cond & PURPLE_INPUT_READ) gc |= G_IO_IN | G_IO_HUP | G_IO_ERR;
	if (cond & PURPLE_INPUT_WRITE) gc |= G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL;
	c->result = g_io_add_watch_full(ch, G_PRIORITY_DEFAULT, gc, io_invoke, c, g_free);
	g_io_channel_unref(ch);
	return c->result;
}

static guint
t_add(guint interval, GSourceFunc f, gpointer data)
{
	return g_timeout_add(interval, f, data);
}

static guint
t_add_s(guint interval, GSourceFunc f, gpointer data)
{
	return g_timeout_add_seconds(interval, f, data);
}

static gboolean
s_remove(guint id)
{
	return g_source_remove(id);
}

static PurpleEventLoopUiOps loop_ops = {
	t_add, s_remove, input_add, s_remove, NULL,
	t_add_s, NULL, NULL, NULL
};

static GHashTable *ui_info;

static GHashTable *
get_ui_info(void)
{
	return ui_info;
}

static PurpleCoreUiOps core_ops = { NULL, NULL, NULL, NULL, get_ui_info, NULL, NULL, NULL };

/* ---- the M8 signals, as registered by pidgin4's libpurple ---- */

static void
register_meta_signals(void)
{
	void *h = purple_conversations_get_handle();

	purple_signal_register(h, "receiving-message-meta", purple_marshal_VOID__POINTER_POINTER_POINTER, NULL, 3,
		purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
		purple_value_new(PURPLE_TYPE_STRING), purple_value_new(PURPLE_TYPE_BOXED, "GHashTable *"));
	purple_signal_register(h, "message-corrected", purple_marshal_BOOLEAN__POINTER_POINTER_POINTER_POINTER_POINTER_POINTER,
		purple_value_new(PURPLE_TYPE_BOOLEAN), 6,
		purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
		purple_value_new(PURPLE_TYPE_STRING), purple_value_new(PURPLE_TYPE_STRING),
		purple_value_new(PURPLE_TYPE_STRING), purple_value_new(PURPLE_TYPE_STRING),
		purple_value_new(PURPLE_TYPE_STRING));
	purple_signal_register(h, "message-reaction", purple_marshal_BOOLEAN__POINTER_POINTER_POINTER_POINTER_POINTER_POINTER,
		purple_value_new(PURPLE_TYPE_BOOLEAN), 6,
		purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
		purple_value_new(PURPLE_TYPE_STRING), purple_value_new(PURPLE_TYPE_STRING),
		purple_value_new(PURPLE_TYPE_STRING), purple_value_new(PURPLE_TYPE_STRING),
		purple_value_new(PURPLE_TYPE_BOOLEAN));
	purple_signal_register(h, "message-retracted", purple_marshal_BOOLEAN__POINTER_POINTER_POINTER_POINTER_POINTER,
		purple_value_new(PURPLE_TYPE_BOOLEAN), 5,
		purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
		purple_value_new(PURPLE_TYPE_STRING), purple_value_new(PURPLE_TYPE_STRING),
		purple_value_new(PURPLE_TYPE_STRING), purple_value_new(PURPLE_TYPE_STRING));
	purple_signal_register(h, "message-receipt", purple_marshal_BOOLEAN__POINTER_POINTER_POINTER_POINTER_POINTER,
		purple_value_new(PURPLE_TYPE_BOOLEAN), 5,
		purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
		purple_value_new(PURPLE_TYPE_STRING), purple_value_new(PURPLE_TYPE_STRING),
		purple_value_new(PURPLE_TYPE_STRING), purple_value_new(PURPLE_TYPE_STRING));
}

/* ---- recording handlers ---- */

static GHashTable *last_meta;           /* copy of the last receiving-message-meta */
static char *last_meta_conv;
static int meta_count;
static GHashTable *seen_ids;            /* ids the "UI" has: discard them */

static void
meta_cb(PurpleAccount *account, const char *conv_name, GHashTable *meta, gpointer data)
{
	GHashTableIter it;
	gpointer k, v;
	const char *id = g_hash_table_lookup(meta, "stanza-id");

	meta_count++;
	g_free(last_meta_conv);
	last_meta_conv = g_strdup(conv_name);
	g_hash_table_remove_all(last_meta);
	g_hash_table_iter_init(&it, meta);
	while (g_hash_table_iter_next(&it, &k, &v))
		g_hash_table_replace(last_meta, g_strdup(k), g_strdup(v));

	if (id != NULL && g_hash_table_contains(seen_ids, id))
		g_hash_table_replace(meta, g_strdup("discard"), g_strdup("1"));
	else if (id != NULL)
		g_hash_table_add(seen_ids, g_strdup(id));
}

static gboolean handle_events = TRUE;
static GString *events;

static gboolean
corrected_cb(PurpleAccount *a, const char *conv, const char *target, const char *new_id,
             const char *body, const char *sender, gpointer data)
{
	g_string_append_printf(events, "corrected(%s,%s,%s,%s,%s);", conv, target, new_id, body, sender);
	return handle_events;
}

static gboolean
reaction_cb(PurpleAccount *a, const char *conv, const char *target, const char *emoji,
            const char *sender, gpointer add, gpointer data)
{
	g_string_append_printf(events, "reaction(%s,%s,%s,%s,%d);", conv, target, emoji, sender, GPOINTER_TO_INT(add));
	return handle_events;
}

static gboolean
retracted_cb(PurpleAccount *a, const char *conv, const char *target, const char *sender,
             const char *reason, gpointer data)
{
	g_string_append_printf(events, "retracted(%s,%s,%s,%s);", conv, target, sender, reason ? reason : "NULL");
	return handle_events;
}

static GString *written;

static void
query_done_cb(PurpleAccount *a, const char *conv, const char *first, const char *last,
              guint complete, gpointer data)
{
	g_string_append_printf(events, "done(%s,%s,%s,%u);", conv, first ? first : "(null)",
	                       last ? last : "(null)", complete);
}

static gboolean
receipt_cb(PurpleAccount *a, const char *conv, const char *id, const char *state,
           const char *sender, gpointer data)
{
	g_string_append_printf(events, "receipt(%s,%s,%s,%s);", conv, id, state, sender);
	return handle_events;
}

/* Runs the event loop for @ms (delayed requests go out after 30 ms) */
static void
spin(guint ms)
{
	gint64 end = g_get_monotonic_time() + ms * 1000;

	while (g_get_monotonic_time() < end) {
		g_main_context_iteration(NULL, FALSE);
		g_usleep(1000);
	}
}

#ifndef STOCK_DUMP     /* the base plugin has none of this */
/* Lets the queued reactor fetches go out (and fail) before capturing */
static void
drain_reactions(DiscordAccount *da)
{
	int i;

	for (i = 0; i < 100 && (da->reaction_busy || da->reaction_timer != 0 ||
	                        (da->reaction_msgs && !g_queue_is_empty(da->reaction_msgs))); i++)
		spin(50);
	spin(60);
}
#endif

/* Answers the oldest captured request with @json (NULL: a failure) */
static void
respond(const char *json)
{
	DiscordProxyConnection *conn = g_queue_pop_head(&pending_requests);
	JsonParser *parser = json_parser_new();
	JsonNode *root = NULL;

	if (conn == NULL) {
		fprintf(stderr, "respond(): no pending request\n");
		exit(2);
	}
	if (json != NULL && json_parser_load_from_data(parser, json, -1, NULL))
		root = json_parser_get_root(parser);
	if (conn->callback)
		conn->callback(conn->ya, root, conn->user_data);
	g_object_unref(parser);
	discord_proxy_connection_free(conn);
}

static gboolean
writing_cb(PurpleAccount *account, const char *who, char **message, PurpleConversation *conv,
           PurpleMessageFlags flags, gpointer data)
{
	g_string_append_printf(written, "[%s|%s|%s|0x%x]", purple_conversation_get_name(conv), who ? who : "",
	                       *message, flags);
	return FALSE;
}

static void
reset(void)
{
	g_string_truncate(events, 0);
	g_string_truncate(written, 0);
	if (requests != NULL)
		g_string_truncate(requests, 0);
	g_hash_table_remove_all(last_meta);
	g_clear_pointer(&last_meta_conv, g_free);
	meta_count = 0;
}

static JsonObject *
parse(const char *json)
{
	JsonParser *p = json_parser_new();
	JsonObject *o;
	GError *error = NULL;

	if (!json_parser_load_from_data(p, json, -1, &error)) {
		fprintf(stderr, "bad json: %s\n", error->message);
		exit(2);
	}
	o = json_object_ref(json_node_get_object(json_parser_get_root(p)));
	g_object_unref(p);
	return o;
}

#define M(key) ((const char *) g_hash_table_lookup(last_meta, key))

/* ---- sample payloads (Discord API documentation shapes) ---- */

#define GUILD_ID   "41771983423143937"
#define CHANNEL_ID "290926798999357250"
#define DM_ID      "319674150115610528"
#define SELF_ID    "80351110224678912"
#define MASON_ID   "53908099506183680"
#define ALICE_ID   "82198898841029460"

static const char *msg_create =
	"{\"reactions\":[{\"count\":2,\"me\":true,\"emoji\":{\"id\":null,\"name\":\"\xf0\x9f\x94\xa5\"}},"
	"                {\"count\":1,\"me\":false,\"emoji\":{\"id\":\"41771983429993937\",\"name\":\"LUL\"}}],"
	" \"attachments\":[],\"tts\":false,\"embeds\":[],"
	" \"timestamp\":\"2017-07-11T17:27:07.299000+00:00\",\"mention_everyone\":false,"
	" \"id\":\"334385199974967042\",\"pinned\":false,\"edited_timestamp\":null,"
	" \"author\":{\"username\":\"Mason\",\"discriminator\":\"0\",\"id\":\"" MASON_ID "\",\"avatar\":null},"
	" \"mention_roles\":[],\"content\":\"Supa Hot <:LUL:41771983429993937>\","
	" \"channel_id\":\"" CHANNEL_ID "\",\"guild_id\":\"" GUILD_ID "\",\"mentions\":[],\"type\":0}";

static const char *msg_reply =
	"{\"type\":19,\"id\":\"334385199974967099\",\"channel_id\":\"" CHANNEL_ID "\",\"guild_id\":\"" GUILD_ID "\","
	" \"content\":\"agreed\",\"timestamp\":\"2017-07-11T17:28:00.000000+00:00\",\"edited_timestamp\":null,"
	" \"author\":{\"username\":\"alice\",\"discriminator\":\"0\",\"id\":\"" ALICE_ID "\"},"
	" \"attachments\":[{\"id\":\"1\",\"filename\":\"cat.png\",\"content_type\":\"image/png\","
	"   \"url\":\"https://cdn.discordapp.com/attachments/1/2/cat.png?ex=1&is=2&hm=3\","
	"   \"proxy_url\":\"https://media.discordapp.net/attachments/1/2/cat.png?ex=1&is=2&hm=3\",\"width\":640,\"height\":480}],"
	" \"message_reference\":{\"channel_id\":\"" CHANNEL_ID "\",\"message_id\":\"334385199974967042\"},"
	" \"referenced_message\":{\"id\":\"334385199974967042\",\"channel_id\":\"" CHANNEL_ID "\",\"content\":\"Supa Hot\","
	"   \"author\":{\"username\":\"Mason\",\"discriminator\":\"0\",\"id\":\"" MASON_ID "\"},\"type\":0}}";

static const char *msg_reply_deleted_ref =
	"{\"type\":19,\"id\":\"334385199974967100\",\"channel_id\":\"" CHANNEL_ID "\","
	" \"content\":\"what was that?\",\"timestamp\":\"2017-07-11T17:29:00+00:00\","
	" \"author\":{\"username\":\"alice\",\"discriminator\":\"0\",\"id\":\"" ALICE_ID "\"},"
	" \"message_reference\":{\"channel_id\":\"" CHANNEL_ID "\",\"message_id\":\"334385199974960000\"},"
	" \"referenced_message\":null}";

static const char *msg_update =
	"{\"type\":0,\"id\":\"334385199974967042\",\"channel_id\":\"" CHANNEL_ID "\",\"guild_id\":\"" GUILD_ID "\","
	" \"content\":\"Supa **Hot** edited\",\"timestamp\":\"2017-07-11T17:27:07.299000+00:00\","
	" \"edited_timestamp\":\"2017-07-11T17:30:00.000000+00:00\",\"pinned\":false,"
	" \"author\":{\"username\":\"Mason\",\"discriminator\":\"0\",\"id\":\"" MASON_ID "\"}}";

static const char *msg_update_embed_only =
	"{\"type\":0,\"id\":\"334385199974967042\",\"channel_id\":\"" CHANNEL_ID "\",\"guild_id\":\"" GUILD_ID "\","
	" \"content\":\"Supa Hot\",\"timestamp\":\"2017-07-11T17:27:07.299000+00:00\",\"edited_timestamp\":null,"
	" \"embeds\":[{\"type\":\"link\",\"url\":\"https://example.com\"}],\"pinned\":false,"
	" \"author\":{\"username\":\"Mason\",\"discriminator\":\"0\",\"id\":\"" MASON_ID "\"}}";

static const char *reaction_add_custom =
	"{\"user_id\":\"" ALICE_ID "\",\"channel_id\":\"" CHANNEL_ID "\",\"message_id\":\"334385199974967042\","
	" \"guild_id\":\"" GUILD_ID "\",\"member\":{\"user\":{\"username\":\"alice\",\"discriminator\":\"0\",\"id\":\"" ALICE_ID "\"}},"
	" \"emoji\":{\"id\":\"41771983429993937\",\"name\":\"LUL\",\"animated\":false}}";

static const char *reaction_remove_unicode_self =
	"{\"user_id\":\"" SELF_ID "\",\"channel_id\":\"" CHANNEL_ID "\",\"message_id\":\"334385199974967042\","
	" \"guild_id\":\"" GUILD_ID "\",\"emoji\":{\"id\":null,\"name\":\"\xf0\x9f\x94\xa5\"}}";

static const char *reaction_add_dm =
	"{\"user_id\":\"" ALICE_ID "\",\"channel_id\":\"" DM_ID "\",\"message_id\":\"555\","
	" \"emoji\":{\"id\":null,\"name\":\"\xf0\x9f\x91\x8d\"}}";

static const char *msg_delete =
	"{\"id\":\"334385199974967042\",\"channel_id\":\"" CHANNEL_ID "\",\"guild_id\":\"" GUILD_ID "\"}";

static const char *msg_delete_unknown =
	"{\"id\":\"111111111111111111\",\"channel_id\":\"" CHANNEL_ID "\",\"guild_id\":\"" GUILD_ID "\"}";

static const char *dm_create =
	"{\"type\":0,\"id\":\"555\",\"channel_id\":\"" DM_ID "\",\"content\":\"hi there\","
	" \"timestamp\":\"2017-07-11T18:00:00+00:00\","
	" \"author\":{\"username\":\"alice\",\"discriminator\":\"0\",\"id\":\"" ALICE_ID "\"}}";

static const char *dm_own_echo =
	"{\"type\":0,\"id\":\"556\",\"channel_id\":\"" DM_ID "\",\"content\":\"my reply\",\"nonce\":\"424242\","
	" \"timestamp\":\"2017-07-11T18:01:00+00:00\","
	" \"author\":{\"username\":\"me\",\"discriminator\":\"0\",\"id\":\"" SELF_ID "\"}}";

static const char *dm_own_other_client =
	"{\"type\":0,\"id\":\"557\",\"channel_id\":\"" DM_ID "\",\"content\":\"from my phone\",\"nonce\":\"999\","
	" \"timestamp\":\"2017-07-11T18:02:00+00:00\","
	" \"author\":{\"username\":\"me\",\"discriminator\":\"0\",\"id\":\"" SELF_ID "\"}}";

/* Stickers (sticker item objects: PNG, Lottie, GIF) */
static const char *msg_stickers =
	"{\"type\":0,\"id\":\"900000000000000001\",\"channel_id\":\"" CHANNEL_ID "\",\"guild_id\":\"" GUILD_ID "\","
	" \"content\":\"\",\"timestamp\":\"2017-07-11T17:40:00+00:00\",\"edited_timestamp\":null,\"embeds\":[],"
	" \"author\":{\"username\":\"Mason\",\"discriminator\":\"0\",\"id\":\"" MASON_ID "\"},"
	" \"sticker_items\":[{\"id\":\"749054660769218631\",\"name\":\"Wave\",\"format_type\":1},"
	"                   {\"id\":\"816087792291282944\",\"name\":\"Hi <3\",\"format_type\":3},"
	"                   {\"id\":\"1045000000000000000\",\"name\":\"Dance\",\"format_type\":4}]}";

/* A Tenor GIF: the bare URL, with a gifv embed (thumbnail + MP4 video) */
#define TENOR_URL "https://tenor.com/view/cat-typing-gif-12002898"
static const char *msg_tenor =
	"{\"type\":0,\"id\":\"900000000000000002\",\"channel_id\":\"" CHANNEL_ID "\",\"guild_id\":\"" GUILD_ID "\","
	" \"content\":\"" TENOR_URL "\",\"timestamp\":\"2017-07-11T17:41:00+00:00\",\"edited_timestamp\":null,"
	" \"author\":{\"username\":\"alice\",\"discriminator\":\"0\",\"id\":\"" ALICE_ID "\"},"
	" \"embeds\":[{\"type\":\"gifv\",\"url\":\"" TENOR_URL "\","
	"   \"provider\":{\"name\":\"Tenor\",\"url\":\"https://tenor.co\"},"
	"   \"thumbnail\":{\"url\":\"https://media.tenor.com/x5BgTNkA0CUAAAAe/cat-typing.png\","
	"     \"proxy_url\":\"https://images-ext-1.discordapp.net/external/abc/https/media.tenor.com/x5BgTNkA0CUAAAAe/cat-typing.png\","
	"     \"width\":498,\"height\":280},"
	"   \"video\":{\"url\":\"https://media.tenor.com/x5BgTNkA0CUAAAPo/cat-typing.mp4\",\"width\":640,\"height\":360}}]}";

static const char *embed_giphy =
	"{\"type\":\"gifv\",\"url\":\"https://giphy.com/gifs/cat-abc\",\"provider\":{\"name\":\"GIPHY\"},"
	" \"thumbnail\":{\"url\":\"https://media.giphy.com/media/abc/giphy_s.gif\",\"width\":480,\"height\":270},"
	" \"video\":{\"url\":\"https://media.giphy.com/media/abc/giphy.mp4\"}}";

/* A link embed (OpenGraph article) with a long description */
#define LONG_DESC "This **is** a long description that goes on and on. " \
	"Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore " \
	"et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris END"
#define LINK_EMBED \
	"{\"type\":\"article\",\"url\":\"https://example.com/post?a=1&b=2\",\"title\":\"A <title>\"," \
	" \"description\":\"" LONG_DESC "\"," \
	" \"provider\":{\"name\":\"Example\"}," \
	" \"thumbnail\":{\"url\":\"https://example.com/t.png\"," \
	"   \"proxy_url\":\"https://images-ext-2.discordapp.net/external/x/https/example.com/t.png\",\"width\":400,\"height\":300}}"
static const char *msg_link =
	"{\"type\":0,\"id\":\"900000000000000003\",\"channel_id\":\"" CHANNEL_ID "\",\"guild_id\":\"" GUILD_ID "\","
	" \"content\":\"look https://example.com/post?a=1&b=2\",\"timestamp\":\"2017-07-11T17:42:00+00:00\",\"edited_timestamp\":null,"
	" \"author\":{\"username\":\"alice\",\"discriminator\":\"0\",\"id\":\"" ALICE_ID "\"},"
	" \"embeds\":[" LINK_EMBED "]}";

/* The same message before its embed arrived, and the embed-only MESSAGE_UPDATE */
static const char *msg_link_bare =
	"{\"type\":0,\"id\":\"900000000000000004\",\"channel_id\":\"" CHANNEL_ID "\",\"guild_id\":\"" GUILD_ID "\","
	" \"content\":\"see https://example.com/post?a=1&b=2\",\"timestamp\":\"2017-07-11T17:43:00+00:00\",\"edited_timestamp\":null,"
	" \"author\":{\"username\":\"alice\",\"discriminator\":\"0\",\"id\":\"" ALICE_ID "\"},\"embeds\":[]}";
static const char *msg_link_update =
	"{\"type\":0,\"id\":\"900000000000000004\",\"channel_id\":\"" CHANNEL_ID "\",\"guild_id\":\"" GUILD_ID "\","
	" \"content\":\"see https://example.com/post?a=1&b=2\",\"timestamp\":\"2017-07-11T17:43:00+00:00\",\"edited_timestamp\":null,"
	" \"author\":{\"username\":\"alice\",\"discriminator\":\"0\",\"id\":\"" ALICE_ID "\"},"
	" \"embeds\":[" LINK_EMBED "]}";

/* MESSAGE_ACK (user gateway): read up to a message on another client */
static const char *message_ack =
	"{\"version\":1234,\"message_id\":\"334385199974967099\",\"channel_id\":\"" CHANNEL_ID "\","
	" \"last_viewed\":3500,\"flags\":0}";
static const char *message_ack_dm =
	"{\"version\":1235,\"message_id\":\"555\",\"channel_id\":\"" DM_ID "\",\"flags\":0}";

/* A scroll-back page (GET /channels/{id}/messages?before=...), newest first */
static const char *older_page =
	"[{\"type\":0,\"id\":\"334385199974960003\",\"channel_id\":\"" CHANNEL_ID "\",\"content\":\"third\","
	"  \"timestamp\":\"2017-07-11T17:00:03+00:00\",\"edited_timestamp\":null,"
	"  \"author\":{\"username\":\"me\",\"discriminator\":\"0\",\"id\":\"" SELF_ID "\"}},"
	" {\"type\":0,\"id\":\"334385199974960002\",\"channel_id\":\"" CHANNEL_ID "\",\"content\":\"second\","
	"  \"timestamp\":\"2017-07-11T17:00:02+00:00\",\"edited_timestamp\":null,"
	"  \"author\":{\"username\":\"alice\",\"discriminator\":\"0\",\"id\":\"" ALICE_ID "\"},"
	"  \"reactions\":[{\"count\":2,\"me\":true,\"emoji\":{\"id\":null,\"name\":\"\xf0\x9f\x91\x8d\"}},"
	"                {\"count\":1,\"me\":false,\"emoji\":{\"id\":\"41771983429993937\",\"name\":\"LUL\"}}]},"
	" {\"type\":0,\"id\":\"334385199974960001\",\"channel_id\":\"" CHANNEL_ID "\",\"content\":\"first\","
	"  \"timestamp\":\"2017-07-11T17:00:01+00:00\",\"edited_timestamp\":null,"
	"  \"author\":{\"username\":\"Mason\",\"discriminator\":\"0\",\"id\":\"" MASON_ID "\"}}]";

/* History messages with reactions (counts only), newest first */
static const char *reacted_page =
	"[{\"type\":0,\"id\":\"334385199974970003\",\"channel_id\":\"" CHANNEL_ID "\",\"content\":\"party\","
	"  \"timestamp\":\"2017-07-11T19:00:03+00:00\",\"author\":{\"username\":\"Mason\",\"discriminator\":\"0\",\"id\":\"" MASON_ID "\"},"
	"  \"reactions\":[{\"count\":1,\"me\":false,\"emoji\":{\"id\":null,\"name\":\"\xf0\x9f\x8e\x89\"}}]},"
	" {\"type\":0,\"id\":\"334385199974970002\",\"channel_id\":\"" CHANNEL_ID "\",\"content\":\"hot\","
	"  \"timestamp\":\"2017-07-11T19:00:02+00:00\",\"author\":{\"username\":\"Mason\",\"discriminator\":\"0\",\"id\":\"" MASON_ID "\"},"
	"  \"reactions\":[{\"count\":2,\"me\":true,\"emoji\":{\"id\":null,\"name\":\"\xf0\x9f\x94\xa5\"}},"
	"                {\"count\":1,\"me\":false,\"emoji\":{\"id\":\"41771983429993937\",\"name\":\"LUL\"}}]},"
	" {\"type\":0,\"id\":\"334385199974970001\",\"channel_id\":\"" CHANNEL_ID "\",\"content\":\"old\","
	"  \"timestamp\":\"2017-07-11T19:00:01+00:00\",\"author\":{\"username\":\"alice\",\"discriminator\":\"0\",\"id\":\"" ALICE_ID "\"},"
	"  \"reactions\":[{\"count\":1,\"me\":false,\"emoji\":{\"id\":null,\"name\":\"\xf0\x9f\x91\x8d\"}}]}]";

/* GET /channels/{id}/messages/{id}/reactions/{emoji}: user objects */
static const char *reactors_fire =
	"[{\"id\":\"" ALICE_ID "\",\"username\":\"alice\",\"discriminator\":\"0\",\"avatar\":null},"
	" {\"id\":\"" SELF_ID "\",\"username\":\"me\",\"discriminator\":\"0\",\"avatar\":null}]";
static const char *reactors_new_user =
	"[{\"id\":\"90000000000000001\",\"username\":\"bob\",\"global_name\":\"Bob\",\"discriminator\":\"0\",\"avatar\":null}]";

/* A bot's rich embed with fields */
static const char *embed_rich =
	"{\"type\":\"rich\",\"title\":\"Build #42\",\"color\":65280,"
	" \"fields\":[{\"name\":\"Status\",\"value\":\"**passed**\",\"inline\":true}]}";

#ifndef STOCK_DUMP	/* stock_dump.c has its own */
int
main(int argc, char **argv)
{
	PurpleAccount *account;
	PurpleConnection *gc;
	DiscordAccount *da;
	DiscordGuild *guild;
	DiscordChannel *channel;
	JsonObject *o;
	char *dir = g_build_filename(argv[1] ? argv[1] : ".", "profile", NULL);

	purple_util_set_user_dir(dir);
	purple_debug_set_enabled(getenv("DEBUG") != NULL);
	purple_eventloop_set_ui_ops(&loop_ops);
	ui_info = g_hash_table_new(g_str_hash, g_str_equal);
	g_hash_table_insert(ui_info, "name", "discord-test");
	/* A message-meta UI from the start: the plugin registers its signal on load */
	g_hash_table_insert(ui_info, "message-meta", "1");
	purple_core_set_ui_ops(&core_ops);
	if (!purple_core_init("discord-test")) {
		fprintf(stderr, "core init failed\n");
		return 2;
	}
	purple_set_blist(purple_blist_new());
	purple_blist_load();
	/* This very plugin as the prpl (PURPLE_INIT_PLUGIN's entry point) */
	{
		PurplePlugin *prpl = purple_plugin_new(TRUE, NULL);

		CHECK(purple_init_plugin(prpl));
		purple_plugins_probe(G_MODULE_SUFFIX);     /* moves it from the load queue */
		CHECK(purple_plugin_load(prpl));
		CHECK(purple_find_prpl(DISCORD_PLUGIN_ID) == prpl);
		/* The IPC commands pidgin4 looks for */
		CHECK(purple_plugin_ipc_get_params(prpl, "send-correction", NULL, NULL, NULL));
		CHECK(purple_plugin_ipc_get_params(prpl, "send-reaction", NULL, NULL, NULL));
		CHECK(purple_plugin_ipc_get_params(prpl, "send-retraction", NULL, NULL, NULL));
		CHECK(purple_plugin_ipc_get_params(prpl, "send-reply", NULL, NULL, NULL));
		CHECK(purple_plugin_ipc_get_params(prpl, "send-marker", NULL, NULL, NULL));
		{
			int n = -1;
			PurpleValue **params = NULL, *ret = NULL;

			purple_plugin_ipc_get_params(prpl, "send-reply", &ret, &n, &params);
			CHECK(n == 6);
			CHECK(ret != NULL && purple_value_get_type(ret) == PURPLE_TYPE_BOOLEAN);
		}
	}
	register_meta_signals();

	last_meta = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	seen_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	events = g_string_new(NULL);
	written = g_string_new(NULL);
	requests = g_string_new(NULL);
	purple_signal_connect(purple_conversations_get_handle(), "receiving-message-meta", &failures, PURPLE_CALLBACK(meta_cb), NULL);
	purple_signal_connect(purple_conversations_get_handle(), "message-corrected", &failures, PURPLE_CALLBACK(corrected_cb), NULL);
	purple_signal_connect(purple_conversations_get_handle(), "message-reaction", &failures, PURPLE_CALLBACK(reaction_cb), NULL);
	purple_signal_connect(purple_conversations_get_handle(), "message-retracted", &failures, PURPLE_CALLBACK(retracted_cb), NULL);
	purple_signal_connect(purple_conversations_get_handle(), "message-receipt", &failures, PURPLE_CALLBACK(receipt_cb), NULL);
	purple_signal_connect(purple_conversations_get_handle(), "writing-chat-msg", &failures, PURPLE_CALLBACK(writing_cb), NULL);
	purple_signal_connect(purple_conversations_get_handle(), "writing-im-msg", &failures, PURPLE_CALLBACK(writing_cb), NULL);

	/* ---- pure helpers ---- */
	{
		GHashTable *meta;

		o = parse(msg_reply);
		meta = discord_message_meta_build(o, TRUE, "alice", FALSE, TRUE, "Mason");
		CHECK_STR(g_hash_table_lookup(meta, "conv-type"), "chat");
		CHECK_STR(g_hash_table_lookup(meta, "sender"), "alice");
		CHECK_STR(g_hash_table_lookup(meta, "timestamp"), "1499794080");
		CHECK_STR(g_hash_table_lookup(meta, "stanza-id"), "334385199974967099");
		CHECK_STR(g_hash_table_lookup(meta, "server-id"), "334385199974967099");
		CHECK_STR(g_hash_table_lookup(meta, "reply-to"), "334385199974967042");
		CHECK_STR(g_hash_table_lookup(meta, "reply-to-sender"), "Mason");
		CHECK_STR(g_hash_table_lookup(meta, "reply-to-text"), "Supa Hot");
		CHECK(g_hash_table_lookup(meta, "outgoing") == NULL);
		CHECK(g_hash_table_lookup(meta, "discard") == NULL);
		CHECK(g_hash_table_lookup(meta, "origin-id") == NULL);
		g_hash_table_unref(meta);

		meta = discord_message_meta_build(o, FALSE, "me@example.com", TRUE, FALSE, NULL);
		CHECK_STR(g_hash_table_lookup(meta, "conv-type"), "im");
		CHECK_STR(g_hash_table_lookup(meta, "outgoing"), "1");
		CHECK(g_hash_table_lookup(meta, "stanza-id") == NULL);
		CHECK(g_hash_table_lookup(meta, "server-id") == NULL);
		CHECK(g_hash_table_lookup(meta, "reply-to-sender") == NULL);
		g_hash_table_unref(meta);
		json_object_unref(o);

		o = parse(msg_reply_deleted_ref);
		meta = discord_message_meta_build(o, TRUE, "alice", FALSE, TRUE, NULL);
		CHECK_STR(g_hash_table_lookup(meta, "reply-to"), "334385199974960000");
		CHECK(g_hash_table_lookup(meta, "reply-to-text") == NULL);
		CHECK_STR(g_hash_table_lookup(meta, "timestamp"), "1499794140");
		g_hash_table_unref(meta);
		json_object_unref(o);

		o = parse(msg_create);
		meta = discord_message_meta_build(o, TRUE, "Mason", FALSE, TRUE, NULL);
		CHECK(g_hash_table_lookup(meta, "reply-to") == NULL);
		CHECK_STR(g_hash_table_lookup(meta, "timestamp"), "1499794027");
		g_hash_table_unref(meta);
		json_object_unref(o);

		char *p = discord_meta_preview("\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9", 2);
		CHECK_STR(p, "\xc3\xa9\xc3\xa9\xe2\x80\xa6");
		g_free(p);
	}

	/* ---- the account ---- */
	account = purple_account_new("me@example.com", DISCORD_PLUGIN_ID);
	purple_accounts_add(account);
	gc = g_new0(PurpleConnection, 1);
	gc->account = account;
	gc->prpl = purple_find_prpl(DISCORD_PLUGIN_ID);
	gc->state = PURPLE_CONNECTED;
	purple_account_set_connection(account, gc);

	/* Stock UI first: no native metadata */
	g_hash_table_remove(ui_info, "message-meta");
	da = g_new0(DiscordAccount, 1);
	da->account = account;
	da->pc = gc;
	discord_native_init(da);
	CHECK(!da->native_meta);
	CHECK(da->deferred_echo == NULL);
	discord_native_free(da);

	g_hash_table_insert(ui_info, "message-meta", "1");
	discord_native_init(da);
	CHECK(da->native_meta);
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
	/* Large, so a message doesn't try to open the room and fetch history */
	purple_account_set_int(account, GUILD_ID "-size", DISCORD_GUILD_SIZE_LARGE);
	o = parse("{\"username\":\"me\",\"discriminator\":\"0\",\"id\":\"" SELF_ID "\"}");
	discord_upsert_user(da->new_users, o);
	json_object_unref(o);

	/* The open room */
	{
		PurpleConversation *conv = serv_got_joined_chat(gc, discord_chat_hash(channel->id), CHANNEL_ID);

		CHECK(conv != NULL);
		purple_conversation_set_data(conv, "id", g_memdup2(&channel->id, sizeof(guint64)));
		purple_conv_chat_set_nick(PURPLE_CONV_CHAT(conv), "me");
	}

	/* ---- MESSAGE_CREATE in a room ---- */
	reset();
	o = parse(msg_create);
	discord_process_message(da, o, DISCORD_MESSAGE_NORMAL);
	json_object_unref(o);
	CHECK(meta_count == 1);
	CHECK_STR(last_meta_conv, CHANNEL_ID);
	CHECK_STR(M("stanza-id"), "334385199974967042");
	CHECK_STR(M("server-id"), "334385199974967042");
	CHECK_STR(M("sender"), "Mason");
	CHECK_STR(M("conv-type"), "chat");
	CHECK_STR(M("markable"), "1");
	CHECK(strstr(written->str, "[" CHANNEL_ID "|Mason|Supa Hot <img src=\"https://cdn.discordapp.com/emojis/41771983429993937.png?size=48\" alt=\":LUL:\" width=\"22\" height=\"22\"/>|0x2]") != NULL);
	/* No reaction lines: who reacted is fetched (below) */
	CHECK(strstr(written->str, "reacted with") == NULL);
	CHECK(g_queue_get_length(da->reaction_msgs) == 1);
	/* Our own reaction is remembered, the custom emoji's id learnt */
	CHECK(g_hash_table_contains(discord_own_reactions_get(da, "334385199974967042", FALSE), "\xf0\x9f\x94\xa5"));
	CHECK(!g_hash_table_contains(discord_own_reactions_get(da, "334385199974967042", FALSE), "LUL:41771983429993937"));
	CHECK_STR(g_hash_table_lookup(da->custom_emoji, "LUL"), "41771983429993937");
	{
		DiscordMsgInfo *info = g_hash_table_lookup(da->msg_info, "334385199974967042");
		CHECK(info != NULL && info->channel_id == channel->id);
		CHECK_STR(info ? info->sender : NULL, "Mason");
	}
	fprintf(stderr, "written: %s\n", written->str);

	/* The same message again (a history fetch): the UI has it, nothing is written */
	reset();
	o = parse(msg_create);
	discord_process_message(da, o, DISCORD_MESSAGE_NORMAL);
	json_object_unref(o);
	CHECK(meta_count == 1);
	CHECK_STR(written->str, "");

	/* ---- a reply with an image attachment: no quote line, one row ---- */
	reset();
	o = parse(msg_reply);
	discord_process_message(da, o, DISCORD_MESSAGE_NORMAL);
	json_object_unref(o);
	CHECK(meta_count == 1);
	CHECK_STR(M("reply-to"), "334385199974967042");
	CHECK_STR(M("reply-to-sender"), "Mason");
	CHECK_STR(M("reply-to-text"), "Supa Hot");
	CHECK(strstr(written->str, "┌──") == NULL);
	CHECK(strstr(written->str, "agreed<br/><a href=\"https://cdn.discordapp.com/attachments/1/2/cat.png?ex=1&amp;is=2&amp;hm=3\">cat.png</a><br/>"
	                           "<img src=\"https://media.discordapp.net/attachments/1/2/cat.png?ex=1&amp;is=2&amp;hm=3\" alt=\"cat.png\"/>") != NULL);
	CHECK(g_strstr_len(written->str, -1, "][") == NULL);   /* one write */
	fprintf(stderr, "written: %s\n", written->str);

	/* ---- MESSAGE_UPDATE: a correction ---- */
	reset();
	o = parse(msg_update);
	discord_process_message(da, o, DISCORD_MESSAGE_EDITED);
	json_object_unref(o);
	CHECK_STR(events->str, "corrected(" CHANNEL_ID ",334385199974967042,334385199974967042,Supa <b>Hot</b> edited,Mason);");
	CHECK(meta_count == 0);
	CHECK_STR(written->str, "");

	/* ... not shown in the UI: the EDIT: line, without ids but with correction-of */
	reset();
	handle_events = FALSE;
	o = parse(msg_update);
	discord_process_message(da, o, DISCORD_MESSAGE_EDITED);
	json_object_unref(o);
	handle_events = TRUE;
	CHECK(meta_count == 1);
	CHECK(M("stanza-id") == NULL && M("server-id") == NULL);
	CHECK_STR(M("correction-of"), "334385199974967042");
	CHECK(strstr(written->str, "|Mason|EDIT: Supa <b>Hot</b> edited|") != NULL);

	/* ... an embed-only update is ignored */
	reset();
	o = parse(msg_update_embed_only);
	discord_process_message(da, o, DISCORD_MESSAGE_EDITED);
	json_object_unref(o);
	CHECK_STR(events->str, "");
	CHECK_STR(written->str, "");

	/* ---- stickers, GIFs and link embeds ---- */
	reset();
	o = parse(msg_stickers);
	discord_process_message(da, o, DISCORD_MESSAGE_NORMAL);
	json_object_unref(o);
	CHECK(meta_count == 1);
	CHECK(strstr(written->str, "|Mason|<img src=\"https://media.discordapp.net/stickers/749054660769218631.png?size=160\" alt=\"Wave\"/>"
	                           "<br/>[Hi &lt;3]"
	                           "<br/><img src=\"https://media.discordapp.net/stickers/1045000000000000000.gif?size=160\" alt=\"Dance\"/>|") != NULL);
	CHECK(strstr(written->str, ".json") == NULL);
	fprintf(stderr, "written: %s\n", written->str);

	reset();
	o = parse(msg_tenor);
	discord_process_message(da, o, DISCORD_MESSAGE_NORMAL);
	json_object_unref(o);
	CHECK(strstr(written->str, "|alice|" TENOR_URL "<br/><img src=\"https://media.tenor.com/x5BgTNkA0CUAAAAC/cat-typing.gif\" alt=\"GIF\"/>|") != NULL);
	CHECK_STR(M("embed-type"), "gifv");
	CHECK_STR(M("embed-url"), TENOR_URL);
	CHECK_STR(M("embed-image"), "https://media.tenor.com/x5BgTNkA0CUAAAAC/cat-typing.gif");
	CHECK(M("embed-title") == NULL);
	fprintf(stderr, "written: %s\n", written->str);

	{
		char *gif;

		o = parse(embed_giphy);
		gif = discord_embed_gif_url(o);
		CHECK_STR(gif, "https://media.giphy.com/media/abc/giphy.gif");
		g_free(gif);
		json_object_unref(o);
		/* No video, no GIF anywhere: nothing better than the thumbnail */
		o = parse("{\"type\":\"gifv\",\"url\":\"https://tenor.com/view/x\",\"thumbnail\":{\"url\":\"https://media.tenor.com/abcAAAAe/x.png\"}}");
		CHECK(discord_embed_gif_url(o) == NULL);
		gif = discord_native_embed_html(o);
		CHECK_STR(gif, "<img src=\"https://media.tenor.com/abcAAAAe/x.png\" alt=\"GIF\"/>");
		g_free(gif);
		json_object_unref(o);
		o = parse(embed_rich);
		gif = discord_native_embed_html(o);
		CHECK_STR(gif, "<b>Build #42</b><br/><b>Status</b> <b>passed</b>");
		g_free(gif);
		json_object_unref(o);
		o = parse("{\"type\":\"link\",\"url\":\"https://example.com\"}");
		CHECK(discord_native_embed_html(o) == NULL);    /* nothing to show */
		json_object_unref(o);
	}

	reset();
	o = parse(msg_link);
	discord_process_message(da, o, DISCORD_MESSAGE_NORMAL);
	json_object_unref(o);
	CHECK(strstr(written->str, "<br/><b><a href=\"https://example.com/post?a=1&amp;b=2\">A &lt;title&gt;</a></b><br/>This <b>is</b> a long description") != NULL);
	CHECK(strstr(written->str, "END") == NULL);                            /* cut at ~200 characters */
	CHECK(strstr(written->str, "\xe2\x80\xa6<br/><img src=\"https://images-ext-2.discordapp.net/external/x/https/example.com/t.png\" alt=\"Image\"/>|") != NULL);
	CHECK(strstr(written->str, "<font back=") == NULL);                    /* not the old block */
	CHECK_STR(M("embed-type"), "article");
	CHECK_STR(M("embed-title"), "A <title>");
	CHECK_STR(M("embed-description"), LONG_DESC);
	CHECK_STR(M("embed-url"), "https://example.com/post?a=1&b=2");
	CHECK_STR(M("embed-image"), "https://images-ext-2.discordapp.net/external/x/https/example.com/t.png");
	fprintf(stderr, "written: %s\n", written->str);

	/* An embed-only MESSAGE_UPDATE: a correction with the embed block,
	 * described by embed-only-update */
	reset();
	o = parse(msg_link_bare);
	discord_process_message(da, o, DISCORD_MESSAGE_NORMAL);
	json_object_unref(o);
	CHECK(M("embed-type") == NULL);
	reset();
	o = parse(msg_link_update);
	discord_process_dispatch(da, "MESSAGE_UPDATE", o);
	json_object_unref(o);
	CHECK(meta_count == 1);
	CHECK_STR(M("embed-only-update"), "1");
	CHECK_STR(M("correction-of"), "900000000000000004");
	CHECK(M("stanza-id") == NULL && M("server-id") == NULL);
	CHECK_STR(M("sender"), "alice");
	CHECK_STR(M("embed-title"), "A <title>");
	CHECK(g_str_has_prefix(events->str, "corrected(" CHANNEL_ID ",900000000000000004,900000000000000004,see "));
	CHECK(strstr(events->str, "<b><a href=\"https://example.com/post?a=1&amp;b=2\">A &lt;title&gt;</a></b>") != NULL);
	CHECK(strstr(events->str, "EDIT") == NULL);
	CHECK_STR(written->str, "");
	fprintf(stderr, "events: %s\n", events->str);

	/* ... for a message the UI doesn't show: nothing (no EDIT: line) */
	reset();
	handle_events = FALSE;
	o = parse(msg_link_update);
	discord_process_message(da, o, DISCORD_MESSAGE_EDITED);
	json_object_unref(o);
	handle_events = TRUE;
	CHECK(strstr(events->str, "corrected(") != NULL);
	CHECK_STR(written->str, "");

	/* ---- reactions ---- */
	reset();
	o = parse(reaction_add_custom);
	CHECK(discord_native_reaction(da, o, TRUE));
	json_object_unref(o);
	CHECK_STR(events->str, "reaction(" CHANNEL_ID ",334385199974967042,:LUL:,alice,1);");

	reset();
	o = parse(reaction_remove_unicode_self);
	CHECK(discord_native_reaction(da, o, FALSE));
	json_object_unref(o);
	CHECK_STR(events->str, "reaction(" CHANNEL_ID ",334385199974967042,\xf0\x9f\x94\xa5,me,0);");
	CHECK(!g_hash_table_contains(discord_own_reactions_get(da, "334385199974967042", FALSE), "\xf0\x9f\x94\xa5"));

	reset();
	handle_events = FALSE;
	o = parse(reaction_add_dm);
	CHECK(!discord_native_reaction(da, o, TRUE));   /* unhandled: the caller writes its line */
	json_object_unref(o);
	handle_events = TRUE;
	CHECK_STR(events->str, "reaction(alice,555,\xf0\x9f\x91\x8d,alice,1);");

	/* ---- deletes ---- */
	reset();
	o = parse(msg_delete);
	CHECK(discord_native_deleted(da, CHANNEL_ID, NULL, json_object_get_string_member(o, "id")));
	json_object_unref(o);
	CHECK_STR(events->str, "retracted(" CHANNEL_ID ",334385199974967042,Mason,NULL);");

	reset();
	o = parse(msg_delete_unknown);
	CHECK(!discord_native_deleted(da, CHANNEL_ID, NULL, json_object_get_string_member(o, "id")));
	json_object_unref(o);
	CHECK_STR(events->str, "");     /* unknown author: the usual text line */

	/* ---- the dispatcher: edits, deletes and reactions end to end ---- */
	reset();
	o = parse(msg_delete);
	discord_process_dispatch(da, "MESSAGE_DELETE", o);
	json_object_unref(o);
	CHECK_STR(events->str, "retracted(" CHANNEL_ID ",334385199974967042,Mason,NULL);");
	CHECK(strstr(written->str, "was deleted") == NULL);

	reset();
	o = parse(msg_delete_unknown);
	discord_process_dispatch(da, "MESSAGE_DELETE", o);
	json_object_unref(o);
	CHECK(strstr(written->str, "was deleted") != NULL);

	reset();
	o = parse(reaction_add_custom);
	discord_process_dispatch(da, "MESSAGE_REACTION_ADD", o);
	json_object_unref(o);
	CHECK_STR(events->str, "reaction(" CHANNEL_ID ",334385199974967042,:LUL:,alice,1);");

	/* ---- DMs ---- */
	reset();
	o = parse(dm_create);
	discord_process_message(da, o, DISCORD_MESSAGE_NORMAL);
	json_object_unref(o);
	CHECK_STR(last_meta_conv, "alice");
	CHECK_STR(M("conv-type"), "im");
	CHECK_STR(M("sender"), "alice");
	CHECK_STR(M("stanza-id"), "555");
	CHECK(strstr(written->str, "[alice|alice|hi there|0x2]") != NULL);
	fprintf(stderr, "written: %s\n", written->str);

	/* Our own send, echoed: SEND only, no "outgoing" */
	reset();
	g_hash_table_add(da->deferred_echo, g_strdup("424242"));
	g_hash_table_insert(da->sent_message_ids, g_strdup("424242"), NULL);
	o = parse(dm_own_echo);
	discord_process_message(da, o, DISCORD_MESSAGE_NORMAL);
	json_object_unref(o);
	CHECK(!g_hash_table_contains(da->deferred_echo, "424242"));
	CHECK(!g_hash_table_contains(da->sent_message_ids, "424242"));
	CHECK_STR(M("stanza-id"), "556");
	CHECK(M("outgoing") == NULL);
	CHECK(M("markable") == NULL);        /* our own */
	CHECK_STR(M("sender"), "me@example.com");
	CHECK(strstr(written->str, "|my reply|0x1]") != NULL);
	fprintf(stderr, "written: %s\n", written->str);

	/* ... the REST reply for it comes second: nothing more */
	reset();
	{
		JsonNode *node = json_node_new(JSON_NODE_OBJECT);
		DiscordNativeSend *send = g_new0(DiscordNativeSend, 1);

		o = parse(dm_own_echo);
		json_node_set_object(node, o);
		send->room_id = to_int(DM_ID);
		send->nonce = g_strdup("424242");
		discord_native_sent_cb(da, node, send);
		json_node_free(node);
		json_object_unref(o);
	}
	CHECK(meta_count == 0);
	CHECK_STR(written->str, "");

	/* ... a REST reply first (no gateway echo yet): shown from it */
	reset();
	{
		JsonNode *node = json_node_new(JSON_NODE_OBJECT);
		DiscordNativeSend *send;
		const char *reply =
			"{\"type\":0,\"id\":\"558\",\"channel_id\":\"" DM_ID "\",\"content\":\"quick\","
			" \"timestamp\":\"2017-07-11T18:03:00+00:00\","
			" \"author\":{\"username\":\"me\",\"discriminator\":\"0\",\"id\":\"" SELF_ID "\"}}";

		g_hash_table_insert(da->sent_message_ids, g_strdup("777"), NULL);
		send = discord_native_send_new(da, to_int(DM_ID), "777");
		o = parse(reply);
		json_node_set_object(node, o);
		discord_native_sent_cb(da, node, send);
		json_node_free(node);
		json_object_unref(o);
	}
	CHECK_STR(M("stanza-id"), "558");
	CHECK(strstr(written->str, "|quick|0x1]") != NULL);
	CHECK(!g_hash_table_contains(da->deferred_echo, "777"));

	/* ... a failed send: an error line, the nonce forgotten */
	reset();
	{
		const char *err = "{\"code\":50013,\"message\":\"Missing Permissions\"}";
		JsonNode *node = json_node_new(JSON_NODE_OBJECT);
		DiscordNativeSend *send;

		g_hash_table_insert(da->sent_message_ids, g_strdup("888"), NULL);
		send = discord_native_send_new(da, channel->id, "888");
		o = parse(err);
		json_node_set_object(node, o);
		discord_native_sent_cb(da, node, send);
		json_node_free(node);
		json_object_unref(o);
	}
	CHECK(strstr(written->str, "Unable to send message: Missing Permissions") != NULL);
	CHECK(!g_hash_table_contains(da->deferred_echo, "888"));
	CHECK(!g_hash_table_contains(da->sent_message_ids, "888"));

	/* Own message from another client: outgoing, REMOTE_SEND */
	reset();
	o = parse(dm_own_other_client);
	discord_process_message(da, o, DISCORD_MESSAGE_NORMAL);
	json_object_unref(o);
	CHECK_STR(M("outgoing"), "1");
	CHECK(M("markable") == NULL);
	CHECK_STR(M("sender"), "me@example.com");
	CHECK(strstr(written->str, "|from my phone|0x") != NULL);

	/* ---- IPC helpers ---- */
	{
		GPtrArray *wanted = g_ptr_array_new_with_free_func(g_free);
		GPtrArray *add = g_ptr_array_new_with_free_func(g_free);
		GPtrArray *rem = g_ptr_array_new_with_free_func(g_free);
		GHashTable *cur = g_hash_table_new(g_str_hash, g_str_equal);
		char *api;

		api = discord_reaction_emoji_to_api(da, guild, ":LUL:");
		CHECK_STR(api, "LUL:41771983429993937");
		g_free(api);
		CHECK(discord_reaction_emoji_to_api(da, guild, ":nope:") == NULL);
		api = discord_reaction_emoji_to_api(da, guild, "\xf0\x9f\x91\x8d");
		CHECK_STR(api, "\xf0\x9f\x91\x8d");
		g_free(api);
		g_hash_table_replace(guild->emojis, g_strdup("LUL"), g_strdup("123"));
		api = discord_reaction_emoji_to_api(da, guild, ":LUL:");
		CHECK_STR(api, "LUL:123");    /* the room's guild first */
		g_free(api);

		g_hash_table_add(cur, "\xf0\x9f\x91\x8d");
		g_hash_table_add(cur, "LUL:1");
		g_ptr_array_add(wanted, g_strdup("\xf0\x9f\x91\x8d"));
		g_ptr_array_add(wanted, g_strdup("\xf0\x9f\x8e\x89"));
		g_ptr_array_add(wanted, g_strdup("\xf0\x9f\x8e\x89"));
		discord_reaction_diff(cur, wanted, add, rem);
		CHECK(add->len == 1 && purple_strequal(g_ptr_array_index(add, 0), "\xf0\x9f\x8e\x89"));
		CHECK(rem->len == 1 && purple_strequal(g_ptr_array_index(rem, 0), "LUL:1"));
		g_ptr_array_set_size(add, 0);
		g_ptr_array_set_size(rem, 0);
		discord_reaction_diff(NULL, wanted, add, rem);
		CHECK(add->len == 2 && rem->len == 0);
		g_ptr_array_set_size(add, 0);
		g_ptr_array_set_size(wanted, 0);
		discord_reaction_diff(cur, wanted, add, rem);   /* "" removes all */
		CHECK(add->len == 0 && rem->len == 2);
		g_ptr_array_free(wanted, TRUE);
		g_ptr_array_free(add, TRUE);
		g_ptr_array_free(rem, TRUE);
		g_hash_table_destroy(cur);

		/* Animated custom emoji */
		char *html = g_regex_replace_eval(emoji_regex, "x &lt;a:dance:456&gt;", -1, 0, 0, discord_replace_emoji_native, da, NULL);
		CHECK_STR(html, "x <img src=\"https://cdn.discordapp.com/emojis/456.gif?size=48\" alt=\":dance:\" width=\"22\" height=\"22\"/>");
		g_free(html);

		/* Spoilers and files stay links */
		o = parse("{\"url\":\"https://cdn.discordapp.com/attachments/1/2/SPOILER_x.png\",\"proxy_url\":\"https://media.discordapp.net/attachments/1/2/SPOILER_x.png\",\"content_type\":\"image/png\",\"filename\":\"SPOILER_x.png\"}");
		html = discord_attachment_html(da, o);
		CHECK_STR(html, "https://cdn.discordapp.com/attachments/1/2/SPOILER_x.png");
		g_free(html);
		json_object_unref(o);
		o = parse("{\"url\":\"https://cdn.discordapp.com/attachments/1/2/a.zip\",\"proxy_url\":\"https://media.discordapp.net/attachments/1/2/a.zip\",\"content_type\":\"application/zip\",\"filename\":\"a.zip\"}");
		html = discord_attachment_html(da, o);
		CHECK_STR(html, "https://cdn.discordapp.com/attachments/1/2/a.zip");
		g_free(html);
		json_object_unref(o);
		purple_account_set_int(account, "image-size", 320);
		o = parse("{\"url\":\"https://cdn.discordapp.com/a/b.png\",\"proxy_url\":\"https://media.discordapp.net/a/b.png\",\"content_type\":\"image/png\",\"filename\":\"b.png\",\"width\":640,\"height\":480}");
		html = discord_attachment_html(da, o);
		CHECK(strstr(html, "src=\"https://media.discordapp.net/a/b.png?width=320&amp;height=240\"") != NULL);
		g_free(html);
		json_object_unref(o);
		purple_account_set_int(account, "image-size", 0);

		/* IPC: dispatched (the request itself fails: the fake connection isn't in the list) */
		CHECK(discord_ipc_send_retraction(account, CHANNEL_ID, "334385199974967042"));
		CHECK(!discord_ipc_send_retraction(account, CHANNEL_ID, "not-a-number"));
		CHECK(!discord_ipc_send_reply(account, "nobody", "1", NULL, NULL, "x"));
		CHECK(discord_ipc_send_correction(account, CHANNEL_ID, "334385199974967042", "fixed *text*"));
		CHECK(!discord_ipc_send_correction(account, CHANNEL_ID, "334385199974967042", ""));
		/* send-reaction diffs against our known set and updates it */
		CHECK(discord_ipc_send_reaction(account, CHANNEL_ID, "334385199974967042", ":LUL: \xf0\x9f\x8e\x89 :nope:"));
		{
			GHashTable *own = discord_own_reactions_get(da, "334385199974967042", FALSE);
			CHECK(own != NULL && g_hash_table_size(own) == 2);
			CHECK(own != NULL && g_hash_table_contains(own, "LUL:123"));   /* the guild's id */
			CHECK(own != NULL && g_hash_table_contains(own, "\xf0\x9f\x8e\x89"));
		}
		CHECK(discord_ipc_send_reaction(account, CHANNEL_ID, "334385199974967042", ""));
		CHECK(g_hash_table_size(discord_own_reactions_get(da, "334385199974967042", FALSE)) == 0);
		/* A reply: sent with the echo held back */
		CHECK(discord_ipc_send_reply(account, CHANNEL_ID, "334385199974967042", NULL, "Supa Hot", "yes"));
		CHECK(!discord_ipc_send_reaction(NULL, CHANNEL_ID, "1", "x"));
		/* ... connected */
		purple_account_set_int(account, "dummy", 0);
		CHECK(discord_ipc_room(da, CHANNEL_ID, NULL) == channel->id);
		CHECK(discord_ipc_room(da, "alice", NULL) == to_int(DM_ID));
		CHECK(discord_ipc_room(da, "nobody", NULL) == 0);
		CHECK(discord_ipc_message_channel(da, 5, "334385199974967042") == channel->id);
		CHECK(discord_ipc_message_channel(da, 5, "404") == 5);
		html = discord_ipc_body_html(da, NULL, "a < b\nline 2");
		CHECK_STR(html, "a &lt; b<br>line 2");
		g_free(html);
	}

	/* ---- read state ---- */
	CHECK(purple_plugin_ipc_get_params(purple_find_prpl(DISCORD_PLUGIN_ID), "send-marker", NULL, NULL, NULL));
	/* MESSAGE_ACK from another client: read elsewhere, from our account */
	reset();
	o = parse(message_ack);
	discord_process_dispatch(da, "MESSAGE_ACK", o);
	json_object_unref(o);
	CHECK_STR(events->str, "receipt(" CHANNEL_ID ",334385199974967099,displayed,me@example.com);");
	CHECK(discord_native_read_get(da, channel->id) == 334385199974967099ULL);
	/* ... again (the echo of an ack): nothing */
	reset();
	o = parse(message_ack);
	discord_process_dispatch(da, "MESSAGE_ACK", o);
	json_object_unref(o);
	CHECK_STR(events->str, "");
	reset();
	o = parse(message_ack_dm);
	discord_process_dispatch(da, "MESSAGE_ACK", o);
	json_object_unref(o);
	CHECK_STR(events->str, "receipt(alice,555,displayed,me@example.com);");

	/* send-marker: one ack per newly read message */
	drain_reactions(da);    /* the earlier requests go out (and fail) first */
	capture_requests = TRUE;
	reset();
	CHECK(discord_ipc_send_marker(account, CHANNEL_ID, "334385199974967100", NULL));
	spin(60);
	CHECK(strstr(requests->str, "POST https://discord.com/api/v10/channels/" CHANNEL_ID "/messages/334385199974967100/ack {\"token\":null}") != NULL);
	CHECK(discord_native_read_get(da, channel->id) == 334385199974967100ULL);
	fprintf(stderr, "requests: %s", requests->str);
	respond("{\"token\":\"ack-token-1\"}");
	CHECK_STR(da->ack_token, "ack-token-1");
	reset();
	CHECK(discord_ipc_send_marker(account, CHANNEL_ID, "334385199974967100", "displayed"));     /* again */
	CHECK(discord_ipc_send_marker(account, CHANNEL_ID, "334385199974967099", "displayed"));     /* older */
	CHECK(!discord_ipc_send_marker(account, CHANNEL_ID, "334385199974967101", "received"));    /* no such thing */
	CHECK(!discord_ipc_send_marker(account, "nobody", "334385199974967101", NULL));
	spin(60);
	CHECK_STR(requests->str, "");
	reset();
	CHECK(discord_ipc_send_marker(account, "alice", "556", "acknowledged"));
	spin(60);
	CHECK(strstr(requests->str, "POST https://discord.com/api/v10/channels/" DM_ID "/messages/556/ack {\"token\":\"ack-token-1\"}") != NULL);
	respond(NULL);
	/* The conversation's unseen update no longer acks (send-marker does) */
	reset();
	discord_mark_conv_seen(purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, CHANNEL_ID, account),
	                       PURPLE_CONV_UPDATE_UNSEEN);
	spin(60);
	CHECK_STR(requests->str, "");
	capture_requests = FALSE;
	g_free(da->ack_token);
	da->ack_token = NULL;

	/* ---- scroll-back: mam-fetch-older and mam-query-done ---- */
	{
		PurplePlugin *prpl = purple_find_prpl(DISCORD_PLUGIN_ID);
		int n = -1;

		CHECK(purple_plugin_ipc_get_params(prpl, "mam-fetch-older", NULL, &n, NULL) && n == 4);
		CHECK(purple_signal_connect(prpl, "mam-query-done", &failures, PURPLE_CALLBACK(query_done_cb), NULL) != 0);
	}
	spin(60);
	capture_requests = TRUE;
	purple_account_set_bool(account, "show-reactions", FALSE);     /* reactions: below */
	reset();
	CHECK(GPOINTER_TO_INT(purple_plugin_ipc_call(purple_find_prpl(DISCORD_PLUGIN_ID), "mam-fetch-older", NULL,
	                                             account, CHANNEL_ID, "334385199974967042", 3)));
	spin(60);
	CHECK_STR(requests->str, "GET https://discord.com/api/v10/channels/" CHANNEL_ID "/messages?limit=3&before=334385199974967042\n");
	respond(older_page);
	/* Oldest first, DELAYED; ours SEND */
	CHECK_STR(written->str,
		"[" CHANNEL_ID "|Mason|first|0x402]"
		"[" CHANNEL_ID "|alice|second|0x402]"
		"[" CHANNEL_ID "|me|third|0x10401]");
	CHECK(meta_count == 3);
	CHECK_STR(M("mam"), "1");
	CHECK_STR(M("mam-query"), "older");
	CHECK_STR(M("stanza-id"), "334385199974960003");
	CHECK_STR(M("outgoing"), "1");
	CHECK(g_str_has_suffix(events->str, "done(" CHANNEL_ID ",334385199974960001,334385199974960003,0);"));
	fprintf(stderr, "older page: %s\n  events: %s\n", written->str, events->str);
	CHECK(!da->history_older);
	/* The last page: fewer than asked for */
	reset();
	CHECK(discord_ipc_mam_fetch_older(account, CHANNEL_ID, "334385199974960001", 100));
	spin(60);
	CHECK_STR(requests->str, "GET https://discord.com/api/v10/channels/" CHANNEL_ID "/messages?limit=100&before=334385199974960001\n");
	respond("[{\"type\":0,\"id\":\"334385199974950000\",\"channel_id\":\"" CHANNEL_ID "\",\"content\":\"zeroth\","
	        "  \"timestamp\":\"2017-07-11T16:00:00+00:00\",\"author\":{\"username\":\"Mason\",\"discriminator\":\"0\",\"id\":\"" MASON_ID "\"}}]");
	CHECK(g_str_has_suffix(events->str, "done(" CHANNEL_ID ",334385199974950000,334385199974950000,1);"));
	/* ... nothing older */
	reset();
	CHECK(discord_ipc_mam_fetch_older(account, CHANNEL_ID, "334385199974950000", 0));      /* count 0 -> 1 */
	spin(60);
	CHECK(strstr(requests->str, "?limit=1&before=334385199974950000") != NULL);
	respond("[]");
	CHECK_STR(events->str, "done(" CHANNEL_ID ",(null),(null),1);");
	CHECK_STR(written->str, "");
	/* ... a failed request */
	reset();
	CHECK(discord_ipc_mam_fetch_older(account, CHANNEL_ID, NULL, 500));                    /* 500 -> 100, newest */
	spin(60);
	CHECK_STR(requests->str, "GET https://discord.com/api/v10/channels/" CHANNEL_ID "/messages?limit=100\n");
	respond(NULL);
	CHECK_STR(events->str, "done(" CHANNEL_ID ",(null),(null),0);");
	/* A DM */
	reset();
	CHECK(discord_ipc_mam_fetch_older(account, "alice", "", 50));
	spin(60);
	CHECK_STR(requests->str, "GET https://discord.com/api/v10/channels/" DM_ID "/messages?limit=50\n");
	respond("[]");
	CHECK_STR(events->str, "done(alice,(null),(null),1);");
	CHECK(!discord_ipc_mam_fetch_older(account, CHANNEL_ID, "abc", 50));
	CHECK(!discord_ipc_mam_fetch_older(account, "nobody", NULL, 50));
	/* A live message afterwards is no archive result */
	reset();
	o = parse(msg_link_bare);
	json_object_set_string_member(o, "id", "900000000000000099");
	discord_process_message(da, o, DISCORD_MESSAGE_NORMAL);
	json_object_unref(o);
	CHECK(M("mam") == NULL && M("mam-query") == NULL);
	CHECK(strstr(written->str, "|0x2]") != NULL);
	capture_requests = FALSE;
	purple_account_set_bool(account, "show-reactions", TRUE);

	/* ---- reactions on history messages ---- */
	{
		GList *l;
		gboolean found = FALSE;

		for (l = PURPLE_PLUGIN_PROTOCOL_INFO(purple_find_prpl(DISCORD_PLUGIN_ID))->protocol_options; l; l = l->next)
			found = found || purple_strequal(purple_account_option_get_setting(l->data), "reaction_history_limit");
		CHECK(found);
	}
	drain_reactions(da);
	capture_requests = TRUE;
	purple_account_set_int(account, "reaction_history_limit", 2);
	reset();
	CHECK(discord_ipc_mam_fetch_older(account, CHANNEL_ID, "334385199974970004", 3));
	spin(60);
	respond(reacted_page);
	CHECK(strstr(written->str, "reacted") == NULL);         /* no text lines */
	CHECK(strstr(written->str, "|Mason|hot|0x402]") != NULL);
	CHECK(g_queue_get_length(da->reaction_msgs) == 2);      /* the newest 2 of 3 */
	CHECK(!g_hash_table_contains(discord_own_reactions_get(da, "334385199974970002", TRUE), "LUL:41771983429993937"));
	CHECK(g_hash_table_contains(discord_own_reactions_get(da, "334385199974970002", TRUE), "\xf0\x9f\x94\xa5"));
	reset();
	spin(200);
	CHECK_STR(requests->str, "");                           /* not at once */
	spin(250);
	CHECK_STR(requests->str, "GET https://discord.com/api/v10/channels/" CHANNEL_ID "/messages/334385199974970002/reactions/%F0%9F%94%A5?limit=100\n");
	spin(400);
	CHECK(g_strstr_len(requests->str, -1, "\n") == requests->str + requests->len - 1);  /* one at a time */
	respond(reactors_fire);
	CHECK_STR(events->str,
		"reaction(" CHANNEL_ID ",334385199974970002,\xf0\x9f\x94\xa5,alice,1);"
		"reaction(" CHANNEL_ID ",334385199974970002,\xf0\x9f\x94\xa5,me,1);");
	reset();
	spin(450);
	CHECK_STR(requests->str, "GET https://discord.com/api/v10/channels/" CHANNEL_ID "/messages/334385199974970002/reactions/LUL%3A41771983429993937?limit=100\n");
	/* That response said the bucket is empty: wait for its reset */
	discord_native_note_rate_limit(da, "0", "1.2");
	respond(reactors_new_user);
	CHECK_STR(events->str, "reaction(" CHANNEL_ID ",334385199974970002,:LUL:,bob,1);");
	reset();
	spin(900);
	CHECK_STR(requests->str, "");
	spin(600);
	CHECK_STR(requests->str, "GET https://discord.com/api/v10/channels/" CHANNEL_ID "/messages/334385199974970003/reactions/%F0%9F%8E%89?limit=100\n");
	discord_native_note_rate_limit(da, "4", "0.5");
	respond(NULL);                                          /* failed: next one */
	CHECK_STR(events->str, "");
	reset();
	spin(450);
	CHECK_STR(requests->str, "");                           /* the queue is empty */
	CHECK(g_queue_is_empty(da->reaction_msgs) && !da->reaction_busy && da->reaction_timer == 0);
	/* Disabled */
	purple_account_set_int(account, "reaction_history_limit", 0);
	reset();
	o = parse(msg_create);
	json_object_set_string_member(o, "id", "334385199974970010");
	discord_process_message(da, o, DISCORD_MESSAGE_NORMAL);
	json_object_unref(o);
	CHECK(g_queue_is_empty(da->reaction_msgs));
	CHECK(strstr(written->str, "reacted") == NULL);
	purple_account_set_int(account, "reaction_history_limit", 25);
	capture_requests = FALSE;

	/* ---- stock UI: the same payloads give the old output ---- */
	{
		DiscordAccount *stock = g_new0(DiscordAccount, 1);

		*stock = *da;
		stock->native_meta = FALSE;
		gc->proto_data = stock;
		reset();
		o = parse(msg_reply);
		discord_process_message(stock, o, DISCORD_MESSAGE_NORMAL);
		json_object_unref(o);
		CHECK(meta_count == 0);
		CHECK(strstr(written->str, "┌──@Mason") != NULL);           /* the quote line */
		CHECK(strstr(written->str, "|alice|agreed|") != NULL);
		CHECK(strstr(written->str, "|alice|https://cdn.discordapp.com/attachments/1/2/cat.png?ex=1&is=2&hm=3|") != NULL);
		fprintf(stderr, "stock written: %s\n", written->str);

		reset();
		o = parse(msg_update);
		discord_process_message(stock, o, DISCORD_MESSAGE_EDITED);
		json_object_unref(o);
		CHECK_STR(events->str, "");
		CHECK(strstr(written->str, "EDIT: Supa <b>Hot</b> edited") != NULL);

		reset();
		o = parse(msg_delete);
		discord_process_dispatch(stock, "MESSAGE_DELETE", o);
		json_object_unref(o);
		CHECK_STR(events->str, "");
		CHECK(strstr(written->str, "was deleted") != NULL);

		reset();
		o = parse(msg_create);
		discord_process_message(stock, o, DISCORD_MESSAGE_NORMAL);
		json_object_unref(o);
		CHECK(meta_count == 0);
		CHECK(strstr(written->str, "|Mason|Supa Hot :LUL:|") != NULL);   /* custom smiley path */
		/* The unseen update acks the channel, MESSAGE_ACK is ignored */
		capture_requests = TRUE;
		reset();
		discord_mark_conv_seen(purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, CHANNEL_ID, account),
		                       PURPLE_CONV_UPDATE_UNSEEN);
		spin(60);
		CHECK(strstr(requests->str, "POST https://discord.com/api/v10/channels/" CHANNEL_ID "/messages/") != NULL &&
		      strstr(requests->str, "/ack ") != NULL);
		respond(NULL);
		reset();
		o = parse(message_ack);
		discord_process_dispatch(stock, "MESSAGE_ACK", o);
		json_object_unref(o);
		CHECK_STR(events->str, "");
		capture_requests = FALSE;

		/* Stickers, GIF and link embeds: the old lines and block */
		reset();
		o = parse(msg_stickers);
		discord_process_message(stock, o, DISCORD_MESSAGE_NORMAL);
		json_object_unref(o);
		CHECK(strstr(written->str, "|Mason|\nhttps://cdn.discordapp.com/stickers/749054660769218631.png"
		                           "\nhttps://cdn.discordapp.com/stickers/816087792291282944.json"
		                           "\nhttps://cdn.discordapp.com/stickers/1045000000000000000.png|") != NULL);
		CHECK(strstr(written->str, "<img") == NULL);
		fprintf(stderr, "stock written: %s\n", written->str);

		reset();
		o = parse(msg_tenor);
		discord_process_message(stock, o, DISCORD_MESSAGE_NORMAL);
		json_object_unref(o);
		CHECK(meta_count == 0);
		CHECK(strstr(written->str, "<font back=\"#cccccc\" color=\"#cccccc\"> </font> " TENOR_URL "<br/>") != NULL);
		CHECK(strstr(written->str, "<img") == NULL);

		reset();
		o = parse(msg_link);
		discord_process_message(stock, o, DISCORD_MESSAGE_NORMAL);
		json_object_unref(o);
		CHECK(strstr(written->str, "<a href=\"https://example.com/post?a=1&amp;b=2\">A &lt;title&gt;</a><br/>") != NULL);
		CHECK(strstr(written->str, "END") != NULL);                /* the whole description */
		CHECK(strstr(written->str, "<img") == NULL);
		fprintf(stderr, "stock written: %s\n", written->str);

		/* ... and an embed-only update is an EDIT: line, as before */
		reset();
		o = parse(msg_link_update);
		discord_process_dispatch(stock, "MESSAGE_UPDATE", o);
		json_object_unref(o);
		CHECK_STR(events->str, "");
		CHECK(meta_count == 0);
		CHECK(strstr(written->str, "|alice|EDIT: see ") != NULL);

		gc->proto_data = da;
		g_free(stock);
	}

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
#endif /* STOCK_DUMP */
