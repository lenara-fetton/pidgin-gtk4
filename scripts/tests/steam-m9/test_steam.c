/*
 * M9: tests for the native message metadata of the Steam plugin (see run.sh).
 * Includes libsteam.c, runs a real (system) libpurple core with a null UI
 * whose ui_info has message-meta = 1, registers the M8 conversation signals
 * itself (as pidgin4's libpurple does), and drives the plugin through a
 * test-mode CM session: protobuf packets go in with steam_cm__test_dispatch()
 * and whatever the plugin sends is collected by steam_cm__test_sent(). No
 * network and no accounts.
 */
#include "libsteam.c"
#include "steam_msgs.h"

SteamCM *steam_cm__test_new(SteamAccount *sa, const SteamCMCallbacks *callbacks,
                            gpointer user_data, guint64 steamid);
void steam_cm__test_dispatch(SteamCM *cm, const guint8 *data, gsize len);
GPtrArray *steam_cm__test_sent(SteamCM *cm);

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(a, b) do { const char *_a = (a), *_b = (b); checks++; \
	if (g_strcmp0(_a, _b) != 0) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s = \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #a, _a ? _a : "(null)", _b ? _b : "(null)"); } } while (0)
#define CHECK_HAS(hay, needle) do { const char *_h = (hay), *_n = (needle); checks++; \
	if (_h == NULL || strstr(_h, _n) == NULL) { failures++; \
	fprintf(stderr, "FAIL %s:%d: \"%s\" does not contain \"%s\"\n", __FILE__, __LINE__, _h ? _h : "(null)", _n); } } while (0)

/* ---- event loop / core ---- */

typedef struct { PurpleInputFunction f; gpointer data; } IoClosure;

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
	guint id;

	c->f = f;
	c->data = data;
	if (cond & PURPLE_INPUT_READ) gc |= G_IO_IN | G_IO_HUP | G_IO_ERR;
	if (cond & PURPLE_INPUT_WRITE) gc |= G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL;
	id = g_io_add_watch_full(ch, G_PRIORITY_DEFAULT, gc, io_invoke, c, g_free);
	g_io_channel_unref(ch);
	return id;
}

static PurpleEventLoopUiOps loop_ops = {
	g_timeout_add, g_source_remove, input_add, g_source_remove, NULL,
	g_timeout_add_seconds, NULL, NULL, NULL
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
	purple_signal_register(h, "sending-message-meta", purple_marshal_VOID__POINTER_POINTER_POINTER, NULL, 3,
		purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
		purple_value_new(PURPLE_TYPE_STRING), purple_value_new(PURPLE_TYPE_BOXED, "GHashTable *"));
	purple_signal_register(h, "message-reaction", purple_marshal_BOOLEAN__POINTER_POINTER_POINTER_POINTER_POINTER_POINTER,
		purple_value_new(PURPLE_TYPE_BOOLEAN), 6,
		purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
		purple_value_new(PURPLE_TYPE_STRING), purple_value_new(PURPLE_TYPE_STRING),
		purple_value_new(PURPLE_TYPE_STRING), purple_value_new(PURPLE_TYPE_STRING),
		purple_value_new(PURPLE_TYPE_BOOLEAN));
	purple_signal_register(h, "message-receipt", purple_marshal_BOOLEAN__POINTER_POINTER_POINTER_POINTER_POINTER,
		purple_value_new(PURPLE_TYPE_BOOLEAN), 5,
		purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
		purple_value_new(PURPLE_TYPE_STRING), purple_value_new(PURPLE_TYPE_STRING),
		purple_value_new(PURPLE_TYPE_STRING), purple_value_new(PURPLE_TYPE_STRING));
}

/* ---- recording handlers ---- */

static GString *metas;        /* "recv{k=v,...};" / "send{...};" in emission order */
static GHashTable *last_meta; /* copy of the last receiving-message-meta */
static int meta_count;
static GHashTable *seen_ids;  /* ids the "UI" has: it discards them */
static GString *events;
static GString *written;
static gboolean handle_events = TRUE;

static void
append_meta(const char *what, GHashTable *meta)
{
	GList *keys = g_list_sort(g_hash_table_get_keys(meta), (GCompareFunc) strcmp), *l;

	g_string_append_printf(metas, "%s{", what);
	for (l = keys; l; l = l->next)
		g_string_append_printf(metas, "%s%s=%s", l == keys ? "" : ",", (char *) l->data,
		                       (char *) g_hash_table_lookup(meta, l->data));
	g_string_append(metas, "};");
	g_list_free(keys);
}

static void
meta_cb(PurpleAccount *account, const char *conv_name, GHashTable *meta, gpointer data)
{
	GHashTableIter it;
	gpointer k, v;
	const char *id = g_hash_table_lookup(meta, "stanza-id");

	meta_count++;
	append_meta("recv", meta);
	g_hash_table_remove_all(last_meta);
	g_hash_table_iter_init(&it, meta);
	while (g_hash_table_iter_next(&it, &k, &v))
		g_hash_table_replace(last_meta, g_strdup(k), g_strdup(v));

	if (id != NULL && g_hash_table_contains(seen_ids, id))
		g_hash_table_replace(meta, g_strdup("discard"), g_strdup("1"));
	else if (id != NULL)
		g_hash_table_add(seen_ids, g_strdup(id));
}

static void
send_meta_cb(PurpleAccount *account, const char *conv_name, GHashTable *meta, gpointer data)
{
	append_meta("send", meta);
}

static gboolean
reaction_cb(PurpleAccount *a, const char *conv, const char *target, const char *emoji,
            const char *sender, gpointer add, gpointer data)
{
	g_string_append_printf(events, "reaction(%s,%s,%s,%s,%d);", conv, target, emoji, sender, GPOINTER_TO_INT(add));
	return handle_events;
}

static gboolean
receipt_cb(PurpleAccount *a, const char *conv, const char *id, const char *state,
           const char *sender, gpointer data)
{
	g_string_append_printf(events, "receipt(%s,%s,%s,%s);", conv, id, state, sender);
	return handle_events;
}

static GString *query_done;

static void
mam_done_cb(PurpleAccount *a, const char *conv, const char *first, const char *last,
            guint complete, gpointer data)
{
	g_string_append_printf(query_done, "done(%s,%s,%s,%u);", conv, first ? first : "NULL",
	                       last ? last : "NULL", complete);
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
	g_string_truncate(metas, 0);
	g_string_truncate(query_done, 0);
	g_hash_table_remove_all(last_meta);
	meta_count = 0;
}

#define M(key) ((const char *) g_hash_table_lookup(last_meta, key))

/* ---- the Steam session ---- */

#define SELF_ID    G_GUINT64_CONSTANT(76561197960287930)
#define FRIEND_ID  G_GUINT64_CONSTANT(76561197960265729)
#define FRIEND_STR "76561197960265729"
#define SESSION_ID 4242

static PurpleAccount *account;
static SteamAccount *sa;

static GByteArray *
packet(guint32 emsg, const SteamMsgProtoBufHeader *hdr, GByteArray *body)
{
	GByteArray *out = g_byte_array_new();

	steam_msg_packet_build(emsg, hdr, body, out);
	if (body)
		g_byte_array_unref(body);
	return out;
}

