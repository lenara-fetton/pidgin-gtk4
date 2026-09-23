/*
 * Unit tests for pidginmenu.c: PurpleMenuAction / PurplePluginAction trees
 * to GMenuModel + GSimpleActionGroup.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "pidgin-internal.h"

#include <gio/gio.h>

#include "pidginmenu.h"

static int calls;
static gpointer last_object;
static gpointer last_data;

static void
record_cb(gpointer object, gpointer data)
{
	calls++;
	last_object = object;
	last_data = data;
}

/* Returns the label of item @i of @model, or NULL. Free with g_free(). */
static char *
item_label(GMenuModel *model, int i)
{
	char *label = NULL;

	g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_LABEL, "s", &label);
	return label;
}

static char *
item_action(GMenuModel *model, int i)
{
	char *action = NULL;

	g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_ACTION, "s", &action);
	return action;
}

/* Activates the "prefix.name" action of an item in @group. */
static void
activate_item(GSimpleActionGroup *group, GMenuModel *model, int i)
{
	char *detailed = item_action(model, i);
	const char *dot;

	g_assert_nonnull(detailed);
	g_assert_true(g_str_has_prefix(detailed, "test."));
	dot = strchr(detailed, '.');
	g_action_group_activate_action(G_ACTION_GROUP(group), dot + 1, NULL);
	g_free(detailed);
}

static gboolean
item_enabled(GSimpleActionGroup *group, GMenuModel *model, int i)
{
	char *detailed = item_action(model, i);
	gboolean enabled;

	enabled = g_action_group_get_action_enabled(G_ACTION_GROUP(group),
	                                            strchr(detailed, '.') + 1);
	g_free(detailed);
	return enabled;
}

/*
 * Menu:
 *   section 0: "One" (cb, data 1), "Disabled" (no callback)
 *   section 1: "Sub" -> [ "Two" (cb, data 2), NULL, "Deeper" -> [ "Three" ] ]
 */
static void
test_menu_actions(void)
{
	GSimpleActionGroup *group = g_simple_action_group_new();
	GList *list = NULL, *sub = NULL, *deeper = NULL;
	GMenu *menu;
	GMenuModel *model, *section, *submenu, *subsection, *deepmenu;
	int object;
	char *s;

	deeper = g_list_append(deeper, purple_menu_action_new("Three",
	                       PURPLE_CALLBACK(record_cb), GINT_TO_POINTER(3), NULL));
	sub = g_list_append(sub, purple_menu_action_new("Two",
	                    PURPLE_CALLBACK(record_cb), GINT_TO_POINTER(2), NULL));
	sub = g_list_append(sub, NULL);
	sub = g_list_append(sub, purple_menu_action_new("_Deeper", NULL, NULL, deeper));

	list = g_list_append(list, purple_menu_action_new("_One",
	                     PURPLE_CALLBACK(record_cb), GINT_TO_POINTER(1), NULL));
	list = g_list_append(list, purple_menu_action_new("Disabled", NULL, NULL, NULL));
	list = g_list_append(list, NULL);
	list = g_list_append(list, purple_menu_action_new("Sub", NULL, NULL, sub));

	menu = pidgin_menu_from_menu_actions(list, &object, group, "test");
	model = G_MENU_MODEL(menu);

	/* Two sections, split at the separator. */
	g_assert_cmpint(g_menu_model_get_n_items(model), ==, 2);

	section = g_menu_model_get_item_link(model, 0, G_MENU_LINK_SECTION);
	g_assert_nonnull(section);
	g_assert_cmpint(g_menu_model_get_n_items(section), ==, 2);
	s = item_label(section, 0);
	g_assert_cmpstr(s, ==, "_One");      /* mnemonic kept */
	g_free(s);
	s = item_label(section, 1);
	g_assert_cmpstr(s, ==, "Disabled");
	g_free(s);
	g_assert_true(item_enabled(group, section, 0));
	g_assert_false(item_enabled(group, section, 1));

	calls = 0;
	activate_item(group, section, 0);
	g_assert_cmpint(calls, ==, 1);
	g_assert_true(last_object == &object);
	g_assert_cmpint(GPOINTER_TO_INT(last_data), ==, 1);

	/* The submenu, itself with two sections, one holding a submenu. */
	subsection = g_menu_model_get_item_link(model, 1, G_MENU_LINK_SECTION);
	g_assert_cmpint(g_menu_model_get_n_items(subsection), ==, 1);
	s = item_label(subsection, 0);
	g_assert_cmpstr(s, ==, "Sub");
	g_free(s);
	submenu = g_menu_model_get_item_link(subsection, 0, G_MENU_LINK_SUBMENU);
	g_assert_nonnull(submenu);
	g_assert_cmpint(g_menu_model_get_n_items(submenu), ==, 2);

	{
		GMenuModel *s0 = g_menu_model_get_item_link(submenu, 0, G_MENU_LINK_SECTION);
		GMenuModel *s1 = g_menu_model_get_item_link(submenu, 1, G_MENU_LINK_SECTION);

		activate_item(group, s0, 0);
		g_assert_cmpint(calls, ==, 2);
		g_assert_cmpint(GPOINTER_TO_INT(last_data), ==, 2);

		deepmenu = g_menu_model_get_item_link(s1, 0, G_MENU_LINK_SUBMENU);
		g_assert_nonnull(deepmenu);
		{
			GMenuModel *d0 = g_menu_model_get_item_link(deepmenu, 0, G_MENU_LINK_SECTION);

			s = item_label(d0, 0);
			g_assert_cmpstr(s, ==, "Three");
			g_free(s);
			activate_item(group, d0, 0);
			g_assert_cmpint(calls, ==, 3);
			g_assert_cmpint(GPOINTER_TO_INT(last_data), ==, 3);
			g_object_unref(d0);
		}
		g_object_unref(deepmenu);
		g_object_unref(s0);
		g_object_unref(s1);
	}

	/* 4 leaf actions, all with distinct names. */
	{
		char **names = g_action_group_list_actions(G_ACTION_GROUP(group));
		g_assert_cmpuint(g_strv_length(names), ==, 4);
		g_strfreev(names);
	}

	/* A second menu in the same group must not clash with the first. */
	{
		GList *more = g_list_append(NULL, purple_menu_action_new("Again",
		                            PURPLE_CALLBACK(record_cb), GINT_TO_POINTER(4), NULL));
		GMenu *menu2 = pidgin_menu_from_menu_actions(more, NULL, group, "test");
		GMenuModel *sec = g_menu_model_get_item_link(G_MENU_MODEL(menu2), 0, G_MENU_LINK_SECTION);
		char **names = g_action_group_list_actions(G_ACTION_GROUP(group));

		g_assert_cmpuint(g_strv_length(names), ==, 5);
		activate_item(group, sec, 0);
		g_assert_cmpint(GPOINTER_TO_INT(last_data), ==, 4);
		g_assert_null(last_object);
		g_strfreev(names);
		g_object_unref(sec);
		g_object_unref(menu2);
	}

	g_object_unref(submenu);
	g_object_unref(subsection);
	g_object_unref(section);
	g_object_unref(menu);
	g_object_unref(group);
}

