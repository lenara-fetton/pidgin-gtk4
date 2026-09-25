/**
 * @file pidginmessageindex.h The pidgin4 message index
 * @ingroup pidgin
 */

/* pidgin4
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
#ifndef _PIDGIN_MESSAGE_INDEX_H_
#define _PIDGIN_MESSAGE_INDEX_H_

#include <glib-object.h>

#include "account.h"

/*
 * PidginMessageIndex is a SQLite store, <profile>/pidgin4/messages.db,
 * kept alongside libpurple's HTML logs (which stay the source of truth
 * and are never modified by it).  Per message it holds the account,
 * conversation, time, sender, the XMPP stanza/origin/server ids, the
 * position of the matching log line, and the plain text in an FTS5 table.
 * It also keeps reactions, receipts, a small per-account key/value store
 * (for the jabber prpl's "jabber-kv-*" signals) and the backfill cursor.
 *
 * Threading: all writes go through one connection guarded by a recursive
 * lock.  pidgin_message_index_begin() takes that lock for a whole batch
 * (the backfill thread uses it per batch); other threads' writes wait for
 * the batch to commit.  Reads use a separate read-only connection so they
 * don't queue behind a batch, except in the thread that owns the running
 * batch, whose reads go through the write connection and so see its own
 * uncommitted rows.
 */

G_BEGIN_DECLS

#define PIDGIN_TYPE_MESSAGE_INDEX (pidgin_message_index_get_type())
G_DECLARE_FINAL_TYPE(PidginMessageIndex, pidgin_message_index, PIDGIN, MESSAGE_INDEX, GObject)

#define PIDGIN_TYPE_INDEXED_MESSAGE (pidgin_indexed_message_get_type())

/** The schema version stored in PRAGMA user_version. */
#define PIDGIN_MESSAGE_INDEX_SCHEMA_VERSION 1

/**
 * One row of the index.  Strings are owned by the struct.
 */
typedef struct {
	gint64 id;            /**< Row id; 0 = not stored. */
	char *account;        /**< pidgin_message_index_account_key(). */
	char *conv;           /**< pidgin_message_index_conv_key(). */
	gboolean is_chat;
	gint64 time;          /**< Seconds since the epoch. */
	char *sender;
	char *body;           /**< Plain text, what FTS indexes (whitespace is
	                           normalized when stored). */
	char *stanza_id;
	char *origin_id;
	char *server_id;
	char *occupant_id;
	char *correction_of;
	char *reply_to;
	char *log_file;       /**< Relative to <profile>/logs, or NULL. */
	gint64 log_offset;    /**< Byte offset of the log line, -1 if unknown. */
	guint flags;          /**< PurpleMessageFlags. */
} PidginIndexedMessage;

/** The boxed type of #PidginIndexedMessage. */
GType pidgin_indexed_message_get_type(void);

/** Returns a new, empty message (id 0, log_offset -1). */
PidginIndexedMessage *pidgin_indexed_message_new(void);

/** Deep-copies @a msg. */
PidginIndexedMessage *pidgin_indexed_message_copy(const PidginIndexedMessage *msg);

/** Frees @a msg and its strings.  NULL is allowed. */
void pidgin_indexed_message_free(PidginIndexedMessage *msg);

/**
 * Opens (creating if needed) the index at @a path.  The parent directory
 * must exist.
 *
 * @return A new reference, or NULL with @a error set.
 */
PidginMessageIndex *pidgin_message_index_open(const char *path, GError **error);

/**
 * Returns the index at <purple_user_dir()>/pidgin4/messages.db, opening
 * it (and creating the directory 0700) on first use.  Returns NULL, after
 * logging a debug error, if it can't be opened.  The index is owned by
 * this module; don't unref it.  Main thread only.
 */
PidginMessageIndex *pidgin_message_index_get_default(void);

/** Returns the database file path of @a idx. */
const char *pidgin_message_index_get_path(PidginMessageIndex *idx);

/**
 * Returns the account key: "<prpl list_icon>/<escaped normalized username>",
 * the same as the account's HTML log directory relative to <profile>/logs,
 * e.g. "jabber/user@example.com".
 */
char *pidgin_message_index_account_key(PurpleAccount *account);

/**
 * Returns the conversation key: purple_escape_filename(purple_normalize(
 * account, name)), the log directory name without ".chat".
 */
