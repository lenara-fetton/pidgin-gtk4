/*
 * pidgin4
 *
 * Pidgin is the legal property of its developers, whose names are too numerous
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
 */
#include "pidgin-internal.h"

#include <glib/gstdio.h>
#include <sqlite3.h>

#include "account.h"
#include "connection.h"
#include "debug.h"
#include "plugin.h"
#include "prefs.h"
#include "prpl.h"
#include "signals.h"
#include "util.h"

#include "pidginbackfill.h"
#include "pidginmessageindex.h"

#define FUZZY_WINDOW 120
#define DEFAULT_LIMIT 100

/* One SQLite connection and its prepared statement cache.  The caller
 * holds the lock that guards it. */
typedef struct {
	sqlite3 *db;
	GHashTable *stmts;   /* const char *sql (static) -> sqlite3_stmt * */
} IndexConn;

struct _PidginMessageIndex {
	GObject parent;

	char *path;

	IndexConn writer;
	GRecMutex write_lock;
	GThread *batch_thread;   /* owner of the running batch, or NULL */

	IndexConn reader;        /* db NULL if it couldn't be opened */
	GMutex read_lock;

	/* The last "jabber-kv-load" answer; the prpl copies it at once. */
	char *kv_last;
	void *kv_handle;         /* instance the kv signals are connected on */
};

G_DEFINE_TYPE(PidginMessageIndex, pidgin_message_index, G_TYPE_OBJECT)

G_DEFINE_BOXED_TYPE(PidginIndexedMessage, pidgin_indexed_message,
		pidgin_indexed_message_copy, pidgin_indexed_message_free)

static PidginMessageIndex *default_index = NULL;

/******************************************************************************
 * PidginIndexedMessage
 *****************************************************************************/

PidginIndexedMessage *
pidgin_indexed_message_new(void)
{
	PidginIndexedMessage *msg = g_new0(PidginIndexedMessage, 1);

	msg->log_offset = -1;
	return msg;
}

PidginIndexedMessage *
pidgin_indexed_message_copy(const PidginIndexedMessage *msg)
{
	PidginIndexedMessage *copy;

	g_return_val_if_fail(msg != NULL, NULL);

	copy = g_new(PidginIndexedMessage, 1);
	*copy = *msg;
	copy->account = g_strdup(msg->account);
	copy->conv = g_strdup(msg->conv);
	copy->sender = g_strdup(msg->sender);
	copy->body = g_strdup(msg->body);
	copy->stanza_id = g_strdup(msg->stanza_id);
	copy->origin_id = g_strdup(msg->origin_id);
	copy->server_id = g_strdup(msg->server_id);
	copy->occupant_id = g_strdup(msg->occupant_id);
	copy->correction_of = g_strdup(msg->correction_of);
	copy->reply_to = g_strdup(msg->reply_to);
	copy->log_file = g_strdup(msg->log_file);

	return copy;
}

void
pidgin_indexed_message_free(PidginIndexedMessage *msg)
{
	if (msg == NULL)
		return;

	g_free(msg->account);
	g_free(msg->conv);
	g_free(msg->sender);
	g_free(msg->body);
	g_free(msg->stanza_id);
	g_free(msg->origin_id);
	g_free(msg->server_id);
	g_free(msg->occupant_id);
	g_free(msg->correction_of);
	g_free(msg->reply_to);
	g_free(msg->log_file);
	g_free(msg);
}

/******************************************************************************
 * Connections
 *****************************************************************************/

static const char *schema_v1 =
	"CREATE TABLE IF NOT EXISTS messages ("
	" id INTEGER PRIMARY KEY,"
	" account TEXT NOT NULL,"
	" conv TEXT NOT NULL,"
	" is_chat INT NOT NULL DEFAULT 0,"
	" time INT NOT NULL,"
	" sender TEXT,"
	" stanza_id TEXT,"
	" origin_id TEXT,"
	" server_id TEXT,"
	" occupant_id TEXT,"
	" correction_of TEXT,"
	" reply_to TEXT,"
	" log_file TEXT,"
	" log_offset INT,"
	" flags INT NOT NULL DEFAULT 0);"
	"CREATE INDEX IF NOT EXISTS messages_conv_time ON messages(account, conv, time);"
	/* Most rows (everything backfilled) have no ids: partial indexes keep
	 * them out.  There is deliberately no (log_file, log_offset) index:
	 * log paths determine account and conv, so position lookups go
	 * through (account, conv, time), and the extra index cost about a
	 * quarter of the database. */
	"CREATE INDEX IF NOT EXISTS messages_stanza_id ON messages(stanza_id)"
	" WHERE stanza_id IS NOT NULL;"
	"CREATE INDEX IF NOT EXISTS messages_origin_id ON messages(origin_id)"
	" WHERE origin_id IS NOT NULL;"
	"CREATE INDEX IF NOT EXISTS messages_server_id ON messages(server_id)"
	" WHERE server_id IS NOT NULL;"
	"CREATE VIRTUAL TABLE IF NOT EXISTS messages_fts USING fts5("
	" body, tokenize = 'unicode61 remove_diacritics 2');"
	"CREATE TRIGGER IF NOT EXISTS messages_fts_delete AFTER DELETE ON messages"
	" BEGIN DELETE FROM messages_fts WHERE rowid = old.id; END;"
	"CREATE TABLE IF NOT EXISTS reactions ("
	" msg INT NOT NULL REFERENCES messages(id) ON DELETE CASCADE,"
	" emoji TEXT NOT NULL,"
	" sender TEXT NOT NULL,"
	" UNIQUE(msg, emoji, sender));"
	"CREATE TABLE IF NOT EXISTS receipts ("
	" msg INT NOT NULL REFERENCES messages(id) ON DELETE CASCADE,"
	" state TEXT NOT NULL,"
	" sender TEXT,"
	" UNIQUE(msg, state, sender));"
	"CREATE TABLE IF NOT EXISTS kv ("
	" account TEXT NOT NULL,"
	" key TEXT NOT NULL,"
	" value TEXT,"
	" PRIMARY KEY(account, key));"
	"CREATE TABLE IF NOT EXISTS indexed_files ("
	" path TEXT PRIMARY KEY,"
	" mtime INT,"
	" size INT,"
	" offset INT);";

static void
index_conn_close(IndexConn *conn)
{
	if (conn->stmts != NULL) {
		g_hash_table_destroy(conn->stmts);
		conn->stmts = NULL;
	}
	if (conn->db != NULL) {
		sqlite3_close_v2(conn->db);
		conn->db = NULL;
	}
}

static gboolean
index_conn_open(IndexConn *conn, const char *path, gboolean readonly,
		GError **error)
{
	int flags = SQLITE_OPEN_FULLMUTEX;
	int rc;

	flags |= readonly ? SQLITE_OPEN_READONLY :
		(SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);

	rc = sqlite3_open_v2(path, &conn->db, flags, NULL);
	if (rc != SQLITE_OK) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
				"Could not open %s: %s", path,
				conn->db ? sqlite3_errmsg(conn->db) : sqlite3_errstr(rc));
		index_conn_close(conn);
		return FALSE;
	}

	conn->stmts = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
			(GDestroyNotify)sqlite3_finalize);
	sqlite3_busy_timeout(conn->db, 10000);
	sqlite3_extended_result_codes(conn->db, 1);

	return TRUE;
}

static gboolean
index_exec(IndexConn *conn, const char *sql, GError **error)
{
	char *errmsg = NULL;

	if (sqlite3_exec(conn->db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
				"SQL error: %s", errmsg ? errmsg : "unknown");
		sqlite3_free(errmsg);
		return FALSE;
	}
	return TRUE;
}

/* Returns a cached, reset statement for @sql, which must be a string
 * literal (statements are keyed by address). */
static sqlite3_stmt *
index_stmt(IndexConn *conn, const char *sql)
{
	sqlite3_stmt *stmt = g_hash_table_lookup(conn->stmts, sql);

	if (stmt == NULL) {
		if (sqlite3_prepare_v3(conn->db, sql, -1, SQLITE_PREPARE_PERSISTENT,
				&stmt, NULL) != SQLITE_OK) {
			g_warning("message index: can't prepare \"%s\": %s", sql,
					sqlite3_errmsg(conn->db));
			return NULL;
		}
		g_hash_table_insert(conn->stmts, (gpointer)sql, stmt);
	}

	return stmt;
}

