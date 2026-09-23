/*
 * Unit tests for pidginblistmodel.c: node items, placement (visibility,
 * ordering, expandable contacts) and the sort functions, driven through a
 * real libpurple buddy list whose update/remove UI ops feed the model, as
 * gtkblist.c does.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "pidgin-internal.h"

#include <glib/gstdio.h>

#include "account.h"
#include "blist.h"
#include "core.h"
#include "debug.h"
#include "prefs.h"

#include "gtkeventloop.h"
#include "pidginblistmodel.h"

static PidginBlistModel *model;
static PurpleAccount *account;

static void
ui_update(PurpleBuddyList *list, PurpleBlistNode *node)
{
	if (model != NULL)
		pidgin_blist_model_update(model, node);
}

static void
ui_remove(PurpleBuddyList *list, PurpleBlistNode *node)
{
	if (model != NULL) {
		pidgin_blist_model_remove(model, node);
		if (purple_blist_node_get_parent(node) != NULL)
			pidgin_blist_model_update(model, purple_blist_node_get_parent(node));
	}
}

static PurpleBlistUiOps blist_ops = {
	NULL, NULL, NULL, ui_update, ui_remove,
};

/**************************************************************************
 * Helpers
 **************************************************************************/

static PurpleGroup *
add_group(const char *name)
{
	PurpleGroup *group = purple_group_new(name);

	purple_blist_add_group(group, NULL);
	return group;
}

static PurpleContact *
add_contact(PurpleGroup *group, const char *alias, const char *name, ...)
{
	PurpleContact *contact = purple_contact_new();
	va_list args;

	purple_blist_add_contact(contact, group, NULL);
	va_start(args, name);
	for (; name != NULL; name = va_arg(args, const char *)) {
		PurpleBuddy *buddy = purple_buddy_new(account, name, NULL);
		purple_blist_add_buddy(buddy, contact, group, NULL);
	}
	va_end(args);
	if (alias != NULL)
		purple_blist_alias_contact(contact, alias);
	return contact;
}

static PurpleChat *
add_chat(PurpleGroup *group, const char *alias)
{
	GHashTable *components = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                               g_free, g_free);
	PurpleChat *chat;

	g_hash_table_insert(components, g_strdup("room"), g_strdup(alias));
	chat = purple_chat_new(account, alias, components);
	purple_blist_add_chat(chat, group, NULL);
	return chat;
}

static GListModel *
children_of(PurpleBlistNode *node)
{
	PidginBlistNodeItem *item = pidgin_blist_model_lookup(model, node);

	g_assert_nonnull(item);
	return pidgin_blist_node_item_get_children(item);
}

static PurpleBlistNode *
node_at(GListModel *list, guint pos)
{
	PidginBlistNodeItem *item = g_list_model_get_item(list, pos);
	PurpleBlistNode *node;

	g_assert_nonnull(item);
	node = pidgin_blist_node_item_get_node(item);
	g_object_unref(item);
	return node;
}

/* "a,b,c": the sort names of the visible children of @parent (or of the
 * root when NULL). */
static char *
visible_names(PurpleBlistNode *parent)
{
	GListModel *list;
	GString *str = g_string_new(NULL);
	guint i, n;

	if (parent == NULL)
		list = g_object_ref(pidgin_blist_model_get_root(model));
	else
		list = children_of(parent);
	if (list == NULL)
		return g_string_free(str, FALSE);

	n = g_list_model_get_n_items(list);
	for (i = 0; i < n; i++) {
		PurpleBlistNode *node = node_at(list, i);
		const char *name;

		if (PURPLE_BLIST_NODE_IS_GROUP(node))
			name = purple_group_get_name((PurpleGroup *)node);
		else if (PURPLE_BLIST_NODE_IS_CONTACT(node))
			name = purple_contact_get_alias((PurpleContact *)node);
		else if (PURPLE_BLIST_NODE_IS_CHAT(node))
			name = purple_chat_get_name((PurpleChat *)node);
		else
			name = purple_buddy_get_name((PurpleBuddy *)node);
		if (str->len > 0)
			g_string_append_c(str, ',');
		g_string_append(str, name);
	}
	g_object_unref(list);
	return g_string_free(str, FALSE);
}