static void
feed(GByteArray *pkt)
{
	steam_cm__test_dispatch(sa->cm, pkt->data, pkt->len);
	g_byte_array_unref(pkt);
}

static GByteArray *
notification(const char *name, GByteArray *body)
{
	SteamMsgProtoBufHeader hdr;
	GByteArray *pkt;

	steam_msg_protobuf_header_init(&hdr);
	hdr.target_job_name = g_strdup(name);
	pkt = packet(STEAM_EMSG_SERVICE_METHOD, &hdr, body);
	steam_msg_protobuf_header_clear(&hdr);
	return pkt;
}

static GByteArray *
service_response(guint64 jobid, gint32 eresult, GByteArray *body)
{
	SteamMsgProtoBufHeader hdr;
	GByteArray *pkt;

	steam_msg_protobuf_header_init(&hdr);
	STEAM_MSG_SET(&hdr, jobid_target, jobid);
	STEAM_MSG_SET(&hdr, eresult, eresult);
	pkt = packet(STEAM_EMSG_SERVICE_METHOD_RESPONSE, &hdr, body);
	steam_msg_protobuf_header_clear(&hdr);
	return pkt;
}

static void
logon(void)
{
	SteamMsgProtoBufHeader hdr;
	SteamMsgClientLogonResponse m;
	GByteArray *body = g_byte_array_new();

	steam_msg_protobuf_header_init(&hdr);
	STEAM_MSG_SET(&hdr, steamid, SELF_ID);
	STEAM_MSG_SET(&hdr, client_sessionid, SESSION_ID);
	steam_msg_client_logon_response_init(&m);
	STEAM_MSG_SET(&m, eresult, STEAM_ERESULT_OK);
	STEAM_MSG_SET(&m, heartbeat_seconds, 9);
	steam_msg_client_logon_response_encode(&m, body);
	steam_msg_client_logon_response_clear(&m);
	feed(packet(STEAM_EMSG_CLIENT_LOG_ON_RESPONSE, &hdr, body));
	steam_msg_protobuf_header_clear(&hdr);
}

/* The last packet the plugin sent: its job name, job id and body */
static char *
sent_last(guint64 *jobid, GByteArray **body)
{
	GPtrArray *sent = steam_cm__test_sent(sa->cm);
	GByteArray *pkt;
	SteamMsgProtoBufHeader hdr;
	const guint8 *b = NULL;
	gsize len = 0;
	guint32 emsg = 0;
	char *name;

	if (sent->len == 0)
		return NULL;
	pkt = g_ptr_array_index(sent, sent->len - 1);
	steam_msg_protobuf_header_init(&hdr);
	if (!steam_msg_packet_parse(pkt->data, pkt->len, &emsg, &hdr, &b, &len)) {
		steam_msg_protobuf_header_clear(&hdr);
		return NULL;
	}
	if (jobid)
		*jobid = hdr.has_jobid_source ? hdr.jobid_source : 0;
	if (body) {
		*body = g_byte_array_new();
		g_byte_array_append(*body, b, len);
	}
	name = g_strdup(hdr.target_job_name ? hdr.target_job_name : "");
	steam_msg_protobuf_header_clear(&hdr);
	return name;
}

static guint
sent_count(void)
{
	return steam_cm__test_sent(sa->cm)->len;
}

static void
incoming(const char *bbcode, const char *plain, guint32 ts, guint32 ordinal, gboolean local_echo)
{
	SteamMsgFriendMessagesIncomingMessage m;
	GByteArray *body = g_byte_array_new();

	steam_msg_friend_messages_incoming_message_init(&m);
	STEAM_MSG_SET(&m, steamid_friend, FRIEND_ID);
	STEAM_MSG_SET(&m, chat_entry_type, STEAM_CHAT_ENTRY_CHAT_MSG);
	m.message = g_strdup(bbcode);
	m.message_no_bbcode = g_strdup(plain);
	STEAM_MSG_SET(&m, rtime32_server_timestamp, ts);
	if (ordinal)
		STEAM_MSG_SET(&m, ordinal, ordinal);
	if (local_echo)
		STEAM_MSG_SET(&m, local_echo, TRUE);
	steam_msg_friend_messages_incoming_message_encode(&m, body);
	steam_msg_friend_messages_incoming_message_clear(&m);
	feed(notification(STEAM_NOTIFY_FRIEND_MESSAGES_INCOMING_MESSAGE, body));
}

static SteamAccount *
steam_account_new_for_test(PurpleConnection *gc)
{
	SteamAccount *s = g_new0(SteamAccount, 1);

	/* As steam_login() sets it up */
	s->account = account;
	s->pc = gc;
	s->cookie_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	s->hostname_ip_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	s->sent_messages_hash = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	s->waiting_conns = g_queue_new();
	s->typing_sent = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	s->friend_requests = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	s->nicknames = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	s->icon_queue = g_queue_new();
	s->app_names = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
	s->live_message_since = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	s->steamid = SELF_ID;
	steam_native_init(s);
	return s;
}

static char *
hex(const GByteArray *b)
{
	GString *s = g_string_new(NULL);
	guint i;

	for (i = 0; b && i < b->len; i++)
		g_string_append_printf(s, "%02x", b->data[i]);
	return g_string_free(s, FALSE);
}

/* Checks that the last packet sent is a call of `method` whose body is
 * `expect_hex` (protoc's encoding; see the vectors below). Returns its job id. */
static guint64
check_sent(const char *method, const char *expect_hex)
{
	guint64 jobid = 0;
	GByteArray *body = NULL;
	char *name = sent_last(&jobid, &body);
	char *h = hex(body);

	CHECK_STR(name, method);
	if (expect_hex)
		CHECK_STR(h, expect_hex);
	g_free(h);
	g_free(name);
	if (body)
		g_byte_array_unref(body);
	return jobid;
}

/* Encodings from protoc's Python bindings for
 * SteamDatabase/Protobufs steammessages_friendmessages.steamclient.proto */
#define VEC_ACK "09010000000100100110e4e2cfaa06"   /* steamid_partner FRIEND, timestamp 1700000100 */
/* GetRecentMessages: steamid1 SELF, steamid2 FRIEND, count 51, most_recent_conversation
 * false, rtime32_start_time 0, bbcode_format, time_last 1700000100, ordinal_last 2 */
#define VEC_RECENT_OLDER "09ba56000001001001110100000001001001183320002d00000000300140e4e2cfaa064802"
/* UpdateMessageReaction: FRIEND, 1700000100, ordinal 2, emoticon "steamhappy", add */
#define VEC_REACT_ADD "09010000000100100110e4e2cfaa06180220012a0a737465616d68617070793001"
/* UpdateMessageReaction: FRIEND, 1700000100, emoticon "steamsad", remove */
#define VEC_REACT_REMOVE "09010000000100100110e4e2cfaa0620012a08737465616d7361643000"
/* MessageReaction notification: FRIEND, 1700000100, ordinal 2, reactor FRIEND,
 * emoticon "steamthumbsup", add */
#define VEC_REACT_NOTIFY "09010000000100100110e4e2cfaa0618022101000000010010012801320d737465616d7468756d627375703801"
/* GetRecentMessages response: [FRIEND 1700000050 "[emoticon]steamhappy[/emoticon]"
 * ordinal 0, reactions {emoticon steamthumbsup by SELF and FRIEND}],
 * [SELF 1700000040 "mine"], more_available */