static void
stmt_done(sqlite3_stmt *stmt)
{
	if (stmt != NULL) {
		sqlite3_reset(stmt);
		sqlite3_clear_bindings(stmt);
	}
}

static void
bind_text(sqlite3_stmt *stmt, int col, const char *text)
{
	if (text == NULL)
		sqlite3_bind_null(stmt, col);
	else
		sqlite3_bind_text(stmt, col, text, -1, SQLITE_TRANSIENT);
}

static char *
column_text(sqlite3_stmt *stmt, int col)
{
	const unsigned char *text = sqlite3_column_text(stmt, col);

	return text ? g_strdup((const char *)text) : NULL;
}

/* Runs a write statement to completion. */
static gboolean
stmt_step_done(IndexConn *conn, sqlite3_stmt *stmt)
{
	int rc = sqlite3_step(stmt);
	gboolean ok = (rc == SQLITE_DONE || rc == SQLITE_ROW);

	if (!ok)
		g_warning("message index: %s", sqlite3_errmsg(conn->db));
	stmt_done(stmt);

	return ok;
}

/* Writer access.  The lock is recursive so a batch owner can call the
 * other write functions. */
static IndexConn *
writer_lock(PidginMessageIndex *idx)
{
	g_rec_mutex_lock(&idx->write_lock);
	return &idx->writer;
}

static void
writer_unlock(PidginMessageIndex *idx)
{
	g_rec_mutex_unlock(&idx->write_lock);
}

/* Reader access: the read-only connection, unless this thread runs the
 * current batch (it must see its own rows) or there is no reader. */
static IndexConn *
reader_lock(PidginMessageIndex *idx, gboolean *is_writer)
{
	if (idx->reader.db == NULL ||
	    g_atomic_pointer_get(&idx->batch_thread) == g_thread_self()) {
		*is_writer = TRUE;
		return writer_lock(idx);
	}

	*is_writer = FALSE;
	g_mutex_lock(&idx->read_lock);
	return &idx->reader;
}

static void
reader_unlock(PidginMessageIndex *idx, gboolean is_writer)
{
	if (is_writer)
		writer_unlock(idx);
	else
		g_mutex_unlock(&idx->read_lock);
}

/******************************************************************************
 * GObject
 *****************************************************************************/

static void
pidgin_message_index_finalize(GObject *obj)
{
	PidginMessageIndex *idx = PIDGIN_MESSAGE_INDEX(obj);

	if (idx->kv_handle != NULL)
		purple_signals_disconnect_by_handle(idx);

	index_conn_close(&idx->reader);
	if (idx->writer.db != NULL) {
		/* Fold the WAL back so the directory holds one file at rest. */
		sqlite3_wal_checkpoint_v2(idx->writer.db, NULL,
				SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
	}
	index_conn_close(&idx->writer);

	g_rec_mutex_clear(&idx->write_lock);
	g_mutex_clear(&idx->read_lock);
	g_free(idx->kv_last);
	g_free(idx->path);

	G_OBJECT_CLASS(pidgin_message_index_parent_class)->finalize(obj);
}

static void
pidgin_message_index_class_init(PidginMessageIndexClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = pidgin_message_index_finalize;
}

static void
pidgin_message_index_init(PidginMessageIndex *idx)
{
	g_rec_mutex_init(&idx->write_lock);
	g_mutex_init(&idx->read_lock);
}

static gboolean
index_setup_schema(PidginMessageIndex *idx, GError **error)
{
	sqlite3_stmt *stmt;
	int version = 0;
	char *sql;
	gboolean ok;

	if (!index_exec(&idx->writer,
			"PRAGMA journal_mode = WAL;"
			"PRAGMA synchronous = NORMAL;"
			"PRAGMA foreign_keys = ON;", error))
		return FALSE;

	if (sqlite3_prepare_v2(idx->writer.db, "PRAGMA user_version", -1,
			&stmt, NULL) != SQLITE_OK) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
				sqlite3_errmsg(idx->writer.db));
		return FALSE;
	}
	if (sqlite3_step(stmt) == SQLITE_ROW)
		version = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);

	if (version > PIDGIN_MESSAGE_INDEX_SCHEMA_VERSION) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
				"%s has schema version %d; this pidgin4 knows %d",
				idx->path, version, PIDGIN_MESSAGE_INDEX_SCHEMA_VERSION);
		return FALSE;
	}
	if (version == PIDGIN_MESSAGE_INDEX_SCHEMA_VERSION)
		return TRUE;

	sql = g_strdup_printf("BEGIN IMMEDIATE; %s PRAGMA user_version = %d; COMMIT;",
			schema_v1, PIDGIN_MESSAGE_INDEX_SCHEMA_VERSION);
	ok = index_exec(&idx->writer, sql, error);
	g_free(sql);
	if (!ok)
		index_exec(&idx->writer, "ROLLBACK", NULL);

	return ok;
}

PidginMessageIndex *
pidgin_message_index_open(const char *path, GError **error)
{
	PidginMessageIndex *idx;
	GError *read_error = NULL;

	g_return_val_if_fail(path != NULL, NULL);

	idx = g_object_new(PIDGIN_TYPE_MESSAGE_INDEX, NULL);
	idx->path = g_strdup(path);

	if (!index_conn_open(&idx->writer, path, FALSE, error) ||
	    !index_setup_schema(idx, error)) {
		g_object_unref(idx);
		return NULL;
	}

	/* The reader is an optimisation; without it reads use the writer. */
	if (!index_conn_open(&idx->reader, path, TRUE, &read_error)) {
		g_debug("message index: no read connection: %s", read_error->message);
		g_clear_error(&read_error);
	} else {
		index_exec(&idx->reader, "PRAGMA foreign_keys = ON", NULL);
	}

	return idx;
}

PidginMessageIndex *
pidgin_message_index_get_default(void)
{
	static gboolean failed = FALSE;
	GError *error = NULL;
	char *dir, *path;

	if (default_index != NULL || failed)
		return default_index;

	dir = g_build_filename(purple_user_dir(), "pidgin4", NULL);
	if (g_mkdir_with_parents(dir, 0700) != 0) {
		purple_debug_error("message-index", "Could not create %s: %s\n",
				dir, g_strerror(errno));
		g_free(dir);
		failed = TRUE;
		return NULL;
	}
	path = g_build_filename(dir, "messages.db", NULL);
	g_free(dir);

	default_index = pidgin_message_index_open(path, &error);
	if (default_index == NULL) {
		purple_debug_error("message-index", "%s\n", error->message);
		g_error_free(error);
		failed = TRUE;
	}
	g_free(path);

	return default_index;
}

const char *
pidgin_message_index_get_path(PidginMessageIndex *idx)
{
	g_return_val_if_fail(PIDGIN_IS_MESSAGE_INDEX(idx), NULL);
	return idx->path;
}

/******************************************************************************
 * Keys and text
 *****************************************************************************/

char *
pidgin_message_index_account_key(PurpleAccount *account)
{
	PurplePlugin *prpl;
	const char *prpl_name = NULL;

	g_return_val_if_fail(account != NULL, NULL);

	prpl = purple_find_prpl(purple_account_get_protocol_id(account));
	if (prpl != NULL && PURPLE_PLUGIN_PROTOCOL_INFO(prpl)->list_icon != NULL)
		prpl_name = PURPLE_PLUGIN_PROTOCOL_INFO(prpl)->list_icon(account, NULL);
	if (prpl_name == NULL) {
		/* Same fallback the log directory can't have; "prpl-foo" -> "foo". */
		prpl_name = purple_account_get_protocol_id(account);
		if (prpl_name != NULL && g_str_has_prefix(prpl_name, "prpl-"))
			prpl_name += 5;
	}

	return g_strdup_printf("%s/%s", prpl_name ? prpl_name : "unknown",
			purple_escape_filename(purple_normalize(account,
					purple_account_get_username(account))));
}

char *
pidgin_message_index_conv_key(PurpleAccount *account, const char *name)
{
	g_return_val_if_fail(name != NULL, NULL);

	return g_strdup(purple_escape_filename(purple_normalize(account, name)));
}

