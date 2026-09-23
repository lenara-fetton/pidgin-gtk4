/*
 * pidgin4: tests for the widget <-> pref bindings (pidginprefbinding.c).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "eventloop.h"
#include "prefs.h"

#include "gtkutils.h"
#include "pidginprefbinding.h"

#include "test-display.h"
#include "test-support.h"

/* prefs.c schedules its save through the event loop ops. */
static guint
t_timeout_add(guint interval, GSourceFunc function, gpointer data)
{
	return g_timeout_add(interval, function, data);
}

static guint
t_timeout_add_seconds(guint interval, GSourceFunc function, gpointer data)
{
	return g_timeout_add_seconds(interval, function, data);
}

static PurpleEventLoopUiOps eventloop_ops = {
	t_timeout_add, g_source_remove, NULL, NULL, NULL, t_timeout_add_seconds,
	NULL, NULL, NULL
};

static GtkWidget *
sink(GtkWidget *w)
{
	return g_object_ref_sink(w);
}

static void
count_cb(GtkWidget *w, gpointer data)
{
	(*(int *)data)++;
}

static void
test_bool(void)
{
	GtkWidget *check = sink(gtk_check_button_new());
	GtkWidget *sw = sink(gtk_switch_new());
	int toggles = 0;

	purple_prefs_add_bool("/t/bool", TRUE);
	pidgin_pref_bind_bool(check, "/t/bool");
	pidgin_pref_bind_bool(sw, "/t/bool");
	g_assert_true(gtk_check_button_get_active(GTK_CHECK_BUTTON(check)));
	g_assert_true(gtk_switch_get_active(GTK_SWITCH(sw)));

	g_signal_connect(check, "toggled", G_CALLBACK(count_cb), &toggles);

	/* pref -> widgets */
	purple_prefs_set_bool("/t/bool", FALSE);
	g_assert_false(gtk_check_button_get_active(GTK_CHECK_BUTTON(check)));
	g_assert_false(gtk_switch_get_active(GTK_SWITCH(sw)));
	g_assert_cmpint(toggles, ==, 1);

	/* widget -> pref (-> the other widget), without feedback loops */
	gtk_check_button_set_active(GTK_CHECK_BUTTON(check), TRUE);
	g_assert_true(purple_prefs_get_bool("/t/bool"));
	g_assert_true(gtk_switch_get_active(GTK_SWITCH(sw)));
	g_assert_cmpint(toggles, ==, 2);
	gtk_switch_set_active(GTK_SWITCH(sw), FALSE);
	g_assert_false(purple_prefs_get_bool("/t/bool"));
	g_assert_false(gtk_check_button_get_active(GTK_CHECK_BUTTON(check)));
	g_assert_cmpint(toggles, ==, 3);

	/* Destroyed widgets disconnect their pref callbacks. */
	g_object_unref(check);
	g_object_unref(sw);
	purple_prefs_set_bool("/t/bool", TRUE);
	purple_prefs_set_bool("/t/bool", FALSE);
	g_assert_cmpint(toggles, ==, 3);
}

static void
test_int(void)
{
	GtkWidget *spin = sink(pidgin_pref_spin_new("/t/int", 0, 100));

	g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin)), ==, 42);
	purple_prefs_set_int("/t/int", 7);
	g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin)), ==, 7);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), 99);
	g_assert_cmpint(purple_prefs_get_int("/t/int"), ==, 99);
	g_object_unref(spin);
	purple_prefs_set_int("/t/int", 1);
}

static void
test_string(void)
{
	GtkWidget *entry = sink(pidgin_pref_entry_new("/t/str"));
	GtkWidget *path = sink(gtk_entry_new());

	g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(entry)), ==, "hello");
	purple_prefs_set_string("/t/str", "world");
	g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(entry)), ==, "world");
	gtk_editable_set_text(GTK_EDITABLE(entry), "typed");
	g_assert_cmpstr(purple_prefs_get_string("/t/str"), ==, "typed");
	/* a NULL string pref shows as "" */
	purple_prefs_set_string("/t/str", NULL);
	g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(entry)), ==, "");

	pidgin_pref_bind_path(path, "/t/path");
	g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(path)), ==, "/tmp/x");
	gtk_editable_set_text(GTK_EDITABLE(path), "/tmp/y");
	g_assert_cmpstr(purple_prefs_get_path("/t/path"), ==, "/tmp/y");
	purple_prefs_set_path("/t/path", "/tmp/z");
	g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(path)), ==, "/tmp/z");

	g_object_unref(entry);
	g_object_unref(path);
	purple_prefs_set_string("/t/str", "after");
}

