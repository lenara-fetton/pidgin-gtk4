/**
 * @file gtkdebug.c GTK+ Debug API
 * @ingroup pidgin
 */

/* pidgin
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
 * The debug window, rewritten for GTK 4 on a plain GtkTextView (no IMHtml).
 * Lines are kept in a ring buffer while the window is open so the level
 * and regex filters can be changed after the fact, as the GTK 2 version's
 * GtkListStore allowed. Its prefs live under /pidgin4/debug; the GTK 2
 * window's /pidgin/debug keys are not touched.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "debug.h"
#include "prefs.h"
#include "util.h"

#include "gtkdebug.h"
#include "gtkutils.h"

#define PREFS PIDGIN4_PREFS_ROOT "/debug"

/* Lines kept for re-filtering. */
#define MAX_LINES 20000

typedef struct
{
	PurpleDebugLevel level;
	char *text;        /* "(hh:mm:ss) category: message", no trailing \n */
	gsize cat_start;   /* byte range of "category:" within text */
	gsize cat_end;
} DebugLine;

typedef struct
{
	GtkWidget *window;
	GtkWidget *view;
	GtkTextBuffer *buffer;
	GtkWidget *scroller;

	GtkWidget *expression;
	GtkWidget *filter;
	GtkWidget *invert;
	GtkWidget *case_insensitive;
	GtkWidget *filterlevel;
	GtkWidget *pause;

	GQueue lines;      /* of DebugLine */
	GRegex *regex;
	guint timer;
	gboolean paused;
} DebugWindow;

static const char *const level_tags[] = {
	"all", "misc", "info", "warning", "error", "fatal"
};

static const char *const level_names[] = {
	N_("All"), N_("Misc"), N_("Info"), N_("Warning"), N_("Error "), N_("Fatal Error")
};

static DebugWindow *debug_win = NULL;
static guint debug_enabled_timer = 0;

static void refilter_all(DebugWindow *win);

static void
debug_line_free(gpointer data)
{
	DebugLine *line = data;

	g_free(line->text);
	g_free(line);
}

/**************************************************************************
 * Display
 **************************************************************************/

static gboolean
line_visible(DebugWindow *win, DebugLine *line)
{
	gboolean match;

	if (line->level < (PurpleDebugLevel)purple_prefs_get_int(PREFS "/filterlevel"))
		return FALSE;

	if (win->regex == NULL ||
	    !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(win->filter)))
		return TRUE;

	match = g_regex_match(win->regex, line->text, 0, NULL);
	if (gtk_check_button_get_active(GTK_CHECK_BUTTON(win->invert)))
		match = !match;
	return match;
}

static gboolean
view_at_bottom(DebugWindow *win)
{
	GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(
		GTK_SCROLLED_WINDOW(win->scroller));

	return gtk_adjustment_get_value(adj) + gtk_adjustment_get_page_size(adj)
		>= gtk_adjustment_get_upper(adj) - 1.0;
}

static void
insert_line(DebugWindow *win, DebugLine *line)
{
	GtkTextIter end;
	int offset;

	gtk_text_buffer_get_end_iter(win->buffer, &end);
	offset = gtk_text_iter_get_offset(&end);
	gtk_text_buffer_insert_with_tags_by_name(win->buffer, &end, line->text, -1,
		level_tags[line->level], NULL);
	gtk_text_buffer_get_end_iter(win->buffer, &end);
	gtk_text_buffer_insert(win->buffer, &end, "\n", 1);

	if (line->cat_end > line->cat_start) {
		GtkTextIter s, e;
		long chars_before = g_utf8_strlen(line->text, line->cat_start);
		long chars_cat = g_utf8_strlen(line->text + line->cat_start,
		                               line->cat_end - line->cat_start);

		gtk_text_buffer_get_iter_at_offset(win->buffer, &s, offset + chars_before);
		gtk_text_buffer_get_iter_at_offset(win->buffer, &e,
			offset + chars_before + chars_cat);
		gtk_text_buffer_apply_tag_by_name(win->buffer, "category", &s, &e);
	}
}