#define assert_names(parent, expected) G_STMT_START { \
	char *_names = visible_names((PurpleBlistNode *)(parent)); \
	g_assert_cmpstr(_names, ==, (expected)); \
	g_free(_names); \
} G_STMT_END

/* Removes every group (and so every node) from the buddy list. */
static void
clear_blist(void)
{
	PurpleBlistNode *gnode;

	while ((gnode = purple_blist_get_root()) != NULL) {
		PurpleBlistNode *cnode;

		while ((cnode = purple_blist_node_get_first_child(gnode)) != NULL) {
			if (PURPLE_BLIST_NODE_IS_CONTACT(cnode))
				purple_blist_remove_contact((PurpleContact *)cnode);
			else if (PURPLE_BLIST_NODE_IS_CHAT(cnode))
				purple_blist_remove_chat((PurpleChat *)cnode);
		}
		purple_blist_remove_group((PurpleGroup *)gnode);
	}
}

static void
setup(void)
{
	model = pidgin_blist_model_new();
	pidgin_blist_model_set_show_flags(model,
		PIDGIN_BLIST_SHOW_OFFLINE | PIDGIN_BLIST_SHOW_DISCONNECTED);
	pidgin_blist_model_set_sort_func(model, pidgin_blist_sort_alphabetical);
}

static void
teardown(void)
{
	clear_blist();
	g_assert_cmpuint(pidgin_blist_model_count_visible(model, -1), ==, 0);
	g_clear_object(&model);
}

/**************************************************************************
 * Tests
 **************************************************************************/

static void
test_sort_functions(void)
{
	PurpleGroup *g = add_group("Sorting");
	PurpleContact *a = add_contact(g, "alpha", "a@x", NULL);
	PurpleContact *b = add_contact(g, "Bravo", "b@x", NULL);
	PurpleContact *b2 = add_contact(g, "bravo", "b2@x", NULL);
	PurpleChat *chat = add_chat(g, "room");
	PurpleBlistNode *na = (PurpleBlistNode *)a, *nb = (PurpleBlistNode *)b;
	PurpleBlistNode *nb2 = (PurpleBlistNode *)b2, *nc = (PurpleBlistNode *)chat;

	/* Alphabetical: case-insensitive, chats mixed in by name, ties by
	 * pointer, and a strict total order. */
	g_assert_cmpint(pidgin_blist_sort_alphabetical(na, nb), <, 0);
	g_assert_cmpint(pidgin_blist_sort_alphabetical(nb, na), >, 0);
	g_assert_cmpint(pidgin_blist_sort_alphabetical(nc, na), >, 0);  /* "room" > "alpha" */
	g_assert_cmpint(pidgin_blist_sort_alphabetical(nb, nb2), !=, 0);
	g_assert_cmpint(pidgin_blist_sort_alphabetical(nb, nb2),
	                ==, -pidgin_blist_sort_alphabetical(nb2, nb));
	g_assert_cmpint(pidgin_blist_sort_alphabetical(na, na), ==, 0);

	/* Status and log activity: all contacts are offline with no logs, so
	 * they fall back to names; chats go last. */
	g_assert_cmpint(pidgin_blist_sort_status(na, nb), <, 0);
	g_assert_cmpint(pidgin_blist_sort_status(nc, nb), >, 0);
	g_assert_cmpint(pidgin_blist_sort_status(nb, nc), <, 0);
	g_assert_cmpint(pidgin_blist_sort_log_activity(na, nb), <, 0);
	g_assert_cmpint(pidgin_blist_sort_log_activity(nc, na), >, 0);
	g_assert_cmpint(pidgin_blist_sort_log_activity(nb, nb2),
	                ==, -pidgin_blist_sort_log_activity(nb2, nb));
}

