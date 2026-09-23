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
#include "httpupload.h"
#include "iq.h"
#include "mam.h"
#include "message.h"
#include "presence.h"
#include "displayed.h"
#include "reactions.h"
#include "styling.h"

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

/**************************************************************************
 * M8 message semantics
 **************************************************************************/

/* The IPC commands return gboolean; purple_plugin_ipc_call returns it as a pointer. */
#define IPC_BOOL(name, ...) \
	GPOINTER_TO_INT(purple_plugin_ipc_call(jabber_plugin, name, NULL, __VA_ARGS__))

static GPtrArray *events = NULL;       /* char* summaries of the event signals */
static gboolean event_return = FALSE;  /* what the event handlers return */

static gboolean
receipt_cb(PurpleAccount *a, const char *conv, const char *id, const char *state,
           const char *sender, gpointer data)
{
	g_ptr_array_add(events, g_strdup_printf("receipt|%s|%s|%s|%s", conv, id,
			state, sender ? sender : ""));
	return event_return;
}

static gboolean
corrected_cb(PurpleAccount *a, const char *conv, const char *target,
             const char *new_id, const char *body, const char *sender, gpointer data)
{
	g_ptr_array_add(events, g_strdup_printf("corrected|%s|%s|%s|%s", conv, target,
			body, sender ? sender : ""));
	return event_return;
}

static gboolean
reaction_cb(PurpleAccount *a, const char *conv, const char *target,
            const char *emoji, const char *sender, gpointer add, gpointer data)
{
	g_ptr_array_add(events, g_strdup_printf("reaction|%s|%s|%s|%s|%c", conv, target,
			emoji, sender ? sender : "", GPOINTER_TO_INT(add) ? '+' : '-'));
	return event_return;
}

static gboolean
retracted_cb(PurpleAccount *a, const char *conv, const char *target,
             const char *sender, const char *reason, gpointer data)
{
	g_ptr_array_add(events, g_strdup_printf("retracted|%s|%s|%s|%s", conv, target,
			sender ? sender : "", reason ? reason : ""));
	return event_return;
}

static const char *
event_at(guint i)
{
	return i < events->len ? g_ptr_array_index(events, i) : NULL;
}

static void
set_meta_ui(gboolean on)
{
	if (on)
		g_hash_table_insert(ui_info, "message-meta", "1");
	else
		g_hash_table_remove(ui_info, "message-meta");
	jabber_ui_message_meta_reset();
}

static void
reset_semantics(void)
{
	reset_capture();
	g_ptr_array_set_size(events, 0);
}

static JabberChat *
join_room(const char *room, const char *features)
{
	JabberChat *chat;
	GHashTable *components;
	char *iqid, *xml;

	components = g_hash_table_new(g_str_hash, g_str_equal);
	g_hash_table_insert(components, "room", (char *)room);
	g_hash_table_insert(components, "server", "muc.example.org");
	g_hash_table_insert(components, "handle", "me");
	reset_capture();
	chat = jabber_join_chat(js, room, "muc.example.org", "me", NULL, components);
	g_hash_table_destroy(components);
	if (chat == NULL)
		return NULL;

	iqid = sent_id_containing("http://jabber.org/protocol/disco#info");
	xml = g_strdup_printf("<iq xmlns='jabber:client' type='result' from='%s@muc.example.org' id='%s'>"
		"<query xmlns='http://jabber.org/protocol/disco#info'>"
		"<identity category='conference' type='text'/>%s"
		"<feature var='http://jabber.org/protocol/muc'/></query></iq>", room, iqid, features);
	feed(xml);
	g_free(xml);
	g_free(iqid);

	xml = g_strdup_printf("<presence xmlns='jabber:client' from='%s@muc.example.org/me' "
		"to='me@example.org/pidgin'><x xmlns='http://jabber.org/protocol/muc#user'>"
		"<item affiliation='owner' role='moderator'/><status code='110'/></x></presence>", room);
	feed(xml);
	g_free(xml);
	return chat;
}

static void
test_styling_pure(void)
{
	char *s;

#define STYLED(html, expected) do { s = jabber_styling_html_to_text(html); \
	CHECK_STR(s, expected); g_free(s); } while (0)

	STYLED("<b>bold</b> <i>it</i> <s>st</s> <u>u</u> <font color=\"#ff0000\">red</font>",
	       "*bold* _it_ ~st~ u red");
	STYLED("a &lt; b &amp;&amp; c<br>next", "a < b && c\nnext");
	STYLED("<b>trailing </b>x", "*trailing* x");
	STYLED("<b> </b>x", " x");
	STYLED("<b></b>", "");
	STYLED("<code>x*y</code> z", "`x*y` z");
	STYLED("<pre>line1<br>line2</pre>after", "```\nline1\nline2\n```\nafter");
	STYLED("<span style='font-weight: bold; font-style: italic;'>s</span>", "*_s_*");
	STYLED("<b><b>nested</b> more</b>", "*nested more*");
	STYLED("<strong>a</strong><em>b</em><del>c</del><tt>d</tt>", "*a*_b_~c~`d`");
	STYLED("already *plain* _0393_", "already *plain* _0393_");
	STYLED("<b>unclosed", "*unclosed*");
	STYLED("<code><b>x</b></code>", "`x`");
	STYLED("line<br/>two", "line\ntwo");
	s = jabber_styling_html_to_text("<a href=\"http://x.org/\">site</a>");
	CHECK(s && strstr(s, "site") && strstr(s, "http://x.org/"));
	g_free(s);
	CHECK(jabber_styling_html_to_text(NULL) == NULL);
#undef STYLED
}

static void
test_reactions_pure(void)
{
	xmlnode *node;
	char **set, **old, **split;
	GPtrArray *added = g_ptr_array_new(), *removed = g_ptr_array_new();

	node = xmlnode_from_str("<reactions xmlns='urn:xmpp:reactions:0' id='x'>"
		"<reaction>\xf0\x9f\x91\x8d</reaction><reaction> \xf0\x9f\x8e\x89 </reaction>"
		"<reaction>\xf0\x9f\x91\x8d</reaction><reaction/></reactions>", -1);
	set = jabber_reactions_parse(node);
	CHECK(g_strv_length(set) == 2);
	CHECK_STR(set[0], "\xf0\x9f\x91\x8d");
	CHECK_STR(set[1], "\xf0\x9f\x8e\x89");
	xmlnode_free(node);

	split = jabber_reactions_split("  \xf0\x9f\x8e\x89 \xe2\x9d\xa4  \xf0\x9f\x8e\x89");
	CHECK(g_strv_length(split) == 2);
	old = jabber_reactions_split("");
	CHECK(g_strv_length(old) == 0);

	jabber_reactions_diff(set, split, added, removed);
	CHECK(added->len == 1 && purple_strequal(g_ptr_array_index(added, 0), "\xe2\x9d\xa4"));
	CHECK(removed->len == 1 && purple_strequal(g_ptr_array_index(removed, 0),
	                                           "\xf0\x9f\x91\x8d"));
	g_ptr_array_set_size(added, 0);
	g_ptr_array_set_size(removed, 0);
	jabber_reactions_diff(NULL, set, added, removed);
	CHECK(added->len == 2 && removed->len == 0);

	g_strfreev(set);
	g_strfreev(split);
	g_strfreev(old);
	g_ptr_array_free(added, TRUE);
	g_ptr_array_free(removed, TRUE);
}

