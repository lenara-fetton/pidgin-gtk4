/*
 * pidgin4: History (M7 port of pidgin/plugins/history.c).
 *
 * Puts the last logged conversation into new conversations, a la
 * Everybuddy (and then stolen by Trillian "Pro").
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * In Pidgin 2 the whole last log went into the GtkIMHtml as one block of
 * HTML. Here each log line becomes a PidginMessage (parsed with
 * PidginMarkup, colours dropped as before) with the CSS class "history",
 * which style.css greys out, followed by a separator item. They are
 * prepended, so they sit before any live message.
 *
 * Only the tail of the last log is shown (/pidgin4/plugins/history/lines,
 * default 100). The message index (messages.db) knows where each indexed
 * line of a log starts, so with it only that tail of the file is read;
 * without it (or for lines it hasn't indexed yet) the whole file is read.
 */
#include "pidgin4-plugin.h"

#include "pidginbackfill.h"
#include "pidginmessageindex.h"

#define HISTORY_PLUGIN_ID "gtk-history"

#define HISTORY_PREFS PIDGIN4_PREFS_ROOT "/plugins/history"
#define PREF_LINES HISTORY_PREFS "/lines"

/** The CSS class of history rows (and of the separator after them). */
#define HISTORY_CLASS "history"

static PurplePlugin *my_plugin = NULL;

/*
 * The HTML body of a log line as libpurple's html logger writes it
 *   <span style="color: #A82F2F"><span style="font-size: smaller">(time)</span> <b>who:</b></span> body<br/>
 *   <span style="font-size: smaller">(time)</span><b> system text</b><br/>
 * (or the older <font> forms). NULL if it doesn't look like one.
 */
static char *
line_body_html(const char *line, gboolean *action)
{
	const char *p, *end, *b, *b_end;

	*action = FALSE;
	p = strstr(line, ")</span>");
	if (p != NULL)
		p += strlen(")</span>");
	else if ((p = strstr(line, ")</font>")) != NULL)
		p += strlen(")</font>");
	else
		return NULL;

	end = p + strlen(p);
	while (end > p && g_ascii_isspace(end[-1]))
		end--;
	if (end - p >= 5 && g_ascii_strncasecmp(end - 5, "<br/>", 5) == 0)
		end -= 5;
	else if (end - p >= 4 && g_ascii_strncasecmp(end - 4, "<br>", 4) == 0)
		end -= 4;

	while (p < end && *p == ' ')
		p++;
	if (g_ascii_strncasecmp(p, "<b>", 3) != 0)
		return g_strndup(p, end - p);

	b = p + 3;
	b_end = strstr(b, "</b>");
	if (b_end == NULL || b_end > end)
		return NULL;
	while (b < b_end && *b == ' ')
		b++;

	if ((b_end > b && b_end[-1] == ':') || g_str_has_prefix(b, "***")) {
		/* who: body */
		const char *after = b_end + 4;

		*action = g_str_has_prefix(b, "***");
		if (g_str_has_prefix(after, "</span>"))
			after += strlen("</span>");
		else if (g_str_has_prefix(after, "</font>"))
			after += strlen("</font>");
		while (after < end && *after == ' ')
			after++;
		if (after > end)
			after = end;
		return g_strndup(after, end - after);
	}

	/* a system line: the bold text is the message */
	return g_strndup(b, b_end - b);
}

static PidginMessage *
message_from_line(const char *line, gboolean html, GDateTime *file_start,
                  GDateTime **last_time, const char *protocol_sml)
{
	PidginIndexedMessage *parsed = pidgin_indexed_message_new();
	PidginMarkupOptions options = { 0 };
	PidginMessage *msg = NULL;
	PurpleMessageFlags flags;
	gboolean ok, action = FALSE;
	char *body = NULL;

	if (html)
		ok = pidgin_backfill_parse_html_line(line, file_start, last_time, parsed);
	else
		ok = pidgin_backfill_parse_txt_line(line, file_start, last_time, parsed);
	if (!ok) {
		pidgin_indexed_message_free(parsed);
		return NULL;
	}

	if (html)
		body = line_body_html(line, &action);
	if (body == NULL)
		body = g_markup_escape_text(parsed->body ? parsed->body : "", -1);
	if (action) {
		char *tmp = g_strconcat("/me ", body, NULL);

		g_free(body);
		body = tmp;
	}

	flags = parsed->flags;
	if (!(flags & (PURPLE_MESSAGE_SEND | PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_SYSTEM |
	               PURPLE_MESSAGE_ERROR)))
		flags |= parsed->sender ? PURPLE_MESSAGE_RECV : PURPLE_MESSAGE_SYSTEM;
	/* never counted as new, never logged again */
	flags |= PURPLE_MESSAGE_NO_LOG | PURPLE_MESSAGE_DELAYED;

	msg = pidgin_message_new(parsed->sender, parsed->sender, body, flags,
	                         (time_t)parsed->time);
	options.flags = PIDGIN_MARKUP_NO_COLOURS;
	options.protocol_sml = protocol_sml;
	pidgin_message_set_parse_options(msg, &options);
	pidgin_message_add_css_class(msg, HISTORY_CLASS);

	g_free(body);
	pidgin_indexed_message_free(parsed);
	return msg;
}

