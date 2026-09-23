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

#include "conversation.h"
#include "util.h"

#include "pidginbackfill.h"

/* A batch is committed after this many rows or this long, whichever comes
 * first, so live writes from the main thread never wait long. */
#define BATCH_ROWS 500
#define BATCH_USEC (100 * 1000)
/* Lines longer than this (e.g. inline base64 images) are skipped. */
#define MAX_LINE (4 * 1024 * 1024)
/* Continuation lines stop being appended past this. */
#define MAX_BODY (1024 * 1024)
#define READ_CHUNK (256 * 1024)
#define PROGRESS_USEC (250 * 1000)
/* An undated time more than this before the previous line means the
 * day rolled over. */
#define ROLLOVER_SLACK (60 * 60)

enum {
	PROP_0,
	PROP_INDEX,
	PROP_LOGS_DIR,
	PROP_THROTTLE_MS,
	N_PROPS
};

enum {
	SIG_PROGRESS,
	SIG_FINISHED,
	N_SIGNALS
};

static GParamSpec *properties[N_PROPS];
static guint signals[N_SIGNALS];

struct _PidginBackfill {
	GObject parent;

	PidginMessageIndex *idx;
	char *logs_dir;
	guint throttle_ms;

	GMutex lock;
	GCond cond;
	gboolean paused;
	gboolean running;         /* start() .. finished */
	gboolean worker_active;   /* the worker function is executing */
	GCancellable *cancellable;
	GMainContext *context;    /* where async signals go */
};

G_DEFINE_TYPE(PidginBackfill, pidgin_backfill, G_TYPE_OBJECT)

static PidginBackfill *default_backfill = NULL;

/******************************************************************************
 * Timestamps
 *****************************************************************************/

static const char *
parse_uint(const char *p, const char *end, int *value, int *digits)
{
	int v = 0, n = 0;

	while (p < end && g_ascii_isdigit(*p) && n < 9) {
		v = v * 10 + (*p - '0');
		p++;
		n++;
	}
	*value = v;
	*digits = n;

	return p;
}

/* Parses "[date ]h:mm[:ss][ AM|PM]" between @p and @end.  The date may be
 * Y-M-D, Y/M/D, M/D/Y (or D/M/Y when the first part is > 12) and D.M.Y,
 * with two- or four-digit years.  *year is 0 if there is no date. */
static gboolean
parse_stamp(const char *p, const char *end, int *year, int *month, int *day,
		int *hour, int *min, int *sec)
{
	int a, b, c, n;
	char sep;

	*year = *month = *day = 0;
	*sec = 0;

	while (p < end && g_ascii_isspace(*p))
		p++;

	p = parse_uint(p, end, &a, &n);
	if (n == 0 || p >= end)
		return FALSE;

	if (*p == '-' || *p == '/' || *p == '.') {
		int na;

		na = n;
		sep = *p++;
		p = parse_uint(p, end, &b, &n);
		if (n == 0 || p >= end || *p != sep)
			return FALSE;
		p++;
		p = parse_uint(p, end, &c, &n);
		if (n == 0)
			return FALSE;

		if (na == 4 || a > 31) {
			*year = a; *month = b; *day = c;
		} else if (sep == '.' || sep == '-' || a > 12) {
			*day = a; *month = b; *year = c;
		} else {
			*month = a; *day = b; *year = c;
		}
		if (*year < 100)
			*year += (*year < 70) ? 2000 : 1900;
		if (*month < 1 || *month > 12 || *day < 1 || *day > 31)
			return FALSE;

		while (p < end && (g_ascii_isspace(*p) || *p == ',' || *p == 'T'))
			p++;
		p = parse_uint(p, end, &a, &n);
		if (n == 0)
			return FALSE;
	}

	*hour = a;
	if (p >= end || *p != ':')
		return FALSE;
	p = parse_uint(p + 1, end, min, &n);
	if (n == 0)
		return FALSE;
	if (p < end && *p == ':') {
		p = parse_uint(p + 1, end, sec, &n);
		if (n == 0)
			return FALSE;
	}

	while (p < end && g_ascii_isspace(*p))
		p++;
	if (p + 1 < end && (p[1] == 'M' || p[1] == 'm' || p[1] == '.')) {
		if (*p == 'P' || *p == 'p') {
			if (*hour < 1 || *hour > 12)
				return FALSE;
			if (*hour != 12)
				*hour += 12;
		} else if (*p == 'A' || *p == 'a') {
			if (*hour < 1 || *hour > 12)
				return FALSE;
			if (*hour == 12)
				*hour = 0;
		}
	}

	return *hour < 24 && *min < 60 && *sec < 61;
}

/* Turns a parsed stamp into a time, using @file_start and the running
 * cursor *@last for undated stamps, and advances the cursor. */
static gboolean
resolve_stamp(const char *p, const char *end, GDateTime *file_start,
		GDateTime **last, gint64 *out)
{
	int year, month, day, hour, min, sec;
	GTimeZone *tz;
	GDateTime *dt;

	if (!parse_stamp(p, end, &year, &month, &day, &hour, &min, &sec))
		return FALSE;

	tz = g_date_time_get_timezone(file_start);

	if (year != 0) {
		/* A date in the stamp wins. */
		dt = g_date_time_new(tz, year, month, day, hour, min, MIN(sec, 59));
		if (dt == NULL)
			return FALSE;
	} else {
		GDateTime *base = (last && *last) ? *last : file_start;
		int y, m, d;

		g_date_time_get_ymd(base, &y, &m, &d);
		dt = g_date_time_new(tz, y, m, d, hour, min, MIN(sec, 59));
		if (dt == NULL)
			return FALSE;
		if (g_date_time_to_unix(dt) <
				g_date_time_to_unix(base) - ROLLOVER_SLACK) {
			GDateTime *next = g_date_time_add_days(dt, 1);

			g_date_time_unref(dt);
			dt = next;
		}
	}

	*out = g_date_time_to_unix(dt);

	/* The cursor only moves forward: a dated line that is older (an
	 * offline message) doesn't pull the following undated lines back. */
	if (last != NULL) {
		if (*last == NULL || g_date_time_compare(dt, *last) > 0) {
			if (*last != NULL)
				g_date_time_unref(*last);
			*last = g_date_time_ref(dt);
		}
	}
	g_date_time_unref(dt);

	return TRUE;
}

