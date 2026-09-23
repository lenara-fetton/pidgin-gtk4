/*
 * msgview-demo: a window with a PidginMessageView, a PidginComposeEntry and
 * its PidginFormatToolbar, filled with sample messages (formatted HTML, an
 * image, smileys, XEP-0393, a correction, reactions, a reply, receipts, a
 * marker, a spoiler, remote-image fallbacks), for eyeballing the M4
 * components and for M4b to copy from. Built, not installed.
 *
 *   msgview-demo [--log FILE] [--xmpp]
 *     --log FILE  also show the messages of a Pidgin HTML log (read only)
 *     --xmpp      start in XMPP mode (XEP-0393 compose, XEP-0392 colours)
 *
 * PIDGIN4_MSGVIEW_SELFTEST=1 runs a headless check instead: 5000 messages,
 * scrollback trimming, prepend, find, compose/send/history; it prints
 * time and memory and exits non-zero on failure.
 *
 * It uses a scratch libpurple profile; it never touches ~/.purple.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include <gtk/gtk.h>

#include "debug.h"
#include "imgstore.h"
#include "prefs.h"
#include "signals.h"
#include "smiley.h"
#include "util.h"

#include "gtkutils.h"
#include "pidgincomposeentry.h"
#include "pidginformattoolbar.h"
#include "pidginmarkup.h"
#include "pidginmessageview.h"
#include "pidginsmileytheme.h"

#include "test-support.h"

#define SELF "me@example.com"

static PidginMessageView *view;
static PidginComposeEntry *entry;
static PidginFormatToolbar *toolbar;
static gboolean xmpp_mode;
static PidginMessage *editing;          /* message being corrected */
static PidginMessage *replying;         /* message being replied to */
static GtkWidget *status_label;
static char *log_file;
static int failures;

static const PidginMarkupOptions xmpp_opts = { PIDGIN_MARKUP_STYLING, "XMPP", NULL, NULL };

/**************************************************************************
 * Sample content
 **************************************************************************/

static int
make_image(int w, int h)
{
	guchar *pixels = g_malloc(w * h * 4);
	GBytes *bytes, *png;
	GdkTexture *texture;
	int x, y, id;

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			guchar *p = pixels + (y * w + x) * 4;
			p[0] = x * 255 / w;
			p[1] = y * 255 / h;
			p[2] = 160;
			p[3] = 255;
		}
	}
	bytes = g_bytes_new_take(pixels, w * h * 4);
	texture = gdk_memory_texture_new(w, h, GDK_MEMORY_R8G8B8A8, bytes, w * 4);
	png = gdk_texture_save_to_png_bytes(texture);
	id = purple_imgstore_add_with_id(g_memdup2(g_bytes_get_data(png, NULL), g_bytes_get_size(png)),
	                                 g_bytes_get_size(png), "gradient.png");
	g_bytes_unref(png);
	g_bytes_unref(bytes);
	g_object_unref(texture);
	return id;
}

static PidginMessage *
add(const char *who, const char *html, PurpleMessageFlags flags, time_t when)
{
	PidginMessage *msg = pidgin_message_new(who, NULL, html, flags,
	                                        when ? when : time(NULL));
	if (xmpp_mode)
		pidgin_message_set_parse_options(msg, &xmpp_opts);
	pidgin_message_view_append(view, msg);
	g_object_unref(msg);
	return msg;
}

