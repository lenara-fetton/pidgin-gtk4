/*
 * pidgin4: tests for the plugin pref frame converter and the foreign
 * toolkit check (gtkpluginpref.c).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include <elf.h>

#include "eventloop.h"
#include "pluginpref.h"
#include "prefs.h"

#include "gtkplugin.h"
#include "gtkpluginpref.h"
#include "pidgincomposeentry.h"

#include "test-display.h"
#include "test-support.h"

static gboolean have_gtk = FALSE;
static const char *profile = NULL;

/**************************************************************************
 * ELF
 **************************************************************************/

static void
test_elf_gtk2(void)
{
	const char *path = "/usr/lib64/pidgin/history.so";
	char *lib = NULL;

	if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
		g_test_skip("no GTK 2 Pidgin plugin installed");
		return;
	}
	g_assert_true(pidgin_plugin_file_is_foreign_toolkit(path, &lib));
	g_assert_cmpstr(lib, ==, "libgtk-x11-2.0.so.0");
	g_free(lib);
	/* @lib may be NULL. */
	g_assert_true(pidgin_plugin_file_is_foreign_toolkit(path, NULL));
}

static void
test_elf_core_plugin(void)
{
	char *path = g_build_filename(g_get_home_dir(), ".local", "pidgin4", "lib",
	                              "purple-2", "psychic.so", NULL);
	char *lib = (char *)"garbage";

	if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
		g_test_skip("no psychic.so in ~/.local/pidgin4");
		g_free(path);
		return;
	}
	g_assert_false(pidgin_plugin_file_is_foreign_toolkit(path, &lib));
	g_assert_null(lib);
	g_free(path);
}

static void
check_not_foreign(const char *name, const void *data, gsize len)
{
	char *path = g_build_filename(profile, name, NULL);
	char *lib = NULL;

	g_assert_true(g_file_set_contents(path, data, len, NULL));
	g_assert_false(pidgin_plugin_file_is_foreign_toolkit(path, &lib));
	g_assert_null(lib);
	g_free(path);
}

static void
test_elf_garbage(void)
{
	guint8 buf[4096];
	char *contents = NULL;
	gsize len = 0, cut;
	Elf64_Ehdr eh;
	guint i;

	/* Missing, a directory, empty. */
	g_assert_false(pidgin_plugin_file_is_foreign_toolkit("/nonexistent/x.so", NULL));
	g_assert_false(pidgin_plugin_file_is_foreign_toolkit(profile, NULL));
	check_not_foreign("empty.so", "", 0);

	/* Random bytes, with and without an ELF magic. */
	for (i = 0; i < sizeof(buf); i++)
		buf[i] = (guint8)g_test_rand_int();
	check_not_foreign("random.so", buf, sizeof(buf));
	memcpy(buf, ELFMAG, SELFMAG);
	buf[EI_CLASS] = ELFCLASS64;
	buf[EI_DATA] = ELFDATA2LSB;
	check_not_foreign("random-elf64.so", buf, sizeof(buf));
	buf[EI_CLASS] = ELFCLASS32;
	check_not_foreign("random-elf32.so", buf, sizeof(buf));

	/* A header whose program headers point far outside the file. */
	memset(&eh, 0, sizeof(eh));
	memcpy(eh.e_ident, ELFMAG, SELFMAG);
	eh.e_ident[EI_CLASS] = ELFCLASS64;
	eh.e_ident[EI_DATA] = ELFDATA2LSB;
	eh.e_phoff = G_MAXUINT64 - 8;
	eh.e_phnum = 0xffff;
	eh.e_phentsize = sizeof(Elf64_Phdr);
	check_not_foreign("bogus-phoff.so", &eh, sizeof(eh));
	eh.e_phoff = sizeof(eh);
	eh.e_phnum = 4;
	check_not_foreign("short-phdrs.so", &eh, sizeof(eh));

	/* A real GTK 2 plugin, truncated at many points: never foreign
	 * unless the dynamic section and its strings survived, never a
	 * crash. */
	if (g_file_get_contents("/usr/lib64/pidgin/history.so", &contents, &len, NULL)) {
		for (cut = 0; cut < len; cut += (cut < 256 ? 7 : 997)) {
			char *path = g_build_filename(profile, "cut.so", NULL);

			g_assert_true(g_file_set_contents(path, contents, cut, NULL));
			(void)pidgin_plugin_file_is_foreign_toolkit(path, NULL);
			g_free(path);
		}
		/* Corrupt every byte of the headers in turn. */
		for (i = 0; i < 1024 && i < len; i++) {
			char *copy = g_memdup2(contents, len);
			char *path = g_build_filename(profile, "flip.so", NULL);

			copy[i] = (char)~copy[i];
			g_assert_true(g_file_set_contents(path, copy, len, NULL));
			(void)pidgin_plugin_file_is_foreign_toolkit(path, NULL);
			g_free(path);
			g_free(copy);
		}
		g_free(contents);
	}
}

