/*
 * pidgin4: PidginMessageIndex tests.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "pidgin-internal.h"

#include <gio/gio.h>
#include <sqlite3.h>

#include "account.h"
#include "signals.h"
#include "util.h"
#include "value.h"

#include "pidginmessageindex.h"

#include "test-support.h"

#define ACCT "jabber/me@example.com"
#define CONV "friend@example.com"
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
		const char *body, const char *stanza_id, const char *origin_id,
		const char *server_id)
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

static void
test_insert_get(Fixture *f, gconstpointer data)
{
	PidginIndexedMessage *msg, *copy;
	gint64 id;

	id = add(f->idx, CONV, T0, "friend", "  Hello \n  world  ", "s1", NULL, NULL);
	msg = pidgin_message_index_get(f->idx, id);
	g_assert_nonnull(msg);
	g_assert_cmpint(msg->id, ==, id);
	g_assert_cmpstr(msg->account, ==, ACCT);
	g_assert_cmpstr(msg->conv, ==, CONV);
	g_assert_cmpint(msg->time, ==, T0);
	g_assert_cmpstr(msg->sender, ==, "friend");
	g_assert_cmpstr(msg->body, ==, "Hello world");
	g_assert_cmpstr(msg->stanza_id, ==, "s1");
	g_assert_null(msg->origin_id);
	g_assert_null(msg->log_file);
	g_assert_cmpint(msg->log_offset, ==, -1);
	g_assert_cmpuint(msg->flags, ==, 2);
	g_assert_false(msg->is_chat);

	copy = g_boxed_copy(PIDGIN_TYPE_INDEXED_MESSAGE, msg);
	g_assert_cmpstr(copy->body, ==, msg->body);
	g_boxed_free(PIDGIN_TYPE_INDEXED_MESSAGE, copy);
	pidgin_indexed_message_free(msg);

	g_assert_null(pidgin_message_index_get(f->idx, id + 100));
	g_assert_cmpint(pidgin_message_index_count(f->idx), ==, 1);

	g_assert_true(pidgin_message_index_set_ids(f->idx, id, NULL, "o1", "srv1"));
	msg = pidgin_message_index_get(f->idx, id);
	g_assert_cmpstr(msg->stanza_id, ==, "s1");
	g_assert_cmpstr(msg->origin_id, ==, "o1");
	g_assert_cmpstr(msg->server_id, ==, "srv1");
	pidgin_indexed_message_free(msg);
	g_assert_false(pidgin_message_index_set_ids(f->idx, id + 100, "x", NULL, NULL));
}

static void
test_find_by_id(Fixture *f, gconstpointer data)
{
	PidginIndexedMessage *msg;
	gint64 a, b, c, d;

	a = add(f->idx, CONV, T0, "friend", "one", "stanza-a", NULL, NULL);
	b = add(f->idx, CONV, T0 + 10, "me", "two", NULL, "origin-b", NULL);
	c = add(f->idx, CONV, T0 + 20, "friend", "three", NULL, NULL, "server-c");
	/* Same id in another conversation must not match. */
	add(f->idx, "other@example.com", T0 + 30, "x", "four", "stanza-a", NULL, NULL);

	msg = pidgin_message_index_find_by_id(f->idx, ACCT, CONV, "stanza-a");
	g_assert_nonnull(msg);
	g_assert_cmpint(msg->id, ==, a);
	pidgin_indexed_message_free(msg);

	msg = pidgin_message_index_find_by_id(f->idx, ACCT, CONV, "origin-b");
	g_assert_cmpint(msg->id, ==, b);
	pidgin_indexed_message_free(msg);

	msg = pidgin_message_index_find_by_id(f->idx, ACCT, CONV, "server-c");
	g_assert_cmpint(msg->id, ==, c);
	pidgin_indexed_message_free(msg);

	g_assert_null(pidgin_message_index_find_by_id(f->idx, ACCT, CONV, "nope"));
	g_assert_null(pidgin_message_index_find_by_id(f->idx, "irc/x", CONV, "stanza-a"));
	g_assert_null(pidgin_message_index_find_by_id(f->idx, ACCT, CONV, ""));

	/* Newest wins. */
	d = add(f->idx, CONV, T0 + 40, "friend", "again", "stanza-a", NULL, NULL);
	msg = pidgin_message_index_find_by_id(f->idx, ACCT, CONV, "stanza-a");
	g_assert_cmpint(msg->id, ==, d);
	pidgin_indexed_message_free(msg);
}