GDateTime *
pidgin_backfill_parse_file_name(const char *name)
{
	int y, mo, d, h, mi, s;
	int offset = 0;
	gboolean has_offset = FALSE;
	GTimeZone *tz;
	GDateTime *dt;

	if (name == NULL || strlen(name) < 17 ||
	    sscanf(name, "%4d-%2d-%2d.%2d%2d%2d", &y, &mo, &d, &h, &mi, &s) != 6)
		return NULL;

	name += 17;
	if ((name[0] == '+' || name[0] == '-') && g_ascii_isdigit(name[1]) &&
	    g_ascii_isdigit(name[2]) && g_ascii_isdigit(name[3]) &&
	    g_ascii_isdigit(name[4])) {
		int hh = (name[1] - '0') * 10 + (name[2] - '0');
		int mm = (name[3] - '0') * 10 + (name[4] - '0');

		offset = (hh * 3600 + mm * 60) * (name[0] == '-' ? -1 : 1);
		has_offset = TRUE;
	}

	/* Prefer the local zone when it matches, so a file that spans a DST
	 * change still gets the right offsets after it. */
	tz = g_time_zone_new_local();
	dt = g_date_time_new(tz, y, mo, d, h, mi, s);
	if (dt != NULL && has_offset && g_date_time_get_utc_offset(dt) !=
			(GTimeSpan)offset * G_TIME_SPAN_SECOND) {
		g_date_time_unref(dt);
		g_time_zone_unref(tz);
		tz = g_time_zone_new_offset(offset);
		dt = g_date_time_new(tz, y, mo, d, h, mi, s);
	}
	g_time_zone_unref(tz);

	return dt;
}

/******************************************************************************
 * Line parsing
 *****************************************************************************/

static void
message_reset(PidginIndexedMessage *out)
{
	g_clear_pointer(&out->sender, g_free);
	g_clear_pointer(&out->body, g_free);
	out->flags = 0;
	out->time = 0;
}

/*
 * Copies [start, end) with numeric character references resolved.  This
 * runs in the worker thread, and libpurple's purple_markup_unescape_entity()
 * decodes numeric references into a static buffer, so they must never
 * reach it from here.  Markup-significant characters become named
 * entities (which it handles without the buffer) so they stay text.
 */
static char *
resolve_numeric_entities(const char *start, const char *end)
{
	GString *out = g_string_sized_new(end - start);
	const char *p = start;

	while (p < end) {
		const char *amp = memchr(p, '&', end - p);
		const char *q;
		guint64 code = 0;
		int base = 10;

		if (amp == NULL) {
			g_string_append_len(out, p, end - p);
			break;
		}
		g_string_append_len(out, p, amp - p);
		p = amp;

		q = amp + 1;
		if (q < end && *q == '#') {
			q++;
			if (q < end && (*q == 'x' || *q == 'X')) {
				base = 16;
				q++;
			}
			while (q < end && g_ascii_isxdigit(*q) &&
			       (base == 16 || g_ascii_isdigit(*q)) && code <= 0x10FFFF) {
				code = code * base + g_ascii_xdigit_value(*q);
				q++;
			}
		}
		if (q >= end || *q != ';' || code == 0 || code > 0x10FFFF ||
		    !g_unichar_validate((gunichar)code)) {
			/* Not a numeric reference: leave it to libpurple. */
			g_string_append_c(out, '&');
			p = amp + 1;
			continue;
		}

		switch (code) {
			case '<': g_string_append(out, "&lt;"); break;
			case '>': g_string_append(out, "&gt;"); break;
			case '&': g_string_append(out, "&amp;"); break;
			case '"': g_string_append(out, "&quot;"); break;
			case '\'': g_string_append(out, "&apos;"); break;
			default: g_string_append_unichar(out, (gunichar)code);
		}
		p = q + 1;
	}

	return g_string_free(out, FALSE);
}

/* Plain text of an HTML fragment. */
static char *
html_to_plain(const char *start, const char *end)
{
	char *html = resolve_numeric_entities(start, end);
	char *plain = purple_markup_strip_html(html);
	char *norm = pidgin_message_index_normalize_text(plain);

	g_free(html);
	g_free(plain);

	return norm;
}

static char *
unescape_range(const char *start, const char *end)
{
	char *raw = resolve_numeric_entities(start, end);
	char *text = purple_unescape_html(raw);
	char *norm = pidgin_message_index_normalize_text(text);

	g_free(raw);
	g_free(text);

	return norm;
}

/* Removes a trailing <br>, <br/> or <br /> (and whitespace) from [p, *end). */
static void
strip_trailing_br(const char *p, const char **end)
{
	const char *e = *end;

	while (e > p && g_ascii_isspace(e[-1]))
		e--;
	if (e - p >= 4 && g_ascii_strncasecmp(e - 4, "<br>", 4) == 0)
		e -= 4;
	else if (e - p >= 5 && g_ascii_strncasecmp(e - 5, "<br/>", 5) == 0)
		e -= 5;
	else if (e - p >= 6 && g_ascii_strncasecmp(e - 6, "<br />", 6) == 0)
		e -= 6;
	*end = e;
}