char *
pidgin_message_index_normalize_text(const char *text)
{
	GString *out;
	gboolean space = FALSE;
	const char *p;

	if (text == NULL)
		return NULL;

	if (!g_utf8_validate(text, -1, NULL)) {
		char *valid = g_utf8_make_valid(text, -1);
		char *ret = pidgin_message_index_normalize_text(valid);

		g_free(valid);
		return ret;
	}

	out = g_string_sized_new(strlen(text));
	for (p = text; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (g_unichar_isspace(c) || c == 0x00a0) {
			space = TRUE;
			continue;
		}
		if (space && out->len > 0)
			g_string_append_c(out, ' ');
		space = FALSE;
		g_string_append_unichar(out, c);
	}

	return g_string_free(out, FALSE);
}

/******************************************************************************
 * Log file paths
 *
 * The API always uses log paths relative to <profile>/logs, laid out as
 * "<prpl>/<account>/<conv>[.chat]/<file>".  Almost every row's path is its
 * own account and conversation directory, so the log_file column then
 * holds only "<file>" (a path is ~75 bytes, a quarter of a backfilled
 * row); any other path is stored whole.  A stored value without '/' is
 * therefore always a bare file name.
 *****************************************************************************/

static gboolean
log_dir_matches(const char *path, const char *account, const char *conv,
		gboolean is_chat, const char **name)
{
	size_t alen, clen;
	const char *p = path;

	if (path == NULL || account == NULL || conv == NULL)
		return FALSE;

	alen = strlen(account);
	clen = strlen(conv);
	if (strncmp(p, account, alen) != 0 || p[alen] != '/')
		return FALSE;
	p += alen + 1;
	if (strncmp(p, conv, clen) != 0)
		return FALSE;
	p += clen;
	if (is_chat) {
		if (strncmp(p, ".chat", 5) != 0)
			return FALSE;
		p += 5;
	}
	if (*p != '/' || p[1] == '\0' || strchr(p + 1, '/') != NULL)
		return FALSE;

	*name = p + 1;
	return TRUE;
}

/* The log_file column value for @msg's log_file. */
static const char *
log_file_to_column(const PidginIndexedMessage *msg)
{
	const char *name;

	if (log_dir_matches(msg->log_file, msg->account, msg->conv, msg->is_chat, &name))
		return name;
	return msg->log_file;
}

/* The API path for a log_file column value of the row @msg. */
static char *
log_file_from_column(const PidginIndexedMessage *msg, const char *value)
{
	if (value == NULL)
		return NULL;
	if (strchr(value, '/') != NULL)
		return g_strdup(value);

	return g_strdup_printf("%s/%s%s/%s", msg->account, msg->conv,
			msg->is_chat ? ".chat" : "", value);
}

/* "<prpl>/<account>/<conv>[.chat]/<file>" -> the row keys and the column
 * value, so lookups by log file use the (account, conv, time) index.
 * For any other shape account and conv are NULL (matching nothing). */
static const char *
split_log_path(const char *path, char **account, char **conv, gboolean *is_chat)
{
	char **parts = g_strsplit(path, "/", 4);
	const char *name = path;

	*account = *conv = NULL;
	*is_chat = FALSE;
	if (g_strv_length(parts) == 4 && strchr(parts[3], '/') == NULL) {
		*account = g_strdup_printf("%s/%s", parts[0], parts[1]);
		*is_chat = g_str_has_suffix(parts[2], ".chat");
		if (*is_chat)
			*conv = g_strndup(parts[2], strlen(parts[2]) - 5);
		else
			*conv = g_strdup(parts[2]);
		name = strrchr(path, '/') + 1;
	}
	g_strfreev(parts);

	return name;
}

/******************************************************************************
 * Messages
 *****************************************************************************/

#define MESSAGE_COLUMNS \
	"m.id, m.account, m.conv, m.is_chat, m.time, m.sender, m.stanza_id," \
	" m.origin_id, m.server_id, m.occupant_id, m.correction_of, m.reply_to," \
	" m.log_file, m.log_offset, m.flags, f.body"
#define MESSAGE_FROM \
	" FROM messages m LEFT JOIN messages_fts f ON f.rowid = m.id"

static PidginIndexedMessage *
message_from_row(sqlite3_stmt *stmt)
{
	PidginIndexedMessage *msg = pidgin_indexed_message_new();

	msg->id = sqlite3_column_int64(stmt, 0);
	msg->account = column_text(stmt, 1);
	msg->conv = column_text(stmt, 2);
	msg->is_chat = sqlite3_column_int(stmt, 3) != 0;
	msg->time = sqlite3_column_int64(stmt, 4);
	msg->sender = column_text(stmt, 5);
	msg->stanza_id = column_text(stmt, 6);
	msg->origin_id = column_text(stmt, 7);
	msg->server_id = column_text(stmt, 8);
	msg->occupant_id = column_text(stmt, 9);
	msg->correction_of = column_text(stmt, 10);
	msg->reply_to = column_text(stmt, 11);
	msg->log_file = log_file_from_column(msg,
			(const char *)sqlite3_column_text(stmt, 12));
	msg->log_offset = sqlite3_column_type(stmt, 13) == SQLITE_NULL ? -1 :
		sqlite3_column_int64(stmt, 13);
	msg->flags = (guint)sqlite3_column_int64(stmt, 14);
	msg->body = column_text(stmt, 15);

	return msg;
}

/* Steps @stmt and returns the first row as a message, or NULL. */
static PidginIndexedMessage *
query_one(sqlite3_stmt *stmt)
{
	PidginIndexedMessage *msg = NULL;

	if (sqlite3_step(stmt) == SQLITE_ROW)
		msg = message_from_row(stmt);
	stmt_done(stmt);

	return msg;
}

static GPtrArray *
query_many(sqlite3_stmt *stmt)
{
	GPtrArray *array = g_ptr_array_new_with_free_func(
			(GDestroyNotify)pidgin_indexed_message_free);

	if (stmt == NULL)
		return array;
	while (sqlite3_step(stmt) == SQLITE_ROW)
		g_ptr_array_add(array, message_from_row(stmt));
	stmt_done(stmt);

	return array;
}

gint64
pidgin_message_index_insert(PidginMessageIndex *idx,
		const PidginIndexedMessage *msg, GError **error)
{
	IndexConn *conn;
	sqlite3_stmt *stmt;
	gint64 id = 0;
	char *body;
	int rc;

	g_return_val_if_fail(PIDGIN_IS_MESSAGE_INDEX(idx), 0);
	g_return_val_if_fail(msg != NULL, 0);
	g_return_val_if_fail(msg->account != NULL && msg->conv != NULL, 0);

	conn = writer_lock(idx);

	/* Outside a batch, keep the row and its text in one transaction. */
	if (idx->batch_thread == NULL)
		index_exec(conn, "SAVEPOINT pidgin_insert", NULL);

	stmt = index_stmt(conn,
			"INSERT INTO messages (account, conv, is_chat, time, sender,"
			" stanza_id, origin_id, server_id, occupant_id, correction_of,"
			" reply_to, log_file, log_offset, flags)"
			" VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14)");
	if (stmt == NULL)
		goto fail;

	bind_text(stmt, 1, msg->account);
	bind_text(stmt, 2, msg->conv);
	sqlite3_bind_int(stmt, 3, msg->is_chat ? 1 : 0);
	sqlite3_bind_int64(stmt, 4, msg->time);
	bind_text(stmt, 5, msg->sender);
	bind_text(stmt, 6, msg->stanza_id);
	bind_text(stmt, 7, msg->origin_id);
	bind_text(stmt, 8, msg->server_id);
	bind_text(stmt, 9, msg->occupant_id);
	bind_text(stmt, 10, msg->correction_of);
	bind_text(stmt, 11, msg->reply_to);
	bind_text(stmt, 12, log_file_to_column(msg));
	if (msg->log_file != NULL && msg->log_offset >= 0)
		sqlite3_bind_int64(stmt, 13, msg->log_offset);
	else
		sqlite3_bind_null(stmt, 13);
	sqlite3_bind_int64(stmt, 14, msg->flags);

	rc = sqlite3_step(stmt);
	stmt_done(stmt);
	if (rc != SQLITE_DONE)
		goto fail;
	id = sqlite3_last_insert_rowid(conn->db);

	stmt = index_stmt(conn,
			"INSERT INTO messages_fts (rowid, body) VALUES (?1, ?2)");
	if (stmt == NULL)
		goto fail;
	body = pidgin_message_index_normalize_text(msg->body ? msg->body : "");
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_bind_text(stmt, 2, body, -1, g_free);
	rc = sqlite3_step(stmt);
	stmt_done(stmt);
	if (rc != SQLITE_DONE)
		goto fail;

	if (idx->batch_thread == NULL)
		index_exec(conn, "RELEASE pidgin_insert", NULL);
	writer_unlock(idx);

	return id;

fail:
	g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			"Could not store message: %s", sqlite3_errmsg(conn->db));
	if (idx->batch_thread == NULL) {
		index_exec(conn, "ROLLBACK TO pidgin_insert", NULL);
		index_exec(conn, "RELEASE pidgin_insert", NULL);
	}
	writer_unlock(idx);

	return 0;
}

