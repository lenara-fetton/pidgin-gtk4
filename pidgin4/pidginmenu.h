/**
 * @file pidginmenu.h PurpleMenuAction/PurplePluginAction to GMenuModel
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
 * Replaces pidgin_append_menu_action() (pidgin/gtkutils.c) and the blist's
 * build_plugin_actions(). libpurple hands out menus as trees of
 * PurpleMenuAction (blist node, conversation and account extended menus)
 * or lists of PurplePluginAction (protocol and plugin "Actions" menus).
 * These functions turn them into a GMenu plus GActions in a
 * GSimpleActionGroup, for a GtkPopoverMenu or GtkMenuButton:
 *
 *   GSimpleActionGroup *group = g_simple_action_group_new();
 *   GMenu *menu = pidgin_menu_from_menu_actions(list, node, group, "node");
 *   gtk_widget_insert_action_group(button, "node", G_ACTION_GROUP(group));
 *   gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(button),
 *                                  G_MENU_MODEL(menu));
 *
 * This file uses GIO only (no GTK), so it can be unit tested headless.
 */
#ifndef _PIDGINMENU_H_
#define _PIDGINMENU_H_

#include <gio/gio.h>

#include "plugin.h"
#include "util.h"

/**
 * Builds a GMenu from @actions, a GList of PurpleMenuAction where a NULL
 * element is a separator (it starts a new section). Actions with children
 * become submenus; actions without a callback and without children are
 * shown disabled. Activating an item calls
 * callback(@object, action->data), like pidgin_append_menu_action() did.
 *
 * The GActions are added to @group with unique names; the menu refers to
 * them as "<@prefix>.<name>", so insert @group into the widget tree under
 * @prefix. Labels are used as mnemonics, as in Pidgin 2.
 *
 * Takes ownership of @actions: the list and every PurpleMenuAction in it are
 * freed (purple_menu_action_free()), as pidgin_append_menu_action() did.
 *
 * @return A new GMenu (never NULL; empty when @actions is NULL).
 */
GMenu *pidgin_menu_from_menu_actions(GList *actions, gpointer object,
                                     GSimpleActionGroup *group,
                                     const char *prefix);

/**
 * Appends the result of pidgin_menu_from_menu_actions() to @menu as a new
 * section. Takes ownership of @actions.
 */
void pidgin_menu_append_menu_actions(GMenu *menu, GList *actions,
                                     gpointer object,
                                     GSimpleActionGroup *group,
                                     const char *prefix);

/**
 * Builds a GMenu from PURPLE_PLUGIN_ACTIONS(@plugin, @context): the
 * protocol "Actions" of an account's connection (@context is the
 * PurpleConnection) or a plugin's actions (@context NULL). NULL entries are
 * separators. Each PurplePluginAction gets its plugin and context set and
 * lives as long as its GAction. Labels are literal (no mnemonics).
 *
 * @return A new GMenu, or NULL if the plugin has no actions.
 */
GMenu *pidgin_menu_from_plugin_actions(PurplePlugin *plugin, gpointer context,
                                       GSimpleActionGroup *group,
                                       const char *prefix);

/**
 * Escapes '_' in @label so GTK shows it literally instead of as a
 * mnemonic. Returns a new string.
 */
char *pidgin_menu_escape_label(const char *label);

#endif /* _PIDGINMENU_H_ */