/*
 * Where to start reading @path: the offset of the oldest of the last
 * @lines lines the index knows for that file, or 0 (read it all).
 */
static gint64
indexed_start_offset(PurpleAccount *account, const char *name, const char *path,
                     guint lines)
{
	PidginMessageIndex *idx = pidgin_message_index_get_default();
	char *logs_dir, *rel = NULL, *akey, *ckey;
	GPtrArray *rows;
	gint64 offset = 0;
	guint i, seen = 0;

	if (idx == NULL || path == NULL)
		return 0;

	logs_dir = g_build_filename(purple_user_dir(), "logs", NULL);
	if (g_str_has_prefix(path, logs_dir) && path[strlen(logs_dir)] == G_DIR_SEPARATOR)
		rel = g_strdup(path + strlen(logs_dir) + 1);
	g_free(logs_dir);
	if (rel == NULL)
		return 0;

	akey = pidgin_message_index_account_key(account);
	ckey = pidgin_message_index_conv_key(account, name);
	/* newest first; a few more than asked for, as other files interleave */
	rows = pidgin_message_index_get_recent(idx, akey, ckey, 0, lines * 2 + 20);
	for (i = 0; rows != NULL && i < rows->len && seen < lines; i++) {
		PidginIndexedMessage *row = g_ptr_array_index(rows, i);

		if (row->log_file == NULL || row->log_offset < 0 ||
		    !purple_strequal(row->log_file, rel))
			continue;
		offset = row->log_offset;
		seen++;
	}
	/* Fewer indexed lines than wanted: the index may not have the file's
	 * start yet (Pidgin 2 wrote it), so read all of it. */
	if (seen < lines)
		offset = 0;

	if (rows != NULL)
		g_ptr_array_unref(rows);
	g_free(akey);
	g_free(ckey);
	g_free(rel);
	return offset;
}

static char *
read_log_tail(const char *path, gint64 offset)
{
	GError *error = NULL;
	char *contents = NULL;
	gsize len = 0;

	if (offset <= 0) {
		if (!g_file_get_contents(path, &contents, &len, &error)) {
			purple_debug_warning("history", "%s\n", error->message);
			g_error_free(error);
		}
		return contents;
	} else {
		FILE *fp = g_fopen(path, "rb");
		GString *str;
		char buf[8192];
		size_t n;

		if (fp == NULL || fseeko(fp, offset, SEEK_SET) != 0) {
			if (fp != NULL)
				fclose(fp);
			return read_log_tail(path, 0);
		}
		str = g_string_new(NULL);
		while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
			g_string_append_len(str, buf, n);
		fclose(fp);
		return g_string_free(str, FALSE);
	}
}

