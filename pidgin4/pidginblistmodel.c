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

/*
 * The placement logic follows pidgin/gtkblist.c: pidgin_blist_update_group,
 * _contact, _buddy and _chat, insert_node() and the sort_method_*()
 * functions, with GListStores instead of a GtkTreeStore.
 */
#include "pidgin-internal.h"

#include "account.h"
#include "debug.h"
#include "log.h"
#include "status.h"
#include "util.h"

#include "pidginblistmodel.h"

/**************************************************************************
 * PidginBlistNodeItem
 **************************************************************************/

struct _PidginBlistNodeItem
{
	GObject parent;

	PurpleBlistNode *node;

	/* Groups and contacts: the store of their visible children. */
	GListStore *children;
	/* The store this item is in (a reference), or NULL when hidden. */
	GListStore *container;

	gboolean expandable;
	gboolean expanded;
	gboolean recent_signonoff;
	guint signonoff_timer;

	char *name;
	char *secondary;
	char *idle;
	char *style;
	GIcon *status_icon;
	GIcon *emblem;
	GIcon *protocol_icon;
	GdkPaintable *buddy_icon;
	GdkPaintable *game_icon;
};

enum {
	ITEM_PROP_0,
	ITEM_PROP_NAME,
	ITEM_PROP_SECONDARY,
	ITEM_PROP_IDLE,
	ITEM_PROP_STYLE,
	ITEM_PROP_STATUS_ICON,
	ITEM_PROP_EMBLEM,
	ITEM_PROP_PROTOCOL_ICON,
	ITEM_PROP_BUDDY_ICON,
	ITEM_PROP_GAME_ICON,
	ITEM_N_PROPS
};

static GParamSpec *item_props[ITEM_N_PROPS];

G_DEFINE_FINAL_TYPE(PidginBlistNodeItem, pidgin_blist_node_item, G_TYPE_OBJECT)

static void
item_set_string(PidginBlistNodeItem *item, char **field, const char *value,
                guint prop)
{
	if (g_strcmp0(*field, value) == 0)
		return;
	g_free(*field);
	*field = g_strdup(value);
	g_object_notify_by_pspec(G_OBJECT(item), item_props[prop]);
}

static void
item_set_icon(PidginBlistNodeItem *item, GIcon **field, GIcon *value,
              guint prop)
{
	if (*field == value || (*field != NULL && value != NULL &&
	                        g_icon_equal(*field, value)))
		return;
	g_set_object(field, value);
	g_object_notify_by_pspec(G_OBJECT(item), item_props[prop]);
}

static void
pidgin_blist_node_item_get_property(GObject *obj, guint prop_id,
                                    GValue *value, GParamSpec *pspec)
{
	PidginBlistNodeItem *item = PIDGIN_BLIST_NODE_ITEM(obj);

	switch (prop_id) {
	case ITEM_PROP_NAME:
		g_value_set_string(value, item->name);
		break;
	case ITEM_PROP_SECONDARY:
		g_value_set_string(value, item->secondary);
		break;
	case ITEM_PROP_IDLE:
		g_value_set_string(value, item->idle);
		break;
	case ITEM_PROP_STYLE:
		g_value_set_string(value, item->style);
		break;
	case ITEM_PROP_STATUS_ICON:
		g_value_set_object(value, item->status_icon);
		break;
	case ITEM_PROP_EMBLEM:
		g_value_set_object(value, item->emblem);
		break;
	case ITEM_PROP_PROTOCOL_ICON:
		g_value_set_object(value, item->protocol_icon);
		break;
	case ITEM_PROP_BUDDY_ICON:
		g_value_set_object(value, item->buddy_icon);
		break;
	case ITEM_PROP_GAME_ICON:
		g_value_set_object(value, item->game_icon);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
	}
}

