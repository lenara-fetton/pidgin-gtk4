/*
 * pidgin4: PidginBackfill tests, on the synthetic logs in tests/data/logs.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "pidgin-internal.h"

#include <gio/gio.h>
#include <sqlite3.h>
#include <utime.h>

#include "conversation.h"

#include "pidginbackfill.h"
#include "pidginmessageindex.h"

#include "test-support.h"

#define JABBER "jabber/me@example.com"
#define FRIEND_FILE JABBER "/friend@example.com/2024-01-02.230000-0800PST.html"
#define ROOM_FILE JABBER "/room@conference.example.com.chat/2024-01-05.100000-0800PST.html"
#define FIXTURE_ROWS 19

typedef struct {
	char *logs;
	PidginMessageIndex *idx;
	PidginBackfill *bf;
} Fixture;

static void
copy_tree(GFile *src, GFile *dst)
{
	GFileEnumerator *en;
	GFileInfo *info;
	GError *error = NULL;

	g_file_make_directory_with_parents(dst, NULL, NULL);
	en = g_file_enumerate_children(src, G_FILE_ATTRIBUTE_STANDARD_NAME ","
			G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
			NULL, &error);
	g_assert_no_error(error);

	while ((info = g_file_enumerator_next_file(en, NULL, &error)) != NULL) {
		GFile *s = g_file_get_child(src, g_file_info_get_name(info));
		GFile *d = g_file_get_child(dst, g_file_info_get_name(info));

		if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
			copy_tree(s, d);
		} else {
			g_file_copy(s, d, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &error);
			g_assert_no_error(error);
		}
		g_object_unref(s);
		g_object_unref(d);
		g_object_unref(info);
	}
	g_assert_no_error(error);
	g_object_unref(en);
}

static void
fixture_setup(Fixture *f, gconstpointer data)
{
	const char *profile = pidgin_test_profile_setup();
	char *src_path = pidgin_test_data_path("logs");
	char *dir, *db;
	GFile *src, *dst;
	GError *error = NULL;

	f->logs = g_build_filename(profile, "logs", NULL);
	src = g_file_new_for_path(src_path);
	dst = g_file_new_for_path(f->logs);
	copy_tree(src, dst);
	g_object_unref(src);
	g_object_unref(dst);
	g_free(src_path);

	dir = g_build_filename(profile, "pidgin4", NULL);
	g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
	db = g_build_filename(dir, "messages.db", NULL);
	f->idx = pidgin_message_index_open(db, &error);
	g_assert_no_error(error);
	g_free(db);
	g_free(dir);

	f->bf = pidgin_backfill_new(f->idx, f->logs);
	g_object_set(f->bf, "throttle-ms", 0, NULL);
}

static void
fixture_teardown(Fixture *f, gconstpointer data)
{
	g_clear_object(&f->bf);
	g_clear_object(&f->idx);
	g_free(f->logs);
	pidgin_test_profile_cleanup();
}

static gint64
local_time(int y, int mo, int d, int h, int mi, int s)
{
	GDateTime *dt = g_date_time_new_local(y, mo, d, h, mi, s);
	gint64 t = g_date_time_to_unix(dt);

	g_date_time_unref(dt);
	return t;
}

static void
run_sync(Fixture *f)
{
	GError *error = NULL;

	g_assert_true(pidgin_backfill_run_sync(f->bf, NULL, &error));
	g_assert_no_error(error);
}

/* Rows of one conversation, oldest first. */
static GPtrArray *
conv_rows(Fixture *f, const char *account, const char *conv)
{
	GPtrArray *rows = pidgin_message_index_get_recent(f->idx, account, conv, 0, 1000);
	guint i;

	for (i = 0; i < rows->len / 2; i++) {
		gpointer tmp = rows->pdata[i];
		rows->pdata[i] = rows->pdata[rows->len - 1 - i];
		rows->pdata[rows->len - 1 - i] = tmp;
	}
	return rows;
}

