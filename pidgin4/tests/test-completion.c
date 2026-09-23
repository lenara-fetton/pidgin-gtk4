/*
 * pidgin4: tests for the buddy name completion popover
 * (pidgincompletion.c).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "account.h"
#include "blist.h"
#include "core.h"
#include "debug.h"
#include "eventloop.h"
#include "request.h"

#include "gtkutils.h"
#include "pidgincompletion.h"

#include "test-display.h"
#include "test-support.h"

static PurpleAccount *account1, *account2;

static GtkWidget *
find_popover(GtkWidget *entry)
{
	GtkWidget *child;

	for (child = gtk_widget_get_first_child(entry); child != NULL;
	     child = gtk_widget_get_next_sibling(child))
		if (GTK_IS_POPOVER(child))
			return child;
	return NULL;
}

static GtkListView *
find_list_view(GtkWidget *widget)
{
	GtkWidget *child;

	if (GTK_IS_LIST_VIEW(widget))
		return GTK_LIST_VIEW(widget);
	for (child = gtk_widget_get_first_child(widget); child != NULL;
	     child = gtk_widget_get_next_sibling(child)) {
		GtkListView *found = find_list_view(child);
		if (found != NULL)
			return found;
	}
	return NULL;
}

static guint
n_rows(GtkWidget *entry)
{
	GtkListView *view = find_list_view(find_popover(entry));

	return g_list_model_get_n_items(G_LIST_MODEL(gtk_list_view_get_model(view)));
}

static gboolean
popover_visible(GtkWidget *entry)
{
	return gtk_widget_get_visible(find_popover(entry));
}

static gboolean
press(GtkWidget *entry, guint keyval)
{
	GListModel *controllers = gtk_widget_observe_controllers(entry);
	gboolean handled = FALSE;
	guint i;

	for (i = 0; i < g_list_model_get_n_items(controllers); i++) {
		GtkEventController *c = g_list_model_get_item(controllers, i);

		if (purple_strequal(gtk_event_controller_get_name(c), "pidgin-buddy-completion"))
			g_signal_emit_by_name(c, "key-pressed", keyval, 0, 0, &handled);
		g_object_unref(c);
	}
	g_object_unref(controllers);
	pidgin_test_iterate();
	return handled;
}

static void
type(GtkWidget *entry, const char *text)
{
	gtk_editable_set_text(GTK_EDITABLE(entry), text);
	pidgin_test_iterate();
}

static GtkWidget *
new_window(GtkWidget **entry_out, GtkWidget **dropdown_out)
{
	GtkWidget *window = gtk_window_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	GtkWidget *entry = gtk_entry_new();

	gtk_box_append(GTK_BOX(box), entry);
	if (dropdown_out != NULL) {
		*dropdown_out = pidgin_account_dropdown_new(account1, TRUE, NULL, NULL);
		gtk_box_append(GTK_BOX(box), *dropdown_out);
	}
	gtk_window_set_child(GTK_WINDOW(window), box);
	gtk_window_present(GTK_WINDOW(window));
	pidgin_test_iterate();
	gtk_widget_grab_focus(entry);
	pidgin_test_iterate();
	*entry_out = entry;
	return window;
}

static void
test_matching(void)
{
	GtkWidget *entry, *window = new_window(&entry, NULL);

	pidgin_buddy_completion_attach(entry, NULL, TRUE);

	/* Name prefixes, case-insensitive. */
	type(entry, "AL");
	g_assert_true(popover_visible(entry));
	g_assert_cmpuint(n_rows(entry), ==, 2);        /* alice, alfred */

	/* A word of the alias. */
	type(entry, "lidd");
	g_assert_true(popover_visible(entry));
	g_assert_cmpuint(n_rows(entry), ==, 1);

	/* The alias itself. */
	type(entry, "big");
	g_assert_cmpuint(n_rows(entry), ==, 1);

	/* No match, and an exact single match: closed. */
	type(entry, "zzz");
	g_assert_false(popover_visible(entry));
	type(entry, "bob@example.com");
	g_assert_false(popover_visible(entry));

	/* Escape closes. */
	type(entry, "b");
	g_assert_true(popover_visible(entry));
	g_assert_true(press(entry, GDK_KEY_Escape));
	g_assert_false(popover_visible(entry));
	/* Keys pass through while closed. */
	g_assert_false(press(entry, GDK_KEY_Down));

	/* Down, Down, Up, Enter picks the first. */
	type(entry, "al");
	g_assert_true(press(entry, GDK_KEY_Down));
	g_assert_true(press(entry, GDK_KEY_Down));
	g_assert_true(press(entry, GDK_KEY_Up));
	g_assert_true(press(entry, GDK_KEY_Return));
	g_assert_false(popover_visible(entry));
	g_assert_true(g_str_has_prefix(gtk_editable_get_text(GTK_EDITABLE(entry)), "al"));
	g_assert_nonnull(strchr(gtk_editable_get_text(GTK_EDITABLE(entry)), '@'));

	/* Tab picks the first without a selection; the entry keeps focus. */
	type(entry, "bo");
	g_assert_true(press(entry, GDK_KEY_Tab));
	g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(entry)), ==, "bob@example.com");
	g_assert_true(gtk_widget_is_ancestor(gtk_root_get_focus(GTK_ROOT(window)), entry));

	/* A click (the list view's activate). */
	type(entry, "big");
	g_signal_emit_by_name(find_list_view(find_popover(entry)), "activate", 0);
	pidgin_test_iterate();
	g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(entry)), ==, "alfred@example.org");

	gtk_window_destroy(GTK_WINDOW(window));
	pidgin_test_iterate();
}