/* Lines of a Pidgin 2 HTML log: (time) name: message<br/> */
static void
load_log(const char *path)
{
	GRegex *re = g_regex_new("^<span style=\"color: #([0-9A-Fa-f]{6})\"><span style=\"font-size: "
	                         "smaller\">\\((.*?)\\)</span> <b>(.*?):?</b></span> (.*)<br/?>$",
	                         0, 0, NULL);
	GFile *file = g_file_new_for_path(path);
	GFileInputStream *in = g_file_read(file, NULL, NULL);
	GDataInputStream *data;
	char *line;
	int n = 0;

	if (in == NULL) {
		g_printerr("Cannot read %s\n", path);
		g_object_unref(file);
		g_regex_unref(re);
		return;
	}
	data = g_data_input_stream_new(G_INPUT_STREAM(in));
	while (n < 300 && (line = g_data_input_stream_read_line_utf8(data, NULL, NULL, NULL))) {
		GMatchInfo *mi = NULL;
		if (g_regex_match(re, line, 0, &mi)) {
			char *color = g_match_info_fetch(mi, 1);
			char *who = g_match_info_fetch(mi, 3);
			char *body = g_match_info_fetch(mi, 4);
			char *name = purple_unescape_html(who);
			PurpleMessageFlags flags = g_ascii_strcasecmp(color, "16569E") ?
				PURPLE_MESSAGE_RECV : PURPLE_MESSAGE_SEND;
			add(name, body, flags, time(NULL) - 3600);
			g_free(color);
			g_free(who);
			g_free(body);
			g_free(name);
			n++;
		}
		g_match_info_free(mi);
		g_free(line);
	}
	g_object_unref(data);
	g_object_unref(in);
	g_object_unref(file);
	g_regex_unref(re);
}

static void
fill_samples(void)
{
	time_t now = time(NULL);
	PidginMessage *msg, *first;
	char *html;
	int img = make_image(160, 90);

	add(NULL, "<b>Sample conversation</b> for the pidgin4 M4 components", PURPLE_MESSAGE_SYSTEM,
	    now - 7200);
	first = add("alice@example.com", "Hi! <b>bold</b>, <i>italic</i>, <u>underline</u>, "
	            "<s>strike</s>, <font color=\"#c00000\">red</font>, "
	            "<span style=\"background-color: #fce94f\">highlighted</span>, "
	            "<font size=\"5\">big</font> and <font size=\"1\">small</font> :-) ;)",
	            PURPLE_MESSAGE_RECV, now - 7000);
	pidgin_message_set_stanza_id(first, "msg-1");
	pidgin_message_add_reaction(first, "\xf0\x9f\x91\x8d", "bob@example.com");
	pidgin_message_add_reaction(first, "\xf0\x9f\x91\x8d", SELF);
	pidgin_message_add_reaction(first, "\xf0\x9f\x98\x82", "carol@example.com");

	msg = add(SELF, "Links: https://pidgin.im/ and <a href=\"xmpp:room@conference.example.com?join\">"
	          "the room</a>, mail <a href=\"mailto:someone@example.com\">someone</a>",
	          PURPLE_MESSAGE_SEND, now - 6900);
	pidgin_message_set_receipt(msg, PIDGIN_RECEIPT_DISPLAYED);

	html = g_strdup_printf("Here is an image: <img id=\"%d\"> and a rule:<hr>after the rule", img);
	add("alice@example.com", html, PURPLE_MESSAGE_RECV, now - 6800);
	g_free(html);

	msg = add(SELF, "I meant to say tomorow", PURPLE_MESSAGE_SEND, now - 6700);
	pidgin_message_set_stanza_id(msg, "msg-2");
	pidgin_message_apply_correction(msg, "I meant to say <b>tomorrow</b>", "msg-2b");
	pidgin_message_set_receipt(msg, PIDGIN_RECEIPT_DELIVERED);

	msg = add("bob@example.com", "Replying to that first one", PURPLE_MESSAGE_RECV, now - 6600);
	pidgin_message_set_reply(msg, "msg-1", "alice@example.com", NULL);

	add("bob@example.com", "/me waves", PURPLE_MESSAGE_RECV, now - 6500);
	add("carol@example.com", "Spoiler: <span style=\"color: black; background-color: black\">"
	    "the butler did it</span> (click)", PURPLE_MESSAGE_RECV, now - 6400);
	add("carol@example.com", "Discord emoji <img src=\"https://cdn.discordapp.com/emojis/1.png\" "
	    "alt=\":blobcat:\"> and a remote picture from an unknown host "
	    "<img src=\"https://example.org/cat.png\" alt=\"cat.png\"> (shown as a link)",
	    PURPLE_MESSAGE_RECV, now - 6300);

	/* XEP-0393 */
	msg = pidgin_message_new("dave@example.com", NULL,
		"XEP-0393: *strong*, _emphasis_, ~strike~, `code`<br>"
		"&gt; a quote<br>&gt;&gt; nested<br>```<br>preformatted *not bold*<br>```<br>"
		"snake_case stays plain, and http://example.com/a_b_c too",
		PURPLE_MESSAGE_RECV, now - 6200);
	pidgin_message_set_parse_options(msg, &xmpp_opts);
	pidgin_message_view_append(view, msg);
	g_object_unref(msg);

	add("alice@example.com", "Did you see my <font face=\"serif\">serif</font> text?",
	    PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_NICK, now - 6100);
	msg = add("bob@example.com", "this one was deleted", PURPLE_MESSAGE_RECV, now - 6000);
	pidgin_message_set_retracted(msg, TRUE);
	add(NULL, "Something went wrong (an error line)", PURPLE_MESSAGE_ERROR, now - 5900);

	/* the unread marker, then newer messages */
	pidgin_message_view_set_marker(view);
	add("alice@example.com", "Newer than the marker line", PURPLE_MESSAGE_RECV, now - 60);
	add("alice@example.com", "Whispering", PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_WHISPER, now - 30);
}