#define VEC_RECENT_RESPONSE "0a44080110b2e2cfaa061a1f5b656d6f7469636f6e5d737465616d68617070795b2f656d6f7469636f6e5d20002a170801120d737465616d7468756d6273757018baad0118010a1008baad0110a8e2cfaa061a046d696e652001"

/* ---- feature 1: inline images and emoticons ---- */

static void
test_rich_text(void)
{
	char *h;

	/* BBCode form */
	h = steam_rich_to_html("hi [emoticon]steamhappy[/emoticon]!", TRUE, NULL);
	CHECK_STR(h, "hi <img src=\"https://steamcommunity-a.akamaihd.net/economy/emoticon/steamhappy\" alt=\":steamhappy:\">!");
	g_free(h);
	h = steam_rich_to_html("look [img src=\"https://images.steamusercontent.com/ugc/123/ABC/\" width=640 height=480][/img] nice", TRUE, NULL);
	CHECK_STR(h, "look <a href=\"https://images.steamusercontent.com/ugc/123/ABC/\">https://images.steamusercontent.com/ugc/123/ABC/</a>"
	             "<br/><img src=\"https://images.steamusercontent.com/ugc/123/ABC/\"> nice");
	g_free(h);
	h = steam_rich_to_html("[url=https://images.steamusercontent.com/ugc/9/X/?a=1&b=2]my pic[/url]", TRUE, NULL);
	CHECK_STR(h, "<a href=\"https://images.steamusercontent.com/ugc/9/X/?a=1&amp;b=2\">my pic</a>"
	             "<br/><img src=\"https://images.steamusercontent.com/ugc/9/X/?a=1&amp;b=2\">");
	g_free(h);
	h = steam_rich_to_html("[url]https://steamusercontent-a.akamaihd.net/ugc/1/2/[/url]", TRUE, NULL);
	CHECK_HAS(h, "<br/><img src=\"https://steamusercontent-a.akamaihd.net/ugc/1/2/\">");
	g_free(h);
	/* A link elsewhere: just the link, no image */
	h = steam_rich_to_html("[url=https://example.com/x.png]x[/url]", TRUE, NULL);
	CHECK_STR(h, "<a href=\"https://example.com/x.png\">x</a>");
	g_free(h);
	/* Userinfo/port tricks and plain http are not image hosts */
	h = steam_rich_to_html("[img src=https://images.steamusercontent.com@evil.example/a][/img]", TRUE, NULL);
	CHECK(strstr(h, "<img") == NULL);
	g_free(h);
	h = steam_rich_to_html("[img src=http://images.steamusercontent.com/a][/img]", TRUE, NULL);
	CHECK(strstr(h, "<img") == NULL);
	g_free(h);
	/* Escaped brackets, formatting tags, stickers, markup in text */
	h = steam_rich_to_html("\\[b] is literal, [b]bold[/b] <x> & [spoiler]s[/spoiler]", TRUE, NULL);
	CHECK_STR(h, "[b] is literal, bold &lt;x&gt; &amp; s");
	g_free(h);
	h = steam_rich_to_html("[sticker type=\"Winter2019SnowmanWave\" limit=\"0\"][/sticker]", TRUE, NULL);
	CHECK_STR(h, "[sticker: Winter2019SnowmanWave]");
	g_free(h);
	h = steam_rich_to_html("a\nb [unclosed", TRUE, NULL);
	CHECK_STR(h, "a<br>b [unclosed");
	g_free(h);
	h = steam_rich_to_html("[emoticon]bad name![/emoticon]", TRUE, NULL);
	CHECK_STR(h, "bad name!");
	g_free(h);
	/* Nothing left to show: the plain form */
	h = steam_rich_to_html("[gameinvite appid=\"440\"][/gameinvite]", TRUE, "invited you to play");
	CHECK_STR(h, "invited you to play");
	g_free(h);

	/* Plain form: U+02D0 tokens, :name:, bare image URLs */
	h = steam_rich_to_html("\xcb\x90steamhappy\xcb\x90 and :steamsad: at 10:30:45, a:b:c", FALSE, NULL);
	CHECK_STR(h, "<img src=\"https://steamcommunity-a.akamaihd.net/economy/emoticon/steamhappy\" alt=\":steamhappy:\"> and "
	             "<img src=\"https://steamcommunity-a.akamaihd.net/economy/emoticon/steamsad\" alt=\":steamsad:\"> at 10:30:45, a:b:c");
	g_free(h);
	h = steam_rich_to_html("see https://images.steamusercontent.com/ugc/5/F/. and https://example.com/", FALSE, NULL);
	CHECK_STR(h, "see <a href=\"https://images.steamusercontent.com/ugc/5/F/\">https://images.steamusercontent.com/ugc/5/F/</a>"
	             "<br/><img src=\"https://images.steamusercontent.com/ugc/5/F/\">. and https://example.com/");
	g_free(h);
	h = steam_rich_to_html(":P: :x: \"q\"", FALSE, NULL);
	CHECK_STR(h, ":P: :x: &quot;q&quot;");
	g_free(h);
}

static void
test_rich_messages(void)
{
	const char *bb = "[emoticon]steamhappy[/emoticon] look [img src=\"https://images.steamusercontent.com/ugc/1/AB/\"][/img]";
	const char *plain = "\xcb\x90steamhappy\xcb\x90 look https://images.steamusercontent.com/ugc/1/AB/";

	/* message-meta UI */
	reset();
	incoming(bb, plain, 1700000100, 0, FALSE);
	CHECK_HAS(written->str, "[" FRIEND_STR "|" FRIEND_STR "|<img src=\"https://steamcommunity-a.akamaihd.net/economy/emoticon/steamhappy\" "
	          "alt=\":steamhappy:\"> look <a href=\"https://images.steamusercontent.com/ugc/1/AB/\">");
	CHECK_HAS(written->str, "<br/><img src=\"https://images.steamusercontent.com/ugc/1/AB/\">|0x2]");

	/* Sent from another client of ours: the same treatment */
	reset();
	incoming("[emoticon]steamsad[/emoticon]", "\xcb\x90steamsad\xcb\x90", 1700000101, 0, TRUE);
	CHECK_HAS(written->str, "alt=\":steamsad:\">|0x1]");

	/* Stock UI: the old text, byte for byte */
	sa->native_meta = FALSE;
	reset();
	incoming(bb, plain, 1700000102, 0, FALSE);
	CHECK_STR(written->str, "[" FRIEND_STR "|" FRIEND_STR "|\xcb\x90steamhappy\xcb\x90 look "
	          "https://images.steamusercontent.com/ugc/1/AB/|0x2]");
	CHECK(meta_count == 0);
	sa->native_meta = TRUE;
}

/* ---- feature 2: ids, read markers, own sends ---- */