static void
test_find_fuzzy(Fixture *f, gconstpointer data)
{
	PidginIndexedMessage *msg;
	gint64 a, b;

	a = add(f->idx, CONV, T0, "friend", "the quick  brown\tfox", NULL, NULL, NULL);
	b = add(f->idx, CONV, T0 + 100, "friend", "the quick brown fox", NULL, NULL, NULL);

	/* Whitespace differences, inside the window, closest wins. */
	msg = pidgin_message_index_find_fuzzy(f->idx, ACCT, CONV, T0 + 30, "friend",
			"  the quick brown\n fox ");
	g_assert_nonnull(msg);
	g_assert_cmpint(msg->id, ==, a);
	pidgin_indexed_message_free(msg);

	msg = pidgin_message_index_find_fuzzy(f->idx, ACCT, CONV, T0 + 90, NULL,
			"the quick brown fox");
	g_assert_cmpint(msg->id, ==, b);
	pidgin_indexed_message_free(msg);

	/* Edge of the window. */
	msg = pidgin_message_index_find_fuzzy(f->idx, ACCT, CONV, T0 - 120, NULL,
			"the quick brown fox");
	g_assert_nonnull(msg);
	g_assert_cmpint(msg->id, ==, a);
	pidgin_indexed_message_free(msg);

	/* Outside the window. */
	g_assert_null(pidgin_message_index_find_fuzzy(f->idx, ACCT, CONV, T0 - 121,
			NULL, "the quick brown fox"));
	g_assert_null(pidgin_message_index_find_fuzzy(f->idx, ACCT, CONV, T0 + 221,
			NULL, "the quick brown fox"));
	/* Wrong sender, text, conversation. */
	g_assert_null(pidgin_message_index_find_fuzzy(f->idx, ACCT, CONV, T0,
			"someone", "the quick brown fox"));
	g_assert_null(pidgin_message_index_find_fuzzy(f->idx, ACCT, CONV, T0,
			"friend", "the quick brown dog"));
	g_assert_null(pidgin_message_index_find_fuzzy(f->idx, ACCT, "x", T0,
			"friend", "the quick brown fox"));
}

static void
test_search(Fixture *f, gconstpointer data)
{
	GPtrArray *res;
	PidginIndexedMessage *msg;
	const char *hostile[] = {
		"\"foo", "AND OR (", "NEAR(", "foo\"bar", "*", "\"", "-", ":", "^x",
		"body:hello", "\"\"\"", "a OR", "(((", "hello)", "NOT", "'", "\\", NULL
	};
	int i;

	add(f->idx, CONV, T0, "friend", "Hello world", NULL, NULL, NULL);
	add(f->idx, CONV, T0 + 10, "me", "help me please", NULL, NULL, NULL);
	add(f->idx, "other@example.com", T0 + 20, "x", "hello again, café", NULL, NULL, NULL);
	add(f->idx, CONV, T0 + 30, "friend", "nothing here", NULL, NULL, NULL);

	res = pidgin_message_index_search(f->idx, "hello", NULL, NULL, 0);
	g_assert_cmpuint(res->len, ==, 2);
	/* Newest first. */
	msg = res->pdata[0];
	g_assert_cmpstr(msg->conv, ==, "other@example.com");
	g_assert_cmpstr(msg->body, ==, "hello again, café");
	g_ptr_array_unref(res);

	res = pidgin_message_index_search(f->idx, "hel*", NULL, NULL, 0);
	g_assert_cmpuint(res->len, ==, 3);
	g_ptr_array_unref(res);

	res = pidgin_message_index_search(f->idx, "hel*", ACCT, CONV, 0);
	g_assert_cmpuint(res->len, ==, 2);
	g_ptr_array_unref(res);

	res = pidgin_message_index_search(f->idx, "hel*", "irc/nobody", NULL, 0);
	g_assert_cmpuint(res->len, ==, 0);
	g_ptr_array_unref(res);

	res = pidgin_message_index_search(f->idx, "hel*", NULL, NULL, 1);
	g_assert_cmpuint(res->len, ==, 1);
	g_ptr_array_unref(res);

	/* All terms must match; diacritics are folded. */
	res = pidgin_message_index_search(f->idx, "hello world", NULL, NULL, 0);
	g_assert_cmpuint(res->len, ==, 1);
	g_ptr_array_unref(res);
	res = pidgin_message_index_search(f->idx, "cafe", NULL, NULL, 0);
	g_assert_cmpuint(res->len, ==, 1);
	g_ptr_array_unref(res);
	res = pidgin_message_index_search(f->idx, "HELLO", NULL, NULL, 0);
	g_assert_cmpuint(res->len, ==, 2);
	g_ptr_array_unref(res);

	/* Hostile input is never an FTS syntax error. */
	for (i = 0; hostile[i] != NULL; i++) {
		res = pidgin_message_index_search(f->idx, hostile[i], NULL, NULL, 0);
		g_assert_nonnull(res);
		g_ptr_array_unref(res);
	}
	res = pidgin_message_index_search(f->idx, "\"hello", NULL, NULL, 0);
	g_assert_cmpuint(res->len, ==, 2);
	g_ptr_array_unref(res);
	res = pidgin_message_index_search(f->idx, "AND OR (", NULL, NULL, 0);
	g_assert_cmpuint(res->len, ==, 0);
	g_ptr_array_unref(res);
	res = pidgin_message_index_search(f->idx, "", NULL, NULL, 0);
	g_assert_cmpuint(res->len, ==, 0);
	g_ptr_array_unref(res);
}