static void
test_order_and_resort(void)
{
	PurpleGroup *g1 = add_group("Zeta");
	PurpleGroup *g2 = add_group("Alpha");
	PurpleContact *c1, *c3;
	PurpleChat *chat;

	c1 = add_contact(g1, "Mike", "mike@x", NULL);
	add_contact(g1, "charlie", "charlie@x", NULL);
	c3 = add_contact(g1, "Xray", "xray@x", NULL);
	chat = add_chat(g1, "lounge");
	add_contact(g2, "solo", "solo@x", NULL);

	/* Groups keep the buddy list order (adding without a position
	 * prepends), children are sorted. */
	assert_names(NULL, "Alpha,Zeta");
	assert_names(g1, "charlie,lounge,Mike,Xray");
	assert_names(g2, "solo");
	g_assert_true(pidgin_blist_model_check(model));

	/* Renaming re-sorts. */
	purple_blist_alias_contact(c3, "bravo");
	assert_names(g1, "bravo,charlie,lounge,Mike");
	purple_blist_alias_chat(chat, "zulu");
	assert_names(g1, "bravo,charlie,Mike,zulu");
	g_assert_true(pidgin_blist_model_check(model));

	/* Sorting "none": the buddy list order (each add prepended). */
	pidgin_blist_model_set_sort_func(model, NULL);
	pidgin_blist_model_update_all(model);
	assert_names(g1, "zulu,bravo,charlie,Mike");
	g_assert_true(pidgin_blist_model_check(model));

	/* Moving a node in the buddy list moves it in the model. */
	purple_blist_add_contact(c1, g1, (PurpleBlistNode *)chat);
	assert_names(g1, "zulu,Mike,bravo,charlie");
	purple_blist_add_contact(c1, g2, NULL);
	assert_names(g1, "zulu,bravo,charlie");
	assert_names(g2, "Mike,solo");
	purple_blist_add_group(g1, NULL);
	assert_names(NULL, "Zeta,Alpha");
	assert_names(g1, "zulu,bravo,charlie");
	g_assert_true(pidgin_blist_model_check(model));

	/* And back to alphabetical. */
	pidgin_blist_model_set_sort_func(model, pidgin_blist_sort_alphabetical);
	pidgin_blist_model_update_all(model);
	assert_names(g2, "Mike,solo");
	assert_names(g1, "bravo,charlie,zulu");
	g_assert_true(pidgin_blist_model_check(model));
}

static void
test_visibility(void)
{
	PurpleGroup *g1 = add_group("Friends");
	PurpleGroup *g2 = add_group("Empty");
	PurpleContact *c;
	PurpleBuddy *buddy;

	c = add_contact(g1, "Pat", "pat@x", NULL);
	add_chat(g1, "room");
	buddy = purple_contact_get_priority_buddy(c);

	assert_names(NULL, "Friends");
	g_assert_cmpuint(pidgin_blist_model_count_visible(model, PURPLE_BLIST_GROUP_NODE), ==, 1);

	/* Empty groups. */
	pidgin_blist_model_set_show_flags(model, PIDGIN_BLIST_SHOW_OFFLINE |
		PIDGIN_BLIST_SHOW_DISCONNECTED | PIDGIN_BLIST_SHOW_EMPTY_GROUPS);
	pidgin_blist_model_update_all(model);
	assert_names(NULL, "Empty,Friends");
	assert_names(g2, "");

	/* Offline buddies of a disconnected account, as Pidgin 2: nothing but
	 * the empty groups. */
	pidgin_blist_model_set_show_flags(model, PIDGIN_BLIST_SHOW_EMPTY_GROUPS);
	pidgin_blist_model_update_all(model);
	assert_names(NULL, "Empty,Friends");
	assert_names(g1, "");
	g_assert_false(pidgin_blist_model_buddy_is_displayable(model, buddy));

	/* Disconnected accounts shown, offline buddies not: the chat only. */
	pidgin_blist_model_set_show_flags(model, PIDGIN_BLIST_SHOW_DISCONNECTED);
	pidgin_blist_model_update_all(model);
	assert_names(NULL, "Friends");
	assert_names(g1, "room");

	/* The per-node "show_offline" setting. */
	purple_blist_node_set_bool((PurpleBlistNode *)buddy, "show_offline", TRUE);
	pidgin_blist_model_update(model, (PurpleBlistNode *)buddy);
	assert_names(g1, "Pat,room");
	g_assert_true(pidgin_blist_model_buddy_is_displayable(model, buddy));

	/* A recent sign-on/off keeps a buddy displayed. */
	purple_blist_node_set_bool((PurpleBlistNode *)buddy, "show_offline", FALSE);
	pidgin_blist_model_update(model, (PurpleBlistNode *)buddy);
	assert_names(g1, "room");
	pidgin_blist_node_item_set_recent_signonoff(
		pidgin_blist_model_lookup(model, (PurpleBlistNode *)buddy), TRUE, 0);
	pidgin_blist_model_update(model, (PurpleBlistNode *)buddy);
	assert_names(g1, "Pat,room");
	g_assert_true(pidgin_blist_model_check(model));

	/* Invisible nodes never show. */
	purple_blist_node_set_flags((PurpleBlistNode *)c, PURPLE_BLIST_NODE_FLAG_INVISIBLE);
	pidgin_blist_model_update(model, (PurpleBlistNode *)c);
	assert_names(g1, "room");
	purple_blist_node_set_flags((PurpleBlistNode *)c, 0);

	pidgin_blist_model_set_show_flags(model,
		PIDGIN_BLIST_SHOW_OFFLINE | PIDGIN_BLIST_SHOW_DISCONNECTED);
	pidgin_blist_model_update_all(model);
	assert_names(NULL, "Friends");
	g_assert_true(pidgin_blist_model_check(model));
}

