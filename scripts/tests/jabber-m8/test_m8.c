/*
 * Unit/integration tests for the M8 protocol core in the jabber prpl.
 *
 * Links libpurple + libjabber from the private prefix, runs a null UI with a
 * GLib event loop, builds a fake connected JabberStream (no network) and
 * drives the parsers with synthetic stanzas.  Outgoing stanzas are captured
 * from jabber-sending-xmlnode (the real sender is disconnected).
 */
#include "internal.h"

#include "account.h"
#include "blist.h"
#include "conversation.h"
#include "core.h"
#include "debug.h"
#include "eventloop.h"
#include "plugin.h"
#include "prefs.h"
#include "signals.h"
#include "util.h"
#include "xmlnode.h"

#include "jabber.h"
#include "bookmarks.h"
#include "carbons.h"
#include "chat.h"
#include "iq.h"
#include "mam.h"
#include "message.h"
#include "presence.h"

#include <string.h>

static int failures = 0, checks = 0;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(a, b) do { const char *_a = (a), *_b = (b); checks++; \
	if (g_strcmp0(_a, _b) != 0) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s == \"%s\", expected \"%s\"\n", __FILE__, __LINE__, \
	        #a, _a ? _a : "(null)", _b ? _b : "(null)"); } } while (0)

/**************************************************************************
 * Null UI
 **************************************************************************/

#define READ_COND  (G_IO_IN | G_IO_HUP | G_IO_ERR)
#define WRITE_COND (G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL)

typedef struct {
	PurpleInputFunction function;
	guint result;
	gpointer data;
} IOClosure;

static gboolean
io_invoke(GIOChannel *source, GIOCondition condition, gpointer data)
{
	IOClosure *closure = data;
	PurpleInputCondition cond = 0;

	if (condition & READ_COND)
		cond |= PURPLE_INPUT_READ;
	if (condition & WRITE_COND)
		cond |= PURPLE_INPUT_WRITE;
	closure->function(closure->data, g_io_channel_unix_get_fd(source), cond);
	return TRUE;
}

static guint
input_add(gint fd, PurpleInputCondition condition, PurpleInputFunction function,
          gpointer data)
{
	IOClosure *closure = g_new0(IOClosure, 1);
	GIOChannel *channel;
	GIOCondition cond = 0;

	closure->function = function;
	closure->data = data;
	if (condition & PURPLE_INPUT_READ)
		cond |= READ_COND;
	if (condition & PURPLE_INPUT_WRITE)
		cond |= WRITE_COND;
	channel = g_io_channel_unix_new(fd);
	closure->result = g_io_add_watch_full(channel, G_PRIORITY_DEFAULT, cond,
	                                      io_invoke, closure, g_free);
	g_io_channel_unref(channel);
	return closure->result;
}

static PurpleEventLoopUiOps eventloop_ops = {
	g_timeout_add, g_source_remove, input_add, g_source_remove, NULL,
	g_timeout_add_seconds, NULL, NULL, NULL
};

static GHashTable *ui_info = NULL;

static GHashTable *
get_ui_info(void)
{
	return ui_info;
}

static PurpleCoreUiOps core_ops = {
	NULL, NULL, NULL, NULL, get_ui_info, NULL, NULL, NULL
};

/**************************************************************************
 * Capture
 **************************************************************************/

static PurplePlugin *jabber_plugin = NULL;
static GPtrArray *sent = NULL;         /* char* (serialized stanzas) */
static GPtrArray *metas = NULL;        /* GHashTable* copies */
static GPtrArray *send_metas = NULL;
static GPtrArray *written = NULL;      /* Written* */
static GHashTable *kv = NULL;          /* key -> value */
static GPtrArray *mam_done = NULL;     /* char* summaries */
static const char *discard_id = NULL;

typedef struct {
	char *conv;
	char *who;
	char *message;
	PurpleMessageFlags flags;
} Written;

static void
written_free(Written *w)
{
	g_free(w->conv);
	g_free(w->who);
	g_free(w->message);
	g_free(w);
}

static void
sending_cb(PurpleConnection *gc, xmlnode **packet, gpointer data)
{
	if (packet && *packet)
		g_ptr_array_add(sent, xmlnode_to_str(*packet, NULL));
}

static GHashTable *
copy_table(GHashTable *meta)
{
	GHashTable *copy = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	GHashTableIter iter;
	gpointer k, v;

	g_hash_table_iter_init(&iter, meta);
	while (g_hash_table_iter_next(&iter, &k, &v))
		g_hash_table_insert(copy, g_strdup(k), g_strdup(v));
	return copy;
}

static void
meta_cb(PurpleAccount *account, const char *conv, GHashTable *meta, gpointer data)
{
	GHashTable *copy = copy_table(meta);
	g_hash_table_insert(copy, g_strdup("_conv"), g_strdup(conv));
	g_ptr_array_add(metas, copy);

	if (discard_id && g_strcmp0(g_hash_table_lookup(meta, "stanza-id"), discard_id) == 0)
		g_hash_table_insert(meta, g_strdup("discard"), g_strdup("1"));
}

static void
send_meta_cb(PurpleAccount *account, const char *conv, GHashTable *meta, gpointer data)
{
	GHashTable *copy = copy_table(meta);
	g_hash_table_insert(copy, g_strdup("_conv"), g_strdup(conv));
	g_ptr_array_add(send_metas, copy);
}

static gboolean
writing_cb(PurpleAccount *account, const char *who, char **message,
           PurpleConversation *conv, PurpleMessageFlags flags, gpointer data)
{
	Written *w = g_new0(Written, 1);
	w->conv = g_strdup(purple_conversation_get_name(conv));
	w->who = g_strdup(who);
	w->message = g_strdup(*message);
	w->flags = flags;
	g_ptr_array_add(written, w);
	return FALSE;
}

static const char *
kv_load_cb(PurpleAccount *account, const char *key, gpointer data)
{
	return g_hash_table_lookup(kv, key);
}

static void
kv_store_cb(PurpleAccount *account, const char *key, const char *value, gpointer data)
{
	if (value)
		g_hash_table_replace(kv, g_strdup(key), g_strdup(value));
	else
		g_hash_table_remove(kv, key);
}

static void
mam_done_cb(PurpleAccount *account, const char *conv, const char *first,
            const char *last, guint complete, gpointer data)
{
	g_ptr_array_add(mam_done, g_strdup_printf("%s|%s|%s|%u",
			conv ? conv : "", first ? first : "", last ? last : "", complete));
}

static void
reset_capture(void)
{
	g_ptr_array_set_size(sent, 0);
	g_ptr_array_set_size(metas, 0);
	g_ptr_array_set_size(send_metas, 0);
	g_ptr_array_set_size(written, 0);
	g_ptr_array_set_size(mam_done, 0);
}

static GHashTable *
last_meta(void)
{
	return metas->len ? g_ptr_array_index(metas, metas->len - 1) : NULL;
}

static Written *
last_written(void)
{
	return written->len ? g_ptr_array_index(written, written->len - 1) : NULL;
}

static const char *
meta_get(GHashTable *meta, const char *key)
{
	return meta ? g_hash_table_lookup(meta, key) : NULL;
}

/* Finds the last sent stanza containing @needle; returns its id attribute. */
static char *
sent_id_containing(const char *needle)
{
	int i;

	for (i = (int)sent->len - 1; i >= 0; i--) {
		const char *str = g_ptr_array_index(sent, i);
		if (strstr(str, needle)) {
			xmlnode *node = xmlnode_from_str(str, -1);
			char *id = g_strdup(xmlnode_get_attrib(node, "id"));
			xmlnode_free(node);
			return id;
		}
	}
	return NULL;
}

static const char *
sent_containing(const char *needle)
{
	int i;

	for (i = (int)sent->len - 1; i >= 0; i--)
		if (strstr(g_ptr_array_index(sent, i), needle))
			return g_ptr_array_index(sent, i);
	return NULL;
}

/**************************************************************************
 * Fake stream
 **************************************************************************/

static JabberStream *js = NULL;
static PurpleAccount *account = NULL;

static void
feed(const char *xml)
{
	xmlnode *packet = xmlnode_from_str(xml, -1);

	if (packet == NULL) {
		fprintf(stderr, "bad test XML: %s\n", xml);
		failures++;
		return;
	}

	if (purple_strequal(packet->name, "message"))
		jabber_message_parse(js, packet);
	else if (purple_strequal(packet->name, "iq"))
		jabber_iq_parse(js, packet);
	else if (purple_strequal(packet->name, "presence"))
		jabber_presence_parse(js, packet);
	xmlnode_free(packet);
}

static void
setup_stream(void)
{
	PurpleConnection *gc = g_new0(PurpleConnection, 1);

	account = purple_account_new("me@example.org/pidgin", "prpl-jabber");
	purple_accounts_add(account);

	gc->prpl = jabber_plugin;
	gc->account = account;
	gc->state = PURPLE_CONNECTED;
	purple_account_set_connection(account, gc);

	js = g_new0(JabberStream, 1);
	js->gc = gc;
	js->user = jabber_id_new("me@example.org/pidgin");
	js->buddies = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
			(GDestroyNotify)jabber_buddy_free);
	js->chats = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
			(GDestroyNotify)jabber_chat_free);
	js->iq_callbacks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
			(GDestroyNotify)jabber_iq_callbackdata_free);
	js->state = JABBER_STREAM_CONNECTED;
	js->pep = TRUE;
	js->fd = -1;
	gc->proto_data = js;
}