static void
pidgin_blist_node_item_set_property(GObject *obj, guint prop_id,
                                    const GValue *value, GParamSpec *pspec)
{
	PidginBlistNodeItem *item = PIDGIN_BLIST_NODE_ITEM(obj);

	switch (prop_id) {
	case ITEM_PROP_NAME:
		item_set_string(item, &item->name, g_value_get_string(value), prop_id);
		break;
	case ITEM_PROP_SECONDARY:
		item_set_string(item, &item->secondary, g_value_get_string(value), prop_id);
		break;
	case ITEM_PROP_IDLE:
		item_set_string(item, &item->idle, g_value_get_string(value), prop_id);
		break;
	case ITEM_PROP_STYLE:
		item_set_string(item, &item->style, g_value_get_string(value), prop_id);
		break;
	case ITEM_PROP_STATUS_ICON:
		item_set_icon(item, &item->status_icon, g_value_get_object(value), prop_id);
		break;
	case ITEM_PROP_EMBLEM:
		item_set_icon(item, &item->emblem, g_value_get_object(value), prop_id);
		break;
	case ITEM_PROP_PROTOCOL_ICON:
		item_set_icon(item, &item->protocol_icon, g_value_get_object(value), prop_id);
		break;
	case ITEM_PROP_BUDDY_ICON:
		/* Textures are compared by identity only. */
		if (item->buddy_icon != g_value_get_object(value)) {
			g_set_object(&item->buddy_icon, g_value_get_object(value));
			g_object_notify_by_pspec(obj, pspec);
		}
		break;
	case ITEM_PROP_GAME_ICON:
		if (item->game_icon != g_value_get_object(value)) {
			g_set_object(&item->game_icon, g_value_get_object(value));
			g_object_notify_by_pspec(obj, pspec);
		}
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
	}
}

static void
pidgin_blist_node_item_dispose(GObject *obj)
{
	PidginBlistNodeItem *item = PIDGIN_BLIST_NODE_ITEM(obj);

	if (item->signonoff_timer != 0) {
		g_source_remove(item->signonoff_timer);
		item->signonoff_timer = 0;
	}
	g_clear_object(&item->children);
	g_clear_object(&item->container);
	g_clear_object(&item->status_icon);
	g_clear_object(&item->emblem);
	g_clear_object(&item->protocol_icon);
	g_clear_object(&item->buddy_icon);
	g_clear_object(&item->game_icon);

	G_OBJECT_CLASS(pidgin_blist_node_item_parent_class)->dispose(obj);
}

static void
pidgin_blist_node_item_finalize(GObject *obj)
{
	PidginBlistNodeItem *item = PIDGIN_BLIST_NODE_ITEM(obj);

	g_free(item->name);
	g_free(item->secondary);
	g_free(item->idle);
	g_free(item->style);

	G_OBJECT_CLASS(pidgin_blist_node_item_parent_class)->finalize(obj);
}

static void
pidgin_blist_node_item_class_init(PidginBlistNodeItemClass *klass)
{
	GObjectClass *obj_class = G_OBJECT_CLASS(klass);
	const GParamFlags flags = G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY |
	                          G_PARAM_STATIC_STRINGS;

	obj_class->get_property = pidgin_blist_node_item_get_property;
	obj_class->set_property = pidgin_blist_node_item_set_property;
	obj_class->dispose = pidgin_blist_node_item_dispose;
	obj_class->finalize = pidgin_blist_node_item_finalize;

	item_props[ITEM_PROP_NAME] = g_param_spec_string("name", NULL, NULL, NULL, flags);
	item_props[ITEM_PROP_SECONDARY] = g_param_spec_string("secondary", NULL, NULL, NULL, flags);
	item_props[ITEM_PROP_IDLE] = g_param_spec_string("idle", NULL, NULL, NULL, flags);
	item_props[ITEM_PROP_STYLE] = g_param_spec_string("style", NULL, NULL, NULL, flags);
	item_props[ITEM_PROP_STATUS_ICON] = g_param_spec_object("status-icon", NULL, NULL,
		G_TYPE_ICON, flags);
	item_props[ITEM_PROP_EMBLEM] = g_param_spec_object("emblem", NULL, NULL,
		G_TYPE_ICON, flags);
	item_props[ITEM_PROP_PROTOCOL_ICON] = g_param_spec_object("protocol-icon", NULL, NULL,
		G_TYPE_ICON, flags);
	item_props[ITEM_PROP_BUDDY_ICON] = g_param_spec_object("buddy-icon", NULL, NULL,
		GDK_TYPE_PAINTABLE, flags);
	item_props[ITEM_PROP_GAME_ICON] = g_param_spec_object("game-icon", NULL, NULL,
		GDK_TYPE_PAINTABLE, flags);

	g_object_class_install_properties(obj_class, ITEM_N_PROPS, item_props);
}

static void
pidgin_blist_node_item_init(PidginBlistNodeItem *item)
{
}

static PidginBlistNodeItem *
pidgin_blist_node_item_new(PurpleBlistNode *node)
{
	PidginBlistNodeItem *item = g_object_new(PIDGIN_TYPE_BLIST_NODE_ITEM, NULL);

	item->node = node;
	if (PURPLE_BLIST_NODE_IS_GROUP(node) || PURPLE_BLIST_NODE_IS_CONTACT(node))
		item->children = g_list_store_new(PIDGIN_TYPE_BLIST_NODE_ITEM);
	/* Groups can always be expanded. */
	item->expandable = PURPLE_BLIST_NODE_IS_GROUP(node);
	return item;
}