/* The logs to show for a conversation, newest first (as Pidgin 2). */
static GList *
conversation_logs(PurpleConversation *c, const char **alias_out)
{
	PurpleAccount *account = purple_conversation_get_account(c);
	const char *name = purple_conversation_get_name(c);
	GList *logs = NULL;

	*alias_out = name;

	if (purple_conversation_get_type(c) == PURPLE_CONV_TYPE_IM) {
		GSList *buddies, *cur;

		/* If we're not logging, don't show anything.
		 * Otherwise, we might show a very old log. */
		if (!purple_prefs_get_bool("/purple/logging/log_ims"))
			return NULL;

		buddies = purple_find_buddies(account, name);
		if (buddies != NULL)
			*alias_out = purple_buddy_get_contact_alias((PurpleBuddy *)buddies->data);

		for (cur = buddies; cur != NULL; cur = cur->next) {
			PurpleBlistNode *node = cur->data;
			PurpleBlistNode *prev = purple_blist_node_get_sibling_prev(node);
			PurpleBlistNode *next = purple_blist_node_get_sibling_next(node);

			if (node != NULL && (prev != NULL || next != NULL)) {
				PurpleBlistNode *node2;
				PurpleBlistNode *parent = purple_blist_node_get_parent(node);

				*alias_out = purple_buddy_get_contact_alias((PurpleBuddy *)node);
				/* A contact with several buddies: all their logs. */
				for (node2 = purple_blist_node_get_first_child(parent); node2 != NULL;
				     node2 = purple_blist_node_get_sibling_next(node2)) {
					logs = g_list_concat(purple_log_get_logs(PURPLE_LOG_IM,
						purple_buddy_get_name((PurpleBuddy *)node2),
						purple_buddy_get_account((PurpleBuddy *)node2)), logs);
				}
				break;
			}
		}
		g_slist_free(buddies);

		if (logs == NULL)
			logs = purple_log_get_logs(PURPLE_LOG_IM, name, account);
		else
			logs = g_list_sort(logs, purple_log_compare);
	} else if (purple_conversation_get_type(c) == PURPLE_CONV_TYPE_CHAT) {
		if (!purple_prefs_get_bool("/purple/logging/log_chats"))
			return NULL;
		logs = purple_log_get_logs(PURPLE_LOG_CHAT, name, account);
	}
	return logs;
}

static void
historize(PurpleConversation *c)
{
	PidginMessageView *view = pidgin4_plugin_conv_view(c);
	PurpleLog *log;
	PurpleLogCommonLoggerData *data;
	GList *logs;
	const char *alias = NULL;
	const char *logger_id;
	char *contents, *escaped_alias, *header, *base;
	char **lines;
	GPtrArray *messages;
	GDateTime *file_start, *last_time = NULL;
	PidginMessage *msg;
	PurplePlugin *prpl;
	const char *sml = NULL;
	guint max_lines, n, first, i;
	gboolean html;
	gint64 offset;

	if (view == NULL)
		return;

	logs = conversation_logs(c, &alias);
	if (logs == NULL)
		return;

	log = logs->data;
	data = log->logger_data;
	logger_id = log->logger ? log->logger->id : NULL;
	if (data == NULL || data->path == NULL ||
	    !(purple_strequal(logger_id, "html") || purple_strequal(logger_id, "txt"))) {
		/* Some other logger: nothing we can split into lines. */
		purple_debug_info("history", "not showing a %s log\n",
		                  logger_id ? logger_id : "(unknown)");
		g_list_free_full(logs, (GDestroyNotify)purple_log_free);
		return;
	}
	html = purple_strequal(logger_id, "html");

	max_lines = MAX(purple_prefs_get_int(PREF_LINES), 1);
	offset = indexed_start_offset(log->account, log->name, data->path, max_lines);
	contents = read_log_tail(data->path, offset);
	if (contents == NULL) {
		g_list_free_full(logs, (GDestroyNotify)purple_log_free);
		return;
	}

	base = g_path_get_basename(data->path);
	file_start = pidgin_backfill_parse_file_name(base);
	g_free(base);
	if (file_start == NULL)
		file_start = g_date_time_new_from_unix_local(log->time);

	prpl = purple_find_prpl(purple_account_get_protocol_id(log->account));
	if (prpl != NULL)
		sml = prpl->info->name;

	lines = g_strsplit(contents, "\n", -1);
	g_free(contents);
	messages = g_ptr_array_new_with_free_func(g_object_unref);

	/* A header line, as Pidgin 2 showed it. */
	escaped_alias = g_markup_escape_text(alias, -1);
	header = g_strdup_printf(_("<b>Conversation with %s on %s:</b>"), escaped_alias,
		log->tm ? purple_date_format_full(log->tm)
		        : purple_date_format_full(localtime(&log->time)));
	msg = pidgin_message_new(NULL, NULL, header,
	                         PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG, log->time);
	pidgin_message_add_css_class(msg, HISTORY_CLASS);
	pidgin_message_add_css_class(msg, "history-header");
	g_ptr_array_add(messages, msg);
	g_free(header);
	g_free(escaped_alias);

	/* Parse every line (the timestamps depend on the ones before), keep
	 * the last max_lines. */
	{
		GPtrArray *parsed = g_ptr_array_new_with_free_func(g_object_unref);

		for (i = 0; lines[i] != NULL; i++) {
			if (*lines[i] == '\0')
				continue;
			msg = message_from_line(lines[i], html, file_start, &last_time, sml);
			if (msg != NULL)
				g_ptr_array_add(parsed, msg);
		}
		n = parsed->len;
		first = n > max_lines ? n - max_lines : 0;
		for (i = first; i < n; i++)
			g_ptr_array_add(messages, g_object_ref(g_ptr_array_index(parsed, i)));
		g_ptr_array_unref(parsed);
	}
	g_strfreev(lines);
	if (last_time != NULL)
		g_date_time_unref(last_time);
	g_date_time_unref(file_start);

	/* The separator between the history and what comes now. */
	msg = pidgin_message_new_marker();
	pidgin_message_add_css_class(msg, HISTORY_CLASS);
	pidgin_message_add_css_class(msg, "history-separator");
	g_ptr_array_add(messages, msg);

	pidgin_message_view_prepend_many(view, messages);
	pidgin_message_view_scroll_to_bottom(view);
	purple_debug_info("history", "%s: %u lines from %s (offset %" G_GINT64_FORMAT ")\n",
	                  purple_conversation_get_name(c), messages->len - 2, data->path, offset);

	g_ptr_array_unref(messages);
	g_list_free_full(logs, (GDestroyNotify)purple_log_free);
}

