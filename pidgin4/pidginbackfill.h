/**
 * @file pidginbackfill.h Background indexer of the existing HTML/text logs
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
#ifndef _PIDGIN_BACKFILL_H_
#define _PIDGIN_BACKFILL_H_

#include <gio/gio.h>

#include "pidginmessageindex.h"

/*
 * PidginBackfill walks <profile>/logs/<prpl>/<account>/<conv>[.chat]/ and
 * feeds every message line of the .html and .txt logs into a
 * PidginMessageIndex (.system directories are skipped).  It streams each
 * file line by line, commits in batches, sleeps "throttle-ms" between
 * batches, can be paused and cancelled, and resumes through the index's
 * indexed_files cursor: an unchanged file is skipped, a file that grew is
 * read from where the last pass stopped, and a file that shrank is
 * forgotten and indexed again.  Lines already linked to a row (by log file
 * and offset) are skipped, and a live row that has no log position yet is
 * linked to its line instead of being duplicated.
 *
 * Before its first run over an index it repairs the dates earlier builds
 * got wrong (pidgin_message_index_repair_misdated()); the kv key
 * PIDGIN_BACKFILL_DATE_REPAIR_KEY (account "") records that it did.
 *
 * Signals (emitted in the thread-default main context of the caller of
 * pidgin_backfill_start(), or synchronously by pidgin_backfill_run_sync()):
 *   "progress" (guint files_done, guint files_total,
 *               guint64 bytes_done, guint64 bytes_total), at most ~4/s
 *               plus once at the end;
 *   "finished" (gboolean completed): FALSE when cancelled or failed.
 */

G_BEGIN_DECLS

/** The index's kv key (account "") set once the date repair has run. */
#define PIDGIN_BACKFILL_DATE_REPAIR_KEY "backfill-date-repair-1"

#define PIDGIN_TYPE_BACKFILL (pidgin_backfill_get_type())
G_DECLARE_FINAL_TYPE(PidginBackfill, pidgin_backfill, PIDGIN, BACKFILL, GObject)

/** Creates a backfill of @a logs_dir into @a idx. */
PidginBackfill *pidgin_backfill_new(PidginMessageIndex *idx, const char *logs_dir);

/** Runs the backfill in a worker thread.  No-op if it is running. */
void pidgin_backfill_start(PidginBackfill *bf);

/** Pauses the worker at its next check (between lines). */
void pidgin_backfill_pause(PidginBackfill *bf);

/** Resumes a paused worker. */
void pidgin_backfill_resume(PidginBackfill *bf);

/** Cancels a running backfill (and clears a pause); "finished" follows with FALSE. */
void pidgin_backfill_cancel(PidginBackfill *bf);

/** TRUE between pidgin_backfill_start() and "finished". */
gboolean pidgin_backfill_is_running(PidginBackfill *bf);

/**
 * Runs the whole backfill in the calling thread.  For tests and tools.
 *
 * @return TRUE if it ran to completion; FALSE with @a error set if it was
 *         cancelled or failed.
 */
gboolean pidgin_backfill_run_sync(PidginBackfill *bf, GCancellable *cancellable,
		GError **error);

/*
 * PidginLogClock dates the lines of one log file.  Most stamps are a time
 * of day only ("(10:45:36 AM)"); their day comes from a cursor, the newest
 * message line so far, starting at the file's start:
 *
 *  - A message line (one with a sender) more than an hour before the
 *    cursor is on the next day (midnight passed), unless that would date
 *    it more than a day after the file's mtime: then it is an older line
 *    out of order (a history replay) on the cursor's day.
 *  - A line without a sender (system, error, raw) never moves the cursor:
 *    it takes the day that puts it nearest the cursor.  Pidgin 2 stamped
 *    those with its own clock while the messages around them carried the
 *    sender's time (libdiscord's replays, once skewed by the UTC offset),
 *    and each one looked like a new day.  Until the file's first message
 *    line they do move it, so logs of system lines only still roll over.
 *  - A dated stamp wins, and a message line's moves the cursor forward.
 *    One at the Unix epoch (a zero time_t that was logged, "12/31/1969")
 *    is dated at the cursor instead.
 *  - No undated line is dated after the file's mtime by more than a day
 *    (message lines: senders' clocks can be off by a time zone) or an
 *    hour (lines without a sender: Pidgin's own clock).
 */
typedef struct _PidginLogClock PidginLogClock;

/**
 * A clock for one file.  @a file_start is the start time from the file name
 * (its time zone is the log's; referenced); @a file_end is the file's mtime
 * (Unix time), or 0 if unknown (no bound).
 */
PidginLogClock *pidgin_log_clock_new(GDateTime *file_start, gint64 file_end);

void pidgin_log_clock_free(PidginLogClock *clock);

/**
 * Resumes inside a file: @a last is the newest time of its rows so far,
 * @a messages whether that is a message line's (TRUE) or the file had
 * lines without a sender only (FALSE).
 */
void pidgin_log_clock_resume(PidginLogClock *clock, gint64 last, gboolean messages);

/** The cursor (Unix time), or 0 before the first line. */
gint64 pidgin_log_clock_get_last(PidginLogClock *clock);

/**
 * Parses one HTML log line, dating it with @a clock (and advancing it).
 * Fills time, sender, body (plain text) and flags of @a out.
 *
 * @return FALSE if @a line is not a message line.
 */
gboolean pidgin_backfill_parse_html_line(const char *line, PidginLogClock *clock,
		PidginIndexedMessage *out);

/** Same as pidgin_backfill_parse_html_line() for the text log format. */
gboolean pidgin_backfill_parse_txt_line(const char *line, PidginLogClock *clock,
		PidginIndexedMessage *out);

/**
 * Parses a log file name such as "2025-07-08.212153-0700PDT.html" into its
 * start time (in the local zone if it has the same offset, otherwise in a
 * fixed-offset zone).  NULL if the name has no date.
 */
GDateTime *pidgin_backfill_parse_file_name(const char *name);

/**
 * Returns the backfill of pidgin_message_index_get_default() over
 * <purple_user_dir()>/logs, creating it on first use; NULL without an
 * index.  Owned by this module.
 */
PidginBackfill *pidgin_backfill_get_default(void);

/**
 * Cancels the default backfill, waits for its thread to stop and drops
 * it.  Called by pidgin_message_index_ui_uninit().
 */
void pidgin_backfill_shutdown_default(void);

G_END_DECLS

#endif /* _PIDGIN_BACKFILL_H_ */