/* Every row's log offset points at the start of its line. */
static void
check_offsets(Fixture *f, GPtrArray *rows)
{
	guint i;

	for (i = 0; i < rows->len; i++) {
		PidginIndexedMessage *msg = rows->pdata[i];
		char *path = g_build_filename(f->logs, msg->log_file, NULL);
		char *contents;
		gsize len;

		g_assert_true(g_file_get_contents(path, &contents, &len, NULL));
		g_assert_cmpint(msg->log_offset, >=, 0);
		g_assert_cmpint(msg->log_offset, <, (gint64)len);
		g_assert_true(msg->log_offset == 0 || contents[msg->log_offset - 1] == '\n');
		g_assert_true(contents[msg->log_offset] == '<' ||
				contents[msg->log_offset] == '(');
		g_free(contents);
		g_free(path);
	}
}

typedef struct {
	gint64 time;
	const char *sender;
	const char *body;
	guint flags;
} Expected;

static void
check_rows(GPtrArray *rows, const Expected *exp, guint n)
{
	guint i;

	g_assert_cmpuint(rows->len, ==, n);
	for (i = 0; i < n; i++) {
		PidginIndexedMessage *msg = rows->pdata[i];

		g_test_message("row %u: %s", i, msg->body);
		g_assert_cmpint(msg->time, ==, exp[i].time);
		g_assert_cmpstr(msg->sender, ==, exp[i].sender);
		g_assert_cmpstr(msg->body, ==, exp[i].body);
		g_assert_cmpuint(msg->flags, ==, exp[i].flags);
	}
}

static void
test_full(Fixture *f, gconstpointer data)
{
	GPtrArray *rows;
	PidginIndexedMessage *msg;
	Expected friend_exp[] = {
		{ local_time(2024, 1, 2, 23, 0, 5), "me", "Hello there", PURPLE_MESSAGE_SEND },
		{ local_time(2024, 1, 2, 23, 1, 10), "Friend <F> & Co", "Hi! How are you & yours?",
			PURPLE_MESSAGE_RECV },
		{ local_time(2024, 1, 2, 23, 2, 0), "friend", "waves", PURPLE_MESSAGE_RECV },
		{ local_time(2024, 1, 2, 23, 3, 0), NULL, "friend has signed off.",
			PURPLE_MESSAGE_SYSTEM },
		{ local_time(2024, 1, 2, 23, 4, 0), NULL, "Message could not be sent",
			PURPLE_MESSAGE_ERROR },
		{ local_time(2024, 1, 2, 23, 5, 0), "friend", "I am away",
			PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_AUTO_RESP },
		{ local_time(2024, 1, 2, 23, 59, 30), "me", "line one line two line three",
			PURPLE_MESSAGE_SEND },
		{ local_time(2024, 1, 3, 0, 0, 15), "friend", "after midnight", PURPLE_MESSAGE_RECV },
		{ local_time(2024, 1, 3, 0, 30, 0), "friend", "dated line", PURPLE_MESSAGE_RECV },
		{ local_time(2024, 1, 3, 1, 15, 0), "me", "iso stamp", PURPLE_MESSAGE_SEND },
		{ local_time(2024, 1, 3, 1, 20, 0), "friend", "first part second part",
			PURPLE_MESSAGE_RECV },
	};
	Expected room_exp[] = {
		{ local_time(2024, 1, 5, 10, 0, 1), "alice", "hi all", PURPLE_MESSAGE_RECV },
		{ local_time(2024, 1, 5, 10, 0, 30), "bob", "psst", PURPLE_MESSAGE_WHISPER },
		{ local_time(2024, 1, 5, 10, 1, 0), "carol", "café crème", PURPLE_MESSAGE_RECV },
	};
	Expected irc_exp[] = {
		{ local_time(2024, 2, 10, 8, 0, 1), "alice", "good morning", PURPLE_MESSAGE_RECV },
		{ local_time(2024, 2, 10, 8, 0, 30), "bob", "stretches", PURPLE_MESSAGE_RECV },
		{ local_time(2024, 2, 10, 8, 1, 0), NULL, "carol has joined the channel",
			PURPLE_MESSAGE_SYSTEM },
		{ local_time(2024, 2, 10, 8, 2, 0), "dave", "multi second line", PURPLE_MESSAGE_RECV },
		{ local_time(2024, 2, 10, 8, 3, 0), "alice", "brb",
			PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_AUTO_RESP },
	};

	run_sync(f);
	g_assert_cmpint(pidgin_message_index_count(f->idx), ==, FIXTURE_ROWS);

	rows = conv_rows(f, JABBER, "friend@example.com");
	check_rows(rows, friend_exp, G_N_ELEMENTS(friend_exp));
	check_offsets(f, rows);
	msg = rows->pdata[0];
	g_assert_cmpstr(msg->log_file, ==, FRIEND_FILE);
	g_assert_false(msg->is_chat);
	g_ptr_array_unref(rows);

	rows = conv_rows(f, JABBER, "room@conference.example.com");
	check_rows(rows, room_exp, G_N_ELEMENTS(room_exp));
	check_offsets(f, rows);
	g_assert_true(((PidginIndexedMessage *)rows->pdata[0])->is_chat);
	g_ptr_array_unref(rows);

	rows = conv_rows(f, "irc/me@irc.example.net", "#channel");
	check_rows(rows, irc_exp, G_N_ELEMENTS(irc_exp));
	check_offsets(f, rows);
	g_assert_true(((PidginIndexedMessage *)rows->pdata[0])->is_chat);
	g_ptr_array_unref(rows);

	/* .system is skipped; the malformed line is dropped. */
	rows = pidgin_message_index_search(f->idx, "indexed", NULL, NULL, 0);
	g_assert_cmpuint(rows->len, ==, 0);
	g_ptr_array_unref(rows);
	rows = pidgin_message_index_search(f->idx, "malformed", NULL, NULL, 0);
	g_assert_cmpuint(rows->len, ==, 0);
	g_ptr_array_unref(rows);
	rows = pidgin_message_index_search(f->idx, "creme", NULL, NULL, 0);
	g_assert_cmpuint(rows->len, ==, 1);
	g_ptr_array_unref(rows);
}