gboolean
pidgin_message_index_set_ids(PidginMessageIndex *idx, gint64 id,
		const char *stanza_id, const char *origin_id, const char *server_id)
{
	IndexConn *conn;
	sqlite3_stmt *stmt;
	gboolean ok;

	g_return_val_if_fail(PIDGIN_IS_MESSAGE_INDEX(idx), FALSE);

	conn = writer_lock(idx);
	stmt = index_stmt(conn,
			"UPDATE messages SET stanza_id = coalesce(?2, stanza_id),"
			" origin_id = coalesce(?3, origin_id),"
			" server_id = coalesce(?4, server_id) WHERE id = ?1");
	if ((ok = (stmt != NULL))) {
		sqlite3_bind_int64(stmt, 1, id);
		bind_text(stmt, 2, stanza_id);
		bind_text(stmt, 3, origin_id);
		bind_text(stmt, 4, server_id);
		ok = stmt_step_done(conn, stmt) && sqlite3_changes(conn->db) > 0;
	}
	writer_unlock(idx);

	return ok;
}

gboolean
pidgin_message_index_delete(PidginMessageIndex *idx, gint64 id)
{
	IndexConn *conn;
	sqlite3_stmt *stmt;
	gboolean ok;

	g_return_val_if_fail(PIDGIN_IS_MESSAGE_INDEX(idx), FALSE);

	conn = writer_lock(idx);
	/* The trigger drops the text, the foreign keys the rest. */
	stmt = index_stmt(conn, "DELETE FROM messages WHERE id = ?1");
	if ((ok = (stmt != NULL))) {
		sqlite3_bind_int64(stmt, 1, id);
		ok = stmt_step_done(conn, stmt) && sqlite3_changes(conn->db) > 0;
	}
	writer_unlock(idx);

	return ok;
}

PidginIndexedMessage *
pidgin_message_index_get(PidginMessageIndex *idx, gint64 id)
{
	IndexConn *conn;
	sqlite3_stmt *stmt;
	PidginIndexedMessage *msg = NULL;
	gboolean w;

	g_return_val_if_fail(PIDGIN_IS_MESSAGE_INDEX(idx), NULL);

	conn = reader_lock(idx, &w);
	stmt = index_stmt(conn, "SELECT " MESSAGE_COLUMNS MESSAGE_FROM
			" WHERE m.id = ?1");
	if (stmt != NULL) {
		sqlite3_bind_int64(stmt, 1, id);
		msg = query_one(stmt);
	}
	reader_unlock(idx, w);

	return msg;
}

PidginIndexedMessage *
pidgin_message_index_find_by_id(PidginMessageIndex *idx,
		const char *account, const char *conv, const char *id)
{
	IndexConn *conn;
	sqlite3_stmt *stmt;
	PidginIndexedMessage *msg = NULL;
	gboolean w;

	g_return_val_if_fail(PIDGIN_IS_MESSAGE_INDEX(idx), NULL);

	if (account == NULL || conv == NULL || id == NULL || *id == '\0')
		return NULL;

	conn = reader_lock(idx, &w);
	stmt = index_stmt(conn, "SELECT " MESSAGE_COLUMNS MESSAGE_FROM
			/* Spelled out so the id indexes are used rather than a
			 * scan of the conversation. */
			" WHERE m.id IN (SELECT id FROM messages WHERE stanza_id = ?3"
			" UNION ALL SELECT id FROM messages WHERE origin_id = ?3"
			" UNION ALL SELECT id FROM messages WHERE server_id = ?3)"
			" AND +m.account = ?1 AND +m.conv = ?2"
			" ORDER BY m.time DESC, m.id DESC LIMIT 1");
	if (stmt != NULL) {
		bind_text(stmt, 1, account);
		bind_text(stmt, 2, conv);
		bind_text(stmt, 3, id);
		msg = query_one(stmt);
	}
	reader_unlock(idx, w);

	return msg;
}

/* The candidate query of the fuzzy match.  Uses the (account, conv, time)
 * index, then compares the stored text of those few rows. */
static PidginIndexedMessage *
find_fuzzy(IndexConn *conn, const char *account, const char *conv,
		gint64 time, const char *sender, const char *norm)
{
	sqlite3_stmt *stmt;

	stmt = index_stmt(conn, "SELECT " MESSAGE_COLUMNS MESSAGE_FROM
			" WHERE m.account = ?1 AND m.conv = ?2"
			" AND m.time BETWEEN ?3 - " G_STRINGIFY(FUZZY_WINDOW)
			" AND ?3 + " G_STRINGIFY(FUZZY_WINDOW)
			" AND (?4 IS NULL OR m.sender = ?4)"
			" AND f.body = ?5"
			" ORDER BY abs(m.time - ?3), m.id DESC LIMIT 1");
	if (stmt == NULL)
		return NULL;

	bind_text(stmt, 1, account);
	bind_text(stmt, 2, conv);
	sqlite3_bind_int64(stmt, 3, time);
	bind_text(stmt, 4, sender);
	bind_text(stmt, 5, norm);

	return query_one(stmt);
}

PidginIndexedMessage *
pidgin_message_index_find_fuzzy(PidginMessageIndex *idx,
		const char *account, const char *conv, gint64 time,
		const char *sender, const char *plain)
{
	IndexConn *conn;
	PidginIndexedMessage *msg;
	char *norm;
	gboolean w;

	g_return_val_if_fail(PIDGIN_IS_MESSAGE_INDEX(idx), NULL);

	if (account == NULL || conv == NULL || plain == NULL)
		return NULL;

	norm = pidgin_message_index_normalize_text(plain);
	conn = reader_lock(idx, &w);
	msg = find_fuzzy(conn, account, conv, time, sender, norm);
	reader_unlock(idx, w);
	g_free(norm);

	return msg;
}

/* Turns user text into a safe FTS5 query: each term becomes a quoted
 * string (inner quotes doubled); a trailing '*' becomes a prefix query.
 * Terms without any word character can't match a unicode61 token and
 * are dropped.  Returns NULL if nothing is left. */
static char *
build_fts_query(const char *query)
{
	GString *out = g_string_new(NULL);
	char **terms;
	int i;

	terms = g_strsplit_set(query, " \t\r\n", -1);
	for (i = 0; terms[i] != NULL; i++) {
		char *term = terms[i];
		gboolean prefix = FALSE;
		gboolean word = FALSE;
		size_t len = strlen(term);
		const char *p;

		while (len > 0 && term[len - 1] == '*') {
			prefix = TRUE;
			term[--len] = '\0';
		}
		if (!g_utf8_validate(term, -1, NULL))
			continue;
		for (p = term; *p != '\0'; p = g_utf8_next_char(p)) {
			gunichar c = g_utf8_get_char(p);
			if (g_unichar_isalnum(c) || g_unichar_ismark(c) || c >= 0x80) {
				word = TRUE;
				break;
			}
		}
		if (!word)
			continue;

		if (out->len > 0)
			g_string_append_c(out, ' ');
		g_string_append_c(out, '"');
		for (p = term; *p != '\0'; p++) {
			if (*p == '"')
				g_string_append_c(out, '"');
			g_string_append_c(out, *p);
		}
		g_string_append_c(out, '"');
		if (prefix)
			g_string_append_c(out, '*');
	}
	g_strfreev(terms);

	if (out->len == 0) {
		g_string_free(out, TRUE);
		return NULL;
	}
	return g_string_free(out, FALSE);
}