PurpleBlistNode *
pidgin_blist_node_item_get_node(PidginBlistNodeItem *item)
{
	g_return_val_if_fail(PIDGIN_IS_BLIST_NODE_ITEM(item), NULL);
	return item->node;
}

GListModel *
pidgin_blist_node_item_get_children(gpointer data)
{
	PidginBlistNodeItem *item = data;

	g_return_val_if_fail(PIDGIN_IS_BLIST_NODE_ITEM(item), NULL);

	if (item->children == NULL || !item->expandable)
		return NULL;
	return G_LIST_MODEL(g_object_ref(item->children));
}

gboolean
pidgin_blist_node_item_get_visible(PidginBlistNodeItem *item)
{
	g_return_val_if_fail(PIDGIN_IS_BLIST_NODE_ITEM(item), FALSE);
	return item->container != NULL;
}

gboolean
pidgin_blist_node_item_get_expandable(PidginBlistNodeItem *item)
{
	g_return_val_if_fail(PIDGIN_IS_BLIST_NODE_ITEM(item), FALSE);
	return item->expandable;
}

gboolean
pidgin_blist_node_item_get_expanded(PidginBlistNodeItem *item)
{
	g_return_val_if_fail(PIDGIN_IS_BLIST_NODE_ITEM(item), FALSE);
	return item->expanded;
}

void
pidgin_blist_node_item_set_expanded(PidginBlistNodeItem *item, gboolean expanded)
{
	g_return_if_fail(PIDGIN_IS_BLIST_NODE_ITEM(item));
	item->expanded = expanded;
}

gboolean
pidgin_blist_node_item_get_recent_signonoff(PidginBlistNodeItem *item)
{
	g_return_val_if_fail(PIDGIN_IS_BLIST_NODE_ITEM(item), FALSE);
	return item->recent_signonoff;
}

void
pidgin_blist_node_item_set_recent_signonoff(PidginBlistNodeItem *item,
                                            gboolean recent, guint timer)
{
	g_return_if_fail(PIDGIN_IS_BLIST_NODE_ITEM(item));

	if (item->signonoff_timer != 0 && item->signonoff_timer != timer)
		g_source_remove(item->signonoff_timer);
	item->signonoff_timer = timer;
	item->recent_signonoff = recent;
}

const char *
pidgin_blist_node_item_get_name(PidginBlistNodeItem *item)
{
	g_return_val_if_fail(PIDGIN_IS_BLIST_NODE_ITEM(item), NULL);
	return item->name;
}

const char *
pidgin_blist_node_item_get_style(PidginBlistNodeItem *item)
{
	g_return_val_if_fail(PIDGIN_IS_BLIST_NODE_ITEM(item), NULL);
	return item->style;
}

/**************************************************************************
 * Sort functions
 **************************************************************************/

static int
compare_pointers(gconstpointer a, gconstpointer b)
{
	return (a < b) ? -1 : (a > b) ? 1 : 0;
}

static const char *
sort_name(PurpleBlistNode *node)
{
	if (PURPLE_BLIST_NODE_IS_CONTACT(node))
		return purple_contact_get_alias((PurpleContact *)node);
	if (PURPLE_BLIST_NODE_IS_CHAT(node))
		return purple_chat_get_name((PurpleChat *)node);
	if (PURPLE_BLIST_NODE_IS_BUDDY(node))
		return purple_buddy_get_alias((PurpleBuddy *)node);
	if (PURPLE_BLIST_NODE_IS_GROUP(node))
		return purple_group_get_name((PurpleGroup *)node);
	return NULL;
}

/* Names first (a NULL name sorts last, as in Pidgin 2), then pointers. */
static int
compare_names(PurpleBlistNode *a, PurpleBlistNode *b)
{
	const char *na = sort_name(a), *nb = sort_name(b);
	int cmp;

	if (na == NULL || nb == NULL)
		cmp = (na == nb) ? 0 : (na == NULL ? 1 : -1);
	else
		cmp = purple_utf8_strcasecmp(na, nb);

	return cmp != 0 ? cmp : compare_pointers(a, b);
}

int
pidgin_blist_sort_alphabetical(PurpleBlistNode *a, PurpleBlistNode *b)
{
	return compare_names(a, b);
}

/* Pidgin 2 appends chats after the contacts for the status and log
 * sorts; here they are ordered by name among themselves. */
