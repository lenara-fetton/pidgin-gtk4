/**
 * @file pidginblistmodel.h The buddy list as GListModels
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
 * The model behind the buddy list window (gtkblist.c), kept apart from the
 * widgets so it can be unit tested.
 *
 * Every PurpleBlistNode the UI has seen gets a PidginBlistNodeItem, found
 * through a hash table (pidgin_blist_model_lookup()). The visible ones sit
 * in GListStores that mirror the GTK 2 tree:
 *
 *   root store:      groups, in buddy list order
 *   group children:  contacts and chats, ordered by the sort function
 *   contact children: its buddies, in buddy list order (only for contacts
 *                     with more than one buddy, i.e. expandable ones)
 *
 * A GtkTreeListModel over the root store, with
 * pidgin_blist_node_item_get_children() as its create function, gives the
 * tree. Hidden nodes (offline buddies, empty groups, ...) are simply not in
 * their parent's store, as with pidgin_blist_hide_node() in Pidgin 2.
 *
 * pidgin_blist_model_update() and _remove() implement the placement half of
 * the PurpleBlistUiOps update/remove ops; the display half (names, icons)
 * belongs to the UI, which is asked through the "item-refresh" signal.
 */
#ifndef _PIDGINBLISTMODEL_H_
#define _PIDGINBLISTMODEL_H_

#include <gtk/gtk.h>

#include "blist.h"

G_BEGIN_DECLS

/**************************************************************************
 * PidginBlistNodeItem
 **************************************************************************/

#define PIDGIN_TYPE_BLIST_NODE_ITEM (pidgin_blist_node_item_get_type())
G_DECLARE_FINAL_TYPE(PidginBlistNodeItem, pidgin_blist_node_item,
                     PIDGIN, BLIST_NODE_ITEM, GObject)

/*
 * Display properties, set by the UI (all notify only when they change):
 *   "name"           string, Pango markup: the first line of the row
 *   "secondary"      string, Pango markup or NULL: status text / idle
 *   "idle"           string or NULL: the short idle time ("1:05")
 *   "status-icon"    GIcon or NULL
 *   "emblem"         GIcon or NULL
 *   "protocol-icon"  GIcon or NULL (NULL hides it)
 *   "buddy-icon"     GdkPaintable or NULL
 *   "style"          string: CSS class of the row (pidgin-blist-online, ...)
 *   "tooltip-key"    int, bumped whenever the tooltip content may change
 */

/** The node. Never NULL; the item does not own it. */
PurpleBlistNode *pidgin_blist_node_item_get_node(PidginBlistNodeItem *item);

/**
 * The children store of a group (contacts and chats) or of an expandable
 * contact (buddies) as a new reference, or NULL for buddies, chats and
 * contacts that have a single buddy. Suitable as a GtkTreeListModel create
 * function (it takes a gpointer item).
 */
GListModel *pidgin_blist_node_item_get_children(gpointer item);

/** Whether the node is currently shown (it is in its parent's store). */
gboolean pidgin_blist_node_item_get_visible(PidginBlistNodeItem *item);

/** Contacts: whether the contact can be expanded into its buddies. */
gboolean pidgin_blist_node_item_get_expandable(PidginBlistNodeItem *item);

/**
 * The row's expanded state as the UI last saw it (groups: not collapsed;
 * contacts: showing their buddies). The model only stores it.
 */
gboolean pidgin_blist_node_item_get_expanded(PidginBlistNodeItem *item);
void pidgin_blist_node_item_set_expanded(PidginBlistNodeItem *item,
                                         gboolean expanded);

/**
 * Buddies: TRUE for a few seconds after the buddy signed on or off
 * (Pidgin 2's recent_signonoff); such a buddy stays displayed even if
 * offline. The UI owns the timer; the model only reads the flag.
 */
gboolean pidgin_blist_node_item_get_recent_signonoff(PidginBlistNodeItem *item);
void pidgin_blist_node_item_set_recent_signonoff(PidginBlistNodeItem *item,
                                                 gboolean recent,
                                                 guint timer);

/** The current value of "name" (may be NULL before the first refresh). */
const char *pidgin_blist_node_item_get_name(PidginBlistNodeItem *item);
const char *pidgin_blist_node_item_get_style(PidginBlistNodeItem *item);

/**************************************************************************
 * Sorting
 **************************************************************************/

/**
 * Orders two contacts or chats of the same group. Must be a total order
 * (break ties by pointer), like Pidgin 2's sort methods.
 */