GPtrArray *
pidgin_message_index_search(PidginMessageIndex *idx, const char *query,
		const char *account, const char *conv, guint limit)
{
	IndexConn *conn;
	sqlite3_stmt *stmt;
	GPtrArray *result;
	char *fts;
	gboolean w;

	g_return_val_if_fail(PIDGIN_IS_MESSAGE_INDEX(idx), NULL);

	fts = query ? build_fts_query(query) : NULL;
	if (fts == NULL)
		return g_ptr_array_new_with_free_func(
				(GDestroyNotify)pidgin_indexed_message_free);

	conn = reader_lock(idx, &w);
	stmt = index_stmt(conn, "SELECT " MESSAGE_COLUMNS
			" FROM messages_fts f JOIN messages m ON m.id = f.rowid"
			" WHERE f.messages_fts MATCH ?1"
			" AND (?2 IS NULL OR m.account = ?2)"
			" AND (?3 IS NULL OR m.conv = ?3)"
			" ORDER BY m.time DESC, m.id DESC LIMIT ?4");
	if (stmt != NULL) {
		sqlite3_bind_text(stmt, 1, fts, -1, SQLITE_TRANSIENT);
		bind_text(stmt, 2, account);
		bind_text(stmt, 3, conv);
		sqlite3_bind_int(stmt, 4, limit ? limit : DEFAULT_LIMIT);
	}
	result = query_many(stmt);
	reader_unlock(idx, w);
	g_free(fts);

	return result;
}

GPtrArray *
pidgin_message_index_get_recent(PidginMessageIndex *idx, const char *account,
		const char *conv, gint64 before_time, guint limit)
{
	IndexConn *conn;
	sqlite3_stmt *stmt;
	GPtrArray *result;
	gboolean w;

	g_return_val_if_fail(PIDGIN_IS_MESSAGE_INDEX(idx), NULL);
	g_return_val_if_fail(account != NULL && conv != NULL, NULL);

	if (before_time <= 0)
		before_time = G_MAXINT64;

	conn = reader_lock(idx, &w);
	stmt = index_stmt(conn, "SELECT " MESSAGE_COLUMNS MESSAGE_FROM
			" WHERE m.account = ?1 AND m.conv = ?2 AND m.time < ?3"
			" ORDER BY m.time DESC, m.id DESC LIMIT ?4");
	if (stmt != NULL) {
		bind_text(stmt, 1, account);
		bind_text(stmt, 2, conv);
		sqlite3_bind_int64(stmt, 3, before_time);
		sqlite3_bind_int(stmt, 4, limit ? limit : DEFAULT_LIMIT);
	}
	result = query_many(stmt);
	reader_unlock(idx, w);

	return result;
}

/******************************************************************************
 * Reactions and receipts
 *****************************************************************************/

gboolean
pidgin_message_index_add_reaction(PidginMessageIndex *idx, gint64 msg,
		const char *emoji, const char *sender)
{
	IndexConn *conn;
	sqlite3_stmt *stmt;
	gboolean ok;

	g_return_val_if_fail(PIDGIN_IS_MESSAGE_INDEX(idx), FALSE);
	g_return_val_if_fail(emoji != NULL && sender != NULL, FALSE);

	conn = writer_lock(idx);
	stmt = index_stmt(conn,
			"INSERT OR IGNORE INTO reactions (msg, emoji, sender) VALUES (?1, ?2, ?3)");
	if ((ok = (stmt != NULL))) {
		sqlite3_bind_int64(stmt, 1, msg);
		bind_text(stmt, 2, emoji);
		bind_text(stmt, 3, sender);
		ok = stmt_step_done(conn, stmt);
	}
	writer_unlock(idx);

	return ok;
}

gboolean
pidgin_message_index_remove_reaction(PidginMessageIndex *idx, gint64 msg,
		const char *emoji, const char *sender)
{
	IndexConn *conn;
	sqlite3_stmt *stmt;
	gboolean ok;

	g_return_val_if_fail(PIDGIN_IS_MESSAGE_INDEX(idx), FALSE);
	g_return_val_if_fail(emoji != NULL && sender != NULL, FALSE);

	conn = writer_lock(idx);
	stmt = index_stmt(conn,
			"DELETE FROM reactions WHERE msg = ?1 AND emoji = ?2 AND sender = ?3");
	if ((ok = (stmt != NULL))) {
		sqlite3_bind_int64(stmt, 1, msg);
		bind_text(stmt, 2, emoji);
		bind_text(stmt, 3, sender);
		ok = stmt_step_done(conn, stmt);
	}
	writer_unlock(idx);

	return ok;
}

GHashTable *
pidgin_message_index_get_reactions(PidginMessageIndex *idx, gint64 msg)
{
	IndexConn *conn;
	sqlite3_stmt *stmt;
	GHashTable *table;
	gboolean w;

	g_return_val_if_fail(PIDGIN_IS_MESSAGE_INDEX(idx), NULL);

	table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
			(GDestroyNotify)g_ptr_array_unref);

	conn = reader_lock(idx, &w);
	stmt = index_stmt(conn,
			"SELECT emoji, sender FROM reactions WHERE msg = ?1 ORDER BY rowid");
	if (stmt != NULL) {
		sqlite3_bind_int64(stmt, 1, msg);
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *emoji = (const char *)sqlite3_column_text(stmt, 0);
			GPtrArray *senders = g_hash_table_lookup(table, emoji);

			if (senders == NULL) {
				senders = g_ptr_array_new_with_free_func(g_free);
				g_hash_table_insert(table, g_strdup(emoji), senders);
			}
			g_ptr_array_add(senders, column_text(stmt, 1));
		}
		stmt_done(stmt);
	}
	reader_unlock(idx, w);

	return table;
}

gboolean
pidgin_message_index_set_receipt(PidginMessageIndex *idx, gint64 msg,
		const char *state, const char *sender)
{
	IndexConn *conn;
	sqlite3_stmt *stmt;
	gboolean ok;

	g_return_val_if_fail(PIDGIN_IS_MESSAGE_INDEX(idx), FALSE);
	g_return_val_if_fail(state != NULL, FALSE);

	conn = writer_lock(idx);
	/* UNIQUE doesn't catch NULL senders, hence the NOT EXISTS. */
	stmt = index_stmt(conn,
			"INSERT OR IGNORE INTO receipts (msg, state, sender)"
			" SELECT ?1, ?2, ?3 WHERE NOT EXISTS (SELECT 1 FROM receipts"
			" WHERE msg = ?1 AND state = ?2 AND sender IS ?3)");
	if ((ok = (stmt != NULL))) {
		sqlite3_bind_int64(stmt, 1, msg);
		bind_text(stmt, 2, state);
		bind_text(stmt, 3, sender);
		ok = stmt_step_done(conn, stmt);
	}
	writer_unlock(idx);

	return ok;
}

char *
pidgin_message_index_get_receipt(PidginMessageIndex *idx, gint64 msg)
{
	IndexConn *conn;
	sqlite3_stmt *stmt;
	char *state = NULL;
	gboolean w;

	g_return_val_if_fail(PIDGIN_IS_MESSAGE_INDEX(idx), NULL);

	conn = reader_lock(idx, &w);
	stmt = index_stmt(conn,
			"SELECT state FROM receipts WHERE msg = ?1"
			" AND state IN ('displayed', 'delivered')"
			" ORDER BY state = 'displayed' DESC LIMIT 1");
	if (stmt != NULL) {
		sqlite3_bind_int64(stmt, 1, msg);
		if (sqlite3_step(stmt) == SQLITE_ROW)
			state = column_text(stmt, 0);
		stmt_done(stmt);
	}
	reader_unlock(idx, w);

	return state;
}

/******************************************************************************
 * Key/value store
 *****************************************************************************/

char *
pidgin_message_index_kv_get(PidginMessageIndex *idx, const char *account,
		const char *key)
{
	IndexConn *conn;
	sqlite3_stmt *stmt;
	char *value = NULL;
	gboolean w;

	g_return_val_if_fail(PIDGIN_IS_MESSAGE_INDEX(idx), NULL);
	g_return_val_if_fail(account != NULL && key != NULL, NULL);

	conn = reader_lock(idx, &w);
	stmt = index_stmt(conn, "SELECT value FROM kv WHERE account = ?1 AND key = ?2");
	if (stmt != NULL) {
		bind_text(stmt, 1, account);
		bind_text(stmt, 2, key);
		if (sqlite3_step(stmt) == SQLITE_ROW)
			value = column_text(stmt, 0);
		stmt_done(stmt);
	}
	reader_unlock(idx, w);

	return value;
}