static void
scroll_to_end(DebugWindow *win)
{
	GtkTextIter end;
	GtkTextMark *mark;

	gtk_text_buffer_get_end_iter(win->buffer, &end);
	mark = gtk_text_buffer_get_mark(win->buffer, "end");
	gtk_text_buffer_move_mark(win->buffer, mark, &end);
	gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(win->view), mark);
}

static void
refilter_all(DebugWindow *win)
{
	GList *l;

	gtk_text_buffer_set_text(win->buffer, "", 0);
	for (l = win->lines.head; l != NULL; l = l->next) {
		if (line_visible(win, l->data))
			insert_line(win, l->data);
	}
	scroll_to_end(win);
}

/**************************************************************************
 * Filter
 **************************************************************************/

static void
regex_compile(DebugWindow *win)
{
	const char *text;
	GError *error = NULL;
	GRegexCompileFlags flags = G_REGEX_OPTIMIZE;

	g_clear_pointer(&win->regex, g_regex_unref);
	gtk_widget_remove_css_class(win->expression, "error");

	text = gtk_editable_get_text(GTK_EDITABLE(win->expression));
	if (text == NULL || *text == '\0')
		return;

	if (gtk_check_button_get_active(GTK_CHECK_BUTTON(win->case_insensitive)))
		flags |= G_REGEX_CASELESS;

	win->regex = g_regex_new(text, flags, 0, &error);
	if (win->regex == NULL) {
		gtk_widget_add_css_class(win->expression, "error");
		gtk_widget_set_tooltip_text(win->expression, error->message);
		g_error_free(error);
	} else {
		gtk_widget_set_tooltip_text(win->expression,
			_("Only show lines matching this regular expression"));
	}
}

static gboolean
regex_timer_cb(gpointer data)
{
	DebugWindow *win = data;

	win->timer = 0;
	regex_compile(win);
	purple_prefs_set_string(PREFS "/regex",
		gtk_editable_get_text(GTK_EDITABLE(win->expression)));
	refilter_all(win);

	return G_SOURCE_REMOVE;
}

static void
regex_changed_cb(GtkEditable *editable, DebugWindow *win)
{
	if (win->timer != 0)
		g_source_remove(win->timer);
	win->timer = g_timeout_add(300, regex_timer_cb, win);
}

static void
filter_toggled_cb(GtkToggleButton *button, DebugWindow *win)
{
	purple_prefs_set_bool(PREFS "/filter", gtk_toggle_button_get_active(button));
	refilter_all(win);
}

static void
invert_toggled_cb(GtkCheckButton *button, DebugWindow *win)
{
	purple_prefs_set_bool(PREFS "/invert", gtk_check_button_get_active(button));
	refilter_all(win);
}

static void
case_toggled_cb(GtkCheckButton *button, DebugWindow *win)
{
	purple_prefs_set_bool(PREFS "/case_insensitive", gtk_check_button_get_active(button));
	regex_compile(win);
	refilter_all(win);
}

static void
filterlevel_changed_cb(GObject *dropdown, GParamSpec *pspec, DebugWindow *win)
{
	guint level = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));

	if (level != GTK_INVALID_LIST_POSITION &&
	    (int)level != purple_prefs_get_int(PREFS "/filterlevel")) {
		purple_prefs_set_int(PREFS "/filterlevel", level);
		refilter_all(win);
	}
}

/**************************************************************************
 * Buttons
 **************************************************************************/

static void
clear_cb(GtkButton *button, DebugWindow *win)
{
	g_queue_clear_full(&win->lines, debug_line_free);
	gtk_text_buffer_set_text(win->buffer, "", 0);
}

static void
pause_cb(GtkToggleButton *button, DebugWindow *win)
{
	win->paused = gtk_toggle_button_get_active(button);
	if (!win->paused)
		refilter_all(win);
}

static void
save_finish_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	char *text = data;
	GError *error = NULL;
	GFile *file;

	file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error);
	if (file != NULL) {
		if (!g_file_replace_contents(file, text, strlen(text), NULL, FALSE,
		                             G_FILE_CREATE_NONE, NULL, NULL, &error)) {
			purple_debug_error("gtkdebug", "Unable to save the debug log: %s\n",
			                   error->message);
		}
		g_object_unref(file);
	}
	g_clear_error(&error);
	g_free(text);
}