/**************************************************************************
 * Interaction
 **************************************************************************/

static void
set_status(const char *text)
{
	if (status_label)
		gtk_label_set_text(GTK_LABEL(status_label), text ? text : "");
}

static gboolean
message_send_cb(PidginComposeEntry *e, const char *markup, gpointer data)
{
	PidginMessage *msg;

	if (editing != NULL) {
		pidgin_message_apply_correction(editing, markup, NULL);
		g_clear_object(&editing);
		set_status(NULL);
		return TRUE;
	}
	msg = pidgin_message_new(SELF, NULL, markup, PURPLE_MESSAGE_SEND, time(NULL));
	if (xmpp_mode)
		pidgin_message_set_parse_options(msg, &xmpp_opts);
	if (replying != NULL) {
		pidgin_message_set_reply(msg, pidgin_message_get_stanza_id(replying),
		                         pidgin_message_get_sender(replying), NULL);
		g_clear_object(&replying);
		set_status(NULL);
	}
	{
		static int n = 100;
		char *id = g_strdup_printf("sent-%d", n++);
		pidgin_message_set_stanza_id(msg, id);
		g_free(id);
	}
	pidgin_message_set_receipt(msg, PIDGIN_RECEIPT_SENT);
	pidgin_message_view_append(view, msg);
	g_object_unref(msg);
	return TRUE;
}

static void
edit_last_cb(PidginComposeEntry *e, gpointer data)
{
	PidginMessage *last = pidgin_message_view_get_last_sent(view);

	if (last == NULL)
		return;
	g_set_object(&editing, last);
	pidgin_compose_entry_set_markup(entry, pidgin_message_get_html(last));
	set_status("Editing the last message (Enter to correct)");
}

static void
reaction_toggled_cb(PidginMessageView *v, PidginMessage *msg, const char *emoji,
                    gboolean add_it, gpointer data)
{
	if (add_it)
		pidgin_message_add_reaction(msg, emoji, SELF);
	else
		pidgin_message_remove_reaction(msg, emoji, SELF);
}

static void
reply_requested_cb(PidginMessageView *v, PidginMessage *msg, gpointer data)
{
	char *text;

	if (pidgin_message_get_stanza_id(msg) == NULL) {
		char *id = g_strdup_printf("demo-%p", (void *)msg);
		pidgin_message_set_stanza_id(msg, id);
		g_free(id);
	}
	g_set_object(&replying, msg);
	text = g_strdup_printf("Replying to %s", pidgin_message_get_alias(msg));
	set_status(text);
	g_free(text);
	gtk_widget_grab_focus(GTK_WIDGET(entry));
}

static void
edit_requested_cb(PidginMessageView *v, PidginMessage *msg, gpointer data)
{
	g_set_object(&editing, msg);
	pidgin_compose_entry_set_markup(entry, pidgin_message_get_html(msg));
	set_status("Editing (Enter to correct)");
}

static void
retract_requested_cb(PidginMessageView *v, PidginMessage *msg, gpointer data)
{
	pidgin_message_set_retracted(msg, TRUE);
}