static void
test_receipts(void)
{
	const char *str;
	Written *w;
	gboolean ok;

	purple_blist_add_buddy(purple_buddy_new(account, "friend@example.net", NULL),
	                       NULL, NULL, NULL);

	/* a request is answered; markable goes into the metadata */
	reset_semantics();
	feed("<message xmlns='jabber:client' from='friend@example.net/phone' type='chat' id='m1'>"
	     "<body>hi</body><request xmlns='urn:xmpp:receipts'/>"
	     "<markable xmlns='urn:xmpp:chat-markers:0'/></message>");
	str = sent_containing("urn:xmpp:receipts");
	CHECK(str && strstr(str, "<received xmlns='urn:xmpp:receipts' id='m1'/>"));
	CHECK(str && strstr(str, "to='friend@example.net/phone'") && strstr(str, "type='chat'"));
	CHECK_STR(meta_get(last_meta(), "markable"), "1");
	w = last_written();
	CHECK(w && purple_strequal(w->message, "hi"));

	/* not to strangers, not from the archive or carbons */
	reset_semantics();
	feed("<message xmlns='jabber:client' from='stranger@example.com/x' type='chat' id='m2'>"
	     "<body>hi</body><request xmlns='urn:xmpp:receipts'/></message>");
	CHECK(sent_containing("urn:xmpp:receipts") == NULL);
	feed("<message xmlns='jabber:client' from='me@example.org'>"
	     "<received xmlns='urn:xmpp:carbons:2'><forwarded xmlns='urn:xmpp:forward:0'>"
	     "<message xmlns='jabber:client' from='friend@example.net/phone' "
	     "to='me@example.org/other' type='chat' id='m3'><body>c</body>"
	     "<request xmlns='urn:xmpp:receipts'/></message></forwarded></received></message>");
	CHECK(sent_containing("urn:xmpp:receipts") == NULL);
	CHECK(written->len == 2);

	/* incoming receipt -> message-receipt delivered, nothing written */
	reset_semantics();
	feed("<message xmlns='jabber:client' from='friend@example.net/phone' id='x1'>"
	     "<received xmlns='urn:xmpp:receipts' id='out1'/></message>");
	CHECK_STR(event_at(0), "receipt|friend@example.net/phone|out1|delivered|friend@example.net/phone");
	CHECK(written->len == 0);

	/* markers */
	reset_semantics();
	feed("<message xmlns='jabber:client' from='friend@example.net/phone' type='chat' id='x2'>"
	     "<displayed xmlns='urn:xmpp:chat-markers:0' id='out1'/></message>");
	feed("<message xmlns='jabber:client' from='friend@example.net/phone' type='chat' id='x3'>"
	     "<received xmlns='urn:xmpp:chat-markers:0' id='out2'/></message>");
	feed("<message xmlns='jabber:client' from='friend@example.net/phone' type='chat' id='x4'>"
	     "<acknowledged xmlns='urn:xmpp:chat-markers:0' id='out3'/></message>");
	CHECK_STR(event_at(0), "receipt|friend@example.net/phone|out1|displayed|friend@example.net/phone");
	CHECK_STR(event_at(1), "receipt|friend@example.net/phone|out2|delivered|friend@example.net/phone");
	CHECK_STR(event_at(2), "receipt|friend@example.net/phone|out3|displayed|friend@example.net/phone");
	CHECK(written->len == 0);

	/* our own displayed marker from another device clears unread here */
	reset_semantics();
	feed("<message xmlns='jabber:client' from='me@example.org'>"
	     "<sent xmlns='urn:xmpp:carbons:2'><forwarded xmlns='urn:xmpp:forward:0'>"
	     "<message xmlns='jabber:client' from='me@example.org/other' "
	     "to='friend@example.net' type='chat' id='x5'>"
	     "<displayed xmlns='urn:xmpp:chat-markers:0' id='in9'/></message>"
	     "</forwarded></sent></message>");
	CHECK_STR(event_at(0), "receipt|friend@example.net|in9|displayed|me@example.org");
	CHECK(written->len == 0);

	/* send path: request + markable, plain body, no XHTML-IM */
	reset_semantics();
	jabber_message_send_im(js->gc, "friend@example.net", "<b>bold</b> &amp; plain", 0);
	str = sent_containing("<body>");
	CHECK(str && strstr(str, "<request xmlns='urn:xmpp:receipts'/>"));
	CHECK(str && strstr(str, "<markable xmlns='urn:xmpp:chat-markers:0'/>"));
	CHECK(str && strstr(str, "<body>*bold* &amp;amp; plain</body>") == NULL);
	CHECK(str && strstr(str, "<body>*bold* &amp; plain</body>"));
	CHECK(str && strstr(str, "<html") == NULL);

	/* without message-meta: answered, but no request/markable and no events */
	set_meta_ui(FALSE);
	reset_semantics();
	jabber_message_send_im(js->gc, "friend@example.net", "<i>x</i>", 0);
	str = sent_containing("<body>");
	CHECK(str && strstr(str, "urn:xmpp:receipts") == NULL);
	CHECK(str && strstr(str, "markable") == NULL);
	CHECK(str && strstr(str, "<body>_x_</body>"));
	reset_semantics();
	feed("<message xmlns='jabber:client' from='friend@example.net/phone' type='chat' id='m4'>"
	     "<body>hi</body><request xmlns='urn:xmpp:receipts'/></message>");
	CHECK(sent_containing("id='m4'") != NULL);
	feed("<message xmlns='jabber:client' from='friend@example.net/phone' id='x6'>"
	     "<received xmlns='urn:xmpp:receipts' id='out1'/></message>");
	CHECK(events->len == 0);
	set_meta_ui(TRUE);

	/* send-marker IPC */
	reset_semantics();
	ok = FALSE;
	ok = IPC_BOOL("send-marker", account,
	                       "friend@example.net/phone", "in7", "displayed");
	CHECK(ok);
	str = sent_containing("chat-markers");
	CHECK(str && strstr(str, "<displayed xmlns='urn:xmpp:chat-markers:0' id='in7'/>"));
	CHECK(str && strstr(str, "to='friend@example.net/phone'") && strstr(str, "type='chat'"));
	CHECK(str && strstr(str, "<store xmlns='urn:xmpp:hints'/>"));
	ok = TRUE;
	ok = IPC_BOOL("send-marker", account,
	                       "friend@example.net", "in7", "bogus");
	CHECK(!ok);
	ok = TRUE;
	ok = IPC_BOOL("send-marker", account,
	                       "friend@example.net", NULL, "displayed");
	CHECK(!ok);
	ok = TRUE;
	ok = IPC_BOOL("send-marker", NULL,
	                       "friend@example.net", "in7", "displayed");
	CHECK(!ok);
	{
		PurpleAccount *irc = purple_account_new("nick@irc.example.org", "prpl-irc");
		ok = TRUE;
		ok = IPC_BOOL("send-marker", irc,
		                       "friend@example.net", "in7", "displayed");
		CHECK(!ok);
		purple_account_destroy(irc);
	}
	{
		PurpleAccount *off = purple_account_new("off@example.org", "prpl-jabber");
		ok = TRUE;
		ok = IPC_BOOL("send-marker", off,
		                       "friend@example.net", "in7", "displayed");
		CHECK(!ok);
		purple_account_destroy(off);
	}
}

