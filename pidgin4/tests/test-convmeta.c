/*
 * pidgin4: PidginConvMeta tests (the M8 metadata glue's decisions).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Headless: the discard (dedup) decision against a scratch message index,
 * the fuzzy match for lines without ids, the contract rule 7 fallback
 * texts and the small helpers. The signal flow itself is covered by the
 * PIDGIN4_CONV_SELFTEST run of the application (pidgin4/TESTING.md).
 */
#include "pidgin-internal.h"

#include "pidginconvmeta.h"
#include "pidginmessageindex.h"

#include "test-support.h"

#define ACCT "jabber/me@example.com"
#define CONV "friend@example.com"
#define ROOM "room@conference.example.com"
#define T0 G_GINT64_CONSTANT(1700000000)

typedef struct {
	PidginMessageIndex *idx;
	char *path;
} Fixture;

static void
fixture_setup(Fixture *f, gconstpointer data)
{
	const char *profile = pidgin_test_profile_setup();
	GError *error = NULL;

	f->path = g_build_filename(profile, "messages.db", NULL);
	f->idx = pidgin_message_index_open(f->path, &error);
	g_assert_no_error(error);
	g_assert_nonnull(f->idx);
}

static void
fixture_teardown(Fixture *f, gconstpointer data)
{
	g_clear_object(&f->idx);
	g_free(f->path);
	pidgin_test_profile_cleanup();
}

static gint64
add(PidginMessageIndex *idx, const char *conv, gint64 time, const char *sender,
    const char *body, const char *stanza_id, const char *origin_id, const char *server_id)
{
	PidginIndexedMessage *msg = pidgin_indexed_message_new();
	GError *error = NULL;
	gint64 id;

	msg->account = g_strdup(ACCT);
	msg->conv = g_strdup(conv);
	msg->time = time;
	msg->sender = g_strdup(sender);
	msg->body = g_strdup(body);
	msg->stanza_id = g_strdup(stanza_id);
	msg->origin_id = g_strdup(origin_id);
	msg->server_id = g_strdup(server_id);
	msg->flags = 2;
	id = pidgin_message_index_insert(idx, msg, &error);
	g_assert_no_error(error);
	g_assert_cmpint(id, >, 0);
	pidgin_indexed_message_free(msg);
	return id;
}

static GHashTable *
meta(const char *first, ...)
{
	GHashTable *m = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	const char *key = first;
	va_list args;

	va_start(args, first);
	while (key != NULL) {
		g_hash_table_insert(m, g_strdup(key), g_strdup(va_arg(args, const char *)));
		key = va_arg(args, const char *);
	}
	va_end(args);
	return m;
}

static gboolean
discard(Fixture *f, const char *conv, GHashTable *m, gboolean ids_unique, gint64 *hit_id)
{
	PidginIndexedMessage *hit = NULL;
	gboolean ret = pidgin_conv_meta_check_discard(f->idx, ACCT, conv, m, ids_unique, &hit);

	if (hit_id)
		*hit_id = hit ? hit->id : 0;
	g_assert_true(ret == (hit != NULL));
	pidgin_indexed_message_free(hit);
	g_hash_table_unref(m);
	return ret;
}

static void
test_discard_server_id(Fixture *f, gconstpointer data)
{
	gint64 id = add(f->idx, CONV, T0, "Friend", "hello", "c1", "o1", "srv-1"), hit = 0;

	/* The archive's id always counts, live or archived */
	g_assert_true(discard(f, CONV, meta("server-id", "srv-1", NULL), FALSE, &hit));
	g_assert_cmpint(hit, ==, id);
	g_assert_true(discard(f, CONV, meta("server-id", "srv-1", "mam", "1", NULL), FALSE, NULL));
	/* Another conversation or another id: kept */
	g_assert_false(discard(f, ROOM, meta("server-id", "srv-1", NULL), FALSE, NULL));
	g_assert_false(discard(f, CONV, meta("server-id", "srv-2", NULL), FALSE, NULL));
	/* No ids at all: kept */
	g_assert_false(discard(f, CONV, meta("conv-type", "im", NULL), FALSE, NULL));
	g_assert_false(pidgin_conv_meta_check_discard(f->idx, ACCT, CONV, NULL, TRUE, NULL));
}