/**************************************************************************
 * Pure tests
 **************************************************************************/

static void
test_ids(void)
{
	xmlnode *msg = xmlnode_from_str(
		"<message xmlns='jabber:client' from='a@b/c' id='m1'>"
		"<stanza-id xmlns='urn:xmpp:sid:0' by='evil@example.org' id='bad'/>"
		"<stanza-id xmlns='urn:xmpp:sid:0' by='Me@Example.org' id='good'/>"
		"<origin-id xmlns='urn:xmpp:sid:0' id='orig'/>"
		"<body>x</body></message>", -1);
	char *id;

	id = jabber_message_get_stanza_id(msg, "me@example.org");
	CHECK_STR(id, "good");
	g_free(id);
	id = jabber_message_get_stanza_id(msg, "other@example.org");
	CHECK(id == NULL);
	id = jabber_message_get_origin_id(msg);
	CHECK_STR(id, "orig");
	g_free(id);
	xmlnode_free(msg);
}

static void
test_fallback(void)
{
	/* "> Hällo 👋\nreply" : '>'=0 ' '=1 H=2 ä=3 l l o=6 ' '=7 👋=8 '\n'=9 r=10 */
	const char *body = "> H\xc3\xa4llo \xf0\x9f\x91\x8b\nreply";
	xmlnode *msg = xmlnode_from_str(
		"<message xmlns='jabber:client'>"
		"<fallback xmlns='urn:xmpp:fallback:0' for='urn:xmpp:reply:0'>"
		"<body start='0' end='10'/></fallback>"
		"<fallback xmlns='urn:xmpp:fallback:0' for='urn:xmpp:reactions:0'/>"
		"<fallback xmlns='urn:xmpp:fallback:0' for='x'><body start='9' end='3'/></fallback>"
		"</message>", -1);
	GList *fb = jabber_fallback_parse(msg);
	const char *reply_only[] = { NS_REPLY, NULL };
	const char *none[] = { NULL };
	char *str;

	CHECK(g_list_length(fb) == 2); /* the invalid range is dropped */
	str = jabber_fallback_ranges_to_string(fb);
	CHECK_STR(str, "urn:xmpp:reply:0:0-10;urn:xmpp:reactions:0:");
	g_free(str);

	str = jabber_fallback_strip(body, fb, reply_only);
	CHECK_STR(str, "reply");
	g_free(str);

	str = jabber_fallback_strip(body, fb, none);
	CHECK_STR(str, body);
	g_free(str);

	str = jabber_fallback_strip(body, fb, jabber_native_fallback_namespaces);
	CHECK_STR(str, ""); /* whole-body reaction fallback */
	g_free(str);

	jabber_fallback_free(fb);
	xmlnode_free(msg);

	/* Ranges past the end are clamped */
	msg = xmlnode_from_str("<message><fallback xmlns='urn:xmpp:fallback:0' "
		"for='urn:xmpp:reply:0'><body start='2' end='99'/></fallback></message>", -1);
	fb = jabber_fallback_parse(msg);
	str = jabber_fallback_strip("abcdef", fb, reply_only);
	CHECK_STR(str, "ab");
	g_free(str);
	jabber_fallback_free(fb);
	xmlnode_free(msg);

	CHECK(jabber_fallback_ranges_to_string(NULL) == NULL);
}

static void
test_carbons_unwrap(void)
{
	JabberCarbonDirection dir;
	xmlnode *fwd, *inner;
	xmlnode *msg = xmlnode_from_str(
		"<message xmlns='jabber:client' from='me@example.org'>"
		"<sent xmlns='urn:xmpp:carbons:2'><forwarded xmlns='urn:xmpp:forward:0'>"
		"<message xmlns='jabber:client' to='f@x' from='me@example.org/phone' type='chat'>"
		"<body>hi</body></message></forwarded></sent></message>", -1);

	inner = jabber_carbons_unwrap(msg, &dir, &fwd);
	CHECK(dir == JABBER_CARBON_SENT);
	CHECK(inner != NULL && purple_strequal(xmlnode_get_attrib(inner, "to"), "f@x"));
	CHECK(fwd != NULL);
	xmlnode_free(msg);

	msg = xmlnode_from_str("<message xmlns='jabber:client'><received xmlns='urn:xmpp:carbons:2'/></message>", -1);
	inner = jabber_carbons_unwrap(msg, &dir, NULL);
	CHECK(dir == JABBER_CARBON_RECEIVED && inner == NULL);
	xmlnode_free(msg);

	msg = xmlnode_from_str("<message xmlns='jabber:client'><body>x</body></message>", -1);
	inner = jabber_carbons_unwrap(msg, &dir, NULL);
	CHECK(dir == JABBER_CARBON_NONE && inner == NULL);
	xmlnode_free(msg);

	CHECK(jabber_carbons_from_is_valid(NULL, "me@example.org"));
	CHECK(jabber_carbons_from_is_valid("ME@example.org", "me@example.org"));
	CHECK(!jabber_carbons_from_is_valid("me@example.org/res", "me@example.org"));
	CHECK(!jabber_carbons_from_is_valid("evil@example.org", "me@example.org"));
}