static void
test_markers_muc(void)
{
	JabberChat *chat, *anon;
	const char *str;
	gboolean ok;

	anon = join_room("anon", "");
	chat = join_room("sem", "<feature var='urn:xmpp:occupant-id:0'/>"
	                        "<feature var='muc_nonanonymous'/>"
	                        "<feature var='urn:xmpp:message-moderate:1'/>");
	CHECK(anon && !anon->nonanonymous);
	CHECK(chat && chat->nonanonymous && chat->occupant_id_supported);
	CHECK(chat && purple_strequal(chat->moderation_ns, "urn:xmpp:message-moderate:1"));
	CHECK(chat && chat->conv != NULL);

	reset_semantics();
	ok = TRUE;
	ok = IPC_BOOL("send-marker", account,
	                       "anon@muc.example.org", "S1", "displayed");
	CHECK(!ok);
	CHECK(sent->len == 0);
	ok = FALSE;
	ok = IPC_BOOL("send-marker", account,
	                       "sem@muc.example.org", "S1", NULL);
	CHECK(ok);
	str = sent_containing("chat-markers");
	CHECK(str && strstr(str, "type='groupchat'") && strstr(str, "to='sem@muc.example.org'"));
	CHECK(str && strstr(str, "<displayed xmlns='urn:xmpp:chat-markers:0' id='S1'/>"));

	/* room markers -> message-receipt with the nick */
	reset_semantics();
	feed("<message xmlns='jabber:client' from='sem@muc.example.org/alice' type='groupchat' id='k1'>"
	     "<displayed xmlns='urn:xmpp:chat-markers:0' id='S0'/>"
	     "<occupant-id xmlns='urn:xmpp:occupant-id:0' id='occ-a'/></message>");
	CHECK_STR(event_at(0), "receipt|sem@muc.example.org|S0|displayed|alice");
	/* XEP-0184 receipts are not for rooms */
	feed("<message xmlns='jabber:client' from='sem@muc.example.org/alice' type='groupchat' id='k2'>"
	     "<received xmlns='urn:xmpp:receipts' id='S0'/></message>");
	CHECK(events->len == 1);
	/* our own marker reflected: our bare JID */
	feed("<message xmlns='jabber:client' from='sem@muc.example.org/me' type='groupchat' id='k3'>"
	     "<displayed xmlns='urn:xmpp:chat-markers:0' id='S2'/></message>");
	CHECK_STR(event_at(1), "receipt|sem@muc.example.org|S2|displayed|me@example.org");

	/* send_chat carries markable, no XHTML */
	reset_semantics();
	jabber_message_send_chat(js->gc, chat->id, "<s>gone</s>", 0);
	str = sent_containing("<body>");
	CHECK(str && strstr(str, "<markable xmlns='urn:xmpp:chat-markers:0'/>"));
	CHECK(str && strstr(str, "urn:xmpp:receipts") == NULL);
	CHECK(str && strstr(str, "<body>~gone~</body>"));
}

static void
test_corrections(void)
{
	GHashTable *m;
	Written *w;
	const char *str;
	gboolean ok;

	/* handled by the UI: no new line */
	event_return = TRUE;
	reset_semantics();
	feed("<message xmlns='jabber:client' from='friend@example.net/phone' type='chat' id='c2'>"
	     "<body>fixed &amp; better</body><replace xmlns='urn:xmpp:message-correct:0' id='c1'/></message>");
	CHECK_STR(event_at(0), "corrected|friend@example.net/phone|c1|fixed &amp; better|friend@example.net/phone");
	CHECK(written->len == 0);
	CHECK(metas->len == 0);

	/* not handled: written with the edit prefix, correction-of in the meta */
	event_return = FALSE;
	reset_semantics();
	feed("<message xmlns='jabber:client' from='friend@example.net/phone' type='chat' id='c3'>"
	     "<body>fixed</body><replace xmlns='urn:xmpp:message-correct:0' id='c1'/></message>");
	CHECK(events->len == 1);
	w = last_written();
	CHECK(w && purple_strequal(w->message, "edit: fixed"));
	CHECK_STR(meta_get(last_meta(), "correction-of"), "c1");

	/* no message-meta UI: the same text fallback, no signal */
	set_meta_ui(FALSE);
	reset_semantics();
	feed("<message xmlns='jabber:client' from='friend@example.net/phone' type='chat' id='c4'>"
	     "<body>fixed</body><replace xmlns='urn:xmpp:message-correct:0' id='c1'/></message>");
	CHECK(events->len == 0);
	w = last_written();
	CHECK(w && purple_strequal(w->message, "edit: fixed"));
	set_meta_ui(TRUE);

	/* rooms: matched by occupant-id, not by nick */
	event_return = TRUE;
	reset_semantics();
	feed("<message xmlns='jabber:client' from='sem@muc.example.org/alice' type='groupchat' id='g1'>"
	     "<body>orig</body><occupant-id xmlns='urn:xmpp:occupant-id:0' id='occ-a'/>"
	     "<stanza-id xmlns='urn:xmpp:sid:0' by='sem@muc.example.org' id='SG1'/></message>");
	CHECK(written->len == 1);
	feed("<message xmlns='jabber:client' from='sem@muc.example.org/alice' type='groupchat' id='g2'>"
	     "<body>evil</body><replace xmlns='urn:xmpp:message-correct:0' id='g1'/>"
	     "<occupant-id xmlns='urn:xmpp:occupant-id:0' id='occ-evil'/></message>");
	CHECK(events->len == 0);
	CHECK(written->len == 1);
	feed("<message xmlns='jabber:client' from='sem@muc.example.org/alice2' type='groupchat' id='g3'>"
	     "<body>orig!</body><replace xmlns='urn:xmpp:message-correct:0' id='g1'/>"
	     "<occupant-id xmlns='urn:xmpp:occupant-id:0' id='occ-a'/></message>");
	CHECK_STR(event_at(0), "corrected|sem@muc.example.org|g1|orig!|alice2");
	CHECK(written->len == 1);

	/* send-correction, 1:1: sent, sending-message-meta, reported locally */
	reset_semantics();
	ok = FALSE;
	ok = IPC_BOOL("send-correction", account,
	                       "friend@example.net", "c0", "new text");
	CHECK(ok);
	str = sent_containing("message-correct");
	CHECK(str && strstr(str, "<replace xmlns='urn:xmpp:message-correct:0' id='c0'/>"));
	CHECK(str && strstr(str, "<body>new text</body>") && strstr(str, "origin-id"));
	CHECK(str && strstr(str, "type='chat'") && strstr(str, "<request xmlns='urn:xmpp:receipts'/>"));
	m = send_metas->len ? g_ptr_array_index(send_metas, 0) : NULL;
	CHECK_STR(meta_get(m, "correction-of"), "c0");
	CHECK_STR(meta_get(m, "conv-type"), "im");
	CHECK(m && meta_get(m, "stanza-id") != NULL);
	CHECK_STR(event_at(0), "corrected|friend@example.net|c0|new text|me@example.org/pidgin");
	CHECK(written->len == 0);

	event_return = FALSE;
	reset_semantics();
	ok = IPC_BOOL("send-correction", account,
	                       "friend@example.net", "c0", "newer");
	w = last_written();
	CHECK(w && purple_strequal(w->message, "edit: newer") && (w->flags & PURPLE_MESSAGE_SEND));

	/* rooms: only sent; the reflection reports it */
	reset_semantics();
	ok = FALSE;
	ok = IPC_BOOL("send-correction", account,
	                       "sem@muc.example.org", "g9", "room fix");
	CHECK(ok);
	str = sent_containing("message-correct");
	CHECK(str && strstr(str, "type='groupchat'") && strstr(str, "urn:xmpp:receipts") == NULL);
	CHECK(events->len == 0 && written->len == 0);

	ok = TRUE;
	ok = IPC_BOOL("send-correction", account,
	                       "friend@example.net", NULL, "x");
	CHECK(!ok);
	ok = TRUE;
	ok = IPC_BOOL("send-correction", account,
	                       "friend@example.net", "c0", "");
	CHECK(!ok);
}