static void
test_ids_and_markers(PurplePlugin *prpl)
{
	guint64 f = 0, jobid;
	guint32 ts = 0, ord = 9;
	char *id;
	gboolean ok = FALSE;
	guint n;

	/* ids */
	id = steam_message_id(FRIEND_ID, 1700000100, 0);
	CHECK_STR(id, FRIEND_STR ":1700000100");
	CHECK(steam_message_id_parse(id, &f, &ts, &ord) && f == FRIEND_ID && ts == 1700000100 && ord == 0);
	g_free(id);
	id = steam_message_id(FRIEND_ID, 1700000100, 3);
	CHECK_STR(id, FRIEND_STR ":1700000100:3");
	CHECK(steam_message_id_parse(id, &f, &ts, &ord) && ord == 3);
	g_free(id);
	CHECK(!steam_message_id_parse("123:456", &f, &ts, &ord));
	CHECK(!steam_message_id_parse(FRIEND_STR ":0", &f, &ts, &ord));
	CHECK(!steam_message_id_parse(FRIEND_STR ":99999999999", &f, &ts, &ord));
	CHECK(!steam_message_id_parse(FRIEND_STR ":1:2:3", &f, &ts, &ord));
	CHECK(!steam_message_id_parse(FRIEND_STR ":1x", &f, &ts, &ord));
	CHECK(!steam_message_id_parse(NULL, &f, &ts, &ord));

	/* Live incoming: ids, markable */
	g_hash_table_remove_all(seen_ids);
	reset();
	incoming("hello", "hello", 1700000100, 0, FALSE);
	CHECK(meta_count == 1);
	CHECK_STR(M("conv-type"), "im");
	CHECK_STR(M("sender"), FRIEND_STR);
	CHECK_STR(M("timestamp"), "1700000100");
	CHECK_STR(M("stanza-id"), FRIEND_STR ":1700000100");
	CHECK_STR(M("server-id"), FRIEND_STR ":1700000100");
	CHECK_STR(M("markable"), "1");
	CHECK(M("outgoing") == NULL && M("mam") == NULL);
	CHECK_STR(written->str, "[" FRIEND_STR "|" FRIEND_STR "|hello|0x2]");
	/* Same second, next ordinal */
	reset();
	incoming("again", "again", 1700000100, 2, FALSE);
	CHECK_STR(M("stanza-id"), FRIEND_STR ":1700000100:2");
	/* The UI has it already: dropped */
	reset();
	incoming("hello", "hello", 1700000100, 0, FALSE);
	CHECK(meta_count == 1);
	CHECK_STR(written->str, "");
	/* Our own message from another client */
	reset();
	incoming("from my phone", "from my phone", 1700000110, 0, TRUE);
	CHECK_STR(M("outgoing"), "1");
	CHECK_STR(M("sender"), "me");
	CHECK(M("markable") == NULL);
	CHECK_STR(M("stanza-id"), FRIEND_STR ":1700000110");
	CHECK_STR(written->str, "[" FRIEND_STR "|me|from my phone|0x1]");

	/* Read on another session of ours -> message-receipt */
	{
		SteamMsgFriendMessagesAckMessage m;
		GByteArray *body = g_byte_array_new();

		steam_msg_friend_messages_ack_message_init(&m);
		STEAM_MSG_SET(&m, steamid_partner, FRIEND_ID);
		STEAM_MSG_SET(&m, timestamp, 1700000100);
		steam_msg_friend_messages_ack_message_encode(&m, body);
		{
			char *h = hex(body);
			CHECK_STR(h, VEC_ACK);
			g_free(h);
		}
		reset();
		feed(notification(STEAM_NOTIFY_FRIEND_MESSAGES_ACK_ECHO, body));
		CHECK_STR(events->str, "receipt(" FRIEND_STR "," FRIEND_STR ":1700000100,displayed,me);");
	}

	/* send-marker -> FriendMessages.AckMessage */
	CHECK(purple_plugin_ipc_get_params(prpl, "send-marker", NULL, NULL, NULL));
	n = sent_count();
	CHECK(GPOINTER_TO_INT(purple_plugin_ipc_call(prpl, "send-marker", &ok, account, FRIEND_STR,
	                                             FRIEND_STR ":1700000100", "displayed")));
	CHECK(ok);
	CHECK(sent_count() == n + 1);
	check_sent(STEAM_METHOD_FRIEND_MESSAGES_ACK_MESSAGE, VEC_ACK);
	CHECK(steam_ipc_send_marker(account, FRIEND_STR, FRIEND_STR ":1700000100:2", NULL));
	CHECK(steam_ipc_send_marker(account, FRIEND_STR, FRIEND_STR ":1700000100", "acknowledged"));
	n = sent_count();
	CHECK(!steam_ipc_send_marker(account, FRIEND_STR, FRIEND_STR ":1700000100", "received"));
	CHECK(!steam_ipc_send_marker(account, "76561197960265730", FRIEND_STR ":1700000100", "displayed"));
	CHECK(!steam_ipc_send_marker(account, FRIEND_STR, "garbage", "displayed"));
	CHECK(!steam_ipc_send_marker(NULL, FRIEND_STR, FRIEND_STR ":1700000100", "displayed"));
	CHECK(sent_count() == n);

	/* Own send: shown with its ids once Steam has it */
	reset();
	CHECK(steam_send_im(sa->pc, FRIEND_STR, "hi :steamhappy: &lt;3", 0) == 0);
	CHECK_STR(written->str, "");
	jobid = check_sent(STEAM_METHOD_FRIEND_MESSAGES_SEND_MESSAGE, NULL);
	{
		SteamMsgFriendMessagesSendMessageResponse r;
		GByteArray *body = g_byte_array_new();

		steam_msg_friend_messages_send_message_response_init(&r);
		STEAM_MSG_SET(&r, server_timestamp, 1700000200);
		STEAM_MSG_SET(&r, ordinal, 1);
		steam_msg_friend_messages_send_message_response_encode(&r, body);
		steam_msg_friend_messages_send_message_response_clear(&r);
		feed(service_response(jobid, STEAM_ERESULT_OK, body));
	}
	CHECK_STR(metas->str, "send{conv-type=im,server-id=" FRIEND_STR ":1700000200:1,stanza-id="
	          FRIEND_STR ":1700000200:1,timestamp=1700000200};");
	CHECK_STR(written->str, "[" FRIEND_STR "|me|hi <img src=\"https://steamcommunity-a.akamaihd.net/economy/emoticon/steamhappy\" "
	          "alt=\":steamhappy:\"> &lt;3|0x1]");
	/* ... and a failure keeps the text */
	reset();
	CHECK(steam_send_im(sa->pc, FRIEND_STR, "lost words", 0) == 0);
	jobid = check_sent(STEAM_METHOD_FRIEND_MESSAGES_SEND_MESSAGE, NULL);
	feed(service_response(jobid, STEAM_ERESULT_FAIL, g_byte_array_new()));
	CHECK_STR(metas->str, "");
	CHECK_HAS(written->str, "Message could not be sent (Generic failure): lost words|0x");

	/* Stock UI: no metadata, the local echo as before, no receipts */
	sa->native_meta = FALSE;
	reset();
	incoming("stock", "stock", 1700000300, 0, FALSE);
	CHECK(meta_count == 0);
	CHECK_STR(written->str, "[" FRIEND_STR "|" FRIEND_STR "|stock|0x2]");
	CHECK(steam_send_im(sa->pc, FRIEND_STR, "stock send", 0) == 1);
	jobid = check_sent(STEAM_METHOD_FRIEND_MESSAGES_SEND_MESSAGE, NULL);
	feed(service_response(jobid, STEAM_ERESULT_OK, g_byte_array_new()));
	CHECK_STR(metas->str, "");
	CHECK_STR(written->str, "[" FRIEND_STR "|" FRIEND_STR "|stock|0x2]");
	{
		SteamMsgFriendMessagesAckMessage m;
		GByteArray *body = g_byte_array_new();

		steam_msg_friend_messages_ack_message_init(&m);
		STEAM_MSG_SET(&m, steamid_partner, FRIEND_ID);
		STEAM_MSG_SET(&m, timestamp, 1700000100);
		steam_msg_friend_messages_ack_message_encode(&m, body);
		feed(notification(STEAM_NOTIFY_FRIEND_MESSAGES_ACK_ECHO, body));
		CHECK_STR(events->str, "");
	}
	CHECK(!steam_ipc_send_marker(account, FRIEND_STR, FRIEND_STR ":1700000100", "displayed"));
	sa->native_meta = TRUE;
}