static void
test_mam_pure(void)
{
	JabberMamQuery *q;
	xmlnode *node, *inner;
	char *str;
	gboolean is_result, has_stamp;
	const char *qid, *id;
	time_t stamp;

	/* unwrap */
	node = xmlnode_from_str(
		"<message xmlns='jabber:client' to='me@example.org/pidgin'>"
		"<result xmlns='urn:xmpp:mam:2' queryid='q1' id='A1'>"
		"<forwarded xmlns='urn:xmpp:forward:0'>"
		"<delay xmlns='urn:xmpp:delay' stamp='2026-09-01T10:00:00Z'/>"
		"<message xmlns='jabber:client' from='f@x/r' type='chat'><body>old</body></message>"
		"</forwarded></result></message>", -1);
	inner = jabber_mam_unwrap(node, &is_result, &qid, &id, &has_stamp, &stamp);
	CHECK(is_result && inner != NULL);
	CHECK_STR(qid, "q1");
	CHECK_STR(id, "A1");
	CHECK(has_stamp && stamp == 1788256800);
	xmlnode_free(node);

	/* catch-up paging */
	q = jabber_mam_query_new(NULL, JABBER_MAM_QUERY_CATCHUP, "me@example.org", FALSE);
	q->after = g_strdup("L0");
	node = jabber_mam_query_build(q);
	str = xmlnode_to_str(node, NULL);
	CHECK(strstr(str, "<after>L0</after>") != NULL);
	CHECK(strstr(str, "<max>50</max>") != NULL);
	CHECK(strstr(str, "var='start'") == NULL && strstr(str, "var=\"start\"") == NULL);
	CHECK(strstr(str, "urn:xmpp:mam:2") != NULL);
	g_free(str);
	xmlnode_free(node);

	CHECK(jabber_mam_query_page_done(q, FALSE, "F1", "L1") == JABBER_MAM_PAGE_NEXT);
	CHECK_STR(q->after, "L1");
	CHECK_STR(q->first, "F1");
	CHECK(jabber_mam_query_page_done(q, FALSE, "F2", "L2") == JABBER_MAM_PAGE_NEXT);
	CHECK_STR(q->first, "F1");
	CHECK_STR(q->last, "L2");
	CHECK(jabber_mam_query_page_done(q, TRUE, "F3", "L3") == JABBER_MAM_PAGE_DONE);
	CHECK(q->complete);
	CHECK_STR(q->last, "L3");
	jabber_mam_query_free(q);

	/* bounded */
	q = jabber_mam_query_new(NULL, JABBER_MAM_QUERY_CATCHUP, "me@example.org", FALSE);
	q->max_pages = 2;
	CHECK(jabber_mam_query_page_done(q, FALSE, "a", "b") == JABBER_MAM_PAGE_NEXT);
	CHECK(jabber_mam_query_page_done(q, FALSE, "c", "d") == JABBER_MAM_PAGE_DONE);
	CHECK(!q->complete);
	jabber_mam_query_free(q);

	/* empty page ends it */
	q = jabber_mam_query_new(NULL, JABBER_MAM_QUERY_CATCHUP, "me@example.org", FALSE);
	q->start = 1788256800;
	node = jabber_mam_query_build(q);
	str = xmlnode_to_str(node, NULL);
	CHECK(strstr(str, "2026-09-01T10:00:00Z") != NULL);
	g_free(str);
	xmlnode_free(node);
	CHECK(jabber_mam_query_page_done(q, FALSE, NULL, NULL) == JABBER_MAM_PAGE_DONE);
	CHECK(q->complete && q->last == NULL);
	jabber_mam_query_free(q);

	/* older */
	q = jabber_mam_query_new(NULL, JABBER_MAM_QUERY_OLDER, "me@example.org", FALSE);
	q->with = g_strdup("f@x");
	q->before = g_strdup("B9");
	node = jabber_mam_query_build(q);
	str = xmlnode_to_str(node, NULL);
	CHECK(strstr(str, "<before>B9</before>") != NULL);
	CHECK(strstr(str, "<value>f@x</value>") != NULL);
	g_free(str);
	xmlnode_free(node);
	CHECK(jabber_mam_query_page_done(q, FALSE, "B1", "B8") == JABBER_MAM_PAGE_DONE);
	CHECK(!q->complete);
	CHECK_STR(q->first, "B1");
	jabber_mam_query_free(q);

	/* position */
	q = jabber_mam_query_new(NULL, JABBER_MAM_QUERY_POSITION, "room@muc.x", TRUE);
	node = jabber_mam_query_build(q);
	str = xmlnode_to_str(node, NULL);
	CHECK(strstr(str, "<before/>") != NULL);
	CHECK(strstr(str, "<max>1</max>") != NULL);
	g_free(str);
	xmlnode_free(node);
	CHECK(jabber_mam_query_page_done(q, TRUE, "Z", "Z") == JABBER_MAM_PAGE_DONE);
	CHECK_STR(q->last, "Z");
	jabber_mam_query_free(q);
}

static void
test_selfping_pure(void)
{
	xmlnode *iq;

	CHECK(jabber_chat_selfping_decide(1000, 900, 0, FALSE) == JABBER_SELFPING_NOTHING);
	CHECK(jabber_chat_selfping_decide(1000, 900, 0, TRUE) == JABBER_SELFPING_SEND);
	CHECK(jabber_chat_selfping_decide(1000, 1000 - JABBER_SELFPING_IDLE, 0, FALSE) == JABBER_SELFPING_SEND);
	CHECK(jabber_chat_selfping_decide(1000, 0, 950, TRUE) == JABBER_SELFPING_NOTHING);
	CHECK(jabber_chat_selfping_decide(1000, 0, 1000 - JABBER_SELFPING_TIMEOUT, FALSE) == JABBER_SELFPING_TIMED_OUT);

	CHECK(jabber_chat_selfping_classify(JABBER_IQ_RESULT, NULL) == JABBER_SELFPING_JOINED);
#define ERR(cond) "<iq type='error'><error type='cancel'><" cond " xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/></error></iq>"
	iq = xmlnode_from_str(ERR("not-acceptable"), -1);
	CHECK(jabber_chat_selfping_classify(JABBER_IQ_ERROR, iq) == JABBER_SELFPING_NOT_JOINED);
	xmlnode_free(iq);
	iq = xmlnode_from_str(ERR("service-unavailable"), -1);
	CHECK(jabber_chat_selfping_classify(JABBER_IQ_ERROR, iq) == JABBER_SELFPING_JOINED);
	xmlnode_free(iq);
	iq = xmlnode_from_str(ERR("feature-not-implemented"), -1);
	CHECK(jabber_chat_selfping_classify(JABBER_IQ_ERROR, iq) == JABBER_SELFPING_JOINED);
	xmlnode_free(iq);
	iq = xmlnode_from_str(ERR("item-not-found"), -1);
	CHECK(jabber_chat_selfping_classify(JABBER_IQ_ERROR, iq) == JABBER_SELFPING_JOINED);
	xmlnode_free(iq);
	iq = xmlnode_from_str(ERR("remote-server-timeout"), -1);
	CHECK(jabber_chat_selfping_classify(JABBER_IQ_ERROR, iq) == JABBER_SELFPING_UNKNOWN);
	xmlnode_free(iq);
	iq = xmlnode_from_str(ERR("bad-request"), -1);
	CHECK(jabber_chat_selfping_classify(JABBER_IQ_ERROR, iq) == JABBER_SELFPING_NOT_JOINED);
	xmlnode_free(iq);
	iq = xmlnode_from_str("<iq type='error'/>", -1);
	CHECK(jabber_chat_selfping_classify(JABBER_IQ_ERROR, iq) == JABBER_SELFPING_NOT_JOINED);
	xmlnode_free(iq);
}