#define THUMBS "\xf0\x9f\x91\x8d"
#define PARTY "\xf0\x9f\x8e\x89"

static void
test_reactions(void)
{
	Written *w;
	const char *str;
	gboolean ok;

	jabber_reactions_reset();

	/* a reaction-only message (fallback body stripped) is not dropped */
	event_return = TRUE;
	reset_semantics();
	feed("<message xmlns='jabber:client' from='friend@example.net/phone' type='chat' id='r1'>"
	     "<reactions xmlns='urn:xmpp:reactions:0' id='m1'><reaction>" THUMBS "</reaction>"
	     "<reaction>" PARTY "</reaction></reactions><body>" THUMBS PARTY "</body>"
	     "<fallback xmlns='urn:xmpp:fallback:0' for='urn:xmpp:reactions:0'/>"
	     "<store xmlns='urn:xmpp:hints'/></message>");
	CHECK(events->len == 2);
	CHECK_STR(event_at(0), "reaction|friend@example.net/phone|m1|" THUMBS "|friend@example.net/phone|+");
	CHECK_STR(event_at(1), "reaction|friend@example.net/phone|m1|" PARTY "|friend@example.net/phone|+");
	CHECK(written->len == 0);

	/* the next set: one removal; the same set again: nothing */
	reset_semantics();
	feed("<message xmlns='jabber:client' from='friend@example.net/laptop' type='chat' id='r2'>"
	     "<reactions xmlns='urn:xmpp:reactions:0' id='m1'><reaction>" PARTY "</reaction>"
	     "</reactions></message>");
	CHECK(events->len == 1);
	CHECK_STR(event_at(0), "reaction|friend@example.net/laptop|m1|" THUMBS "|friend@example.net/laptop|-");
	feed("<message xmlns='jabber:client' from='friend@example.net/laptop' type='chat' id='r3'>"
	     "<reactions xmlns='urn:xmpp:reactions:0' id='m1'><reaction>" PARTY "</reaction>"
	     "</reactions></message>");
	CHECK(events->len == 1);
	feed("<message xmlns='jabber:client' from='friend@example.net/laptop' type='chat' id='r4'>"
	     "<reactions xmlns='urn:xmpp:reactions:0' id='m1'/></message>");
	CHECK_STR(event_at(1), "reaction|friend@example.net/laptop|m1|" PARTY "|friend@example.net/laptop|-");
	CHECK(written->len == 0);

	/* not handled: a text line instead of the fallback body */
	event_return = FALSE;
	reset_semantics();
	feed("<message xmlns='jabber:client' from='friend@example.net/phone' type='chat' id='r5'>"
	     "<reactions xmlns='urn:xmpp:reactions:0' id='m2'><reaction>" THUMBS "</reaction>"
	     "</reactions><body>" THUMBS "</body>"
	     "<fallback xmlns='urn:xmpp:fallback:0' for='urn:xmpp:reactions:0'/></message>");
	w = last_written();
	CHECK(w && purple_strequal(w->message, "friend@example.net reacted " THUMBS " to a message"));
	CHECK(w && (w->flags & PURPLE_MESSAGE_SYSTEM));
	CHECK(written->len == 1);

	/* no message-meta UI: the same text line, not the raw fallback */
	set_meta_ui(FALSE);
	reset_semantics();
	feed("<message xmlns='jabber:client' from='friend@example.net/phone' type='chat' id='r6'>"
	     "<reactions xmlns='urn:xmpp:reactions:0' id='m3'><reaction>" THUMBS "</reaction>"
	     "<reaction>" PARTY "</reaction></reactions><body>" THUMBS PARTY "</body>"
	     "<fallback xmlns='urn:xmpp:fallback:0' for='urn:xmpp:reactions:0'/></message>");
	CHECK(events->len == 0);
	w = last_written();
	CHECK(w && purple_strequal(w->message,
	      "friend@example.net reacted " THUMBS " " PARTY " to a message"));
	CHECK(written->len == 1);
	feed("<message xmlns='jabber:client' from='friend@example.net/phone' type='chat' id='r7'>"
	     "<reactions xmlns='urn:xmpp:reactions:0' id='m3'/></message>");
	w = last_written();
	CHECK(w && purple_strequal(w->message,
	      "friend@example.net removed the reaction " THUMBS " " PARTY));
	set_meta_ui(TRUE);

	/* rooms: the nick is the sender, the occupant-id the identity */
	event_return = TRUE;
	reset_semantics();
	feed("<message xmlns='jabber:client' from='sem@muc.example.org/alice' type='groupchat' id='r8'>"
	     "<reactions xmlns='urn:xmpp:reactions:0' id='SG1'><reaction>" THUMBS "</reaction></reactions>"
	     "<occupant-id xmlns='urn:xmpp:occupant-id:0' id='occ-a'/></message>");
	CHECK_STR(event_at(0), "reaction|sem@muc.example.org|SG1|" THUMBS "|alice|+");
	/* same occupant, new nick: still the same set */
	feed("<message xmlns='jabber:client' from='sem@muc.example.org/alice2' type='groupchat' id='r9'>"
	     "<reactions xmlns='urn:xmpp:reactions:0' id='SG1'><reaction>" THUMBS "</reaction></reactions>"
	     "<occupant-id xmlns='urn:xmpp:occupant-id:0' id='occ-a'/></message>");
	CHECK(events->len == 1);
	CHECK(written->len == 0);

	/* send-reaction, 1:1: the complete set, store hint, fallback body */
	reset_semantics();
	ok = FALSE;
	ok = IPC_BOOL("send-reaction", account,
	                       "friend@example.net", "m5", THUMBS " " PARTY " " THUMBS);
	CHECK(ok);
	str = sent_containing("urn:xmpp:reactions:0");
	CHECK(str && strstr(str, "<reactions xmlns='urn:xmpp:reactions:0' id='m5'><reaction>"
	                    THUMBS "</reaction><reaction>" PARTY "</reaction></reactions>"));
	CHECK(str && strstr(str, "<body>" THUMBS " " PARTY "</body>"));
	CHECK(str && strstr(str, "<fallback xmlns='urn:xmpp:fallback:0' for='urn:xmpp:reactions:0'/>"));
	CHECK(str && strstr(str, "<store xmlns='urn:xmpp:hints'/>") && strstr(str, "origin-id"));
	CHECK(events->len == 2);
	CHECK_STR(event_at(0), "reaction|friend@example.net|m5|" THUMBS "|me@example.org/pidgin|+");
	reset_semantics();
	ok = IPC_BOOL("send-reaction", account,
	                       "friend@example.net", "m5", PARTY);
	CHECK(events->len == 1);
	CHECK_STR(event_at(0), "reaction|friend@example.net|m5|" THUMBS "|me@example.org/pidgin|-");
	/* our own reactions coming back as a sent carbon: no change */
	feed("<message xmlns='jabber:client' from='me@example.org'>"
	     "<sent xmlns='urn:xmpp:carbons:2'><forwarded xmlns='urn:xmpp:forward:0'>"
	     "<message xmlns='jabber:client' from='me@example.org/other' "
	     "to='friend@example.net' type='chat' id='r10'>"
	     "<reactions xmlns='urn:xmpp:reactions:0' id='m5'><reaction>" PARTY "</reaction></reactions>"
	     "</message></forwarded></sent></message>");
	CHECK(events->len == 1);
	reset_semantics();
	ok = IPC_BOOL("send-reaction", account,
	                       "friend@example.net", "m5", "");
	CHECK(ok);
	str = sent_containing("urn:xmpp:reactions:0");
	CHECK(str && strstr(str, "<reactions xmlns='urn:xmpp:reactions:0' id='m5'/>"));
	CHECK(str && strstr(str, "<body>") == NULL);
	CHECK_STR(event_at(0), "reaction|friend@example.net|m5|" PARTY "|me@example.org/pidgin|-");

	/* rooms: groupchat, no local report */
	reset_semantics();
	ok = IPC_BOOL("send-reaction", account,
	                       "sem@muc.example.org", "SG1", PARTY);
	CHECK(ok);
	str = sent_containing("urn:xmpp:reactions:0");
	CHECK(str && strstr(str, "type='groupchat'"));
	CHECK(events->len == 0);

	ok = TRUE;
	ok = IPC_BOOL("send-reaction", account,
	                       "friend@example.net", "", PARTY);
	CHECK(!ok);
	event_return = FALSE;
}