static void
test_recent(Fixture *f, gconstpointer data)
{
	GPtrArray *res;
	int i;

	for (i = 0; i < 10; i++) {
		char *body = g_strdup_printf("message %d", i);
		add(f->idx, CONV, T0 + i * 60, "friend", body, NULL, NULL, NULL);
		g_free(body);
	}
	add(f->idx, "other@example.com", T0, "x", "elsewhere", NULL, NULL, NULL);

	res = pidgin_message_index_get_recent(f->idx, ACCT, CONV, 0, 3);
	g_assert_cmpuint(res->len, ==, 3);
	g_assert_cmpstr(((PidginIndexedMessage *)res->pdata[0])->body, ==, "message 9");
	g_assert_cmpstr(((PidginIndexedMessage *)res->pdata[2])->body, ==, "message 7");
	g_ptr_array_unref(res);

	res = pidgin_message_index_get_recent(f->idx, ACCT, CONV, T0 + 7 * 60, 0);
	g_assert_cmpuint(res->len, ==, 7);
	g_assert_cmpstr(((PidginIndexedMessage *)res->pdata[0])->body, ==, "message 6");
	g_ptr_array_unref(res);
}

static void
test_reactions_receipts(Fixture *f, gconstpointer data)
{
	GHashTable *table;
	GPtrArray *senders;
	char *state;
	gint64 id;

	id = add(f->idx, CONV, T0, "friend", "react to me", NULL, NULL, NULL);

	table = pidgin_message_index_get_reactions(f->idx, id);
	g_assert_cmpuint(g_hash_table_size(table), ==, 0);
	g_hash_table_unref(table);

	g_assert_true(pidgin_message_index_add_reaction(f->idx, id, "👍", "alice"));
	g_assert_true(pidgin_message_index_add_reaction(f->idx, id, "👍", "bob"));
	g_assert_true(pidgin_message_index_add_reaction(f->idx, id, "👍", "alice"));
	g_assert_true(pidgin_message_index_add_reaction(f->idx, id, "🎉", "carol"));

	table = pidgin_message_index_get_reactions(f->idx, id);
	g_assert_cmpuint(g_hash_table_size(table), ==, 2);
	senders = g_hash_table_lookup(table, "👍");
	g_assert_cmpuint(senders->len, ==, 2);
	g_assert_cmpstr(senders->pdata[0], ==, "alice");
	g_assert_cmpstr(senders->pdata[1], ==, "bob");
	g_hash_table_unref(table);

	g_assert_true(pidgin_message_index_remove_reaction(f->idx, id, "👍", "alice"));
	table = pidgin_message_index_get_reactions(f->idx, id);
	senders = g_hash_table_lookup(table, "👍");
	g_assert_cmpuint(senders->len, ==, 1);
	g_assert_cmpstr(senders->pdata[0], ==, "bob");
	g_hash_table_unref(table);

	g_assert_null(pidgin_message_index_get_receipt(f->idx, id));
	g_assert_true(pidgin_message_index_set_receipt(f->idx, id, "delivered", NULL));
	g_assert_true(pidgin_message_index_set_receipt(f->idx, id, "delivered", NULL));
	state = pidgin_message_index_get_receipt(f->idx, id);
	g_assert_cmpstr(state, ==, "delivered");
	g_free(state);
	g_assert_true(pidgin_message_index_set_receipt(f->idx, id, "displayed", "friend"));
	g_assert_true(pidgin_message_index_set_receipt(f->idx, id, "delivered", "friend"));
	state = pidgin_message_index_get_receipt(f->idx, id);
	g_assert_cmpstr(state, ==, "displayed");
	g_free(state);
}