static void
test_bookmarks_pure(void)
{
	xmlnode *item, *storage, *node;
	JabberBookmark *b;
	GList *list;
	char *str;

	item = xmlnode_from_str(
		"<item id='Room@Conference.Example.org'>"
		"<conference xmlns='urn:xmpp:bookmarks:1' name='The Room' autojoin='true'>"
		"<nick>me</nick><password>pw</password></conference></item>", -1);
	b = jabber_bookmark_parse_item(item);
	CHECK(b != NULL);
	if (b) {
		CHECK_STR(b->jid, "room@conference.example.org");
		CHECK_STR(b->name, "The Room");
		CHECK_STR(b->nick, "me");
		CHECK_STR(b->password, "pw");
		CHECK(b->autojoin);

		node = jabber_bookmark_to_conference(b);
		str = xmlnode_to_str(node, NULL);
		CHECK(strstr(str, "autojoin='true'") || strstr(str, "autojoin=\"true\""));
		CHECK(strstr(str, "<nick>me</nick>") != NULL);
		g_free(str);
		xmlnode_free(node);
		jabber_bookmark_free(b);
	}
	xmlnode_free(item);

	item = xmlnode_from_str("<item id='nonode.example.org'><conference xmlns='urn:xmpp:bookmarks:1'/></item>", -1);
	CHECK(jabber_bookmark_parse_item(item) == NULL);
	xmlnode_free(item);

	storage = xmlnode_from_str(
		"<storage xmlns='storage:bookmarks'>"
		"<conference jid='a@muc.x' autojoin='1'><nick>n</nick></conference>"
		"<conference jid='b@muc.x' name='B'/>"
		"<url url='http://x'/></storage>", -1);
	list = jabber_bookmarks_parse_storage(storage);
	CHECK(g_list_length(list) == 2);
	if (g_list_length(list) == 2) {
		JabberBookmark *a = list->data, *bb = list->next->data;
		CHECK(a->autojoin && !bb->autojoin);
		CHECK_STR(a->nick, "n");
		CHECK_STR(bb->name, "B");
	}
	node = jabber_bookmarks_to_storage(list);
	str = xmlnode_to_str(node, NULL);
	CHECK(strstr(str, "storage:bookmarks") && strstr(str, "a@muc.x") && strstr(str, "b@muc.x"));
	g_free(str);
	xmlnode_free(node);
	g_list_free_full(list, (GDestroyNotify)jabber_bookmark_free);
	xmlnode_free(storage);
}

/**************************************************************************
 * Integration tests on the fake stream
 **************************************************************************/

static void
test_live_ids(void)
{
	GHashTable *m;
	Written *w;

	reset_capture();
	feed("<message xmlns='jabber:client' from='friend@example.net/phone' "
	     "to='me@example.org/pidgin' type='chat' id='live1'>"
	     "<body>hello</body>"
	     "<origin-id xmlns='urn:xmpp:sid:0' id='o-live1'/>"
	     "<stanza-id xmlns='urn:xmpp:sid:0' by='me@example.org' id='S-live1'/>"
	     "<stanza-id xmlns='urn:xmpp:sid:0' by='friend@example.net' id='spoof'/>"
	     "</message>");
	m = last_meta();
	CHECK(metas->len == 1);
	CHECK_STR(meta_get(m, "_conv"), "friend@example.net/phone");
	CHECK_STR(meta_get(m, "stanza-id"), "live1");
	CHECK_STR(meta_get(m, "origin-id"), "o-live1");
	CHECK_STR(meta_get(m, "server-id"), "S-live1");
	CHECK_STR(meta_get(m, "server-id-by"), "me@example.org");
	CHECK_STR(meta_get(m, "conv-type"), "im");
	CHECK(meta_get(m, "mam") == NULL && meta_get(m, "carbon") == NULL);
	w = last_written();
	CHECK(w && purple_strequal(w->message, "hello"));
	CHECK(w && (w->flags & PURPLE_MESSAGE_RECV) && !(w->flags & PURPLE_MESSAGE_DELAYED));
	/* catch-up hasn't run: the live id must not move mam/last-id */
	CHECK(g_hash_table_lookup(kv, "mam/last-id") == NULL);

	/* the same server id again (e.g. from the archive) is dropped */
	reset_capture();
	feed("<message xmlns='jabber:client' from='friend@example.net/phone' "
	     "type='chat' id='live1-dup'><body>hello</body>"
	     "<stanza-id xmlns='urn:xmpp:sid:0' by='me@example.org' id='S-live1'/></message>");
	CHECK(metas->len == 0 && written->len == 0);
}