static void
history_prefs_check(PurplePlugin *plugin)
{
	if (!purple_prefs_get_bool("/purple/logging/log_ims") &&
	    !purple_prefs_get_bool("/purple/logging/log_chats"))
	{
		purple_notify_warning(plugin, NULL, _("History Plugin Requires Logging"),
			_("Logging can be enabled from Tools -> Preferences -> Logging.\n\n"
			  "Enabling logs for instant messages and/or chats will activate "
			  "history for the same conversation type(s)."));
	}
}

static void
history_prefs_cb(const char *name, PurplePrefType type, gconstpointer val, gpointer data)
{
	history_prefs_check((PurplePlugin *)data);
}

static PurplePluginPrefFrame *
get_plugin_pref_frame(PurplePlugin *plugin)
{
	PurplePluginPrefFrame *frame = purple_plugin_pref_frame_new();
	PurplePluginPref *pref;

	pref = purple_plugin_pref_new_with_name_and_label(PREF_LINES,
		_("Lines of the last conversation to show:"));
	purple_plugin_pref_set_bounds(pref, 1, 10000);
	purple_plugin_pref_frame_add(frame, pref);
	return frame;
}

static gboolean
plugin_load(PurplePlugin *plugin)
{
	my_plugin = plugin;
	purple_signal_connect(purple_conversations_get_handle(), "conversation-created",
	                      plugin, PURPLE_CALLBACK(historize), NULL);

	purple_prefs_connect_callback(plugin, "/purple/logging/log_ims",
	                              history_prefs_cb, plugin);
	purple_prefs_connect_callback(plugin, "/purple/logging/log_chats",
	                              history_prefs_cb, plugin);

	history_prefs_check(plugin);
	return TRUE;
}

static gboolean
plugin_unload(PurplePlugin *plugin)
{
	my_plugin = NULL;
	return TRUE;
}

static PurplePluginUiInfo prefs_info = {
	get_plugin_pref_frame,
	0,
	NULL,
	NULL, NULL, NULL, NULL
};

static PurplePluginInfo info =
{
	PURPLE_PLUGIN_MAGIC,
	PURPLE_MAJOR_VERSION,
	PURPLE_MINOR_VERSION,
	PURPLE_PLUGIN_STANDARD,
	PIDGIN_PLUGIN_TYPE,
	0,
	NULL,
	PURPLE_PRIORITY_DEFAULT,
	HISTORY_PLUGIN_ID,
	N_("History"),
	DISPLAY_VERSION,
	N_("Shows recently logged conversations in new conversations."),
	N_("When a new conversation is opened this plugin will insert "
	   "the last conversation into the current conversation."),
	"Sean Egan <seanegan@gmail.com>",
	PURPLE_WEBSITE,
	plugin_load,
	plugin_unload,
	NULL,
	NULL,
	NULL,
	&prefs_info,
	NULL,

	/* padding */
	NULL,
	NULL,
	NULL,
	NULL
};

static void
init_plugin(PurplePlugin *plugin)
{
	purple_prefs_add_none(PIDGIN4_PREFS_ROOT "/plugins");
	purple_prefs_add_none(HISTORY_PREFS);
	purple_prefs_add_int(PREF_LINES, 100);
}

PURPLE_INIT_PLUGIN(history, init_plugin, info)