static void
test_replies(void)
{
	GHashTable *m;
	Written *w;
	const char *str;
	gboolean ok;
	xmlnode *node, *body;
	GList *fallbacks;
	char *text, *stripped;

	reset_semantics();
	ok = FALSE;
	ok = IPC_BOOL("send-reply", account,
	                       "friend@example.net", "q1", "friend@example.net/phone",
	                       "Hello\nworld\n", "Hi <you>!");
	CHECK(ok);
	str = sent_containing("urn:xmpp:reply:0");
	CHECK(str && strstr(str, "<body>&gt; Hello\n&gt; world\nHi &lt;you&gt;!</body>"));
	CHECK(str && strstr(str, "<reply xmlns='urn:xmpp:reply:0' to='friend@example.net/phone' id='q1'/>"));
	CHECK(str && strstr(str, "<fallback xmlns='urn:xmpp:fallback:0' for='urn:xmpp:reply:0'>"
	                    "<body start='0' end='16'/></fallback>"));
	CHECK(str && strstr(str, "<markable xmlns='urn:xmpp:chat-markers:0'/>"));
	m = send_metas->len ? g_ptr_array_index(send_metas, 0) : NULL;
	CHECK_STR(meta_get(m, "reply-to"), "q1");
	CHECK_STR(meta_get(m, "reply-to-sender"), "friend@example.net/phone");
	w = last_written();
	CHECK(w && purple_strequal(w->message, "Hi &lt;you&gt;!") && (w->flags & PURPLE_MESSAGE_SEND));

	/* what a receiver strips is exactly the quote */
	node = str ? xmlnode_from_str(str, -1) : NULL;
	body = node ? xmlnode_get_child(node, "body") : NULL;
	text = body ? xmlnode_get_data(body) : NULL;
	fallbacks = node ? jabber_fallback_parse(node) : NULL;
	stripped = jabber_fallback_strip(text, fallbacks, jabber_native_fallback_namespaces);
	CHECK_STR(stripped, "Hi <you>!");
	g_free(stripped);
	g_free(text);
	jabber_fallback_free(fallbacks);
	if (node)
		xmlnode_free(node);

	/* no quote: no fallback; rooms: groupchat, not written */
	reset_semantics();
	ok = IPC_BOOL("send-reply", account,
	                       "sem@muc.example.org", "SG1", "sem@muc.example.org/alice",
	                       NULL, "sure");
	CHECK(ok);
	str = sent_containing("urn:xmpp:reply:0");
	CHECK(str && strstr(str, "type='groupchat'") && strstr(str, "<body>sure</body>"));
	CHECK(str && strstr(str, "urn:xmpp:fallback:0") == NULL);
	CHECK(written->len == 0);
	m = send_metas->len ? g_ptr_array_index(send_metas, 0) : NULL;
	CHECK_STR(meta_get(m, "conv-type"), "chat");
	CHECK_STR(meta_get(m, "reply-to"), "SG1");

	ok = TRUE;
	ok = IPC_BOOL("send-reply", account,
	                       "friend@example.net", NULL, NULL, NULL, "x");
	CHECK(!ok);
}

