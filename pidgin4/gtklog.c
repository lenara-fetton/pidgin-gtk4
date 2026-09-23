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

/*
 * The log viewer: port of pidgin/gtklog.c. The logs are a GtkListView
 * over a GtkTreeListModel (month, or account then month for the system
 * log), the selected log is shown in a PidginMessageView, one row per
 * log line. Search asks the message index which log files contain the
 * words and scans the files it has not indexed (incrementally, from an
 * idle callback) like Pidgin 2 did. The viewer never writes logs or the
 * index; only "Delete Log" (after a confirmation) removes a log.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include <string.h>
#include <glib/gstdio.h>

#include "account.h"
#include "debug.h"
#include "log.h"
#include "notify.h"
#include "prefs.h"
#include "request.h"
#include "signals.h"
#include "util.h"

#include "gtklog.h"
#include "gtkutils.h"
#include "pidginbackfill.h"
#include "pidginmessage.h"
#include "pidginmessageindex.h"
#include "pidginmessageview.h"
#include "pidginselftest.h"

/* Rows asked from the index per conversation; with more matches than
 * this the index can't prove a file has none, so those are scanned. */
#define INDEX_SEARCH_LIMIT 10000

/* Time spent per idle callback of the linear search, in microseconds. */
#define SCAN_SLICE_US 15000

typedef struct _PidginLogViewer PidginLogViewer;

struct _PidginLogViewer {
	/* What is shown: a buddy or chat, a contact, or the system log */
	PurpleLogType type;
	char *name;
	PurpleAccount *account;
	PurpleContact *contact;
	gboolean syslog;

	GList *logs;                 /* PurpleLog*, owned, newest first */

	GtkWidget *window;
	GtkWidget *label;            /* the header */
	GtkWidget *size_label;
	GtkWidget *list_view;
	GtkWidget *view;             /* PidginMessageView */
	GtkWidget *entry;            /* GtkSearchEntry */
	GtkWidget *status;           /* search progress */
	GtkWidget *spinner;
	GtkWidget *delete_button;

	GtkTreeListModel *tree;
	GtkSingleSelection *selection;
	PurpleLog *shown;

	/* Search */
	char *search;
	GHashTable *matches;         /* PurpleLog* set, while a search is active */
	GList *scan_queue;           /* logs still to scan linearly */
	guint scan_total;
	guint scan_idle;
	guint n_indexed;             /* logs decided by the index */

	GCancellable *cancellable;
};

static GList *log_viewers = NULL;
static PidginLogViewer *syslog_viewer = NULL;

static void populate_log_tree(PidginLogViewer *lv);

/**************************************************************************
 * PidginLogNode: an account, month or log in the tree
 **************************************************************************/

#define PIDGIN_TYPE_LOG_NODE (pidgin_log_node_get_type())
G_DECLARE_FINAL_TYPE(PidginLogNode, pidgin_log_node, PIDGIN, LOG_NODE, GObject)

struct _PidginLogNode {
	GObject parent;
	char *label;
	PurpleLog *log;              /* leaves only; owned by the viewer */
	GIcon *icon;
	GListStore *children;        /* groups only */
};

G_DEFINE_FINAL_TYPE(PidginLogNode, pidgin_log_node, G_TYPE_OBJECT)

static void
pidgin_log_node_finalize(GObject *obj)
{
	PidginLogNode *node = PIDGIN_LOG_NODE(obj);

	g_free(node->label);
	g_clear_object(&node->icon);
	g_clear_object(&node->children);
	G_OBJECT_CLASS(pidgin_log_node_parent_class)->finalize(obj);
}

static void
pidgin_log_node_class_init(PidginLogNodeClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = pidgin_log_node_finalize;
}

static void pidgin_log_node_init(PidginLogNode *node) { }

static PidginLogNode *
log_node_new(const char *label, PurpleLog *log)
{
	PidginLogNode *node = g_object_new(PIDGIN_TYPE_LOG_NODE, NULL);

	node->label = g_strdup(label);
	node->log = log;
	if (log == NULL)
		node->children = g_list_store_new(PIDGIN_TYPE_LOG_NODE);
	return node;
}

static GListModel *
create_children_cb(gpointer item, gpointer data)
{
	PidginLogNode *node = item;

	return node->children ? G_LIST_MODEL(g_object_ref(node->children)) : NULL;
}

/**************************************************************************
 * Helpers
 **************************************************************************/

static const char *
log_get_date(PurpleLog *log)
{
	if (log->tm)
		return purple_date_format_full(log->tm);
	else
		return purple_date_format_full(localtime(&log->time));
}

/* The log file of a log written by the HTML or text logger, or NULL. */
static const char *
log_get_path(PurpleLog *log)
{
	PurpleLogCommonLoggerData *data;

	if (log->logger == NULL || log->logger->id == NULL ||
	    (strcmp(log->logger->id, "html") != 0 && strcmp(log->logger->id, "txt") != 0))
		return NULL;
	data = log->logger_data;
	return data ? data->path : NULL;
}

/* The log file relative to <profile>/logs, as the message index keys it. */
static char *
log_get_rel_path(PurpleLog *log)
{
	const char *path = log_get_path(log);
	char *logs_dir;
	char *rel = NULL;
	size_t len;

	if (path == NULL)
		return NULL;
	logs_dir = g_build_filename(purple_user_dir(), "logs", NULL);
	len = strlen(logs_dir);
	if (strncmp(path, logs_dir, len) == 0 && path[len] == G_DIR_SEPARATOR)
		rel = g_strdup(path + len + 1);
	g_free(logs_dir);
	return rel;
}

/* The start time of the log's file for the line parsers. */
static GDateTime *
log_get_file_start(PurpleLog *log)
{
	const char *path = log_get_path(log);
	GDateTime *dt = NULL;

	if (path != NULL) {
		char *base = g_path_get_basename(path);
		dt = pidgin_backfill_parse_file_name(base);
		g_free(base);
	}
	if (dt == NULL)
		dt = g_date_time_new_from_unix_local(log->time);
	return dt;
}

static gboolean
viewer_exists(PidginLogViewer *lv)
{
	return lv != NULL && (lv == syslog_viewer || g_list_find(log_viewers, lv) != NULL);
}

/**************************************************************************
 * Showing a log
 **************************************************************************/

static gboolean
skip_prefix(const char **p, const char *prefix)
{
	size_t len = strlen(prefix);

	if (g_ascii_strncasecmp(*p, prefix, len) != 0)
		return FALSE;
	*p += len;
	return TRUE;
}

/*
 * The HTML body of a message line written by html_logger_write():
 *   <span style="color: #C"><span style="font-size: smaller">(T)</span> <b>F:</b></span> M<br>
 * i.e. what follows the sender's </b> (and the colour's </span>), without
 * the trailing <br>. NULL if the line has another shape.
 */