/**************************************************************************
 * Pref frames
 **************************************************************************/

#define P "/plugins/core/pluginpref-test"

static GtkWidget *
find_named(GtkWidget *widget, const char *name)
{
	GtkWidget *child;

	if (purple_strequal(gtk_widget_get_name(widget), name))
		return widget;
	for (child = gtk_widget_get_first_child(widget); child != NULL;
	     child = gtk_widget_get_next_sibling(child)) {
		GtkWidget *found = find_named(child, name);
		if (found != NULL)
			return found;
	}
	return NULL;
}

static gboolean
find_label_text(GtkWidget *widget, const char *text)
{
	GtkWidget *child;

	if (GTK_IS_LABEL(widget) &&
	    strstr(gtk_label_get_text(GTK_LABEL(widget)), text) != NULL)
		return TRUE;
	for (child = gtk_widget_get_first_child(widget); child != NULL;
	     child = gtk_widget_get_next_sibling(child))
		if (find_label_text(child, text))
			return TRUE;
	return FALSE;
}

static void
register_prefs(void)
{
	GList *paths;

	purple_prefs_add_none(P);
	purple_prefs_add_bool(P "/bool", TRUE);
	purple_prefs_add_int(P "/int", 5);
	purple_prefs_add_string(P "/string", "hello");
	purple_prefs_add_string(P "/masked", "secret");
	purple_prefs_add_string(P "/multiline", "line1\nline2");
	purple_prefs_add_string(P "/html", "<b>bold</b> text");
	purple_prefs_add_string(P "/choice_str", "b");
	purple_prefs_add_int(P "/choice_int", 20);
	purple_prefs_add_path(P "/path", "/tmp/some/where");
	paths = g_list_append(NULL, (char *)"/a");
	paths = g_list_append(paths, (char *)"/b");
	purple_prefs_add_path_list(P "/paths", paths);
	g_list_free(paths);
}