static void
save_cb(GtkButton *button, DebugWindow *win)
{
	GtkFileDialog *dialog;
	GtkTextIter start, end;
	char *text, *tmp;
	time_t now = time(NULL);

	gtk_text_buffer_get_bounds(win->buffer, &start, &end);
	tmp = gtk_text_buffer_get_text(win->buffer, &start, &end, FALSE);
	text = g_strdup_printf("Pidgin 4 Debug Log : %s\n%s",
	                       purple_date_format_full(localtime(&now)), tmp);
	g_free(tmp);

	dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, _("Save Debug Log"));
	gtk_file_dialog_set_initial_name(dialog, "purple-debug.log");
	gtk_file_dialog_save(dialog, GTK_WINDOW(win->window), NULL, save_finish_cb, text);
	g_object_unref(dialog);
}

/**************************************************************************
 * Window
 **************************************************************************/

static gboolean
close_request_cb(GtkWindow *window, DebugWindow *win)
{
	/* Closing the window turns the debug window off, as in Pidgin 2. */
	purple_prefs_set_bool(PREFS "/enabled", FALSE);
	return FALSE;
}

static void
destroy_cb(GtkWidget *widget, DebugWindow *win)
{
	if (win->timer != 0)
		g_source_remove(win->timer);
	g_clear_pointer(&win->regex, g_regex_unref);
	g_queue_clear_full(&win->lines, debug_line_free);
	g_free(win);

	if (debug_win == win)
		debug_win = NULL;
}

static void
size_changed_cb(GtkWindow *window, GParamSpec *pspec, DebugWindow *win)
{
	int width, height;

	gtk_window_get_default_size(window, &width, &height);
	if (width > 0 && height > 0 && !gtk_window_is_maximized(window)) {
		purple_prefs_set_int(PREFS "/width", width);
		purple_prefs_set_int(PREFS "/height", height);
	}
}