static void
test_carbons(void)
{
	GHashTable *m;
	Written *w;

	/* received */
	reset_capture();
	feed("<message xmlns='jabber:client' from='me@example.org' to='me@example.org/pidgin'>"
	     "<received xmlns='urn:xmpp:carbons:2'><forwarded xmlns='urn:xmpp:forward:0'>"
	     "<message xmlns='jabber:client' from='friend@example.net/laptop' "
	     "to='me@example.org/phone' type='chat' id='c1'><body>to phone</body>"
	     "<stanza-id xmlns='urn:xmpp:sid:0' by='me@example.org' id='S-c1'/>"
	     "</message></forwarded></received></message>");
	m = last_meta();
	CHECK_STR(meta_get(m, "carbon"), "received");
	CHECK_STR(meta_get(m, "stanza-id"), "c1");
	CHECK_STR(meta_get(m, "server-id"), "S-c1");
	w = last_written();
	CHECK(w && purple_strequal(w->message, "to phone") && (w->flags & PURPLE_MESSAGE_RECV));

	/* sent */
	reset_capture();
	feed("<message xmlns='jabber:client' from='me@example.org' to='me@example.org/pidgin'>"
	     "<sent xmlns='urn:xmpp:carbons:2'><forwarded xmlns='urn:xmpp:forward:0'>"
	     "<message xmlns='jabber:client' to='friend@example.net' "
	     "from='me@example.org/phone' type='chat' id='c2'><body>from phone</body>"
	     "<origin-id xmlns='urn:xmpp:sid:0' id='o-c2'/>"
	     "</message></forwarded></sent></message>");
	m = last_meta();
	CHECK_STR(meta_get(m, "carbon"), "sent");
	CHECK_STR(meta_get(m, "outgoing"), "1");
	CHECK_STR(meta_get(m, "origin-id"), "o-c2");
	CHECK_STR(meta_get(m, "_conv"), "friend@example.net");
	w = last_written();
	CHECK(w != NULL);
	if (w) {
		CHECK_STR(w->message, "from phone");
		CHECK(g_str_has_prefix(w->conv, "friend@example.net"));
		CHECK((w->flags & PURPLE_MESSAGE_SEND) && (w->flags & PURPLE_MESSAGE_REMOTE_SEND));
		CHECK(!(w->flags & PURPLE_MESSAGE_RECV));
	}

	/* spoofed: from someone else */
	reset_capture();
	feed("<message xmlns='jabber:client' from='evil@example.net' to='me@example.org/pidgin'>"
	     "<sent xmlns='urn:xmpp:carbons:2'><forwarded xmlns='urn:xmpp:forward:0'>"
	     "<message xmlns='jabber:client' to='friend@example.net' from='me@example.org/x' "
	     "type='chat'><body>fake</body></message></forwarded></sent></message>");
	CHECK(written->len == 0 && metas->len == 0);

	/* private and groupchat carbons are ignored */
	reset_capture();
	feed("<message xmlns='jabber:client' from='me@example.org'>"
	     "<received xmlns='urn:xmpp:carbons:2'><forwarded xmlns='urn:xmpp:forward:0'>"
	     "<message xmlns='jabber:client' from='f@x/r' type='chat'><body>p</body>"
	     "<private xmlns='urn:xmpp:carbons:2'/></message></forwarded></received></message>");
	feed("<message xmlns='jabber:client' from='me@example.org'>"
	     "<received xmlns='urn:xmpp:carbons:2'><forwarded xmlns='urn:xmpp:forward:0'>"
	     "<message xmlns='jabber:client' from='room@muc.x/n' type='groupchat'><body>g</body>"
	     "</message></forwarded></received></message>");
	/* MUC PM for a room we are not in */
	feed("<message xmlns='jabber:client' from='me@example.org'>"
	     "<received xmlns='urn:xmpp:carbons:2'><forwarded xmlns='urn:xmpp:forward:0'>"
	     "<message xmlns='jabber:client' from='other@muc.x/n' type='chat'><body>pm</body>"
	     "<x xmlns='http://jabber.org/protocol/muc#user'/></message></forwarded></received></message>");
	CHECK(written->len == 0);
}

static char *
catchup_queryid(void)
{
	const char *str = sent_containing("urn:xmpp:mam:2");
	xmlnode *node, *query;
	char *qid;

	if (str == NULL)
		return NULL;
	node = xmlnode_from_str(str, -1);
	query = xmlnode_get_child(node, "query");
	qid = g_strdup(xmlnode_get_attrib(query, "queryid"));
	xmlnode_free(node);
	return qid;
}