static char *
extract_html_body(const char *line, gboolean *action)
{
	const char *p = strstr(line, "</b>");
	const char *b = strstr(line, "<b>");
	const char *end;

	if (p == NULL || b == NULL || b > p)
		return NULL;

	*action = strncmp(b + 3, "***", 3) == 0;

	p += 4;
	if (!skip_prefix(&p, "</span>"))
		skip_prefix(&p, "</font>");
	while (*p == ' ')
		p++;

	end = p + strlen(p);
	while (end > p && g_ascii_isspace(end[-1]))
		end--;
	if (end - p >= 4 && g_ascii_strncasecmp(end - 4, "<br>", 4) == 0)
		end -= 4;
	else if (end - p >= 5 && g_ascii_strncasecmp(end - 5, "<br/>", 5) == 0)
		end -= 5;
	else if (end - p >= 6 && g_ascii_strncasecmp(end - 6, "<br />", 6) == 0)
		end -= 6;

	return g_strndup(p, end - p);
}

/* Whether @html has any text (framing lines like </body></html> don't). */
static gboolean
line_has_text(const char *html)
{
	char *plain = purple_markup_strip_html(html);
	gboolean ret;

	g_strstrip(plain);
	ret = *plain != '\0';
	g_free(plain);
	return ret;
}

/* The messages of @log, oldest first: one per log line; a line that is not
 * a message line becomes a raw row (text logs: a continuation line joins
 * the message before it). */
static GPtrArray *
log_read_messages(PurpleLog *log)
{
	PurpleLogReadFlags flags = 0;
	char *read = purple_log_read(log, &flags);
	gboolean html = (flags & PURPLE_LOG_READ_NO_NEWLINE) != 0;
	GDateTime *file_start = log_get_file_start(log);
	GDateTime *last = NULL;
	GPtrArray *messages = g_ptr_array_new_with_free_func(g_object_unref);
	PidginIndexedMessage *parsed = pidgin_indexed_message_new();
	PidginMessage *prev = NULL;
	char **lines, **l;

	lines = g_strsplit(read ? read : "", "\n", -1);
	g_free(read);

	for (l = lines; *l != NULL; l++) {
		char *line = *l;
		size_t len = strlen(line);
		PidginMessage *msg;
		gboolean ok;

		if (len > 0 && line[len - 1] == '\r')
			line[len - 1] = '\0';
		if (*line == '\0')
			continue;

		if (html) {
			ok = pidgin_backfill_parse_html_line(line, file_start, &last, parsed);
		} else {
			char *plain = purple_markup_strip_html(line);
			ok = pidgin_backfill_parse_txt_line(plain, file_start, &last, parsed);
			g_free(plain);
		}

		if (ok) {
			char *body = NULL;
			gboolean action = FALSE;

			if (html && parsed->sender != NULL &&
			    !(parsed->flags & (PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_ERROR |
			                       PURPLE_MESSAGE_RAW)))
				body = extract_html_body(line, &action);
			if (body == NULL)
				body = g_markup_escape_text(parsed->body ? parsed->body : "", -1);
			if (action) {
				char *tmp = g_strconcat("/me ", body, NULL);
				g_free(body);
				body = tmp;
			}

			msg = pidgin_message_new(parsed->sender, NULL, body,
			                         (PurpleMessageFlags)parsed->flags,
			                         (time_t)parsed->time);
			g_free(body);
			g_ptr_array_add(messages, msg);
			prev = msg;
		} else if (!html && prev != NULL) {
			char *joined = g_strconcat(pidgin_message_get_html(prev), "<br>", line, NULL);
			pidgin_message_set_html(prev, joined);
			g_free(joined);
		} else if (line_has_text(line)) {
			msg = pidgin_message_new(NULL, NULL, line, PURPLE_MESSAGE_RAW,
			                         last ? (time_t)g_date_time_to_unix(last) : log->time);
			g_ptr_array_add(messages, msg);
			prev = NULL;
		}
	}

	g_strfreev(lines);
	pidgin_indexed_message_free(parsed);
	if (last != NULL)
		g_date_time_unref(last);
	g_date_time_unref(file_start);
	return messages;
}

static void
update_delete_button(PidginLogViewer *lv)
{
	gtk_widget_set_sensitive(lv->delete_button,
		lv->shown != NULL && purple_log_is_deletable(lv->shown));
}

static void
log_show(PidginLogViewer *lv, PurpleLog *log)
{
	PidginMessageView *view = PIDGIN_MESSAGE_VIEW(lv->view);
	GPtrArray *messages;

	lv->shown = log;
	update_delete_button(lv);
	pidgin_message_view_clear(view);
	if (log == NULL)
		return;

	if (log->type != PURPLE_LOG_SYSTEM) {
		char *name = g_markup_escape_text(log->name, -1);
		char *title;

		if (log->type == PURPLE_LOG_CHAT)
			title = g_strdup_printf(_("<span size='larger' weight='bold'>Conversation in %s on %s</span>"),
			                        name, log_get_date(log));
		else
			title = g_strdup_printf(_("<span size='larger' weight='bold'>Conversation with %s on %s</span>"),
			                        name, log_get_date(log));

		gtk_label_set_markup(GTK_LABEL(lv->label), title);
		g_free(title);
		g_free(name);
	}

	purple_signal_emit(pidgin_log_get_handle(), "log-displaying", lv, log);

	messages = log_read_messages(log);
	if (messages->len > 0)
		pidgin_message_view_prepend_many(view, messages);

	pidgin_message_view_set_search_text(view, lv->search);
	if (messages->len > 0)
		pidgin_message_view_scroll_to_message(view, g_ptr_array_index(messages, 0));
	g_ptr_array_unref(messages);
}

static PidginLogNode *
node_at(PidginLogViewer *lv, guint position)
{
	GtkTreeListRow *row = gtk_tree_list_model_get_row(lv->tree, position);
	PidginLogNode *node = NULL;

	if (row != NULL) {
		node = gtk_tree_list_row_get_item(row);
		g_object_unref(row);
	}
	return node;
}

static void
log_selection_changed_cb(GtkSelectionModel *model, guint position, guint n_items,
                         PidginLogViewer *lv)
{
	guint selected = gtk_single_selection_get_selected(lv->selection);
	PidginLogNode *node;

	if (selected == GTK_INVALID_LIST_POSITION)
		return;
	node = node_at(lv, selected);
	if (node == NULL)
		return;
	if (node->log != NULL && node->log != lv->shown)
		log_show(lv, node->log);
	g_object_unref(node);
}