static gboolean
skip_prefix(const char **p, const char *prefix)
{
	size_t len = strlen(prefix);

	if (g_ascii_strncasecmp(*p, prefix, len) != 0)
		return FALSE;
	*p += len;
	return TRUE;
}

static guint
color_flags(const char *color)
{
	if (color == NULL)
		return 0;
	if (g_ascii_strncasecmp(color, "16569E", 6) == 0)
		return PURPLE_MESSAGE_SEND;
	if (g_ascii_strncasecmp(color, "A82F2F", 6) == 0)
		return PURPLE_MESSAGE_RECV;
	if (g_ascii_strncasecmp(color, "062585", 6) == 0)
		return PURPLE_MESSAGE_RECV;   /* /me; the direction isn't logged */
	if (g_ascii_strncasecmp(color, "6C2585", 6) == 0)
		return PURPLE_MESSAGE_WHISPER;
	if (g_ascii_strncasecmp(color, "FF0000", 6) == 0)
		return PURPLE_MESSAGE_ERROR;
	return 0;
}

/*
 * The lines html_logger_write() writes (and older Gaim/Pidgin variants
 * with <font> instead of <span>):
 *
 *   <span style="color: #C"><span style="font-size: smaller">(T)</span> <b>F:</b></span> M<br>
 *   <span style="color: #C"><span style="font-size: smaller">(T)</span> <b>***F</b></span> M<br>
 *   <span style="color: #C"><span style="font-size: smaller">(T)</span> <b>F &lt;AUTO-REPLY&gt;:</b></span> M<br>
 *   <span style="color: #6C2585"><span style="font-size: smaller">(T)</span><b> F:</b></span> M<br>
 *   <span style="color: #FF0000"><span style="font-size: smaller">(T)</span><b> M</b></span><br>
 *   <span style="font-size: smaller">(T)</span><b> M</b><br>          system
 *   <span style="font-size: smaller">(T)</span> M<br>                 raw
 *   <span style="font-size: smaller">(T)</font><b> F:</b> M<br>       unhandled type
 *   <font color="#C"><font size="2">(T)</font> <b>F:</b></font> M<br/>   Gaim
 */
gboolean
pidgin_backfill_parse_html_line(const char *line, GDateTime *file_start,
		GDateTime **last_time_inout, PidginIndexedMessage *out)
{
	const char *p = line;
	const char *color = NULL;
	const char *stamp, *stamp_end, *end;
	gint64 time;

	g_return_val_if_fail(line != NULL, FALSE);
	g_return_val_if_fail(file_start != NULL, FALSE);
	g_return_val_if_fail(out != NULL, FALSE);

	while (g_ascii_isspace(*p))
		p++;

	if (skip_prefix(&p, "<span style=\"color: #") ||
	    skip_prefix(&p, "<font color=\"#")) {
		color = p;
		p = strchr(p, '>');
		if (p == NULL)
			return FALSE;
		p++;
	}

	if (!skip_prefix(&p, "<span style=\"font-size: smaller\">(") &&
	    !skip_prefix(&p, "<font size=\"2\">("))
		return FALSE;

	stamp = p;
	stamp_end = strchr(p, ')');
	if (stamp_end == NULL || stamp_end - stamp > 64)
		return FALSE;
	p = stamp_end + 1;
	if (!skip_prefix(&p, "</span>") && !skip_prefix(&p, "</font>"))
		return FALSE;

	if (!resolve_stamp(stamp, stamp_end, file_start, last_time_inout, &time))
		return FALSE;

	message_reset(out);
	out->time = time;
	out->flags = color_flags(color);

	end = p + strlen(p);
	strip_trailing_br(p, &end);

	while (p < end && *p == ' ')
		p++;

	if (skip_prefix(&p, "<b>")) {
		const char *b = p;
		const char *b_end = strstr(p, "</b>");
		const char *after;

		if (b_end == NULL || b_end > end)
			return FALSE;
		after = b_end + 4;
		if (color != NULL && !skip_prefix(&after, "</span>"))
			skip_prefix(&after, "</font>");
		if (after > end)
			after = end;

		while (b < b_end && *b == ' ')
			b++;

		if (out->flags & PURPLE_MESSAGE_ERROR) {
			out->body = html_to_plain(b, b_end);
		} else if (color != NULL && b_end - b > 3 && strncmp(b, "***", 3) == 0) {
			/* /me */
			out->sender = unescape_range(b + 3, b_end);
			out->body = html_to_plain(after, end);
		} else if (b_end > b && b_end[-1] == ':') {
			const char *s_end = b_end - 1;
			static const char auto_reply[] = " &lt;AUTO-REPLY&gt;";
			size_t alen = sizeof(auto_reply) - 1;

			if ((size_t)(s_end - b) > alen &&
			    strncmp(s_end - alen, auto_reply, alen) == 0) {
				s_end -= alen;
				out->flags |= PURPLE_MESSAGE_AUTO_RESP;
			}
			out->sender = unescape_range(b, s_end);
			out->body = html_to_plain(after, end);
			/* The "unhandled type" line has no colour. */
		} else {
			/* <b> text</b>: a system message. */
			out->flags |= PURPLE_MESSAGE_SYSTEM;
			out->body = html_to_plain(b, end);
		}
	} else {
		out->flags |= PURPLE_MESSAGE_RAW;
		out->body = html_to_plain(p, end);
	}

	if (out->sender != NULL && *out->sender == '\0')
		g_clear_pointer(&out->sender, g_free);

	return TRUE;
}

/*
 * The lines txt_logger_write() writes:
 *   (T) F: M          (T) ***F M          (T) F <AUTO-REPLY>: M
 *   (T) M  (system, error, raw)           (T) *F* M  (whisper)
 * The text log doesn't say whether a message was sent or received.
 */