char *pidgin_message_index_conv_key(PurpleAccount *account, const char *name);

/**
 * Returns @a text with runs of whitespace collapsed to one space and the
 * ends trimmed: the form the index stores and compares.
 */
char *pidgin_message_index_normalize_text(const char *text);

/**
 * Inserts @a msg (its id is ignored).
 *
 * @return The new row id, or 0 with @a error set.
 */
gint64 pidgin_message_index_insert(PidginMessageIndex *idx,
		const PidginIndexedMessage *msg, GError **error);

/**
 * Sets ids learned after the message was stored (e.g. from
 * "sending-message-meta" or the server's echo).  NULL arguments leave the
 * column unchanged.
 */
gboolean pidgin_message_index_set_ids(PidginMessageIndex *idx, gint64 id,
		const char *stanza_id, const char *origin_id, const char *server_id);

/** Deletes a row, with its text, reactions and receipts. */
gboolean pidgin_message_index_delete(PidginMessageIndex *idx, gint64 id);

/** Returns row @a id (body filled), or NULL. */
PidginIndexedMessage *pidgin_message_index_get(PidginMessageIndex *idx, gint64 id);

/**
 * Returns the newest message of @a account/@a conv whose stanza, origin or
 * server id equals @a id, or NULL.
 */
PidginIndexedMessage *pidgin_message_index_find_by_id(PidginMessageIndex *idx,
		const char *account, const char *conv, const char *id);

/**
 * For lines without ids (e.g. written by Pidgin 2): returns the message of
 * @a account/@a conv within 120 seconds of @a time, from @a sender (NULL =
 * any), whose normalized text equals the normalized @a plain; the closest
 * in time wins.  NULL if none.
 */
PidginIndexedMessage *pidgin_message_index_find_fuzzy(PidginMessageIndex *idx,
		const char *account, const char *conv, gint64 time,
		const char *sender, const char *plain);

/**
 * Full-text search.  @a query is user text: every whitespace-separated
 * term is quoted (so no input is an FTS syntax error) and all terms must
 * match; a term ending in '*' is a prefix search.  @a account and
 * @a conv may be NULL for "any".  Newest first, at most @a limit (0 = 100).
 *
 * @return A GPtrArray of PidginIndexedMessage* (frees its elements).
 */
GPtrArray *pidgin_message_index_search(PidginMessageIndex *idx,
		const char *query, const char *account, const char *conv, guint limit);

/**
 * Returns the messages of one conversation older than @a before_time
 * (0 = now), newest first, at most @a limit (0 = 100).  Same array type
 * as pidgin_message_index_search().
 */
GPtrArray *pidgin_message_index_get_recent(PidginMessageIndex *idx,
		const char *account, const char *conv, gint64 before_time, guint limit);

/** Adds a reaction; adding an existing one is a successful no-op. */
gboolean pidgin_message_index_add_reaction(PidginMessageIndex *idx, gint64 msg,
		const char *emoji, const char *sender);

/** Removes a reaction. */
gboolean pidgin_message_index_remove_reaction(PidginMessageIndex *idx, gint64 msg,
		const char *emoji, const char *sender);

/**
 * Returns the reactions of @a msg: emoji (char *) -> GPtrArray of senders
 * (char *), both in insertion order.  Never NULL; free with
 * g_hash_table_unref().
 */
GHashTable *pidgin_message_index_get_reactions(PidginMessageIndex *idx, gint64 msg);

/**
 * Records a receipt/marker @a state ("delivered", "displayed", ...) of
 * @a msg from @a sender (may be NULL).
 */
gboolean pidgin_message_index_set_receipt(PidginMessageIndex *idx, gint64 msg,
		const char *state, const char *sender);

/**
 * Returns the strongest receipt of @a msg: "displayed" > "delivered", or
 * NULL if none.
 */
char *pidgin_message_index_get_receipt(PidginMessageIndex *idx, gint64 msg);

/** Returns the value of @a key for @a account (an account key), or NULL. */
char *pidgin_message_index_kv_get(PidginMessageIndex *idx, const char *account,
		const char *key);

/** Stores @a value under @a key for @a account; NULL deletes the key. */
gboolean pidgin_message_index_kv_set(PidginMessageIndex *idx, const char *account,
		const char *key, const char *value);