static PurplePluginPrefFrame *
make_frame(void)
{
	PurplePluginPrefFrame *frame = purple_plugin_pref_frame_new();
	PurplePluginPref *pref;

	pref = purple_plugin_pref_new_with_label("Section One");
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_label("Some explanatory text.");
	purple_plugin_pref_set_type(pref, PURPLE_PLUGIN_PREF_INFO);
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(P "/bool", "A _bool");
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(P "/int", "An int");
	purple_plugin_pref_set_bounds(pref, 1, 50);
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(P "/string", "A string");
	purple_plugin_pref_set_max_length(pref, 16);
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(P "/masked", "Password");
	purple_plugin_pref_set_masked(pref, TRUE);
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_label("Section Two");
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(P "/multiline", "Multi");
	purple_plugin_pref_set_type(pref, PURPLE_PLUGIN_PREF_STRING_FORMAT);
	purple_plugin_pref_set_format_type(pref, PURPLE_STRING_FORMAT_TYPE_MULTILINE);
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(P "/html", "HTML");
	purple_plugin_pref_set_type(pref, PURPLE_PLUGIN_PREF_STRING_FORMAT);
	purple_plugin_pref_set_format_type(pref,
		PURPLE_STRING_FORMAT_TYPE_HTML | PURPLE_STRING_FORMAT_TYPE_MULTILINE);
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(P "/choice_str", "String choice");
	purple_plugin_pref_set_type(pref, PURPLE_PLUGIN_PREF_CHOICE);
	purple_plugin_pref_add_choice(pref, "Alpha", (gpointer)"a");
	purple_plugin_pref_add_choice(pref, "Beta", (gpointer)"b");
	purple_plugin_pref_add_choice(pref, "Gamma", (gpointer)"c");
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(P "/choice_int", "Int choice");
	purple_plugin_pref_set_type(pref, PURPLE_PLUGIN_PREF_CHOICE);
	purple_plugin_pref_add_choice(pref, "Ten", GINT_TO_POINTER(10));
	purple_plugin_pref_add_choice(pref, "Twenty", GINT_TO_POINTER(20));
	purple_plugin_pref_add_choice(pref, "Thirty", GINT_TO_POINTER(30));
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(P "/path", "A path");
	purple_plugin_pref_frame_add(frame, pref);
	pref = purple_plugin_pref_new_with_name_and_label(P "/paths", "Paths");
	purple_plugin_pref_frame_add(frame, pref);

	/* A pref that does not exist is skipped. */
	pref = purple_plugin_pref_new_with_name_and_label(P "/missing", "Missing");
	purple_plugin_pref_frame_add(frame, pref);

	/* Unnamed, unlabelled: skipped. */
	pref = purple_plugin_pref_new();
	purple_plugin_pref_frame_add(frame, pref);

	return frame;
}