static void
test_mam_catchup(void)
{
	char *qid, *iqid, *xml;
	GHashTable *m;
	Written *w;
	const char *str;

	reset_capture();
	js->mam_supported = TRUE;
	jabber_mam_catchup(js);

	/* no stored id and a message-meta UI: a bounded catch-up by time */
	str = sent_containing("urn:xmpp:mam:2");
	CHECK(str != NULL);
	CHECK(str && strstr(str, "var='start'") != NULL);
	CHECK(str && strstr(str, "<max>50</max>") != NULL);
	qid = catchup_queryid();
	iqid = sent_id_containing("urn:xmpp:mam:2");
	CHECK(qid != NULL && iqid != NULL);

	/* an incoming archived message */
	reset_capture();
	xml = g_strdup_printf(
		"<message xmlns='jabber:client' to='me@example.org/pidgin'>"
		"<result xmlns='urn:xmpp:mam:2' queryid='%s' id='A1'>"
		"<forwarded xmlns='urn:xmpp:forward:0'>"
		"<delay xmlns='urn:xmpp:delay' stamp='2026-09-01T10:00:00Z'/>"
		"<message xmlns='jabber:client' from='friend@example.net/laptop' "
		"to='me@example.org/phone' type='chat' id='m-A1'><body>while away</body>"
		"<composing xmlns='http://jabber.org/protocol/chatstates'/></message>"
		"</forwarded></result></message>", qid);
	feed(xml);
	g_free(xml);
	m = last_meta();
	CHECK_STR(meta_get(m, "mam"), "1");
	CHECK_STR(meta_get(m, "mam-query"), "catchup");
	CHECK_STR(meta_get(m, "server-id"), "A1");
	CHECK_STR(meta_get(m, "server-id-by"), "me@example.org");
	CHECK_STR(meta_get(m, "stanza-id"), "m-A1");
	CHECK_STR(meta_get(m, "timestamp"), "1788256800");
	w = last_written();
	CHECK(w && purple_strequal(w->message, "while away"));
	CHECK(w && (w->flags & PURPLE_MESSAGE_DELAYED));

	/* an archived message we sent from another device */
	xml = g_strdup_printf(
		"<message xmlns='jabber:client'>"
		"<result xmlns='urn:xmpp:mam:2' queryid='%s' id='A2'>"
		"<forwarded xmlns='urn:xmpp:forward:0'>"
		"<delay xmlns='urn:xmpp:delay' stamp='2026-09-01T10:01:00Z'/>"
		"<message xmlns='jabber:client' to='friend@example.net' "
		"from='me@example.org/phone' type='chat'><body>my answer</body></message>"
		"</forwarded></result></message>", qid);
	feed(xml);
	g_free(xml);
	w = last_written();
	CHECK(w && purple_strequal(w->message, "my answer"));
	CHECK(w && (w->flags & PURPLE_MESSAGE_SEND) && (w->flags & PURPLE_MESSAGE_REMOTE_SEND)
	      && (w->flags & PURPLE_MESSAGE_DELAYED));
	CHECK_STR(meta_get(last_meta(), "outgoing"), "1");

	/* the live message from test_live_ids comes back: dropped */
	reset_capture();
	xml = g_strdup_printf(
		"<message xmlns='jabber:client'>"
		"<result xmlns='urn:xmpp:mam:2' queryid='%s' id='S-live1'>"
		"<forwarded xmlns='urn:xmpp:forward:0'>"
		"<delay xmlns='urn:xmpp:delay' stamp='2026-09-01T10:02:00Z'/>"
		"<message xmlns='jabber:client' from='friend@example.net/phone' type='chat'>"
		"<body>hello</body></message></forwarded></result></message>", qid);
	feed(xml);
	g_free(xml);
	CHECK(written->len == 0);

	/* a result from someone else's "archive" is ignored */
	xml = g_strdup_printf(
		"<message xmlns='jabber:client' from='evil@example.net'>"
		"<result xmlns='urn:xmpp:mam:2' queryid='%s' id='E1'>"
		"<forwarded xmlns='urn:xmpp:forward:0'><message xmlns='jabber:client' "
		"from='x@y/z' type='chat'><body>evil</body></message></forwarded></result></message>", qid);
	feed(xml);
	g_free(xml);
	/* unknown queryid is ignored */
	feed("<message xmlns='jabber:client'><result xmlns='urn:xmpp:mam:2' queryid='nope' id='N1'>"
	     "<forwarded xmlns='urn:xmpp:forward:0'><message xmlns='jabber:client' "
	     "from='x@y/z' type='chat'><body>n</body></message></forwarded></result></message>");
	CHECK(written->len == 0);

	/* the discard key from the UI drops a message */
	discard_id = "m-A3";
	xml = g_strdup_printf(
		"<message xmlns='jabber:client'>"
		"<result xmlns='urn:xmpp:mam:2' queryid='%s' id='A3'>"
		"<forwarded xmlns='urn:xmpp:forward:0'>"
		"<message xmlns='jabber:client' from='friend@example.net/laptop' type='chat' "
		"id='m-A3'><body>dup in index</body></message></forwarded></result></message>", qid);
	feed(xml);
	g_free(xml);
	discard_id = NULL;
	CHECK(metas->len == 1 && written->len == 0);

	/* first page not complete: next page with after=A3 */
	reset_capture();
	xml = g_strdup_printf("<iq xmlns='jabber:client' type='result' id='%s'>"
		"<fin xmlns='urn:xmpp:mam:2'><set xmlns='http://jabber.org/protocol/rsm'>"
		"<first>A1</first><last>A3</last></set></fin></iq>", iqid);
	feed(xml);
	g_free(xml);
	g_free(iqid);
	CHECK_STR(g_hash_table_lookup(kv, "mam/last-id"), "A3");
	str = sent_containing("urn:xmpp:mam:2");
	CHECK(str && strstr(str, "<after>A3</after>") != NULL);
	CHECK(str && strstr(str, "var='start'") == NULL);
	CHECK(mam_done->len == 0);
	iqid = sent_id_containing("urn:xmpp:mam:2");

	/* second page completes */
	reset_capture();
	xml = g_strdup_printf("<iq xmlns='jabber:client' type='result' id='%s'>"
		"<fin xmlns='urn:xmpp:mam:2' complete='true'><set xmlns='http://jabber.org/protocol/rsm'>"
		"<first>A4</first><last>A5</last></set></fin></iq>", iqid);
	feed(xml);
	g_free(xml);
	g_free(iqid);
	CHECK_STR(g_hash_table_lookup(kv, "mam/last-id"), "A5");
	CHECK(mam_done->len == 1);
	if (mam_done->len)
		CHECK_STR(g_ptr_array_index(mam_done, 0), "|A1|A5|1");
	CHECK(js->mam_catchup_done);
	CHECK(js->mam_queries == NULL || g_hash_table_size(js->mam_queries) == 0);

	/* after the catch-up, live ids move mam/last-id */
	feed("<message xmlns='jabber:client' from='friend@example.net/phone' type='chat' id='l2'>"
	     "<body>live again</body><stanza-id xmlns='urn:xmpp:sid:0' by='me@example.org' id='S-l2'/></message>");
	CHECK_STR(g_hash_table_lookup(kv, "mam/last-id"), "S-l2");

	/* next connection resumes from it; an expired id restarts by time */
	reset_capture();
	js->mam_catchup_done = FALSE;
	jabber_mam_catchup(js);
	str = sent_containing("urn:xmpp:mam:2");
	CHECK(str && strstr(str, "<after>S-l2</after>") != NULL);
	iqid = sent_id_containing("urn:xmpp:mam:2");
	reset_capture();
	xml = g_strdup_printf("<iq xmlns='jabber:client' type='error' id='%s'>"
		"<error type='cancel'><item-not-found xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
		"</error></iq>", iqid);
	feed(xml);
	g_free(xml);
	g_free(iqid);
	CHECK(g_hash_table_lookup(kv, "mam/last-id") == NULL);
	str = sent_containing("urn:xmpp:mam:2");
	CHECK(str && strstr(str, "var='start'") != NULL && strstr(str, "<after>") == NULL);
	iqid = sent_id_containing("urn:xmpp:mam:2");
	xml = g_strdup_printf("<iq xmlns='jabber:client' type='result' id='%s'>"
		"<fin xmlns='urn:xmpp:mam:2' complete='true'/></iq>", iqid);
	feed(xml);
	g_free(xml);
	g_free(iqid);
	CHECK(js->mam_catchup_done);

	g_free(qid);
}

static void
test_fallback_integration(void)
{
	GHashTable *m;
	Written *w;

	reset_capture();
	feed("<message xmlns='jabber:client' from='friend@example.net/phone' type='chat' id='r1'>"
	     "<body>&gt; quoted\nmy reply</body>"
	     "<reply xmlns='urn:xmpp:reply:0' to='me@example.org/pidgin' id='orig-1'/>"
	     "<fallback xmlns='urn:xmpp:fallback:0' for='urn:xmpp:reply:0'>"
	     "<body start='0' end='9'/></fallback></message>");
	m = last_meta();
	CHECK_STR(meta_get(m, "reply-to"), "orig-1");
	CHECK_STR(meta_get(m, "reply-to-sender"), "me@example.org/pidgin");
	CHECK_STR(meta_get(m, "fallback-ranges"), "urn:xmpp:reply:0:0-9");
	CHECK_STR(meta_get(m, "fallback-stripped"), "1");
	w = last_written();
	CHECK(w && purple_strequal(w->message, "my reply"));

	/* a correction carries correction-of */
	reset_capture();
	feed("<message xmlns='jabber:client' from='friend@example.net/phone' type='chat' id='r2'>"
	     "<body>fixed</body><replace xmlns='urn:xmpp:message-correct:0' id='r1'/></message>");
	CHECK_STR(meta_get(last_meta(), "correction-of"), "r1");

	/* without a message-meta UI: no signal, no stripping */
	g_hash_table_remove(ui_info, "message-meta");
	jabber_ui_message_meta_reset();
	reset_capture();
	feed("<message xmlns='jabber:client' from='friend@example.net/phone' type='chat' id='r3'>"
	     "<body>&gt; quoted\nmy reply</body>"
	     "<reply xmlns='urn:xmpp:reply:0' to='me@example.org/pidgin' id='orig-1'/>"
	     "<fallback xmlns='urn:xmpp:fallback:0' for='urn:xmpp:reply:0'>"
	     "<body start='0' end='9'/></fallback></message>");
	CHECK(metas->len == 0);
	w = last_written();
	CHECK(w && strstr(w->message, "quoted") != NULL);
	g_hash_table_insert(ui_info, "message-meta", "1");
	jabber_ui_message_meta_reset();
}