gboolean
pidgin_backfill_parse_txt_line(const char *line, GDateTime *file_start,
		GDateTime **last_time_inout, PidginIndexedMessage *out)
{
	const char *p, *stamp_end, *colon;
	gint64 time;

	g_return_val_if_fail(line != NULL, FALSE);
	g_return_val_if_fail(file_start != NULL, FALSE);
	g_return_val_if_fail(out != NULL, FALSE);

	if (line[0] != '(')
		return FALSE;
	stamp_end = strchr(line, ')');
	if (stamp_end == NULL || stamp_end - line > 64 || stamp_end[1] != ' ')
		return FALSE;
	if (!resolve_stamp(line + 1, stamp_end, file_start, last_time_inout, &time))
		return FALSE;

	message_reset(out);
	out->time = time;
	p = stamp_end + 2;

	if (strncmp(p, "***", 3) == 0 && p[3] != '\0' && p[3] != ' ') {
		const char *sp = strchr(p + 3, ' ');

		out->flags = PURPLE_MESSAGE_RECV;
		out->sender = g_strndup(p + 3, sp ? sp - (p + 3) : (gssize)strlen(p + 3));
		out->body = pidgin_message_index_normalize_text(sp ? sp + 1 : "");
		return TRUE;
	}

	if (p[0] == '*' && p[1] != '*' && p[1] != ' ') {
		const char *star = strstr(p + 1, "* ");

		if (star != NULL && star - p < 64) {
			out->flags = PURPLE_MESSAGE_WHISPER;
			out->sender = g_strndup(p + 1, star - (p + 1));
			out->body = pidgin_message_index_normalize_text(star + 2);
			return TRUE;
		}
	}

	colon = strstr(p, ": ");
	if (colon != NULL && colon > p && colon - p <= 64) {
		const char *s_end = colon;
		static const char auto_reply[] = " <AUTO-REPLY>";
		size_t alen = sizeof(auto_reply) - 1;
		const char *c;
		int spaces = 0;

		if ((size_t)(s_end - p) > alen &&
		    strncmp(s_end - alen, auto_reply, alen) == 0) {
			s_end -= alen;
			out->flags |= PURPLE_MESSAGE_AUTO_RESP;
		}
		for (c = p; c < s_end; c++)
			if (*c == ' ')
				spaces++;
		/* A nick or alias, not a system sentence with a colon. */
		if (spaces <= 2) {
			out->flags |= PURPLE_MESSAGE_RECV;
			out->sender = g_strndup(p, s_end - p);
			out->body = pidgin_message_index_normalize_text(colon + 2);
			return TRUE;
		}
		out->flags = 0;
	}

	out->flags = PURPLE_MESSAGE_SYSTEM;
	out->body = pidgin_message_index_normalize_text(p);

	return TRUE;
}

/* Lines that are part of the file framing, never message text. */
static gboolean
is_framing_line(const char *line, gboolean html)
{
	if (strncmp(line, "---- ", 5) == 0)
		return TRUE;
	if (!html)
		return FALSE;
	return line[0] == '\0' ||
		g_str_has_prefix(line, "<span") || g_str_has_prefix(line, "<font") ||
		g_str_has_prefix(line, "<!DOCTYPE") || g_str_has_prefix(line, "<html") ||
		g_str_has_prefix(line, "<head") || g_str_has_prefix(line, "<h1") ||
		g_str_has_prefix(line, "<h3") || g_str_has_prefix(line, "<body") ||
		g_str_has_prefix(line, "</p>") || g_str_has_prefix(line, "</body") ||
		g_str_has_prefix(line, "</html");
}

/******************************************************************************
 * The file walk
 *****************************************************************************/

typedef struct {
	char *rel;       /* relative to logs_dir */
	gint64 size;
	gint64 mtime;
} LogFile;

typedef struct {
	PidginBackfill *bf;
	GCancellable *cancellable;
	gboolean async;

	GArray *files;          /* LogFile */
	guint files_done;
	guint64 bytes_done;
	guint64 bytes_total;
	gint64 last_progress;

	gboolean in_batch;
	guint batch_rows;
	gint64 batch_start;
	gboolean have_unlogged;

	guint64 rows_added;
} BackfillRun;

static void
log_file_clear(gpointer data)
{
	g_free(((LogFile *)data)->rel);
}

static gboolean
is_log_name(const char *name)
{
	return g_str_has_suffix(name, ".html") || g_str_has_suffix(name, ".htm") ||
		g_str_has_suffix(name, ".txt");
}

/* Lists the entries of @dir (relative @rel) into a sorted array. */
static GPtrArray *
list_dir(const char *dir)
{
	GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
	GDir *d = g_dir_open(dir, 0, NULL);
	const char *name;

	if (d == NULL)
		return names;
	while ((name = g_dir_read_name(d)) != NULL)
		g_ptr_array_add(names, g_strdup(name));
	g_dir_close(d);
	g_ptr_array_sort_values(names, (GCompareFunc)strcmp);

	return names;
}

/* logs/<prpl>/<account>/<conv>/<file>: depth 0 = prpl ... 3 = file. */
static void
scan_dir(BackfillRun *run, const char *abs, const char *rel, int depth)
{
	GPtrArray *names;
	guint i;

	if (g_cancellable_is_cancelled(run->cancellable))
		return;

	names = list_dir(abs);
	for (i = 0; i < names->len; i++) {
		const char *name = names->pdata[i];
		char *child_abs, *child_rel;
		GStatBuf st;

		/* .system (and any other hidden directory) isn't a conversation. */
		if (name[0] == '.')
			continue;
		if (depth == 3 && !is_log_name(name))
			continue;

		child_abs = g_build_filename(abs, name, NULL);
		child_rel = rel ? g_build_filename(rel, name, NULL) : g_strdup(name);

		if (g_stat(child_abs, &st) == 0) {
			if (depth < 3 && S_ISDIR(st.st_mode)) {
				scan_dir(run, child_abs, child_rel, depth + 1);
			} else if (depth == 3 && S_ISREG(st.st_mode)) {
				LogFile lf;

				lf.rel = child_rel;
				lf.size = st.st_size;
				lf.mtime = st.st_mtime;
				g_array_append_val(run->files, lf);
				run->bytes_total += st.st_size;
				child_rel = NULL;
			}
		}

		g_free(child_abs);
		g_free(child_rel);
	}
	g_ptr_array_unref(names);
}