typedef int (*PidginBlistSortFunc)(PurpleBlistNode *a, PurpleBlistNode *b);

/** By contact alias / chat name, case-insensitively. */
int pidgin_blist_sort_alphabetical(PurpleBlistNode *a, PurpleBlistNode *b);

/** Contacts by presence (purple_presence_compare()) then name; chats last. */
int pidgin_blist_sort_status(PurpleBlistNode *a, PurpleBlistNode *b);

/** Contacts by log activity (most active first) then name; chats last. */
int pidgin_blist_sort_log_activity(PurpleBlistNode *a, PurpleBlistNode *b);

/**************************************************************************
 * PidginBlistModel
 **************************************************************************/

typedef enum
{
	/** /pidgin/blist/show_offline_buddies */
	PIDGIN_BLIST_SHOW_OFFLINE          = 1 << 0,
	/** /pidgin/blist/show_empty_groups */
	PIDGIN_BLIST_SHOW_EMPTY_GROUPS     = 1 << 1,
	/**
	 * /pidgin4/blist/show_disconnected_accounts: also show the buddies
	 * and chats of accounts that are not connected. Pidgin 2 never does.
	 */
	PIDGIN_BLIST_SHOW_DISCONNECTED     = 1 << 2,
} PidginBlistShowFlags;

#define PIDGIN_TYPE_BLIST_MODEL (pidgin_blist_model_get_type())
G_DECLARE_FINAL_TYPE(PidginBlistModel, pidgin_blist_model,
                     PIDGIN, BLIST_MODEL, GObject)

/*
 * Signals:
 *   "item-refresh"  (PidginBlistNodeItem *item): recompute the display
 *                   properties of @item (emitted by every update of it).
 *   "item-inserted" (PidginBlistNodeItem *item): @item was (re)inserted
 *                   into its parent's store, e.g. so the UI can restore
 *                   the row's expanded state.
 */

PidginBlistModel *pidgin_blist_model_new(void);

/** The root store (groups). Not a new reference. */
GListModel *pidgin_blist_model_get_root(PidginBlistModel *model);

/** The item of @node, or NULL if the model has not seen it yet. */
PidginBlistNodeItem *pidgin_blist_model_lookup(PidginBlistModel *model,
                                               PurpleBlistNode *node);

/** The item of @node, created if needed. */
PidginBlistNodeItem *pidgin_blist_model_ensure(PidginBlistModel *model,
                                               PurpleBlistNode *node);

/**
 * The update UI op: (re)places @node and the nodes whose visibility
 * depends on it (its contact and group), then asks for a refresh.
 */
void pidgin_blist_model_update(PidginBlistModel *model, PurpleBlistNode *node);

/**
 * The remove UI op: @node is being removed from the list (or moved; it
 * comes back with an update). Its item is dropped.
 */
void pidgin_blist_model_remove(PidginBlistModel *model, PurpleBlistNode *node);

/** Updates every node of the buddy list (after a filter or sort change). */
void pidgin_blist_model_update_all(PidginBlistModel *model);

/** Drops every item (the buddy list is being destroyed). */
void pidgin_blist_model_clear(PidginBlistModel *model);

/** NULL means buddy list order ("none"). Call update_all() afterwards. */
void pidgin_blist_model_set_sort_func(PidginBlistModel *model,
                                      PidginBlistSortFunc func);
PidginBlistSortFunc pidgin_blist_model_get_sort_func(PidginBlistModel *model);

/** Call update_all() afterwards. */
void pidgin_blist_model_set_show_flags(PidginBlistModel *model,
                                       PidginBlistShowFlags flags);
PidginBlistShowFlags pidgin_blist_model_get_show_flags(PidginBlistModel *model);

/** Pidgin 2's buddy_is_displayable() with the model's flags. */
gboolean pidgin_blist_model_buddy_is_displayable(PidginBlistModel *model,
                                                 PurpleBuddy *buddy);

/**
 * Counts the visible items of @type (a PurpleBlistNodeType, or -1 for all)
 * by walking the stores. For tests and the selftest.
 */
guint pidgin_blist_model_count_visible(PidginBlistModel *model, int type);

/**
 * Checks that every store is ordered as the model promises and that the
 * items in it are exactly the visible ones. Returns TRUE if so; otherwise
 * logs what is wrong. For tests and the selftest.
 */
gboolean pidgin_blist_model_check(PidginBlistModel *model);

G_END_DECLS

#endif /* _PIDGINBLISTMODEL_H_ */