static void
set_xmpp_mode(gboolean on)
{
	xmpp_mode = on;
	pidgin_compose_entry_set_caps(entry, on ? PIDGIN_FORMAT_STYLING_ALL :
	                              PIDGIN_FORMAT_HTML_ALL | PIDGIN_FORMAT_CUSTOM_SMILEY);
	pidgin_message_view_set_nick_color_scheme(view, on ? PIDGIN_NICK_COLOR_XEP0392 :
	                                          PIDGIN_NICK_COLOR_PIDGIN);
}

static void
xmpp_toggled_cb(GtkCheckButton *check, gpointer data)
{
	set_xmpp_mode(gtk_check_button_get_active(check));
}

static void
find_toggled_cb(GtkToggleButton *button, gpointer data)
{
	pidgin_message_view_set_search_mode(view, gtk_toggle_button_get_active(button));
}

static void
marker_cb(GtkButton *button, gpointer data)
{
	pidgin_message_view_set_marker(view);
}

static void
older_cb(GtkButton *button, gpointer data)
{
	GPtrArray *older = g_ptr_array_new_with_free_func(g_object_unref);
	time_t base = time(NULL) - 86400;
	int i;

	for (i = 0; i < 20; i++) {
		char *text = g_strdup_printf("Older message %d (as MAM scroll-back would add)", i);
		g_ptr_array_add(older, pidgin_message_new("archive@example.com", NULL, text,
		                                          PURPLE_MESSAGE_RECV, base + i * 60));
		g_free(text);
	}
	pidgin_message_view_prepend_many(view, older);
	g_ptr_array_unref(older);
}

static GtkWidget *
build_window(GtkApplication *app)
{
	GtkWidget *window, *box, *bar, *check, *button, *compose, *e = NULL, *t = NULL;

	window = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(window), "pidgin4 message view demo");
	gtk_window_set_default_size(GTK_WINDOW(window), 760, 620);

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_margin_start(bar, 6);
	gtk_widget_set_margin_end(bar, 6);
	check = gtk_check_button_new_with_label("XMPP mode (XEP-0393, XEP-0392)");
	g_signal_connect(check, "toggled", G_CALLBACK(xmpp_toggled_cb), NULL);
	gtk_box_append(GTK_BOX(bar), check);
	button = gtk_toggle_button_new_with_label("Find");
	g_signal_connect(button, "toggled", G_CALLBACK(find_toggled_cb), NULL);
	gtk_box_append(GTK_BOX(bar), button);
	button = gtk_button_new_with_label("Marker");
	g_signal_connect(button, "clicked", G_CALLBACK(marker_cb), NULL);
	gtk_box_append(GTK_BOX(bar), button);
	button = gtk_button_new_with_label("Load older");
	g_signal_connect(button, "clicked", G_CALLBACK(older_cb), NULL);
	gtk_box_append(GTK_BOX(bar), button);
	status_label = gtk_label_new(NULL);
	gtk_widget_set_hexpand(status_label, TRUE);
	gtk_label_set_xalign(GTK_LABEL(status_label), 1);
	gtk_box_append(GTK_BOX(bar), status_label);
	gtk_box_append(GTK_BOX(box), bar);

	view = PIDGIN_MESSAGE_VIEW(pidgin_create_message_view());
	pidgin_message_view_set_is_chat(view, TRUE);
	pidgin_message_view_set_self_id(view, SELF);
	gtk_widget_set_vexpand(GTK_WIDGET(view), TRUE);
	g_signal_connect(view, "reaction-toggled", G_CALLBACK(reaction_toggled_cb), NULL);
	g_signal_connect(view, "reply-requested", G_CALLBACK(reply_requested_cb), NULL);
	g_signal_connect(view, "edit-requested", G_CALLBACK(edit_requested_cb), NULL);
	g_signal_connect(view, "retract-requested", G_CALLBACK(retract_requested_cb), NULL);
	gtk_box_append(GTK_BOX(box), GTK_WIDGET(view));

	compose = pidgin_create_compose_entry(PURPLE_CONNECTION_HTML |
	                                      PURPLE_CONNECTION_ALLOW_CUSTOM_SMILEY,
	                                      TRUE, &e, &t);
	entry = PIDGIN_COMPOSE_ENTRY(e);
	toolbar = PIDGIN_FORMAT_TOOLBAR(t);
	gtk_widget_set_vexpand(compose, FALSE);
	gtk_widget_set_size_request(compose, -1, 110);
	g_signal_connect(entry, "message-send", G_CALLBACK(message_send_cb), NULL);
	g_signal_connect(entry, "edit-last-requested", G_CALLBACK(edit_last_cb), NULL);
	gtk_box_append(GTK_BOX(box), compose);

	gtk_window_set_child(GTK_WINDOW(window), box);
	if (xmpp_mode) {
		gtk_check_button_set_active(GTK_CHECK_BUTTON(check), TRUE);
		set_xmpp_mode(TRUE);
	}
	return window;
}