/* Progress, on the caller's context for async runs. */
typedef struct {
	PidginBackfill *bf;
	guint files_done, files_total;
	guint64 bytes_done, bytes_total;
} ProgressData;

static gboolean
progress_idle(gpointer data)
{
	ProgressData *pd = data;

	g_signal_emit(pd->bf, signals[SIG_PROGRESS], 0, pd->files_done,
			pd->files_total, pd->bytes_done, pd->bytes_total);

	return G_SOURCE_REMOVE;
}

static void
progress_data_free(gpointer data)
{
	ProgressData *pd = data;

	g_object_unref(pd->bf);
	g_free(pd);
}

static void
report_progress(BackfillRun *run, gboolean force)
{
	gint64 now = g_get_monotonic_time();
	ProgressData *pd;

	if (!force && now - run->last_progress < PROGRESS_USEC)
		return;
	run->last_progress = now;

	pd = g_new(ProgressData, 1);
	pd->bf = g_object_ref(run->bf);
	pd->files_done = run->files_done;
	pd->files_total = run->files->len;
	pd->bytes_done = run->bytes_done;
	pd->bytes_total = run->bytes_total;

	if (run->async) {
		/* Not g_main_context_invoke_full(): with the caller's context the
		 * default one, it calls progress_idle() on this worker thread
		 * whenever the context is free between main loop iterations. */
		GSource *source = g_idle_source_new();

		g_source_set_priority(source, G_PRIORITY_DEFAULT_IDLE);
		g_source_set_callback(source, progress_idle, pd, progress_data_free);
		g_source_attach(source, run->bf->context);
		g_source_unref(source);
	} else {
		progress_idle(pd);
		progress_data_free(pd);
	}
}

static void
batch_begin(BackfillRun *run)
{
	if (run->in_batch)
		return;
	pidgin_message_index_begin(run->bf->idx);
	run->in_batch = TRUE;
	run->batch_rows = 0;
	run->batch_start = g_get_monotonic_time();
}

/* Commits the open batch, then honours the throttle and pause. */
static void
batch_commit(BackfillRun *run)
{
	PidginBackfill *bf = run->bf;
	gint64 until;

	if (!run->in_batch)
		return;
	pidgin_message_index_commit(bf->idx);
	run->in_batch = FALSE;

	g_mutex_lock(&bf->lock);
	until = g_get_monotonic_time() + (gint64)bf->throttle_ms * 1000;
	while (!g_cancellable_is_cancelled(run->cancellable) &&
	       (bf->paused || (bf->throttle_ms > 0 && g_get_monotonic_time() < until))) {
		if (bf->paused)
			g_cond_wait(&bf->cond, &bf->lock);
		else
			g_cond_wait_until(&bf->cond, &bf->lock, until);
	}
	g_mutex_unlock(&bf->lock);
}

static gboolean
batch_full(BackfillRun *run)
{
	return run->in_batch && (run->batch_rows >= BATCH_ROWS ||
		g_get_monotonic_time() - run->batch_start >= BATCH_USEC ||
		g_atomic_int_get(&run->bf->paused));
}

/* Stores one parsed message unless its line is already indexed; links a
 * live row with no log position instead of duplicating it. */
static void
store_message(BackfillRun *run, PidginIndexedMessage *msg)
{
	PidginMessageIndex *idx = run->bf->idx;
	gint64 id;

	batch_begin(run);
	run->batch_rows++;

	if (pidgin_message_index_has_log_position(idx, msg))
		return;

	if (run->have_unlogged &&
	    (id = pidgin_message_index_find_unlogged(idx, msg)) != 0) {
		pidgin_message_index_mark_log_position(idx, id, msg->log_file,
				msg->log_offset);
		return;
	}

	if (pidgin_message_index_insert(idx, msg, NULL) != 0)
		run->rows_added++;
}

/* A buffered line reader with a bounded line length. */
typedef struct {
	FILE *fp;
	char *buf;
	size_t start, end;
	gboolean eof;
} LineReader;

/* Reads one line into @line (without the newline).  Returns the number of
 * bytes consumed, 0 at EOF.  *complete is FALSE for a last line without a
 * newline (still being written); *oversize is TRUE if it was cut. */
static gsize
reader_next(LineReader *r, GString *line, gboolean *complete, gboolean *oversize)
{
	gsize consumed = 0;

	g_string_truncate(line, 0);
	*complete = FALSE;
	*oversize = FALSE;

	for (;;) {
		char *nl;
		size_t n;

		if (r->start == r->end) {
			if (r->eof)
				return consumed;
			r->start = 0;
			r->end = fread(r->buf, 1, READ_CHUNK, r->fp);
			if (r->end == 0) {
				r->eof = TRUE;
				return consumed;
			}
		}

		nl = memchr(r->buf + r->start, '\n', r->end - r->start);
		n = (nl ? (size_t)(nl - (r->buf + r->start)) : r->end - r->start);
		if (line->len + n <= MAX_LINE)
			g_string_append_len(line, r->buf + r->start, n);
		else
			*oversize = TRUE;
		consumed += n;
		r->start += n;

		if (nl != NULL) {
			r->start++;
			consumed++;
			*complete = TRUE;
			if (line->len > 0 && line->str[line->len - 1] == '\r')
				g_string_truncate(line, line->len - 1);
			return consumed;
		}
	}
}