static void
bump_mtime(const char *path)
{
	GStatBuf st;
	struct utimbuf times;

	g_assert_cmpint(g_stat(path, &st), ==, 0);
	times.actime = st.st_atime;
	times.modtime = st.st_mtime + 10;
	g_assert_cmpint(g_utime(path, &times), ==, 0);
}

static gint64
max_id(Fixture *f)
{
	sqlite3 *db;
	sqlite3_stmt *stmt;
	gint64 id = -1;

	g_assert_cmpint(sqlite3_open_v2(pidgin_message_index_get_path(f->idx), &db,
			SQLITE_OPEN_READONLY, NULL), ==, SQLITE_OK);
	g_assert_cmpint(sqlite3_prepare_v2(db, "SELECT max(id) FROM messages", -1,
			&stmt, NULL), ==, SQLITE_OK);
	if (sqlite3_step(stmt) == SQLITE_ROW)
		id = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);
	sqlite3_close(db);

	return id;
}

static void
test_incremental(Fixture *f, gconstpointer data)
{
	char *path;
	FILE *fp;
	GPtrArray *rows;
	PidginIndexedMessage *msg;
	gint64 mtime, size, offset;
	GStatBuf st;
	char *contents;
	gsize len;
	char *second_nl;

	run_sync(f);
	g_assert_cmpint(pidgin_message_index_count(f->idx), ==, FIXTURE_ROWS);

	/* The cursor covers the whole file. */
	path = g_build_filename(f->logs, FRIEND_FILE, NULL);
	g_assert_true(pidgin_message_index_get_file_state(f->idx, FRIEND_FILE,
			&mtime, &size, &offset));
	g_assert_cmpint(g_stat(path, &st), ==, 0);
	g_assert_cmpint(size, ==, st.st_size);
	g_assert_cmpint(offset, ==, st.st_size);
	g_assert_cmpint(mtime, ==, st.st_mtime);

	/* No changes: nothing new. */
	run_sync(f);
	g_assert_cmpint(pidgin_message_index_count(f->idx), ==, FIXTURE_ROWS);

	/* Appended line: only it is indexed, on the rolled-over day. */
	fp = g_fopen(path, "a");
	g_assert_nonnull(fp);
	fputs("<span style=\"color: #A82F2F\"><span style=\"font-size: smaller\">"
	      "(01:30:00 AM)</span> <b>friend:</b></span> appended line<br>\n", fp);
	fclose(fp);
	bump_mtime(path);
	run_sync(f);
	g_assert_cmpint(pidgin_message_index_count(f->idx), ==, FIXTURE_ROWS + 1);
	rows = pidgin_message_index_search(f->idx, "appended", NULL, NULL, 0);
	g_assert_cmpuint(rows->len, ==, 1);
	msg = rows->pdata[0];
	g_assert_cmpint(msg->time, ==, local_time(2024, 1, 3, 1, 30, 0));
	g_assert_cmpstr(msg->log_file, ==, FRIEND_FILE);
	g_assert_cmpint(msg->log_offset, ==, st.st_size);
	g_ptr_array_unref(rows);
	g_free(path);

	/* Truncated file: forgotten and indexed again. */
	path = g_build_filename(f->logs, ROOM_FILE, NULL);
	g_assert_true(g_file_get_contents(path, &contents, &len, NULL));
	second_nl = strchr(strchr(contents, '\n') + 1, '\n');
	g_assert_true(g_file_set_contents(path, contents, second_nl + 1 - contents, NULL));
	g_free(contents);
	run_sync(f);
	g_assert_cmpint(pidgin_message_index_count(f->idx), ==, FIXTURE_ROWS + 1 - 2);
	rows = conv_rows(f, JABBER, "room@conference.example.com");
	g_assert_cmpuint(rows->len, ==, 1);
	g_assert_cmpstr(((PidginIndexedMessage *)rows->pdata[0])->body, ==, "hi all");
	g_ptr_array_unref(rows);
	g_free(path);

	/* And a pass after that is a no-op again. */
	offset = max_id(f);
	run_sync(f);
	g_assert_cmpint(max_id(f), ==, offset);
	g_assert_cmpint(pidgin_message_index_count(f->idx), ==, FIXTURE_ROWS - 1);
}