/* Expands the tree down to the first (newest) log and selects it. */
static void
select_first_log(PidginLogViewer *lv)
{
	guint pos = 0;
	GtkTreeListRow *row;

	while ((row = gtk_tree_list_model_get_row(lv->tree, pos)) != NULL) {
		PidginLogNode *node = gtk_tree_list_row_get_item(row);
		gboolean leaf = node->log != NULL;

		g_object_unref(node);
		if (leaf) {
			g_object_unref(row);
			gtk_single_selection_set_selected(lv->selection, pos);
			return;
		}
		gtk_tree_list_row_set_expanded(row, TRUE);
		g_object_unref(row);
		pos++;
	}

	/* Nothing to show (e.g. no log matches the search) */
	log_show(lv, NULL);
}

/* Activating a group opens or closes it, as in Pidgin 2. */
static void
log_row_activated_cb(GtkListView *view, guint position, PidginLogViewer *lv)
{
	GtkTreeListRow *row = gtk_tree_list_model_get_row(lv->tree, position);

	if (row == NULL)
		return;
	if (gtk_tree_list_row_is_expandable(row))
		gtk_tree_list_row_set_expanded(row, !gtk_tree_list_row_get_expanded(row));
	g_object_unref(row);
}

/**************************************************************************
 * The tree
 **************************************************************************/

static void
add_months(GListStore *store, GList *logs, PidginLogViewer *lv, PurpleAccount *only)
{
	PidginLogNode *month_node = NULL;
	char *prev_month = NULL;
	GList *l;

	for (l = logs; l != NULL; l = l->next) {
		PurpleLog *log = l->data;
		PidginLogNode *leaf;
		char *month;

		if (only != NULL && log->account != only)
			continue;
		if (lv->matches != NULL && !g_hash_table_contains(lv->matches, log))
			continue;

		month = g_strdup(purple_utf8_strftime(_("%B %Y"),
		                 log->tm ? log->tm : localtime(&log->time)));
		if (month_node == NULL || !purple_strequal(month, prev_month)) {
			month_node = log_node_new(month, NULL);
			g_list_store_append(store, month_node);
			g_object_unref(month_node);
			g_free(prev_month);
			prev_month = month;
		} else {
			g_free(month);
		}

		leaf = log_node_new(log_get_date(log), log);
		g_list_store_append(month_node->children, leaf);
		g_object_unref(leaf);
	}
	g_free(prev_month);
}

static void
populate_log_tree(PidginLogViewer *lv)
{
	GListStore *root = g_list_store_new(PIDGIN_TYPE_LOG_NODE);

	if (lv->syslog) {
		/* The system log: per account, then per month */
		GList *accounts = NULL, *l;

		for (l = lv->logs; l != NULL; l = l->next) {
			PurpleLog *log = l->data;
			if (g_list_find(accounts, log->account) == NULL)
				accounts = g_list_append(accounts, log->account);
		}
		for (l = accounts; l != NULL; l = l->next) {
			PurpleAccount *account = l->data;
			char *label = g_strdup_printf("%s (%s)", purple_account_get_username(account),
			                              purple_account_get_protocol_name(account));
			PidginLogNode *node = log_node_new(label, NULL);

			node->icon = pidgin_create_prpl_gicon(account, NULL);
			add_months(node->children, lv->logs, lv, account);
			if (g_list_model_get_n_items(G_LIST_MODEL(node->children)) > 0)
				g_list_store_append(root, node);
			g_object_unref(node);
			g_free(label);
		}
		g_list_free(accounts);
	} else {
		add_months(root, lv->logs, lv, NULL);
	}

	lv->shown = NULL;
	lv->tree = gtk_tree_list_model_new(G_LIST_MODEL(root), FALSE, FALSE,
	                                   create_children_cb, NULL, NULL);
	g_signal_handlers_block_by_func(lv->selection, log_selection_changed_cb, lv);
	gtk_single_selection_set_model(lv->selection, G_LIST_MODEL(lv->tree));
	g_signal_handlers_unblock_by_func(lv->selection, log_selection_changed_cb, lv);
	/* The selection holds the model now */
	g_object_unref(lv->tree);

	select_first_log(lv);
}

static guint
count_shown_logs(PidginLogViewer *lv)
{
	GList *l;
	guint n = 0;

	for (l = lv->logs; l != NULL; l = l->next)
		if (lv->matches == NULL || g_hash_table_contains(lv->matches, l->data))
			n++;
	return n;
}

static void
node_setup_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	GtkWidget *expander = gtk_tree_expander_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	GtkWidget *image = gtk_image_new();
	GtkWidget *label = gtk_label_new(NULL);

	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_box_append(GTK_BOX(box), image);
	gtk_box_append(GTK_BOX(box), label);
	gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), box);
	gtk_list_item_set_child(li, expander);
}

static void
node_bind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	GtkTreeListRow *row = gtk_list_item_get_item(li);
	GtkWidget *expander = gtk_list_item_get_child(li);
	GtkWidget *box = gtk_tree_expander_get_child(GTK_TREE_EXPANDER(expander));
	GtkWidget *image = gtk_widget_get_first_child(box);
	GtkWidget *label = gtk_widget_get_next_sibling(image);
	PidginLogNode *node = gtk_tree_list_row_get_item(row);

	gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(expander), row);
	gtk_label_set_text(GTK_LABEL(label), node->label);
	if (node->icon != NULL)
		gtk_image_set_from_gicon(GTK_IMAGE(image), node->icon);
	gtk_widget_set_visible(image, node->icon != NULL);
	g_object_unref(node);
}

static void
node_unbind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(gtk_list_item_get_child(li)), NULL);
}

/**************************************************************************
 * Search
 **************************************************************************/

static void
search_set_status(PidginLogViewer *lv, const char *text, gboolean busy)
{
	gtk_label_set_text(GTK_LABEL(lv->status), text ? text : "");
	gtk_widget_set_visible(lv->spinner, busy);
	gtk_spinner_set_spinning(GTK_SPINNER(lv->spinner), busy);
}

static void
search_stop(PidginLogViewer *lv)
{
	g_clear_handle_id(&lv->scan_idle, g_source_remove);
	g_clear_pointer(&lv->scan_queue, g_list_free);
	lv->scan_total = 0;
}

static void
search_finished(PidginLogViewer *lv)
{
	char *text;
	guint n = count_shown_logs(lv);

	lv->scan_idle = 0;
	text = g_strdup_printf(ngettext("%u log matches", "%u logs match", n), n);
	search_set_status(lv, text, FALSE);
	g_free(text);
	populate_log_tree(lv);
}

/* Pidgin 2's search: the log's text contains the term (case-insensitive). */
static gboolean
log_contains(PurpleLog *log, const char *term)
{
	char *read = purple_log_read(log, NULL);
	gboolean found = FALSE;

	if (read != NULL && *read != '\0') {
		char *plain = purple_markup_strip_html(read);
		found = purple_strcasestr(plain, term) != NULL;
		g_free(plain);
	}
	g_free(read);
	return found;
}