static void
test_discard_client_ids(Fixture *f, gconstpointer data)
{
	add(f->idx, CONV, T0, "Friend", "hello", "c1", "o1", NULL);

	/* XMPP message ids can repeat: a live message isn't dropped on them */
	g_assert_false(discard(f, CONV, meta("stanza-id", "c1", NULL), FALSE, NULL));
	g_assert_false(discard(f, CONV, meta("origin-id", "o1", NULL), FALSE, NULL));
	/* ... but an archive copy is */
	g_assert_true(discard(f, CONV, meta("stanza-id", "c1", "mam", "1", NULL), FALSE, NULL));
	g_assert_true(discard(f, CONV, meta("origin-id", "o1", "mam", "1", NULL), FALSE, NULL));
	/* Server-assigned ids (IRC msgid): always */
	g_assert_true(discard(f, CONV, meta("stanza-id", "c1", NULL), TRUE, NULL));
	/* Empty ids never match */
	g_assert_false(discard(f, CONV, meta("stanza-id", "", "mam", "1", NULL), TRUE, NULL));
}

static void
test_fuzzy(Fixture *f, gconstpointer data)
{
	PidginIndexedMessage *hit = NULL;
	gint64 id;

	/* A line Pidgin 2 logged (no ids) */
	id = add(f->idx, ROOM, T0, "alice", "see  you\ntomorrow", NULL, NULL, NULL);

	g_assert_true(pidgin_conv_meta_fuzzy_duplicate(f->idx, ACCT, ROOM, T0 + 90, "alice",
	                                               "see you tomorrow", &hit));
	g_assert_nonnull(hit);
	g_assert_cmpint(hit->id, ==, id);
	pidgin_indexed_message_free(hit);
	/* Any sender (IMs pass NULL) */
	g_assert_true(pidgin_conv_meta_fuzzy_duplicate(f->idx, ACCT, ROOM, T0 - 100, NULL,
	                                               "see you tomorrow", NULL));
	/* Outside +-2 min, another sender, other text: not a duplicate */
	g_assert_false(pidgin_conv_meta_fuzzy_duplicate(f->idx, ACCT, ROOM, T0 + 200, "alice",
	                                                "see you tomorrow", NULL));
	g_assert_false(pidgin_conv_meta_fuzzy_duplicate(f->idx, ACCT, ROOM, T0, "bob",
	                                                "see you tomorrow", NULL));
	g_assert_false(pidgin_conv_meta_fuzzy_duplicate(f->idx, ACCT, ROOM, T0, "alice",
	                                                "see you later", &hit));
	g_assert_null(hit);
	g_assert_false(pidgin_conv_meta_fuzzy_duplicate(f->idx, ACCT, CONV, T0, "alice",
	                                                "see you tomorrow", NULL));
	g_assert_false(pidgin_conv_meta_fuzzy_duplicate(NULL, ACCT, ROOM, T0, NULL, "x", NULL));
}