static void
test_live_rows_linked(Fixture *f, gconstpointer data)
{
	PidginIndexedMessage *msg = pidgin_indexed_message_new();
	GPtrArray *rows;
	gint64 id;

	/* A message pidgin4 stored live, before the backfill saw its line. */
	msg->account = g_strdup(JABBER);
	msg->conv = g_strdup("friend@example.com");
	msg->time = local_time(2024, 1, 2, 23, 0, 7);
	msg->sender = g_strdup("me@example.com");
	msg->body = g_strdup("Hello there");
	msg->stanza_id = g_strdup("live-1");
	id = pidgin_message_index_insert(f->idx, msg, NULL);
	pidgin_indexed_message_free(msg);

	run_sync(f);
	g_assert_cmpint(pidgin_message_index_count(f->idx), ==, FIXTURE_ROWS);
	msg = pidgin_message_index_get(f->idx, id);
	g_assert_cmpstr(msg->log_file, ==, FRIEND_FILE);
	g_assert_cmpint(msg->log_offset, >, 0);
	g_assert_cmpstr(msg->stanza_id, ==, "live-1");
	pidgin_indexed_message_free(msg);

	rows = pidgin_message_index_search(f->idx, "hello there", NULL, NULL, 0);
	g_assert_cmpuint(rows->len, ==, 1);
	g_ptr_array_unref(rows);
}