static void
test_connected_only(void)
{
	GtkWidget *entry, *window = new_window(&entry, NULL);

	/* The accounts are offline: nothing is offered. */
	pidgin_buddy_completion_attach(entry, NULL, FALSE);
	type(entry, "al");
	g_assert_false(popover_visible(entry));

	gtk_window_destroy(GTK_WINDOW(window));
	pidgin_test_iterate();
}

static void
test_account_dropdown(void)
{
	GtkWidget *entry, *dropdown, *window = new_window(&entry, &dropdown);

	pidgin_buddy_completion_attach(entry, dropdown, TRUE);
	g_assert_true(pidgin_account_dropdown_get_selected(dropdown) == account1);
	type(entry, "carol");
	g_assert_true(press(entry, GDK_KEY_Tab));
	g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(entry)), ==, "carol@other.example");
	g_assert_true(pidgin_account_dropdown_get_selected(dropdown) == account2);

	/* The drop-down goes away first: picking still works. */
	gtk_box_remove(GTK_BOX(gtk_widget_get_parent(dropdown)), dropdown);
	pidgin_test_iterate();
	type(entry, "al");
	g_assert_true(press(entry, GDK_KEY_Tab));

	gtk_window_destroy(GTK_WINDOW(window));
	pidgin_test_iterate();
}

static void
test_request_field(void)
{
	GtkWidget *entry, *dropdown, *window = new_window(&entry, &dropdown);
	PurpleRequestField *field = purple_request_field_account_new("account", "Account", account1);
	PurpleRequestField *hidden = purple_request_field_account_new("account", "Account", account1);

	/* A visible field: its drop-down (ui_data) and value follow. */
	purple_request_field_set_ui_data(field, dropdown);
	pidgin_buddy_completion_attach_to_field(entry, field, TRUE);
	type(entry, "carol");
	g_assert_false(press(entry, GDK_KEY_Return));
	/* Enter without a selection only closes the popover. */
	g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(entry)), ==, "carol");
	type(entry, "caro");
	g_assert_true(press(entry, GDK_KEY_Down));
	g_assert_true(press(entry, GDK_KEY_Return));
	g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(entry)), ==, "carol@other.example");
	g_assert_true(pidgin_account_dropdown_get_selected(dropdown) == account2);
	g_assert_true(purple_request_field_account_get_value(field) == account2);
	gtk_window_destroy(GTK_WINDOW(window));
	pidgin_test_iterate();

	/* A hidden field (no widget): the value still follows. */
	window = new_window(&entry, NULL);
	pidgin_buddy_completion_attach_to_field(entry, hidden, TRUE);
	type(entry, "caro");
	g_assert_true(press(entry, GDK_KEY_Tab));
	g_assert_true(purple_request_field_account_get_value(hidden) == account2);
	gtk_window_destroy(GTK_WINDOW(window));
	pidgin_test_iterate();

	purple_request_field_destroy(field);
	purple_request_field_destroy(hidden);
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

/* libpurple (no log domain) logs criticals for buddies whose prpl is not
 * loaded; they must not fail the test. Everything else stays fatal. */
static gboolean
fatal_filter(const char *domain, GLogLevelFlags level, const char *message,
             gpointer data)
{
	return domain != NULL;
}

int
main(int argc, char *argv[])
{
	PurpleGroup *group;
	int ret;

	g_test_init(&argc, &argv, NULL);
	g_test_log_set_fatal_handler(fatal_filter, NULL);
	g_setenv("GTK_IM_MODULE", "gtk-im-context-simple", TRUE);

	if (!pidgin_test_init_gtk())
		return 77;

	pidgin_test_profile_setup();
	purple_debug_set_enabled(FALSE);
	purple_eventloop_set_ui_ops(&eventloop_ops);
	g_assert_true(purple_core_init("pidgin4-test"));
	purple_set_blist(purple_blist_new());

	account1 = purple_account_new("me@example.com", "prpl-jabber");
	purple_accounts_add(account1);
	account2 = purple_account_new("me@other.example", "prpl-jabber");
	purple_accounts_add(account2);

	group = purple_group_new("Buddies");
	purple_blist_add_group(group, NULL);
	purple_blist_add_buddy(purple_buddy_new(account1, "alice@example.com", "Alice Liddell"),
	                       NULL, group, NULL);
	purple_blist_add_buddy(purple_buddy_new(account1, "bob@example.com", NULL),
	                       NULL, group, NULL);
	purple_blist_add_buddy(purple_buddy_new(account1, "alfred@example.org", "Big Al"),
	                       NULL, group, NULL);
	purple_blist_add_buddy(purple_buddy_new(account2, "carol@other.example", NULL),
	                       NULL, group, NULL);

	g_test_add_func("/completion/matching", test_matching);
	g_test_add_func("/completion/connected-only", test_connected_only);
	g_test_add_func("/completion/account-dropdown", test_account_dropdown);
	g_test_add_func("/completion/request-field", test_request_field);

	ret = g_test_run();

	/* No purple_core_quit(): it would save into the scratch directory,
	 * which is removed anyway. */
	pidgin_test_profile_cleanup();
	return ret;
}