/* ---- feature 3: scroll-back ---- */

static void
add_history(SteamMsgFriendMessagesGetRecentMessagesResponse *r, guint32 accountid, guint32 ts,
            guint32 ordinal, const char *text)
{
	SteamMsgFriendMessage fm;

	memset(&fm, 0, sizeof(fm));
	STEAM_MSG_SET(&fm, accountid, accountid);
	STEAM_MSG_SET(&fm, timestamp, ts);
	if (ordinal)
		STEAM_MSG_SET(&fm, ordinal, ordinal);
	fm.message = g_strdup(text);
	g_array_append_val(r->messages, fm);
}

static void
test_scrollback(PurplePlugin *prpl)
{
	SteamMsgFriendMessagesGetRecentMessagesResponse r;
	SteamMsgFriendMessagesGetRecentMessagesRequest q;
	GByteArray *body;
	guint64 jobid;
	gboolean ok = FALSE;
	guint32 self_acct = steam_cm_steamid_to_accountid(SELF_ID);
	guint32 friend_acct = steam_cm_steamid_to_accountid(FRIEND_ID);
	guint n;

	CHECK(purple_plugin_ipc_get_params(prpl, "mam-fetch-older", NULL, NULL, NULL));

	/* A page before FRIEND:1700000100:2 */
	reset();
	CHECK(GPOINTER_TO_INT(purple_plugin_ipc_call(prpl, "mam-fetch-older", &ok, account, FRIEND_STR,
	                                             FRIEND_STR ":1700000100:2", 50)));
	CHECK(ok);
	jobid = check_sent(STEAM_METHOD_FRIEND_MESSAGES_GET_RECENT_MESSAGES, VEC_RECENT_OLDER);
	CHECK(g_slist_length(sa->older_fetches) == 1);

	/* Newest first; Steam may include the before message itself */
	steam_msg_friend_messages_get_recent_messages_response_init(&r);
	add_history(&r, friend_acct, 1700000100, 2, "the before message");
	add_history(&r, friend_acct, 1700000050, 0, "[emoticon]steamhappy[/emoticon] old");
	add_history(&r, self_acct, 1700000040, 0, "mine [url]https://images.steamusercontent.com/ugc/7/Z/[/url]");
	STEAM_MSG_SET(&r, more_available, TRUE);
	body = g_byte_array_new();
	steam_msg_friend_messages_get_recent_messages_response_encode(&r, body);
	steam_msg_friend_messages_get_recent_messages_response_clear(&r);
	feed(service_response(jobid, STEAM_ERESULT_OK, body));

	CHECK(g_slist_length(sa->older_fetches) == 0);
	CHECK_STR(written->str,
		"[" FRIEND_STR "|me|mine <a href=\"https://images.steamusercontent.com/ugc/7/Z/\">https://images.steamusercontent.com/ugc/7/Z/</a>"
		"<br/><img src=\"https://images.steamusercontent.com/ugc/7/Z/\">|0x401]"
		"[" FRIEND_STR "|" FRIEND_STR "|<img src=\"https://steamcommunity-a.akamaihd.net/economy/emoticon/steamhappy\" alt=\":steamhappy:\"> old|0x402]");
	CHECK_STR(metas->str,
		"recv{conv-type=im,mam=1,mam-query=older,outgoing=1,sender=me,server-id=" FRIEND_STR ":1700000040,"
		"stanza-id=" FRIEND_STR ":1700000040,timestamp=1700000040};"
		"recv{conv-type=im,mam=1,mam-query=older,markable=1,sender=" FRIEND_STR ",server-id=" FRIEND_STR ":1700000050,"
		"stanza-id=" FRIEND_STR ":1700000050,timestamp=1700000050};");
	CHECK_STR(query_done->str, "done(" FRIEND_STR "," FRIEND_STR ":1700000040," FRIEND_STR ":1700000050,0);");

	/* The next page (from first_id), with fewer than asked: complete; one the UI has */
	reset();
	g_hash_table_add(seen_ids, g_strdup(FRIEND_STR ":1700000030"));
	CHECK(steam_ipc_mam_fetch_older(account, FRIEND_STR, FRIEND_STR ":1700000040", 3));
	jobid = check_sent(STEAM_METHOD_FRIEND_MESSAGES_GET_RECENT_MESSAGES, NULL);
	steam_msg_friend_messages_get_recent_messages_response_init(&r);
	add_history(&r, friend_acct, 1700000030, 0, "known");
	add_history(&r, friend_acct, 1700000020, 0, "first ever");
	body = g_byte_array_new();
	steam_msg_friend_messages_get_recent_messages_response_encode(&r, body);
	steam_msg_friend_messages_get_recent_messages_response_clear(&r);
	feed(service_response(jobid, STEAM_ERESULT_OK, body));
	CHECK_STR(written->str, "[" FRIEND_STR "|" FRIEND_STR "|first ever|0x402]");
	CHECK_STR(query_done->str, "done(" FRIEND_STR "," FRIEND_STR ":1700000020," FRIEND_STR ":1700000030,1);");

	/* More than asked for: trimmed to the newest, not complete */
	reset();
	CHECK(steam_ipc_mam_fetch_older(account, FRIEND_STR, FRIEND_STR ":1700000040", 1));
	jobid = check_sent(STEAM_METHOD_FRIEND_MESSAGES_GET_RECENT_MESSAGES, NULL);
	steam_msg_friend_messages_get_recent_messages_response_init(&r);
	add_history(&r, friend_acct, 1700000031, 0, "newer");
	add_history(&r, friend_acct, 1700000021, 0, "older");
	body = g_byte_array_new();
	steam_msg_friend_messages_get_recent_messages_response_encode(&r, body);
	steam_msg_friend_messages_get_recent_messages_response_clear(&r);
	feed(service_response(jobid, STEAM_ERESULT_OK, body));
	CHECK_STR(written->str, "[" FRIEND_STR "|" FRIEND_STR "|newer|0x402]");
	CHECK_STR(query_done->str, "done(" FRIEND_STR "," FRIEND_STR ":1700000031," FRIEND_STR ":1700000031,0);");

	/* The newest page: no time bound but "now"; an empty answer is complete */
	reset();
	CHECK(steam_ipc_mam_fetch_older(account, FRIEND_STR, NULL, 500));
	{
		GByteArray *b = NULL;
		char *name = sent_last(&jobid, &b);

		steam_msg_friend_messages_get_recent_messages_request_init(&q);
		CHECK(steam_msg_friend_messages_get_recent_messages_request_decode(&q, b->data, b->len));
		CHECK(q.count == 100 && q.time_last == G_MAXINT32 && !q.has_ordinal_last && q.bbcode_format &&
		      q.steamid2 == FRIEND_ID && q.steamid1 == SELF_ID);
		steam_msg_friend_messages_get_recent_messages_request_clear(&q);
		g_byte_array_unref(b);
		g_free(name);
	}
	body = g_byte_array_new();
	feed(service_response(jobid, STEAM_ERESULT_OK, body));
	CHECK_STR(query_done->str, "done(" FRIEND_STR ",NULL,NULL,1);");

	/* A failed request: no done (the UI times out and can ask again) */
	reset();
	CHECK(steam_ipc_mam_fetch_older(account, FRIEND_STR, "", 10));
	jobid = check_sent(STEAM_METHOD_FRIEND_MESSAGES_GET_RECENT_MESSAGES, NULL);
	feed(service_response(jobid, STEAM_ERESULT_FAIL, g_byte_array_new()));
	CHECK_STR(query_done->str, "");
	CHECK_STR(written->str, "");

	/* Bad arguments */
	n = sent_count();
	CHECK(!steam_ipc_mam_fetch_older(account, "76561197960265730", FRIEND_STR ":1700000040", 10));
	CHECK(!steam_ipc_mam_fetch_older(account, "nobody", NULL, 10));
	CHECK(!steam_ipc_mam_fetch_older(account, FRIEND_STR, "junk", 10));
	CHECK(!steam_ipc_mam_fetch_older(NULL, FRIEND_STR, NULL, 10));
	CHECK(sent_count() == n);

	/* The sign-on catch-up carries mam-query=catchup */
	{
		SteamCMHistoryMessage h[2];

		memset(h, 0, sizeof(h));
		h[0].accountid = friend_acct;
		h[0].timestamp = 1700000500;
		h[0].message = "while [emoticon]steamsad[/emoticon] away";
		h[1].accountid = self_acct;
		h[1].timestamp = 1700000400;
		h[1].message = "bye";
		sa->history_since = 1700000300;
		g_hash_table_remove_all(sa->live_message_since);
		reset();
		steam_got_history_cb(sa->cm, FRIEND_ID, h, 2, FALSE, sa);
		CHECK_HAS(metas->str, "mam=1,mam-query=catchup,outgoing=1,sender=me,server-id=" FRIEND_STR ":1700000400,");
		CHECK_HAS(metas->str, "mam=1,mam-query=catchup,markable=1,sender=" FRIEND_STR ",server-id=" FRIEND_STR ":1700000500,");
		CHECK_STR(written->str, "[" FRIEND_STR "|me|bye|0x401][" FRIEND_STR "|" FRIEND_STR
		          "|while <img src=\"https://steamcommunity-a.akamaihd.net/economy/emoticon/steamsad\" alt=\":steamsad:\"> away|0x402]");

		/* Stock: the old lines, no metadata */
		sa->native_meta = FALSE;
		reset();
		h[0].timestamp = 1700000501;
		h[0].message = "while \xcb\x90steamsad\xcb\x90 away";
		h[1].timestamp = 1700000401;
		steam_got_history_cb(sa->cm, FRIEND_ID, h, 2, FALSE, sa);
		CHECK(meta_count == 0);
		CHECK_STR(written->str, "[" FRIEND_STR "|me|bye|0x401][" FRIEND_STR "|" FRIEND_STR
		          "|while \xcb\x90steamsad\xcb\x90 away|0x402]");
		CHECK(!steam_ipc_mam_fetch_older(account, FRIEND_STR, NULL, 10));
		sa->native_meta = TRUE;
	}
}