static gboolean
scan_idle_cb(gpointer data)
{
	PidginLogViewer *lv = data;
	gint64 start = g_get_monotonic_time();
	char *text;

	while (lv->scan_queue != NULL) {
		PurpleLog *log = lv->scan_queue->data;

		lv->scan_queue = g_list_delete_link(lv->scan_queue, lv->scan_queue);
		if (log_contains(log, lv->search))
			g_hash_table_add(lv->matches, log);
		if (g_get_monotonic_time() - start > SCAN_SLICE_US)
			break;
	}

	if (lv->scan_queue == NULL) {
		search_finished(lv);
		return G_SOURCE_REMOVE;
	}

	text = g_strdup_printf(_("Searching... %u of %u logs"),
		lv->scan_total - g_list_length(lv->scan_queue), lv->scan_total);
	search_set_status(lv, text, TRUE);
	g_free(text);
	return G_SOURCE_CONTINUE;
}

/*
 * The index part: for each conversation shown, the log files that have
 * rows matching @term. Logs whose file is indexed (and unchanged since)
 * are decided here; the others go to the linear scan.
 */
static void
search_index(PidginLogViewer *lv, PidginMessageIndex *idx, const char *term)
{
	GHashTable *convs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		(GDestroyNotify)g_hash_table_unref);
	GList *l;

	for (l = lv->logs; l != NULL; l = l->next) {
		PurpleLog *log = l->data;
		char *rel = log_get_rel_path(log);
		char *akey, *ckey, *key;
		GHashTable *files;
		gint64 mtime = 0, size = 0, offset = 0;
		GStatBuf st;

		if (rel == NULL || log->type == PURPLE_LOG_SYSTEM ||
		    !pidgin_message_index_get_file_state(idx, rel, &mtime, &size, &offset) ||
		    g_stat(log_get_path(log), &st) != 0 || (gint64)st.st_size != size) {
			/* Not indexed, or changed since: scan it */
			lv->scan_queue = g_list_prepend(lv->scan_queue, log);
			g_free(rel);
			continue;
		}

		akey = pidgin_message_index_account_key(log->account);
		ckey = pidgin_message_index_conv_key(log->account, log->name);
		key = g_strconcat(akey, "\n", ckey, NULL);
		files = g_hash_table_lookup(convs, key);
		if (files == NULL) {
			GPtrArray *rows = pidgin_message_index_search(idx, term, akey, ckey,
			                                              INDEX_SEARCH_LIMIT);
			guint i;

			files = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
			for (i = 0; rows != NULL && i < rows->len; i++) {
				PidginIndexedMessage *m = g_ptr_array_index(rows, i);
				if (m->log_file != NULL)
					g_hash_table_add(files, g_strdup(m->log_file));
			}
			/* Saturated: a file without rows here may still match */
			if (rows != NULL && rows->len >= INDEX_SEARCH_LIMIT)
				g_hash_table_insert(files, g_strdup(""), NULL);
			if (rows != NULL)
				g_ptr_array_unref(rows);
			g_hash_table_insert(convs, key, files);
		} else {
			g_free(key);
		}

		if (g_hash_table_contains(files, rel)) {
			g_hash_table_add(lv->matches, log);
			lv->n_indexed++;
		} else if (g_hash_table_contains(files, "")) {
			lv->scan_queue = g_list_prepend(lv->scan_queue, log);
		} else {
			lv->n_indexed++;
		}
		g_free(akey);
		g_free(ckey);
		g_free(rel);
	}

	g_hash_table_unref(convs);
}

static void
search_start(PidginLogViewer *lv, const char *term)
{
	PidginMessageIndex *idx;

	search_stop(lv);
	g_free(lv->search);
	lv->search = g_strdup(term);
	g_clear_pointer(&lv->matches, g_hash_table_unref);
	lv->matches = g_hash_table_new(g_direct_hash, g_direct_equal);
	lv->n_indexed = 0;

	/* The index is optional (NULL if the database can't be opened). */
	idx = pidgin_message_index_get_default();
	if (idx != NULL)
		search_index(lv, idx, term);
	else
		lv->scan_queue = g_list_copy(lv->logs);

	lv->scan_queue = g_list_reverse(lv->scan_queue);
	lv->scan_total = g_list_length(lv->scan_queue);
	purple_debug_info("gtklog", "search \"%s\": %u logs from the index, %u to scan\n",
	                  term, lv->n_indexed, lv->scan_total);

	if (lv->scan_queue == NULL) {
		search_finished(lv);
		return;
	}
	search_set_status(lv, _("Searching..."), TRUE);
	lv->scan_idle = g_idle_add(scan_idle_cb, lv);
}

static void
search_clear(PidginLogViewer *lv)
{
	search_stop(lv);
	search_set_status(lv, NULL, FALSE);
	if (lv->search == NULL)
		return;
	g_clear_pointer(&lv->search, g_free);
	g_clear_pointer(&lv->matches, g_hash_table_unref);
	populate_log_tree(lv);
}

static void
search_activate_cb(GtkSearchEntry *entry, PidginLogViewer *lv)
{
	const char *term = gtk_editable_get_text(GTK_EDITABLE(entry));
	char *stripped = g_strstrip(g_strdup(term));

	if (*stripped == '\0')
		search_clear(lv);
	else if (lv->search == NULL || strcmp(lv->search, stripped) != 0)
		search_start(lv, stripped);
	g_free(stripped);
}

static void
search_changed_cb(GtkSearchEntry *entry, PidginLogViewer *lv)
{
	/* An emptied entry shows every log again; a term is searched on Enter. */
	if (*gtk_editable_get_text(GTK_EDITABLE(entry)) == '\0')
		search_clear(lv);
}

static void
search_stop_cb(GtkSearchEntry *entry, PidginLogViewer *lv)
{
	gtk_editable_set_text(GTK_EDITABLE(entry), "");
	search_clear(lv);
}

/**************************************************************************
 * Delete and Browse
 **************************************************************************/

typedef struct {
	PidginLogViewer *lv;
	PurpleLog *log;
} DeleteData;

static void
delete_response_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	DeleteData *dd = data;
	PidginLogViewer *lv = dd->lv;
	PurpleLog *log = dd->log;
	int button;

	button = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, NULL);
	g_free(dd);

	/* The viewer may be gone (its cancellable cancelled the dialog). */
	if (button != 1 || !viewer_exists(lv) || g_list_find(lv->logs, log) == NULL)
		return;

	if (!purple_log_delete(log)) {
		purple_notify_error(NULL, NULL, _("Log Deletion Failed"),
		                    _("Check permissions and try again."));
		return;
	}

	lv->logs = g_list_remove(lv->logs, log);
	lv->scan_queue = g_list_remove(lv->scan_queue, log);
	if (lv->matches != NULL)
		g_hash_table_remove(lv->matches, log);
	if (lv->shown == log)
		lv->shown = NULL;
	purple_log_free(log);
	populate_log_tree(lv);
}

