/**
 * @file gtkblist.h GTK 4 Buddy List API
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
#ifndef _PIDGINBLIST_H_
#define _PIDGINBLIST_H_

#include "pidgin.h"

#include "blist.h"

#include "pidginblistmodel.h"

/*
 * The pidgin4 buddy list: the application's main window.
 *
 * Signals on pidgin_blist_get_handle(), as in Pidgin 2 (same names and
 * signatures, so plugins such as cap keep working):
 *   "gtkblist-created"   (PurpleBuddyList *)
 *   "gtkblist-hiding"    (PurpleBuddyList *)
 *   "gtkblist-unhiding"  (PurpleBuddyList *)
 *   "drawing-tooltip"    (PurpleBlistNode *, GString *text, gboolean full):
 *                        append Pango-markup lines to the tooltip text
 *   "drawing-buddy"      (PurpleBuddy *) -> char *: an escaped name
 *
 * Prefs: the shared /pidgin/blist/{show_buddy_icons, show_empty_groups,
 * show_idle_time, show_offline_buddies, show_protocol_icons, sort_type}
 * (same meaning as in Pidgin 2) and pidgin4's own /pidgin4/blist/...
 * (window size, close_hides, show_disconnected_accounts).
 */

/** Registers the prefs and signals. Call before the core loads the list. */
void pidgin_blist_init(void);
void pidgin_blist_uninit(void);

void *pidgin_blist_get_handle(void);
PurpleBlistUiOps *pidgin_blist_get_ui_ops(void);

/** The buddy list window, or NULL before purple_blist_show(). */
GtkWidget *pidgin_blist_get_window(void);

/** The model behind the list, or NULL before purple_blist_show(). */
PidginBlistModel *pidgin_blist_get_model(void);

/**
 * Adds @widget (usually a PidginMiniDialog) to the alert area at the top of
 * the buddy list. It stays until it removes itself (or is destroyed).
 */
void pidgin_blist_add_alert(GtkWidget *widget);

/** Shows or hides (or minimizes) the buddy list window. */
void pidgin_blist_toggle_visibility(void);

/** Re-reads everything (after a pref change). */
void pidgin_blist_refresh(PurpleBuddyList *list);

/** The Join Chat dialog. */
void pidgin_blist_joinchat_show(void);
gboolean pidgin_blist_joinchat_is_showable(void);

/**
 * Pango markup tooltip text of a node, with the "drawing-tooltip" lines
 * (@full: the long form, as Pidgin 2).
 */
char *pidgin_blist_get_tooltip_text(PurpleBlistNode *node, gboolean full);

/**
 * Builds the context menu of @node into @menu, with the actions in @group
 * (to be inserted as "node"). Public for the selftest.
 */
void pidgin_blist_build_node_menu(PurpleBlistNode *node, GMenu *menu,
                                  GSimpleActionGroup *group);

/** The tooltip widget of @node (floating), as a row shows it. For the
 * selftests. */
GtkWidget *pidgin_blist_tooltip_widget_new(PurpleBlistNode *node);

/**************************************************************************
 * Sort methods (Pidgin 2's API with a comparator instead of a
 * GtkTreeIter inserter)
 **************************************************************************/

typedef struct
{
	char *id;
	char *name;
	/* NULL: buddy list order */
	PidginBlistSortFunc func;
} PidginBlistSortMethod;

void pidgin_blist_sort_method_reg(const char *id, const char *name,
                                  PidginBlistSortFunc func);
void pidgin_blist_sort_method_unreg(const char *id);
void pidgin_blist_sort_method_set(const char *id);
GList *pidgin_blist_get_sort_methods(void);

/**
 * Test hook. If PIDGIN4_BLIST_SELFTEST is set, expands and collapses every
 * group, builds every context menu model, toggles the Show options (and
 * restores them), checks the model and logs counts. Changes no saved data
 * except transiently; signs nothing in.
 */
void pidgin_blist_selftest(void);

#endif /* _PIDGINBLIST_H_ */