/* ---- feature 4: reactions ---- */

static GByteArray *
unhex(const char *h)
{
	GByteArray *b = g_byte_array_new();
	gsize i, n = strlen(h);

	for (i = 0; i + 1 < n; i += 2) {
		char byte[3] = { h[i], h[i + 1], 0 };
		guint8 v = (guint8) strtoul(byte, NULL, 16);

		g_byte_array_append(b, &v, 1);
	}
	return b;
}

static void
reaction_notify(guint64 reactor, guint32 ts, guint32 ordinal, gint32 type, const char *name, gboolean add,
                const char *expect_hex)
{
	SteamMsgFriendMessagesMessageReaction m;
	GByteArray *body = g_byte_array_new();

	steam_msg_friend_messages_message_reaction_init(&m);
	STEAM_MSG_SET(&m, steamid_friend, FRIEND_ID);
	STEAM_MSG_SET(&m, server_timestamp, ts);
	if (ordinal)
		STEAM_MSG_SET(&m, ordinal, ordinal);
	STEAM_MSG_SET(&m, reactor, reactor);
	STEAM_MSG_SET(&m, reaction_type, type);
	m.reaction = g_strdup(name);
	STEAM_MSG_SET(&m, is_add, add);
	steam_msg_friend_messages_message_reaction_encode(&m, body);
	steam_msg_friend_messages_message_reaction_clear(&m);
	if (expect_hex) {
		char *h = hex(body);
		CHECK_STR(h, expect_hex);
		g_free(h);
	}
	feed(notification(STEAM_NOTIFY_FRIEND_MESSAGES_MESSAGE_REACTION, body));
}

