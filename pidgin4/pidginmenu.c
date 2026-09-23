/*
 * pidgin
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

#include "pidginmenu.h"

#define NEXT_ID_KEY "pidgin-menu-next-id"

typedef struct {
	void (*callback)(gpointer object, gpointer data);
	gpointer object;
	gpointer data;
} MenuActionClosure;

/* Returns a new action name that is unique within @group. */
static char *
new_action_name(GSimpleActionGroup *group, const char *kind)
{
	guint id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(group), NEXT_ID_KEY));

	g_object_set_data(G_OBJECT(group), NEXT_ID_KEY, GUINT_TO_POINTER(id + 1));
	return g_strdup_printf("%s-%u", kind, id);
}

static char *
detailed_name(const char *prefix, const char *name)
{
	if (prefix == NULL || *prefix == '\0')
		return g_strdup(name);
	return g_strdup_printf("%s.%s", prefix, name);
}

char *
pidgin_menu_escape_label(const char *label)
{
	GString *str;
	const char *p;

	if (label == NULL)
		return g_strdup("");

	str = g_string_sized_new(strlen(label) + 4);
	for (p = label; *p; p++) {
		if (*p == '_')
			g_string_append_c(str, '_');
		g_string_append_c(str, *p);
	}
	return g_string_free(str, FALSE);
}

/**************************************************************************
 * PurpleMenuAction
 **************************************************************************/

static void
menu_action_activate_cb(GSimpleAction *action, GVariant *param, gpointer user_data)
{
	MenuActionClosure *closure = user_data;

	if (closure->callback != NULL)
		closure->callback(closure->object, closure->data);
}

/* Frees a PurpleMenuAction tree whose children were not consumed. */
static void
free_menu_action_tree(PurpleMenuAction *act)
{
	GList *l;

	if (act == NULL)
		return;

	for (l = act->children; l != NULL; l = l->next)
		free_menu_action_tree(l->data);
	g_list_free(act->children);
	act->children = NULL;
	purple_menu_action_free(act);
}

static void
build_menu_actions(GMenu *menu, GList *actions, gpointer object,
                   GSimpleActionGroup *group, const char *prefix)
{
	GMenu *section = NULL;
	GList *l;

	for (l = actions; l != NULL; l = l->next) {
		PurpleMenuAction *act = l->data;
		GMenuItem *item;

		if (act == NULL) {
			/* A separator: close the current section. */
			if (section != NULL) {
				g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
				g_clear_object(&section);
			}
			continue;
		}

		if (section == NULL)
			section = g_menu_new();

		if (act->children != NULL) {
			GMenu *submenu = g_menu_new();

			build_menu_actions(submenu, act->children, object, group, prefix);
			g_list_free(act->children);
			act->children = NULL;

			item = g_menu_item_new_submenu(act->label, G_MENU_MODEL(submenu));
			g_object_unref(submenu);
		} else {
			GSimpleAction *action;
			char *name, *detailed;

			name = new_action_name(group, "action");
			action = g_simple_action_new(name, NULL);

			if (act->callback != NULL) {
				MenuActionClosure *closure = g_new0(MenuActionClosure, 1);

				closure->callback = (void (*)(gpointer, gpointer))act->callback;
				closure->object = object;
				closure->data = act->data;
				g_object_set_data_full(G_OBJECT(action), "pidgin-closure",
				                       closure, g_free);
				g_signal_connect(action, "activate",
				                 G_CALLBACK(menu_action_activate_cb), closure);
			} else {
				g_simple_action_set_enabled(action, FALSE);
			}
			g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
			g_object_unref(action);

			detailed = detailed_name(prefix, name);
			item = g_menu_item_new(act->label, detailed);
			g_free(detailed);
			g_free(name);
		}

		g_menu_append_item(section, item);
		g_object_unref(item);
		purple_menu_action_free(act);
	}

	if (section != NULL) {
		g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
		g_object_unref(section);
	}
}

GMenu *
pidgin_menu_from_menu_actions(GList *actions, gpointer object,
                              GSimpleActionGroup *group, const char *prefix)
{
	GMenu *menu = g_menu_new();

	if (group == NULL) {
		g_critical("pidgin_menu_from_menu_actions: group is NULL");
		g_list_free_full(actions, (GDestroyNotify)free_menu_action_tree);
		return menu;
	}

	build_menu_actions(menu, actions, object, group, prefix);
	g_list_free(actions);

	return menu;
}

void
pidgin_menu_append_menu_actions(GMenu *menu, GList *actions, gpointer object,
                                GSimpleActionGroup *group, const char *prefix)
{
	GMenu *sub;

	g_return_if_fail(G_IS_MENU(menu));

	sub = pidgin_menu_from_menu_actions(actions, object, group, prefix);
	if (g_menu_model_get_n_items(G_MENU_MODEL(sub)) > 0)
		g_menu_append_section(menu, NULL, G_MENU_MODEL(sub));
	g_object_unref(sub);
}

/**************************************************************************
 * PurplePluginAction
 **************************************************************************/

static void
plugin_action_activate_cb(GSimpleAction *action, GVariant *param, gpointer user_data)
{
	PurplePluginAction *pam = user_data;

	if (pam != NULL && pam->callback != NULL)
		pam->callback(pam);
}

GMenu *
pidgin_menu_from_plugin_actions(PurplePlugin *plugin, gpointer context,
                                GSimpleActionGroup *group, const char *prefix)
{
	GMenu *menu, *section = NULL;
	GList *actions, *l;

	g_return_val_if_fail(plugin != NULL, NULL);
	g_return_val_if_fail(group != NULL, NULL);

	if (!PURPLE_PLUGIN_HAS_ACTIONS(plugin))
		return NULL;

	actions = PURPLE_PLUGIN_ACTIONS(plugin, context);
	if (actions == NULL)
		return NULL;

	menu = g_menu_new();
	for (l = actions; l != NULL; l = l->next) {
		PurplePluginAction *pam = l->data;
		GSimpleAction *action;
		char *name, *detailed, *label;

		if (pam == NULL) {
			if (section != NULL) {
				g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
				g_clear_object(&section);
			}
			continue;
		}

		pam->plugin = plugin;
		pam->context = context;

		name = new_action_name(group, "plugin-action");
		action = g_simple_action_new(name, NULL);
		g_object_set_data_full(G_OBJECT(action), "pidgin-plugin-action", pam,
		                       (GDestroyNotify)purple_plugin_action_free);
		g_signal_connect(action, "activate",
		                 G_CALLBACK(plugin_action_activate_cb), pam);
		g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
		g_object_unref(action);

		if (section == NULL)
			section = g_menu_new();
		detailed = detailed_name(prefix, name);
		label = pidgin_menu_escape_label(pam->label);
		g_menu_append(section, label, detailed);
		g_free(label);
		g_free(detailed);
		g_free(name);
	}
	g_list_free(actions);

	if (section != NULL) {
		g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
		g_object_unref(section);
	}

	return menu;
}