static char *
text_view_text(GtkWidget *view)
{
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
	GtkTextIter start, end;

	gtk_text_buffer_get_bounds(buffer, &start, &end);
	return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

static void
test_frame(void)
{
	PurplePluginPrefFrame *frame;
	GtkWidget *box, *w, *window;
	char *text;
	GtkTextBuffer *buffer;
	GtkTextIter iter;

	if (!have_gtk) {
		g_test_skip("no display");
		return;
	}

	g_assert_null(pidgin_plugin_pref_frame_to_widget(NULL));

	frame = make_frame();
	box = pidgin_plugin_pref_frame_to_widget(frame);
	g_assert_nonnull(box);
	window = gtk_window_new();
	gtk_window_set_child(GTK_WINDOW(window), box);
	gtk_window_present(GTK_WINDOW(window));
	pidgin_test_iterate();

	/* Sections and info text. */
	g_assert_true(find_label_text(box, "Section One"));
	g_assert_true(find_label_text(box, "Section Two"));
	g_assert_true(find_label_text(box, "Some explanatory text."));
	g_assert_null(find_named(box, P "/missing"));

	/* bool */
	w = find_named(box, P "/bool");
	g_assert_true(GTK_IS_CHECK_BUTTON(w));
	g_assert_true(gtk_check_button_get_active(GTK_CHECK_BUTTON(w)));
	gtk_check_button_set_active(GTK_CHECK_BUTTON(w), FALSE);
	g_assert_false(purple_prefs_get_bool(P "/bool"));
	purple_prefs_set_bool(P "/bool", TRUE);
	g_assert_true(gtk_check_button_get_active(GTK_CHECK_BUTTON(w)));

	/* int within bounds */
	w = find_named(box, P "/int");
	g_assert_true(GTK_IS_SPIN_BUTTON(w));
	g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w)), ==, 5);
	{
		double min, max;
		gtk_spin_button_get_range(GTK_SPIN_BUTTON(w), &min, &max);
		g_assert_cmpfloat(min, ==, 1);
		g_assert_cmpfloat(max, ==, 50);
	}
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(w), 42);
	g_assert_cmpint(purple_prefs_get_int(P "/int"), ==, 42);
	purple_prefs_set_int(P "/int", 7);
	g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w)), ==, 7);

	/* string */
	w = find_named(box, P "/string");
	g_assert_true(GTK_IS_ENTRY(w));
	g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(w)), ==, "hello");
	g_assert_cmpint(gtk_entry_get_max_length(GTK_ENTRY(w)), ==, 16);
	g_assert_true(gtk_entry_get_visibility(GTK_ENTRY(w)));
	gtk_editable_set_text(GTK_EDITABLE(w), "changed");
	g_assert_cmpstr(purple_prefs_get_string(P "/string"), ==, "changed");
	purple_prefs_set_string(P "/string", "from pref");
	g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(w)), ==, "from pref");

	/* masked */
	w = find_named(box, P "/masked");
	g_assert_true(GTK_IS_ENTRY(w));
	g_assert_false(gtk_entry_get_visibility(GTK_ENTRY(w)));
	g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(w)), ==, "secret");

	/* multi-line: a GtkTextView */
	w = find_named(box, P "/multiline");
	g_assert_true(GTK_IS_TEXT_VIEW(w));
	g_assert_false(PIDGIN_IS_COMPOSE_ENTRY(w));
	text = text_view_text(w);
	g_assert_cmpstr(text, ==, "line1\nline2");
	g_free(text);
	buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(w));
	gtk_text_buffer_get_end_iter(buffer, &iter);
	gtk_text_buffer_insert(buffer, &iter, "\nline3", -1);
	g_assert_cmpstr(purple_prefs_get_string(P "/multiline"), ==, "line1\nline2\nline3");
	purple_prefs_set_string(P "/multiline", "other");
	text = text_view_text(w);
	g_assert_cmpstr(text, ==, "other");
	g_free(text);

	/* HTML: a PidginComposeEntry */
	w = find_named(box, P "/html");
	g_assert_true(PIDGIN_IS_COMPOSE_ENTRY(w));
	text = pidgin_compose_entry_get_text(PIDGIN_COMPOSE_ENTRY(w));
	g_assert_cmpstr(text, ==, "bold text");
	g_free(text);
	text = pidgin_compose_entry_get_markup(PIDGIN_COMPOSE_ENTRY(w));
	g_assert_nonnull(strstr(text, "<b>bold</b>"));
	g_free(text);
	buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(w));
	gtk_text_buffer_get_end_iter(buffer, &iter);
	gtk_text_buffer_insert(buffer, &iter, " more", -1);
	g_assert_nonnull(strstr(purple_prefs_get_string(P "/html"), "text more"));
	g_assert_nonnull(strstr(purple_prefs_get_string(P "/html"), "<b>bold</b>"));
	purple_prefs_set_string(P "/html", "<i>it</i>");
	text = pidgin_compose_entry_get_text(PIDGIN_COMPOSE_ENTRY(w));
	g_assert_cmpstr(text, ==, "it");
	g_free(text);

	/* string choice */
	w = find_named(box, P "/choice_str");
	g_assert_true(GTK_IS_DROP_DOWN(w));
	g_assert_cmpuint(gtk_drop_down_get_selected(GTK_DROP_DOWN(w)), ==, 1);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(w), 2);
	g_assert_cmpstr(purple_prefs_get_string(P "/choice_str"), ==, "c");
	purple_prefs_set_string(P "/choice_str", "a");
	g_assert_cmpuint(gtk_drop_down_get_selected(GTK_DROP_DOWN(w)), ==, 0);

	/* int choice */
	w = find_named(box, P "/choice_int");
	g_assert_true(GTK_IS_DROP_DOWN(w));
	g_assert_cmpuint(gtk_drop_down_get_selected(GTK_DROP_DOWN(w)), ==, 1);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(w), 0);
	g_assert_cmpint(purple_prefs_get_int(P "/choice_int"), ==, 10);
	purple_prefs_set_int(P "/choice_int", 30);
	g_assert_cmpuint(gtk_drop_down_get_selected(GTK_DROP_DOWN(w)), ==, 2);

	/* paths: read-only labels */
	w = find_named(box, P "/path");
	g_assert_true(GTK_IS_LABEL(w));
	g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(w)), ==, "/tmp/some/where");
	w = find_named(box, P "/paths");
	g_assert_true(GTK_IS_LABEL(w));
	g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(w)), ==, "/a\n/b");

	/* Destroy the widgets, then the frame: no callback may be left. */
	gtk_window_destroy(GTK_WINDOW(window));
	pidgin_test_iterate();
	purple_plugin_pref_frame_destroy(frame);

	purple_prefs_set_bool(P "/bool", FALSE);
	purple_prefs_set_int(P "/int", 3);
	purple_prefs_set_string(P "/string", "after");
	purple_prefs_set_string(P "/multiline", "after");
	purple_prefs_set_string(P "/html", "<u>after</u>");
	purple_prefs_set_string(P "/choice_str", "b");
	purple_prefs_set_int(P "/choice_int", 20);
}