static int
count_rows(const char *path, const char *sql)
{
	sqlite3 *db;
	sqlite3_stmt *stmt;
	int n = -1;

	g_assert_cmpint(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL), ==, SQLITE_OK);
	g_assert_cmpint(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL), ==, SQLITE_OK);
	if (sqlite3_step(stmt) == SQLITE_ROW)
		n = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	sqlite3_close(db);

	return n;
}

static void
test_delete_cascade(Fixture *f, gconstpointer data)
{
	GPtrArray *res;
	gint64 id;

	id = add(f->idx, CONV, T0, "friend", "doomed message", NULL, NULL, NULL);
	pidgin_message_index_add_reaction(f->idx, id, "👍", "alice");
	pidgin_message_index_set_receipt(f->idx, id, "delivered", NULL);

	g_assert_true(pidgin_message_index_delete(f->idx, id));
	g_assert_false(pidgin_message_index_delete(f->idx, id));
	g_assert_null(pidgin_message_index_get(f->idx, id));

	res = pidgin_message_index_search(f->idx, "doomed", NULL, NULL, 0);
	g_assert_cmpuint(res->len, ==, 0);
	g_ptr_array_unref(res);

	g_assert_cmpint(count_rows(f->path, "SELECT count(*) FROM reactions"), ==, 0);
	g_assert_cmpint(count_rows(f->path, "SELECT count(*) FROM receipts"), ==, 0);
	g_assert_cmpint(count_rows(f->path, "SELECT count(*) FROM messages_fts"), ==, 0);
}

static void
test_kv(Fixture *f, gconstpointer data)
{
	char *value;

	g_assert_null(pidgin_message_index_kv_get(f->idx, ACCT, "mam/last-id"));
	g_assert_true(pidgin_message_index_kv_set(f->idx, ACCT, "mam/last-id", "abc"));
	g_assert_true(pidgin_message_index_kv_set(f->idx, "irc/x", "mam/last-id", "other"));
	value = pidgin_message_index_kv_get(f->idx, ACCT, "mam/last-id");
	g_assert_cmpstr(value, ==, "abc");
	g_free(value);
	g_assert_true(pidgin_message_index_kv_set(f->idx, ACCT, "mam/last-id", "def"));
	value = pidgin_message_index_kv_get(f->idx, ACCT, "mam/last-id");
	g_assert_cmpstr(value, ==, "def");
	g_free(value);
	g_assert_true(pidgin_message_index_kv_set(f->idx, ACCT, "mam/last-id", NULL));
	g_assert_null(pidgin_message_index_kv_get(f->idx, ACCT, "mam/last-id"));
	value = pidgin_message_index_kv_get(f->idx, "irc/x", "mam/last-id");
	g_assert_cmpstr(value, ==, "other");
	g_free(value);
}