gboolean
pidgin_message_index_kv_set(PidginMessageIndex *idx, const char *account,
		const char *key, const char *value)
{
	IndexConn *conn;
	sqlite3_stmt *stmt;
	gboolean ok;

	g_return_val_if_fail(PIDGIN_IS_MESSAGE_INDEX(idx), FALSE);
	g_return_val_if_fail(account != NULL && key != NULL, FALSE);

	conn = writer_lock(idx);
	if (value == NULL)
		stmt = index_stmt(conn, "DELETE FROM kv WHERE account = ?1 AND key = ?2");
	else
		stmt = index_stmt(conn,
				"INSERT OR REPLACE INTO kv (account, key, value) VALUES (?1, ?2, ?3)");
	if ((ok = (stmt != NULL))) {
		bind_text(stmt, 1, account);
		bind_text(stmt, 2, key);
		if (value != NULL)
			bind_text(stmt, 3, value);
		ok = stmt_step_done(conn, stmt);
	}
	writer_unlock(idx);

	return ok;
}

/******************************************************************************
 * Log positions and the backfill cursor
 *****************************************************************************/

gboolean
pidgin_message_index_mark_log_position(PidginMessageIndex *idx, gint64 msg,
		const char *log_file, gint64 offset)
{
	IndexConn *conn;
	sqlite3_stmt *stmt;
	gboolean ok;
	const char *name;

	g_return_val_if_fail(PIDGIN_IS_MESSAGE_INDEX(idx), FALSE);

	name = log_file ? strrchr(log_file, '/') : NULL;
	name = name ? name + 1 : log_file;

	conn = writer_lock(idx);
	/* The same compaction as log_file_to_column(), done against the row. */
	stmt = index_stmt(conn,
			"UPDATE messages SET log_file = CASE WHEN ?2 = account || '/' ||"
			" conv || CASE WHEN is_chat THEN '.chat' ELSE '' END || '/' || ?4"
			" THEN ?4 ELSE ?2 END, log_offset = ?3 WHERE id = ?1");
	if ((ok = (stmt != NULL))) {
		sqlite3_bind_int64(stmt, 1, msg);
		bind_text(stmt, 2, log_file);
		bind_text(stmt, 4, name);
		if (log_file != NULL && offset >= 0)
			sqlite3_bind_int64(stmt, 3, offset);
		else
			sqlite3_bind_null(stmt, 3);
		ok = stmt_step_done(conn, stmt) && sqlite3_changes(conn->db) > 0;
	}
	writer_unlock(idx);

	return ok;
}

gboolean
pidgin_message_index_get_file_state(PidginMessageIndex *idx, const char *path,
		gint64 *mtime, gint64 *size, gint64 *offset)
{
	IndexConn *conn;
	sqlite3_stmt *stmt;
	gboolean found = FALSE;
	gboolean w;

	g_return_val_if_fail(PIDGIN_IS_MESSAGE_INDEX(idx), FALSE);
	g_return_val_if_fail(path != NULL, FALSE);

	conn = reader_lock(idx, &w);
	stmt = index_stmt(conn,
			"SELECT mtime, size, offset FROM indexed_files WHERE path = ?1");
	if (stmt != NULL) {
		bind_text(stmt, 1, path);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			found = TRUE;
			if (mtime)
				*mtime = sqlite3_column_int64(stmt, 0);
			if (size)
				*size = sqlite3_column_int64(stmt, 1);
			if (offset)
				*offset = sqlite3_column_int64(stmt, 2);
		}
		stmt_done(stmt);
	}
	reader_unlock(idx, w);

	return found;
}

gboolean
pidgin_message_index_set_file_state(PidginMessageIndex *idx, const char *path,
		gint64 mtime, gint64 size, gint64 offset)
{
	IndexConn *conn;
	sqlite3_stmt *stmt;
	gboolean ok;

	g_return_val_if_fail(PIDGIN_IS_MESSAGE_INDEX(idx), FALSE);
	g_return_val_if_fail(path != NULL, FALSE);

	conn = writer_lock(idx);
	stmt = index_stmt(conn,
			"INSERT OR REPLACE INTO indexed_files (path, mtime, size, offset)"
			" VALUES (?1, ?2, ?3, ?4)");
	if ((ok = (stmt != NULL))) {
		bind_text(stmt, 1, path);
		sqlite3_bind_int64(stmt, 2, mtime);
		sqlite3_bind_int64(stmt, 3, size);
		sqlite3_bind_int64(stmt, 4, offset);
		ok = stmt_step_done(conn, stmt);
	}
	writer_unlock(idx);

	return ok;
}

gboolean
pidgin_message_index_forget_file(PidginMessageIndex *idx, const char *path)
{
	IndexConn *conn;
	sqlite3_stmt *stmt;
	gboolean ok = TRUE;
	gboolean own_tx;
	char *account, *conv;
	gboolean is_chat;
	const char *name;

	g_return_val_if_fail(PIDGIN_IS_MESSAGE_INDEX(idx), FALSE);
	g_return_val_if_fail(path != NULL, FALSE);

	name = split_log_path(path, &account, &conv, &is_chat);
	conn = writer_lock(idx);
	own_tx = (idx->batch_thread == NULL);
	if (own_tx)
		index_exec(conn, "SAVEPOINT pidgin_forget", NULL);

	stmt = index_stmt(conn,
			"UPDATE messages SET log_file = NULL, log_offset = NULL"
			" WHERE account = ?1 AND conv = ?2 AND is_chat = ?4 AND log_file = ?3"
			" AND (stanza_id IS NOT NULL OR origin_id IS NOT NULL"
			" OR server_id IS NOT NULL)");
	if (stmt != NULL) {
		bind_text(stmt, 1, account);
		bind_text(stmt, 2, conv);
		bind_text(stmt, 3, name);
		sqlite3_bind_int(stmt, 4, is_chat);
		ok = stmt_step_done(conn, stmt) && ok;
	}
	stmt = index_stmt(conn, "DELETE FROM messages WHERE account = ?1"
			" AND conv = ?2 AND is_chat = ?4 AND log_file = ?3");
	if (stmt != NULL) {
		bind_text(stmt, 1, account);
		bind_text(stmt, 2, conv);
		bind_text(stmt, 3, name);
		sqlite3_bind_int(stmt, 4, is_chat);
		ok = stmt_step_done(conn, stmt) && ok;
	}
	g_free(account);
	g_free(conv);
	stmt = index_stmt(conn, "DELETE FROM indexed_files WHERE path = ?1");
	if (stmt != NULL) {
		bind_text(stmt, 1, path);
		ok = stmt_step_done(conn, stmt) && ok;
	}

	if (own_tx)
		index_exec(conn, "RELEASE pidgin_forget", NULL);
	writer_unlock(idx);

	return ok;
}

/******************************************************************************
 * Batches
 *****************************************************************************/

void
pidgin_message_index_begin(PidginMessageIndex *idx)
{
	IndexConn *conn;

	g_return_if_fail(PIDGIN_IS_MESSAGE_INDEX(idx));

	conn = writer_lock(idx);
	g_warn_if_fail(idx->batch_thread == NULL);
	if (!index_exec(conn, "BEGIN IMMEDIATE", NULL))
		g_warning("message index: BEGIN failed: %s", sqlite3_errmsg(conn->db));
	g_atomic_pointer_set(&idx->batch_thread, g_thread_self());
}

gboolean
pidgin_message_index_commit(PidginMessageIndex *idx)
{
	GError *error = NULL;
	gboolean ok;

	g_return_val_if_fail(PIDGIN_IS_MESSAGE_INDEX(idx), FALSE);
	g_return_val_if_fail(idx->batch_thread == g_thread_self(), FALSE);

	ok = index_exec(&idx->writer, "COMMIT", &error);
	if (!ok) {
		g_warning("message index: %s", error->message);
		g_error_free(error);
		index_exec(&idx->writer, "ROLLBACK", NULL);
	}
	g_atomic_pointer_set(&idx->batch_thread, NULL);
	writer_unlock(idx);

	return ok;
}