static void
test_send(void)
{
	GHashTable *m;
	const char *str;
	xmlnode *node, *origin;

	reset_capture();
	jabber_message_send_im(js->gc, "friend@example.net", "outgoing", 0);
	str = sent_containing("outgoing");
	CHECK(str != NULL);
	if (!str)
		return;
	node = xmlnode_from_str(str, -1);
	origin = xmlnode_get_child_with_namespace(node, "origin-id", NS_SID);
	CHECK(origin != NULL);
	CHECK(origin && purple_strequal(xmlnode_get_attrib(origin, "id"),
	                                xmlnode_get_attrib(node, "id")));
	m = send_metas->len ? g_ptr_array_index(send_metas, 0) : NULL;
	CHECK_STR(meta_get(m, "_conv"), "friend@example.net");
	CHECK_STR(meta_get(m, "origin-id"), xmlnode_get_attrib(origin, "id"));
	CHECK_STR(meta_get(m, "stanza-id"), xmlnode_get_attrib(node, "id"));

	/* the same message from the archive later in the session: dropped */
	{
		char *xml = g_strdup_printf(
			"<message xmlns='jabber:client' from='me@example.org/pidgin' "
			"to='friend@example.net' type='chat'><body>outgoing</body>"
			"<origin-id xmlns='urn:xmpp:sid:0' id='%s'/></message>",
			xmlnode_get_attrib(origin, "id"));
		JabberMessageContext ctx = { JABBER_MESSAGE_ORIGIN_MAM, "A-out",
			"me@example.org", "catchup", FALSE, 0 };
		xmlnode *msg = xmlnode_from_str(xml, -1);

		reset_capture();
		jabber_message_parse_with_context(js, msg, &ctx);
		CHECK(written->len == 0);
		xmlnode_free(msg);
		g_free(xml);
	}
	xmlnode_free(node);
}

static void
test_muc(void)
{
	JabberChat *chat;
	GHashTable *components;
	char *iqid, *xml, *qid;
	const char *str;
	GHashTable *m;
	Written *w;

	reset_capture();
	components = g_hash_table_new(g_str_hash, g_str_equal);
	g_hash_table_insert(components, "room", "room");
	g_hash_table_insert(components, "server", "muc.example.org");
	g_hash_table_insert(components, "handle", "me");
	chat = jabber_join_chat(js, "room", "muc.example.org", "me", NULL, components);
	CHECK(chat != NULL);
	if (!chat)
		return;

	/* disco#info to the room goes out before the join presence */
	iqid = sent_id_containing("http://jabber.org/protocol/disco#info");
	CHECK(iqid != NULL);
	str = sent_containing("<presence");
	CHECK(str && strstr(str, "room@muc.example.org/me"));
	CHECK(str && strstr(str, "maxstanzas") == NULL); /* no archive position yet */

	xml = g_strdup_printf("<iq xmlns='jabber:client' type='result' from='room@muc.example.org' id='%s'>"
		"<query xmlns='http://jabber.org/protocol/disco#info'>"
		"<identity category='conference' type='text'/>"
		"<feature var='urn:xmpp:mam:2'/><feature var='urn:xmpp:occupant-id:0'/>"
		"<feature var='http://jabber.org/protocol/muc'/></query></iq>", iqid);
	feed(xml);
	g_free(xml);
	g_free(iqid);
	CHECK(chat->disco_done && chat->mam_supported && chat->occupant_id_supported);

	/* our own presence: joined; the room catch-up only learns the position */
	reset_capture();
	feed("<presence xmlns='jabber:client' from='room@muc.example.org/me' to='me@example.org/pidgin'>"
	     "<x xmlns='http://jabber.org/protocol/muc#user'><item affiliation='member' role='participant'/>"
	     "<status code='110'/></x></presence>");
	CHECK(chat->conv != NULL);
	CHECK(chat->self_joined);
	CHECK(js->selfping_timer != 0);
	str = sent_containing("urn:xmpp:mam:2");
	CHECK(str && strstr(str, "to='room@muc.example.org'") && strstr(str, "<before/>"));
	iqid = sent_id_containing("urn:xmpp:mam:2");
	xml = g_strdup_printf("<iq xmlns='jabber:client' type='result' from='room@muc.example.org' id='%s'>"
		"<fin xmlns='urn:xmpp:mam:2' complete='true'><set xmlns='http://jabber.org/protocol/rsm'>"
		"<first>R9</first><last>R9</last></set></fin></iq>", iqid);
	feed(xml);
	g_free(xml);
	g_free(iqid);
	CHECK_STR(g_hash_table_lookup(kv, "mam/last-id/room@muc.example.org"), "R9");
	CHECK(chat->mam_catchup_done);

	/* a room message with occupant-id and the room's stanza-id */
	reset_capture();
	feed("<message xmlns='jabber:client' from='room@muc.example.org/alice' type='groupchat' id='g1'>"
	     "<body>hi room</body>"
	     "<occupant-id xmlns='urn:xmpp:occupant-id:0' id='occ-alice'/>"
	     "<stanza-id xmlns='urn:xmpp:sid:0' by='room@muc.example.org' id='R10'/>"
	     "<stanza-id xmlns='urn:xmpp:sid:0' by='me@example.org' id='wrong'/></message>");
	m = last_meta();
	CHECK_STR(meta_get(m, "conv-type"), "chat");
	CHECK_STR(meta_get(m, "_conv"), "room@muc.example.org");
	CHECK_STR(meta_get(m, "sender"), "alice");
	CHECK_STR(meta_get(m, "occupant-id"), "occ-alice");
	CHECK_STR(meta_get(m, "server-id"), "R10");
	w = last_written();
	CHECK(w && purple_strequal(w->message, "hi room"));
	CHECK_STR(g_hash_table_lookup(kv, "mam/last-id/room@muc.example.org"), "R10");

	/* scroll-back through the IPC command */
	reset_capture();
	{
		gboolean ok = FALSE;
		purple_plugin_ipc_call(jabber_plugin, "mam-fetch-older", &ok, account,
		                       "room@muc.example.org", "R10", 20);
		CHECK(ok);
	}
	str = sent_containing("urn:xmpp:mam:2");
	CHECK(str && strstr(str, "<before>R10</before>") && strstr(str, "<max>20</max>"));
	qid = catchup_queryid();
	iqid = sent_id_containing("urn:xmpp:mam:2");
	xml = g_strdup_printf(
		"<message xmlns='jabber:client' from='room@muc.example.org'>"
		"<result xmlns='urn:xmpp:mam:2' queryid='%s' id='R5'>"
		"<forwarded xmlns='urn:xmpp:forward:0'>"
		"<delay xmlns='urn:xmpp:delay' stamp='2026-08-01T10:00:00Z'/>"
		"<message xmlns='jabber:client' from='room@muc.example.org/bob' type='groupchat'>"
		"<body>old room msg</body></message></forwarded></result></message>", qid);
	feed(xml);
	g_free(xml);
	m = last_meta();
	CHECK_STR(meta_get(m, "mam-query"), "older");
	CHECK_STR(meta_get(m, "server-id"), "R5");
	w = last_written();
	CHECK(w && purple_strequal(w->message, "old room msg") && (w->flags & PURPLE_MESSAGE_DELAYED));
	xml = g_strdup_printf("<iq xmlns='jabber:client' type='result' from='room@muc.example.org' id='%s'>"
		"<fin xmlns='urn:xmpp:mam:2'><set xmlns='http://jabber.org/protocol/rsm'>"
		"<first>R5</first><last>R5</last></set></fin></iq>", iqid);
	feed(xml);
	g_free(xml);
	g_free(iqid);
	g_free(qid);
	CHECK(mam_done->len == 1);
	if (mam_done->len)
		CHECK_STR(g_ptr_array_index(mam_done, 0), "room@muc.example.org|R5|R5|0");
	/* scroll-back doesn't move the catch-up position */
	CHECK_STR(g_hash_table_lookup(kv, "mam/last-id/room@muc.example.org"), "R10");

	/* rejoining a room with a known position suppresses room history */
	reset_capture();
	jabber_chat_destroy(chat);
	chat = jabber_join_chat(js, "room", "muc.example.org", "me", NULL, components);
	g_hash_table_destroy(components);
	str = sent_containing("<presence");
	CHECK(str && strstr(str, "maxstanzas='0'"));
}