static gboolean
chats_last(PurpleBlistNode *a, PurpleBlistNode *b, int *result)
{
	gboolean ca = PURPLE_BLIST_NODE_IS_CHAT(a), cb = PURPLE_BLIST_NODE_IS_CHAT(b);

	if (ca && cb)
		*result = compare_names(a, b);
	else if (ca)
		*result = 1;
	else if (cb)
		*result = -1;
	else
		return FALSE;
	return TRUE;
}

int
pidgin_blist_sort_status(PurpleBlistNode *a, PurpleBlistNode *b)
{
	PurpleBuddy *ba, *bb;
	int cmp;

	if (chats_last(a, b, &cmp))
		return cmp;
	if (!PURPLE_BLIST_NODE_IS_CONTACT(a) || !PURPLE_BLIST_NODE_IS_CONTACT(b))
		return compare_names(a, b);

	ba = purple_contact_get_priority_buddy((PurpleContact *)a);
	bb = purple_contact_get_priority_buddy((PurpleContact *)b);

	cmp = purple_presence_compare(ba ? purple_buddy_get_presence(ba) : NULL,
	                              bb ? purple_buddy_get_presence(bb) : NULL);
	return cmp != 0 ? cmp : compare_names(a, b);
}

static int
log_activity_score(PurpleBlistNode *contact)
{
	PurpleBlistNode *n;
	int score = 0;

	for (n = purple_blist_node_get_first_child(contact); n != NULL;
	     n = purple_blist_node_get_sibling_next(n)) {
		PurpleBuddy *buddy = (PurpleBuddy *)n;

		if (!PURPLE_BLIST_NODE_IS_BUDDY(n))
			continue;
		score += purple_log_get_activity_score(PURPLE_LOG_IM,
			purple_buddy_get_name(buddy), purple_buddy_get_account(buddy));
	}
	return score;
}

int
pidgin_blist_sort_log_activity(PurpleBlistNode *a, PurpleBlistNode *b)
{
	int cmp, sa, sb;

	if (chats_last(a, b, &cmp))
		return cmp;
	if (!PURPLE_BLIST_NODE_IS_CONTACT(a) || !PURPLE_BLIST_NODE_IS_CONTACT(b))
		return compare_names(a, b);

	sa = log_activity_score(a);
	sb = log_activity_score(b);
	if (sa != sb)
		return sa > sb ? -1 : 1;
	return compare_names(a, b);
}

/**************************************************************************
 * PidginBlistModel
 **************************************************************************/

struct _PidginBlistModel
{
	GObject parent;

	GListStore *root;
	/* PurpleBlistNode * -> PidginBlistNodeItem * (owned) */
	GHashTable *items;
	PidginBlistSortFunc sort_func;
	PidginBlistShowFlags flags;
};

enum {
	SIGNAL_ITEM_REFRESH,
	SIGNAL_ITEM_INSERTED,
	N_SIGNALS
};

static guint model_signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(PidginBlistModel, pidgin_blist_model, G_TYPE_OBJECT)

static int compare_items(PidginBlistModel *model, PidginBlistNodeItem *a,
                         PidginBlistNodeItem *b);

static void
pidgin_blist_model_dispose(GObject *obj)
{
	PidginBlistModel *model = PIDGIN_BLIST_MODEL(obj);

	if (model->items != NULL)
		pidgin_blist_model_clear(model);
	g_clear_pointer(&model->items, g_hash_table_destroy);
	g_clear_object(&model->root);

	G_OBJECT_CLASS(pidgin_blist_model_parent_class)->dispose(obj);
}

static void
pidgin_blist_model_class_init(PidginBlistModelClass *klass)
{
	GObjectClass *obj_class = G_OBJECT_CLASS(klass);

	obj_class->dispose = pidgin_blist_model_dispose;

	model_signals[SIGNAL_ITEM_REFRESH] = g_signal_new("item-refresh",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, PIDGIN_TYPE_BLIST_NODE_ITEM);
	model_signals[SIGNAL_ITEM_INSERTED] = g_signal_new("item-inserted",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, PIDGIN_TYPE_BLIST_NODE_ITEM);
}

static void
pidgin_blist_model_init(PidginBlistModel *model)
{
	model->root = g_list_store_new(PIDGIN_TYPE_BLIST_NODE_ITEM);
	model->items = g_hash_table_new_full(g_direct_hash, g_direct_equal,
	                                     NULL, g_object_unref);
	model->sort_func = pidgin_blist_sort_alphabetical;
}

PidginBlistModel *
pidgin_blist_model_new(void)
{
	return g_object_new(PIDGIN_TYPE_BLIST_MODEL, NULL);
}