static void
test_dropdown(void)
{
	static const char *const labels[] = { "Never", "When away", "Always" };
	static const char *const values[] = { "never", "away", "always" };
	static const char *const int_labels[] = { "Top", "Bottom", "Left" };
	static const int int_values[] = { 2, 3, 0 };
	GtkWidget *dd = sink(pidgin_pref_dropdown_string_new("/t/dd", labels, values, 3));
	GtkWidget *ddi = sink(pidgin_pref_dropdown_int_new("/t/ddint", int_labels,
	                                                   int_values, 3));

	/* initial value selected, and binding wrote nothing */
	g_assert_cmpuint(gtk_drop_down_get_selected(GTK_DROP_DOWN(dd)), ==, 1);
	g_assert_cmpstr(purple_prefs_get_string("/t/dd"), ==, "away");
	g_assert_cmpuint(gtk_drop_down_get_selected(GTK_DROP_DOWN(ddi)), ==, 1);
	g_assert_cmpint(purple_prefs_get_int("/t/ddint"), ==, 3);

	purple_prefs_set_string("/t/dd", "always");
	g_assert_cmpuint(gtk_drop_down_get_selected(GTK_DROP_DOWN(dd)), ==, 2);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), 0);
	g_assert_cmpstr(purple_prefs_get_string("/t/dd"), ==, "never");
	/* An unknown value leaves the selection and the pref alone. */
	purple_prefs_set_string("/t/dd", "bogus");
	g_assert_cmpuint(gtk_drop_down_get_selected(GTK_DROP_DOWN(dd)), ==, 0);
	g_assert_cmpstr(purple_prefs_get_string("/t/dd"), ==, "bogus");

	purple_prefs_set_int("/t/ddint", 0);
	g_assert_cmpuint(gtk_drop_down_get_selected(GTK_DROP_DOWN(ddi)), ==, 2);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(ddi), 0);
	g_assert_cmpint(purple_prefs_get_int("/t/ddint"), ==, 2);

	g_object_unref(dd);
	g_object_unref(ddi);
	purple_prefs_set_string("/t/dd", "never");
	purple_prefs_set_int("/t/ddint", 3);
}

static void
test_sensitive(void)
{
	GtkWidget *a = sink(gtk_label_new("a"));
	GtkWidget *b = sink(gtk_label_new("b"));
	GtkWidget *c = sink(gtk_label_new("c"));
	GtkWidget *d = sink(gtk_label_new("d"));

	purple_prefs_set_bool("/t/bool", TRUE);
	purple_prefs_set_string("/t/dd", "custom");
	pidgin_pref_bind_sensitive(a, "/t/bool", FALSE);
	pidgin_pref_bind_sensitive(b, "/t/bool", TRUE);
	pidgin_pref_bind_sensitive_string(c, "/t/dd", "custom");
	pidgin_pref_bind_insensitive_string(d, "/t/dd", "custom");
	g_assert_true(gtk_widget_get_sensitive(a));
	g_assert_false(gtk_widget_get_sensitive(b));
	g_assert_true(gtk_widget_get_sensitive(c));
	g_assert_false(gtk_widget_get_sensitive(d));

	purple_prefs_set_bool("/t/bool", FALSE);
	purple_prefs_set_string("/t/dd", "none");
	g_assert_false(gtk_widget_get_sensitive(a));
	g_assert_true(gtk_widget_get_sensitive(b));
	g_assert_false(gtk_widget_get_sensitive(c));
	g_assert_true(gtk_widget_get_sensitive(d));

	/* A value and a sensitivity binding on one widget coexist. */
	{
		GtkWidget *check = sink(gtk_check_button_new());

		pidgin_pref_bind_bool(check, "/t/bool2");
		pidgin_pref_bind_sensitive(check, "/t/bool", FALSE);
		purple_prefs_set_bool("/t/bool2", TRUE);
		purple_prefs_set_bool("/t/bool", TRUE);
		g_assert_true(gtk_check_button_get_active(GTK_CHECK_BUTTON(check)));
		g_assert_true(gtk_widget_get_sensitive(check));
		g_object_unref(check);
	}

	g_object_unref(a);
	g_object_unref(b);
	g_object_unref(c);
	g_object_unref(d);
	purple_prefs_set_bool("/t/bool", FALSE);
	purple_prefs_set_string("/t/dd", "x");
}

static void
test_checkbox_new(void)
{
	GtkWidget *check = sink(pidgin_pref_checkbox_new("_Label", "/t/bool2"));

	g_assert_cmpstr(gtk_check_button_get_label(GTK_CHECK_BUTTON(check)), ==, "_Label");
	g_assert_true(gtk_check_button_get_use_underline(GTK_CHECK_BUTTON(check)));
	g_assert_true(gtk_check_button_get_active(GTK_CHECK_BUTTON(check)));
	g_object_unref(check);
}

int
main(int argc, char *argv[])
{
	int ret;

	if (!pidgin_test_init_gtk())
		return 77;

	g_test_init(&argc, &argv, NULL);
	pidgin_test_profile_setup();
	purple_eventloop_set_ui_ops(&eventloop_ops);
	purple_prefs_init();

	purple_prefs_add_none("/t");
	purple_prefs_add_int("/t/int", 42);
	purple_prefs_add_string("/t/str", "hello");
	purple_prefs_add_path("/t/path", "/tmp/x");
	purple_prefs_add_string("/t/dd", "away");
	purple_prefs_add_int("/t/ddint", 3);
	purple_prefs_add_bool("/t/bool2", FALSE);

	g_test_add_func("/prefbinding/bool", test_bool);
	g_test_add_func("/prefbinding/int", test_int);
	g_test_add_func("/prefbinding/string", test_string);
	g_test_add_func("/prefbinding/dropdown", test_dropdown);
	g_test_add_func("/prefbinding/sensitive", test_sensitive);
	g_test_add_func("/prefbinding/checkbox-new", test_checkbox_new);

	ret = g_test_run();
	pidgin_test_profile_cleanup();
	return ret;
}