static void
delete_cb(GtkWidget *button, PidginLogViewer *lv)
{
	PurpleLog *log = lv->shown;
	const char *time;
	const char *name;
	GtkAlertDialog *alert;
	const char *buttons[] = { _("Cancel"), _("Delete"), NULL };
	DeleteData *dd;
	char *tmp;

	if (log == NULL || !purple_log_is_deletable(log))
		return;

	time = log_get_date(log);
	if (log->type == PURPLE_LOG_IM)
	{
		PurpleBuddy *buddy = purple_find_buddy(log->account, log->name);
		if (buddy != NULL)
			name = purple_buddy_get_contact_alias(buddy);
		else
			name = log->name;

		tmp = g_strdup_printf(_("Are you sure you want to permanently delete the log of the "
		                        "conversation with %s which started at %s?"), name, time);
	}
	else if (log->type == PURPLE_LOG_CHAT)
	{
		PurpleChat *chat = purple_blist_find_chat(log->account, log->name);
		if (chat != NULL)
			name = purple_chat_get_name(chat);
		else
			name = log->name;

		tmp = g_strdup_printf(_("Are you sure you want to permanently delete the log of the "
		                        "conversation in %s which started at %s?"), name, time);
	}
	else
	{
		tmp = g_strdup_printf(_("Are you sure you want to permanently delete the system log "
		                        "which started at %s?"), time);
	}

	alert = gtk_alert_dialog_new("%s", _("Delete Log?"));
	gtk_alert_dialog_set_detail(alert, tmp);
	gtk_alert_dialog_set_buttons(alert, buttons);
	gtk_alert_dialog_set_cancel_button(alert, 0);
	gtk_alert_dialog_set_default_button(alert, 0);
	dd = g_new0(DeleteData, 1);
	dd->lv = lv;
	dd->log = log;
	gtk_alert_dialog_choose(alert, GTK_WINDOW(lv->window), lv->cancellable,
	                        delete_response_cb, dd);
	g_object_unref(alert);
	g_free(tmp);
}

static void
browse_done_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	GError *error = NULL;

	if (!gtk_file_launcher_launch_finish(GTK_FILE_LAUNCHER(source), result, &error)) {
		if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) &&
		    !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			purple_debug_warning("gtklog", "could not open the log folder: %s\n",
			                     error->message);
		g_error_free(error);
	}
}

/* Opens the log folder in the file manager (Pidgin 2 did so on Windows). */
static void
browse_cb(GtkWidget *button, PidginLogViewer *lv)
{
	PurpleLog *log = lv->shown;
	GtkFileLauncher *launcher;
	GFile *file;
	char *logdir = NULL;

	if (log != NULL)
		logdir = purple_log_get_log_dir(log->type, log->name, log->account);
	if (logdir == NULL)
		logdir = g_build_filename(purple_user_dir(), "logs", NULL);

	file = g_file_new_for_path(logdir);
	launcher = gtk_file_launcher_new(file);
	gtk_file_launcher_launch(launcher, GTK_WINDOW(lv->window), lv->cancellable,
	                         browse_done_cb, NULL);
	g_object_unref(launcher);
	g_object_unref(file);
	g_free(logdir);
}

/**************************************************************************
 * The window
 **************************************************************************/

static void
close_cb(GtkWidget *button, PidginLogViewer *lv)
{
	gtk_window_destroy(GTK_WINDOW(lv->window));
}

static void
viewer_destroy_cb(GtkWidget *window, PidginLogViewer *lv)
{
	if (lv == syslog_viewer)
		syslog_viewer = NULL;
	log_viewers = g_list_remove(log_viewers, lv);

	search_stop(lv);
	g_cancellable_cancel(lv->cancellable);
	g_clear_object(&lv->cancellable);
	purple_request_close_with_handle(lv);

	g_clear_object(&lv->selection);
	g_clear_pointer(&lv->matches, g_hash_table_unref);
	g_list_free_full(lv->logs, (GDestroyNotify)purple_log_free);
	g_free(lv->search);
	g_free(lv->name);
	g_free(lv);
}

static PidginLogViewer *
find_viewer(PurpleLogType type, const char *name, PurpleAccount *account,
            PurpleContact *contact)
{
	GList *l;

	for (l = log_viewers; l != NULL; l = l->next) {
		PidginLogViewer *lv = l->data;

		if (contact != NULL || lv->contact != NULL) {
			if (lv->contact == contact)
				return lv;
			continue;
		}
		if (lv->account == account && lv->type == type) {
			char *normal = g_strdup(purple_normalize(account, name));
			gboolean match = purple_strequal(normal, purple_normalize(account, lv->name));
			g_free(normal);
			if (match)
				return lv;
		}
	}
	return NULL;
}

/* Pidgin 2's "No logs were found" notice, with the pref that explains it. */
static void
notify_no_logs(PurpleLogType type, gboolean syslog, const char *title)
{
	const char *log_preferences = NULL;

	if (syslog) {
		if (!purple_prefs_get_bool("/purple/logging/log_system"))
			log_preferences = _("System events will only be logged if the \"Log all status changes to system log\" preference is enabled.");
	} else if (type == PURPLE_LOG_IM) {
		if (!purple_prefs_get_bool("/purple/logging/log_ims"))
			log_preferences = _("Instant messages will only be logged if the \"Log all instant messages\" preference is enabled.");
	} else if (type == PURPLE_LOG_CHAT) {
		if (!purple_prefs_get_bool("/purple/logging/log_chats"))
			log_preferences = _("Chats will only be logged if the \"Log all chats\" preference is enabled.");
	}

	purple_notify_info(NULL, title, _("No logs were found"), log_preferences);
}