static DebugWindow *
debug_window_new(void)
{
	DebugWindow *win;
	GtkWidget *vbox, *toolbar, *button, *label;
	GtkStringList *levels;
	GtkTextIter end;
	gsize i;

	win = g_new0(DebugWindow, 1);
	g_queue_init(&win->lines);

	win->window = gtk_window_new();
	gtk_window_set_application(GTK_WINDOW(win->window), pidgin_application_get());
	gtk_window_set_title(GTK_WINDOW(win->window), _("Debug Window"));
	gtk_widget_set_name(win->window, "debug");
	gtk_window_set_default_size(GTK_WINDOW(win->window),
		MAX(200, purple_prefs_get_int(PREFS "/width")),
		MAX(150, purple_prefs_get_int(PREFS "/height")));
	g_signal_connect(win->window, "close-request", G_CALLBACK(close_request_cb), win);
	g_signal_connect(win->window, "destroy", G_CALLBACK(destroy_cb), win);
	g_signal_connect(win->window, "notify::default-width", G_CALLBACK(size_changed_cb), win);
	g_signal_connect(win->window, "notify::default-height", G_CALLBACK(size_changed_cb), win);

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_window_set_child(GTK_WINDOW(win->window), vbox);

	toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	gtk_widget_add_css_class(toolbar, "toolbar");
	gtk_box_append(GTK_BOX(vbox), toolbar);

	button = gtk_button_new_from_icon_name("document-save-symbolic");
	gtk_widget_set_tooltip_text(button, _("Save"));
	g_signal_connect(button, "clicked", G_CALLBACK(save_cb), win);
	gtk_box_append(GTK_BOX(toolbar), button);

	button = gtk_button_new_from_icon_name("edit-clear-all-symbolic");
	gtk_widget_set_tooltip_text(button, _("Clear"));
	g_signal_connect(button, "clicked", G_CALLBACK(clear_cb), win);
	gtk_box_append(GTK_BOX(toolbar), button);

	win->pause = gtk_toggle_button_new();
	gtk_button_set_icon_name(GTK_BUTTON(win->pause), "media-playback-pause-symbolic");
	gtk_widget_set_tooltip_text(win->pause, _("Pause"));
	g_signal_connect(win->pause, "toggled", G_CALLBACK(pause_cb), win);
	gtk_box_append(GTK_BOX(toolbar), win->pause);

	gtk_box_append(GTK_BOX(toolbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL));

	/* regex filter */
	win->filter = gtk_toggle_button_new_with_mnemonic(_("_Filter"));
	gtk_widget_set_tooltip_text(win->filter, _("Filter"));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(win->filter),
	                             purple_prefs_get_bool(PREFS "/filter"));
	gtk_box_append(GTK_BOX(toolbar), win->filter);

	win->expression = gtk_search_entry_new();
	gtk_widget_set_hexpand(win->expression, TRUE);
	gtk_editable_set_text(GTK_EDITABLE(win->expression),
	                      purple_prefs_get_string(PREFS "/regex"));
	gtk_box_append(GTK_BOX(toolbar), win->expression);

	win->invert = gtk_check_button_new_with_mnemonic(_("_Invert"));
	gtk_check_button_set_active(GTK_CHECK_BUTTON(win->invert),
	                            purple_prefs_get_bool(PREFS "/invert"));
	gtk_box_append(GTK_BOX(toolbar), win->invert);

	win->case_insensitive = gtk_check_button_new_with_mnemonic(_("_Case Insensitive"));
	gtk_check_button_set_active(GTK_CHECK_BUTTON(win->case_insensitive),
	                            purple_prefs_get_bool(PREFS "/case_insensitive"));
	gtk_box_append(GTK_BOX(toolbar), win->case_insensitive);

	gtk_box_append(GTK_BOX(toolbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL));

	label = gtk_label_new_with_mnemonic(_("_Level "));
	gtk_box_append(GTK_BOX(toolbar), label);
	levels = gtk_string_list_new(NULL);
	for (i = 0; i < G_N_ELEMENTS(level_names); i++)
		gtk_string_list_append(levels, _(level_names[i]));
	win->filterlevel = gtk_drop_down_new(G_LIST_MODEL(levels), NULL);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(win->filterlevel),
		CLAMP(purple_prefs_get_int(PREFS "/filterlevel"), 0,
		      (int)G_N_ELEMENTS(level_names) - 1));
	gtk_widget_set_tooltip_text(win->filterlevel,
		_("Select the debug filter level."));
	pidgin_set_accessible_label(win->filterlevel, label);
	gtk_box_append(GTK_BOX(toolbar), win->filterlevel);

	/* the text view */
	win->view = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(win->view), FALSE);
	gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(win->view), FALSE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(win->view), GTK_WRAP_WORD_CHAR);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(win->view), TRUE);
	gtk_widget_add_css_class(win->view, "pidgin-debug-view");
	win->buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(win->view));

	gtk_text_buffer_create_tag(win->buffer, "all", NULL);
	gtk_text_buffer_create_tag(win->buffer, "misc", "foreground", "#888888", NULL);
	gtk_text_buffer_create_tag(win->buffer, "info", NULL);
	gtk_text_buffer_create_tag(win->buffer, "warning", "foreground", "#b35900", NULL);
	gtk_text_buffer_create_tag(win->buffer, "error", "foreground", "#e01b24", NULL);
	gtk_text_buffer_create_tag(win->buffer, "fatal", "foreground", "#e01b24",
	                           "weight", PANGO_WEIGHT_BOLD, NULL);
	gtk_text_buffer_create_tag(win->buffer, "category", "weight", PANGO_WEIGHT_BOLD, NULL);
	gtk_text_buffer_get_end_iter(win->buffer, &end);
	gtk_text_buffer_create_mark(win->buffer, "end", &end, FALSE);

	win->scroller = pidgin_make_scrollable(win->view, GTK_POLICY_AUTOMATIC,
	                                       GTK_POLICY_ALWAYS, -1, -1);
	gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(win->scroller), FALSE);
	gtk_widget_set_vexpand(win->scroller, TRUE);
	gtk_box_append(GTK_BOX(vbox), win->scroller);

	regex_compile(win);

	g_signal_connect(win->expression, "changed", G_CALLBACK(regex_changed_cb), win);
	g_signal_connect(win->filter, "toggled", G_CALLBACK(filter_toggled_cb), win);
	g_signal_connect(win->invert, "toggled", G_CALLBACK(invert_toggled_cb), win);
	g_signal_connect(win->case_insensitive, "toggled", G_CALLBACK(case_toggled_cb), win);
	g_signal_connect(win->filterlevel, "notify::selected",
	                 G_CALLBACK(filterlevel_changed_cb), win);

	return win;
}