static gboolean
wait_if_paused(BackfillRun *run)
{
	PidginBackfill *bf = run->bf;

	if (!g_atomic_int_get(&bf->paused))
		return !g_cancellable_is_cancelled(run->cancellable);

	g_mutex_lock(&bf->lock);
	while (bf->paused && !g_cancellable_is_cancelled(run->cancellable))
		g_cond_wait(&bf->cond, &bf->lock);
	g_mutex_unlock(&bf->lock);

	return !g_cancellable_is_cancelled(run->cancellable);
}

/* Indexes @lf from byte @offset.  Returns FALSE if cancelled. */
static gboolean
index_file(BackfillRun *run, LogFile *lf, gint64 offset)
{
	PidginBackfill *bf = run->bf;
	char **parts;
	char *path, *account, *conv;
	gboolean is_chat, html, cancelled = FALSE;
	GDateTime *file_start, *last = NULL;
	PidginIndexedMessage *pending = NULL;
	PidginIndexedMessage parsed = { 0 };
	LineReader reader = { 0 };
	GString *line;
	gint64 pos = offset, last_time;
	guint lines = 0;

	parts = g_strsplit(lf->rel, G_DIR_SEPARATOR_S, 4);
	if (g_strv_length(parts) != 4) {
		g_strfreev(parts);
		return TRUE;
	}
	account = g_strdup_printf("%s/%s", parts[0], parts[1]);
	is_chat = g_str_has_suffix(parts[2], ".chat");
	conv = is_chat ? g_strndup(parts[2], strlen(parts[2]) - 5) : g_strdup(parts[2]);
	html = !g_str_has_suffix(parts[3], ".txt");

	file_start = pidgin_backfill_parse_file_name(parts[3]);
	if (file_start == NULL)
		file_start = g_date_time_new_from_unix_local(lf->mtime);
	g_strfreev(parts);

	/* Resuming inside a file: restart the day cursor from its rows. */
	if (offset > 0 && (last_time =
			pidgin_message_index_last_time_for_file(bf->idx, lf->rel)) > 0) {
		GDateTime *utc = g_date_time_new_from_unix_utc(last_time);

		last = g_date_time_to_timezone(utc, g_date_time_get_timezone(file_start));
		g_date_time_unref(utc);
	}

	path = g_build_filename(bf->logs_dir, lf->rel, NULL);
	reader.fp = g_fopen(path, "rb");
	if (reader.fp == NULL ||
	    (offset > 0 && fseeko(reader.fp, offset, SEEK_SET) != 0)) {
		g_debug("backfill: can't read %s: %s", path, g_strerror(errno));
		if (reader.fp != NULL)
			fclose(reader.fp);
		g_free(path);
		g_free(account);
		g_free(conv);
		g_date_time_unref(file_start);
		g_clear_pointer(&last, g_date_time_unref);
		return TRUE;
	}
	reader.buf = g_malloc(READ_CHUNK);
	line = g_string_sized_new(1024);

	for (;;) {
		gboolean complete, oversize;
		gsize n = reader_next(&reader, line, &complete, &oversize);
		gboolean ok;

		if (n == 0 || !complete)
			break;

		if ((++lines & 0xff) == 0) {
			report_progress(run, FALSE);
			if (g_cancellable_is_cancelled(run->cancellable) ||
			    g_atomic_int_get(&bf->paused)) {
				/* Checkpoint at this line, then wait or stop. */
				if (pending != NULL) {
					store_message(run, pending);
					g_clear_pointer(&pending, pidgin_indexed_message_free);
				}
				batch_begin(run);
				pidgin_message_index_set_file_state(bf->idx, lf->rel,
						lf->mtime, lf->size, pos);
				batch_commit(run);
				if (!wait_if_paused(run)) {
					cancelled = TRUE;
					break;
				}
			}
		}

		if (oversize) {
			pos += n;
			continue;
		}
		if (!g_utf8_validate_len(line->str, line->len, NULL)) {
			char *valid = g_utf8_make_valid(line->str, line->len);

			g_string_assign(line, valid);
			g_free(valid);
		}

		if (html)
			ok = pidgin_backfill_parse_html_line(line->str, file_start, &last, &parsed);
		else
			ok = pidgin_backfill_parse_txt_line(line->str, file_start, &last, &parsed);

		if (ok) {
			if (pending != NULL) {
				store_message(run, pending);
				g_clear_pointer(&pending, pidgin_indexed_message_free);
			}
			if (batch_full(run)) {
				pidgin_message_index_set_file_state(bf->idx, lf->rel,
						lf->mtime, lf->size, pos);
				batch_commit(run);
				if (g_cancellable_is_cancelled(run->cancellable)) {
					cancelled = TRUE;
					break;
				}
			}

			pending = pidgin_indexed_message_new();
			pending->account = g_strdup(account);
			pending->conv = g_strdup(conv);
			pending->is_chat = is_chat;
			pending->log_file = g_strdup(lf->rel);
			pending->log_offset = pos;
			pending->time = parsed.time;
			pending->flags = parsed.flags;
			pending->sender = g_steal_pointer(&parsed.sender);
			pending->body = g_steal_pointer(&parsed.body);
		} else if (pending != NULL && !is_framing_line(line->str, html) &&
		           strlen(pending->body) < MAX_BODY) {
			/* A message with a raw newline in it. */
			char *more = html ? html_to_plain(line->str, line->str + line->len)
			                  : pidgin_message_index_normalize_text(line->str);

			if (*more != '\0') {
				char *joined = g_strconcat(pending->body, " ", more, NULL);

				g_free(pending->body);
				pending->body = joined;
			}
			g_free(more);
		}

		pos += n;
	}

	if (!cancelled) {
		if (pending != NULL)
			store_message(run, pending);
		batch_begin(run);
		pidgin_message_index_set_file_state(bf->idx, lf->rel, lf->mtime,
				lf->size, pos);
		if (batch_full(run))
			batch_commit(run);
	}

	pidgin_indexed_message_free(pending);
	message_reset(&parsed);
	g_string_free(line, TRUE);
	g_free(reader.buf);
	fclose(reader.fp);
	g_free(path);
	g_free(account);
	g_free(conv);
	g_date_time_unref(file_start);
	g_clear_pointer(&last, g_date_time_unref);

	return !cancelled;
}