GListModel *
pidgin_blist_model_get_root(PidginBlistModel *model)
{
	g_return_val_if_fail(PIDGIN_IS_BLIST_MODEL(model), NULL);
	return G_LIST_MODEL(model->root);
}

PidginBlistNodeItem *
pidgin_blist_model_lookup(PidginBlistModel *model, PurpleBlistNode *node)
{
	g_return_val_if_fail(PIDGIN_IS_BLIST_MODEL(model), NULL);
	return node ? g_hash_table_lookup(model->items, node) : NULL;
}

PidginBlistNodeItem *
pidgin_blist_model_ensure(PidginBlistModel *model, PurpleBlistNode *node)
{
	PidginBlistNodeItem *item;

	g_return_val_if_fail(PIDGIN_IS_BLIST_MODEL(model), NULL);
	g_return_val_if_fail(node != NULL, NULL);

	item = g_hash_table_lookup(model->items, node);
	if (item == NULL) {
		item = pidgin_blist_node_item_new(node);
		g_hash_table_insert(model->items, node, item);
	}
	return item;
}

static int
store_sort_cb(gconstpointer a, gconstpointer b, gpointer data)
{
	return compare_items(data, (PidginBlistNodeItem *)a, (PidginBlistNodeItem *)b);
}

void
pidgin_blist_model_set_sort_func(PidginBlistModel *model, PidginBlistSortFunc func)
{
	GHashTableIter iter;
	gpointer value;

	g_return_if_fail(PIDGIN_IS_BLIST_MODEL(model));

	if (model->sort_func == func)
		return;
	model->sort_func = func;

	/* Re-sort every group in one go: placing items one by one would
	 * binary-search stores that are not in the new order yet. */
	g_hash_table_iter_init(&iter, model->items);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		PidginBlistNodeItem *gitem = value;
		guint j, m;

		if (!PURPLE_BLIST_NODE_IS_GROUP(gitem->node))
			continue;

		g_list_store_sort(gitem->children, store_sort_cb, model);

		/* The rows were re-created collapsed. */
		m = g_list_model_get_n_items(G_LIST_MODEL(gitem->children));
		for (j = 0; j < m; j++) {
			PidginBlistNodeItem *item = g_list_model_get_item(G_LIST_MODEL(gitem->children), j);
			if (item->expandable && item->expanded)
				g_signal_emit(model, model_signals[SIGNAL_ITEM_INSERTED], 0, item);
			g_object_unref(item);
		}
	}
}

PidginBlistSortFunc
pidgin_blist_model_get_sort_func(PidginBlistModel *model)
{
	g_return_val_if_fail(PIDGIN_IS_BLIST_MODEL(model), NULL);
	return model->sort_func;
}

void
pidgin_blist_model_set_show_flags(PidginBlistModel *model, PidginBlistShowFlags flags)
{
	g_return_if_fail(PIDGIN_IS_BLIST_MODEL(model));
	model->flags = flags;
}

PidginBlistShowFlags
pidgin_blist_model_get_show_flags(PidginBlistModel *model)
{
	g_return_val_if_fail(PIDGIN_IS_BLIST_MODEL(model), 0);
	return model->flags;
}

/**************************************************************************
 * Placement
 **************************************************************************/

static gboolean
account_shown(PidginBlistModel *model, PurpleAccount *account)
{
	return purple_account_is_connected(account) ||
	       (model->flags & PIDGIN_BLIST_SHOW_DISCONNECTED);
}

gboolean
pidgin_blist_model_buddy_is_displayable(PidginBlistModel *model, PurpleBuddy *buddy)
{
	PurpleBlistNode *node = (PurpleBlistNode *)buddy;
	PidginBlistNodeItem *item;

	g_return_val_if_fail(PIDGIN_IS_BLIST_MODEL(model), FALSE);

	if (buddy == NULL || !PURPLE_BLIST_NODE_IS_VISIBLE(node))
		return FALSE;
	if (!account_shown(model, purple_buddy_get_account(buddy)))
		return FALSE;

	item = g_hash_table_lookup(model->items, node);
	return purple_presence_is_online(purple_buddy_get_presence(buddy)) ||
	       (item != NULL && item->recent_signonoff) ||
	       (model->flags & PIDGIN_BLIST_SHOW_OFFLINE) ||
	       purple_blist_node_get_bool(node, "show_offline");
}

/* Orders two items of the same store. Groups (root) and buddies (contact
 * stores) keep the buddy list order; contacts and chats use the sort
 * function. */