static void
test_reactions(PurplePlugin *prpl)
{
	SteamCMReactionType type;
	gboolean ok = FALSE;
	guint64 jobid;
	char *name;
	guint n;

	/* Representation */
	name = steam_reaction_to_emoji(STEAM_CM_REACTION_EMOTICON, "steamhappy");
	CHECK_STR(name, ":steamhappy:");
	g_free(name);
	name = steam_reaction_to_emoji(STEAM_CM_REACTION_STICKER, "Winter2019SnowmanWave");
	CHECK_STR(name, "sticker:Winter2019SnowmanWave");
	g_free(name);
	CHECK(steam_reaction_to_emoji(STEAM_CM_REACTION_EMOTICON, "a b") == NULL);
	CHECK(steam_reaction_to_emoji(7, "x") == NULL);
	name = steam_reaction_from_emoji(":steamhappy:", &type);
	CHECK_STR(name, "steamhappy");
	CHECK(type == STEAM_CM_REACTION_EMOTICON);
	g_free(name);
	name = steam_reaction_from_emoji("sticker:Cozy", &type);
	CHECK(name != NULL && type == STEAM_CM_REACTION_STICKER);
	g_free(name);
	CHECK(steam_reaction_from_emoji("\xf0\x9f\x91\x8d", &type) == NULL);
	CHECK(steam_reaction_from_emoji("::", &type) == NULL);
	CHECK(steam_reaction_from_emoji(":a:b:", &type) == NULL);

	/* The friend reacts */
	reset();
	reaction_notify(FRIEND_ID, 1700000100, 2, STEAM_REACTION_TYPE_EMOTICON, "steamthumbsup", TRUE, VEC_REACT_NOTIFY);
	CHECK_STR(events->str, "reaction(" FRIEND_STR "," FRIEND_STR ":1700000100:2,:steamthumbsup:," FRIEND_STR ",1);");
	CHECK_STR(written->str, "");
	/* ... and the UI doesn't take it: a line in the open conversation */
	handle_events = FALSE;
	reset();
	reaction_notify(FRIEND_ID, 1700000100, 2, STEAM_REACTION_TYPE_EMOTICON, "steamthumbsup", FALSE, NULL);
	CHECK_HAS(written->str, "removed the reaction :steamthumbsup:|0x");
	handle_events = TRUE;
	/* We react on another session: reported once */
	reset();
	reaction_notify(SELF_ID, 1700000100, 0, STEAM_REACTION_TYPE_EMOTICON, "steamsad", TRUE, NULL);
	reaction_notify(SELF_ID, 1700000100, 0, STEAM_REACTION_TYPE_EMOTICON, "steamsad", TRUE, NULL);
	CHECK_STR(events->str, "reaction(" FRIEND_STR "," FRIEND_STR ":1700000100,:steamsad:,me,1);");

	/* send-reaction: the complete set -> adds and removes */
	CHECK(purple_plugin_ipc_get_params(prpl, "send-reaction", NULL, NULL, NULL));
	reset();
	CHECK(GPOINTER_TO_INT(purple_plugin_ipc_call(prpl, "send-reaction", &ok, account, FRIEND_STR,
	                                             FRIEND_STR ":1700000100:2", ":steamhappy:")));
	CHECK(ok);
	jobid = check_sent(STEAM_METHOD_FRIEND_MESSAGES_UPDATE_REACTION, VEC_REACT_ADD);
	CHECK_STR(events->str, "reaction(" FRIEND_STR "," FRIEND_STR ":1700000100:2,:steamhappy:,me,1);");
	feed(service_response(jobid, STEAM_ERESULT_OK, g_byte_array_new()));
	/* The server's echo of it is no news */
	reset();
	reaction_notify(SELF_ID, 1700000100, 2, STEAM_REACTION_TYPE_EMOTICON, "steamhappy", TRUE, NULL);
	CHECK_STR(events->str, "");
	/* Unchanged set: nothing to send */
	n = sent_count();
	CHECK(steam_ipc_send_reaction(account, FRIEND_STR, FRIEND_STR ":1700000100:2", ":steamhappy:"));
	CHECK(sent_count() == n);
	/* steamsad (from the other session) removed */
	reset();
	CHECK(steam_ipc_send_reaction(account, FRIEND_STR, FRIEND_STR ":1700000100", ""));
	CHECK(sent_count() == n + 1);
	jobid = check_sent(STEAM_METHOD_FRIEND_MESSAGES_UPDATE_REACTION, VEC_REACT_REMOVE);
	CHECK_STR(events->str, "reaction(" FRIEND_STR "," FRIEND_STR ":1700000100,:steamsad:,me,0);");
	feed(service_response(jobid, STEAM_ERESULT_OK, g_byte_array_new()));
	/* Refused by Steam: undone */
	reset();
	CHECK(steam_ipc_send_reaction(account, FRIEND_STR, FRIEND_STR ":1700000100", ":notmine: sticker:Cozy"));
	CHECK(sent_count() == n + 3);
	jobid = check_sent(STEAM_METHOD_FRIEND_MESSAGES_UPDATE_REACTION, NULL);
	feed(service_response(jobid, STEAM_ERESULT_ACCESS_DENIED, g_byte_array_new()));
	CHECK_HAS(events->str, "reaction(" FRIEND_STR "," FRIEND_STR ":1700000100,:notmine:,me,1);");
	CHECK_HAS(events->str, "reaction(" FRIEND_STR "," FRIEND_STR ":1700000100,sticker:Cozy,me,1);");
	CHECK_HAS(events->str, ",me,0);");   /* the refused one (sent last) taken back */
	CHECK(g_slist_length(sa->reaction_updates) == 1);
	/* Not Steam reactions, bad targets */
	n = sent_count();
	CHECK(!steam_ipc_send_reaction(account, FRIEND_STR, FRIEND_STR ":1700000100", "\xf0\x9f\x91\x8d"));
	CHECK(!steam_ipc_send_reaction(account, FRIEND_STR, FRIEND_STR ":1700000100", ":ok: :bad name"));
	CHECK(!steam_ipc_send_reaction(account, "76561197960265730", FRIEND_STR ":1700000100", ":x1:"));
	CHECK(!steam_ipc_send_reaction(account, FRIEND_STR, "x", ":x1:"));
	CHECK(sent_count() == n);

	/* Reactions listed on history messages (protoc's encoding of the response) */
	{
		SteamMsgFriendMessagesGetRecentMessagesResponse r;
		GByteArray *vec = unhex(VEC_RECENT_RESPONSE), *again = g_byte_array_new();
		char *h;

		steam_msg_friend_messages_get_recent_messages_response_init(&r);
		CHECK(steam_msg_friend_messages_get_recent_messages_response_decode(&r, vec->data, vec->len));
		CHECK(r.messages->len == 2);
		CHECK(g_array_index(r.messages, SteamMsgFriendMessage, 0).reactions != NULL &&
		      g_array_index(r.messages, SteamMsgFriendMessage, 0).reactions->len == 1);
		steam_msg_friend_messages_get_recent_messages_response_encode(&r, again);
		h = hex(again);
		CHECK_STR(h, VEC_RECENT_RESPONSE);
		g_free(h);
		steam_msg_friend_messages_get_recent_messages_response_clear(&r);
		g_byte_array_unref(again);

		reset();
		CHECK(steam_ipc_mam_fetch_older(account, FRIEND_STR, FRIEND_STR ":1700000060", 10));
		jobid = check_sent(STEAM_METHOD_FRIEND_MESSAGES_GET_RECENT_MESSAGES, NULL);
		feed(service_response(jobid, STEAM_ERESULT_OK, vec));
		CHECK_STR(events->str, "reaction(" FRIEND_STR "," FRIEND_STR ":1700000050,:steamthumbsup:,me,1);"
		          "reaction(" FRIEND_STR "," FRIEND_STR ":1700000050,:steamthumbsup:," FRIEND_STR ",1);");
		CHECK(g_hash_table_contains(steam_own_reactions(sa, FRIEND_STR ":1700000050", FALSE), ":steamthumbsup:"));
	}

	/* Stock UI: nothing new */
	sa->native_meta = FALSE;
	reset();
	reaction_notify(FRIEND_ID, 1700000100, 2, STEAM_REACTION_TYPE_EMOTICON, "steamhappy", TRUE, NULL);
	CHECK_STR(events->str, "");
	CHECK_STR(written->str, "");
	CHECK(!steam_ipc_send_reaction(account, FRIEND_STR, FRIEND_STR ":1700000100", ":x1:"));
	sa->native_meta = TRUE;
}

/* ---- feature 5: game image ---- */

/* appdetails?appids=440&filters=basic, trimmed (the store API's shape) */
static const char *appdetails_440 =
	"{\"440\":{\"success\":true,\"data\":{\"type\":\"game\",\"name\":\"Team Fortress 2\",\"steam_appid\":440,"
	"\"header_image\":\"https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/440/header.jpg?t=1757348372\","
	"\"capsule_image\":\"https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/440/capsule_231x87.jpg?t=1757348372\","
	"\"capsule_imagev5\":\"https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/440/capsule_184x69.jpg?t=1757348372\"}}}";