static void
test_log_position(Fixture *f, gconstpointer data)
{
	PidginIndexedMessage *msg;
	gint64 id, mtime = 0, size = 0, offset = 0;

	id = add(f->idx, CONV, T0, "friend", "logged", "sid", NULL, NULL);
	g_assert_true(pidgin_message_index_mark_log_position(f->idx, id,
			"jabber/me@example.com/friend@example.com/a.html", 1234));
	msg = pidgin_message_index_get(f->idx, id);
	g_assert_cmpstr(msg->log_file, ==, "jabber/me@example.com/friend@example.com/a.html");
	g_assert_cmpint(msg->log_offset, ==, 1234);
	g_assert_true(pidgin_message_index_has_log_position(f->idx, msg));
	msg->time += 60;
	g_assert_true(pidgin_message_index_has_log_position(f->idx, msg));
	msg->log_offset = 1235;
	g_assert_false(pidgin_message_index_has_log_position(f->idx, msg));
	pidgin_indexed_message_free(msg);

	/* The column keeps only the file name when the path is the row's own
	 * log directory; other paths round-trip whole. */
	g_assert_cmpint(count_rows(f->path, "SELECT count(*) FROM messages"
			" WHERE log_file = 'a.html'"), ==, 1);
	g_assert_true(pidgin_message_index_mark_log_position(f->idx, id,
			"irc/elsewhere/#x.chat/b.txt", 5));
	msg = pidgin_message_index_get(f->idx, id);
	g_assert_cmpstr(msg->log_file, ==, "irc/elsewhere/#x.chat/b.txt");
	pidgin_indexed_message_free(msg);
	g_assert_true(pidgin_message_index_mark_log_position(f->idx, id,
			"jabber/me@example.com/friend@example.com/a.html", 1234));

	g_assert_false(pidgin_message_index_get_file_state(f->idx, "a", NULL, NULL, NULL));
	g_assert_true(pidgin_message_index_set_file_state(f->idx, "a", 10, 20, 15));
	g_assert_true(pidgin_message_index_get_file_state(f->idx, "a", &mtime, &size, &offset));
	g_assert_cmpint(mtime, ==, 10);
	g_assert_cmpint(size, ==, 20);
	g_assert_cmpint(offset, ==, 15);

	/* forget_file drops plain rows but keeps (unlinks) rows with ids. */
	msg = pidgin_indexed_message_new();
	msg->account = g_strdup(ACCT);
	msg->conv = g_strdup(CONV);
	msg->time = T0;
	msg->body = g_strdup("backfilled");
	msg->log_file = g_strdup("jabber/me@example.com/friend@example.com/a.html");
	msg->log_offset = 99;
	g_assert_cmpint(pidgin_message_index_insert(f->idx, msg, NULL), >, 0);
	pidgin_indexed_message_free(msg);
	g_assert_true(pidgin_message_index_set_file_state(f->idx,
			"jabber/me@example.com/friend@example.com/a.html", 1, 2, 3));

	g_assert_true(pidgin_message_index_forget_file(f->idx,
			"jabber/me@example.com/friend@example.com/a.html"));
	g_assert_cmpint(pidgin_message_index_count(f->idx), ==, 1);
	msg = pidgin_message_index_get(f->idx, id);
	g_assert_null(msg->log_file);
	g_assert_cmpint(msg->log_offset, ==, -1);
	pidgin_indexed_message_free(msg);
	g_assert_false(pidgin_message_index_get_file_state(f->idx,
			"jabber/me@example.com/friend@example.com/a.html", NULL, NULL, NULL));
}

static void
test_batch(Fixture *f, gconstpointer data)
{
	PidginIndexedMessage *msg;
	gint64 id;

	pidgin_message_index_begin(f->idx);
	id = add(f->idx, CONV, T0, "friend", "in a batch", "b1", NULL, NULL);
	/* The batch owner reads its own uncommitted rows. */
	msg = pidgin_message_index_find_by_id(f->idx, ACCT, CONV, "b1");
	g_assert_nonnull(msg);
	g_assert_cmpint(msg->id, ==, id);
	pidgin_indexed_message_free(msg);
	g_assert_true(pidgin_message_index_commit(f->idx));

	msg = pidgin_message_index_get(f->idx, id);
	g_assert_nonnull(msg);
	pidgin_indexed_message_free(msg);
}

static void
test_reopen(Fixture *f, gconstpointer data)
{
	GError *error = NULL;
	PidginIndexedMessage *msg;
	char *value;
	gint64 id;

	id = add(f->idx, CONV, T0, "friend", "persistent", "p1", NULL, NULL);
	pidgin_message_index_kv_set(f->idx, ACCT, "k", "v");
	g_clear_object(&f->idx);

	g_assert_cmpint(count_rows(f->path, "PRAGMA user_version"), ==,
			PIDGIN_MESSAGE_INDEX_SCHEMA_VERSION);

	f->idx = pidgin_message_index_open(f->path, &error);
	g_assert_no_error(error);
	msg = pidgin_message_index_find_by_id(f->idx, ACCT, CONV, "p1");
	g_assert_nonnull(msg);
	g_assert_cmpint(msg->id, ==, id);
	g_assert_cmpstr(msg->body, ==, "persistent");
	pidgin_indexed_message_free(msg);
	value = pidgin_message_index_kv_get(f->idx, ACCT, "k");
	g_assert_cmpstr(value, ==, "v");
	g_free(value);
	g_assert_cmpstr(pidgin_message_index_get_path(f->idx), ==, f->path);
}

static void
test_newer_schema(void)
{
	const char *profile = pidgin_test_profile_setup();
	char *path = g_build_filename(profile, "future.db", NULL);
	GError *error = NULL;
	PidginMessageIndex *idx;
	sqlite3 *db;

	g_assert_cmpint(sqlite3_open(path, &db), ==, SQLITE_OK);
	g_assert_cmpint(sqlite3_exec(db, "PRAGMA user_version = 99", NULL, NULL, NULL), ==, SQLITE_OK);
	sqlite3_close(db);

	idx = pidgin_message_index_open(path, &error);
	g_assert_null(idx);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
	g_error_free(error);
	g_free(path);
	pidgin_test_profile_cleanup();
}