/**************************************************************************
 * Selftest
 **************************************************************************/

#define CHECK(cond) G_STMT_START { \
	if (!(cond)) { \
		g_printerr("selftest: FAILED: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
		failures++; \
	} \
} G_STMT_END

static long
rss_kb(void)
{
	char *contents = NULL;
	long pages = 0, rss = 0;

	if (g_file_get_contents("/proc/self/statm", &contents, NULL, NULL))
		sscanf(contents, "%ld %ld", &pages, &rss);
	g_free(contents);
	return rss * (sysconf(_SC_PAGESIZE) / 1024);
}

static void
drain(void)
{
	int i;

	/* let layout and frame callbacks run */
	for (i = 0; i < 20; i++) {
		while (g_main_context_iteration(NULL, FALSE))
			;
		g_usleep(5000);
	}
}

static void
scroll_through(void)
{
	GtkWidget *sw = gtk_widget_get_last_child(GTK_WIDGET(view));
	GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw));
	int i;

	for (i = 0; i <= 10; i++) {
		gtk_adjustment_set_value(adj, (gtk_adjustment_get_upper(adj) -
		                         gtk_adjustment_get_page_size(adj)) * i / 10.0);
		drain();
	}
}

static const char *bodies[] = {
	"plain text message number %d",
	"<b>bold</b> <i>italic</i> <font color=\"#aa0000\">red %d</font> https://example.com/%d",
	"a longer message %d that wraps a few times because it keeps going and going with words "
	"so that the row needs height-for-width layout, with <u>underline</u> and <s>strike</s>",
	"<span style=\"font-size: large\">large</span> needle %d :-)",
};