static gboolean
debug_enabled_timeout_cb(gpointer data)
{
	debug_enabled_timer = 0;

	if (data)
		pidgin_debug_window_show();
	else
		pidgin_debug_window_hide();

	return FALSE;
}

static void
debug_enabled_cb(const char *name, PurplePrefType type,
				 gconstpointer value, gpointer data)
{
	if (debug_enabled_timer != 0)
		g_source_remove(debug_enabled_timer);
	debug_enabled_timer = g_timeout_add(0, debug_enabled_timeout_cb, GINT_TO_POINTER(GPOINTER_TO_INT(value)));
}

static void
pidgin_glib_log_handler(const gchar *domain, GLogLevelFlags flags,
					  const gchar *msg, gpointer user_data)
{
	PurpleDebugLevel level;
	char *new_msg = NULL;
	char *new_domain = NULL;

	if ((flags & G_LOG_LEVEL_ERROR) == G_LOG_LEVEL_ERROR)
		level = PURPLE_DEBUG_ERROR;
	else if ((flags & G_LOG_LEVEL_CRITICAL) == G_LOG_LEVEL_CRITICAL)
		level = PURPLE_DEBUG_FATAL;
	else if ((flags & G_LOG_LEVEL_WARNING) == G_LOG_LEVEL_WARNING)
		level = PURPLE_DEBUG_WARNING;
	else if ((flags & G_LOG_LEVEL_MESSAGE) == G_LOG_LEVEL_MESSAGE)
		level = PURPLE_DEBUG_INFO;
	else if ((flags & G_LOG_LEVEL_INFO) == G_LOG_LEVEL_INFO)
		level = PURPLE_DEBUG_INFO;
	else if ((flags & G_LOG_LEVEL_DEBUG) == G_LOG_LEVEL_DEBUG)
		level = PURPLE_DEBUG_MISC;
	else
	{
		purple_debug_warning("gtkdebug",
				   "Unknown glib logging level in %d\n", flags);

		level = PURPLE_DEBUG_MISC; /* This will never happen. */
	}

	if (msg != NULL)
		new_msg = purple_utf8_try_convert(msg);

	if (domain != NULL)
		new_domain = purple_utf8_try_convert(domain);

	if (new_msg != NULL)
	{
		purple_debug(level, (new_domain != NULL ? new_domain : "g_log"),
				   "%s\n", new_msg);

		g_free(new_msg);
	}

	/* Criticals and warnings must stay visible without -d too. */
	if (level >= PURPLE_DEBUG_WARNING && !purple_debug_is_enabled())
		g_log_default_handler(domain, flags, msg, user_data);

	g_free(new_domain);
}

void
pidgin_debug_init(void)
{
	/* Debug window preferences, all pidgin4-only. */
	purple_prefs_add_none(PREFS);

	/* Controls printing to the debug window */
	purple_prefs_add_bool(PREFS "/enabled", FALSE);
	purple_prefs_add_int(PREFS "/filterlevel", PURPLE_DEBUG_ALL);
	purple_prefs_add_int(PREFS "/width",  700);
	purple_prefs_add_int(PREFS "/height", 400);

	purple_prefs_add_string(PREFS "/regex", "");
	purple_prefs_add_bool(PREFS "/filter", FALSE);
	purple_prefs_add_bool(PREFS "/invert", FALSE);
	purple_prefs_add_bool(PREFS "/case_insensitive", FALSE);

	purple_prefs_connect_callback(pidgin_debug_get_handle(), PREFS "/enabled",
								debug_enabled_cb, NULL);

#define REGISTER_G_LOG_HANDLER(name) \
	g_log_set_handler((name), G_LOG_LEVEL_MASK | G_LOG_FLAG_FATAL \
					  | G_LOG_FLAG_RECURSION, \
					  pidgin_glib_log_handler, NULL)

	/* Register the glib/gtk log handlers. */
	REGISTER_G_LOG_HANDLER(NULL);
	REGISTER_G_LOG_HANDLER("Gdk");
	REGISTER_G_LOG_HANDLER("Gtk");
	REGISTER_G_LOG_HANDLER("GdkPixbuf");
	REGISTER_G_LOG_HANDLER("GLib");
	REGISTER_G_LOG_HANDLER("GModule");
	REGISTER_G_LOG_HANDLER("GLib-GObject");
	REGISTER_G_LOG_HANDLER("GLib-GIO");
	REGISTER_G_LOG_HANDLER("GThread");
	REGISTER_G_LOG_HANDLER("pidgin4");
}