/* purple_account_new() needs the D-Bus pointer registry and the account
 * subsystem; the key functions only read the username and protocol id. */
static PurpleAccount *
fake_account(const char *username, const char *protocol_id)
{
	PurpleAccount *account = g_new0(PurpleAccount, 1);

	account->username = g_strdup(username);
	account->protocol_id = g_strdup(protocol_id);

	return account;
}

static void
fake_account_free(PurpleAccount *account)
{
	g_free(account->username);
	g_free(account->protocol_id);
	g_free(account);
}

static void
test_keys(void)
{
	PurpleAccount *account = fake_account("Me@Example.com/Home", "prpl-jabber");
	char *key;

	key = pidgin_message_index_account_key(account);
	/* No prpl is loaded here: the prpl name falls back to the id. */
	g_assert_true(g_str_has_prefix(key, "jabber/"));
	g_assert_cmpstr(key + 7, ==, purple_escape_filename(purple_normalize(account,
			"Me@Example.com/Home")));
	g_free(key);

	key = pidgin_message_index_conv_key(account, "friend@example.com");
	g_assert_cmpstr(key, ==, "friend@example.com");
	g_free(key);

	key = pidgin_message_index_normalize_text("  a \t b\n\nc  ");
	g_assert_cmpstr(key, ==, "a b c");
	g_free(key);

	fake_account_free(account);
}

/* The jabber prpl's kv signals, registered on a fake handle. */
static int fake_jabber;

static void
test_kv_signals(Fixture *f, gconstpointer data)
{
	PurpleAccount *account = fake_account("me@example.com", "prpl-jabber");
	const char *value;
	char *key, *stored;

	purple_signal_register(&fake_jabber, "jabber-kv-load",
			purple_marshal_POINTER__POINTER_POINTER,
			purple_value_new(PURPLE_TYPE_STRING), 2,
			purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
			purple_value_new(PURPLE_TYPE_STRING));
	purple_signal_register(&fake_jabber, "jabber-kv-store",
			purple_marshal_VOID__POINTER_POINTER_POINTER,
			NULL, 3,
			purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING));

	pidgin_message_index_connect_kv_signals(f->idx, &fake_jabber);

	value = purple_signal_emit_return_1(&fake_jabber, "jabber-kv-load",
			account, "fast/token");
	g_assert_null(value);

	purple_signal_emit(&fake_jabber, "jabber-kv-store", account, "fast/token", "t0k3n");
	value = purple_signal_emit_return_1(&fake_jabber, "jabber-kv-load",
			account, "fast/token");
	g_assert_cmpstr(value, ==, "t0k3n");

	key = pidgin_message_index_account_key(account);
	stored = pidgin_message_index_kv_get(f->idx, key, "fast/token");
	g_assert_cmpstr(stored, ==, "t0k3n");
	g_free(stored);
	g_free(key);

	purple_signal_emit(&fake_jabber, "jabber-kv-store", account, "fast/token", NULL);
	value = purple_signal_emit_return_1(&fake_jabber, "jabber-kv-load",
			account, "fast/token");
	g_assert_null(value);

	purple_signals_disconnect_by_handle(f->idx);
	purple_signals_unregister_by_instance(&fake_jabber);
	fake_account_free(account);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	purple_signals_init();

#define ADD(name, func) \
	g_test_add(name, Fixture, NULL, fixture_setup, func, fixture_teardown)

	ADD("/messageindex/insert-get", test_insert_get);
	ADD("/messageindex/find-by-id", test_find_by_id);
	ADD("/messageindex/find-fuzzy", test_find_fuzzy);
	ADD("/messageindex/search", test_search);
	ADD("/messageindex/recent", test_recent);
	ADD("/messageindex/reactions-receipts", test_reactions_receipts);
	ADD("/messageindex/delete-cascade", test_delete_cascade);
	ADD("/messageindex/kv", test_kv);
	ADD("/messageindex/log-position", test_log_position);
	ADD("/messageindex/batch", test_batch);
	ADD("/messageindex/reopen", test_reopen);
	ADD("/messageindex/kv-signals", test_kv_signals);
	g_test_add_func("/messageindex/newer-schema", test_newer_schema);
	g_test_add_func("/messageindex/keys", test_keys);

	return g_test_run();
}