static void
test_bookmarks_integration(void)
{
	char *iqid, *xml;

	reset_capture();
	jabber_bookmarks_fetch(js);
	iqid = sent_id_containing("urn:xmpp:bookmarks:1");
	CHECK(iqid != NULL);
	xml = g_strdup_printf("<iq xmlns='jabber:client' type='result' id='%s'>"
		"<pubsub xmlns='http://jabber.org/protocol/pubsub'>"
		"<items node='urn:xmpp:bookmarks:1'>"
		"<item id='auto@muc.example.org'><conference xmlns='urn:xmpp:bookmarks:1' "
		"autojoin='true'><nick>bm</nick></conference></item>"
		"<item id='manual@muc.example.org'><conference xmlns='urn:xmpp:bookmarks:1'/></item>"
		"</items></pubsub></iq>", iqid);
	reset_capture();
	feed(xml);
	g_free(xml);
	g_free(iqid);
	CHECK(sent_containing("auto@muc.example.org/bm") != NULL);
	CHECK(sent_containing("manual@muc.example.org") == NULL);
	CHECK(jabber_chat_find(js, "auto", "muc.example.org") != NULL);

	/* IPC add publishes a native item with publish-options */
	reset_capture();
	{
		gboolean ok = FALSE;
		purple_plugin_ipc_call(jabber_plugin, "bookmark-add", &ok, account,
		                       "New@muc.example.org", "nick2", FALSE);
		CHECK(ok);
	}
	CHECK(sent_containing("<item id='new@muc.example.org'>") != NULL);
	CHECK(sent_containing("pubsub#publish-options") != NULL);
	CHECK(sent_containing("new@muc.example.org/nick2") == NULL); /* not autojoin */
	reset_capture();
	{
		gboolean ok = FALSE;
		purple_plugin_ipc_call(jabber_plugin, "bookmark-remove", &ok, account,
		                       "new@muc.example.org");
		CHECK(ok);
	}
	CHECK(sent_containing("<retract") != NULL);
}

/**************************************************************************/

int
main(int argc, char **argv)
{
	GList *l;
	const char *userdir = argc > 1 ? argv[1] : "./test-home";

	ui_info = g_hash_table_new(g_str_hash, g_str_equal);
	g_hash_table_insert(ui_info, "name", "m8test");
	g_hash_table_insert(ui_info, "message-meta", "1");

	purple_util_set_user_dir(userdir);
	purple_debug_set_enabled(g_getenv("M8_DEBUG") != NULL);
	purple_core_set_ui_ops(&core_ops);
	purple_eventloop_set_ui_ops(&eventloop_ops);
	if (!purple_core_init("m8test")) {
		fprintf(stderr, "core init failed\n");
		return 2;
	}
	purple_set_blist(purple_blist_new());

	for (l = purple_plugins_get_protocols(); l; l = l->next) {
		PurplePlugin *p = l->data;
		if (purple_strequal(p->info->id, "prpl-jabber"))
			jabber_plugin = p;
	}
	if (jabber_plugin == NULL) {
		fprintf(stderr, "prpl-jabber not found\n");
		return 2;
	}

	sent = g_ptr_array_new_with_free_func(g_free);
	metas = g_ptr_array_new_with_free_func((GDestroyNotify)g_hash_table_destroy);
	send_metas = g_ptr_array_new_with_free_func((GDestroyNotify)g_hash_table_destroy);
	written = g_ptr_array_new_with_free_func((GDestroyNotify)written_free);
	mam_done = g_ptr_array_new_with_free_func(g_free);
	kv = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	/* Capture outgoing stanzas instead of writing them to a socket. */
	purple_signal_disconnect(jabber_plugin, "jabber-sending-xmlnode", jabber_plugin,
	                         PURPLE_CALLBACK(jabber_send_signal_cb));
	purple_signal_connect(jabber_plugin, "jabber-sending-xmlnode", &sent,
	                      PURPLE_CALLBACK(sending_cb), NULL);
	purple_signal_connect(jabber_plugin, "jabber-kv-load", &kv,
	                      PURPLE_CALLBACK(kv_load_cb), NULL);
	purple_signal_connect(jabber_plugin, "jabber-kv-store", &kv,
	                      PURPLE_CALLBACK(kv_store_cb), NULL);
	purple_signal_connect(jabber_plugin, "mam-query-done", &kv,
	                      PURPLE_CALLBACK(mam_done_cb), NULL);
	purple_signal_connect(purple_conversations_get_handle(), "receiving-message-meta",
	                      &metas, PURPLE_CALLBACK(meta_cb), NULL);
	purple_signal_connect(purple_conversations_get_handle(), "sending-message-meta",
	                      &metas, PURPLE_CALLBACK(send_meta_cb), NULL);
	purple_signal_connect(purple_conversations_get_handle(), "writing-im-msg",
	                      &written, PURPLE_CALLBACK(writing_cb), NULL);
	purple_signal_connect(purple_conversations_get_handle(), "writing-chat-msg",
	                      &written, PURPLE_CALLBACK(writing_cb), NULL);

	test_ids();
	test_fallback();
	test_carbons_unwrap();
	test_mam_pure();
	test_selfping_pure();
	test_bookmarks_pure();

	setup_stream();
	CHECK(jabber_ui_supports_message_meta());
	test_live_ids();
	test_carbons();
	test_mam_catchup();
	test_fallback_integration();
	test_send();
	test_muc();
	test_bookmarks_integration();

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