static gboolean
selftest(gpointer data)
{
	GtkApplication *app = data;
	gint64 t0, t1;
	long rss0, rss1;
	guint i, n;
	time_t base = time(NULL) - 5000 * 30;
	char *markup;

	drain();
	rss0 = rss_kb();
	t0 = g_get_monotonic_time();

	pidgin_message_view_set_scrollback(view, 0);
	for (i = 0; i < 5000; i++) {
		char *html = g_strdup_printf(bodies[i % G_N_ELEMENTS(bodies)], i, i);
		PidginMessage *msg = pidgin_message_new(i % 3 ? "alice@example.com" : SELF, NULL, html,
		                                        i % 3 ? PURPLE_MESSAGE_RECV : PURPLE_MESSAGE_SEND,
		                                        base + i * 30);
		if (i % 50 == 0)
			pidgin_message_add_reaction(msg, "\xf0\x9f\x91\x8d", "bob@example.com");
		pidgin_message_view_append(view, msg);
		g_object_unref(msg);
		g_free(html);
		if (i % 500 == 0)
			drain();
	}
	drain();
	t1 = g_get_monotonic_time();
	rss1 = rss_kb();

	n = g_list_model_get_n_items(pidgin_message_view_get_model(view));
	CHECK(n >= 5000);
	CHECK(pidgin_message_view_is_at_bottom(view));
	g_print("selftest: appended 5000 messages in %.2f s; RSS %ld -> %ld kB (+%ld kB)\n",
	        (t1 - t0) / 1e6, rss0, rss1, rss1 - rss0);
	CHECK((t1 - t0) / 1e6 < 60);
	CHECK(rss1 - rss0 < 400 * 1024);

	t0 = g_get_monotonic_time();
	scroll_through();
	t1 = g_get_monotonic_time();
	g_print("selftest: scrolled through in %.2f s; RSS %ld kB\n", (t1 - t0) / 1e6, rss_kb());

	/* find: every 4th body has "needle" */
	pidgin_message_view_set_search_text(view, "NEEDLE");
	drain();
	g_print("selftest: find 'NEEDLE': %u of %u shown\n",
	        pidgin_message_view_get_n_visible(view), n);
	CHECK(pidgin_message_view_get_n_visible(view) == 1250);
	pidgin_message_view_set_search_text(view, NULL);
	drain();
	CHECK(pidgin_message_view_get_n_visible(view) == n);

	/* prepend older while scrolled to the middle: what is shown stays */
	{
		GtkWidget *sw = gtk_widget_get_last_child(GTK_WIDGET(view));
		GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw));
		double before, after;

		gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) / 2);
		drain();
		before = gtk_adjustment_get_value(adj);
		older_cb(NULL, NULL);
		drain();
		after = gtk_adjustment_get_value(adj);
		g_print("selftest: prepend 20 at %.0f px -> %.0f px\n", before, after);
		CHECK(after > before);
		CHECK(!pidgin_message_view_is_at_bottom(view));
	}
	CHECK(g_list_model_get_n_items(pidgin_message_view_get_model(view)) == n + 20);
	pidgin_message_view_set_scrollback(view, 1000);
	drain();
	CHECK(g_list_model_get_n_items(pidgin_message_view_get_model(view)) == 1000);

	/* marker: there is only ever one, at the end */
	pidgin_message_view_set_marker(view);
	pidgin_message_view_set_marker(view);
	drain();
	CHECK(g_list_model_get_n_items(pidgin_message_view_get_model(view)) == 1001);
	{
		PidginMessage *last = g_list_model_get_item(pidgin_message_view_get_model(view), 1000);
		CHECK(pidgin_message_get_kind(last) == PIDGIN_MESSAGE_KIND_MARKER);
		g_object_unref(last);
	}
	pidgin_message_view_remove_marker(view);
	CHECK(g_list_model_get_n_items(pidgin_message_view_get_model(view)) == 1000);

	/* compose: bold text, send, history, 0393 */
	gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(entry)), "", 0);
	pidgin_compose_entry_toggle_bold(entry);
	gtk_text_buffer_insert_at_cursor(gtk_text_view_get_buffer(GTK_TEXT_VIEW(entry)), "hello", -1);
	pidgin_compose_entry_toggle_bold(entry);
	gtk_text_buffer_insert_at_cursor(gtk_text_view_get_buffer(GTK_TEXT_VIEW(entry)), " world", -1);
	markup = pidgin_compose_entry_get_markup(entry);
	g_print("selftest: compose -> '%s'\n", markup);
	CHECK(g_str_has_prefix(markup, "<b>hello</b> world"));
	g_free(markup);
	CHECK(pidgin_compose_entry_send(entry));
	CHECK(pidgin_compose_entry_is_empty(entry));
	CHECK(g_list_length(pidgin_compose_entry_get_history(entry)) == 1);
	pidgin_compose_entry_history_up(entry);
	markup = pidgin_compose_entry_get_markup(entry);
	CHECK(g_str_has_prefix(markup, "<b>hello</b> world"));
	g_free(markup);
	pidgin_compose_entry_history_down(entry);
	CHECK(pidgin_compose_entry_is_empty(entry));

	set_xmpp_mode(TRUE);
	pidgin_compose_entry_toggle_bold(entry);
	gtk_text_buffer_insert_at_cursor(gtk_text_view_get_buffer(GTK_TEXT_VIEW(entry)), "bold", -1);
	pidgin_compose_entry_toggle_bold(entry);
	gtk_text_buffer_insert_at_cursor(gtk_text_view_get_buffer(GTK_TEXT_VIEW(entry)), " & <plain>", -1);
	markup = pidgin_compose_entry_get_markup(entry);
	g_print("selftest: XMPP compose -> '%s'\n", markup);
	CHECK(purple_strequal(markup, "*bold* &amp; &lt;plain&gt;"));
	g_free(markup);
	CHECK(pidgin_compose_entry_send(entry));
	drain();

	/* the toolbar follows the capabilities */
	CHECK(pidgin_format_toolbar_get_entry(toolbar) == entry);

	g_print("selftest: %s\n", failures ? "FAILED" : "passed");
	g_application_quit(G_APPLICATION(app));
	return G_SOURCE_REMOVE;
}