static const char *
game_attr(const char *attr)
{
	PurpleBuddy *b = purple_find_buddy(account, FRIEND_STR);
	PurpleStatus *st = b ? purple_presence_get_status(purple_buddy_get_presence(b), "ingame") : NULL;

	if (st == NULL || !purple_status_is_active(st))
		return NULL;
	return purple_status_get_attr_string(st, attr);
}

static void
app_details(guint32 appid, const char *json)
{
	SteamAppFetch *fetch = g_new0(SteamAppFetch, 1);

	fetch->sa = sa;
	fetch->appid = appid;
	steam_got_app_name_cb(NULL, fetch, json, strlen(json), NULL);
}

static void
test_game_image(void)
{
	PurpleBuddy *b = purple_buddy_new(account, FRIEND_STR, NULL);
	SteamBuddy *sb;
	SteamCMPersona persona;
	char *u;

	purple_blist_add_buddy(b, NULL, steam_get_buddy_group(), NULL);
	sb = steam_buddy_get_or_create(sa, b);
	sb->relationship = STEAM_RELATIONSHIP_FRIEND;
	sb->personastate = STEAM_PERSONA_ONLINE;
	sb->personastate_known = TRUE;

	/* Only https on Steam's CDN hosts */
	u = steam_app_image_url("https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/440/capsule_184x69.jpg?t=1");
	CHECK(u != NULL);
	g_free(u);
	CHECK(steam_app_image_url("http://shared.akamai.steamstatic.com/a.jpg") == NULL);
	CHECK(steam_app_image_url("https://evil.example/a.jpg") == NULL);
	CHECK(steam_app_image_url("https://shared.akamai.steamstatic.com@evil.example/a.jpg") == NULL);
	CHECK(steam_app_image_url("https://shared.akamai.steamstatic.com/a\".jpg") == NULL);
	CHECK(steam_app_image_url(NULL) == NULL);

	/* In game; the name is cached, so there's no lookup */
	g_hash_table_replace(sa->app_names, GUINT_TO_POINTER(440), g_strdup("Team Fortress 2"));
	memset(&persona, 0, sizeof(persona));
	persona.steamid = FRIEND_ID;
	persona.has_game = TRUE;
	persona.game_app_id = 440;
	persona.gameid = 440;
	steam_buddy_set_game(sa, sb, &persona);
	steam_buddy_update_status(sa, sb);
	CHECK_STR(game_attr("game"), "Team Fortress 2");
	CHECK_STR(game_attr("game_app_id"), "440");
	CHECK(game_attr("game_icon_url") == NULL);

	/* The store API answers: the image is published */
	app_details(440, appdetails_440);
	CHECK_STR(game_attr("game"), "Team Fortress 2");
	CHECK_STR(game_attr("game_icon_url"),
	          "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/440/capsule_184x69.jpg?t=1757348372");
	/* ... and cached per app for the next update */
	steam_buddy_set_game(sa, sb, NULL);
	steam_buddy_update_status(sa, sb);
	CHECK(game_attr("game") == NULL);
	steam_buddy_set_game(sa, sb, &persona);
	steam_buddy_update_status(sa, sb);
	CHECK_HAS(game_attr("game_icon_url"), "/apps/440/capsule_184x69.jpg");

	/* Stock UI: no image, the rest as before */
	sa->native_meta = FALSE;
	steam_buddy_update_status(sa, sb);
	CHECK_STR(game_attr("game"), "Team Fortress 2");
	CHECK_STR(game_attr("game_app_id"), "440");
	CHECK(game_attr("game_icon_url") == NULL);
	sa->native_meta = TRUE;
	steam_buddy_set_game(sa, sb, NULL);
	steam_buddy_update_status(sa, sb);
}

int
main(int argc, char **argv)
{
	PurpleConnection *gc;
	PurplePlugin *prpl;
	char *dir = g_build_filename(argc > 1 ? argv[1] : ".", "profile", NULL);

	purple_util_set_user_dir(dir);
	purple_debug_set_enabled(getenv("DEBUG") != NULL);
	purple_eventloop_set_ui_ops(&loop_ops);
	ui_info = g_hash_table_new(g_str_hash, g_str_equal);
	g_hash_table_insert(ui_info, "name", "steam-test");
	purple_core_set_ui_ops(&core_ops);
	if (!purple_core_init("steam-test")) {
		fprintf(stderr, "core init failed\n");
		return 2;
	}
	purple_set_blist(purple_blist_new());
	purple_blist_load();
	register_meta_signals();

	/* This very plugin as the prpl (PURPLE_INIT_PLUGIN's entry point) */
	g_hash_table_insert(ui_info, "message-meta", "1");
	prpl = purple_plugin_new(TRUE, NULL);
	CHECK(purple_init_plugin(prpl));
	purple_plugins_probe(G_MODULE_SUFFIX);     /* moves it from the load queue */
	CHECK(purple_plugin_load(prpl));
	CHECK(purple_find_prpl(STEAM_PLUGIN_ID) == prpl);

	metas = g_string_new(NULL);
	last_meta = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	seen_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	events = g_string_new(NULL);
	written = g_string_new(NULL);
	query_done = g_string_new(NULL);
	purple_signal_connect(purple_conversations_get_handle(), "receiving-message-meta", &failures, PURPLE_CALLBACK(meta_cb), NULL);
	purple_signal_connect(purple_conversations_get_handle(), "sending-message-meta", &failures, PURPLE_CALLBACK(send_meta_cb), NULL);
	purple_signal_connect(purple_conversations_get_handle(), "message-reaction", &failures, PURPLE_CALLBACK(reaction_cb), NULL);
	purple_signal_connect(purple_conversations_get_handle(), "message-receipt", &failures, PURPLE_CALLBACK(receipt_cb), NULL);
	purple_signal_connect(purple_conversations_get_handle(), "writing-im-msg", &failures, PURPLE_CALLBACK(writing_cb), NULL);
	/* The plugin's own signal (message-meta UIs only) */
	CHECK(purple_signal_connect(prpl, "mam-query-done", &failures, PURPLE_CALLBACK(mam_done_cb), NULL) != 0);

	/* ---- pure helpers ---- */
	test_rich_text();

	/* ---- the account and a logged-on test CM ---- */
	account = purple_account_new("me", STEAM_PLUGIN_ID);
	purple_accounts_add(account);
	purple_account_set_bool(account, "download_offline_history", FALSE);
	gc = g_new0(PurpleConnection, 1);
	gc->account = account;
	gc->prpl = prpl;
	gc->state = PURPLE_CONNECTED;
	purple_account_set_connection(account, gc);

	/* Stock UI: no native metadata */
	g_hash_table_remove(ui_info, "message-meta");
	sa = steam_account_new_for_test(gc);
	CHECK(!sa->native_meta);
	g_hash_table_insert(ui_info, "message-meta", "1");
	steam_native_init(sa);
	CHECK(sa->native_meta);
	gc->proto_data = sa;
	sa->cm = steam_cm__test_new(sa, &steam_cm_callbacks, sa, SELF_ID);
	logon();
	CHECK(steam_cm_is_logged_on(sa->cm));

	test_rich_messages();
	test_ids_and_markers(prpl);
	test_scrollback(prpl);
	test_reactions(prpl);
	test_game_image();

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