static gboolean
backfill_run(PidginBackfill *bf, GCancellable *cancellable, gboolean async,
		GError **error)
{
	BackfillRun run = { 0 };
	gboolean ok = TRUE;
	guint i;

	run.bf = bf;
	run.cancellable = cancellable;
	run.async = async;
	run.files = g_array_new(FALSE, TRUE, sizeof(LogFile));
	g_array_set_clear_func(run.files, log_file_clear);

	scan_dir(&run, bf->logs_dir, NULL, 0);
	run.have_unlogged = pidgin_message_index_has_unlogged(bf->idx);
	report_progress(&run, TRUE);

	for (i = 0; i < run.files->len; i++) {
		LogFile *lf = &g_array_index(run.files, LogFile, i);
		gint64 mtime, size, offset;
		gboolean known;

		if (!wait_if_paused(&run)) {
			ok = FALSE;
			break;
		}

		known = pidgin_message_index_get_file_state(bf->idx, lf->rel,
				&mtime, &size, &offset);
		if (known && lf->size == size && lf->mtime == mtime && offset >= size) {
			/* Unchanged. */
		} else if (known && (lf->size < size || lf->size < offset)) {
			/* Shrank or was replaced: start over. */
			batch_begin(&run);
			pidgin_message_index_forget_file(bf->idx, lf->rel);
			ok = index_file(&run, lf, 0);
		} else {
			ok = index_file(&run, lf, known ? offset : 0);
		}
		if (!ok)
			break;

		run.files_done++;
		run.bytes_done += lf->size;
		report_progress(&run, FALSE);
	}

	batch_commit(&run);

	if (ok && g_cancellable_is_cancelled(cancellable))
		ok = FALSE;
	if (!ok)
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
				"The log backfill was cancelled");

	report_progress(&run, TRUE);
	g_debug("backfill: %u of %u files, %" G_GUINT64_FORMAT " new rows%s",
			run.files_done, run.files->len, run.rows_added,
			ok ? "" : " (cancelled)");
	g_array_unref(run.files);

	return ok;
}

/******************************************************************************
 * Async API
 *****************************************************************************/

static void
worker_thread(GTask *task, gpointer source, gpointer data,
		GCancellable *cancellable)
{
	PidginBackfill *bf = source;
	GError *error = NULL;

	if (backfill_run(bf, cancellable, TRUE, &error))
		g_task_return_boolean(task, TRUE);
	else
		g_task_return_error(task, error);

	g_mutex_lock(&bf->lock);
	bf->worker_active = FALSE;
	g_cond_broadcast(&bf->cond);
	g_mutex_unlock(&bf->lock);
}

static void
worker_done(GObject *source, GAsyncResult *result, gpointer data)
{
	PidginBackfill *bf = PIDGIN_BACKFILL(source);
	gboolean completed = g_task_propagate_boolean(G_TASK(result), NULL);

	bf->running = FALSE;
	g_clear_object(&bf->cancellable);
	g_clear_pointer(&bf->context, g_main_context_unref);

	g_signal_emit(bf, signals[SIG_FINISHED], 0, completed);
}

void
pidgin_backfill_start(PidginBackfill *bf)
{
	GTask *task;

	g_return_if_fail(PIDGIN_IS_BACKFILL(bf));

	if (bf->running)
		return;

	bf->running = TRUE;
	bf->worker_active = TRUE;
	bf->cancellable = g_cancellable_new();
	bf->context = g_main_context_ref_thread_default();

	task = g_task_new(bf, bf->cancellable, worker_done, NULL);
	g_task_set_source_tag(task, pidgin_backfill_start);
	g_task_set_return_on_cancel(task, FALSE);
	g_task_run_in_thread(task, worker_thread);
	g_object_unref(task);
}

void
pidgin_backfill_pause(PidginBackfill *bf)
{
	g_return_if_fail(PIDGIN_IS_BACKFILL(bf));

	g_mutex_lock(&bf->lock);
	g_atomic_int_set(&bf->paused, TRUE);
	g_mutex_unlock(&bf->lock);
}

void
pidgin_backfill_resume(PidginBackfill *bf)
{
	g_return_if_fail(PIDGIN_IS_BACKFILL(bf));

	g_mutex_lock(&bf->lock);
	g_atomic_int_set(&bf->paused, FALSE);
	g_cond_broadcast(&bf->cond);
	g_mutex_unlock(&bf->lock);
}

void
pidgin_backfill_cancel(PidginBackfill *bf)
{
	g_return_if_fail(PIDGIN_IS_BACKFILL(bf));

	if (bf->cancellable != NULL)
		g_cancellable_cancel(bf->cancellable);

	/* Wake a paused or throttled worker so it sees the cancellation; the
	 * pause ends with the run, so the next start isn't paused. */
	g_mutex_lock(&bf->lock);
	g_atomic_int_set(&bf->paused, FALSE);
	g_cond_broadcast(&bf->cond);
	g_mutex_unlock(&bf->lock);
}