static int
compare_items(PidginBlistModel *model, PidginBlistNodeItem *a, PidginBlistNodeItem *b)
{
	PurpleBlistNode *na = a->node, *nb = b->node, *n;

	if (na == nb)
		return 0;

	if (model->sort_func != NULL &&
	    (PURPLE_BLIST_NODE_IS_CONTACT(na) || PURPLE_BLIST_NODE_IS_CHAT(na)))
		return model->sort_func(na, nb);

	/* Buddy list order: is nb after na? */
	for (n = purple_blist_node_get_sibling_next(na); n != NULL;
	     n = purple_blist_node_get_sibling_next(n)) {
		if (n == nb)
			return -1;
	}
	return 1;
}

/* The item counts as hidden (get_visible() is FALSE) while it is being
 * removed, so handlers of the store's (and the GtkTreeListModel's)
 * signals can tell a removal from a user action. */
static void
detach_item(PidginBlistNodeItem *item)
{
	GListStore *store = item->container;
	guint pos;

	if (store == NULL)
		return;
	item->container = NULL;
	if (g_list_store_find(store, item, &pos))
		g_list_store_remove(store, pos);
	g_object_unref(store);
}

/* Where @item belongs in @store (binary search), ignoring @item itself. */
static guint
find_position(PidginBlistModel *model, GListStore *store, PidginBlistNodeItem *item)
{
	guint lo = 0, hi = g_list_model_get_n_items(G_LIST_MODEL(store));

	while (lo < hi) {
		guint mid = lo + (hi - lo) / 2;
		PidginBlistNodeItem *other = g_list_model_get_item(G_LIST_MODEL(store), mid);
		int cmp = compare_items(model, item, other);

		g_object_unref(other);
		if (cmp > 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* TRUE if @item, at @pos of its store, is ordered with its neighbours. */
static gboolean
in_order(PidginBlistModel *model, GListStore *store, PidginBlistNodeItem *item,
         guint pos)
{
	guint n = g_list_model_get_n_items(G_LIST_MODEL(store));
	gboolean ok = TRUE;

	if (pos > 0) {
		PidginBlistNodeItem *prev = g_list_model_get_item(G_LIST_MODEL(store), pos - 1);
		ok = compare_items(model, prev, item) < 0;
		g_object_unref(prev);
	}
	if (ok && pos + 1 < n) {
		PidginBlistNodeItem *next = g_list_model_get_item(G_LIST_MODEL(store), pos + 1);
		ok = compare_items(model, item, next) < 0;
		g_object_unref(next);
	}
	return ok;
}

/* Puts @item into @store at its sorted position (or keeps it there), or
 * removes it from wherever it is if !@show. */
static void
place_item(PidginBlistModel *model, PidginBlistNodeItem *item,
           GListStore *store, gboolean show)
{
	guint pos;

	if (!show || store == NULL) {
		detach_item(item);
		return;
	}

	if (item->container == store && g_list_store_find(store, item, &pos) &&
	    in_order(model, store, item, pos))
		return;

	/* Take it out (of wherever it is) and put it in the right place. */
	detach_item(item);

	pos = find_position(model, store, item);
	g_list_store_insert(store, pos, item);
	item->container = g_object_ref(store);
	g_signal_emit(model, model_signals[SIGNAL_ITEM_INSERTED], 0, item);
}

static void
refresh_item(PidginBlistModel *model, PidginBlistNodeItem *item)
{
	g_signal_emit(model, model_signals[SIGNAL_ITEM_REFRESH], 0, item);
}

/* Empties a children store, forgetting where its items were. */
static void
detach_children(PidginBlistNodeItem *item)
{
	guint i, n;

	if (item->children == NULL)
		return;

	n = g_list_model_get_n_items(G_LIST_MODEL(item->children));
	for (i = 0; i < n; i++) {
		PidginBlistNodeItem *child = g_list_model_get_item(G_LIST_MODEL(item->children), i);
		g_clear_object(&child->container);
		g_object_unref(child);
	}
	g_list_store_remove_all(item->children);
}

static void
update_group(PidginBlistModel *model, PurpleBlistNode *gnode)
{
	PidginBlistNodeItem *item = pidgin_blist_model_ensure(model, gnode);
	gboolean show;

	show = PURPLE_BLIST_NODE_IS_VISIBLE(gnode) &&
	       (g_list_model_get_n_items(G_LIST_MODEL(item->children)) > 0 ||
	        (model->flags & PIDGIN_BLIST_SHOW_EMPTY_GROUPS));

	place_item(model, item, model->root, show);
	refresh_item(model, item);
}

static int
count_buddy_children(PurpleBlistNode *cnode)
{
	PurpleBlistNode *n;
	int count = 0;

	for (n = purple_blist_node_get_first_child(cnode); n != NULL;
	     n = purple_blist_node_get_sibling_next(n)) {
		if (PURPLE_BLIST_NODE_IS_BUDDY(n))
			count++;
	}
	return count;
}

static void
place_buddy(PidginBlistModel *model, PurpleBlistNode *bnode, gboolean refresh)
{
	PidginBlistNodeItem *item = pidgin_blist_model_ensure(model, bnode);
	PidginBlistNodeItem *citem;
	PurpleBlistNode *cnode = purple_blist_node_get_parent(bnode);

	citem = cnode ? g_hash_table_lookup(model->items, cnode) : NULL;
	if (citem == NULL) {
		detach_item(item);
		return;
	}

	place_item(model, item, citem->children,
	           citem->expandable &&
	           pidgin_blist_model_buddy_is_displayable(model, (PurpleBuddy *)bnode));
	if (refresh)
		refresh_item(model, item);
}

static void
update_contact(PidginBlistModel *model, PurpleBlistNode *cnode, gboolean children)
{
	PidginBlistNodeItem *item, *gitem;
	PurpleBlistNode *gnode = purple_blist_node_get_parent(cnode);
	PurpleBlistNode *bnode;
	gboolean show = FALSE, expandable;

	if (gnode == NULL)
		return;

	item = pidgin_blist_model_ensure(model, cnode);
	gitem = pidgin_blist_model_ensure(model, gnode);

	if (PURPLE_BLIST_NODE_IS_VISIBLE(cnode)) {
		for (bnode = purple_blist_node_get_first_child(cnode); bnode != NULL;
		     bnode = purple_blist_node_get_sibling_next(bnode)) {
			if (PURPLE_BLIST_NODE_IS_BUDDY(bnode) &&
			    pidgin_blist_model_buddy_is_displayable(model, (PurpleBuddy *)bnode)) {
				show = TRUE;
				break;
			}
		}
	}

	/* Whether the row has an expander is fixed while it is in the
	 * GtkTreeListModel, so a change needs a reinsert. */
	expandable = count_buddy_children(cnode) > 1;
	if (expandable != item->expandable) {
		detach_item(item);
		detach_children(item);
		item->expandable = expandable;
		if (!expandable)
			item->expanded = FALSE;
		children = TRUE;
	}

	place_item(model, item, gitem->children, show);

	if (children) {
		for (bnode = purple_blist_node_get_first_child(cnode); bnode != NULL;
		     bnode = purple_blist_node_get_sibling_next(bnode)) {
			if (PURPLE_BLIST_NODE_IS_BUDDY(bnode))
				place_buddy(model, bnode, TRUE);
		}
	}

	refresh_item(model, item);
	update_group(model, gnode);
}

static void
update_chat(PidginBlistModel *model, PurpleBlistNode *node)
{
	PidginBlistNodeItem *item, *gitem;
	PurpleBlistNode *gnode = purple_blist_node_get_parent(node);

	if (gnode == NULL)
		return;

	item = pidgin_blist_model_ensure(model, node);
	gitem = pidgin_blist_model_ensure(model, gnode);

	place_item(model, item, gitem->children,
	           PURPLE_BLIST_NODE_IS_VISIBLE(node) &&
	           account_shown(model, purple_chat_get_account((PurpleChat *)node)));
	refresh_item(model, item);
	update_group(model, gnode);
}

void
pidgin_blist_model_update(PidginBlistModel *model, PurpleBlistNode *node)
{
	PurpleBlistNode *cnode;

	g_return_if_fail(PIDGIN_IS_BLIST_MODEL(model));

	if (node == NULL)
		return;

	switch (purple_blist_node_get_type(node)) {
	case PURPLE_BLIST_GROUP_NODE:
		update_group(model, node);
		break;
	case PURPLE_BLIST_CONTACT_NODE:
		update_contact(model, node, FALSE);
		break;
	case PURPLE_BLIST_BUDDY_NODE:
		cnode = purple_blist_node_get_parent(node);
		if (cnode == NULL)
			return;
		/* The contact first: it owns the store the buddy goes in. Its
		 * other buddies are placed too if its expandability changed. */
		update_contact(model, cnode, FALSE);
		place_buddy(model, node, TRUE);
		break;
	case PURPLE_BLIST_CHAT_NODE:
		update_chat(model, node);
		break;
	default:
		break;
	}
}

void
pidgin_blist_model_remove(PidginBlistModel *model, PurpleBlistNode *node)
{
	PidginBlistNodeItem *item;

	g_return_if_fail(PIDGIN_IS_BLIST_MODEL(model));

	item = g_hash_table_lookup(model->items, node);
	if (item == NULL)
		return;

	detach_item(item);
	detach_children(item);
	g_hash_table_remove(model->items, node);
}

void
pidgin_blist_model_update_all(PidginBlistModel *model)
{
	PurpleBuddyList *list = purple_get_blist();
	PurpleBlistNode *gnode, *cnode;

	g_return_if_fail(PIDGIN_IS_BLIST_MODEL(model));

	if (list == NULL)
		return;

	for (gnode = purple_blist_get_root(); gnode != NULL;
	     gnode = purple_blist_node_get_sibling_next(gnode)) {
		if (!PURPLE_BLIST_NODE_IS_GROUP(gnode))
			continue;
		for (cnode = purple_blist_node_get_first_child(gnode); cnode != NULL;
		     cnode = purple_blist_node_get_sibling_next(cnode)) {
			if (PURPLE_BLIST_NODE_IS_CONTACT(cnode))
				update_contact(model, cnode, TRUE);
			else if (PURPLE_BLIST_NODE_IS_CHAT(cnode))
				update_chat(model, cnode);
		}
		update_group(model, gnode);
	}
}

void
pidgin_blist_model_clear(PidginBlistModel *model)
{
	GHashTableIter iter;
	gpointer value;

	g_return_if_fail(PIDGIN_IS_BLIST_MODEL(model));

	g_list_store_remove_all(model->root);
	g_hash_table_iter_init(&iter, model->items);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		PidginBlistNodeItem *item = value;

		if (item->children != NULL)
			g_list_store_remove_all(item->children);
		g_clear_object(&item->container);
	}
	g_hash_table_remove_all(model->items);
}

/**************************************************************************
 * Checks (tests and selftest)
 **************************************************************************/

static guint
count_store(GListStore *store, int type)
{
	guint i, n = g_list_model_get_n_items(G_LIST_MODEL(store)), count = 0;

	for (i = 0; i < n; i++) {
		PidginBlistNodeItem *item = g_list_model_get_item(G_LIST_MODEL(store), i);

		if (type < 0 || (int)purple_blist_node_get_type(item->node) == type)
			count++;
		if (item->children != NULL && item->expandable)
			count += count_store(item->children, type);
		g_object_unref(item);
	}
	return count;
}

guint
pidgin_blist_model_count_visible(PidginBlistModel *model, int type)
{
	g_return_val_if_fail(PIDGIN_IS_BLIST_MODEL(model), 0);
	return count_store(model->root, type);
}

static gboolean
check_store(PidginBlistModel *model, GListStore *store, PurpleBlistNode *parent)
{
	guint i, n = g_list_model_get_n_items(G_LIST_MODEL(store));
	gboolean ok = TRUE;

	for (i = 0; i < n; i++) {
		PidginBlistNodeItem *item = g_list_model_get_item(G_LIST_MODEL(store), i);

		if (item->container != store) {
			purple_debug_error("blistmodel", "%s: wrong container\n",
			                   sort_name(item->node));
			ok = FALSE;
		}
		if (g_hash_table_lookup(model->items, item->node) != item) {
			purple_debug_error("blistmodel", "%s: not in the hash table\n",
			                   sort_name(item->node));
			ok = FALSE;
		}
		if (purple_blist_node_get_parent(item->node) != parent) {
			purple_debug_error("blistmodel", "%s: wrong parent\n",
			                   sort_name(item->node));
			ok = FALSE;
		}
		if (i + 1 < n) {
			PidginBlistNodeItem *next = g_list_model_get_item(G_LIST_MODEL(store), i + 1);
			if (compare_items(model, item, next) >= 0) {
				purple_debug_error("blistmodel", "%s and %s are out of order\n",
				                   sort_name(item->node), sort_name(next->node));
				ok = FALSE;
			}
			g_object_unref(next);
		}
		if (item->children != NULL && item->expandable &&
		    !check_store(model, item->children, item->node))
			ok = FALSE;
		g_object_unref(item);
	}
	return ok;
}

gboolean
pidgin_blist_model_check(PidginBlistModel *model)
{
	GHashTableIter iter;
	gpointer value;
	gboolean ok;

	g_return_val_if_fail(PIDGIN_IS_BLIST_MODEL(model), FALSE);

	ok = check_store(model, model->root, NULL);

	/* Every item that claims to be visible is in its store. */
	g_hash_table_iter_init(&iter, model->items);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		PidginBlistNodeItem *item = value;
		guint pos;

		if (item->container != NULL &&
		    !g_list_store_find(item->container, item, &pos)) {
			purple_debug_error("blistmodel", "%s: not in its container\n",
			                   sort_name(item->node));
			ok = FALSE;
		}
	}
	return ok;
}