void
pidgin_debug_uninit(void)
{
	purple_debug_set_ui_ops(NULL);
	purple_prefs_disconnect_by_handle(pidgin_debug_get_handle());

	if (debug_enabled_timer != 0) {
		g_source_remove(debug_enabled_timer);
		debug_enabled_timer = 0;
	}

	if (debug_win != NULL)
		gtk_window_destroy(GTK_WINDOW(debug_win->window));
}

void
pidgin_debug_window_show_for_session(void)
{
	if (debug_win == NULL)
		debug_win = debug_window_new();

	gtk_window_present(GTK_WINDOW(debug_win->window));
}

void
pidgin_debug_window_show(void)
{
	pidgin_debug_window_show_for_session();

	/* Remembered: the window opens again on the next start. */
	purple_prefs_set_bool(PREFS "/enabled", TRUE);
}

void
pidgin_debug_window_hide(void)
{
	if (debug_win != NULL)
		gtk_window_destroy(GTK_WINDOW(debug_win->window));
}

static void
pidgin_debug_print(PurpleDebugLevel level, const char *category,
					 const char *arg_s)
{
	DebugLine *line;
	GString *str;
	char *text;
	gsize len;
	time_t mtime;
	gboolean bottom;

	if (debug_win == NULL)
		return;

	if (level < 0 || level > PURPLE_DEBUG_FATAL)
		level = PURPLE_DEBUG_MISC;

	mtime = time(NULL);
	line = g_new0(DebugLine, 1);
	line->level = level;

	str = g_string_new(NULL);
	g_string_append_printf(str, "(%s) ",
		purple_utf8_strftime("%H:%M:%S", localtime(&mtime)));
	if (category != NULL && *category != '\0') {
		line->cat_start = str->len;
		g_string_append_printf(str, "%s:", category);
		line->cat_end = str->len;
		g_string_append_c(str, ' ');
	}

	text = purple_utf8_try_convert(arg_s);
	g_string_append(str, text ? text : arg_s);
	g_free(text);

	/* one entry per call; drop the trailing newline */
	len = str->len;
	while (len > 0 && str->str[len - 1] == '\n')
		len--;
	g_string_truncate(str, len);
	line->text = g_string_free(str, FALSE);

	g_queue_push_tail(&debug_win->lines, line);
	while (g_queue_get_length(&debug_win->lines) > MAX_LINES) {
		/* The oldest line is dropped from the store only; the text view
		 * keeps what it shows until the next refilter or clear. */
		debug_line_free(g_queue_pop_head(&debug_win->lines));
	}

	if (debug_win->paused || !line_visible(debug_win, line))
		return;

	/* Keep the view about as long as the store. */
	if (gtk_text_buffer_get_line_count(debug_win->buffer) > MAX_LINES + 2000) {
		refilter_all(debug_win);
		return;
	}

	bottom = view_at_bottom(debug_win);
	insert_line(debug_win, line);
	if (bottom)
		scroll_to_end(debug_win);
}

static gboolean
pidgin_debug_is_enabled(PurpleDebugLevel level, const char *category)
{
	return (debug_win != NULL);
}

static PurpleDebugUiOps ops =
{
	pidgin_debug_print,
	pidgin_debug_is_enabled,
	NULL,
	NULL,
	NULL,
	NULL
};

PurpleDebugUiOps *
pidgin_debug_get_ui_ops(void)
{
	return &ops;
}

void *
pidgin_debug_get_handle(void) {
	static int handle;

	return &handle;
}