static void
test_retractions(void)
{
	Written *w;
	const char *str;
	gboolean ok;
	JabberChat *chat = jabber_chat_find(js, "sem", "muc.example.org");

	event_return = TRUE;
	reset_semantics();
	feed("<message xmlns='jabber:client' from='friend@example.net/phone' type='chat' id='t1'>"
	     "<retract xmlns='urn:xmpp:message-retract:1' id='m5'/>"
	     "<fallback xmlns='urn:xmpp:fallback:0' for='urn:xmpp:message-retract:1'/>"
	     "<body>This person attempted to retract a previous message</body>"
	     "<store xmlns='urn:xmpp:hints'/></message>");
	CHECK_STR(event_at(0), "retracted|friend@example.net/phone|m5|friend@example.net/phone|");
	CHECK(written->len == 0);
	feed("<message xmlns='jabber:client' from='friend@example.net/phone' type='chat' id='t2'>"
	     "<apply-to xmlns='urn:xmpp:fasten:0' id='m6'>"
	     "<retract xmlns='urn:xmpp:message-retract:0'/></apply-to>"
	     "<body>fallback</body></message>");
	CHECK_STR(event_at(1), "retracted|friend@example.net/phone|m6|friend@example.net/phone|");
	/* both forms in one stanza: one event, for the current one */
	feed("<message xmlns='jabber:client' from='friend@example.net/phone' type='chat' id='t3'>"
	     "<apply-to xmlns='urn:xmpp:fasten:0' id='m7'>"
	     "<retract xmlns='urn:xmpp:message-retract:0'/></apply-to>"
	     "<retract xmlns='urn:xmpp:message-retract:1' id='m7'/></message>");
	CHECK(events->len == 3);
	CHECK(written->len == 0);

	/* not handled / no message-meta UI: a text line */
	event_return = FALSE;
	reset_semantics();
	feed("<message xmlns='jabber:client' from='friend@example.net/phone' type='chat' id='t4'>"
	     "<retract xmlns='urn:xmpp:message-retract:1' id='m5'/><body>fb</body></message>");
	w = last_written();
	CHECK(w && purple_strequal(w->message, "friend@example.net retracted a message"));
	CHECK(written->len == 1);
	set_meta_ui(FALSE);
	reset_semantics();
	feed("<message xmlns='jabber:client' from='friend@example.net/phone' type='chat' id='t5'>"
	     "<retract xmlns='urn:xmpp:message-retract:1' id='m5'/><body>fb</body></message>");
	CHECK(events->len == 0);
	w = last_written();
	CHECK(w && purple_strequal(w->message, "friend@example.net retracted a message"));
	set_meta_ui(TRUE);

	/* moderation (0425 v0.3+), from the room itself */
	event_return = TRUE;
	reset_semantics();
	feed("<message xmlns='jabber:client' from='sem@muc.example.org' type='groupchat' id='t6'>"
	     "<retract xmlns='urn:xmpp:message-retract:1' id='SG1'>"
	     "<moderated xmlns='urn:xmpp:message-moderate:1' by='sem@muc.example.org/mod'>"
	     "<occupant-id xmlns='urn:xmpp:occupant-id:0' id='occ-m'/></moderated>"
	     "<reason>spam</reason></retract></message>");
	CHECK_STR(event_at(0), "retracted|sem@muc.example.org|SG1|mod|spam");
	/* 0425 v0.2 */
	feed("<message xmlns='jabber:client' from='sem@muc.example.org' type='groupchat' id='t7'>"
	     "<apply-to xmlns='urn:xmpp:fasten:0' id='SG2'>"
	     "<moderated xmlns='urn:xmpp:message-moderate:0' by='sem@muc.example.org/mod'>"
	     "<retract xmlns='urn:xmpp:message-retract:0'/><reason>old</reason></moderated>"
	     "</apply-to></message>");
	CHECK_STR(event_at(1), "retracted|sem@muc.example.org|SG2|mod|old");
	/* moderation claimed by an occupant: ignored */
	feed("<message xmlns='jabber:client' from='sem@muc.example.org/eve' type='groupchat' id='t8'>"
	     "<retract xmlns='urn:xmpp:message-retract:1' id='SG1'>"
	     "<moderated xmlns='urn:xmpp:message-moderate:1' by='sem@muc.example.org/eve'/>"
	     "</retract></message>");
	CHECK(events->len == 2);
	/* an occupant retracting someone else's message: ignored */
	feed("<message xmlns='jabber:client' from='sem@muc.example.org/alice' type='groupchat' id='t9'>"
	     "<retract xmlns='urn:xmpp:message-retract:1' id='g1'/>"
	     "<occupant-id xmlns='urn:xmpp:occupant-id:0' id='occ-evil'/></message>");
	CHECK(events->len == 2);
	feed("<message xmlns='jabber:client' from='sem@muc.example.org/alice' type='groupchat' id='t10'>"
	     "<retract xmlns='urn:xmpp:message-retract:1' id='g1'/>"
	     "<occupant-id xmlns='urn:xmpp:occupant-id:0' id='occ-a'/></message>");
	CHECK_STR(event_at(2), "retracted|sem@muc.example.org|g1|alice|");
	CHECK(written->len == 0);

	event_return = FALSE;
	reset_semantics();
	feed("<message xmlns='jabber:client' from='sem@muc.example.org' type='groupchat' id='t11'>"
	     "<retract xmlns='urn:xmpp:message-retract:1' id='SG3'>"
	     "<moderated xmlns='urn:xmpp:message-moderate:1' by='sem@muc.example.org/mod'/>"
	     "<reason>spam</reason></retract></message>");
	w = last_written();
	CHECK(w && purple_strequal(w->message, "mod removed a message: spam"));

	/* send-retraction: both forms, fallback, reported locally in 1:1 */
	event_return = TRUE;
	reset_semantics();
	ok = FALSE;
	ok = IPC_BOOL("send-retraction", account,
	                       "friend@example.net", "m8");
	CHECK(ok);
	str = sent_containing("message-retract");
	CHECK(str && strstr(str, "<retract xmlns='urn:xmpp:message-retract:1' id='m8'/>"));
	CHECK(str && strstr(str, "<apply-to xmlns='urn:xmpp:fasten:0' id='m8'>"
	                    "<retract xmlns='urn:xmpp:message-retract:0'/></apply-to>"));
	CHECK(str && strstr(str, "<fallback xmlns='urn:xmpp:fallback:0' for='urn:xmpp:message-retract:1'/>"));
	CHECK(str && strstr(str, "<body>") && strstr(str, "<store xmlns='urn:xmpp:hints'/>"));
	CHECK_STR(event_at(0), "retracted|friend@example.net|m8|me@example.org/pidgin|");
	event_return = FALSE;
	reset_semantics();
	ok = IPC_BOOL("send-retraction", account,
	                       "friend@example.net", "m9");
	w = last_written();
	CHECK(w && strstr(w->message, "retracted a message") && (w->flags & PURPLE_MESSAGE_SYSTEM));
	reset_semantics();
	ok = IPC_BOOL("send-retraction", account,
	                       "sem@muc.example.org", "g5");
	str = sent_containing("message-retract");
	CHECK(ok && str && strstr(str, "type='groupchat'"));
	CHECK(events->len == 0 && written->len == 0);

	/* send-moderation: the IQ to the room, in the room's version */
	reset_semantics();
	ok = FALSE;
	ok = IPC_BOOL("send-moderation", account,
	                       "sem@muc.example.org", "SG4", "off-topic");
	CHECK(ok);
	str = sent_containing("message-moderate");
	CHECK(str && strstr(str, "<iq") && strstr(str, "type='set'") &&
	      strstr(str, "to='sem@muc.example.org'"));
	CHECK(str && strstr(str, "<moderate xmlns='urn:xmpp:message-moderate:1' id='SG4'>"
	                    "<retract xmlns='urn:xmpp:message-retract:1'/>"
	                    "<reason>off-topic</reason></moderate>"));
	{
		/* an error goes into the room */
		char *iqid = sent_id_containing("message-moderate");
		char *xml = g_strdup_printf("<iq xmlns='jabber:client' type='error' "
			"from='sem@muc.example.org' id='%s'><error type='auth'>"
			"<forbidden xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/></error></iq>", iqid);
		reset_semantics();
		feed(xml);
		w = last_written();
		CHECK(w && strstr(w->message, "Could not remove the message") &&
		      (w->flags & PURPLE_MESSAGE_ERROR));
		g_free(xml);
		g_free(iqid);
	}
	if (chat)
		chat->moderation_ns = "urn:xmpp:message-moderate:0";
	reset_semantics();
	ok = IPC_BOOL("send-moderation", account,
	                       "sem@muc.example.org", "SG5", NULL);
	str = sent_containing("message-moderate");
	CHECK(ok && str && strstr(str, "<apply-to xmlns='urn:xmpp:fasten:0' id='SG5'>"
	                          "<moderate xmlns='urn:xmpp:message-moderate:0'>"
	                          "<retract xmlns='urn:xmpp:message-retract:0'/></moderate></apply-to>"));
	if (chat)
		chat->moderation_ns = "urn:xmpp:message-moderate:1";
	ok = TRUE;
	ok = IPC_BOOL("send-moderation", account,
	                       "friend@example.net", "SG5", NULL);
	CHECK(!ok);
	ok = TRUE;
	ok = IPC_BOOL("send-retraction", account,
	                       "friend@example.net", NULL);
	CHECK(!ok);
}