gint64
pidgin_message_index_count(PidginMessageIndex *idx)
{
	IndexConn *conn;
	sqlite3_stmt *stmt;
	gint64 count = 0;
	gboolean w;

	g_return_val_if_fail(PIDGIN_IS_MESSAGE_INDEX(idx), 0);

	conn = reader_lock(idx, &w);
	stmt = index_stmt(conn, "SELECT count(*) FROM messages");
	if (stmt != NULL) {
		if (sqlite3_step(stmt) == SQLITE_ROW)
			count = sqlite3_column_int64(stmt, 0);
		stmt_done(stmt);
	}
	reader_unlock(idx, w);

	return count;
}

/******************************************************************************
 * Private helpers for the backfill
 *****************************************************************************/

gboolean
pidgin_message_index_has_unlogged(PidginMessageIndex *idx)
{
	IndexConn *conn;
	sqlite3_stmt *stmt;
	gboolean found = FALSE;
	gboolean w;

	conn = reader_lock(idx, &w);
	stmt = index_stmt(conn,
			"SELECT 1 FROM messages WHERE log_file IS NULL LIMIT 1");
	if (stmt != NULL) {
		found = (sqlite3_step(stmt) == SQLITE_ROW);
		stmt_done(stmt);
	}
	reader_unlock(idx, w);

	return found;
}

gint64
pidgin_message_index_last_time_for_file(PidginMessageIndex *idx,
		const char *log_file, gboolean *senders)
{
	IndexConn *conn;
	sqlite3_stmt *stmt;
	gint64 time = 0;
	gboolean w;
	char *account, *conv;
	gboolean is_chat;
	const char *name;

	if (senders != NULL)
		*senders = FALSE;
	name = split_log_path(log_file, &account, &conv, &is_chat);
	conn = reader_lock(idx, &w);
	stmt = index_stmt(conn,
			"SELECT max(CASE WHEN sender IS NOT NULL THEN time END), max(time)"
			" FROM messages WHERE account = ?1 AND conv = ?2"
			" AND is_chat = ?4 AND log_file = ?3");
	if (stmt != NULL) {
		bind_text(stmt, 1, account);
		bind_text(stmt, 2, conv);
		bind_text(stmt, 3, name);
		sqlite3_bind_int(stmt, 4, is_chat);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			gboolean with_sender = sqlite3_column_type(stmt, 0) != SQLITE_NULL;

			time = sqlite3_column_int64(stmt, with_sender ? 0 : 1);
			if (senders != NULL)
				*senders = with_sender;
		}
		stmt_done(stmt);
	}
	reader_unlock(idx, w);
	g_free(account);
	g_free(conv);

	return time;
}

gboolean
pidgin_message_index_has_log_position(PidginMessageIndex *idx,
		const PidginIndexedMessage *msg)
{
	IndexConn *conn;
	sqlite3_stmt *stmt;
	gboolean found = FALSE;
	gboolean w;

	/* A live row linked to the line may carry a slightly different time. */
	conn = reader_lock(idx, &w);
	stmt = index_stmt(conn,
			"SELECT 1 FROM messages WHERE account = ?1 AND conv = ?2"
			" AND time BETWEEN ?3 - " G_STRINGIFY(FUZZY_WINDOW)
			" AND ?3 + " G_STRINGIFY(FUZZY_WINDOW)
			" AND log_file = ?4 AND log_offset = ?5");
	if (stmt != NULL) {
		bind_text(stmt, 1, msg->account);
		bind_text(stmt, 2, msg->conv);
		sqlite3_bind_int64(stmt, 3, msg->time);
		bind_text(stmt, 4, log_file_to_column(msg));
		sqlite3_bind_int64(stmt, 5, msg->log_offset);
		found = (sqlite3_step(stmt) == SQLITE_ROW);
		stmt_done(stmt);
	}
	reader_unlock(idx, w);

	return found;
}

gint64
pidgin_message_index_find_unlogged(PidginMessageIndex *idx,
		const PidginIndexedMessage *msg)
{
	IndexConn *conn;
	sqlite3_stmt *stmt;
	gint64 id = 0;
	char *norm;
	gboolean w;

	norm = pidgin_message_index_normalize_text(msg->body ? msg->body : "");
	conn = reader_lock(idx, &w);
	stmt = index_stmt(conn,
			"SELECT m.id FROM messages m JOIN messages_fts f ON f.rowid = m.id"
			" WHERE m.account = ?1 AND m.conv = ?2"
			" AND m.time BETWEEN ?3 - " G_STRINGIFY(FUZZY_WINDOW)
			" AND ?3 + " G_STRINGIFY(FUZZY_WINDOW)
			" AND m.log_file IS NULL AND f.body = ?4"
			" ORDER BY abs(m.time - ?3) LIMIT 1");
	if (stmt != NULL) {
		bind_text(stmt, 1, msg->account);
		bind_text(stmt, 2, msg->conv);
		sqlite3_bind_int64(stmt, 3, msg->time);
		sqlite3_bind_text(stmt, 4, norm, -1, SQLITE_STATIC);
		if (sqlite3_step(stmt) == SQLITE_ROW)
			id = sqlite3_column_int64(stmt, 0);
		stmt_done(stmt);
	}
	reader_unlock(idx, w);
	g_free(norm);

	return id;
}

/******************************************************************************
 * Date repair
 *****************************************************************************/

/* The backfill's clock never dates a line more than a day after its file's
 * mtime, nor at the epoch (pidginbackfill.c). */
#define REPAIR_SLACK (24 * 60 * 60)
#define REPAIR_CHUNK 500     /* rows per transaction: live writes wait no longer */

/* The files with a row after their recorded mtime (+ a day) or at the
 * epoch.  One pass over the table, grouped by file. */
static const char repair_files_sql[] =
	"SELECT g.account, g.conv, g.is_chat, g.log_file, g.path, g.maxt, g.mint"
	" FROM (SELECT account, conv, is_chat, log_file,"
	"  CASE WHEN instr(log_file, '/') > 0 THEN log_file"
	"  ELSE account || '/' || conv || CASE WHEN is_chat THEN '.chat' ELSE '' END"
	"  || '/' || log_file END AS path,"
	"  max(time) AS maxt, min(time) AS mint"
	"  FROM messages WHERE log_file IS NOT NULL"
	"  GROUP BY account, conv, is_chat, log_file) g"
	" JOIN indexed_files f ON f.path = g.path"
	" WHERE g.mint <= ?1 OR g.maxt > f.mtime + ?1";

/* Rows with nothing but the log line (reactions and receipts are checked
 * when deleting). */
static const char repair_rows_sql[] =
	"SELECT id, account, conv, is_chat, log_file FROM messages"
	" WHERE log_file IS NOT NULL AND stanza_id IS NULL AND origin_id IS NULL"
	" AND server_id IS NULL AND occupant_id IS NULL AND correction_of IS NULL"
	" AND reply_to IS NULL";

static char *
repair_key(const char *account, const char *conv, int is_chat, const char *log_file)
{
	return g_strdup_printf("%s\n%s\n%d\n%s", account, conv, is_chat, log_file);
}