static PidginLogViewer *
display_log_viewer(PidginLogViewer *lv, const char *title, GtkWidget *icon, gint64 log_size)
{
	GtkWidget *win, *vbox, *title_box, *pane, *sw, *rbox, *hbox;
	GtkListItemFactory *factory;
	char *text, *win_title;

	/* Window ***********/
	if (log_size > 0) {
		char *sz = purple_str_size_to_units(log_size);
		win_title = g_strdup_printf("%s (%s)", title, sz);
		g_free(sz);
	} else {
		win_title = g_strdup(title);
	}
	lv->window = win = pidgin_dialog_new(win_title, NULL, "log_viewer", TRUE);
	g_free(win_title);
	gtk_window_set_default_size(GTK_WINDOW(win), 760, 540);
	g_signal_connect(win, "destroy", G_CALLBACK(viewer_destroy_cb), lv);
	lv->cancellable = g_cancellable_new();

	vbox = pidgin_dialog_get_content_area(win);

	/* Icon and label *************/
	title_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	gtk_box_append(GTK_BOX(vbox), title_box);
	if (icon != NULL)
		gtk_box_append(GTK_BOX(title_box), icon);

	lv->label = gtk_label_new(NULL);
	text = g_markup_printf_escaped("<span size='larger' weight='bold'>%s</span>", title);
	gtk_label_set_markup(GTK_LABEL(lv->label), text);
	g_free(text);
	gtk_label_set_xalign(GTK_LABEL(lv->label), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(lv->label), PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand(lv->label, TRUE);
	gtk_box_append(GTK_BOX(title_box), lv->label);

	/* Pane *************/
	pane = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_widget_set_vexpand(pane, TRUE);
	gtk_paned_set_position(GTK_PANED(pane), 230);
	gtk_paned_set_shrink_start_child(GTK_PANED(pane), FALSE);
	gtk_box_append(GTK_BOX(vbox), pane);

	/* List *************/
	lv->selection = gtk_single_selection_new(NULL);
	gtk_single_selection_set_autoselect(lv->selection, FALSE);
	gtk_single_selection_set_can_unselect(lv->selection, TRUE);
	g_signal_connect(lv->selection, "selection-changed",
	                 G_CALLBACK(log_selection_changed_cb), lv);

	factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(node_setup_cb), NULL);
	g_signal_connect(factory, "bind", G_CALLBACK(node_bind_cb), NULL);
	g_signal_connect(factory, "unbind", G_CALLBACK(node_unbind_cb), NULL);
	lv->list_view = gtk_list_view_new(GTK_SELECTION_MODEL(g_object_ref(lv->selection)),
	                                  factory);
	g_signal_connect(lv->list_view, "activate", G_CALLBACK(log_row_activated_cb), lv);
	pidgin_set_accessible_label(lv->list_view, lv->label);
	sw = pidgin_make_scrollable(lv->list_view, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC,
	                            200, -1);
	gtk_paned_set_start_child(GTK_PANED(pane), sw);

	/* A fancy little box ************/
	rbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BOX_SPACE);
	gtk_paned_set_end_child(GTK_PANED(pane), rbox);

	/* Viewer ************/
	lv->view = pidgin_create_message_view();
	gtk_widget_set_name(lv->view, "pidgin_log_view");
	gtk_widget_set_size_request(lv->view, 320, 200);
	gtk_widget_set_vexpand(lv->view, TRUE);
	gtk_widget_set_hexpand(lv->view, TRUE);
	/* A log is shown whole, never trimmed */
	pidgin_message_view_set_scrollback(PIDGIN_MESSAGE_VIEW(lv->view), 0);
	gtk_box_append(GTK_BOX(rbox), lv->view);

	/* Search box **********/
	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	gtk_box_append(GTK_BOX(rbox), hbox);
	lv->entry = gtk_search_entry_new();
	gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(lv->entry), _("Search"));
	gtk_widget_set_hexpand(lv->entry, TRUE);
	gtk_box_append(GTK_BOX(hbox), lv->entry);
	lv->spinner = gtk_spinner_new();
	gtk_widget_set_visible(lv->spinner, FALSE);
	gtk_box_append(GTK_BOX(hbox), lv->spinner);
	lv->status = gtk_label_new(NULL);
	gtk_widget_add_css_class(lv->status, "dim-label");
	gtk_box_append(GTK_BOX(hbox), lv->status);
	g_signal_connect(lv->entry, "activate", G_CALLBACK(search_activate_cb), lv);
	g_signal_connect(lv->entry, "search-changed", G_CALLBACK(search_changed_cb), lv);
	g_signal_connect(lv->entry, "stop-search", G_CALLBACK(search_stop_cb), lv);
	gtk_search_entry_set_key_capture_widget(GTK_SEARCH_ENTRY(lv->entry), lv->list_view);

	/* Log size (left of the buttons) ************/
	lv->size_label = gtk_label_new(NULL);
	if (log_size > 0) {
		char *sz_txt = purple_str_size_to_units(log_size);
		text = g_markup_printf_escaped("<span weight='bold'>%s</span> %s",
		                               _("Total log size:"), sz_txt);
		gtk_label_set_markup(GTK_LABEL(lv->size_label), text);
		g_free(sz_txt);
		g_free(text);
	}
	gtk_label_set_xalign(GTK_LABEL(lv->size_label), 0.0);
	gtk_widget_set_hexpand(lv->size_label, TRUE);
	gtk_box_prepend(GTK_BOX(pidgin_dialog_get_action_area(win)), lv->size_label);
	gtk_widget_set_halign(pidgin_dialog_get_action_area(win), GTK_ALIGN_FILL);

	/* Buttons ************/
	pidgin_dialog_add_button(win, _("_Browse logs folder"), G_CALLBACK(browse_cb), lv);
	lv->delete_button = pidgin_dialog_add_button(win, _("_Delete Log..."),
		G_CALLBACK(delete_cb), lv);
	pidgin_dialog_add_button(win, _("_Close"), G_CALLBACK(close_cb), lv);

	populate_log_tree(lv);

	gtk_window_present(GTK_WINDOW(win));
	return lv;
}

static gint64
logs_total_size(GList *logs)
{
	gint64 total = 0;

	for (; logs != NULL; logs = logs->next)
		total += purple_log_get_size(logs->data);
	return total;
}

void
pidgin_log_show(PurpleLogType type, const char *buddyname, PurpleAccount *account)
{
	PidginLogViewer *lv;
	const char *name = buddyname;
	char *title;
	GList *logs;

	g_return_if_fail(account != NULL);
	g_return_if_fail(buddyname != NULL);

	if ((lv = find_viewer(type, buddyname, account, NULL)) != NULL) {
		gtk_window_present(GTK_WINDOW(lv->window));
		return;
	}

	if (type == PURPLE_LOG_CHAT) {
		PurpleChat *chat = purple_blist_find_chat(account, buddyname);

		if (chat != NULL)
			name = purple_chat_get_name(chat);
		title = g_strdup_printf(_("Conversations in %s"), name);
	} else {
		PurpleBuddy *buddy = purple_find_buddy(account, buddyname);

		if (buddy != NULL)
			name = purple_buddy_get_contact_alias(buddy);
		title = g_strdup_printf(_("Conversations with %s"), name);
	}

	logs = purple_log_get_logs(type, buddyname, account);
	if (logs == NULL) {
		notify_no_logs(type, FALSE, title);
		g_free(title);
		return;
	}

	lv = g_new0(PidginLogViewer, 1);
	lv->type = type;
	lv->name = g_strdup(buddyname);
	lv->account = account;
	lv->logs = logs;
	log_viewers = g_list_prepend(log_viewers, lv);

	display_log_viewer(lv, title,
		pidgin_create_prpl_image(account, NULL, PIDGIN_PRPL_ICON_MEDIUM),
		purple_log_get_total_size(type, buddyname, account));
	g_free(title);
}