static void
test_styling_meta(void)
{
	reset_semantics();
	feed("<message xmlns='jabber:client' from='friend@example.net/phone' type='chat' id='u1'>"
	     "<body>*not bold*</body><unstyled xmlns='urn:xmpp:styling:0'/></message>");
	CHECK_STR(meta_get(last_meta(), "unstyled"), "1");
	feed("<message xmlns='jabber:client' from='friend@example.net/phone' type='chat' id='u2'>"
	     "<body>*bold*</body></message>");
	CHECK(meta_get(last_meta(), "unstyled") == NULL);
}

static void
test_displayed_sync(void)
{
	const char *str;
	gboolean ok;
	char *iqid, *xml;

	/* PEP notification from our own node */
	reset_semantics();
	feed("<message xmlns='jabber:client' from='me@example.org' to='me@example.org/pidgin' "
	     "type='headline'><event xmlns='http://jabber.org/protocol/pubsub#event'>"
	     "<items node='urn:xmpp:mds:displayed:0'><item id='friend@example.net'>"
	     "<displayed xmlns='urn:xmpp:mds:displayed:0'><stanza-id xmlns='urn:xmpp:sid:0' "
	     "id='S77' by='me@example.org'/></displayed></item></items></event></message>");
	CHECK_STR(event_at(0), "receipt|friend@example.net|S77|displayed|me@example.org");
	CHECK(written->len == 0);
	/* anyone else's: ignored */
	feed("<message xmlns='jabber:client' from='evil@example.com' to='me@example.org/pidgin' "
	     "type='headline'><event xmlns='http://jabber.org/protocol/pubsub#event'>"
	     "<items node='urn:xmpp:mds:displayed:0'><item id='friend@example.net'>"
	     "<displayed xmlns='urn:xmpp:mds:displayed:0'><stanza-id xmlns='urn:xmpp:sid:0' "
	     "id='S78' by='me@example.org'/></displayed></item></items></event></message>");
	CHECK(events->len == 1);

	/* initial fetch */
	reset_semantics();
	jabber_displayed_fetch(js);
	str = sent_containing("urn:xmpp:mds:displayed:0");
	CHECK(str && strstr(str, "type='get'") && strstr(str, "<items node='urn:xmpp:mds:displayed:0'/>"));
	iqid = sent_id_containing("urn:xmpp:mds:displayed:0");
	xml = g_strdup_printf("<iq xmlns='jabber:client' type='result' id='%s'>"
		"<pubsub xmlns='http://jabber.org/protocol/pubsub'><items node='urn:xmpp:mds:displayed:0'>"
		"<item id='sem@muc.example.org'><displayed xmlns='urn:xmpp:mds:displayed:0'>"
		"<stanza-id xmlns='urn:xmpp:sid:0' id='SG9' by='sem@muc.example.org'/></displayed></item>"
		"<item id='friend@example.net'><displayed xmlns='urn:xmpp:mds:displayed:0'>"
		"<stanza-id xmlns='urn:xmpp:sid:0' id='S80' by='me@example.org'/></displayed></item>"
		"</items></pubsub></iq>", iqid);
	feed(xml);
	g_free(xml);
	g_free(iqid);
	CHECK(events->len == 2);
	CHECK_STR(event_at(0), "receipt|sem@muc.example.org|SG9|displayed|me@example.org");
	CHECK_STR(event_at(1), "receipt|friend@example.net|S80|displayed|me@example.org");

	/* mds-publish IPC */
	reset_semantics();
	ok = FALSE;
	ok = IPC_BOOL("mds-publish", account,
	                       "friend@example.net/phone", "S81");
	CHECK(ok);
	str = sent_containing("urn:xmpp:mds:displayed:0");
	CHECK(str && strstr(str, "<publish node='urn:xmpp:mds:displayed:0'><item id='friend@example.net'>"
	                    "<displayed xmlns='urn:xmpp:mds:displayed:0'><stanza-id xmlns='urn:xmpp:sid:0' "
	                    "id='S81' by='me@example.org'/></displayed></item></publish>"));
	CHECK(str && strstr(str, "<publish-options>") && strstr(str, "whitelist") &&
	      strstr(str, "<field var='pubsub#max_items'><value>max</value></field>"));
	reset_semantics();
	ok = IPC_BOOL("mds-publish", account,
	                       "sem@muc.example.org", "SG10");
	str = sent_containing("urn:xmpp:mds:displayed:0");
	CHECK(ok && str && strstr(str, "<item id='sem@muc.example.org'>") &&
	      strstr(str, "id='SG10' by='sem@muc.example.org'"));
	ok = TRUE;
	ok = IPC_BOOL("mds-publish", account,
	                       "friend@example.net", NULL);
	CHECK(!ok);

	/* without message-meta: nothing fetched or emitted */
	set_meta_ui(FALSE);
	reset_semantics();
	jabber_displayed_fetch(js);
	CHECK(sent->len == 0);
	set_meta_ui(TRUE);
}