/* A second conversion of the same frame (the dialog's Configure window
 * can be opened twice) works independently. */
static void
test_frame_twice(void)
{
	PurplePluginPrefFrame *frame;
	GtkWidget *a, *b, *wa, *wb;

	if (!have_gtk) {
		g_test_skip("no display");
		return;
	}

	frame = make_frame();
	a = g_object_ref_sink(pidgin_plugin_pref_frame_to_widget(frame));
	b = g_object_ref_sink(pidgin_plugin_pref_frame_to_widget(frame));
	wa = find_named(a, P "/string");
	wb = find_named(b, P "/string");
	gtk_editable_set_text(GTK_EDITABLE(wa), "shared");
	g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(wb)), ==, "shared");
	g_object_unref(a);
	purple_prefs_set_string(P "/string", "only b");
	g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(wb)), ==, "only b");
	g_object_unref(b);
	purple_prefs_set_string(P "/string", "none");
	purple_plugin_pref_frame_destroy(frame);
}

/**************************************************************************
 * main
 **************************************************************************/

static guint
input_add(int fd, PurpleInputCondition cond, PurpleInputFunction func,
          gpointer data)
{
	return 0;
}

static PurpleEventLoopUiOps eventloop_ops = {
	g_timeout_add,
	g_source_remove,
	input_add,
	g_source_remove,
	NULL,
	g_timeout_add_seconds,
	NULL, NULL, NULL
};

int
main(int argc, char *argv[])
{
	int ret;

	g_test_init(&argc, &argv, NULL);

	/* No input method daemon on the private display (its warning would
	 * be fatal under g_test_init()). */
	g_setenv("GTK_IM_MODULE", "gtk-im-context-simple", TRUE);
	have_gtk = pidgin_test_init_gtk();
	profile = pidgin_test_profile_setup();
	purple_eventloop_set_ui_ops(&eventloop_ops);
	purple_prefs_init();
	register_prefs();

	g_test_add_func("/pluginpref/elf/gtk2", test_elf_gtk2);
	g_test_add_func("/pluginpref/elf/core-plugin", test_elf_core_plugin);
	g_test_add_func("/pluginpref/elf/garbage", test_elf_garbage);
	g_test_add_func("/pluginpref/frame", test_frame);
	g_test_add_func("/pluginpref/frame-twice", test_frame_twice);

	ret = g_test_run();

	/* No purple_prefs_uninit(): it needs the core (D-Bus pointer map). */
	pidgin_test_profile_cleanup();
	return ret;
}