/**************************************************************************
 * main
 **************************************************************************/

static gboolean
quit_cb(gpointer app)
{
	/* scroll through the samples once so every row is built */
	scroll_through();
	g_application_quit(G_APPLICATION(app));
	return G_SOURCE_REMOVE;
}

static void
activate_cb(GtkApplication *app, gpointer data)
{
	GtkWidget *window = build_window(app);
	GtkCssProvider *css = gtk_css_provider_new();

	/* pidgin4's stylesheet, from the GResource linked in */
	gtk_css_provider_load_from_resource(css, PIDGIN4_RESOURCE_PATH "/style.css");
	gtk_style_context_add_provider_for_display(gdk_display_get_default(),
		GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(css);

	if (g_getenv("PIDGIN4_MSGVIEW_SELFTEST") != NULL) {
		gtk_window_present(GTK_WINDOW(window));
		g_idle_add(selftest, app);
		return;
	}

	fill_samples();
	if (log_file != NULL)
		load_log(log_file);
	gtk_window_present(GTK_WINDOW(window));

	/* PIDGIN4_DEMO_QUIT_AFTER=ms: show the samples, then quit (headless
	 * render check) */
	if (g_getenv("PIDGIN4_DEMO_QUIT_AFTER") != NULL)
		g_timeout_add(atoi(g_getenv("PIDGIN4_DEMO_QUIT_AFTER")), quit_cb, app);
}

/* As gtkmain.c: libpurple's criticals (no log domain) are not fatal. Here
 * that is purple_dbus_register_pointer() complaining that this harness
 * didn't start libpurple's D-Bus layer. */
static gboolean
fatal_log_filter(const char *log_domain, GLogLevelFlags log_level,
                 const char *message, gpointer data)
{
	return !(log_domain == NULL && !(log_level & G_LOG_LEVEL_ERROR));
}

int
main(int argc, char *argv[])
{
	GtkApplication *app;
	int status, i;
	const char *profile;

	g_test_log_set_fatal_handler(fatal_log_filter, NULL);

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--log") && i + 1 < argc)
			log_file = argv[++i];
		else if (!strcmp(argv[i], "--xmpp"))
			xmpp_mode = TRUE;
	}

	/* Under "meson test" (PIDGIN4_MSGVIEW_SELFTEST=meson) only run on the
	 * display named by PIDGIN4_TEST_DISPLAY (an Xvfb), never on the
	 * user's session; skip otherwise (meson treats 77 as skipped). */
	if (purple_strequal(g_getenv("PIDGIN4_MSGVIEW_SELFTEST"), "meson")) {
		const char *display = g_getenv("PIDGIN4_TEST_DISPLAY");
		if (display == NULL || *display == '\0')
			return 77;
		g_setenv("DISPLAY", display, TRUE);
		g_setenv("GDK_BACKEND", "x11", TRUE);
		g_unsetenv("WAYLAND_DISPLAY");
		g_setenv("GTK_A11Y", "none", TRUE);
	}
	if (!gtk_init_check())
		return 77;

	/* A scratch libpurple: prefs (defaults), signals, images, smileys. */
	profile = pidgin_test_profile_setup();
	purple_debug_set_enabled(g_getenv("PIDGIN4_DEMO_DEBUG") != NULL);
	purple_signals_init();
	purple_prefs_init();
	purple_imgstore_init();
	purple_smileys_init();
	pidgin_markup_init();
	pidgin_smiley_themes_init();
	pidgin_smiley_theme_set_current("Default");
	pidgin_message_view_signals_init();
	g_debug("demo profile %s", profile);

	app = gtk_application_new("com.minowick.Pidgin4.MsgViewDemo", G_APPLICATION_NON_UNIQUE);
	g_signal_connect(app, "activate", G_CALLBACK(activate_cb), NULL);
	status = g_application_run(G_APPLICATION(app), 1, argv);
	g_object_unref(app);

	g_clear_object(&editing);
	g_clear_object(&replying);
	pidgin_message_view_signals_uninit();
	pidgin_smiley_themes_uninit();
	pidgin_markup_uninit();
	pidgin_test_profile_cleanup();

	return status != 0 ? status : (failures ? 1 : 0);
}