static void
test_features_and_flags(void)
{
	const char *expected[] = {
		"urn:xmpp:receipts", "urn:xmpp:chat-markers:0", "urn:xmpp:message-correct:0",
		"urn:xmpp:reactions:0", "urn:xmpp:message-retract:1",
		"urn:xmpp:message-retract:0", "urn:xmpp:reply:0", "urn:xmpp:styling:0",
		"urn:xmpp:mds:displayed:0+notify", NULL
	};
	const char **e;
	PurpleAccount *bad;
	PurpleConnection *gc;

	for (e = expected; *e; e++) {
		GList *l;
		gboolean found = FALSE;

		for (l = jabber_features; l; l = l->next) {
			JabberFeature *feat = l->data;
			if (purple_strequal(feat->namespace, *e))
				found = TRUE;
		}
		if (!found)
			fprintf(stderr, "feature %s not advertised\n", *e);
		CHECK(found);
	}

	/* jabber_login sets the flags before it validates anything; an
	 * invalid JID stops it before any connection attempt. */
	bad = purple_account_new("bad@@example.org", "prpl-jabber");
	gc = g_new0(PurpleConnection, 1);
	gc->prpl = jabber_plugin;
	gc->account = bad;
	purple_account_set_connection(bad, gc);
	jabber_login(bad);
	CHECK(gc->flags & PURPLE_CONNECTION_HTML);
	CHECK(gc->flags & PURPLE_CONNECTION_NO_FONTSIZE);
	CHECK(gc->flags & PURPLE_CONNECTION_NO_BGCOLOR);
	CHECK(gc->flags & PURPLE_CONNECTION_NO_URLDESC);
	CHECK(gc->flags & PURPLE_CONNECTION_NO_IMAGES);
	if (gc->disconnect_timeout)
		purple_timeout_remove(gc->disconnect_timeout);
	purple_account_set_connection(bad, NULL);
}

/**************************************************************************
 * HTTP upload IPC (XEP-0363)
 **************************************************************************/

static int features_updates = 0;
static PurpleConversation *features_conv = NULL;

static void
conv_updated_cb(PurpleConversation *conv, PurpleConvUpdateType type, gpointer data)
{
	/* the account's other conversations (earlier tests) get it too */
	if (type == PURPLE_CONV_UPDATE_FEATURES && conv == features_conv)
		features_updates++;
}

static void
test_http_upload_ipc(void)
{
	PurpleConversation *conv;
	gboolean ok = FALSE;
	char *id;
	char *reply;
	PurpleConnectionState state;

	/* the signal hookup is deferred to the event loop */
	while (g_main_context_iteration(NULL, FALSE))
		;
	purple_signal_connect(purple_conversations_get_handle(), "conversation-updated",
	                      &features_updates, PURPLE_CALLBACK(conv_updated_cb), NULL);
	conv = purple_conversation_new(PURPLE_CONV_TYPE_IM, account, "upload-friend@example.net");
	features_conv = conv;

	/* no service yet */
	CHECK(!GPOINTER_TO_INT(purple_plugin_ipc_call(jabber_plugin, "http-upload-available",
	                                               &ok, account)));
	CHECK(ok);
	ok = FALSE;
	CHECK(GPOINTER_TO_SIZE(purple_plugin_ipc_call(jabber_plugin, "http-upload-max-size",
	                                              &ok, account)) == 0);
	CHECK(ok);
	CHECK(!GPOINTER_TO_INT(purple_plugin_ipc_call(jabber_plugin, "http-upload-available",
	                                               NULL, NULL)));

	/* sign-on discovery: disco#info on the server finds the service */
	reset_capture();
	features_updates = 0;
	purple_signal_emit(purple_connections_get_handle(), "signed-on", js->gc);
	id = sent_id_containing("http://jabber.org/protocol/disco#info");
	CHECK(id != NULL);
	CHECK(features_updates == 0);
	reply = g_strdup_printf(
		"<iq xmlns='jabber:client' type='result' from='example.org' id='%s'>"
		"<query xmlns='http://jabber.org/protocol/disco#info'>"
		"<identity category='store' type='file' name='HTTP File Upload'/>"
		"<feature var='urn:xmpp:http:upload:0'/>"
		"<x xmlns='jabber:x:data' type='result'>"
		"<field var='FORM_TYPE' type='hidden'><value>urn:xmpp:http:upload:0</value></field>"
		"<field var='max-file-size'><value>5242880</value></field>"
		"</x></query></iq>", id ? id : "");
	feed(reply);
	g_free(reply);
	g_free(id);
	CHECK(features_updates == 1);
	CHECK(GPOINTER_TO_INT(purple_plugin_ipc_call(jabber_plugin, "http-upload-available",
	                                              NULL, account)));
	CHECK(GPOINTER_TO_SIZE(purple_plugin_ipc_call(jabber_plugin, "http-upload-max-size",
	                                              NULL, account)) == 5242880);

	/* the account switch turns it off */
	purple_account_set_bool(account, "http_upload", FALSE);
	CHECK(!GPOINTER_TO_INT(purple_plugin_ipc_call(jabber_plugin, "http-upload-available",
	                                               NULL, account)));
	CHECK(GPOINTER_TO_SIZE(purple_plugin_ipc_call(jabber_plugin, "http-upload-max-size",
	                                              NULL, account)) == 0);
	purple_account_set_bool(account, "http_upload", TRUE);

	/* not connected: unavailable */
	state = js->gc->state;
	js->gc->state = PURPLE_CONNECTING;
	CHECK(!GPOINTER_TO_INT(purple_plugin_ipc_call(jabber_plugin, "http-upload-available",
	                                               NULL, account)));
	js->gc->state = state;

	/* no stated limit: 0 */
	jabber_http_upload_set_service(js->gc, "upload.example.org", 0);
	CHECK(features_updates == 2);
	CHECK(GPOINTER_TO_INT(purple_plugin_ipc_call(jabber_plugin, "http-upload-available",
	                                              NULL, account)));
	CHECK(GPOINTER_TO_SIZE(purple_plugin_ipc_call(jabber_plugin, "http-upload-max-size",
	                                              NULL, account)) == 0);

	purple_signals_disconnect_by_handle(&features_updates);
	features_conv = NULL;
	purple_conversation_destroy(conv);
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
	events = g_ptr_array_new_with_free_func(g_free);

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
	purple_signal_connect(purple_conversations_get_handle(), "message-receipt",
	                      &events, PURPLE_CALLBACK(receipt_cb), NULL);
	purple_signal_connect(purple_conversations_get_handle(), "message-corrected",
	                      &events, PURPLE_CALLBACK(corrected_cb), NULL);
	purple_signal_connect(purple_conversations_get_handle(), "message-reaction",
	                      &events, PURPLE_CALLBACK(reaction_cb), NULL);
	purple_signal_connect(purple_conversations_get_handle(), "message-retracted",
	                      &events, PURPLE_CALLBACK(retracted_cb), NULL);

	test_ids();
	test_fallback();
	test_carbons_unwrap();
	test_mam_pure();
	test_selfping_pure();
	test_bookmarks_pure();
	test_styling_pure();
	test_reactions_pure();

	setup_stream();
	CHECK(jabber_ui_supports_message_meta());
	test_live_ids();
	test_carbons();
	test_mam_catchup();
	test_fallback_integration();
	test_send();
	test_muc();
	test_bookmarks_integration();

	/* M8 message semantics */
	test_receipts();
	test_markers_muc();
	test_corrections();
	test_reactions();
	test_replies();
	test_retractions();
	test_styling_meta();
	test_displayed_sync();
	test_features_and_flags();
	test_http_upload_ipc();

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