int
pidgin_message_index_repair_misdated(PidginMessageIndex *idx, const char *logs_dir,
		GCancellable *cancellable)
{
	IndexConn scan = { 0 };
	IndexConn *conn;
	GError *error = NULL;
	GHashTable *files, *names;
	GPtrArray *paths;
	GArray *ids;
	sqlite3_stmt *stmt;
	guint i;
	int rc;

	g_return_val_if_fail(PIDGIN_IS_MESSAGE_INDEX(idx), -1);
	g_return_val_if_fail(logs_dir != NULL, -1);

	/* Its own connection: the scans take seconds on a big index, and the
	 * shared reader would keep the UI waiting that long. */
	if (!index_conn_open(&scan, idx->path, TRUE, &error)) {
		g_warning("message index: date repair: %s", error->message);
		g_clear_error(&error);
		return -1;
	}

	files = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	paths = g_ptr_array_new_with_free_func(g_free);
	ids = g_array_new(FALSE, FALSE, sizeof(gint64));

	stmt = index_stmt(&scan, repair_files_sql);
	if (stmt == NULL)
		goto failed;
	sqlite3_bind_int64(stmt, 1, REPAIR_SLACK);
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		const char *path = (const char *)sqlite3_column_text(stmt, 4);
		gint64 maxt = sqlite3_column_int64(stmt, 5);
		gint64 mint = sqlite3_column_int64(stmt, 6);
		char *abs = g_build_filename(logs_dir, path, NULL);
		GStatBuf st;

		/* By the file's mtime now (the recorded one can be older than a
		 * live row linked since); a file that is gone can't be read again. */
		if (g_stat(abs, &st) == 0 && S_ISREG(st.st_mode) &&
		    (mint <= REPAIR_SLACK || maxt > (gint64)st.st_mtime + REPAIR_SLACK)) {
			g_hash_table_add(files, repair_key(
					(const char *)sqlite3_column_text(stmt, 0),
					(const char *)sqlite3_column_text(stmt, 1),
					sqlite3_column_int(stmt, 2),
					(const char *)sqlite3_column_text(stmt, 3)));
			g_hash_table_add(names, g_strdup((const char *)sqlite3_column_text(stmt, 3)));
			g_ptr_array_add(paths, g_strdup(path));
		}
		g_free(abs);
	}
	stmt_done(stmt);
	if (rc != SQLITE_DONE)
		goto failed;

	if (paths->len > 0) {
		stmt = index_stmt(&scan, repair_rows_sql);
		if (stmt == NULL)
			goto failed;
		while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
			const char *log_file = (const char *)sqlite3_column_text(stmt, 4);
			char *key;

			if (!g_hash_table_contains(names, log_file))
				continue;
			key = repair_key((const char *)sqlite3_column_text(stmt, 1),
					(const char *)sqlite3_column_text(stmt, 2),
					sqlite3_column_int(stmt, 3), log_file);
			if (g_hash_table_contains(files, key)) {
				gint64 id = sqlite3_column_int64(stmt, 0);

				g_array_append_val(ids, id);
			}
			g_free(key);
		}
		stmt_done(stmt);
		if (rc != SQLITE_DONE)
			goto failed;
	}
	index_conn_close(&scan);

	/* The files first: if this stops half way, the next backfill reads
	 * them again (and the next repair finds what is left). */
	conn = writer_lock(idx);
	index_exec(conn, "SAVEPOINT pidgin_repair", NULL);
	for (i = 0; i < paths->len; i++) {
		stmt = index_stmt(conn, "DELETE FROM indexed_files WHERE path = ?1");
		if (stmt != NULL) {
			bind_text(stmt, 1, paths->pdata[i]);
			stmt_step_done(conn, stmt);
		}
	}
	index_exec(conn, "RELEASE pidgin_repair", NULL);
	writer_unlock(idx);

	for (i = 0; i < ids->len; i++) {
		if (i % REPAIR_CHUNK == 0) {
			if (i > 0) {
				index_exec(conn, "RELEASE pidgin_repair", NULL);
				writer_unlock(idx);
			}
			if (g_cancellable_is_cancelled(cancellable))
				goto cancelled;
			conn = writer_lock(idx);
			index_exec(conn, "SAVEPOINT pidgin_repair", NULL);
		}
		stmt = index_stmt(conn,
				"DELETE FROM messages WHERE id = ?1 AND stanza_id IS NULL"
				" AND origin_id IS NULL AND server_id IS NULL AND occupant_id IS NULL"
				" AND correction_of IS NULL AND reply_to IS NULL"
				" AND NOT EXISTS (SELECT 1 FROM reactions WHERE msg = ?1)"
				" AND NOT EXISTS (SELECT 1 FROM receipts WHERE msg = ?1)");
		if (stmt != NULL) {
			sqlite3_bind_int64(stmt, 1, g_array_index(ids, gint64, i));
			stmt_step_done(conn, stmt);
		}
	}
	if (ids->len > 0) {
		index_exec(conn, "RELEASE pidgin_repair", NULL);
		writer_unlock(idx);
	}

	g_debug("message index: date repair: %u files, %u rows to index again",
			paths->len, ids->len);
	rc = paths->len;
	goto done;

failed:
	g_warning("message index: date repair: %s", sqlite3_errmsg(scan.db));
	stmt_done(stmt);
	index_conn_close(&scan);
cancelled:
	rc = -1;
done:
	g_hash_table_destroy(files);
	g_hash_table_destroy(names);
	g_ptr_array_unref(paths);
	g_array_unref(ids);
	return rc;
}

/******************************************************************************
 * jabber key/value signals
 *****************************************************************************/

static gpointer
kv_load_cb(PurpleAccount *account, const char *key, PidginMessageIndex *idx)
{
	char *acct;

	if (account == NULL || key == NULL)
		return NULL;

	acct = pidgin_message_index_account_key(account);
	/* The prpl copies the answer at once; keep it alive until the next. */
	g_free(idx->kv_last);
	idx->kv_last = pidgin_message_index_kv_get(idx, acct, key);
	g_free(acct);

	return idx->kv_last;
}

static void
kv_store_cb(PurpleAccount *account, const char *key, const char *value,
		PidginMessageIndex *idx)
{
	char *acct;

	if (account == NULL || key == NULL)
		return;

	acct = pidgin_message_index_account_key(account);
	pidgin_message_index_kv_set(idx, acct, key, value);
	g_free(acct);
}

void
pidgin_message_index_connect_kv_signals(PidginMessageIndex *idx, void *handle)
{
	g_return_if_fail(PIDGIN_IS_MESSAGE_INDEX(idx));
	g_return_if_fail(handle != NULL);

	if (idx->kv_handle == handle)
		return;

	purple_signal_connect(handle, "jabber-kv-load", idx,
			PURPLE_CALLBACK(kv_load_cb), idx);
	purple_signal_connect(handle, "jabber-kv-store", idx,
			PURPLE_CALLBACK(kv_store_cb), idx);
	idx->kv_handle = handle;
}

/******************************************************************************
 * UI glue
 *****************************************************************************/

static int ui_handle;
static gboolean backfill_started = FALSE;

static gboolean
is_jabber(PurplePlugin *plugin)
{
	return plugin != NULL && plugin->info != NULL &&
		purple_strequal(plugin->info->id, "prpl-jabber");
}

static void
plugin_load_cb(PurplePlugin *plugin, gpointer data)
{
	PidginMessageIndex *idx = pidgin_message_index_get_default();

	if (idx != NULL && is_jabber(plugin))
		pidgin_message_index_connect_kv_signals(idx, plugin);
}

static void
plugin_unload_cb(PurplePlugin *plugin, gpointer data)
{
	/* libpurple drops the plugin's signals and their handlers. */
	if (default_index != NULL && is_jabber(plugin) &&
	    default_index->kv_handle == plugin)
		default_index->kv_handle = NULL;
}

static void
signed_on_cb(PurpleConnection *gc, gpointer data)
{
	PidginBackfill *bf;

	if (backfill_started)
		return;
	backfill_started = TRUE;

	if (purple_strequal(g_getenv("PIDGIN4_NO_BACKFILL"), "1") ||
	    !purple_prefs_get_bool("/pidgin4/index/backfill"))
		return;

	bf = pidgin_backfill_get_default();
	if (bf != NULL)
		pidgin_backfill_start(bf);
}

void
pidgin_message_index_ui_init(void)
{
	PidginMessageIndex *idx;
	PurplePlugin *jabber;

	if (!purple_prefs_exists("/pidgin4"))
		purple_prefs_add_none("/pidgin4");
	purple_prefs_add_none("/pidgin4/index");
	purple_prefs_add_bool("/pidgin4/index/backfill", TRUE);

	idx = pidgin_message_index_get_default();
	if (idx == NULL)
		return;

	purple_signal_connect(purple_plugins_get_handle(), "plugin-load",
			&ui_handle, PURPLE_CALLBACK(plugin_load_cb), NULL);
	purple_signal_connect(purple_plugins_get_handle(), "plugin-unload",
			&ui_handle, PURPLE_CALLBACK(plugin_unload_cb), NULL);

	jabber = purple_plugins_find_with_id("prpl-jabber");
	if (jabber != NULL && purple_plugin_is_loaded(jabber))
		pidgin_message_index_connect_kv_signals(idx, jabber);

	purple_signal_connect(purple_connections_get_handle(), "signed-on",
			&ui_handle, PURPLE_CALLBACK(signed_on_cb), NULL);
}

void
pidgin_message_index_ui_uninit(void)
{
	/* Stops and joins the backfill thread before the index goes. */
	pidgin_backfill_shutdown_default();

	purple_signals_disconnect_by_handle(&ui_handle);
	backfill_started = FALSE;

	if (default_index != NULL) {
		purple_signals_disconnect_by_handle(default_index);
		default_index->kv_handle = NULL;
		g_clear_object(&default_index);
	}
}