void
pidgin_log_show_contact(PurpleContact *contact)
{
	PidginLogViewer *lv;
	PurpleBlistNode *child;
	GList *logs = NULL;
	GtkWidget *image;
	const char *name = NULL;
	char *title;
	gint64 total_log_size = 0;

	g_return_if_fail(contact != NULL);

	if ((lv = find_viewer(PURPLE_LOG_IM, NULL, NULL, contact)) != NULL) {
		gtk_window_present(GTK_WINDOW(lv->window));
		return;
	}

	for (child = purple_blist_node_get_first_child((PurpleBlistNode*)contact) ;
	     child != NULL ;
	     child = purple_blist_node_get_sibling_next(child)) {
		const char *buddy_name;
		PurpleAccount *account;

		if (!PURPLE_BLIST_NODE_IS_BUDDY(child))
			continue;

		buddy_name = purple_buddy_get_name((PurpleBuddy *)child);
		account = purple_buddy_get_account((PurpleBuddy *)child);
		logs = g_list_concat(purple_log_get_logs(PURPLE_LOG_IM, buddy_name, account), logs);
		total_log_size += purple_log_get_total_size(PURPLE_LOG_IM, buddy_name, account);
	}
	logs = g_list_sort(logs, purple_log_compare);

	if (contact->alias != NULL)
		name = contact->alias;
	else if (contact->priority != NULL)
		name = purple_buddy_get_contact_alias(contact->priority);

	/* This will happen if the contact doesn't have an alias,
	 * and none of the contact's buddies are online.
	 * There is probably a better way to deal with this. */
	if (name == NULL) {
		if (contact->node.child != NULL && PURPLE_BLIST_NODE_IS_BUDDY(contact->node.child))
			name = purple_buddy_get_contact_alias((PurpleBuddy *) contact->node.child);
		if (name == NULL)
			name = "";
	}

	title = g_strdup_printf(_("Conversations with %s"), name);
	if (logs == NULL) {
		notify_no_logs(PURPLE_LOG_IM, FALSE, title);
		g_free(title);
		return;
	}

	lv = g_new0(PidginLogViewer, 1);
	lv->type = PURPLE_LOG_IM;
	lv->contact = contact;
	lv->logs = logs;
	log_viewers = g_list_prepend(log_viewers, lv);

	image = gtk_image_new_from_icon_name("pidgin-status-person");
	gtk_image_set_pixel_size(GTK_IMAGE(image), 22);
	display_log_viewer(lv, title, image, total_log_size);
	g_free(title);
}

static GList *
get_system_logs(void)
{
	GList *accounts, *logs = NULL;

	for (accounts = purple_accounts_get_all(); accounts != NULL; accounts = accounts->next) {
		PurpleAccount *account = (PurpleAccount *)accounts->data;

		if (purple_find_prpl(purple_account_get_protocol_id(account)) == NULL)
			continue;

		logs = g_list_concat(purple_log_get_system_logs(account), logs);
	}
	return g_list_sort(logs, purple_log_compare);
}

void
pidgin_syslog_show(void)
{
	GList *logs;

	if (syslog_viewer != NULL) {
		gtk_window_present(GTK_WINDOW(syslog_viewer->window));
		return;
	}

	logs = get_system_logs();
	if (logs == NULL) {
		notify_no_logs(PURPLE_LOG_SYSTEM, TRUE, _("System Log"));
		return;
	}

	syslog_viewer = g_new0(PidginLogViewer, 1);
	syslog_viewer->type = PURPLE_LOG_SYSTEM;
	syslog_viewer->syslog = TRUE;
	syslog_viewer->logs = logs;
	display_log_viewer(syslog_viewer, _("System Log"), NULL, logs_total_size(logs));
}

/****************************************************************************
 * GTK+ LOG SUBSYSTEM *******************************************************
 ****************************************************************************/

void *
pidgin_log_get_handle(void)
{
	static int handle;

	return &handle;
}

void
pidgin_log_init(void)
{
	void *handle = pidgin_log_get_handle();

	/* As in Pidgin 2; the viewer is opaque to plugins in pidgin4. */
	purple_signal_register(handle, "log-displaying",
	                     purple_marshal_VOID__POINTER_POINTER,
	                     NULL, 2,
	                     purple_value_new(PURPLE_TYPE_BOXED,
	                                    "PidginLogViewer *"),
	                     purple_value_new(PURPLE_TYPE_SUBTYPE,
	                                    PURPLE_SUBTYPE_LOG));
}

void
pidgin_log_uninit(void)
{
	while (log_viewers != NULL) {
		PidginLogViewer *lv = log_viewers->data;
		/* The destroy handler removes it from the list. */
		gtk_window_destroy(GTK_WINDOW(lv->window));
	}
	if (syslog_viewer != NULL)
		gtk_window_destroy(GTK_WINDOW(syslog_viewer->window));

	purple_signals_unregister_by_instance(pidgin_log_get_handle());
}

/**************************************************************************
 * Selftest
 **************************************************************************/

#define SELFTEST_MAX_BUDDIES 300
#define SELFTEST_MIN_SIZE (50 * 1024)

static guint
count_leaves(PidginLogViewer *lv)
{
	/* Leaves shown in the tree (with every group expanded) */
	guint pos = 0, n = 0;
	GtkTreeListRow *row;

	while ((row = gtk_tree_list_model_get_row(lv->tree, pos++)) != NULL) {
		PidginLogNode *node = gtk_tree_list_row_get_item(row);

		if (node->log != NULL)
			n++;
		else
			gtk_tree_list_row_set_expanded(row, TRUE);
		g_object_unref(node);
		g_object_unref(row);
	}
	return n;
}

static gboolean
search_done(gpointer data)
{
	PidginLogViewer *lv = data;

	return lv->scan_idle == 0;
}

/* A word of 4+ letters from the messages shown, to search for. */
static char *
pick_search_word(PidginLogViewer *lv)
{
	GListModel *model = pidgin_message_view_get_model(PIDGIN_MESSAGE_VIEW(lv->view));
	guint i, n = g_list_model_get_n_items(model);
	char *word = NULL;

	for (i = 0; i < n && word == NULL; i++) {
		PidginMessage *msg = g_list_model_get_item(model, i);
		const char *text = pidgin_message_get_kind(msg) == PIDGIN_MESSAGE_KIND_NORMAL
			? pidgin_message_get_plain_text(msg) : NULL;
		char **words, **w;

		if (text != NULL && !(pidgin_message_get_flags(msg) & PURPLE_MESSAGE_RAW)) {
			words = g_strsplit_set(text, " \t\n.,;:!?()[]{}\"'<>/\\", -1);
			for (w = words; *w != NULL && word == NULL; w++) {
				const char *c;
				gboolean alpha = strlen(*w) >= 4;

				for (c = *w; *c && alpha; c++)
					alpha = g_ascii_isalpha(*c);
				if (alpha)
					word = g_strdup(*w);
			}
			g_strfreev(words);
		}
		g_object_unref(msg);
	}
	return word;
}