static void
test_texts(void)
{
	char *s;

	s = pidgin_conv_meta_text_edited("Alice", "new text");
	g_assert_cmpstr(s, ==, "Alice edited: new text");
	g_free(s);

	s = pidgin_conv_meta_text_reaction("Bob", "\xf0\x9f\x91\x8d", TRUE,
		"a rather long message that goes on for more than forty characters");
	g_assert_cmpstr(s, ==, "Bob reacted \xf0\x9f\x91\x8d to: "
		"a rather long message that goes on for m\xe2\x80\xa6");
	g_free(s);
	s = pidgin_conv_meta_text_reaction("Bob", "x", FALSE, "short");
	g_assert_cmpstr(s, ==, "Bob removed the reaction x from: short");
	g_free(s);

	s = pidgin_conv_meta_text_retracted("Carol", FALSE, NULL);
	g_assert_cmpstr(s, ==, "Carol retracted a message");
	g_free(s);
	s = pidgin_conv_meta_text_retracted("mod", TRUE, "spam");
	g_assert_cmpstr(s, ==, "mod removed a message: spam");
	g_free(s);
	s = pidgin_conv_meta_text_retracted("mod", TRUE, "");
	g_assert_cmpstr(s, ==, "mod removed a message");
	g_free(s);

	s = pidgin_conv_meta_snippet("line one\nline two", 40);
	g_assert_cmpstr(s, ==, "line one line two");
	g_free(s);
	s = pidgin_conv_meta_snippet("\xc3\xa9\xc3\xa9\xc3\xa9", 2);   /* counts characters */
	g_assert_cmpstr(s, ==, "\xc3\xa9\xc3\xa9\xe2\x80\xa6");
	g_free(s);
	s = pidgin_conv_meta_snippet(NULL, 5);
	g_assert_cmpstr(s, ==, "");
	g_free(s);
}

static void
test_helpers(void)
{
	char *s;
	PidginIndexedMessage *row;
	PidginMessage *msg;

	s = pidgin_conv_meta_bare_jid("a@b.c/phone");
	g_assert_cmpstr(s, ==, "a@b.c");
	g_free(s);
	s = pidgin_conv_meta_bare_jid("a@b.c");
	g_assert_cmpstr(s, ==, "a@b.c");
	g_free(s);
	g_assert_null(pidgin_conv_meta_bare_jid(NULL));

	g_assert_true(pidgin_conv_meta_is_image_url("https://upload.example.com/x/cat.JPG"));
	g_assert_true(pidgin_conv_meta_is_image_url(
		"aesgcm://upload.example.com/y/pic.png#0011223344556677"));
	g_assert_false(pidgin_conv_meta_is_image_url("https://example.com/doc.pdf"));
	g_assert_false(pidgin_conv_meta_is_image_url("https://example.com/a.png and text"));
	g_assert_false(pidgin_conv_meta_is_image_url("ftp://example.com/a.png"));
	g_assert_false(pidgin_conv_meta_is_image_url("not a url"));

	/* Scroll-back rows become plain messages with their ids */
	row = pidgin_indexed_message_new();
	row->id = 42;
	row->time = T0;
	row->sender = g_strdup("Friend");
	row->body = g_strdup("1 < 2 & so on");
	row->server_id = g_strdup("srv-9");
	row->flags = 0;
	msg = pidgin_conv_meta_message_from_index(row);
	g_assert_cmpstr(pidgin_message_get_plain_text(msg), ==, "1 < 2 & so on");
	g_assert_cmpstr(pidgin_message_get_server_id(msg), ==, "srv-9");
	g_assert_cmpint(pidgin_message_get_index_id(msg), ==, 42);
	g_assert_cmpint(pidgin_message_get_time(msg), ==, T0);
	g_assert_true(pidgin_message_get_flags(msg) & PURPLE_MESSAGE_RECV);
	g_assert_true(pidgin_message_get_flags(msg) & PURPLE_MESSAGE_DELAYED);
	g_object_unref(msg);
	pidgin_indexed_message_free(row);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add("/convmeta/discard/server-id", Fixture, NULL, fixture_setup,
	           test_discard_server_id, fixture_teardown);
	g_test_add("/convmeta/discard/client-ids", Fixture, NULL, fixture_setup,
	           test_discard_client_ids, fixture_teardown);
	g_test_add("/convmeta/fuzzy", Fixture, NULL, fixture_setup, test_fuzzy, fixture_teardown);
	g_test_add_func("/convmeta/texts", test_texts);
	g_test_add_func("/convmeta/helpers", test_helpers);

	return g_test_run();
}