static void
test_parse_lines(void)
{
	GDateTime *start = pidgin_backfill_parse_file_name("2025-07-08.212153-0700PDT.html");
	GDateTime *last = NULL;
	PidginIndexedMessage *msg = pidgin_indexed_message_new();

	g_assert_nonnull(start);
	g_assert_cmpint(g_date_time_to_unix(start), ==, local_time(2025, 7, 8, 21, 21, 53));
	g_assert_null(pidgin_backfill_parse_file_name("notes.html"));

	/* 24-hour time. */
	g_assert_true(pidgin_backfill_parse_html_line("<span style=\"color: #A82F2F\">"
			"<span style=\"font-size: smaller\">(22:19:46)</span> <b>a:</b></span> x<br>",
			start, &last, msg));
	g_assert_cmpint(msg->time, ==, local_time(2025, 7, 8, 22, 19, 46));

	/* US date + 12-hour time, before the cursor: doesn't move it back. */
	g_assert_true(pidgin_backfill_parse_html_line("<span style=\"color: #16569E\">"
			"<span style=\"font-size: smaller\">(07/07/2025 10:19:46 PM)</span> <b>b:</b></span> y<br>",
			start, &last, msg));
	g_assert_cmpint(msg->time, ==, local_time(2025, 7, 7, 22, 19, 46));
	g_assert_cmpuint(msg->flags, ==, PURPLE_MESSAGE_SEND);
	g_assert_cmpint(g_date_time_to_unix(last), ==, local_time(2025, 7, 8, 22, 19, 46));

	/* Day rollover. */
	g_assert_true(pidgin_backfill_parse_html_line("<span style=\"color: #A82F2F\">"
			"<span style=\"font-size: smaller\">(12:01:00 AM)</span> <b>a:</b></span> z<br>",
			start, &last, msg));
	g_assert_cmpint(msg->time, ==, local_time(2025, 7, 9, 0, 1, 0));

	/* European date, ISO date. */
	g_assert_true(pidgin_backfill_parse_html_line("<span style=\"color: #A82F2F\">"
			"<span style=\"font-size: smaller\">(10.07.2025 08:00:00)</span> <b>a:</b></span> z<br>",
			start, &last, msg));
	g_assert_cmpint(msg->time, ==, local_time(2025, 7, 10, 8, 0, 0));
	g_assert_true(pidgin_backfill_parse_html_line("<span style=\"color: #A82F2F\">"
			"<span style=\"font-size: smaller\">(2025-07-11 09:00)</span> <b>a:</b></span> z<br>",
			start, &last, msg));
	g_assert_cmpint(msg->time, ==, local_time(2025, 7, 11, 9, 0, 0));

	/* Gaim's <font> markup. */
	g_assert_true(pidgin_backfill_parse_html_line("<font color=\"#A82F2F\"><font size=\"2\">"
			"(09:05:00)</font> <b>old:</b></font> <span style='font-weight: bold;'>hey</span><br/>",
			start, &last, msg));
	g_assert_cmpstr(msg->sender, ==, "old");
	g_assert_cmpstr(msg->body, ==, "hey");
	g_assert_cmpint(msg->time, ==, local_time(2025, 7, 11, 9, 5, 0));

	/* The "unhandled type" and raw forms. */
	g_assert_true(pidgin_backfill_parse_html_line("<span style=\"font-size: smaller\">"
			"(09:06:00)</font><b> who:</b> what<br>", start, &last, msg));
	g_assert_cmpstr(msg->sender, ==, "who");
	g_assert_cmpstr(msg->body, ==, "what");
	g_assert_cmpuint(msg->flags, ==, 0);
	g_assert_true(pidgin_backfill_parse_html_line("<span style=\"font-size: smaller\">"
			"(09:07:00)</span> raw <i>text</i><br>", start, &last, msg));
	g_assert_null(msg->sender);
	g_assert_cmpstr(msg->body, ==, "raw text");
	g_assert_cmpuint(msg->flags, ==, PURPLE_MESSAGE_RAW);

	/* Numeric references (g_markup_escape_text() writes &#39;) are decoded
	 * without making markup of escaped '<'. */
	g_assert_true(pidgin_backfill_parse_html_line("<span style=\"color: #A82F2F\">"
			"<span style=\"font-size: smaller\">(09:08:00)</span> <b>O&#39;Brien:</b></span>"
			" &#60;b&#62; is &#x2603; &amp;&#38; &#0; &#xZZ;<br>", start, &last, msg));
	g_assert_cmpstr(msg->sender, ==, "O'Brien");
	g_assert_cmpstr(msg->body, ==, "<b> is \xe2\x98\x83 && &#0; &#xZZ;");

	/* Not message lines. */
	g_assert_false(pidgin_backfill_parse_html_line("<html><head>", start, &last, msg));
	g_assert_false(pidgin_backfill_parse_html_line("", start, &last, msg));
	g_assert_false(pidgin_backfill_parse_html_line("<span style=\"font-size: smaller\">"
			"(25:99:00)</span> x<br>", start, &last, msg));
	g_assert_false(pidgin_backfill_parse_html_line("<span style=\"font-size: smaller\">"
			"(10:00:00", start, &last, msg));
	g_assert_false(pidgin_backfill_parse_txt_line("Conversation with x", start, &last, msg));

	g_assert_true(pidgin_backfill_parse_txt_line("(09:08:00) *bob* whisper", start, &last, msg));
	g_assert_cmpstr(msg->sender, ==, "bob");
	g_assert_cmpuint(msg->flags, ==, PURPLE_MESSAGE_WHISPER);
	g_assert_true(pidgin_backfill_parse_txt_line("(09:09:00) The topic for #x is: stuff",
			start, &last, msg));
	g_assert_null(msg->sender);
	g_assert_cmpuint(msg->flags, ==, PURPLE_MESSAGE_SYSTEM);

	pidgin_indexed_message_free(msg);
	g_date_time_unref(last);
	g_date_time_unref(start);
}