static void
test_empty(void)
{
	GSimpleActionGroup *group = g_simple_action_group_new();
	GMenu *menu = pidgin_menu_from_menu_actions(NULL, NULL, group, "test");
	GMenu *outer = g_menu_new();

	g_assert_cmpint(g_menu_model_get_n_items(G_MENU_MODEL(menu)), ==, 0);

	/* Leading/trailing/double separators produce no empty sections. */
	{
		GList *list = NULL;
		GMenu *m;

		list = g_list_append(list, NULL);
		list = g_list_append(list, purple_menu_action_new("A", NULL, NULL, NULL));
		list = g_list_append(list, NULL);
		list = g_list_append(list, NULL);
		list = g_list_append(list, purple_menu_action_new("B", NULL, NULL, NULL));
		list = g_list_append(list, NULL);
		m = pidgin_menu_from_menu_actions(list, NULL, group, "test");
		g_assert_cmpint(g_menu_model_get_n_items(G_MENU_MODEL(m)), ==, 2);
		g_object_unref(m);
	}

	pidgin_menu_append_menu_actions(outer, NULL, NULL, group, "test");
	g_assert_cmpint(g_menu_model_get_n_items(G_MENU_MODEL(outer)), ==, 0);

	g_object_unref(outer);
	g_object_unref(menu);
	g_object_unref(group);
}

/* PurplePluginAction lists. */
static PurplePluginAction *seen_action;

static void
plugin_action_cb(PurplePluginAction *action)
{
	seen_action = action;
}

static GList *
fake_actions(PurplePlugin *plugin, gpointer context)
{
	GList *l = NULL;
	PurplePluginAction *act;

	act = purple_plugin_action_new("Set _Mood", plugin_action_cb);
	act->user_data = GINT_TO_POINTER(42);
	l = g_list_append(l, act);
	l = g_list_append(l, NULL);
	l = g_list_append(l, purple_plugin_action_new("Other", plugin_action_cb));
	return l;
}

static void
test_plugin_actions(void)
{
	PurplePluginInfo info = { 0 };
	PurplePlugin plugin = { 0 };
	GSimpleActionGroup *group = g_simple_action_group_new();
	GMenuModel *model, *section;
	GMenu *menu;
	int context;
	char *s;

	info.actions = fake_actions;
	plugin.info = &info;

	menu = pidgin_menu_from_plugin_actions(&plugin, &context, group, "test");
	g_assert_nonnull(menu);
	model = G_MENU_MODEL(menu);
	g_assert_cmpint(g_menu_model_get_n_items(model), ==, 2);

	section = g_menu_model_get_item_link(model, 0, G_MENU_LINK_SECTION);
	s = item_label(section, 0);
	g_assert_cmpstr(s, ==, "Set __Mood");   /* literal underscore */
	g_free(s);

	seen_action = NULL;
	activate_item(group, section, 0);
	g_assert_nonnull(seen_action);
	g_assert_true(seen_action->plugin == &plugin);
	g_assert_true(seen_action->context == &context);
	g_assert_cmpint(GPOINTER_TO_INT(seen_action->user_data), ==, 42);

	g_object_unref(section);
	g_object_unref(menu);
	/* Frees the PurplePluginActions with their GActions. */
	g_object_unref(group);

	/* No actions callback: no menu. */
	info.actions = NULL;
	group = g_simple_action_group_new();
	g_assert_null(pidgin_menu_from_plugin_actions(&plugin, NULL, group, "test"));
	g_object_unref(group);
}

static void
test_escape(void)
{
	char *s = pidgin_menu_escape_label("a_b__c");

	g_assert_cmpstr(s, ==, "a__b____c");
	g_free(s);
	s = pidgin_menu_escape_label(NULL);
	g_assert_cmpstr(s, ==, "");
	g_free(s);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/pidginmenu/menu-actions", test_menu_actions);
	g_test_add_func("/pidginmenu/empty-and-separators", test_empty);
	g_test_add_func("/pidginmenu/plugin-actions", test_plugin_actions);
	g_test_add_func("/pidginmenu/escape", test_escape);

	return g_test_run();
}