static void
test_expandable_contacts(void)
{
	PurpleGroup *g = add_group("People");
	PurpleContact *c = add_contact(g, "Sam", "sam@x", "sam@y", NULL);
	PurpleContact *single = add_contact(g, "Una", "una@x", NULL);
	PidginBlistNodeItem *item;
	PurpleBuddy *second;
	GListModel *children;

	item = pidgin_blist_model_lookup(model, (PurpleBlistNode *)c);
	g_assert_true(pidgin_blist_node_item_get_expandable(item));
	assert_names(c, "sam@y,sam@x");  /* adds prepend */

	item = pidgin_blist_model_lookup(model, (PurpleBlistNode *)single);
	g_assert_false(pidgin_blist_node_item_get_expandable(item));
	g_assert_null(pidgin_blist_node_item_get_children(item));

	/* Adding a buddy to a single contact makes it expandable. */
	purple_blist_add_buddy(purple_buddy_new(account, "una@y", NULL), single, g, NULL);
	item = pidgin_blist_model_lookup(model, (PurpleBlistNode *)single);
	g_assert_true(pidgin_blist_node_item_get_expandable(item));
	assert_names(single, "una@y,una@x");

	/* Removing one makes it a single row again. */
	second = (PurpleBuddy *)purple_blist_node_get_sibling_next(
		purple_blist_node_get_first_child((PurpleBlistNode *)c));
	purple_blist_remove_buddy(second);
	item = pidgin_blist_model_lookup(model, (PurpleBlistNode *)c);
	g_assert_false(pidgin_blist_node_item_get_expandable(item));
	children = pidgin_blist_node_item_get_children(item);
	g_assert_null(children);
	assert_names(g, "Sam,Una");

	/* Merging contacts. */
	purple_blist_merge_contact(c, (PurpleBlistNode *)single);
	assert_names(g, "Una");
	assert_names(single, "una@y,una@x,sam@y");  /* merged buddies are appended */
	g_assert_true(pidgin_blist_model_check(model));
	g_assert_cmpuint(pidgin_blist_model_count_visible(model, PURPLE_BLIST_BUDDY_NODE), ==, 3);
}

static int inserted, refreshed, notifies;

static void
inserted_cb(PidginBlistModel *m, PidginBlistNodeItem *item, gpointer data)
{
	inserted++;
}

static void
refresh_cb(PidginBlistModel *m, PidginBlistNodeItem *item, gpointer data)
{
	refreshed++;
}

static void
notify_cb(GObject *obj, GParamSpec *pspec, gpointer data)
{
	notifies++;
}