gboolean
pidgin_backfill_is_running(PidginBackfill *bf)
{
	g_return_val_if_fail(PIDGIN_IS_BACKFILL(bf), FALSE);

	return bf->running;
}

static void
sync_cancelled(GCancellable *cancellable, PidginBackfill *bf)
{
	g_mutex_lock(&bf->lock);
	g_cond_broadcast(&bf->cond);
	g_mutex_unlock(&bf->lock);
}

gboolean
pidgin_backfill_run_sync(PidginBackfill *bf, GCancellable *cancellable,
		GError **error)
{
	GCancellable *own = NULL;
	gulong handler = 0;
	gboolean ok;

	g_return_val_if_fail(PIDGIN_IS_BACKFILL(bf), FALSE);
	g_return_val_if_fail(!bf->running, FALSE);

	if (cancellable == NULL)
		cancellable = own = g_cancellable_new();
	handler = g_cancellable_connect(cancellable, G_CALLBACK(sync_cancelled),
			bf, NULL);

	ok = backfill_run(bf, cancellable, FALSE, error);

	g_cancellable_disconnect(cancellable, handler);
	g_clear_object(&own);
	g_signal_emit(bf, signals[SIG_FINISHED], 0, ok);

	return ok;
}

/******************************************************************************
 * GObject
 *****************************************************************************/

static void
pidgin_backfill_set_property(GObject *obj, guint prop_id, const GValue *value,
		GParamSpec *pspec)
{
	PidginBackfill *bf = PIDGIN_BACKFILL(obj);

	switch (prop_id) {
		case PROP_INDEX:
			bf->idx = g_value_dup_object(value);
			break;
		case PROP_LOGS_DIR:
			bf->logs_dir = g_value_dup_string(value);
			break;
		case PROP_THROTTLE_MS:
			bf->throttle_ms = g_value_get_uint(value);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
	}
}

static void
pidgin_backfill_get_property(GObject *obj, guint prop_id, GValue *value,
		GParamSpec *pspec)
{
	PidginBackfill *bf = PIDGIN_BACKFILL(obj);

	switch (prop_id) {
		case PROP_INDEX:
			g_value_set_object(value, bf->idx);
			break;
		case PROP_LOGS_DIR:
			g_value_set_string(value, bf->logs_dir);
			break;
		case PROP_THROTTLE_MS:
			g_value_set_uint(value, bf->throttle_ms);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
	}
}

static void
pidgin_backfill_finalize(GObject *obj)
{
	PidginBackfill *bf = PIDGIN_BACKFILL(obj);

	g_clear_object(&bf->idx);
	g_clear_object(&bf->cancellable);
	g_clear_pointer(&bf->context, g_main_context_unref);
	g_free(bf->logs_dir);
	g_mutex_clear(&bf->lock);
	g_cond_clear(&bf->cond);

	G_OBJECT_CLASS(pidgin_backfill_parent_class)->finalize(obj);
}

static void
pidgin_backfill_class_init(PidginBackfillClass *klass)
{
	GObjectClass *obj_class = G_OBJECT_CLASS(klass);

	obj_class->set_property = pidgin_backfill_set_property;
	obj_class->get_property = pidgin_backfill_get_property;
	obj_class->finalize = pidgin_backfill_finalize;

	properties[PROP_INDEX] = g_param_spec_object("index", "index",
			"The message index to fill", PIDGIN_TYPE_MESSAGE_INDEX,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
	properties[PROP_LOGS_DIR] = g_param_spec_string("logs-dir", "logs-dir",
			"The <profile>/logs directory", NULL,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
	properties[PROP_THROTTLE_MS] = g_param_spec_uint("throttle-ms", "throttle-ms",
			"Pause between batches, in milliseconds", 0, 10000, 20,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS);
	g_object_class_install_properties(obj_class, N_PROPS, properties);

	signals[SIG_PROGRESS] = g_signal_new("progress", G_TYPE_FROM_CLASS(klass),
			G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 4,
			G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT64, G_TYPE_UINT64);
	signals[SIG_FINISHED] = g_signal_new("finished", G_TYPE_FROM_CLASS(klass),
			G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1,
			G_TYPE_BOOLEAN);
}

static void
pidgin_backfill_init(PidginBackfill *bf)
{
	g_mutex_init(&bf->lock);
	g_cond_init(&bf->cond);
}

PidginBackfill *
pidgin_backfill_new(PidginMessageIndex *idx, const char *logs_dir)
{
	g_return_val_if_fail(PIDGIN_IS_MESSAGE_INDEX(idx), NULL);
	g_return_val_if_fail(logs_dir != NULL, NULL);

	return g_object_new(PIDGIN_TYPE_BACKFILL, "index", idx,
			"logs-dir", logs_dir, NULL);
}

PidginBackfill *
pidgin_backfill_get_default(void)
{
	PidginMessageIndex *idx;
	char *logs;

	if (default_backfill != NULL)
		return default_backfill;

	idx = pidgin_message_index_get_default();
	if (idx == NULL)
		return NULL;

	logs = g_build_filename(purple_user_dir(), "logs", NULL);
	default_backfill = pidgin_backfill_new(idx, logs);
	g_free(logs);

	return default_backfill;
}

void
pidgin_backfill_shutdown_default(void)
{
	PidginBackfill *bf = default_backfill;

	if (bf == NULL)
		return;

	pidgin_backfill_cancel(bf);
	pidgin_backfill_resume(bf);

	/* The worker only checks the cancellable between lines, so this is
	 * short; its "finished" is never delivered if the main loop is gone. */
	g_mutex_lock(&bf->lock);
	while (bf->worker_active)
		g_cond_wait(&bf->cond, &bf->lock);
	g_mutex_unlock(&bf->lock);

	g_clear_object(&default_backfill);
}