/* The async path: paused before it starts, then resumed or cancelled. */
typedef struct {
	GMainLoop *loop;
	gboolean finished;
	gboolean completed;
	guint progress;
	guint files_total;
} AsyncState;

static void
progress_cb(PidginBackfill *bf, guint files_done, guint files_total,
		guint64 bytes_done, guint64 bytes_total, AsyncState *s)
{
	g_assert_true(g_main_context_is_owner(g_main_context_default()));
	g_assert_cmpuint(files_done, <=, files_total);
	g_assert_cmpuint(bytes_done, <=, bytes_total);
	s->progress++;
	s->files_total = files_total;
}

static void
finished_cb(PidginBackfill *bf, gboolean completed, AsyncState *s)
{
	s->finished = TRUE;
	s->completed = completed;
	g_main_loop_quit(s->loop);
}

static gboolean
quit_cb(gpointer loop)
{
	g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}

static void
spin(AsyncState *s, guint ms)
{
	g_timeout_add(ms, quit_cb, s->loop);
	g_main_loop_run(s->loop);
}

static void
test_async(Fixture *f, gconstpointer cancel)
{
	AsyncState s = { 0 };
	guint timeout;

	s.loop = g_main_loop_new(NULL, FALSE);
	g_signal_connect(f->bf, "progress", G_CALLBACK(progress_cb), &s);
	g_signal_connect(f->bf, "finished", G_CALLBACK(finished_cb), &s);

	pidgin_backfill_pause(f->bf);
	pidgin_backfill_start(f->bf);
	g_assert_true(pidgin_backfill_is_running(f->bf));
	pidgin_backfill_start(f->bf);   /* no-op */

	spin(&s, 200);
	g_assert_false(s.finished);
	g_assert_true(pidgin_backfill_is_running(f->bf));
	g_assert_cmpint(pidgin_message_index_count(f->idx), ==, 0);
	/* The scan reported its totals. */
	g_assert_cmpuint(s.progress, >=, 1);
	g_assert_cmpuint(s.files_total, ==, 3);   /* .system is skipped */

	if (GPOINTER_TO_INT(cancel))
		pidgin_backfill_cancel(f->bf);
	else
		pidgin_backfill_resume(f->bf);

	timeout = g_timeout_add_seconds(10, quit_cb, s.loop);
	g_main_loop_run(s.loop);
	g_source_remove(timeout);

	g_assert_true(s.finished);
	g_assert_false(pidgin_backfill_is_running(f->bf));
	if (GPOINTER_TO_INT(cancel)) {
		g_assert_false(s.completed);
		g_assert_cmpint(pidgin_message_index_count(f->idx), ==, 0);
	} else {
		g_assert_true(s.completed);
		g_assert_cmpint(pidgin_message_index_count(f->idx), ==, FIXTURE_ROWS);
	}

	/* It can run again afterwards. */
	s.finished = FALSE;
	pidgin_backfill_start(f->bf);
	timeout = g_timeout_add_seconds(10, quit_cb, s.loop);
	g_main_loop_run(s.loop);
	g_source_remove(timeout);
	g_assert_true(s.finished);
	g_assert_true(s.completed);
	g_assert_cmpint(pidgin_message_index_count(f->idx), ==, FIXTURE_ROWS);

	g_signal_handlers_disconnect_by_data(f->bf, &s);
	g_main_loop_unref(s.loop);
}

int
main(int argc, char *argv[])
{
	/* Expected times are built in this zone; the fixtures are PST. */
	g_setenv("TZ", "America/Los_Angeles", TRUE);
	tzset();

	g_test_init(&argc, &argv, NULL);

#define ADD(name, func, data) \
	g_test_add(name, Fixture, data, fixture_setup, func, fixture_teardown)

	g_test_add_func("/backfill/parse-lines", test_parse_lines);
	ADD("/backfill/full", test_full, NULL);
	ADD("/backfill/incremental", test_incremental, NULL);
	ADD("/backfill/live-rows-linked", test_live_rows_linked, NULL);
	ADD("/backfill/async-resume", test_async, GINT_TO_POINTER(FALSE));
	ADD("/backfill/async-cancel", test_async, GINT_TO_POINTER(TRUE));

	return g_test_run();
}