static void
test_signals_and_properties(void)
{
	PurpleGroup *g;
	PurpleContact *c;
	PidginBlistNodeItem *item;
	GIcon *icon1, *icon2;

	g_signal_connect(model, "item-inserted", G_CALLBACK(inserted_cb), NULL);
	g_signal_connect(model, "item-refresh", G_CALLBACK(refresh_cb), NULL);
	inserted = refreshed = 0;

	g = add_group("Signals");
	c = add_contact(g, "Kim", "kim@x", NULL);
	g_assert_cmpint(inserted, >=, 2);  /* group and contact */
	g_assert_cmpint(refreshed, >=, 2);

	/* An update that does not move anything inserts nothing, but asks
	 * for a refresh. */
	inserted = refreshed = 0;
	pidgin_blist_model_update(model, (PurpleBlistNode *)c);
	g_assert_cmpint(inserted, ==, 0);
	g_assert_cmpint(refreshed, >=, 1);

	/* Properties notify only on change. */
	item = pidgin_blist_model_lookup(model, (PurpleBlistNode *)c);
	g_signal_connect(item, "notify", G_CALLBACK(notify_cb), NULL);
	notifies = 0;
	g_object_set(item, "name", "Kim", "secondary", NULL, NULL);
	g_assert_cmpint(notifies, ==, 1);
	g_object_set(item, "name", "Kim", "secondary", NULL, NULL);
	g_assert_cmpint(notifies, ==, 1);
	g_assert_cmpstr(pidgin_blist_node_item_get_name(item), ==, "Kim");

	icon1 = g_themed_icon_new("pidgin-status-available");
	icon2 = g_themed_icon_new("pidgin-status-available");
	g_object_set(item, "status-icon", icon1, NULL);
	g_assert_cmpint(notifies, ==, 2);
	g_object_set(item, "status-icon", icon2, NULL);  /* equal icon */
	g_assert_cmpint(notifies, ==, 2);
	g_object_set(item, "status-icon", NULL, NULL);
	g_assert_cmpint(notifies, ==, 3);
	g_object_unref(icon1);
	g_object_unref(icon2);

	/* Expanded state is stored, not interpreted. */
	pidgin_blist_node_item_set_expanded(item, TRUE);
	g_assert_true(pidgin_blist_node_item_get_expanded(item));

	g_signal_handlers_disconnect_by_func(item, notify_cb, NULL);
	g_signal_handlers_disconnect_by_func(model, inserted_cb, NULL);
	g_signal_handlers_disconnect_by_func(model, refresh_cb, NULL);
}

/* Every test runs on an empty buddy list and a fresh model. */
static void
run_test(gconstpointer data)
{
	void (*func)(void) = data;

	setup();
	func();
	teardown();
}

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
	char *dir;
	int ret;

	g_test_init(&argc, &argv, NULL);
	g_test_log_set_fatal_handler(fatal_filter, NULL);

	dir = g_dir_make_tmp("pidgin4-test-blistmodel-XXXXXX", NULL);
	g_assert_nonnull(dir);
	purple_util_set_user_dir(dir);
	purple_debug_set_enabled(FALSE);
	purple_eventloop_set_ui_ops(pidgin_eventloop_get_ui_ops());
	g_assert_true(purple_core_init("pidgin4-test"));

	purple_set_blist(purple_blist_new());
	purple_blist_set_ui_ops(&blist_ops);

	account = purple_account_new("me@example.com", "prpl-jabber");
	purple_accounts_add(account);

	g_test_add_data_func("/blistmodel/sort-functions", test_sort_functions, run_test);
	g_test_add_data_func("/blistmodel/order", test_order_and_resort, run_test);
	g_test_add_data_func("/blistmodel/visibility", test_visibility, run_test);
	g_test_add_data_func("/blistmodel/expandable-contacts",
	                     test_expandable_contacts, run_test);
	g_test_add_data_func("/blistmodel/signals", test_signals_and_properties,
	                     run_test);

	ret = g_test_run();

	/* No purple_core_quit(): it would save into the scratch directory. */
	purple_blist_set_ui_ops(NULL);
	g_free(dir);
	return ret;
}