/**
 * Records where message @a msg was written in the HTML logs.  @a log_file
 * is relative to <profile>/logs and has the log layout
 * "<prpl>/<account>/<conv>[.chat]/<file>" (lookups by file rely on its
 * first parts matching the row's account and conv keys).
 */
gboolean pidgin_message_index_mark_log_position(PidginMessageIndex *idx, gint64 msg,
		const char *log_file, gint64 offset);

/**
 * Reads the backfill cursor of @a path (relative to <profile>/logs).
 *
 * @return FALSE if the file has no state.
 */
gboolean pidgin_message_index_get_file_state(PidginMessageIndex *idx,
		const char *path, gint64 *mtime, gint64 *size, gint64 *offset);

/** Stores the backfill cursor of @a path. */
gboolean pidgin_message_index_set_file_state(PidginMessageIndex *idx,
		const char *path, gint64 mtime, gint64 size, gint64 offset);

/**
 * Forgets a log file: deletes its rows and its cursor.  Rows that carry
 * an XMPP id (live messages whose position had been recorded) are kept
 * and only unlinked from the file, so a reindex can link them again.
 */
gboolean pidgin_message_index_forget_file(PidginMessageIndex *idx, const char *path);

/**
 * Starts a batch: takes the writer lock and BEGINs a transaction.  Every
 * begin must be paired with pidgin_message_index_commit() in the same
 * thread.  Nested batches are not supported.
 */
void pidgin_message_index_begin(PidginMessageIndex *idx);

/** COMMITs the batch and releases the writer lock. */
gboolean pidgin_message_index_commit(PidginMessageIndex *idx);

/** Returns the number of rows in messages. */
gint64 pidgin_message_index_count(PidginMessageIndex *idx);

/**
 * Connects @a idx to the "jabber-kv-load" / "jabber-kv-store" signals
 * registered on @a handle (normally the prpl-jabber plugin).  Exposed for
 * the unit tests; pidgin_message_index_ui_init() does this itself.
 */
void pidgin_message_index_connect_kv_signals(PidginMessageIndex *idx, void *handle);

/**
 * UI glue: registers the /pidgin4/index prefs, serves the jabber prpl's
 * key/value signals from the default index (now or whenever prpl-jabber
 * gets loaded), and starts the log backfill on the first sign-on unless
 * PIDGIN4_NO_BACKFILL=1 or /pidgin4/index/backfill is FALSE.
 */
void pidgin_message_index_ui_init(void);

/** Stops the backfill, disconnects the signals and closes the default index. */
void pidgin_message_index_ui_uninit(void);

/*
 * Private helpers for PidginBackfill; not for other callers.
 */

/**
 * TRUE if a row of @a msg's account and conversation, within 120 seconds
 * of its time, already records its log_file and log_offset.
 */
gboolean pidgin_message_index_has_log_position(PidginMessageIndex *idx,
		const PidginIndexedMessage *msg);

/**
 * Returns the id of a row that has no log position yet (a live message)
 * and matches @a msg's account, conversation, text and time (within 120
 * seconds), or 0.
 */
gint64 pidgin_message_index_find_unlogged(PidginMessageIndex *idx,
		const PidginIndexedMessage *msg);

/** TRUE if any row has no log position yet. */
gboolean pidgin_message_index_has_unlogged(PidginMessageIndex *idx);

/**
 * The newest time of the rows of @a log_file that have a sender, or of any
 * of its rows when none has one (then *@a senders is FALSE); 0 if it has
 * no rows.
 */
gint64 pidgin_message_index_last_time_for_file(PidginMessageIndex *idx,
		const char *log_file, gboolean *senders);

/**
 * Undoes the dates earlier backfills got wrong: log files (under
 * @a logs_dir) with a row dated more than a day after the file's mtime, or
 * at the Unix epoch, lose their rows that hold nothing but the log line (no
 * stanza, origin, server or occupant id, no correction or reply, no
 * reactions or receipts) and their indexed_files entry, so the backfill
 * indexes them again.  Rows with live data stay, linked to their lines.
 * Files that are gone are left alone.  Scans on its own read connection,
 * and deletes in short transactions.
 *
 * @return the number of files, or -1 if cancelled or failed.
 */
int pidgin_message_index_repair_misdated(PidginMessageIndex *idx, const char *logs_dir,
		GCancellable *cancellable);

G_END_DECLS

#endif /* _PIDGIN_MESSAGE_INDEX_H_ */