static void
selftest_buddy_log(void)
{
	PurpleBlistNode *node;
	PurpleBuddy *found = NULL, *first = NULL;
	PidginLogViewer *lv;
	const char *path;
	GStatBuf before, after;
	char *word, *file = NULL;
	guint checked = 0, visible, leaves;
	int size;
	gboolean newest_matches;

	for (node = purple_blist_get_root(); node != NULL && found == NULL &&
	     checked < SELFTEST_MAX_BUDDIES; node = purple_blist_node_next(node, TRUE)) {
		PurpleBuddy *buddy;

		if (!PURPLE_BLIST_NODE_IS_BUDDY(node))
			continue;
		buddy = (PurpleBuddy *)node;
		checked++;
		size = purple_log_get_total_size(PURPLE_LOG_IM, purple_buddy_get_name(buddy),
		                                 purple_buddy_get_account(buddy));
		/* The first with some history (several logs) to search in, else
		 * the first with any log. */
		if (size >= SELFTEST_MIN_SIZE)
			found = buddy;
		else if (size > 0 && first == NULL)
			first = buddy;
	}
	if (found == NULL)
		found = first;
	if (found == NULL) {
		pidgin_selftest_log("log", "no buddy with logs among the first %u; skipped", checked);
		return;
	}

	pidgin_log_show(PURPLE_LOG_IM, purple_buddy_get_name(found), purple_buddy_get_account(found));
	pidgin_selftest_iterate(500);
	lv = find_viewer(PURPLE_LOG_IM, purple_buddy_get_name(found),
	                 purple_buddy_get_account(found), NULL);
	if (lv == NULL) {
		pidgin_selftest_fail("log", "no viewer for %s", purple_buddy_get_name(found));
		return;
	}

	/* The newest log is selected and shown */
	if (lv->shown == NULL || lv->shown != lv->logs->data) {
		pidgin_selftest_fail("log", "the newest log is not shown");
		gtk_window_destroy(GTK_WINDOW(lv->window));
		return;
	}
	path = log_get_path(lv->shown);
	if (path != NULL) {
		file = g_strdup(path);
		if (g_stat(file, &before) != 0)
			g_clear_pointer(&file, g_free);
	}
	visible = pidgin_message_view_get_n_visible(PIDGIN_MESSAGE_VIEW(lv->view));
	pidgin_selftest_log("log", "%s: %u logs, the newest has %u rows",
	                    purple_buddy_get_name(found), g_list_length(lv->logs), visible);
	if (visible == 0)
		pidgin_selftest_fail("log", "the newest log shows no rows");

	/* Search for a word of that log */
	word = pick_search_word(lv);
	if (word == NULL) {
		pidgin_selftest_log("log", "no word to search for in the newest log");
	} else {
		gtk_editable_set_text(GTK_EDITABLE(lv->entry), word);
		g_signal_emit_by_name(lv->entry, "activate");
		if (!pidgin_selftest_wait(search_done, lv, 120000))
			pidgin_selftest_fail("log", "the search for \"%s\" did not finish", word);
		newest_matches = lv->matches != NULL && g_hash_table_contains(lv->matches, lv->logs->data);
		leaves = count_leaves(lv);
		pidgin_selftest_log("log", "search \"%s\": %u logs in the tree (%u decided by the "
		                    "index, %u scanned)", word, leaves, lv->n_indexed, lv->scan_total);
		if (leaves == 0)
			pidgin_selftest_fail("log", "the search for \"%s\" found no log", word);
		if (!newest_matches)
			pidgin_selftest_fail("log", "the newest log does not match \"%s\"", word);
		if (lv->shown != NULL &&
		    pidgin_message_view_get_n_visible(PIDGIN_MESSAGE_VIEW(lv->view)) == 0)
			pidgin_selftest_log("log", "note: no row contains \"%s\" as typed", word);

		/* Clearing the search shows every log again */
		gtk_editable_set_text(GTK_EDITABLE(lv->entry), "");
		search_clear(lv);
		if (count_shown_logs(lv) != g_list_length(lv->logs))
			pidgin_selftest_fail("log", "clearing the search does not show every log");
		g_free(word);
	}

	gtk_window_destroy(GTK_WINDOW(lv->window));
	pidgin_selftest_iterate(100);

	/* Read-only: the viewed log file is unchanged */
	if (file != NULL) {
		if (g_stat(file, &after) != 0 || after.st_size != before.st_size ||
		    after.st_mtime != before.st_mtime)
			pidgin_selftest_fail("log", "the log file %s was modified", file);
		g_free(file);
	}

	/* The contact's logs (all its buddies merged) */
	if (purple_buddy_get_contact(found) != NULL) {
		PurpleContact *contact = purple_buddy_get_contact(found);

		pidgin_log_show_contact(contact);
		pidgin_selftest_iterate(300);
		lv = find_viewer(PURPLE_LOG_IM, NULL, NULL, contact);
		if (lv == NULL) {
			pidgin_selftest_fail("log", "no contact viewer");
		} else {
			pidgin_selftest_log("log", "contact: %u logs, %s", g_list_length(lv->logs),
			                    lv->shown ? "newest shown" : "nothing shown");
			if (lv->shown == NULL)
				pidgin_selftest_fail("log", "the contact viewer shows no log");
			gtk_window_destroy(GTK_WINDOW(lv->window));
			pidgin_selftest_iterate(100);
		}
	}
}

void
pidgin_log_selftest(void)
{
	GList *syslogs = get_system_logs();
	guint n_syslogs = g_list_length(syslogs);

	g_list_free_full(syslogs, (GDestroyNotify)purple_log_free);

	/* The system log (skipped without logs: that would be a notice) */
	if (n_syslogs > 0) {
		guint leaves;

		pidgin_syslog_show();
		pidgin_selftest_iterate(500);
		if (syslog_viewer == NULL) {
			pidgin_selftest_fail("log", "the system log did not open");
		} else {
			leaves = count_leaves(syslog_viewer);
			pidgin_selftest_log("log", "system log: %u logs, %u in the tree, newest %s",
			                    n_syslogs, leaves, syslog_viewer->shown ? "shown" : "not shown");
			if (leaves != n_syslogs)
				pidgin_selftest_fail("log", "the system log tree has %u of %u logs",
				                     leaves, n_syslogs);
			if (syslog_viewer->shown == NULL)
				pidgin_selftest_fail("log", "no system log is shown");
			gtk_window_destroy(GTK_WINDOW(syslog_viewer->window));
			pidgin_selftest_iterate(100);
		}
	} else {
		pidgin_selftest_log("log", "no system logs; the system log viewer is skipped");
	}

	selftest_buddy_log();

	if (log_viewers != NULL || syslog_viewer != NULL)
		pidgin_selftest_fail("log", "viewers left open");
}
