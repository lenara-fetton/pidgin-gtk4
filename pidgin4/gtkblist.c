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
 * GTK 4 rewrite of pidgin/gtkblist.c.
 *
 *   - The list is a GtkListView over a GtkTreeListModel whose root is the
 *     PidginBlistModel's group store (pidginblistmodel.c holds the
 *     placement rules: visibility, sorting, expandable contacts).
 *   - Rows are widgets (GtkTreeExpander > GtkBox) bound to the
 *     PidginBlistNodeItem properties; this file computes those properties
 *     (names, status text, icons, CSS classes) in blist_refresh_item().
 *     The GTK 2 theme engine (PidginBlistTheme) is gone: its colours and
 *     fonts are CSS classes (.pidgin-blist-online, -away, -idle, -offline,
 *     -group, -contact, -chat) that <profile>/pidgin4/gtk4.css can style
 *     (TODO(M5): loading gtk4.css).
 *   - The menubar is a GMenuModel with window actions ("win.*"), the
 *     context menus are GtkPopoverMenus built per node, tooltips are
 *     custom widgets from ::query-tooltip, the connection error area is a
 *     box of PidginMiniDialogs, and DnD uses GtkDragSource/GtkDropTarget.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "account.h"
#include "blist.h"
#include "buddyicon.h"
#include "connection.h"
#include "conversation.h"
#include "core.h"
#include "debug.h"
#include "notify.h"
#include "plugin.h"
#include "prefs.h"
#include "privacy.h"
#include "prpl.h"
#include "request.h"
#include "savedstatuses.h"
#include "server.h"
#include "signals.h"
#include "status.h"
#include "util.h"

#include "gtkaccount.h"
#include "gtkblist.h"
#include "gtkdebug.h"
#include "gtkdialogs.h"
#include "gtkdocklet.h"
#include "gtkstatusbox.h"
#include "gtkutils.h"
#include "pidginblistmodel.h"
#include "pidginmenu.h"
#include "pidginminidialog.h"

#define BLIST_PREFS  PIDGIN_PREFS_ROOT "/blist"
#define BLIST4_PREFS PIDGIN4_PREFS_ROOT "/blist"

/* Object data keys */
#define ROW_KEY      "pidgin-blist-row"
#define ACCOUNT_KEY  "pidgin-account"
#define DO_NOT_CLEAR_ERROR "pidgin-do-not-clear-error"

typedef struct
{
	GtkWidget *window;
	GtkWidget *vbox;
	GtkWidget *menubar;
	GtkWidget *alert_scroll;
	GtkWidget *alert_box;
	GtkWidget *scrolled;
	GtkWidget *list_view;
	GtkWidget *status_box;
	GtkListItemFactory *factory;

	PidginBlistModel *model;
	GtkTreeListModel *tree;
	GtkSingleSelection *selection;

	GMenu *sort_menu;
	GMenu *accounts_menu;
	GMenu *plugins_menu;

	/* The open context menu and its node. */
	GtkWidget *popover;
	PurpleBlistNode *popover_node;

	/* PurpleAccount * -> PidginMiniDialog * (generic errors) */
	GHashTable *error_dialogs;
	PidginMiniDialog *signed_on_elsewhere;

	/* PurpleBlistNode * -> IconCacheEntry * */
	GHashTable *icon_cache;
	/* PurpleAccount * -> GIcon * */
	GHashTable *prpl_icons;

	guint refresh_timer;
	gboolean biglist;
} PidginBuddyList;

static PidginBuddyList *gtkblist = NULL;
static GList *sort_methods = NULL;
static PidginBlistSortMethod *current_sort_method = NULL;

static void blist_refresh_item(PidginBlistNodeItem *item);
static void rebuild_sort_menu(void);
static void rebuild_accounts_menu(void);
static void rebuild_plugins_menu(void);
static void update_menu_sensitivity(void);
static void show_node_menu(PurpleBlistNode *node, GtkWidget *widget, double x, double y);
static void set_node_expanded(PidginBlistNodeItem *item, gboolean expanded);
static void add_buddy_for_node(PurpleBlistNode *node);
static void add_chat_for_node(PurpleBlistNode *node);

/**************************************************************************
 * Helpers
 **************************************************************************/

void *
pidgin_blist_get_handle(void)
{
	static int handle;

	return &handle;
}

GtkWidget *
pidgin_blist_get_window(void)
{
	return gtkblist ? gtkblist->window : NULL;
}

PidginBlistModel *
pidgin_blist_get_model(void)
{
	return gtkblist ? gtkblist->model : NULL;
}

static void
todo_m5(const char *what)
{
	purple_debug_info("gtkblist", "TODO(M5): %s is not ported yet\n", what);
}

static gboolean
show_protocol_icons(void)
{
	return purple_prefs_get_bool(BLIST_PREFS "/show_protocol_icons");
}

static GIcon *
themed_icon_if_exists(const char *name)
{
	GdkDisplay *display = gdk_display_get_default();
	GtkIconTheme *theme = display ? gtk_icon_theme_get_for_display(display) : NULL;

	if (theme != NULL && !gtk_icon_theme_has_icon(theme, name))
		return NULL;
	return g_themed_icon_new(name);
}

/* The account's protocol icon (cached). Not a new reference. */
static GIcon *
account_prpl_icon(PurpleAccount *account)
{
	GIcon *icon;

	if (gtkblist == NULL)
		return NULL;

	icon = g_hash_table_lookup(gtkblist->prpl_icons, account);
	if (icon == NULL) {
		icon = pidgin_create_prpl_gicon(account, NULL);
		g_hash_table_insert(gtkblist->prpl_icons, account, icon);
	}
	return icon;
}

static PidginBlistNodeItem *
lookup_item(PurpleBlistNode *node)
{
	return gtkblist ? pidgin_blist_model_lookup(gtkblist->model, node) : NULL;
}

static gboolean
contact_is_expanded(PurpleBlistNode *cnode)
{
	PidginBlistNodeItem *item = lookup_item(cnode);

	return item != NULL && pidgin_blist_node_item_get_expandable(item) &&
	       pidgin_blist_node_item_get_expanded(item);
}

/* The buddy a (collapsed) contact row stands for: the priority buddy, or
 * the first displayable one if the priority buddy is not displayable. */
static PurpleBuddy *
contact_display_buddy(PurpleContact *contact)
{
	PurpleBuddy *buddy = purple_contact_get_priority_buddy(contact);
	PurpleBlistNode *n;

	if (gtkblist == NULL || buddy == NULL ||
	    pidgin_blist_model_buddy_is_displayable(gtkblist->model, buddy))
		return buddy;

	for (n = purple_blist_node_get_first_child((PurpleBlistNode *)contact);
	     n != NULL; n = purple_blist_node_get_sibling_next(n)) {
		if (PURPLE_BLIST_NODE_IS_BUDDY(n) &&
		    pidgin_blist_model_buddy_is_displayable(gtkblist->model, (PurpleBuddy *)n))
			return (PurpleBuddy *)n;
	}
	return buddy;
}

/* The buddy for IM/info actions on a contact or buddy node. */
static PurpleBuddy *
node_buddy(PurpleBlistNode *node)
{
	if (PURPLE_BLIST_NODE_IS_CONTACT(node))
		return contact_display_buddy((PurpleContact *)node);
	if (PURPLE_BLIST_NODE_IS_BUDDY(node))
		return (PurpleBuddy *)node;
	return NULL;
}

static PurplePluginProtocolInfo *
connected_prpl_info(PurpleAccount *account)
{
	PurpleConnection *gc = purple_account_get_connection(account);
	PurplePlugin *prpl = gc ? purple_connection_get_prpl(gc) : NULL;

	return prpl ? PURPLE_PLUGIN_PROTOCOL_INFO(prpl) : NULL;
}

/* Markup a prpl hands us may be broken; escape it then. */
static char *
safe_markup(char *markup)
{
	char *escaped;

	if (markup == NULL || pango_parse_markup(markup, -1, 0, NULL, NULL, NULL, NULL))
		return markup;
	escaped = g_markup_escape_text(markup, -1);
	g_free(markup);
	return escaped;
}

/**************************************************************************
 * Display properties (Pidgin 2's buddy_node(), pidgin_blist_update_*())
 **************************************************************************/

typedef struct {
	GBytes *bytes;
	GdkTexture *texture;
} IconCacheEntry;

static void
icon_cache_entry_free(IconCacheEntry *entry)
{
	g_bytes_unref(entry->bytes);
	g_clear_object(&entry->texture);
	g_free(entry);
}

/* The node's icon as a texture, decoded only when the data changed.
 * Pidgin 2's pidgin_blist_get_buddy_icon() without the scaling and
 * greying, which CSS does. Returns a new reference or NULL. */
static GdkTexture *
node_icon_texture(PurpleBlistNode *node)
{
	PurpleBuddy *buddy = NULL;
	PurpleBlistNode *custom_node = node;
	PurpleStoredImage *custom;
	PurpleBuddyIcon *icon = NULL;
	gconstpointer data = NULL;
	size_t len = 0;
	IconCacheEntry *entry;
	GBytes *bytes;

	if (PURPLE_BLIST_NODE_IS_CONTACT(node)) {
		buddy = contact_display_buddy((PurpleContact *)node);
	} else if (PURPLE_BLIST_NODE_IS_BUDDY(node)) {
		buddy = (PurpleBuddy *)node;
		custom_node = purple_blist_node_get_parent(node);
	}
	if (custom_node == NULL)
		custom_node = node;

	custom = purple_buddy_icons_node_find_custom_icon(custom_node);
	if (custom != NULL) {
		data = purple_imgstore_get_data(custom);
		len = purple_imgstore_get_size(custom);
	} else if (buddy != NULL) {
		icon = purple_buddy_icons_find(purple_buddy_get_account(buddy),
		                               purple_buddy_get_name(buddy));
		if (icon != NULL)
			data = purple_buddy_icon_get_data(icon, &len);
	}

	if (data == NULL || len == 0) {
		purple_imgstore_unref(custom);
		purple_buddy_icon_unref(icon);
		g_hash_table_remove(gtkblist->icon_cache, node);
		return NULL;
	}

	bytes = g_bytes_new(data, len);
	purple_imgstore_unref(custom);
	purple_buddy_icon_unref(icon);

	entry = g_hash_table_lookup(gtkblist->icon_cache, node);
	if (entry != NULL && g_bytes_equal(entry->bytes, bytes)) {
		g_bytes_unref(bytes);
		return entry->texture ? g_object_ref(entry->texture) : NULL;
	}

	entry = g_new0(IconCacheEntry, 1);
	entry->bytes = bytes;
	entry->texture = pidgin_texture_new_from_data(g_bytes_get_data(bytes, NULL),
	                                              g_bytes_get_size(bytes));
	g_hash_table_insert(gtkblist->icon_cache, node, entry);
	return entry->texture ? g_object_ref(entry->texture) : NULL;
}

static const char *
buddy_status_icon_name(PurpleBuddy *buddy)
{
	PidginBlistNodeItem *item = lookup_item((PurpleBlistNode *)buddy);
	PurplePresence *p = purple_buddy_get_presence(buddy);
	gboolean recent = item != NULL && pidgin_blist_node_item_get_recent_signonoff(item);

	if (PURPLE_BUDDY_IS_ONLINE(buddy) && recent)
		return "pidgin-status-log-in";
	if (recent)
		return "pidgin-status-log-out";
	/* Also covers buddies whose prpl is not loaded (no statuses at all),
	 * which only show with "Buddies of Disconnected Accounts". */
	if (!purple_presence_is_online(p))
		return "pidgin-status-offline";
	if (purple_presence_is_status_primitive_active(p, PURPLE_STATUS_UNAVAILABLE))
		return "pidgin-status-busy";
	if (purple_presence_is_status_primitive_active(p, PURPLE_STATUS_AWAY))
		return "pidgin-status-away";
	if (purple_presence_is_status_primitive_active(p, PURPLE_STATUS_EXTENDED_AWAY))
		return "pidgin-status-extended-away";
	if (purple_presence_is_status_primitive_active(p, PURPLE_STATUS_OFFLINE))
		return "pidgin-status-offline";
	if (!purple_presence_is_idle(p) &&
	    purple_presence_is_status_primitive_active(p, PURPLE_STATUS_INVISIBLE))
		return "pidgin-status-invisible";
	return "pidgin-status-available";
}

/* The CSS class of a buddy row (Pidgin 2's theme font choice). */
static const char *
buddy_style(PurpleBuddy *buddy)
{
	PurplePresence *p = purple_buddy_get_presence(buddy);

	if (purple_presence_is_idle(p))
		return "pidgin-blist-idle";
	if (!purple_presence_is_online(p))
		return "pidgin-blist-offline";
	if (purple_presence_is_available(p))
		return "pidgin-blist-online";
	return "pidgin-blist-away";
}

static GIcon *
mood_icon(const char *mood)
{
	char *name;
	GIcon *icon;

	if (purple_strequal(mood, "busy"))
		return themed_icon_if_exists("pidgin-status-busy");
	if (purple_strequal(mood, "hiptop"))
		return themed_icon_if_exists("pidgin-emblem-hiptop");

	name = g_strdup_printf("pidgin-mood-%s", mood);
	icon = themed_icon_if_exists(name);
	g_free(name);
	return icon;
}

/* Pidgin 2's pidgin_blist_get_emblem(). Returns a new reference or NULL. */
static GIcon *
node_emblem(PurpleBlistNode *node)
{
	PurpleBuddy *buddy = NULL;
	PurplePresence *p;
	PurpleStatus *tune;
	PurplePlugin *prpl;
	PurplePluginProtocolInfo *prpl_info;
	const char *name = NULL;

	if (PURPLE_BLIST_NODE_IS_CONTACT(node)) {
		if (contact_is_expanded(node))
			return NULL;
		buddy = contact_display_buddy((PurpleContact *)node);
	} else if (PURPLE_BLIST_NODE_IS_BUDDY(node)) {
		buddy = (PurpleBuddy *)node;
		p = purple_buddy_get_presence(buddy);
		if (purple_presence_is_status_primitive_active(p, PURPLE_STATUS_MOBILE))
			return themed_icon_if_exists("pidgin-mood-mobile");
		/* A buddy under an expanded contact shows its protocol here
		 * unless the protocol icons are on. */
		if (contact_is_expanded(purple_blist_node_get_parent(node))) {
			if (show_protocol_icons())
				return NULL;
			return g_object_ref(account_prpl_icon(purple_buddy_get_account(buddy)));
		}
	}
	if (buddy == NULL)
		return NULL;

	if (!purple_privacy_check(purple_buddy_get_account(buddy), purple_buddy_get_name(buddy)))
		return themed_icon_if_exists("pidgin-emblem-blocked");

	p = purple_buddy_get_presence(buddy);
	if (purple_presence_is_status_primitive_active(p, PURPLE_STATUS_MOBILE))
		return themed_icon_if_exists("pidgin-mood-mobile");

	tune = purple_presence_get_status(p, "tune");
	if (tune != NULL && purple_status_is_active(tune)) {
		if (purple_status_get_attr_string(tune, "game") != NULL)
			return themed_icon_if_exists("pidgin-emblem-game");
		return themed_icon_if_exists("pidgin-mood-music");
	}

	prpl = purple_find_prpl(purple_account_get_protocol_id(purple_buddy_get_account(buddy)));
	if (prpl == NULL)
		return NULL;

	prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(prpl);
	if (prpl_info != NULL && prpl_info->list_emblem != NULL)
		name = prpl_info->list_emblem(buddy);

	if (name == NULL) {
		PurpleStatus *status;

		if (!purple_presence_is_status_primitive_active(p, PURPLE_STATUS_MOOD))
			return NULL;
		status = purple_presence_get_status(p, "mood");
		name = purple_status_get_attr_string(status, PURPLE_MOOD_NAME);
		if (name == NULL || *name == '\0')
			return NULL;
		return mood_icon(name);
	} else {
		char *icon_name = g_strdup_printf("pidgin-emblem-%s", name);
		GIcon *icon = themed_icon_if_exists(icon_name);
		g_free(icon_name);
		return icon;
	}
}

static char *
idle_text_long(PurplePresence *presence)
{
	time_t idle_secs = purple_presence_get_idle_time(presence);
	time_t t;
	int iday, ihrs, imin;

	if (idle_secs <= 0)
		return g_strdup(_("Idle"));

	time(&t);
	iday = (t - idle_secs) / (24 * 60 * 60);
	ihrs = ((t - idle_secs) / 60 / 60) % 24;
	imin = ((t - idle_secs) / 60) % 60;

	if (iday)
		return g_strdup_printf(_("Idle %dd %dh %02dm"), iday, ihrs, imin);
	if (ihrs)
		return g_strdup_printf(_("Idle %dh %02dm"), ihrs, imin);
	return g_strdup_printf(_("Idle %dm"), imin);
}

static void
refresh_buddy_row(PidginBlistNodeItem *item, PurpleBlistNode *node,
                  PurpleBuddy *buddy)
{
	PurplePresence *presence = purple_buddy_get_presence(buddy);
	PurpleAccount *account = purple_buddy_get_account(buddy);
	PurpleContact *contact = purple_buddy_get_contact(buddy);
	PurplePlugin *prpl;
	PurplePluginProtocolInfo *prpl_info = NULL;
	gboolean contact_row = PURPLE_BLIST_NODE_IS_CONTACT(node);
	char *nametext, *statustext = NULL, *idle_long = NULL, *secondary = NULL;
	char *idle_short = NULL;
	const char *name;
	GIcon *status, *emblem;
	GdkTexture *texture = NULL;

	/* Name */
	if (contact_row && contact != NULL && contact->alias != NULL)
		name = contact->alias;
	else
		name = purple_buddy_get_alias(buddy);

	/* A plugin can supply an escaped name. */
	nametext = purple_signal_emit_return_1(pidgin_blist_get_handle(),
	                                       "drawing-buddy", buddy);
	if (nametext == NULL)
		nametext = g_markup_escape_text(name ? name : "", -1);
	nametext = safe_markup(nametext);

	/* Status text and idle time */
	if (gtkblist->biglist) {
		prpl = purple_find_prpl(purple_account_get_protocol_id(account));
		if (prpl != NULL)
			prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(prpl);

		if (prpl_info && prpl_info->status_text &&
		    purple_account_get_connection(account) != NULL) {
			char *tmp = prpl_info->status_text(buddy);
			const char *end;

			if (tmp != NULL && !g_utf8_validate(tmp, -1, &end)) {
				char *valid = g_strndup(tmp, end - tmp);
				g_free(tmp);
				tmp = valid;
			}
			if (tmp != NULL) {
				g_strdelimit(tmp, "\n", ' ');
				purple_str_strip_char(tmp, '\r');
			}
			statustext = safe_markup(tmp);
		}

		if (!purple_presence_is_online(presence) && statustext == NULL)
			statustext = g_strdup(_("Offline"));

		if (purple_presence_is_idle(presence) &&
		    purple_prefs_get_bool(BLIST_PREFS "/show_idle_time"))
			idle_long = idle_text_long(presence);

		if (statustext != NULL || idle_long != NULL)
			secondary = g_strdup_printf("%s%s%s",
				idle_long ? idle_long : "",
				(idle_long && statustext) ? " - " : "",
				statustext ? statustext : "");

		texture = node_icon_texture(node);
	} else if (purple_prefs_get_bool(BLIST_PREFS "/show_idle_time") &&
	           purple_presence_is_idle(presence)) {
		time_t idle_secs = purple_presence_get_idle_time(presence);

		if (idle_secs > 0) {
			time_t t = time(NULL);
			idle_short = g_strdup_printf("%d:%02d",
				(int)((t - idle_secs) / 3600), (int)(((t - idle_secs) / 60) % 60));
		}
	}

	status = g_themed_icon_new(buddy_status_icon_name(buddy));
	emblem = node_emblem(node);

	g_object_set(item,
		"name", nametext,
		"secondary", secondary,
		"idle", idle_short,
		"status-icon", status,
		"emblem", emblem,
		"protocol-icon", show_protocol_icons() ? account_prpl_icon(account) : NULL,
		"buddy-icon", texture,
		"style", buddy_style(buddy),
		NULL);

	g_clear_object(&status);
	g_clear_object(&emblem);
	g_clear_object(&texture);
	g_free(nametext);
	g_free(statustext);
	g_free(idle_long);
	g_free(idle_short);
	g_free(secondary);
}

static char *
group_title(PurpleGroup *group, gboolean expanded)
{
	char *esc = g_markup_escape_text(purple_group_get_name(group), -1);
	char *mark;

	if (expanded) {
		mark = g_strdup_printf("<b>%s</b>", esc);
	} else {
		gboolean all = gtkblist != NULL &&
			(pidgin_blist_model_get_show_flags(gtkblist->model) &
			 PIDGIN_BLIST_SHOW_DISCONNECTED);

		mark = g_strdup_printf("<b>%s</b> <span weight='light'>(%d/%d)</span>",
			esc, purple_blist_get_group_online_count(group),
			purple_blist_get_group_size(group, all));
	}
	g_free(esc);
	return mark;
}

static void
blist_refresh_item(PidginBlistNodeItem *item)
{
	PurpleBlistNode *node = pidgin_blist_node_item_get_node(item);

	if (gtkblist == NULL)
		return;

	if (PURPLE_BLIST_NODE_IS_GROUP(node)) {
		char *title = group_title((PurpleGroup *)node,
		                          pidgin_blist_node_item_get_expanded(item));
		GdkTexture *texture = gtkblist->biglist ? node_icon_texture(node) : NULL;

		g_object_set(item,
			"name", title,
			"secondary", NULL,
			"idle", NULL,
			"status-icon", NULL,
			"emblem", NULL,
			"protocol-icon", NULL,
			"buddy-icon", texture,
			"style", "pidgin-blist-group",
			NULL);
		g_free(title);
		g_clear_object(&texture);
	} else if (PURPLE_BLIST_NODE_IS_CONTACT(node)) {
		PurpleContact *contact = (PurpleContact *)node;

		if (contact_is_expanded(node)) {
			char *mark = g_markup_escape_text(purple_contact_get_alias(contact), -1);
			GIcon *status = g_themed_icon_new("pidgin-status-person");

			g_object_set(item,
				"name", mark,
				"secondary", NULL,
				"idle", NULL,
				"status-icon", status,
				"emblem", NULL,
				"protocol-icon", NULL,
				"buddy-icon", NULL,
				"style", "pidgin-blist-contact",
				NULL);
			g_free(mark);
			g_object_unref(status);
		} else {
			PurpleBuddy *buddy = contact_display_buddy(contact);

			if (buddy != NULL)
				refresh_buddy_row(item, node, buddy);
		}
	} else if (PURPLE_BLIST_NODE_IS_BUDDY(node)) {
		refresh_buddy_row(item, node, (PurpleBuddy *)node);
	} else if (PURPLE_BLIST_NODE_IS_CHAT(node)) {
		PurpleChat *chat = (PurpleChat *)node;
		char *mark = g_markup_escape_text(purple_chat_get_name(chat), -1);
		GIcon *status = g_themed_icon_new("pidgin-status-chat");
		GdkTexture *texture = gtkblist->biglist ? node_icon_texture(node) : NULL;

		/* TODO(M4): bold/"nick said" for chats with unseen messages. */
		g_object_set(item,
			"name", mark,
			"secondary", NULL,
			"idle", NULL,
			"status-icon", status,
			"emblem", NULL,
			"protocol-icon", show_protocol_icons()
				? account_prpl_icon(purple_chat_get_account(chat)) : NULL,
			"buddy-icon", texture,
			"style", "pidgin-blist-chat",
			NULL);
		g_free(mark);
		g_object_unref(status);
		g_clear_object(&texture);
	}
}

static void
model_item_refresh_cb(PidginBlistModel *model, PidginBlistNodeItem *item, gpointer data)
{
	blist_refresh_item(item);
}

/**************************************************************************
 * Expanded state
 **************************************************************************/

/* The GtkTreeListRow of a visible group or of a contact in an expanded
 * group, or NULL. */
static GtkTreeListRow *
row_for_item(PidginBlistNodeItem *item)
{
	PurpleBlistNode *node = pidgin_blist_node_item_get_node(item);
	GListModel *root = pidgin_blist_model_get_root(gtkblist->model);
	guint pos;

	if (PURPLE_BLIST_NODE_IS_GROUP(node)) {
		if (!g_list_store_find(G_LIST_STORE(root), item, &pos))
			return NULL;
		return gtk_tree_list_model_get_child_row(gtkblist->tree, pos);
	}

	if (PURPLE_BLIST_NODE_IS_CONTACT(node) || PURPLE_BLIST_NODE_IS_CHAT(node)) {
		PidginBlistNodeItem *gitem = lookup_item(purple_blist_node_get_parent(node));
		GtkTreeListRow *grow, *row = NULL;
		GListModel *children;

		if (gitem == NULL || (grow = row_for_item(gitem)) == NULL)
			return NULL;
		children = pidgin_blist_node_item_get_children(gitem);
		if (children != NULL && gtk_tree_list_row_get_expanded(grow) &&
		    g_list_store_find(G_LIST_STORE(children), item, &pos))
			row = gtk_tree_list_row_get_child_row(grow, pos);
		g_clear_object(&children);
		g_object_unref(grow);
		return row;
	}
	return NULL;
}

/* Pidgin 2's gtk_blist_row_expanded_cb / _collapsed_cb. */
static void
set_node_expanded(PidginBlistNodeItem *item, gboolean expanded)
{
	PurpleBlistNode *node = pidgin_blist_node_item_get_node(item);

	pidgin_blist_node_item_set_expanded(item, expanded);

	if (PURPLE_BLIST_NODE_IS_GROUP(node)) {
		/* Only write a change: blist.xml is shared with Pidgin 2. */
		if (purple_blist_node_get_bool(node, "collapsed") != !expanded)
			purple_blist_node_set_bool(node, "collapsed", !expanded);

		if (!expanded) {
			PurpleBlistNode *cnode;

			for (cnode = purple_blist_node_get_first_child(node); cnode != NULL;
			     cnode = purple_blist_node_get_sibling_next(cnode)) {
				PidginBlistNodeItem *citem = lookup_item(cnode);

				if (citem != NULL && pidgin_blist_node_item_get_expanded(citem)) {
					pidgin_blist_node_item_set_expanded(citem, FALSE);
					blist_refresh_item(citem);
				}
			}
		}
	}
	blist_refresh_item(item);
}

/* After (re)insertion the tree row starts collapsed: restore it. */
static void
model_item_inserted_cb(PidginBlistModel *model, PidginBlistNodeItem *item, gpointer data)
{
	PurpleBlistNode *node = pidgin_blist_node_item_get_node(item);
	GtkTreeListRow *row;
	gboolean expanded;

	if (gtkblist == NULL || gtkblist->tree == NULL)
		return;

	if (PURPLE_BLIST_NODE_IS_GROUP(node))
		expanded = !purple_blist_node_get_bool(node, "collapsed");
	else if (PURPLE_BLIST_NODE_IS_CONTACT(node))
		expanded = pidgin_blist_node_item_get_expandable(item) &&
		           pidgin_blist_node_item_get_expanded(item);
	else
		return;

	row = row_for_item(item);
	if (row == NULL)
		return;
	pidgin_blist_node_item_set_expanded(item, expanded);
	if (gtk_tree_list_row_get_expanded(row) != expanded)
		gtk_tree_list_row_set_expanded(row, expanded);
	g_object_unref(row);
}

/* Expands or collapses a node's row, as the expander would. */
static void
expand_node(PurpleBlistNode *node, gboolean expanded)
{
	PidginBlistNodeItem *item = lookup_item(node);
	GtkTreeListRow *row;

	if (item == NULL)
		return;
	row = row_for_item(item);
	if (row != NULL) {
		gtk_tree_list_row_set_expanded(row, expanded);
		g_object_unref(row);
	}
	set_node_expanded(item, expanded);
}

/**************************************************************************
 * Tooltips (Pidgin 2's pidgin_get_tooltip_text / create_tip_for_node)
 **************************************************************************/

static char *
chat_tooltip_text(PurpleChat *chat)
{
	GString *str = g_string_new("");
	PurpleAccount *account = purple_chat_get_account(chat);
	PurplePlugin *prpl = purple_find_prpl(purple_account_get_protocol_id(account));
	PurplePluginProtocolInfo *prpl_info = prpl ? PURPLE_PLUGIN_PROTOCOL_INFO(prpl) : NULL;
	PurpleConversation *conv;
	GList *connections = purple_connections_get_all(), *cur;
	char *chat_name, *tmp;

	if (connections && connections->next) {
		tmp = g_markup_escape_text(purple_account_get_username(account), -1);
		g_string_append_printf(str, _("<b>Account:</b> %s"), tmp);
		g_free(tmp);
	}

	if (prpl_info && prpl_info->get_chat_name)
		chat_name = prpl_info->get_chat_name(purple_chat_get_components(chat));
	else
		chat_name = g_strdup(purple_chat_get_name(chat));
	conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, chat_name, account);
	g_free(chat_name);

	if (conv && !purple_conv_chat_has_left(PURPLE_CONV_CHAT(conv))) {
		g_string_append_printf(str, _("\n<b>Occupants:</b> %d"),
			g_list_length(purple_conv_chat_get_users(PURPLE_CONV_CHAT(conv))));

		if (prpl_info && (prpl_info->options & OPT_PROTO_CHAT_TOPIC)) {
			const char *chattopic = purple_conv_chat_get_topic(PURPLE_CONV_CHAT(conv));
			char *topic = chattopic ? g_markup_escape_text(chattopic, -1) : NULL;
			g_string_append_printf(str, _("\n<b>Topic:</b> %s"), topic ? topic : _("(no topic set)"));
			g_free(topic);
		}
	}

	if (prpl_info && prpl_info->chat_info != NULL &&
	    purple_account_get_connection(account) != NULL)
		cur = prpl_info->chat_info(purple_account_get_connection(account));
	else
		cur = NULL;

	while (cur != NULL) {
		struct proto_chat_entry *pce = cur->data;

		if (!pce->secret && (!pce->required &&
			g_hash_table_lookup(purple_chat_get_components(chat), pce->identifier) == NULL)) {
			char *name, *value;

			tmp = purple_text_strip_mnemonic(pce->label);
			name = g_markup_escape_text(tmp, -1);
			g_free(tmp);
			value = g_markup_escape_text(g_hash_table_lookup(
				purple_chat_get_components(chat), pce->identifier) ?: "", -1);
			g_string_append_printf(str, "\n<b>%s</b> %s", name ? name : "", value);
			g_free(name);
			g_free(value);
		}

		g_free(pce);
		cur = g_list_delete_link(cur, cur);
	}

	return g_string_free(str, FALSE);
}

static char *
buddy_tooltip_text(PurpleBlistNode *node, gboolean full)
{
	PurpleContact *c;
	PurpleBuddy *b;
	PurplePresence *presence;
	PurpleNotifyUserInfo *user_info;
	PurplePlugin *prpl;
	PurplePluginProtocolInfo *prpl_info = NULL;
	GList *connections;
	char *tmp, *html, *markup;
	time_t idle_secs, signon;

	if (PURPLE_BLIST_NODE_IS_CONTACT(node)) {
		c = (PurpleContact *)node;
		b = contact_display_buddy(c);
	} else {
		b = (PurpleBuddy *)node;
		c = purple_buddy_get_contact(b);
	}
	if (b == NULL)
		return g_strdup("");

	prpl = purple_find_prpl(purple_account_get_protocol_id(b->account));
	if (prpl != NULL)
		prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(prpl);

	presence = purple_buddy_get_presence(b);
	user_info = purple_notify_user_info_new();

	/* Account */
	connections = purple_connections_get_all();
	if (full && connections && connections->next) {
		tmp = g_markup_escape_text(purple_account_get_username(b->account), -1);
		purple_notify_user_info_add_pair(user_info, _("Account"), tmp);
		g_free(tmp);
	}

	/* Alias */
	if (full && c && b->alias != NULL && b->alias[0] != '\0' &&
	    (c->alias != NULL && c->alias[0] != '\0') &&
	    !purple_strequal(c->alias, b->alias)) {
		tmp = g_markup_escape_text(b->alias, -1);
		purple_notify_user_info_add_pair(user_info, _("Buddy Alias"), tmp);
		g_free(tmp);
	}

	/* Nickname/Server Alias */
	if (full && b->server_alias != NULL && b->server_alias[0] != '\0') {
		tmp = g_markup_escape_text(b->server_alias, -1);
		purple_notify_user_info_add_pair(user_info, _("Nickname"), tmp);
		g_free(tmp);
	}

	/* Logged In */
	signon = purple_presence_get_login_time(presence);
	if (full && PURPLE_BUDDY_IS_ONLINE(b) && signon > 0) {
		if (signon > time(NULL))
			tmp = g_strdup(purple_date_format_long(localtime(&signon)));
		else
			tmp = purple_str_seconds_to_string(time(NULL) - signon);
		purple_notify_user_info_add_pair(user_info, _("Logged In"), tmp);
		g_free(tmp);
	}

	/* Idle */
	if (purple_presence_is_idle(presence)) {
		idle_secs = purple_presence_get_idle_time(presence);
		if (idle_secs > 0) {
			tmp = purple_str_seconds_to_string(time(NULL) - idle_secs);
			purple_notify_user_info_add_pair(user_info, _("Idle"), tmp);
			g_free(tmp);
		}
	}

	/* Last Seen */
	if (full && c && !PURPLE_BUDDY_IS_ONLINE(b)) {
		PurpleBlistNode *bnode;
		int lastseen = 0;

		if (!contact_is_expanded((PurpleBlistNode *)c) || PURPLE_BLIST_NODE_IS_CONTACT(node)) {
			for (bnode = ((PurpleBlistNode *)c)->child; bnode != NULL; bnode = bnode->next) {
				int value = purple_blist_node_get_int(bnode, "last_seen");
				if (value > lastseen)
					lastseen = value;
			}
		} else {
			lastseen = purple_blist_node_get_int(&b->node, "last_seen");
		}

		if (lastseen > 0) {
			tmp = purple_str_seconds_to_string(time(NULL) - lastseen);
			purple_notify_user_info_add_pair(user_info, _("Last Seen"), tmp);
			g_free(tmp);
		}
	}

	/* Offline? */
	if (!PURPLE_BUDDY_IS_ONLINE(b))
		purple_notify_user_info_add_pair(user_info, _("Status"), _("Offline"));

	if (purple_account_is_connected(b->account) && prpl_info && prpl_info->tooltip_text)
		prpl_info->tooltip_text(b, user_info, full);

	html = purple_notify_user_info_get_text_with_newline(user_info, "<br>");
	purple_notify_user_info_destroy(user_info);

	/* The prpl text is purple HTML; show it as Pango markup. */
	markup = pidgin_html_to_pango_markup(html);
	g_free(html);
	return markup;
}

static char *
group_tooltip_text(PurpleGroup *group)
{
	GString *str = g_string_new("");
	int count;

	count = purple_blist_get_group_online_count(group);
	if (count != 0)
		g_string_append_printf(str, "<b>%s:</b> %d", _("Online Buddies"), count);

	count = purple_blist_get_group_size(group, FALSE);
	if (count != 0)
		g_string_append_printf(str, "%s<b>%s:</b> %d", str->len ? "\n" : "",
		                       _("Total Buddies"), count);

	return g_string_free(str, FALSE);
}

char *
pidgin_blist_get_tooltip_text(PurpleBlistNode *node, gboolean full)
{
	GString *str;
	char *text = NULL;

	g_return_val_if_fail(node != NULL, NULL);

	if (PURPLE_BLIST_NODE_IS_CHAT(node))
		text = chat_tooltip_text((PurpleChat *)node);
	else if (PURPLE_BLIST_NODE_IS_CONTACT(node) || PURPLE_BLIST_NODE_IS_BUDDY(node))
		text = buddy_tooltip_text(node, full);
	else if (PURPLE_BLIST_NODE_IS_GROUP(node))
		text = group_tooltip_text((PurpleGroup *)node);

	str = g_string_new(text);
	g_free(text);

	/* Same signature as Pidgin 2 (cap adds lines here). */
	purple_signal_emit(pidgin_blist_get_handle(), "drawing-tooltip", node, str, full);

	return safe_markup(g_string_free(str, FALSE));
}

static GtkWidget *
tooltip_label(const char *markup)
{
	GtkWidget *label = gtk_label_new(NULL);

	gtk_label_set_markup(GTK_LABEL(label), markup);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(label), 60);
	return label;
}

/* One block of the tooltip: icons, name, the text and the buddy icon. */
static GtkWidget *
tooltip_block(PurpleBlistNode *node, gboolean full)
{
	GtkWidget *hbox, *vbox, *title, *image;
	PurpleAccount *account = NULL;
	GdkTexture *texture;
	const char *name = NULL, *status_icon = NULL;
	char *markup, *text;

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BORDER);
	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BOX_SPACE);
	gtk_widget_set_hexpand(vbox, TRUE);
	gtk_box_append(GTK_BOX(hbox), vbox);

	if (PURPLE_BLIST_NODE_IS_BUDDY(node) || PURPLE_BLIST_NODE_IS_CONTACT(node)) {
		PurpleBuddy *buddy = node_buddy(node);

		if (buddy == NULL)
			return hbox;
		account = purple_buddy_get_account(buddy);
		name = PURPLE_BLIST_NODE_IS_CONTACT(node)
			? purple_contact_get_alias((PurpleContact *)node)
			: purple_buddy_get_alias(buddy);
		status_icon = buddy_status_icon_name(buddy);
	} else if (PURPLE_BLIST_NODE_IS_CHAT(node)) {
		account = purple_chat_get_account((PurpleChat *)node);
		name = purple_chat_get_name((PurpleChat *)node);
		status_icon = "pidgin-status-chat";
	} else if (PURPLE_BLIST_NODE_IS_GROUP(node)) {
		name = purple_group_get_name((PurpleGroup *)node);
	}

	title = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	if (status_icon != NULL)
		gtk_box_append(GTK_BOX(title), gtk_image_new_from_icon_name(status_icon));
	markup = g_markup_printf_escaped("<b><big>%s</big></b>", name ? name : "");
	gtk_box_append(GTK_BOX(title), tooltip_label(markup));
	g_free(markup);
	if (account != NULL) {
		image = gtk_image_new_from_gicon(account_prpl_icon(account));
		gtk_box_append(GTK_BOX(title), image);
	}
	gtk_box_append(GTK_BOX(vbox), title);

	text = pidgin_blist_get_tooltip_text(node, full);
	if (text != NULL && *text != '\0')
		gtk_box_append(GTK_BOX(vbox), tooltip_label(text));
	g_free(text);

	texture = node_icon_texture(node);
	if (texture != NULL) {
		GtkWidget *picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(texture));

		gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
		gtk_widget_set_size_request(picture, 96, 96);
		gtk_widget_set_valign(picture, GTK_ALIGN_START);
		gtk_box_append(GTK_BOX(hbox), picture);
		g_object_unref(texture);
	}

	return hbox;
}

static GtkWidget *
tooltip_widget(PurpleBlistNode *node)
{
	GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BORDER);

	gtk_widget_add_css_class(vbox, "pidgin-blist-tooltip");

	/* A collapsed contact shows each of its displayable buddies, the
	 * displayed one in full, as Pidgin 2 did. */
	if (PURPLE_BLIST_NODE_IS_CONTACT(node) && !contact_is_expanded(node)) {
		PurpleBuddy *shown = contact_display_buddy((PurpleContact *)node);
		PurpleBlistNode *n;

		if (shown != NULL)
			gtk_box_append(GTK_BOX(vbox), tooltip_block((PurpleBlistNode *)shown, TRUE));
		for (n = purple_blist_node_get_first_child(node); n != NULL;
		     n = purple_blist_node_get_sibling_next(n)) {
			if (n == (PurpleBlistNode *)shown || !PURPLE_BLIST_NODE_IS_BUDDY(n) ||
			    !pidgin_blist_model_buddy_is_displayable(gtkblist->model, (PurpleBuddy *)n))
				continue;
			gtk_box_append(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
			gtk_box_append(GTK_BOX(vbox), tooltip_block(n, FALSE));
		}
	} else {
		gtk_box_append(GTK_BOX(vbox), tooltip_block(node, TRUE));
	}
	return vbox;
}

/**************************************************************************
 * Rows
 **************************************************************************/

typedef struct {
	GtkListItem *list_item;
	GtkWidget *expander;
	GtkWidget *status;
	GtkWidget *name;
	GtkWidget *secondary;
	GtkWidget *idle;
	GtkWidget *emblem;
	GtkWidget *protocol;
	GtkWidget *avatar;

	PidginBlistNodeItem *item;
	GtkTreeListRow *row;
	GPtrArray *bindings;
	gulong style_id;
	gulong expanded_id;
	char *style;

	GtkWidget *drop_highlight;
} BlistRow;

static PurpleBlistNode *
row_node(BlistRow *r)
{
	return r->item ? pidgin_blist_node_item_get_node(r->item) : NULL;
}

static gboolean
string_to_visible(GBinding *binding, const GValue *from, GValue *to, gpointer data)
{
	const char *s = g_value_get_string(from);

	g_value_set_boolean(to, s != NULL && *s != '\0');
	return TRUE;
}

static gboolean
object_to_visible(GBinding *binding, const GValue *from, GValue *to, gpointer data)
{
	g_value_set_boolean(to, g_value_get_object(from) != NULL);
	return TRUE;
}

static void
bind_prop(BlistRow *r, const char *source, GtkWidget *target, const char *prop)
{
	g_ptr_array_add(r->bindings, g_object_bind_property(r->item, source,
		target, prop, G_BINDING_SYNC_CREATE));
}

static void
bind_visible(BlistRow *r, const char *source, GtkWidget *target, gboolean is_string)
{
	g_ptr_array_add(r->bindings, g_object_bind_property_full(r->item, source,
		target, "visible", G_BINDING_SYNC_CREATE,
		is_string ? string_to_visible : object_to_visible, NULL, NULL, NULL));
}

static void
row_update_style(BlistRow *r)
{
	const char *style = r->item ? pidgin_blist_node_item_get_style(r->item) : NULL;

	if (r->style != NULL) {
		gtk_widget_remove_css_class(r->expander, r->style);
		g_clear_pointer(&r->style, g_free);
	}
	if (style != NULL) {
		gtk_widget_add_css_class(r->expander, style);
		r->style = g_strdup(style);
	}
}

static void
row_style_changed_cb(GObject *obj, GParamSpec *pspec, BlistRow *r)
{
	row_update_style(r);
}

static void
row_expanded_cb(GtkTreeListRow *row, GParamSpec *pspec, BlistRow *r)
{
	gpointer item;

	/* Nothing to record while the buddy list is being destroyed. */
	if (gtkblist == NULL)
		return;

	/* A row whose item left the model notifies "expanded" (now FALSE)
	 * too; that is not the user collapsing it. */
	item = gtk_tree_list_row_get_item(row);
	if (item == NULL)
		return;
	g_object_unref(item);

	if (r->item != NULL && pidgin_blist_node_item_get_visible(r->item))
		set_node_expanded(r->item, gtk_tree_list_row_get_expanded(row));
}

static gboolean
row_query_tooltip_cb(GtkWidget *widget, int x, int y, gboolean keyboard,
                     GtkTooltip *tooltip, BlistRow *r)
{
	PurpleBlistNode *node = row_node(r);

	if (node == NULL || gtkblist == NULL || gtkblist->popover != NULL)
		return FALSE;

	gtk_tooltip_set_custom(tooltip, tooltip_widget(node));
	return TRUE;
}

static void
row_select(BlistRow *r)
{
	guint pos = gtk_list_item_get_position(r->list_item);

	if (pos != GTK_INVALID_LIST_POSITION)
		gtk_selection_model_select_item(GTK_SELECTION_MODEL(gtkblist->selection), pos, TRUE);
}

static void
row_right_click_cb(GtkGestureClick *gesture, int n_press, double x, double y, BlistRow *r)
{
	PurpleBlistNode *node = row_node(r);

	if (node == NULL)
		return;
	gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
	row_select(r);
	show_node_menu(node, r->expander, x, y);
}

static void
row_long_press_cb(GtkGestureLongPress *gesture, double x, double y, BlistRow *r)
{
	PurpleBlistNode *node = row_node(r);

	if (node == NULL)
		return;
	row_select(r);
	show_node_menu(node, r->expander, x, y);
}

/**************************************************************************
 * Drag and drop (Pidgin 2's pidgin_blist_drag_data_rcv_cb)
 **************************************************************************/

typedef enum {
	DROP_BEFORE,
	DROP_INTO_OR_BEFORE,
	DROP_INTO_OR_AFTER,
	DROP_AFTER
} DropPosition;

static DropPosition
drop_position(GtkWidget *widget, double y)
{
	int height = gtk_widget_get_height(widget);

	if (height <= 0)
		return DROP_INTO_OR_AFTER;
	if (y < height / 4.0)
		return DROP_BEFORE;
	if (y < height / 2.0)
		return DROP_INTO_OR_BEFORE;
	if (y < height * 3 / 4.0)
		return DROP_INTO_OR_AFTER;
	return DROP_AFTER;
}

static void
move_node(PurpleBlistNode *n, PurpleBlistNode *node, DropPosition position)
{
	gboolean expanded = PURPLE_BLIST_NODE_IS_CONTACT(node) && contact_is_expanded(node);
	gboolean after = (position == DROP_AFTER || position == DROP_INTO_OR_AFTER);

	if (n == node)
		return;

	if (PURPLE_BLIST_NODE_IS_CONTACT(n)) {
		PurpleContact *c = (PurpleContact *)n;

		if (PURPLE_BLIST_NODE_IS_CONTACT(node) && expanded) {
			purple_blist_merge_contact(c, node);
		} else if (PURPLE_BLIST_NODE_IS_CONTACT(node) || PURPLE_BLIST_NODE_IS_CHAT(node)) {
			purple_blist_add_contact(c, (PurpleGroup *)node->parent,
			                         after ? node : node->prev);
		} else if (PURPLE_BLIST_NODE_IS_GROUP(node)) {
			purple_blist_add_contact(c, (PurpleGroup *)node, NULL);
		} else if (PURPLE_BLIST_NODE_IS_BUDDY(node)) {
			purple_blist_merge_contact(c, node);
		}
	} else if (PURPLE_BLIST_NODE_IS_BUDDY(n)) {
		PurpleBuddy *b = (PurpleBuddy *)n;

		if (PURPLE_BLIST_NODE_IS_BUDDY(node)) {
			purple_blist_add_buddy(b, (PurpleContact *)node->parent,
			                       (PurpleGroup *)node->parent->parent,
			                       after ? node : node->prev);
		} else if (PURPLE_BLIST_NODE_IS_CHAT(node)) {
			purple_blist_add_buddy(b, NULL, (PurpleGroup *)node->parent, NULL);
		} else if (PURPLE_BLIST_NODE_IS_GROUP(node)) {
			purple_blist_add_buddy(b, NULL, (PurpleGroup *)node, NULL);
		} else if (PURPLE_BLIST_NODE_IS_CONTACT(node)) {
			if (expanded) {
				if (position == DROP_BEFORE)
					purple_blist_add_buddy(b, NULL, (PurpleGroup *)node->parent, node->prev);
				else
					purple_blist_add_buddy(b, (PurpleContact *)node,
					                       (PurpleGroup *)node->parent, NULL);
			} else {
				purple_blist_add_buddy(b, NULL, (PurpleGroup *)node->parent,
				                       after ? NULL : node->prev);
			}
		}
	} else if (PURPLE_BLIST_NODE_IS_CHAT(n)) {
		PurpleChat *chat = (PurpleChat *)n;

		if (PURPLE_BLIST_NODE_IS_BUDDY(node)) {
			purple_blist_add_chat(chat, (PurpleGroup *)node->parent->parent,
			                      node->parent);
		} else if (PURPLE_BLIST_NODE_IS_CONTACT(node) || PURPLE_BLIST_NODE_IS_CHAT(node)) {
			purple_blist_add_chat(chat, (PurpleGroup *)node->parent,
			                      after ? node : node->prev);
		} else if (PURPLE_BLIST_NODE_IS_GROUP(node)) {
			purple_blist_add_chat(chat, (PurpleGroup *)node, NULL);
		}
	} else if (PURPLE_BLIST_NODE_IS_GROUP(n)) {
		PurpleGroup *g = (PurpleGroup *)n;

		if (PURPLE_BLIST_NODE_IS_GROUP(node)) {
			purple_blist_add_group(g, after ? node : node->prev);
		} else if (PURPLE_BLIST_NODE_IS_BUDDY(node)) {
			purple_blist_add_group(g, node->parent->parent);
		} else if (PURPLE_BLIST_NODE_IS_CONTACT(node) || PURPLE_BLIST_NODE_IS_CHAT(node)) {
			purple_blist_add_group(g, node->parent);
		}
	}
}

/* Dropped files on a buddy: an image can become the contact's custom
 * icon, anything else is sent (Pidgin 2's pidgin_dnd_file_manage without
 * the conversation parts). */
typedef struct {
	PurpleAccount *account;
	char *who;
	char *path;
	PurpleBlistNode *icon_node;
} DndFile;

static void
dnd_file_free(DndFile *f)
{
	g_free(f->who);
	g_free(f->path);
	g_free(f);
}

static void
dnd_send_file_cb(DndFile *f, int choice)
{
	PurpleConnection *gc = purple_account_get_connection(f->account);

	if (gc != NULL)
		serv_send_file(gc, f->who, f->path);
	dnd_file_free(f);
}

static void
dnd_set_icon_cb(DndFile *f, int choice)
{
	PurpleBlistNode *node = f->icon_node;

	/* The node may have been removed while the question was open; the
	 * request is closed with it, but be careful. */
	if (node != NULL && lookup_item(node) != NULL)
		purple_buddy_icons_node_set_custom_icon_from_file(node, f->path);
	dnd_file_free(f);
}

static void
dnd_cancel_cb(DndFile *f, int choice)
{
	dnd_file_free(f);
}

static void
dnd_file_manage(PurpleBlistNode *node, GFile *file)
{
	PurpleBuddy *buddy = node_buddy(node);
	PurpleAccount *account;
	PurplePluginProtocolInfo *prpl_info;
	char *path = g_file_get_path(file);
	char *content_type;
	gboolean is_image, can_send;
	DndFile *f;

	if (buddy == NULL || path == NULL) {
		g_free(path);
		return;
	}
	account = purple_buddy_get_account(buddy);
	prpl_info = connected_prpl_info(account);
	can_send = prpl_info != NULL && prpl_info->send_file != NULL &&
		(prpl_info->can_receive_file == NULL ||
		 prpl_info->can_receive_file(purple_account_get_connection(account),
		                             purple_buddy_get_name(buddy)));

	content_type = g_content_type_guess(path, NULL, 0, NULL);
	is_image = content_type != NULL && g_str_has_prefix(content_type, "image/");
	g_free(content_type);

	f = g_new0(DndFile, 1);
	f->account = account;
	f->who = g_strdup(purple_buddy_get_name(buddy));
	f->path = path;
	f->icon_node = PURPLE_BLIST_NODE_IS_BUDDY(node) ? purple_blist_node_get_parent(node) : node;

	if (is_image) {
		char *basename = g_path_get_basename(path);
		char *msg = g_strdup_printf(_("You have dragged an image onto %s"),
		                            purple_buddy_get_alias(buddy));

		if (can_send)
			purple_request_action(f->icon_node, NULL, msg,
				_("You can set this image as the buddy icon shown for this "
				  "contact, or send it as a file."), 0,
				account, f->who, NULL, f, 3,
				_("_Set as buddy icon"), G_CALLBACK(dnd_set_icon_cb),
				_("Se_nd image file"), G_CALLBACK(dnd_send_file_cb),
				_("Cancel"), G_CALLBACK(dnd_cancel_cb));
		else
			purple_request_action(f->icon_node, NULL, msg,
				_("You can set this image as the buddy icon shown for this "
				  "contact."), 0,
				account, f->who, NULL, f, 2,
				_("_Set as buddy icon"), G_CALLBACK(dnd_set_icon_cb),
				_("Cancel"), G_CALLBACK(dnd_cancel_cb));
		g_free(msg);
		g_free(basename);
	} else if (can_send) {
		dnd_send_file_cb(f, 0);
	} else {
		purple_notify_error(NULL, NULL, _("Cannot send a file"),
			_("The buddy's account is not connected or cannot send files."));
		dnd_file_free(f);
	}
}

static GdkContentProvider *
row_drag_prepare_cb(GtkDragSource *source, double x, double y, BlistRow *r)
{
	PurpleBlistNode *node = row_node(r);
	GdkContentProvider *providers[2];
	int n = 0;
	PurpleBuddy *buddy;

	if (node == NULL)
		return NULL;

	providers[n++] = gdk_content_provider_new_typed(PIDGIN_TYPE_BLIST_NODE_ITEM, r->item);

	/* Pidgin 2 also offered application/x-im-contact for other apps. */
	buddy = node_buddy(node);
	if (buddy != NULL) {
		PurplePluginProtocolInfo *prpl_info = connected_prpl_info(purple_buddy_get_account(buddy));

		if (prpl_info != NULL && prpl_info->list_icon != NULL) {
			GString *str = g_string_new(NULL);
			GBytes *bytes;

			g_string_printf(str,
				"MIME-Version: 1.0\r\n"
				"Content-Type: application/x-im-contact\r\n"
				"X-IM-Protocol: %s\r\n"
				"X-IM-Username: %s\r\n",
				prpl_info->list_icon(purple_buddy_get_account(buddy), buddy),
				purple_buddy_get_name(buddy));
			if (buddy->alias != NULL)
				g_string_append_printf(str, "X-IM-Alias: %s\r\n", buddy->alias);
			g_string_append(str, "\r\n");
			bytes = g_string_free_to_bytes(str);
			providers[n++] = gdk_content_provider_new_for_bytes("application/x-im-contact", bytes);
			g_bytes_unref(bytes);
		}
	}

	return gdk_content_provider_new_union(providers, n);
}

static void
row_drag_begin_cb(GtkDragSource *source, GdkDrag *drag, BlistRow *r)
{
	GdkPaintable *paintable = gtk_widget_paintable_new(r->expander);

	gtk_drag_source_set_icon(source, paintable, 0, 0);
	g_object_unref(paintable);
}

static void
row_clear_drop_highlight(BlistRow *r)
{
	gtk_widget_remove_css_class(r->expander, "pidgin-blist-drop-before");
	gtk_widget_remove_css_class(r->expander, "pidgin-blist-drop-into");
	gtk_widget_remove_css_class(r->expander, "pidgin-blist-drop-after");
}

static GdkDragAction
row_drop_motion_cb(GtkDropTarget *target, double x, double y, BlistRow *r)
{
	DropPosition pos = drop_position(r->expander, y);
	GdkDrop *drop = gtk_drop_target_get_current_drop(target);
	GdkDragAction action = GDK_ACTION_COPY;

	/* Nodes move, files are copied (sent). One action only. */
	if (drop != NULL &&
	    gdk_content_formats_contain_gtype(gdk_drop_get_formats(drop),
	                                      PIDGIN_TYPE_BLIST_NODE_ITEM))
		action = GDK_ACTION_MOVE;

	row_clear_drop_highlight(r);
	if (pos == DROP_BEFORE)
		gtk_widget_add_css_class(r->expander, "pidgin-blist-drop-before");
	else if (pos == DROP_AFTER)
		gtk_widget_add_css_class(r->expander, "pidgin-blist-drop-after");
	else
		gtk_widget_add_css_class(r->expander, "pidgin-blist-drop-into");
	return action;
}

static void
row_drop_leave_cb(GtkDropTarget *target, BlistRow *r)
{
	row_clear_drop_highlight(r);
}

static gboolean
row_drop_cb(GtkDropTarget *target, const GValue *value, double x, double y, BlistRow *r)
{
	PurpleBlistNode *node = row_node(r);

	row_clear_drop_highlight(r);
	if (node == NULL)
		return FALSE;

	if (G_VALUE_HOLDS(value, PIDGIN_TYPE_BLIST_NODE_ITEM)) {
		PidginBlistNodeItem *dragged = g_value_get_object(value);
		PurpleBlistNode *n;

		/* Only nodes that are still in the list. */
		if (dragged == NULL ||
		    (n = pidgin_blist_node_item_get_node(dragged)) == NULL ||
		    lookup_item(n) != dragged)
			return FALSE;
		move_node(n, node, drop_position(r->expander, y));
		return TRUE;
	}

	if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
		GSList *files, *l;

		if (node_buddy(node) == NULL)
			return FALSE;
		files = gdk_file_list_get_files(g_value_get_boxed(value));
		for (l = files; l != NULL; l = l->next)
			dnd_file_manage(node, l->data);
		g_slist_free(files);
		return files != NULL;
	}

	if (G_VALUE_HOLDS_STRING(value)) {
		/* text/uri-list from an app that does not offer a file list. */
		char **uris = g_uri_list_extract_uris(g_value_get_string(value));
		gboolean handled = FALSE;
		int i;

		if (node_buddy(node) != NULL) {
			for (i = 0; uris && uris[i]; i++) {
				if (g_str_has_prefix(uris[i], "file:")) {
					GFile *file = g_file_new_for_uri(uris[i]);
					dnd_file_manage(node, file);
					g_object_unref(file);
					handled = TRUE;
				}
			}
		}
		g_strfreev(uris);
		return handled;
	}

	return FALSE;
}

/**************************************************************************
 * The list item factory
 **************************************************************************/

static GtkWidget *
row_image(const char *css_class)
{
	GtkWidget *image = gtk_image_new();

	gtk_widget_add_css_class(image, css_class);
	return image;
}

static void
factory_setup_cb(GtkSignalListItemFactory *factory, GObject *object, gpointer data)
{
	GtkListItem *li = GTK_LIST_ITEM(object);
	BlistRow *r = g_new0(BlistRow, 1);
	GtkWidget *box, *vbox;
	GtkGesture *gesture;
	GtkDragSource *source;
	GtkDropTarget *target;
	GType drop_types[] = { PIDGIN_TYPE_BLIST_NODE_ITEM, GDK_TYPE_FILE_LIST, G_TYPE_STRING };

	r->list_item = li;
	r->bindings = g_ptr_array_new();

	r->expander = gtk_tree_expander_new();
	gtk_tree_expander_set_indent_for_icon(GTK_TREE_EXPANDER(r->expander), TRUE);
	gtk_widget_add_css_class(r->expander, "pidgin-blist-row");

	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	gtk_tree_expander_set_child(GTK_TREE_EXPANDER(r->expander), box);

	r->status = row_image("pidgin-blist-status-icon");
	gtk_box_append(GTK_BOX(box), r->status);

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_hexpand(vbox, TRUE);
	gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(box), vbox);

	r->name = gtk_label_new(NULL);
	gtk_label_set_use_markup(GTK_LABEL(r->name), TRUE);
	gtk_label_set_xalign(GTK_LABEL(r->name), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(r->name), PANGO_ELLIPSIZE_END);
	gtk_widget_add_css_class(r->name, "pidgin-blist-name");
	gtk_box_append(GTK_BOX(vbox), r->name);

	r->secondary = gtk_label_new(NULL);
	gtk_label_set_use_markup(GTK_LABEL(r->secondary), TRUE);
	gtk_label_set_xalign(GTK_LABEL(r->secondary), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(r->secondary), PANGO_ELLIPSIZE_END);
	gtk_widget_add_css_class(r->secondary, "pidgin-blist-status-text");
	gtk_box_append(GTK_BOX(vbox), r->secondary);

	r->idle = gtk_label_new(NULL);
	gtk_widget_add_css_class(r->idle, "pidgin-blist-idle-time");
	gtk_box_append(GTK_BOX(box), r->idle);

	r->emblem = row_image("pidgin-blist-emblem");
	gtk_box_append(GTK_BOX(box), r->emblem);

	r->protocol = row_image("pidgin-blist-protocol-icon");
	gtk_box_append(GTK_BOX(box), r->protocol);

	r->avatar = gtk_picture_new();
	gtk_picture_set_content_fit(GTK_PICTURE(r->avatar), GTK_CONTENT_FIT_CONTAIN);
	gtk_picture_set_can_shrink(GTK_PICTURE(r->avatar), TRUE);
	gtk_widget_set_size_request(r->avatar, 32, 32);
	gtk_widget_set_valign(r->avatar, GTK_ALIGN_CENTER);
	gtk_widget_add_css_class(r->avatar, "pidgin-blist-avatar");
	gtk_box_append(GTK_BOX(box), r->avatar);

	/* Context menu: right click, long press (the Menu key is a shortcut
	 * on the list view). */
	gesture = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), GDK_BUTTON_SECONDARY);
	g_signal_connect(gesture, "pressed", G_CALLBACK(row_right_click_cb), r);
	gtk_widget_add_controller(r->expander, GTK_EVENT_CONTROLLER(gesture));

	gesture = gtk_gesture_long_press_new();
	gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(gesture), TRUE);
	g_signal_connect(gesture, "pressed", G_CALLBACK(row_long_press_cb), r);
	gtk_widget_add_controller(r->expander, GTK_EVENT_CONTROLLER(gesture));

	gtk_widget_set_has_tooltip(r->expander, TRUE);
	g_signal_connect(r->expander, "query-tooltip", G_CALLBACK(row_query_tooltip_cb), r);

	source = gtk_drag_source_new();
	gtk_drag_source_set_actions(source, GDK_ACTION_MOVE | GDK_ACTION_COPY);
	g_signal_connect(source, "prepare", G_CALLBACK(row_drag_prepare_cb), r);
	g_signal_connect(source, "drag-begin", G_CALLBACK(row_drag_begin_cb), r);
	gtk_widget_add_controller(r->expander, GTK_EVENT_CONTROLLER(source));

	target = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_MOVE | GDK_ACTION_COPY);
	gtk_drop_target_set_gtypes(target, drop_types, G_N_ELEMENTS(drop_types));
	g_signal_connect(target, "motion", G_CALLBACK(row_drop_motion_cb), r);
	g_signal_connect(target, "leave", G_CALLBACK(row_drop_leave_cb), r);
	g_signal_connect(target, "drop", G_CALLBACK(row_drop_cb), r);
	gtk_widget_add_controller(r->expander, GTK_EVENT_CONTROLLER(target));

	g_object_set_data_full(G_OBJECT(li), ROW_KEY, r, g_free);
	gtk_list_item_set_child(li, r->expander);
}

static void
factory_bind_cb(GtkSignalListItemFactory *factory, GObject *object, gpointer data)
{
	GtkListItem *li = GTK_LIST_ITEM(object);
	BlistRow *r = g_object_get_data(G_OBJECT(li), ROW_KEY);
	GtkTreeListRow *row = gtk_list_item_get_item(li);
	PurpleBlistNode *node;

	r->row = row;
	r->item = gtk_tree_list_row_get_item(row);
	node = pidgin_blist_node_item_get_node(r->item);
	gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(r->expander), row);

	bind_prop(r, "name", r->name, "label");
	bind_prop(r, "secondary", r->secondary, "label");
	bind_visible(r, "secondary", r->secondary, TRUE);
	bind_prop(r, "idle", r->idle, "label");
	bind_visible(r, "idle", r->idle, TRUE);
	bind_prop(r, "status-icon", r->status, "gicon");
	bind_visible(r, "status-icon", r->status, FALSE);
	bind_prop(r, "emblem", r->emblem, "gicon");
	bind_visible(r, "emblem", r->emblem, FALSE);
	bind_prop(r, "protocol-icon", r->protocol, "gicon");
	bind_visible(r, "protocol-icon", r->protocol, FALSE);
	bind_prop(r, "buddy-icon", r->avatar, "paintable");

	/* Buddy icons: the column is there for every buddy, contact and chat
	 * with "Buddy Details" on (Pidgin 2's biglist), and for groups that
	 * have a custom icon. */
	if (PURPLE_BLIST_NODE_IS_GROUP(node))
		bind_visible(r, "buddy-icon", r->avatar, FALSE);
	else
		gtk_widget_set_visible(r->avatar, gtkblist->biglist);

	if (PURPLE_BLIST_NODE_IS_GROUP(node))
		gtk_widget_add_css_class(r->expander, "pidgin-blist-group-row");

	r->style_id = g_signal_connect(r->item, "notify::style",
	                               G_CALLBACK(row_style_changed_cb), r);
	row_update_style(r);
	r->expanded_id = g_signal_connect(row, "notify::expanded",
	                                  G_CALLBACK(row_expanded_cb), r);
}

static void
factory_unbind_cb(GtkSignalListItemFactory *factory, GObject *object, gpointer data)
{
	GtkListItem *li = GTK_LIST_ITEM(object);
	BlistRow *r = g_object_get_data(G_OBJECT(li), ROW_KEY);
	guint i;

	for (i = 0; i < r->bindings->len; i++)
		g_binding_unbind(g_ptr_array_index(r->bindings, i));
	g_ptr_array_set_size(r->bindings, 0);

	if (r->style_id != 0)
		g_signal_handler_disconnect(r->item, r->style_id);
	r->style_id = 0;
	if (r->expanded_id != 0 && r->row != NULL)
		g_signal_handler_disconnect(r->row, r->expanded_id);
	r->expanded_id = 0;

	row_clear_drop_highlight(r);
	gtk_widget_remove_css_class(r->expander, "pidgin-blist-group-row");
	if (r->style != NULL) {
		gtk_widget_remove_css_class(r->expander, r->style);
		g_clear_pointer(&r->style, g_free);
	}
	gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(r->expander), NULL);
	g_clear_object(&r->item);
	r->row = NULL;
}

static void
factory_teardown_cb(GtkSignalListItemFactory *factory, GObject *object, gpointer data)
{
	BlistRow *r = g_object_get_data(object, ROW_KEY);

	if (r != NULL) {
		g_ptr_array_unref(r->bindings);
		r->bindings = NULL;
		g_free(r->style);
		r->style = NULL;
	}
}

/* Rebinds every row (display prefs that change the row layout). */
static void
rebind_rows(void)
{
	if (gtkblist == NULL || gtkblist->list_view == NULL)
		return;
	gtk_list_view_set_factory(GTK_LIST_VIEW(gtkblist->list_view), NULL);
	gtk_list_view_set_factory(GTK_LIST_VIEW(gtkblist->list_view), gtkblist->factory);
}

/**************************************************************************
 * Activation and keyboard
 **************************************************************************/

static void
gtk_blist_join_chat(PurpleChat *chat)
{
	PurpleAccount *account = purple_chat_get_account(chat);
	PurplePlugin *prpl = purple_find_prpl(purple_account_get_protocol_id(account));
	PurplePluginProtocolInfo *prpl_info = prpl ? PURPLE_PLUGIN_PROTOCOL_INFO(prpl) : NULL;
	GHashTable *components = purple_chat_get_components(chat);
	PurpleConversation *conv;
	char *chat_name = NULL;

	if (prpl_info && prpl_info->get_chat_name)
		chat_name = prpl_info->get_chat_name(components);

	conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT,
		chat_name ? chat_name : purple_chat_get_name(chat), account);

	/* TODO(M4): pidgin_conv_attach_to_conversation(conv). */
	if (conv != NULL)
		purple_conversation_present(conv);

	if (purple_account_get_connection(account) != NULL)
		serv_join_chat(purple_account_get_connection(account), components);
	g_free(chat_name);
}

static void
activate_node(PurpleBlistNode *node)
{
	if (PURPLE_BLIST_NODE_IS_CONTACT(node) || PURPLE_BLIST_NODE_IS_BUDDY(node)) {
		PurpleBuddy *buddy = node_buddy(node);

		if (buddy != NULL)
			pidgin_dialogs_im_with_user(purple_buddy_get_account(buddy),
			                            purple_buddy_get_name(buddy));
	} else if (PURPLE_BLIST_NODE_IS_CHAT(node)) {
		gtk_blist_join_chat((PurpleChat *)node);
	} else if (PURPLE_BLIST_NODE_IS_GROUP(node)) {
		PidginBlistNodeItem *item = lookup_item(node);

		if (item != NULL)
			expand_node(node, !pidgin_blist_node_item_get_expanded(item));
	}
}

static PurpleBlistNode *
node_at_position(guint pos)
{
	GtkTreeListRow *row;
	PidginBlistNodeItem *item;
	PurpleBlistNode *node;

	row = g_list_model_get_item(G_LIST_MODEL(gtkblist->tree), pos);
	if (row == NULL)
		return NULL;
	item = gtk_tree_list_row_get_item(row);
	node = pidgin_blist_node_item_get_node(item);
	g_object_unref(item);
	g_object_unref(row);
	return node;
}

static PurpleBlistNode *
selected_node(void)
{
	guint pos;

	if (gtkblist == NULL)
		return NULL;
	pos = gtk_single_selection_get_selected(gtkblist->selection);
	if (pos == GTK_INVALID_LIST_POSITION)
		return NULL;
	return node_at_position(pos);
}

static void
list_activate_cb(GtkListView *view, guint position, gpointer data)
{
	PurpleBlistNode *node = node_at_position(position);

	if (node != NULL)
		activate_node(node);
}

static gboolean
menu_key_cb(GtkWidget *widget, GVariant *args, gpointer data)
{
	PurpleBlistNode *node = selected_node();

	if (node == NULL)
		return FALSE;
	show_node_menu(node, gtkblist->list_view, -1, -1);
	return TRUE;
}

static gboolean
info_key_cb(GtkWidget *widget, GVariant *args, gpointer data)
{
	PurpleBlistNode *node = selected_node();
	PurpleBuddy *buddy = node ? node_buddy(node) : NULL;
	PurpleConnection *gc;

	if (buddy == NULL)
		return FALSE;
	gc = purple_account_get_connection(purple_buddy_get_account(buddy));
	if (gc != NULL)
		serv_get_info(gc, purple_buddy_get_name(buddy));
	return TRUE;
}

static void alias_node(PurpleBlistNode *node);

static gboolean
alias_key_cb(GtkWidget *widget, GVariant *args, gpointer data)
{
	PurpleBlistNode *node = selected_node();

	if (node == NULL)
		return FALSE;
	alias_node(node);
	return TRUE;
}

/**************************************************************************
 * Context menus (Pidgin 2's create_*_menu, pidgin_blist_make_buddy_menu)
 **************************************************************************/

typedef void (*NodeActionFunc)(PurpleBlistNode *node);

typedef struct {
	NodeActionFunc func;
	PurpleBlistNode *node;
	char *arg;
} NodeAction;

static void
node_action_free(NodeAction *na)
{
	g_free(na->arg);
	g_free(na);
}

static void
node_action_activate_cb(GSimpleAction *action, GVariant *param, NodeAction *na)
{
	/* The context menu closes when its node goes away, but an item can
	 * be about another node (a contact's other buddies, a group), so
	 * only act on nodes the model still has. */
	if (gtkblist == NULL || lookup_item(na->node) == NULL)
		return;
	na->func(na->node);
}

static char *
new_menu_action(GSimpleActionGroup *group, NodeActionFunc func,
                PurpleBlistNode *node, const char *arg, GVariant *state)
{
	static guint counter = 0;
	GSimpleAction *action;
	NodeAction *na;
	char *name = g_strdup_printf("blist-%u", counter++);
	char *detailed;

	na = g_new0(NodeAction, 1);
	na->func = func;
	na->node = node;
	na->arg = g_strdup(arg);

	if (state != NULL)
		action = g_simple_action_new_stateful(name, NULL, state);
	else
		action = g_simple_action_new(name, NULL);
	if (func != NULL)
		g_signal_connect(action, "activate", G_CALLBACK(node_action_activate_cb), na);
	else
		g_simple_action_set_enabled(action, FALSE);
	g_object_set_data_full(G_OBJECT(action), "pidgin-node-action", na,
	                       (GDestroyNotify)node_action_free);
	g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
	g_object_unref(action);

	detailed = g_strdup_printf("node.%s", name);
	g_free(name);
	return detailed;
}

static void
menu_add(GMenu *section, GSimpleActionGroup *group, const char *label,
         NodeActionFunc func, PurpleBlistNode *node)
{
	char *action = new_menu_action(group, func, node, NULL, NULL);

	g_menu_append(section, label, action);
	g_free(action);
}

static void
menu_add_disabled(GMenu *section, GSimpleActionGroup *group, const char *label)
{
	char *action = new_menu_action(group, NULL, NULL, NULL, NULL);

	g_menu_append(section, label, action);
	g_free(action);
}

static void
menu_add_check(GMenu *section, GSimpleActionGroup *group, const char *label,
               NodeActionFunc func, PurpleBlistNode *node, gboolean active)
{
	char *action = new_menu_action(group, func, node, NULL,
	                               g_variant_new_boolean(active));

	g_menu_append(section, label, action);
	g_free(action);
}

/* Callbacks */

static void
menu_info_cb(PurpleBlistNode *node)
{
	PurpleBuddy *b = (PurpleBuddy *)node;
	PurpleConnection *gc = purple_account_get_connection(purple_buddy_get_account(b));

	if (gc != NULL)
		serv_get_info(gc, purple_buddy_get_name(b));
}

static void
menu_im_cb(PurpleBlistNode *node)
{
	PurpleBuddy *b = (PurpleBuddy *)node;

	pidgin_dialogs_im_with_user(purple_buddy_get_account(b), purple_buddy_get_name(b));
}

static void
menu_send_file_cb(PurpleBlistNode *node)
{
	PurpleBuddy *b = (PurpleBuddy *)node;
	PurpleConnection *gc = purple_account_get_connection(purple_buddy_get_account(b));

	if (gc != NULL)
		serv_send_file(gc, purple_buddy_get_name(b), NULL);
}

static void
menu_pounce_cb(PurpleBlistNode *node)
{
	todo_m5("the buddy pounce editor");
}

static void
menu_showlog_cb(PurpleBlistNode *node)
{
	todo_m5("the log viewer");
}

static void
menu_showoffline_cb(PurpleBlistNode *node)
{
	gboolean setting = !purple_blist_node_get_bool(node, "show_offline");
	PurpleBlistNode *cnode, *bnode;

	purple_blist_node_set_bool(node, "show_offline", setting);

	if (PURPLE_BLIST_NODE_IS_BUDDY(node)) {
		pidgin_blist_model_update(gtkblist->model, node);
	} else if (PURPLE_BLIST_NODE_IS_CONTACT(node)) {
		for (bnode = purple_blist_node_get_first_child(node); bnode != NULL;
		     bnode = purple_blist_node_get_sibling_next(bnode)) {
			purple_blist_node_set_bool(bnode, "show_offline", setting);
			pidgin_blist_model_update(gtkblist->model, bnode);
		}
	} else if (PURPLE_BLIST_NODE_IS_GROUP(node)) {
		for (cnode = purple_blist_node_get_first_child(node); cnode != NULL;
		     cnode = purple_blist_node_get_sibling_next(cnode)) {
			purple_blist_node_set_bool(cnode, "show_offline", setting);
			for (bnode = purple_blist_node_get_first_child(cnode); bnode != NULL;
			     bnode = purple_blist_node_get_sibling_next(bnode)) {
				purple_blist_node_set_bool(bnode, "show_offline", setting);
				pidgin_blist_model_update(gtkblist->model, bnode);
			}
		}
	}
}

static void
menu_toggle_privacy_cb(PurpleBlistNode *node)
{
	PurpleBuddy *buddy = (PurpleBuddy *)node;
	PurpleAccount *account = purple_buddy_get_account(buddy);
	const char *name = purple_buddy_get_name(buddy);

	if (purple_privacy_check(account, name))
		purple_privacy_deny(account, name, FALSE, FALSE);
	else
		purple_privacy_allow(account, name, FALSE, FALSE);

	pidgin_blist_model_update(gtkblist->model, node);
}

static void
alias_node(PurpleBlistNode *node)
{
	if (PURPLE_BLIST_NODE_IS_CONTACT(node)) {
		PurpleContact *contact = (PurpleContact *)node;

		/* As Pidgin 2: a contact without an alias shown collapsed is
		 * renamed through its buddy. */
		if (contact->alias != NULL || contact_is_expanded(node))
			pidgin_dialogs_alias_contact(contact);
		else if (purple_contact_get_priority_buddy(contact) != NULL)
			pidgin_dialogs_alias_buddy(purple_contact_get_priority_buddy(contact));
	} else if (PURPLE_BLIST_NODE_IS_BUDDY(node)) {
		pidgin_dialogs_alias_buddy((PurpleBuddy *)node);
	} else if (PURPLE_BLIST_NODE_IS_CHAT(node)) {
		pidgin_dialogs_alias_chat((PurpleChat *)node);
	} else if (PURPLE_BLIST_NODE_IS_GROUP(node)) {
		pidgin_dialogs_rename_group((PurpleGroup *)node);
	}
}

static void
menu_remove_cb(PurpleBlistNode *node)
{
	if (PURPLE_BLIST_NODE_IS_BUDDY(node))
		pidgin_dialogs_remove_buddy((PurpleBuddy *)node);
	else if (PURPLE_BLIST_NODE_IS_CHAT(node))
		pidgin_dialogs_remove_chat((PurpleChat *)node);
	else if (PURPLE_BLIST_NODE_IS_GROUP(node))
		pidgin_dialogs_remove_group((PurpleGroup *)node);
	else if (PURPLE_BLIST_NODE_IS_CONTACT(node))
		pidgin_dialogs_remove_contact((PurpleContact *)node);
}

static void
menu_expand_cb(PurpleBlistNode *node)
{
	expand_node(node, TRUE);
}

static void
menu_collapse_cb(PurpleBlistNode *node)
{
	expand_node(node, FALSE);
}

static void
custom_icon_chosen_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	PurpleBlistNode *node = data;
	GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
	char *path;

	if (file == NULL)
		return;
	path = g_file_get_path(file);
	/* Only if the node still exists. */
	if (path != NULL && gtkblist != NULL && lookup_item(node) != NULL)
		purple_buddy_icons_node_set_custom_icon_from_file(node, path);
	g_free(path);
	g_object_unref(file);
}

static GtkFileFilter *
image_filter(void)
{
	GtkFileFilter *filter = gtk_file_filter_new();

	gtk_file_filter_set_name(filter, _("Images"));
	gtk_file_filter_add_mime_type(filter, "image/*");
	return filter;
}

static void
menu_set_custom_icon_cb(PurpleBlistNode *node)
{
	GtkFileDialog *dialog = gtk_file_dialog_new();
	GtkFileFilter *filter = image_filter();

	gtk_file_dialog_set_title(dialog, _("Buddy Icon"));
	gtk_file_dialog_set_default_filter(dialog, filter);
	gtk_file_dialog_open(dialog, GTK_WINDOW(gtkblist->window), NULL,
	                     custom_icon_chosen_cb, node);
	g_object_unref(filter);
	g_object_unref(dialog);
}

static void
menu_remove_custom_icon_cb(PurpleBlistNode *node)
{
	purple_buddy_icons_node_set_custom_icon(node, NULL, 0);
}

static void
menu_join_cb(PurpleBlistNode *node)
{
	gtk_blist_join_chat((PurpleChat *)node);
}

static void
menu_autojoin_cb(PurpleBlistNode *node)
{
	purple_blist_node_set_bool(node, "gtk-autojoin",
		!purple_blist_node_get_bool(node, "gtk-autojoin"));
}

static void
menu_persistent_cb(PurpleBlistNode *node)
{
	purple_blist_node_set_bool(node, "gtk-persistent",
		!purple_blist_node_get_bool(node, "gtk-persistent"));
}

static void
chat_components_edit_ok(PurpleChat *chat, PurpleRequestFields *allfields)
{
	GList *groups, *fields;

	for (groups = purple_request_fields_get_groups(allfields); groups; groups = groups->next) {
		fields = purple_request_field_group_get_fields(groups->data);
		for (; fields; fields = fields->next) {
			PurpleRequestField *field = fields->data;
			const char *id;
			char *val;

			id = purple_request_field_get_id(field);
			if (purple_request_field_get_type(field) == PURPLE_REQUEST_FIELD_INTEGER)
				val = g_strdup_printf("%d", purple_request_field_int_get_value(field));
			else
				val = g_strdup(purple_request_field_string_get_value(field));

			if (!val)
				g_hash_table_remove(purple_chat_get_components(chat), id);
			else
				g_hash_table_replace(purple_chat_get_components(chat), g_strdup(id), val);
		}
	}
}

static void
menu_chat_edit_cb(PurpleBlistNode *node)
{
	PurpleChat *chat = (PurpleChat *)node;
	PurpleConnection *gc = purple_account_get_connection(purple_chat_get_account(chat));
	PurplePluginProtocolInfo *prpl_info;
	PurpleRequestFields *fields;
	PurpleRequestFieldGroup *group;
	GList *parts, *iter;

	if (gc == NULL)
		return;
	prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(purple_connection_get_prpl(gc));
	if (prpl_info->chat_info == NULL)
		return;

	fields = purple_request_fields_new();
	group = purple_request_field_group_new(NULL);
	purple_request_fields_add_group(fields, group);

	parts = prpl_info->chat_info(gc);
	for (iter = parts; iter; iter = iter->next) {
		struct proto_chat_entry *pce = iter->data;
		PurpleRequestField *field;

		if (pce->is_int) {
			int val;
			const char *str = g_hash_table_lookup(purple_chat_get_components(chat), pce->identifier);
			if (!str || sscanf(str, "%d", &val) != 1)
				val = pce->min;
			field = purple_request_field_int_new(pce->identifier, pce->label, val);
		} else {
			field = purple_request_field_string_new(pce->identifier, pce->label,
				g_hash_table_lookup(purple_chat_get_components(chat), pce->identifier), FALSE);
			if (pce->secret)
				purple_request_field_string_set_masked(field, TRUE);
		}
		if (pce->required)
			purple_request_field_set_required(field, TRUE);
		purple_request_field_group_add_field(group, field);
		g_free(pce);
	}
	g_list_free(parts);

	purple_request_fields(node, _("Edit Chat"), NULL, _("Please update the necessary fields."),
			fields, _("Save"), G_CALLBACK(chat_components_edit_ok), _("Cancel"), NULL,
			NULL, NULL, NULL,
			chat);
}

static void
menu_add_buddy_cb(PurpleBlistNode *node)
{
	add_buddy_for_node(node);
}

static void
menu_add_chat_cb(PurpleBlistNode *node)
{
	add_chat_for_node(node);
}

static void
menu_move_to_cb(PurpleBlistNode *node)
{
	/* The target group name is the action's argument; look it up now. */
	(void)node;
}

static void
move_to_action_cb(GSimpleAction *action, GVariant *param, NodeAction *na)
{
	PurpleGroup *group = purple_find_group(na->arg);

	if (group != NULL && gtkblist != NULL && lookup_item(na->node) != NULL)
		purple_blist_add_contact((PurpleContact *)na->node, group, NULL);
}

/* The prpl's blist_node_menu and the extended menu (plugins, via the
 * "blist-node-extended-menu" signal), converted by pidginmenu. */
static void
append_extended_menus(GMenu *menu, GSimpleActionGroup *group,
                      PurpleBlistNode *node, PurpleAccount *account)
{
	PurplePluginProtocolInfo *prpl_info = account ? connected_prpl_info(account) : NULL;

	if (prpl_info != NULL && prpl_info->blist_node_menu != NULL)
		pidgin_menu_append_menu_actions(menu, prpl_info->blist_node_menu(node),
		                                node, group, "node");
	pidgin_menu_append_menu_actions(menu, purple_blist_node_get_extended_menu(node),
	                                node, group, "node");
}

static void
append_move_to_menu(GMenu *section, GSimpleActionGroup *group, PurpleBlistNode *node)
{
	GMenu *submenu = g_menu_new();
	PurpleBlistNode *gnode;

	for (gnode = purple_blist_get_root(); gnode != NULL;
	     gnode = purple_blist_node_get_sibling_next(gnode)) {
		const char *name;
		char *action, *label;
		GAction *gaction;

		if (!PURPLE_BLIST_NODE_IS_GROUP(gnode) || gnode == purple_blist_node_get_parent(node))
			continue;

		name = purple_group_get_name((PurpleGroup *)gnode);
		action = new_menu_action(group, menu_move_to_cb, node, name, NULL);
		/* Replace the generic handler: this one needs the group name. */
		gaction = g_action_map_lookup_action(G_ACTION_MAP(group), action + strlen("node."));
		g_signal_handlers_disconnect_matched(gaction, G_SIGNAL_MATCH_FUNC, 0, 0,
		                                     NULL, node_action_activate_cb, NULL);
		g_signal_connect(gaction, "activate", G_CALLBACK(move_to_action_cb),
		                 g_object_get_data(G_OBJECT(gaction), "pidgin-node-action"));
		label = pidgin_menu_escape_label(name);
		g_menu_append(submenu, label, action);
		g_free(label);
		g_free(action);
	}
	g_menu_append_submenu(section, _("Move to"), G_MENU_MODEL(submenu));
	g_object_unref(submenu);
}

static void
append_custom_icon_items(GMenu *section, GSimpleActionGroup *group, PurpleBlistNode *node)
{
	menu_add(section, group, _("Set Custom Icon"), menu_set_custom_icon_cb, node);
	if (purple_buddy_icons_node_has_custom_icon(node))
		menu_add(section, group, _("Remove Custom Icon"), menu_remove_custom_icon_cb, node);
	else
		menu_add_disabled(section, group, _("Remove Custom Icon"));
}

static void
make_buddy_menu(GMenu *menu, GSimpleActionGroup *group, PurpleBuddy *buddy, gboolean sub)
{
	PurpleAccount *account = purple_buddy_get_account(buddy);
	PurplePluginProtocolInfo *prpl_info = connected_prpl_info(account);
	PurpleBlistNode *node = (PurpleBlistNode *)buddy;
	PurpleContact *contact = purple_buddy_get_contact(buddy);
	PurpleBlistNode *cnode = (PurpleBlistNode *)contact;
	gboolean contact_expanded = contact != NULL && contact_is_expanded(cnode);
	gboolean several = node->parent && node->parent->child && node->parent->child->next;
	GMenu *section = g_menu_new();

	if (prpl_info && prpl_info->get_info)
		menu_add(section, group, _("Get _Info"), menu_info_cb, node);
	menu_add(section, group, _("I_M"), menu_im_cb, node);

	if (prpl_info && prpl_info->send_file &&
	    (!prpl_info->can_receive_file ||
	     prpl_info->can_receive_file(purple_account_get_connection(account), buddy->name)))
		menu_add(section, group, _("_Send File..."), menu_send_file_cb, node);

	menu_add(section, group, _("Add Buddy _Pounce..."), menu_pounce_cb, node);

	if (several && !sub && !contact_expanded)
		menu_add(section, group, _("View _Log"), menu_showlog_cb, cnode);
	else if (!sub)
		menu_add(section, group, _("View _Log"), menu_showlog_cb, node);

	if (!PURPLE_BLIST_NODE_HAS_FLAG(node, PURPLE_BLIST_NODE_FLAG_NO_SAVE))
		menu_add(section, group,
			purple_blist_node_get_bool(node, "show_offline")
				? _("Hide When Offline") : _("Show When Offline"),
			menu_showoffline_cb, node);

	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);

	append_extended_menus(menu, group, node, account);

	if (!contact_expanded && contact != NULL) {
		section = g_menu_new();
		append_move_to_menu(section, group, cnode);
		g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
		g_object_unref(section);
	}

	if (!sub || contact_expanded) {
		PurpleBlistNode *target = (several && !sub && !contact_expanded) ? cnode : node;

		section = g_menu_new();
		menu_add(section, group,
			purple_privacy_check(account, purple_buddy_get_name(buddy))
				? _("_Block") : _("Un_block"),
			menu_toggle_privacy_cb, node);
		menu_add(section, group, _("_Alias..."), alias_node, target);
		menu_add(section, group, _("_Remove"), menu_remove_cb, target);
		g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
		g_object_unref(section);
	}
}

static void
make_group_menu(GMenu *menu, GSimpleActionGroup *group, PurpleBlistNode *node)
{
	GMenu *section = g_menu_new();

	if (purple_connections_get_all() != NULL)
		menu_add(section, group, _("Add _Buddy..."), menu_add_buddy_cb, node);
	else
		menu_add_disabled(section, group, _("Add _Buddy..."));
	if (pidgin_blist_joinchat_is_showable())
		menu_add(section, group, _("Add C_hat..."), menu_add_chat_cb, node);
	else
		menu_add_disabled(section, group, _("Add C_hat..."));
	menu_add(section, group, _("_Delete Group"), menu_remove_cb, node);
	menu_add(section, group, _("_Rename"), alias_node, node);
	if (!(purple_blist_node_get_flags(node) & PURPLE_BLIST_NODE_FLAG_NO_SAVE))
		menu_add(section, group,
			purple_blist_node_get_bool(node, "show_offline")
				? _("Hide When Offline") : _("Show When Offline"),
			menu_showoffline_cb, node);
	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);

	section = g_menu_new();
	append_custom_icon_items(section, group, node);
	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);

	append_extended_menus(menu, group, node, NULL);
}

static void
make_chat_menu(GMenu *menu, GSimpleActionGroup *group, PurpleBlistNode *node)
{
	PurpleChat *chat = (PurpleChat *)node;
	GMenu *section = g_menu_new();

	menu_add(section, group, _("_Join"), menu_join_cb, node);
	menu_add_check(section, group, _("Auto-Join"), menu_autojoin_cb, node,
	               purple_blist_node_get_bool(node, "gtk-autojoin"));
	menu_add_check(section, group, _("Persistent"), menu_persistent_cb, node,
	               purple_blist_node_get_bool(node, "gtk-persistent"));
	menu_add(section, group, _("View _Log"), menu_showlog_cb, node);
	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);

	append_extended_menus(menu, group, node, purple_chat_get_account(chat));

	section = g_menu_new();
	if (purple_account_get_connection(purple_chat_get_account(chat)) != NULL)
		menu_add(section, group, _("_Edit Settings..."), menu_chat_edit_cb, node);
	else
		menu_add_disabled(section, group, _("_Edit Settings..."));
	menu_add(section, group, _("_Alias..."), alias_node, node);
	menu_add(section, group, _("_Remove"), menu_remove_cb, node);
	append_custom_icon_items(section, group, node);
	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);
}

static void
make_contact_menu(GMenu *menu, GSimpleActionGroup *group, PurpleBlistNode *node)
{
	GMenu *section = g_menu_new();

	menu_add(section, group, _("View _Log"), menu_showlog_cb, node);
	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);

	section = g_menu_new();
	menu_add(section, group, _("_Alias..."), alias_node, node);
	menu_add(section, group, _("_Remove"), menu_remove_cb, node);
	append_custom_icon_items(section, group, node);
	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);

	section = g_menu_new();
	menu_add(section, group, _("_Collapse"), menu_collapse_cb, node);
	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);

	append_extended_menus(menu, group, node, NULL);
}

void
pidgin_blist_build_node_menu(PurpleBlistNode *node, GMenu *menu, GSimpleActionGroup *group)
{
	g_return_if_fail(node != NULL);
	g_return_if_fail(G_IS_MENU(menu));
	g_return_if_fail(G_IS_SIMPLE_ACTION_GROUP(group));

	if (PURPLE_BLIST_NODE_IS_GROUP(node)) {
		make_group_menu(menu, group, node);
	} else if (PURPLE_BLIST_NODE_IS_CHAT(node)) {
		make_chat_menu(menu, group, node);
	} else if (PURPLE_BLIST_NODE_IS_CONTACT(node) && contact_is_expanded(node)) {
		make_contact_menu(menu, group, node);
	} else if (PURPLE_BLIST_NODE_IS_CONTACT(node) || PURPLE_BLIST_NODE_IS_BUDDY(node)) {
		PurpleBuddy *b = node_buddy(node);
		gboolean show_offline = purple_prefs_get_bool(BLIST_PREFS "/show_offline_buddies");

		if (b == NULL)
			return;
		make_buddy_menu(menu, group, b, FALSE);

		if (PURPLE_BLIST_NODE_IS_CONTACT(node)) {
			GMenu *section = g_menu_new();
			PidginBlistNodeItem *item = lookup_item(node);
			PurpleBlistNode *bnode;

			append_custom_icon_items(section, group, node);
			if (item != NULL && pidgin_blist_node_item_get_expandable(item)) {
				if (contact_is_expanded(node))
					menu_add(section, group, _("_Collapse"), menu_collapse_cb, node);
				else
					menu_add(section, group, _("_Expand"), menu_expand_cb, node);
			}

			/* A submenu for each of the contact's other buddies. */
			for (bnode = purple_blist_node_get_first_child(node); bnode != NULL;
			     bnode = purple_blist_node_get_sibling_next(bnode)) {
				PurpleBuddy *buddy = (PurpleBuddy *)bnode;
				GMenu *submenu;
				char *label;

				if (buddy == b || !PURPLE_BLIST_NODE_IS_BUDDY(bnode))
					continue;
				if (purple_account_get_connection(purple_buddy_get_account(buddy)) == NULL)
					continue;
				if (!show_offline && !PURPLE_BUDDY_IS_ONLINE(buddy))
					continue;

				submenu = g_menu_new();
				make_buddy_menu(submenu, group, buddy, TRUE);
				label = pidgin_menu_escape_label(purple_buddy_get_name(buddy));
				g_menu_append_submenu(section, label, G_MENU_MODEL(submenu));
				g_free(label);
				g_object_unref(submenu);
			}
			g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
			g_object_unref(section);
		}
	}
}

static gboolean
unparent_popover_idle(gpointer data)
{
	GtkWidget *popover = data;

	if (gtk_widget_get_parent(popover) != NULL)
		gtk_widget_unparent(popover);
	g_object_unref(popover);
	return G_SOURCE_REMOVE;
}

static void
popover_closed_cb(GtkPopover *popover, gpointer data)
{
	if (gtkblist != NULL && gtkblist->popover == GTK_WIDGET(popover)) {
		gtkblist->popover = NULL;
		gtkblist->popover_node = NULL;
	}
	/* Activated items run after "closed"; unparent later. */
	g_idle_add(unparent_popover_idle, g_object_ref(popover));
}

static void
close_node_menu(void)
{
	if (gtkblist != NULL && gtkblist->popover != NULL)
		gtk_popover_popdown(GTK_POPOVER(gtkblist->popover));
}

static void
show_node_menu(PurpleBlistNode *node, GtkWidget *widget, double x, double y)
{
	GSimpleActionGroup *group;
	GMenu *menu;
	GtkWidget *popover;
	GdkRectangle rect;

	close_node_menu();

	menu = g_menu_new();
	group = g_simple_action_group_new();
	pidgin_blist_build_node_menu(node, menu, group);
	if (g_menu_model_get_n_items(G_MENU_MODEL(menu)) == 0) {
		g_object_unref(menu);
		g_object_unref(group);
		return;
	}

	popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
	gtk_widget_insert_action_group(popover, "node", G_ACTION_GROUP(group));
	gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
	gtk_widget_set_halign(popover, GTK_ALIGN_START);
	gtk_widget_set_parent(popover, gtkblist->vbox);

	if (x >= 0 && y >= 0) {
		graphene_point_t in = GRAPHENE_POINT_INIT(x, y), out;

		if (gtk_widget_compute_point(widget, gtkblist->vbox, &in, &out)) {
			rect.x = out.x;
			rect.y = out.y;
			rect.width = rect.height = 1;
			gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
		}
	} else {
		graphene_rect_t bounds;

		if (gtk_widget_compute_bounds(widget, gtkblist->vbox, &bounds)) {
			rect.x = bounds.origin.x + bounds.size.width / 3;
			rect.y = bounds.origin.y + bounds.size.height / 3;
			rect.width = rect.height = 1;
			gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
		}
	}

	g_signal_connect(popover, "closed", G_CALLBACK(popover_closed_cb), NULL);
	gtkblist->popover = popover;
	gtkblist->popover_node = node;
	gtk_popover_popup(GTK_POPOVER(popover));

	g_object_unref(menu);
	g_object_unref(group);
}

/**************************************************************************
 * Add buddy / add chat / join chat dialogs (Pidgin 2's
 * make_blist_request_dialog and friends)
 **************************************************************************/

/* A text entry with a menu of the existing groups next to it (the GTK 2
 * dialogs used a combo box entry). */
static void
group_chosen_cb(GtkButton *button, GtkWidget *entry)
{
	GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER);

	gtk_editable_set_text(GTK_EDITABLE(entry), gtk_button_get_label(button));
	if (popover != NULL)
		gtk_popover_popdown(GTK_POPOVER(popover));
}

static GtkWidget *
group_entry_new(const char *group_name, GtkWidget **entry_out)
{
	GtkWidget *hbox, *entry, *button, *popover, *list, *scroll;
	PurpleBlistNode *gnode;

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(hbox, "linked");

	entry = gtk_entry_new();
	gtk_widget_set_hexpand(entry, TRUE);
	gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
	if (group_name != NULL)
		gtk_editable_set_text(GTK_EDITABLE(entry), group_name);
	else if (purple_blist_get_root() != NULL)
		gtk_editable_set_text(GTK_EDITABLE(entry),
			purple_group_get_name((PurpleGroup *)purple_blist_get_root()));
	else
		gtk_editable_set_text(GTK_EDITABLE(entry), _("Buddies"));
	gtk_box_append(GTK_BOX(hbox), entry);

	list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	for (gnode = purple_blist_get_root(); gnode != NULL;
	     gnode = purple_blist_node_get_sibling_next(gnode)) {
		GtkWidget *item;

		if (!PURPLE_BLIST_NODE_IS_GROUP(gnode))
			continue;
		item = gtk_button_new_with_label(purple_group_get_name((PurpleGroup *)gnode));
		gtk_widget_add_css_class(item, "flat");
		gtk_widget_set_halign(gtk_button_get_child(GTK_BUTTON(item)), GTK_ALIGN_START);
		g_signal_connect(item, "clicked", G_CALLBACK(group_chosen_cb), entry);
		gtk_box_append(GTK_BOX(list), item);
	}
	scroll = pidgin_make_scrollable(list, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC, -1, -1);
	gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 300);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);

	popover = gtk_popover_new();
	gtk_popover_set_child(GTK_POPOVER(popover), scroll);

	button = gtk_menu_button_new();
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), popover);
	gtk_widget_set_tooltip_text(button, _("Existing groups"));
	gtk_box_append(GTK_BOX(hbox), button);

	*entry_out = entry;
	return hbox;
}

static const char *
entry_text(GtkWidget *entry)
{
	return gtk_editable_get_text(GTK_EDITABLE(entry));
}

static PurpleGroup *
find_or_add_group(const char *name)
{
	PurpleGroup *g;

	if (name == NULL || *name == '\0')
		return NULL;
	if ((g = purple_find_group(name)) == NULL) {
		g = purple_group_new(name);
		purple_blist_add_group(g, NULL);
	}
	return g;
}

/* Offer to merge contacts with the same alias (gtk_blist_auto_personize). */
static void
do_personize(GList *merges)
{
	PurpleBlistNode *contact = NULL;
	int max = 0;
	GList *tmp;

	for (tmp = merges; tmp; tmp = tmp->next) {
		PurpleBlistNode *node = tmp->data;
		PurpleBlistNode *b;
		int i = 0;

		if (PURPLE_BLIST_NODE_IS_BUDDY(node))
			node = purple_blist_node_get_parent(node);
		if (!PURPLE_BLIST_NODE_IS_CONTACT(node))
			continue;
		for (b = purple_blist_node_get_first_child(node); b; b = purple_blist_node_get_sibling_next(b))
			i++;
		if (i > max) {
			contact = node;
			max = i;
		}
	}

	if (contact != NULL) {
		for (tmp = merges; tmp; tmp = tmp->next) {
			PurpleBlistNode *node = tmp->data;

			if (PURPLE_BLIST_NODE_IS_BUDDY(node))
				node = purple_blist_node_get_parent(node);
			if (node != contact)
				purple_blist_merge_contact((PurpleContact *)node, contact);
		}
		/* Show the expanded contact, so people know what happened. */
		expand_node(contact, TRUE);
	}
	g_list_free(merges);
}

static void
auto_personize(PurpleBlistNode *group, const char *alias)
{
	PurpleBlistNode *contact, *buddy;
	GList *merges = NULL;
	int i = 0;
	char *a = g_utf8_casefold(alias, -1);

	for (contact = purple_blist_node_get_first_child(group); contact != NULL;
	     contact = purple_blist_node_get_sibling_next(contact)) {
		char *node_alias;

		if (!PURPLE_BLIST_NODE_IS_CONTACT(contact))
			continue;

		node_alias = g_utf8_casefold(purple_contact_get_alias((PurpleContact *)contact), -1);
		if (node_alias && !g_utf8_collate(node_alias, a)) {
			merges = g_list_append(merges, contact);
			i++;
			g_free(node_alias);
			continue;
		}
		g_free(node_alias);

		for (buddy = purple_blist_node_get_first_child(contact); buddy;
		     buddy = purple_blist_node_get_sibling_next(buddy)) {
			if (!PURPLE_BLIST_NODE_IS_BUDDY(buddy))
				continue;
			node_alias = g_utf8_casefold(purple_buddy_get_alias((PurpleBuddy *)buddy), -1);
			if (node_alias && !g_utf8_collate(node_alias, a)) {
				merges = g_list_append(merges, buddy);
				i++;
				g_free(node_alias);
				break;
			}
			g_free(node_alias);
		}
	}
	g_free(a);

	if (i > 1) {
		char *msg = g_strdup_printf(ngettext("You have %d contact named %s. Would you like to merge them?",
			"You currently have %d contacts named %s. Would you like to merge them?", i), i, alias);
		purple_request_action(NULL, NULL, msg,
			_("Merging these contacts will cause them to share a single entry on the buddy list "
			  "and use a single conversation window. You can separate them again by choosing "
			  "'Expand' from the contact's context menu"), 0, NULL, NULL, NULL,
			merges, 2, _("_Yes"), PURPLE_CALLBACK(do_personize), _("_No"), PURPLE_CALLBACK(g_list_free));
		g_free(msg);
	} else {
		g_list_free(merges);
	}
}

typedef struct {
	GtkWidget *window;
	GtkWidget *account_menu;
	GtkWidget *ok_button;
	GtkWidget *roomlist_button;
	GtkSizeGroup *sg;
	PurpleAccount *account;

	/* Add buddy */
	GtkWidget *entry;
	GtkWidget *alias_entry;
	GtkWidget *invite_entry;
	GtkWidget *group_entry;

	/* Chats */
	GtkWidget *chat_box;
	GList *chat_entries;
	char *default_chat_name;
	GtkWidget *autojoin;
	GtkWidget *persistent;
	gboolean join;
} BlistRequestData;

static void
blist_request_data_free(BlistRequestData *data)
{
	g_list_free(data->chat_entries);
	g_free(data->default_chat_name);
	g_clear_object(&data->sg);
	g_free(data);
}

static gboolean
add_buddy_account_filter(PurpleAccount *account)
{
	PurplePluginProtocolInfo *prpl_info = connected_prpl_info(account);

	return prpl_info != NULL && prpl_info->add_buddy != NULL;
}

static gboolean
chat_account_filter(PurpleAccount *account)
{
	PurplePluginProtocolInfo *prpl_info = connected_prpl_info(account);

	return prpl_info != NULL && prpl_info->chat_info != NULL;
}

gboolean
pidgin_blist_joinchat_is_showable(void)
{
	GList *c;

	for (c = purple_connections_get_all(); c != NULL; c = c->next) {
		if (chat_account_filter(purple_connection_get_account(c->data)))
			return TRUE;
	}
	return FALSE;
}

static GtkWidget *
make_blist_request_dialog(BlistRequestData *data, PurpleAccount *account,
                          const char *title, const char *label_text,
                          PurpleFilterAccountFunc filter_func)
{
	GtkWidget *vbox, *content;

	data->account = account;
	data->window = pidgin_dialog_new(title, GTK_WINDOW(gtkblist ? gtkblist->window : NULL),
	                                 "blist-request", FALSE);
	g_object_set_data_full(G_OBJECT(data->window), "pidgin-blist-request", data,
	                       (GDestroyNotify)blist_request_data_free);

	content = pidgin_dialog_get_content_area(data->window);
	pidgin_dialog_add_message(data->window, "dialog-question", NULL, label_text, FALSE);

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BOX_SPACE);
	gtk_box_append(GTK_BOX(content), vbox);

	data->sg = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
	data->account_menu = pidgin_account_dropdown_new(account, FALSE, filter_func, NULL);
	pidgin_add_widget_to_vbox(GTK_BOX(vbox), _("A_ccount"), data->sg,
	                          data->account_menu, TRUE, NULL);
	if (data->account == NULL)
		data->account = pidgin_account_dropdown_get_selected(data->account_menu);

	return vbox;
}

static void
close_request_cb(GtkWidget *button, BlistRequestData *data)
{
	gtk_window_destroy(GTK_WINDOW(data->window));
}

/* Add buddy */

static void
add_buddy_update_sensitivity(BlistRequestData *data)
{
	PurplePluginProtocolInfo *prpl_info = data->account ? connected_prpl_info(data->account) : NULL;

	gtk_widget_set_sensitive(data->invite_entry,
		prpl_info == NULL || (prpl_info->options & OPT_PROTO_INVITE_MESSAGE));
	gtk_widget_set_sensitive(data->ok_button,
		data->account != NULL && *entry_text(data->entry) != '\0');
}

static void
add_buddy_account_changed_cb(GObject *dropdown, GParamSpec *pspec, BlistRequestData *data)
{
	data->account = pidgin_account_dropdown_get_selected(data->account_menu);
	add_buddy_update_sensitivity(data);
}

static void
add_buddy_entry_changed_cb(GtkEditable *editable, BlistRequestData *data)
{
	add_buddy_update_sensitivity(data);
}

static void
add_buddy_ok_cb(GtkWidget *button, BlistRequestData *data)
{
	const char *grp, *who, *whoalias, *invite;
	PurpleAccount *account = data->account;
	PurpleGroup *g;
	PurpleBuddy *b;
	PurpleConversation *c;

	if (account == NULL || g_list_find(purple_accounts_get_all(), account) == NULL) {
		gtk_window_destroy(GTK_WINDOW(data->window));
		return;
	}

	who = entry_text(data->entry);
	grp = entry_text(data->group_entry);
	whoalias = entry_text(data->alias_entry);
	if (*whoalias == '\0')
		whoalias = NULL;
	invite = entry_text(data->invite_entry);
	if (*invite == '\0')
		invite = NULL;

	g = NULL;
	if (grp != NULL && *grp != '\0') {
		g = find_or_add_group(grp);
		b = purple_find_buddy_in_group(account, who, g);
	} else if ((b = purple_find_buddy(account, who)) != NULL) {
		g = purple_buddy_get_group(b);
	}

	if (b == NULL) {
		b = purple_buddy_new(account, who, whoalias);
		purple_blist_add_buddy(b, NULL, g, NULL);
	}

	purple_account_add_buddy_with_invite(account, b, invite);

	/* Offer to merge people with the same alias. */
	if (whoalias != NULL && g != NULL)
		auto_personize((PurpleBlistNode *)g, whoalias);

	c = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, who, account);
	if (c != NULL) {
		PurpleBuddyIcon *icon = purple_conv_im_get_icon(PURPLE_CONV_IM(c));
		if (icon != NULL)
			purple_buddy_icon_update(icon);
	}

	gtk_window_destroy(GTK_WINDOW(data->window));
}

static void
pidgin_blist_request_add_buddy(PurpleAccount *account, const char *username,
                               const char *group, const char *alias)
{
	BlistRequestData *data = g_new0(BlistRequestData, 1);
	GtkWidget *vbox, *box;

	if (account == NULL && purple_connections_get_all() != NULL)
		account = purple_connection_get_account(purple_connections_get_all()->data);

	vbox = make_blist_request_dialog(data, account, _("Add Buddy"),
		_("Add a buddy.\n"), add_buddy_account_filter);

	data->entry = gtk_entry_new();
	gtk_entry_set_activates_default(GTK_ENTRY(data->entry), TRUE);
	if (username != NULL)
		gtk_editable_set_text(GTK_EDITABLE(data->entry), username);
	pidgin_add_widget_to_vbox(GTK_BOX(vbox), _("Buddy's _username:"),
	                          data->sg, data->entry, TRUE, NULL);

	data->alias_entry = gtk_entry_new();
	gtk_entry_set_activates_default(GTK_ENTRY(data->alias_entry), TRUE);
	if (alias != NULL)
		gtk_editable_set_text(GTK_EDITABLE(data->alias_entry), alias);
	pidgin_add_widget_to_vbox(GTK_BOX(vbox), _("(Optional) A_lias:"),
	                          data->sg, data->alias_entry, TRUE, NULL);

	data->invite_entry = gtk_entry_new();
	gtk_entry_set_activates_default(GTK_ENTRY(data->invite_entry), TRUE);
	pidgin_add_widget_to_vbox(GTK_BOX(vbox), _("(Optional) _Invite message:"),
	                          data->sg, data->invite_entry, TRUE, NULL);

	box = group_entry_new(group, &data->group_entry);
	pidgin_add_widget_to_vbox(GTK_BOX(vbox), _("Add buddy to _group:"),
	                          data->sg, box, TRUE, NULL);

	pidgin_dialog_add_button(data->window, _("_Cancel"), G_CALLBACK(close_request_cb), data);
	data->ok_button = pidgin_dialog_add_button(data->window, _("_Add"),
	                                           G_CALLBACK(add_buddy_ok_cb), data);
	gtk_widget_add_css_class(data->ok_button, "suggested-action");
	gtk_window_set_default_widget(GTK_WINDOW(data->window), data->ok_button);

	g_signal_connect(data->account_menu, "notify::selected",
	                 G_CALLBACK(add_buddy_account_changed_cb), data);
	g_signal_connect(data->entry, "changed", G_CALLBACK(add_buddy_entry_changed_cb), data);
	add_buddy_update_sensitivity(data);

	gtk_window_present(GTK_WINDOW(data->window));
	gtk_widget_grab_focus(username != NULL ? data->alias_entry : data->entry);
}

/* Chat entries */

static void
chat_update_sensitivity(BlistRequestData *data)
{
	gboolean sensitive = data->account != NULL;
	PurplePluginProtocolInfo *prpl_info;
	GList *l;

	for (l = data->chat_entries; l != NULL; l = l->next) {
		if (!g_object_get_data(l->data, "is_spin") &&
		    GPOINTER_TO_INT(g_object_get_data(l->data, "required")) &&
		    *entry_text(l->data) == '\0')
			sensitive = FALSE;
	}
	gtk_widget_set_sensitive(data->ok_button, sensitive);

	prpl_info = data->account ? connected_prpl_info(data->account) : NULL;
	if (data->roomlist_button != NULL)
		gtk_widget_set_sensitive(data->roomlist_button,
			prpl_info != NULL && prpl_info->roomlist_get_list != NULL);
}

static void
chat_entry_changed_cb(GtkEditable *editable, BlistRequestData *data)
{
	chat_update_sensitivity(data);
}

static void
rebuild_chat_entries(BlistRequestData *data, const char *default_chat_name)
{
	PurplePluginProtocolInfo *prpl_info;
	PurpleConnection *gc;
	GList *list = NULL, *tmp;
	GHashTable *defaults = NULL;
	GtkWidget *child;
	gboolean focus = TRUE;

	while ((child = gtk_widget_get_first_child(data->chat_box)) != NULL)
		gtk_box_remove(GTK_BOX(data->chat_box), child);
	g_list_free(data->chat_entries);
	data->chat_entries = NULL;

	if (data->account == NULL ||
	    (gc = purple_account_get_connection(data->account)) == NULL) {
		chat_update_sensitivity(data);
		return;
	}
	prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(purple_connection_get_prpl(gc));

	if (prpl_info->chat_info != NULL)
		list = prpl_info->chat_info(gc);
	if (prpl_info->chat_info_defaults != NULL)
		defaults = prpl_info->chat_info_defaults(gc, default_chat_name);

	for (tmp = list; tmp; tmp = tmp->next) {
		struct proto_chat_entry *pce = tmp->data;
		GtkWidget *input;

		if (pce->is_int) {
			input = gtk_spin_button_new_with_range(pce->min, pce->max, 1);
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(input), pce->min);
			pidgin_add_widget_to_vbox(GTK_BOX(data->chat_box), pce->label, data->sg,
			                          input, FALSE, NULL);
		} else {
			const char *value = defaults ? g_hash_table_lookup(defaults, pce->identifier) : NULL;

			input = pce->secret ? gtk_password_entry_new() : gtk_entry_new();
			if (pce->secret)
				gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(input), TRUE);
			else
				gtk_entry_set_activates_default(GTK_ENTRY(input), TRUE);
			if (value != NULL)
				gtk_editable_set_text(GTK_EDITABLE(input), value);
			pidgin_add_widget_to_vbox(GTK_BOX(data->chat_box), pce->label, data->sg,
			                          input, TRUE, NULL);
			g_signal_connect(input, "changed", G_CALLBACK(chat_entry_changed_cb), data);
		}

		if (focus) {
			gtk_widget_grab_focus(input);
			focus = FALSE;
		}
		g_object_set_data_full(G_OBJECT(input), "identifier", g_strdup(pce->identifier), g_free);
		g_object_set_data(G_OBJECT(input), "is_spin", GINT_TO_POINTER(pce->is_int));
		g_object_set_data(G_OBJECT(input), "required", GINT_TO_POINTER(pce->required));
		data->chat_entries = g_list_append(data->chat_entries, input);

		g_free(pce);
	}

	g_list_free(list);
	if (defaults != NULL)
		g_hash_table_destroy(defaults);

	chat_update_sensitivity(data);
}

static void
chat_account_changed_cb(GObject *dropdown, GParamSpec *pspec, BlistRequestData *data)
{
	PurpleAccount *account = pidgin_account_dropdown_get_selected(data->account_menu);
	gboolean same = data->account != NULL && account != NULL &&
		purple_strequal(purple_account_get_protocol_id(data->account),
		                purple_account_get_protocol_id(account));

	data->account = account;
	if (!same)
		rebuild_chat_entries(data, data->default_chat_name);
	else
		chat_update_sensitivity(data);
}

static GHashTable *
chat_components(BlistRequestData *data, gboolean skip_empty)
{
	GHashTable *components = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	GList *l;

	for (l = data->chat_entries; l != NULL; l = l->next) {
		const char *id = g_object_get_data(l->data, "identifier");

		if (g_object_get_data(l->data, "is_spin")) {
			g_hash_table_replace(components, g_strdup(id), g_strdup_printf("%d",
				gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(l->data))));
		} else {
			const char *value = entry_text(l->data);

			if (!skip_empty || *value != '\0')
				g_hash_table_replace(components, g_strdup(id), g_strdup(value));
		}
	}
	return components;
}

static void
roomlist_cb(GtkWidget *button, BlistRequestData *data)
{
	todo_m5("the room list");
}

static void
join_chat_ok_cb(GtkWidget *button, BlistRequestData *data)
{
	PurpleChat *chat;

	if (data->account != NULL && g_list_find(purple_accounts_get_all(), data->account)) {
		chat = purple_chat_new(data->account, NULL, chat_components(data, FALSE));
		gtk_blist_join_chat(chat);
		purple_blist_remove_chat(chat);
	}
	gtk_window_destroy(GTK_WINDOW(data->window));
}

void
pidgin_blist_joinchat_show(void)
{
	BlistRequestData *data = g_new0(BlistRequestData, 1);
	GtkWidget *vbox;

	data->join = TRUE;
	vbox = make_blist_request_dialog(data, NULL, _("Join a Chat"),
		_("Please enter the appropriate information about the chat "
		  "you would like to join.\n"), chat_account_filter);

	data->chat_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BOX_SPACE);
	gtk_box_append(GTK_BOX(vbox), data->chat_box);

	data->roomlist_button = pidgin_dialog_add_button(data->window, _("Room _List"),
	                                                 G_CALLBACK(roomlist_cb), data);
	pidgin_dialog_add_button(data->window, _("_Cancel"), G_CALLBACK(close_request_cb), data);
	data->ok_button = pidgin_dialog_add_button(data->window, _("_Join"),
	                                           G_CALLBACK(join_chat_ok_cb), data);
	gtk_widget_add_css_class(data->ok_button, "suggested-action");
	gtk_window_set_default_widget(GTK_WINDOW(data->window), data->ok_button);

	g_signal_connect(data->account_menu, "notify::selected",
	                 G_CALLBACK(chat_account_changed_cb), data);
	rebuild_chat_entries(data, NULL);

	gtk_window_present(GTK_WINDOW(data->window));
}

static void
add_chat_ok_cb(GtkWidget *button, BlistRequestData *data)
{
	PurpleChat *chat;

	if (data->account == NULL || g_list_find(purple_accounts_get_all(), data->account) == NULL) {
		gtk_window_destroy(GTK_WINDOW(data->window));
		return;
	}

	chat = purple_chat_new(data->account, entry_text(data->alias_entry),
	                       chat_components(data, TRUE));
	if (chat != NULL) {
		PurpleGroup *group = find_or_add_group(entry_text(data->group_entry));

		purple_blist_add_chat(chat, group, NULL);

		if (gtk_check_button_get_active(GTK_CHECK_BUTTON(data->autojoin)))
			purple_blist_node_set_bool((PurpleBlistNode *)chat, "gtk-autojoin", TRUE);
		if (gtk_check_button_get_active(GTK_CHECK_BUTTON(data->persistent)))
			purple_blist_node_set_bool((PurpleBlistNode *)chat, "gtk-persistent", TRUE);
	}

	gtk_window_destroy(GTK_WINDOW(data->window));
}

static void
pidgin_blist_request_add_chat(PurpleAccount *account, PurpleGroup *group,
                              const char *alias, const char *name)
{
	BlistRequestData *data;
	GtkWidget *vbox, *box;
	GList *l;

	if (account != NULL) {
		PurplePluginProtocolInfo *prpl_info = connected_prpl_info(account);

		if (prpl_info == NULL || prpl_info->join_chat == NULL) {
			purple_notify_error(purple_account_get_connection(account), NULL,
				_("This protocol does not support chat rooms."), NULL);
			return;
		}
	} else {
		/* Find an account with chat capabilities */
		for (l = purple_connections_get_all(); l != NULL; l = l->next) {
			PurpleConnection *gc = l->data;

			if (PURPLE_PLUGIN_PROTOCOL_INFO(purple_connection_get_prpl(gc))->join_chat != NULL) {
				account = purple_connection_get_account(gc);
				break;
			}
		}
		if (account == NULL) {
			purple_notify_error(NULL, NULL,
				_("You are not currently signed on with any "
				  "protocols that have the ability to chat."), NULL);
			return;
		}
	}

	data = g_new0(BlistRequestData, 1);
	vbox = make_blist_request_dialog(data, account, _("Add Chat"),
		_("Please enter an alias, and the appropriate information "
		  "about the chat you would like to add to your buddy list.\n"),
		chat_account_filter);
	data->default_chat_name = g_strdup(name);

	data->chat_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BOX_SPACE);
	gtk_box_append(GTK_BOX(vbox), data->chat_box);

	data->alias_entry = gtk_entry_new();
	gtk_entry_set_activates_default(GTK_ENTRY(data->alias_entry), TRUE);
	if (alias != NULL)
		gtk_editable_set_text(GTK_EDITABLE(data->alias_entry), alias);
	pidgin_add_widget_to_vbox(GTK_BOX(vbox), _("A_lias:"), data->sg,
	                          data->alias_entry, TRUE, NULL);

	box = group_entry_new(group ? purple_group_get_name(group) : NULL, &data->group_entry);
	pidgin_add_widget_to_vbox(GTK_BOX(vbox), _("_Group:"), data->sg, box, TRUE, NULL);

	data->autojoin = gtk_check_button_new_with_mnemonic(_("Automatically _join when account connects"));
	data->persistent = gtk_check_button_new_with_mnemonic(_("_Remain in chat after window is closed"));
	gtk_box_append(GTK_BOX(vbox), data->autojoin);
	gtk_box_append(GTK_BOX(vbox), data->persistent);

	data->roomlist_button = pidgin_dialog_add_button(data->window, _("Room _List"),
	                                                 G_CALLBACK(roomlist_cb), data);
	pidgin_dialog_add_button(data->window, _("_Cancel"), G_CALLBACK(close_request_cb), data);
	data->ok_button = pidgin_dialog_add_button(data->window, _("_Add"),
	                                           G_CALLBACK(add_chat_ok_cb), data);
	gtk_widget_add_css_class(data->ok_button, "suggested-action");
	gtk_window_set_default_widget(GTK_WINDOW(data->window), data->ok_button);

	g_signal_connect(data->account_menu, "notify::selected",
	                 G_CALLBACK(chat_account_changed_cb), data);
	rebuild_chat_entries(data, name);
	if (name != NULL)
		gtk_widget_grab_focus(data->alias_entry);

	gtk_window_present(GTK_WINDOW(data->window));
}

static void
add_group_cb(PurpleConnection *gc, const char *group_name)
{
	if (group_name == NULL || *group_name == '\0')
		return;
	purple_blist_add_group(purple_group_new(group_name), NULL);
}

static void
pidgin_blist_request_add_group(void)
{
	purple_request_input(NULL, _("Add Group"), NULL,
	                     _("Please enter the name of the group to be added."),
	                     NULL, FALSE, FALSE, NULL,
	                     _("Add"), G_CALLBACK(add_group_cb),
	                     _("Cancel"), NULL,
	                     NULL, NULL, NULL,
	                     NULL);
}

/* The group a new buddy/chat goes in, from the selected node. */
static PurpleGroup *
group_of_node(PurpleBlistNode *node)
{
	if (node == NULL)
		return NULL;
	if (PURPLE_BLIST_NODE_IS_BUDDY(node))
		return purple_buddy_get_group((PurpleBuddy *)node);
	if (PURPLE_BLIST_NODE_IS_CONTACT(node) || PURPLE_BLIST_NODE_IS_CHAT(node))
		return (PurpleGroup *)purple_blist_node_get_parent(node);
	if (PURPLE_BLIST_NODE_IS_GROUP(node))
		return (PurpleGroup *)node;
	return NULL;
}

static void
add_buddy_for_node(PurpleBlistNode *node)
{
	PurpleGroup *group = group_of_node(node);

	purple_blist_request_add_buddy(NULL, NULL,
		group ? purple_group_get_name(group) : NULL, NULL);
}

static void
add_chat_for_node(PurpleBlistNode *node)
{
	purple_blist_request_add_chat(NULL, group_of_node(node), NULL, NULL);
}

/**************************************************************************
 * The alert area: connection errors and other mini-dialogs (Pidgin 2's
 * error_scrollbook and update_account_error_state)
 **************************************************************************/

static gboolean
update_alert_area_idle(gpointer data)
{
	if (gtkblist != NULL)
		gtk_widget_set_visible(gtkblist->alert_scroll,
			gtk_widget_get_first_child(gtkblist->alert_box) != NULL);
	return G_SOURCE_REMOVE;
}

static void
alert_parent_changed_cb(GtkWidget *widget, GParamSpec *pspec, gpointer data)
{
	if (gtk_widget_get_parent(widget) == NULL) {
		g_signal_handlers_disconnect_by_func(widget, alert_parent_changed_cb, data);
		g_idle_add(update_alert_area_idle, NULL);
	}
}

void
pidgin_blist_add_alert(GtkWidget *widget)
{
	g_return_if_fail(GTK_IS_WIDGET(widget));

	if (gtkblist == NULL) {
		/* No buddy list (yet): nothing can show it. */
		purple_debug_warning("gtkblist", "An alert arrived before the buddy list exists\n");
		g_object_ref_sink(widget);
		g_object_unref(widget);
		return;
	}

	gtk_box_append(GTK_BOX(gtkblist->alert_box), widget);
	g_signal_connect(widget, "notify::parent", G_CALLBACK(alert_parent_changed_cb), NULL);
	gtk_widget_set_visible(gtkblist->alert_scroll, TRUE);
	/* Pidgin 2 made the list urgent; present it. */
	if (gtk_widget_get_visible(gtkblist->window))
		gtk_window_present(GTK_WINDOW(gtkblist->window));
}

static void
generic_error_modify_cb(PidginMiniDialog *md, GtkButton *button, PurpleAccount *account)
{
	purple_account_clear_current_error(account);
	pidgin_account_dialog_show(PIDGIN_MODIFY_ACCOUNT_DIALOG, account);
}

static void
generic_error_enable_cb(PidginMiniDialog *md, GtkButton *button, PurpleAccount *account)
{
	purple_account_clear_current_error(account);
	purple_account_set_enabled(account, purple_core_get_ui(), TRUE);
}

static void
generic_error_reconnect_cb(PidginMiniDialog *md, GtkButton *button, PurpleAccount *account)
{
	purple_account_connect(account);
}

static void
generic_error_destroy_cb(GtkWidget *dialog, PurpleAccount *account)
{
	/* The buddy list is being destroyed (quitting): the error is saved
	 * state and stays, as in Pidgin 2. */
	if (gtkblist == NULL)
		return;

	if (g_hash_table_lookup(gtkblist->error_dialogs, account) == dialog)
		g_hash_table_remove(gtkblist->error_dialogs, account);
	/* Dismissed by the user (not replaced because the error changed):
	 * the error is dealt with. */
	if (g_object_get_data(G_OBJECT(dialog), DO_NOT_CLEAR_ERROR) == NULL &&
	    g_list_find(purple_accounts_get_all(), account) != NULL)
		purple_account_clear_current_error(account);
}

#define SSL_FAQ_URI "https://developer.pidgin.im/wiki/FAQssl"

static void
ssl_faq_clicked_cb(PidginMiniDialog *md, GtkButton *button, gpointer data)
{
	purple_notify_uri(NULL, SSL_FAQ_URI);
}

static void
dismiss_cb(PidginMiniDialog *md, GtkButton *button, gpointer data)
{
}

static void
add_generic_error_dialog(PurpleAccount *account, const PurpleConnectionErrorInfo *err)
{
	PidginMiniDialog *md;
	const char *username = purple_account_get_username(account);
	gboolean enabled = purple_account_get_enabled(account, purple_core_get_ui());
	char *primary;

	if (enabled)
		primary = g_strdup_printf(_("%s disconnected"), username);
	else
		primary = g_strdup_printf(_("%s disabled"), username);

	md = pidgin_mini_dialog_new(primary, err->description, "dialog-error");
	pidgin_mini_dialog_set_gicon(md, account_prpl_icon(account));
	g_free(primary);

	g_object_set_data(G_OBJECT(md), ACCOUNT_KEY, account);
	if (enabled)
		pidgin_mini_dialog_add_button(md, _("Reconnect"),
			(PidginMiniDialogCallback)generic_error_reconnect_cb, account);
	else
		pidgin_mini_dialog_add_button(md, _("Re-enable"),
			(PidginMiniDialogCallback)generic_error_enable_cb, account);
	pidgin_mini_dialog_add_button(md, _("Modify Account"),
		(PidginMiniDialogCallback)generic_error_modify_cb, account);
	if (err->type == PURPLE_CONNECTION_ERROR_NO_SSL_SUPPORT)
		pidgin_mini_dialog_add_non_closing_button(md, _("SSL FAQs"), ssl_faq_clicked_cb, NULL);
	pidgin_mini_dialog_add_button(md, _("_Dismiss"), dismiss_cb, NULL);

	g_signal_connect(md, "destroy", G_CALLBACK(generic_error_destroy_cb), account);
	g_hash_table_insert(gtkblist->error_dialogs, account, md);
	pidgin_blist_add_alert(GTK_WIDGET(md));
}

static void
remove_generic_error_dialog(PurpleAccount *account)
{
	PidginMiniDialog *md = g_hash_table_lookup(gtkblist->error_dialogs, account);

	if (md == NULL)
		return;
	/* The error changed: do not clear it when the dialog goes. */
	g_object_set_data(G_OBJECT(md), DO_NOT_CLEAR_ERROR, GINT_TO_POINTER(TRUE));
	g_hash_table_remove(gtkblist->error_dialogs, account);
	pidgin_mini_dialog_close(md);
}

static void
update_generic_error_message(PurpleAccount *account, const char *description)
{
	PidginMiniDialog *md = g_hash_table_lookup(gtkblist->error_dialogs, account);

	if (md != NULL)
		pidgin_mini_dialog_set_description(md, description);
}

/* "Signed on elsewhere" (PURPLE_CONNECTION_ERROR_NAME_IN_USE): one dialog
 * listing every such account. */

static GtkWidget *
find_account_label(PurpleAccount *account)
{
	GtkWidget *child;

	if (gtkblist->signed_on_elsewhere == NULL)
		return NULL;
	for (child = gtk_widget_get_first_child(GTK_WIDGET(
	         pidgin_mini_dialog_get_contents(gtkblist->signed_on_elsewhere)));
	     child != NULL; child = gtk_widget_get_next_sibling(child)) {
		if (g_object_get_data(G_OBJECT(child), ACCOUNT_KEY) == account)
			return child;
	}
	return NULL;
}

typedef void (*AccountFunction)(PurpleAccount *);

static void
elsewhere_foreach_account(PidginMiniDialog *md, AccountFunction f)
{
	GtkWidget *child;

	for (child = gtk_widget_get_first_child(GTK_WIDGET(pidgin_mini_dialog_get_contents(md)));
	     child != NULL; child = gtk_widget_get_next_sibling(child)) {
		PurpleAccount *account = g_object_get_data(G_OBJECT(child), ACCOUNT_KEY);

		if (account != NULL && g_list_find(purple_accounts_get_all(), account))
			f(account);
	}
}

static void
enable_account(PurpleAccount *account)
{
	purple_account_set_enabled(account, purple_core_get_ui(), TRUE);
}

/* Both buttons close the dialog and clear the accounts' errors. (Pidgin 2
 * did that from "destroy"; in GTK 4 the contents are gone by then, and it
 * must not happen when quitting.) */
static void
reconnect_elsewhere_accounts(PidginMiniDialog *md, GtkButton *button, gpointer unused)
{
	elsewhere_foreach_account(md, enable_account);
	elsewhere_foreach_account(md, purple_account_clear_current_error);
}

static void
dismiss_elsewhere_accounts(PidginMiniDialog *md, GtkButton *button, gpointer unused)
{
	elsewhere_foreach_account(md, purple_account_clear_current_error);
}

static void
elsewhere_destroy_cb(PidginMiniDialog *md, gpointer unused)
{
	if (gtkblist != NULL && gtkblist->signed_on_elsewhere == md)
		gtkblist->signed_on_elsewhere = NULL;
}

static void
update_signed_on_elsewhere_title(void)
{
	PidginMiniDialog *md = gtkblist->signed_on_elsewhere;
	guint accounts;
	char *title;

	if (md == NULL)
		return;

	accounts = pidgin_mini_dialog_get_num_children(md);
	if (accounts == 0) {
		pidgin_mini_dialog_close(md);
		gtkblist->signed_on_elsewhere = NULL;
		return;
	}

	title = g_strdup_printf(
		ngettext("%d account was disabled because you signed on from another location:",
		         "%d accounts were disabled because you signed on from another location:",
		         accounts), accounts);
	pidgin_mini_dialog_set_description(md, title);
	g_free(title);
}

static void
add_to_signed_on_elsewhere(PurpleAccount *account)
{
	PidginMiniDialog *md;
	GtkWidget *hbox, *label;
	const PurpleConnectionErrorInfo *err;

	if (gtkblist->signed_on_elsewhere == NULL) {
		md = gtkblist->signed_on_elsewhere =
			pidgin_mini_dialog_new(_("Welcome back!"), NULL, "network-offline");
		pidgin_mini_dialog_add_button(md, _("Re-enable"), reconnect_elsewhere_accounts, NULL);
		pidgin_mini_dialog_add_button(md, _("_Dismiss"), dismiss_elsewhere_accounts, NULL);
		g_signal_connect(md, "destroy", G_CALLBACK(elsewhere_destroy_cb), NULL);
		pidgin_blist_add_alert(GTK_WIDGET(md));
	}
	md = gtkblist->signed_on_elsewhere;

	if (find_account_label(account) != NULL)
		return;

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	g_object_set_data(G_OBJECT(hbox), ACCOUNT_KEY, account);
	gtk_box_append(GTK_BOX(hbox), gtk_image_new_from_gicon(account_prpl_icon(account)));
	label = gtk_label_new(purple_account_get_username(account));
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	err = purple_account_get_current_error(account);
	if (err != NULL && err->description != NULL && *err->description != '\0')
		gtk_widget_set_tooltip_text(label, err->description);
	gtk_box_append(GTK_BOX(hbox), label);
	gtk_box_append(pidgin_mini_dialog_get_contents(md), hbox);

	update_signed_on_elsewhere_title();
}

static void
remove_from_signed_on_elsewhere(PurpleAccount *account)
{
	GtkWidget *label = find_account_label(account);

	if (label == NULL)
		return;
	gtk_box_remove(pidgin_mini_dialog_get_contents(gtkblist->signed_on_elsewhere), label);
	update_signed_on_elsewhere_title();
}

static void
update_signed_on_elsewhere_tooltip(PurpleAccount *account, const char *description)
{
	GtkWidget *hbox = find_account_label(account);

	if (hbox != NULL)
		gtk_widget_set_tooltip_text(gtk_widget_get_last_child(hbox), description);
}

static void
update_account_error_state(PurpleAccount *account,
                           const PurpleConnectionErrorInfo *old,
                           const PurpleConnectionErrorInfo *new,
                           gpointer data)
{
	gboolean descriptions_differ;

	if (gtkblist == NULL || (old == NULL && new == NULL))
		return;

	if (old != NULL && new == NULL) {
		if (old->type == PURPLE_CONNECTION_ERROR_NAME_IN_USE)
			remove_from_signed_on_elsewhere(account);
		else
			remove_generic_error_dialog(account);
		return;
	}

	if (old == NULL && new != NULL) {
		if (new->type == PURPLE_CONNECTION_ERROR_NAME_IN_USE)
			add_to_signed_on_elsewhere(account);
		else
			add_generic_error_dialog(account, new);
		return;
	}

	/* else, new and old are both non-NULL */
	descriptions_differ = !purple_strequal(old->description, new->description);

	if (new->type == PURPLE_CONNECTION_ERROR_NAME_IN_USE) {
		if (old->type == PURPLE_CONNECTION_ERROR_NAME_IN_USE && descriptions_differ) {
			update_signed_on_elsewhere_tooltip(account, new->description);
		} else {
			remove_generic_error_dialog(account);
			add_to_signed_on_elsewhere(account);
		}
	} else {
		if (old->type == PURPLE_CONNECTION_ERROR_NAME_IN_USE) {
			remove_from_signed_on_elsewhere(account);
			add_generic_error_dialog(account, new);
		} else if (g_hash_table_lookup(gtkblist->error_dialogs, account) == NULL) {
			add_generic_error_dialog(account, new);
		} else if (descriptions_differ) {
			update_generic_error_message(account, new->description);
		}
	}
}

/* Accounts load before the list: show the errors they already have. */
static void
show_initial_account_errors(void)
{
	GList *l;

	for (l = purple_accounts_get_all(); l != NULL; l = l->next) {
		PurpleAccount *account = l->data;

		update_account_error_state(account, NULL,
			purple_account_get_current_error(account), NULL);
	}
}

/**************************************************************************
 * Menubar
 **************************************************************************/

static void
win_action(GSimpleAction *action, GVariant *param, gpointer data)
{
	const char *name = g_action_get_name(G_ACTION(action));

	if (purple_strequal(name, "new-im"))
		pidgin_dialogs_im();
	else if (purple_strequal(name, "join-chat"))
		pidgin_blist_joinchat_show();
	else if (purple_strequal(name, "get-info"))
		pidgin_dialogs_info();
	else if (purple_strequal(name, "view-log"))
		pidgin_dialogs_log();
	else if (purple_strequal(name, "add-buddy"))
		add_buddy_for_node(selected_node());
	else if (purple_strequal(name, "add-chat"))
		add_chat_for_node(selected_node());
	else if (purple_strequal(name, "add-group"))
		purple_blist_request_add_group();
	else if (purple_strequal(name, "online-help"))
		purple_notify_uri(NULL, PURPLE_WEBSITE "documentation");
	else if (purple_strequal(name, "pounces"))
		todo_m5("Buddy Pounces");
	else if (purple_strequal(name, "certificates"))
		todo_m5("the certificate manager");
	else if (purple_strequal(name, "smileys"))
		todo_m5("Custom Smileys");
	else if (purple_strequal(name, "plugins"))
		todo_m5("the plugins dialog");
	else if (purple_strequal(name, "preferences"))
		todo_m5("the preferences window");
	else if (purple_strequal(name, "privacy"))
		todo_m5("the privacy dialog");
	else if (purple_strequal(name, "transfers"))
		todo_m5("the file transfer window");
	else if (purple_strequal(name, "roomlist"))
		todo_m5("the room list");
	else if (purple_strequal(name, "system-log"))
		todo_m5("the system log viewer");
}

/* Stateful toggles are prefs; the pref callbacks update the state. */
static const struct {
	const char *action;
	const char *pref;
} pref_toggles[] = {
	{ "show-offline",        BLIST_PREFS "/show_offline_buddies" },
	{ "show-empty-groups",   BLIST_PREFS "/show_empty_groups" },
	{ "show-buddy-details",  BLIST_PREFS "/show_buddy_icons" },
	{ "show-idle-times",     BLIST_PREFS "/show_idle_time" },
	{ "show-protocol-icons", BLIST_PREFS "/show_protocol_icons" },
	{ "show-disconnected",   BLIST4_PREFS "/show_disconnected_accounts" },
	{ "mute",                PIDGIN_PREFS_ROOT "/sound/mute" },
};

static const char *
toggle_pref(const char *action)
{
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(pref_toggles); i++)
		if (purple_strequal(pref_toggles[i].action, action))
			return pref_toggles[i].pref;
	return NULL;
}

static void
toggle_change_state(GSimpleAction *action, GVariant *value, gpointer data)
{
	const char *pref = toggle_pref(g_action_get_name(G_ACTION(action)));

	if (pref != NULL)
		purple_prefs_set_bool(pref, g_variant_get_boolean(value));
	g_simple_action_set_state(action, value);
}

static void
sort_change_state(GSimpleAction *action, GVariant *value, gpointer data)
{
	purple_prefs_set_string(BLIST_PREFS "/sort_type", g_variant_get_string(value, NULL));
	g_simple_action_set_state(action, value);
}

static const GActionEntry win_entries[] = {
	{ .name = "new-im", .activate = win_action },
	{ .name = "join-chat", .activate = win_action },
	{ .name = "get-info", .activate = win_action },
	{ .name = "view-log", .activate = win_action },
	{ .name = "add-buddy", .activate = win_action },
	{ .name = "add-chat", .activate = win_action },
	{ .name = "add-group", .activate = win_action },
	{ .name = "online-help", .activate = win_action },
	{ .name = "pounces", .activate = win_action },
	{ .name = "certificates", .activate = win_action },
	{ .name = "smileys", .activate = win_action },
	{ .name = "plugins", .activate = win_action },
	{ .name = "preferences", .activate = win_action },
	{ .name = "privacy", .activate = win_action },
	{ .name = "transfers", .activate = win_action },
	{ .name = "roomlist", .activate = win_action },
	{ .name = "system-log", .activate = win_action },
	{ .name = "show-offline", .state = "false", .change_state = toggle_change_state },
	{ .name = "show-empty-groups", .state = "false", .change_state = toggle_change_state },
	{ .name = "show-buddy-details", .state = "false", .change_state = toggle_change_state },
	{ .name = "show-idle-times", .state = "false", .change_state = toggle_change_state },
	{ .name = "show-protocol-icons", .state = "false", .change_state = toggle_change_state },
	{ .name = "show-disconnected", .state = "false", .change_state = toggle_change_state },
	{ .name = "mute", .state = "false", .change_state = toggle_change_state },
	{ .name = "sort", .parameter_type = "s", .state = "'alphabetical'",
	  .change_state = sort_change_state },
};

static void
set_action_state(const char *name, GVariant *state)
{
	GAction *action;

	if (gtkblist == NULL)
		return;
	action = g_action_map_lookup_action(G_ACTION_MAP(gtkblist->window), name);
	if (action != NULL)
		g_simple_action_set_state(G_SIMPLE_ACTION(action), state);
	else
		g_variant_unref(g_variant_ref_sink(state));
}

static void
set_action_enabled(const char *name, gboolean enabled)
{
	GAction *action = g_action_map_lookup_action(G_ACTION_MAP(gtkblist->window), name);

	if (action != NULL)
		g_simple_action_set_enabled(G_SIMPLE_ACTION(action), enabled);
}

static void
sync_toggle_states(void)
{
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(pref_toggles); i++)
		set_action_state(pref_toggles[i].action,
			g_variant_new_boolean(purple_prefs_get_bool(pref_toggles[i].pref)));
	set_action_state("sort", g_variant_new_string(
		purple_prefs_get_string(BLIST_PREFS "/sort_type")));
}

/* Pidgin 2's require_connection[] */
static void
update_menu_sensitivity(void)
{
	gboolean connected = purple_connections_get_all() != NULL;

	if (gtkblist == NULL)
		return;
	set_action_enabled("new-im", connected);
	set_action_enabled("join-chat", pidgin_blist_joinchat_is_showable());
	set_action_enabled("get-info", connected);
	set_action_enabled("add-buddy", connected);
	set_action_enabled("add-chat", pidgin_blist_joinchat_is_showable());
	set_action_enabled("roomlist", connected);
	set_action_enabled("privacy", connected);
}

static GMenu *
section_new(GMenu *menu)
{
	GMenu *section = g_menu_new();

	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);
	return section;
}

static GMenuModel *
build_menubar(void)
{
	GMenu *bar = g_menu_new();
	GMenu *menu, *section, *sub;

	/* Buddies */
	menu = g_menu_new();
	section = section_new(menu);
	g_menu_append(section, _("New Instant _Message..."), "win.new-im");
	g_menu_append(section, _("Join a _Chat..."), "win.join-chat");
	g_menu_append(section, _("Get User _Info..."), "win.get-info");
	g_menu_append(section, _("View User _Log..."), "win.view-log");
	section = section_new(menu);
	sub = g_menu_new();
	g_menu_append(sub, _("_Offline Buddies"), "win.show-offline");
	g_menu_append(sub, _("_Empty Groups"), "win.show-empty-groups");
	g_menu_append(sub, _("Buddy _Details"), "win.show-buddy-details");
	g_menu_append(sub, _("Idle _Times"), "win.show-idle-times");
	g_menu_append(sub, _("_Protocol Icons"), "win.show-protocol-icons");
	g_menu_append(sub, _("Buddies of _Disconnected Accounts"), "win.show-disconnected");
	g_menu_append_submenu(section, _("Sh_ow"), G_MENU_MODEL(sub));
	g_object_unref(sub);
	gtkblist->sort_menu = g_menu_new();
	g_menu_append_submenu(section, _("_Sort Buddies"), G_MENU_MODEL(gtkblist->sort_menu));
	section = section_new(menu);
	g_menu_append(section, _("_Add Buddy..."), "win.add-buddy");
	g_menu_append(section, _("Add C_hat..."), "win.add-chat");
	g_menu_append(section, _("Add _Group..."), "win.add-group");
	section = section_new(menu);
	g_menu_append(section, _("_Quit"), "app.quit");
	g_menu_append_submenu(bar, _("_Buddies"), G_MENU_MODEL(menu));
	g_object_unref(menu);

	/* Accounts */
	menu = g_menu_new();
	section = section_new(menu);
	g_menu_append(section, _("Manage Accounts"), "app.accounts");
	gtkblist->accounts_menu = g_menu_new();
	g_menu_append_section(menu, NULL, G_MENU_MODEL(gtkblist->accounts_menu));
	g_menu_append_submenu(bar, _("_Accounts"), G_MENU_MODEL(menu));
	g_object_unref(menu);

	/* Tools (TODO(M5): the windows behind most of these) */
	menu = g_menu_new();
	section = section_new(menu);
	g_menu_append(section, _("Buddy _Pounces"), "win.pounces");
	g_menu_append(section, _("_Certificates"), "win.certificates");
	g_menu_append(section, _("Custom Smile_ys"), "win.smileys");
	g_menu_append(section, _("Plu_gins"), "win.plugins");
	g_menu_append(section, _("Pr_eferences"), "win.preferences");
	g_menu_append(section, _("Pr_ivacy"), "win.privacy");
	section = section_new(menu);
	g_menu_append(section, _("_File Transfers"), "win.transfers");
	g_menu_append(section, _("R_oom List"), "win.roomlist");
	g_menu_append(section, _("System _Log"), "win.system-log");
	section = section_new(menu);
	g_menu_append(section, _("Mute _Sounds"), "win.mute");
	gtkblist->plugins_menu = g_menu_new();
	g_menu_append_section(menu, NULL, G_MENU_MODEL(gtkblist->plugins_menu));
	g_menu_append_submenu(bar, _("_Tools"), G_MENU_MODEL(menu));
	g_object_unref(menu);

	/* Help */
	menu = g_menu_new();
	section = section_new(menu);
	g_menu_append(section, _("Online _Help"), "win.online-help");
	section = section_new(menu);
	g_menu_append(section, _("_Debug Window"), "app.debug");
	section = section_new(menu);
	g_menu_append(section, _("_About"), "app.about");
	g_menu_append_submenu(bar, _("_Help"), G_MENU_MODEL(menu));
	g_object_unref(menu);

	return G_MENU_MODEL(bar);
}

static void
rebuild_sort_menu(void)
{
	const char *current = purple_prefs_get_string(BLIST_PREFS "/sort_type");
	GList *l;

	if (gtkblist == NULL || gtkblist->sort_menu == NULL)
		return;

	g_menu_remove_all(gtkblist->sort_menu);
	for (l = sort_methods; l != NULL; l = l->next) {
		PidginBlistSortMethod *method = l->data;
		GMenuItem *item = g_menu_item_new(_(method->name), NULL);

		g_menu_item_set_action_and_target_value(item, "win.sort",
			g_variant_new_string(method->id));
		g_menu_append_item(gtkblist->sort_menu, item);
		g_object_unref(item);
	}
	set_action_state("sort", g_variant_new_string(current ? current : "none"));
}

/* Accounts menu (Pidgin 2's pidgin_blist_update_accounts_menu) */

typedef struct {
	PurpleAccount *account;
	int what;  /* 0 edit, 1 enable, 2 disable */
} AccountAction;

static void
account_action_cb(GSimpleAction *action, GVariant *param, AccountAction *aa)
{
	PurpleAccount *account = aa->account;

	if (g_list_find(purple_accounts_get_all(), account) == NULL)
		return;

	switch (aa->what) {
	case 0:
		pidgin_account_dialog_show(PIDGIN_MODIFY_ACCOUNT_DIALOG, account);
		break;
	case 1:
		purple_savedstatus_activate_for_account(purple_savedstatus_get_current(), account);
		purple_account_set_enabled(account, PIDGIN_UI, TRUE);
		break;
	case 2:
		purple_account_set_enabled(account, PIDGIN_UI, FALSE);
		break;
	}
}

static char *
add_account_action(GSimpleActionGroup *group, PurpleAccount *account, int what, int index)
{
	static const char *const kinds[] = { "edit", "enable", "disable" };
	char *name = g_strdup_printf("%s-%d", kinds[what], index);
	GSimpleAction *action = g_simple_action_new(name, NULL);
	AccountAction *aa = g_new0(AccountAction, 1);
	char *detailed;

	aa->account = account;
	aa->what = what;
	g_object_set_data_full(G_OBJECT(action), "pidgin-account-action", aa, g_free);
	g_signal_connect(action, "activate", G_CALLBACK(account_action_cb), aa);
	g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
	g_object_unref(action);

	detailed = g_strdup_printf("acct.%s", name);
	g_free(name);
	return detailed;
}

static char *
account_label(PurpleAccount *account)
{
	char *label = g_strdup_printf("%s (%s)", purple_account_get_username(account),
	                              purple_account_get_protocol_name(account));
	char *escaped = pidgin_menu_escape_label(label);

	g_free(label);
	return escaped;
}

static void
rebuild_accounts_menu(void)
{
	GSimpleActionGroup *group;
	GMenu *disabled = NULL, *enabled;
	GList *l;
	int i;

	if (gtkblist == NULL || gtkblist->accounts_menu == NULL)
		return;

	group = g_simple_action_group_new();
	g_menu_remove_all(gtkblist->accounts_menu);

	for (l = purple_accounts_get_all(), i = 0; l != NULL; l = l->next, i++) {
		PurpleAccount *account = l->data;
		char *label, *action;

		if (purple_account_get_enabled(account, PIDGIN_UI))
			continue;
		if (disabled == NULL)
			disabled = g_menu_new();
		label = account_label(account);
		action = add_account_action(group, account, 1, i);
		g_menu_append(disabled, label, action);
		g_free(label);
		g_free(action);
	}
	if (disabled != NULL) {
		GMenu *section = section_new(gtkblist->accounts_menu);
		g_menu_append_submenu(section, _("Enable Account"), G_MENU_MODEL(disabled));
		g_object_unref(disabled);
	}

	enabled = section_new(gtkblist->accounts_menu);
	for (l = purple_accounts_get_all(), i = 0; l != NULL; l = l->next, i++) {
		PurpleAccount *account = l->data;
		PurpleConnection *gc;
		PurplePlugin *plugin;
		GMenu *submenu, *section, *actions = NULL;
		char *label, *action;

		if (!purple_account_get_enabled(account, PIDGIN_UI))
			continue;

		submenu = g_menu_new();
		section = section_new(submenu);
		action = add_account_action(group, account, 0, i);
		g_menu_append(section, _("_Edit Account"), action);
		g_free(action);

		gc = purple_account_get_connection(account);
		plugin = (gc && PURPLE_CONNECTION_IS_CONNECTED(gc)) ? purple_connection_get_prpl(gc) : NULL;
		if (plugin != NULL)
			actions = pidgin_menu_from_plugin_actions(plugin, gc, group, "acct");
		if (actions != NULL) {
			g_menu_append_section(submenu, NULL, G_MENU_MODEL(actions));
			g_object_unref(actions);
		} else {
			/* TODO(M5): "Set Mood..." (the mood dialog). */
			section = section_new(submenu);
			g_menu_append(section, _("No actions available"), "acct.none");
		}

		section = section_new(submenu);
		action = add_account_action(group, account, 2, i);
		g_menu_append(section, _("_Disable"), action);
		g_free(action);

		label = account_label(account);
		g_menu_append_submenu(enabled, label, G_MENU_MODEL(submenu));
		g_free(label);
		g_object_unref(submenu);
	}

	/* A disabled placeholder action. */
	{
		GSimpleAction *none = g_simple_action_new("none", NULL);
		g_simple_action_set_enabled(none, FALSE);
		g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(none));
		g_object_unref(none);
	}

	gtk_widget_insert_action_group(gtkblist->window, "acct", G_ACTION_GROUP(group));
	g_object_unref(group);
}

static void
rebuild_plugins_menu(void)
{
	GSimpleActionGroup *group;
	GList *l;

	if (gtkblist == NULL || gtkblist->plugins_menu == NULL)
		return;

	group = g_simple_action_group_new();
	g_menu_remove_all(gtkblist->plugins_menu);

	for (l = purple_plugins_get_loaded(); l != NULL; l = l->next) {
		PurplePlugin *plugin = l->data;
		GMenu *submenu;
		char *label;

		if (PURPLE_IS_PROTOCOL_PLUGIN(plugin) || !PURPLE_PLUGIN_HAS_ACTIONS(plugin))
			continue;
		submenu = pidgin_menu_from_plugin_actions(plugin, NULL, group, "plugin");
		if (submenu == NULL)
			continue;
		label = pidgin_menu_escape_label(_(plugin->info->name));
		g_menu_append_submenu(gtkblist->plugins_menu, label, G_MENU_MODEL(submenu));
		g_free(label);
		g_object_unref(submenu);
	}

	gtk_widget_insert_action_group(gtkblist->window, "plugin", G_ACTION_GROUP(group));
	g_object_unref(group);
}

static void
setup_accels(void)
{
	static const struct {
		const char *action;
		const char *accel;
	} accels[] = {
		/* Pidgin 2's accelerators, except those that would take the
		 * text editing keys (Ctrl+A, Ctrl+C) away from entries. */
		{ "win.new-im", "<Control>m" },
		{ "win.get-info", "<Control>i" },
		{ "win.view-log", "<Control>l" },
		{ "win.add-buddy", "<Control>b" },
		{ "win.smileys", "<Control>y" },
		{ "win.plugins", "<Control>u" },
		{ "win.preferences", "<Control>p" },
		{ "win.transfers", "<Control>t" },
		{ "win.online-help", "F1" },
	};
	GtkApplication *app = pidgin_application_get();
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(accels); i++)
		gtk_application_set_accels_for_action(app, accels[i].action,
			(const char *[]){ accels[i].accel, NULL });
}

/**************************************************************************
 * Prefs and libpurple signals
 **************************************************************************/

static PidginBlistShowFlags
show_flags_from_prefs(void)
{
	PidginBlistShowFlags flags = 0;

	if (purple_prefs_get_bool(BLIST_PREFS "/show_offline_buddies"))
		flags |= PIDGIN_BLIST_SHOW_OFFLINE;
	if (purple_prefs_get_bool(BLIST_PREFS "/show_empty_groups"))
		flags |= PIDGIN_BLIST_SHOW_EMPTY_GROUPS;
	if (purple_prefs_get_bool(BLIST4_PREFS "/show_disconnected_accounts"))
		flags |= PIDGIN_BLIST_SHOW_DISCONNECTED;
	return flags;
}

void
pidgin_blist_refresh(PurpleBuddyList *list)
{
	gboolean biglist;

	if (gtkblist == NULL)
		return;

	biglist = purple_prefs_get_bool(BLIST_PREFS "/show_buddy_icons");
	pidgin_blist_model_set_show_flags(gtkblist->model, show_flags_from_prefs());
	pidgin_blist_model_update_all(gtkblist->model);

	if (biglist != gtkblist->biglist) {
		gtkblist->biglist = biglist;
		if (biglist)
			gtk_widget_add_css_class(gtkblist->list_view, "pidgin-blist-biglist");
		else
			gtk_widget_remove_css_class(gtkblist->list_view, "pidgin-blist-biglist");
		pidgin_blist_model_update_all(gtkblist->model);
		rebind_rows();
	}
}

static void
prefs_redo_list_cb(const char *name, PurplePrefType type, gconstpointer value, gpointer data)
{
	sync_toggle_states();
	pidgin_blist_refresh(purple_get_blist());
}

static void
prefs_mute_cb(const char *name, PurplePrefType type, gconstpointer value, gpointer data)
{
	sync_toggle_states();
}

static void
prefs_sort_cb(const char *name, PurplePrefType type, gconstpointer value, gpointer data)
{
	pidgin_blist_sort_method_set(value);
	sync_toggle_states();
}

static void
account_menu_changed_cb(gpointer instance, gpointer data)
{
	rebuild_accounts_menu();
	update_menu_sensitivity();
}

static void
account_removed_cb(PurpleAccount *account, gpointer data)
{
	if (gtkblist == NULL)
		return;
	g_hash_table_remove(gtkblist->prpl_icons, account);
	remove_generic_error_dialog(account);
	remove_from_signed_on_elsewhere(account);
	rebuild_accounts_menu();
}

static void
sign_on_off_cb(PurpleConnection *gc, gpointer data)
{
	rebuild_accounts_menu();
	update_menu_sensitivity();
}

static void
plugin_changed_cb(PurplePlugin *p, gpointer data)
{
	rebuild_plugins_menu();
}

/* Idle times change without libpurple telling us (Pidgin 2's
 * pidgin_blist_refresh_timer). */
static gboolean
refresh_timer_cb(gpointer data)
{
	PurpleBlistNode *gnode, *cnode, *bnode;

	if (gtkblist == NULL)
		return G_SOURCE_REMOVE;

	for (gnode = purple_blist_get_root(); gnode; gnode = purple_blist_node_get_sibling_next(gnode)) {
		for (cnode = purple_blist_node_get_first_child(gnode); cnode;
		     cnode = purple_blist_node_get_sibling_next(cnode)) {
			if (!PURPLE_BLIST_NODE_IS_CONTACT(cnode))
				continue;
			for (bnode = purple_blist_node_get_first_child(cnode); bnode;
			     bnode = purple_blist_node_get_sibling_next(bnode)) {
				PurpleBuddy *buddy = (PurpleBuddy *)bnode;

				if (PURPLE_BLIST_NODE_IS_BUDDY(bnode) &&
				    purple_presence_is_idle(purple_buddy_get_presence(buddy)))
					pidgin_blist_model_update(gtkblist->model, bnode);
			}
		}
	}
	return G_SOURCE_CONTINUE;
}

static gboolean
buddy_signonoff_timeout_cb(gpointer data)
{
	PurpleBlistNode *node = data;
	PidginBlistNodeItem *item = lookup_item(node);

	if (item != NULL) {
		/* The timer is ending: do not let the item remove it. */
		pidgin_blist_node_item_set_recent_signonoff(item, FALSE, 0);
		pidgin_blist_model_update(gtkblist->model, node);
	}
	return G_SOURCE_REMOVE;
}

static void
buddy_signonoff_cb(PurpleBuddy *buddy, gpointer data)
{
	PidginBlistNodeItem *item;
	guint timer;

	if (gtkblist == NULL)
		return;
	item = pidgin_blist_model_ensure(gtkblist->model, (PurpleBlistNode *)buddy);
	timer = g_timeout_add_seconds(10, buddy_signonoff_timeout_cb, buddy);
	pidgin_blist_node_item_set_recent_signonoff(item, TRUE, timer);
	pidgin_blist_model_update(gtkblist->model, (PurpleBlistNode *)buddy);
}

static void
buddy_privacy_changed_cb(PurpleBuddy *buddy, gpointer data)
{
	if (gtkblist != NULL)
		pidgin_blist_model_update(gtkblist->model, (PurpleBlistNode *)buddy);
}

static gboolean
autojoin_cb(PurpleConnection *gc, gpointer data)
{
	PurpleAccount *account = purple_connection_get_account(gc);
	PurpleBlistNode *gnode, *cnode;

	for (gnode = purple_blist_get_root(); gnode; gnode = purple_blist_node_get_sibling_next(gnode)) {
		if (!PURPLE_BLIST_NODE_IS_GROUP(gnode))
			continue;
		for (cnode = purple_blist_node_get_first_child(gnode); cnode;
		     cnode = purple_blist_node_get_sibling_next(cnode)) {
			PurpleChat *chat;

			if (!PURPLE_BLIST_NODE_IS_CHAT(cnode))
				continue;
			chat = (PurpleChat *)cnode;
			if (purple_chat_get_account(chat) != account)
				continue;
			if (purple_blist_node_get_bool(cnode, "gtk-autojoin"))
				serv_join_chat(gc, purple_chat_get_components(chat));
		}
	}

	/* Stop processing; we handled the autojoins. */
	return TRUE;
}

/**************************************************************************
 * The window
 **************************************************************************/

static void
save_window_size(void)
{
	int width, height;

	if (gtkblist == NULL || gtkblist->window == NULL)
		return;

	/* Wayland has no window positions for clients, so only the size. */
	gtk_window_get_default_size(GTK_WINDOW(gtkblist->window), &width, &height);
	if (width > 0 && height > 0 && !gtk_window_is_maximized(GTK_WINDOW(gtkblist->window))) {
		purple_prefs_set_int(BLIST4_PREFS "/width", width);
		purple_prefs_set_int(BLIST4_PREFS "/height", height);
	}
}

static gboolean
window_close_request_cb(GtkWindow *window, gpointer data)
{
	save_window_size();

	/* With a tray icon (M6: a StatusNotifierWatcher accepted it) closing
	 * the buddy list hides it into the tray, as in Pidgin 2; without one it
	 * quits, unless /pidgin4/blist/close_hides is set. */
	if (purple_prefs_get_bool(BLIST4_PREFS "/close_hides") ||
	    pidgin_docklet_is_embedded()) {
		purple_signal_emit(pidgin_blist_get_handle(), "gtkblist-hiding", purple_get_blist());
		gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
		purple_prefs_set_bool(BLIST4_PREFS "/list_visible", FALSE);
		return TRUE;
	}

	/* Quit from an idle, not inside the close-request emission: GTK holds
	 * a reference on the window until the emission ends, so the window
	 * (and the status box's destroy handler, which disconnects libpurple
	 * signals) would otherwise be finalized after libpurple has shut
	 * down. */
	g_idle_add_once((GSourceOnceFunc)pidgin_application_quit, NULL);
	return TRUE;
}

static void
build_window(void)
{
	GtkWidget *window, *vbox;
	GtkListItemFactory *factory;
	GtkEventController *shortcuts;
	GMenuModel *menubar;
	GListModel *root;

	window = gtk_application_window_new(pidgin_application_get());
	gtkblist->window = window;
	gtk_window_set_title(GTK_WINDOW(window), _("Buddy List"));
	gtk_window_set_default_size(GTK_WINDOW(window),
		purple_prefs_get_int(BLIST4_PREFS "/width"),
		purple_prefs_get_int(BLIST4_PREFS "/height"));
	gtk_application_window_set_show_menubar(GTK_APPLICATION_WINDOW(window), FALSE);
	gtk_widget_add_css_class(window, "pidgin-blist-window");
	g_signal_connect(window, "close-request", G_CALLBACK(window_close_request_cb), NULL);

	g_action_map_add_action_entries(G_ACTION_MAP(window), win_entries,
	                                G_N_ELEMENTS(win_entries), NULL);

	gtkblist->vbox = vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_window_set_child(GTK_WINDOW(window), vbox);

	menubar = build_menubar();
	gtkblist->menubar = gtk_popover_menu_bar_new_from_model(menubar);
	g_object_unref(menubar);
	gtk_box_append(GTK_BOX(vbox), gtkblist->menubar);

	/* The alert area (Pidgin 2's headline and error scrollbook). */
	gtkblist->alert_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BOX_SPACE);
	gtk_widget_add_css_class(gtkblist->alert_box, "pidgin-blist-alerts");
	gtkblist->alert_scroll = pidgin_make_scrollable(gtkblist->alert_box,
		GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC, -1, -1);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(gtkblist->alert_scroll), TRUE);
	gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(gtkblist->alert_scroll), 240);
	gtk_widget_set_visible(gtkblist->alert_scroll, FALSE);
	gtk_box_append(GTK_BOX(vbox), gtkblist->alert_scroll);

	/* The list */
	root = G_LIST_MODEL(g_object_ref(pidgin_blist_model_get_root(gtkblist->model)));
	gtkblist->tree = gtk_tree_list_model_new(root, FALSE, FALSE,
		(GtkTreeListModelCreateModelFunc)pidgin_blist_node_item_get_children,
		NULL, NULL);
	gtkblist->selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(gtkblist->tree)));
	gtk_single_selection_set_autoselect(gtkblist->selection, FALSE);
	gtk_single_selection_set_can_unselect(gtkblist->selection, TRUE);

	factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(factory_setup_cb), NULL);
	g_signal_connect(factory, "bind", G_CALLBACK(factory_bind_cb), NULL);
	g_signal_connect(factory, "unbind", G_CALLBACK(factory_unbind_cb), NULL);
	g_signal_connect(factory, "teardown", G_CALLBACK(factory_teardown_cb), NULL);
	gtkblist->factory = g_object_ref(factory);

	gtkblist->list_view = gtk_list_view_new(
		GTK_SELECTION_MODEL(g_object_ref(gtkblist->selection)), factory);
	gtk_widget_add_css_class(gtkblist->list_view, "pidgin-blist");
	if (gtkblist->biglist)
		gtk_widget_add_css_class(gtkblist->list_view, "pidgin-blist-biglist");
	g_signal_connect(gtkblist->list_view, "activate", G_CALLBACK(list_activate_cb), NULL);

	shortcuts = gtk_shortcut_controller_new();
	gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcuts),
		gtk_shortcut_new(gtk_shortcut_trigger_parse_string("Menu|<Shift>F10"),
		                 gtk_callback_action_new(menu_key_cb, NULL, NULL)));
	gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcuts),
		gtk_shortcut_new(gtk_shortcut_trigger_parse_string("<Control>o"),
		                 gtk_callback_action_new(info_key_cb, NULL, NULL)));
	gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcuts),
		gtk_shortcut_new(gtk_shortcut_trigger_parse_string("F2"),
		                 gtk_callback_action_new(alias_key_cb, NULL, NULL)));
	gtk_widget_add_controller(gtkblist->list_view, shortcuts);

	gtkblist->scrolled = pidgin_make_scrollable(gtkblist->list_view,
		GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC, -1, -1);
	gtk_widget_set_vexpand(gtkblist->scrolled, TRUE);
	gtk_box_append(GTK_BOX(vbox), gtkblist->scrolled);

	/* The status box */
	gtkblist->status_box = pidgin_status_box_new();
	gtk_box_append(GTK_BOX(vbox), gtkblist->status_box);

	sync_toggle_states();
	rebuild_sort_menu();
	rebuild_accounts_menu();
	rebuild_plugins_menu();
	update_menu_sensitivity();
	setup_accels();
}

/**************************************************************************
 * UI ops
 **************************************************************************/

static void
pidgin_blist_new_list(PurpleBuddyList *blist)
{
}

static void
pidgin_blist_new_node(PurpleBlistNode *node)
{
}

static void
pidgin_blist_show(PurpleBuddyList *list)
{
	void *handle;

	if (gtkblist != NULL) {
		gtk_window_present(GTK_WINDOW(gtkblist->window));
		return;
	}

	gtkblist = g_new0(PidginBuddyList, 1);
	gtkblist->error_dialogs = g_hash_table_new(g_direct_hash, g_direct_equal);
	gtkblist->icon_cache = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
	                                             (GDestroyNotify)icon_cache_entry_free);
	gtkblist->prpl_icons = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
	                                             g_object_unref);
	gtkblist->biglist = purple_prefs_get_bool(BLIST_PREFS "/show_buddy_icons");

	gtkblist->model = pidgin_blist_model_new();
	pidgin_blist_model_set_show_flags(gtkblist->model, show_flags_from_prefs());
	pidgin_blist_model_set_sort_func(gtkblist->model,
		current_sort_method ? current_sort_method->func : pidgin_blist_sort_alphabetical);
	g_signal_connect(gtkblist->model, "item-refresh", G_CALLBACK(model_item_refresh_cb), NULL);
	g_signal_connect(gtkblist->model, "item-inserted", G_CALLBACK(model_item_inserted_cb), NULL);

	build_window();

	/* Nodes loaded before the window existed. */
	pidgin_blist_model_update_all(gtkblist->model);

	gtkblist->refresh_timer = g_timeout_add_seconds(30, refresh_timer_cb, NULL);

	/* Things that affect how buddies are displayed */
	handle = pidgin_blist_get_handle();
	purple_prefs_connect_callback(handle, BLIST_PREFS "/show_buddy_icons", prefs_redo_list_cb, NULL);
	purple_prefs_connect_callback(handle, BLIST_PREFS "/show_idle_time", prefs_redo_list_cb, NULL);
	purple_prefs_connect_callback(handle, BLIST_PREFS "/show_empty_groups", prefs_redo_list_cb, NULL);
	purple_prefs_connect_callback(handle, BLIST_PREFS "/show_offline_buddies", prefs_redo_list_cb, NULL);
	purple_prefs_connect_callback(handle, BLIST_PREFS "/show_protocol_icons", prefs_redo_list_cb, NULL);
	purple_prefs_connect_callback(handle, BLIST4_PREFS "/show_disconnected_accounts",
	                              prefs_redo_list_cb, NULL);
	purple_prefs_connect_callback(handle, BLIST_PREFS "/sort_type", prefs_sort_cb, NULL);
	purple_prefs_connect_callback(handle, PIDGIN_PREFS_ROOT "/sound/mute", prefs_mute_cb, NULL);

	handle = purple_accounts_get_handle();
	purple_signal_connect(handle, "account-enabled", gtkblist,
	                      PURPLE_CALLBACK(account_menu_changed_cb), NULL);
	purple_signal_connect(handle, "account-disabled", gtkblist,
	                      PURPLE_CALLBACK(account_menu_changed_cb), NULL);
	purple_signal_connect(handle, "account-added", gtkblist,
	                      PURPLE_CALLBACK(account_menu_changed_cb), NULL);
	purple_signal_connect(handle, "account-removed", gtkblist,
	                      PURPLE_CALLBACK(account_removed_cb), NULL);
	purple_signal_connect(handle, "account-error-changed", gtkblist,
	                      PURPLE_CALLBACK(update_account_error_state), NULL);
	purple_signal_connect(handle, "account-actions-changed", gtkblist,
	                      PURPLE_CALLBACK(account_menu_changed_cb), NULL);
	purple_signal_connect(pidgin_account_get_handle(), "account-modified", gtkblist,
	                      PURPLE_CALLBACK(account_menu_changed_cb), NULL);

	handle = purple_connections_get_handle();
	purple_signal_connect(handle, "signed-on", gtkblist, PURPLE_CALLBACK(sign_on_off_cb), NULL);
	purple_signal_connect(handle, "signed-off", gtkblist, PURPLE_CALLBACK(sign_on_off_cb), NULL);

	handle = purple_plugins_get_handle();
	purple_signal_connect(handle, "plugin-load", gtkblist, PURPLE_CALLBACK(plugin_changed_cb), NULL);
	purple_signal_connect(handle, "plugin-unload", gtkblist, PURPLE_CALLBACK(plugin_changed_cb), NULL);

	/* TODO(M4): conversation-updated/-created/-deleting and chat-joined
	 * (unseen message markers on rows). */

	show_initial_account_errors();

	/* The buddy list is the main window, shown at startup unless it was
	 * hidden in the tray at the last quit and a tray host is running (M6,
	 * gtkdocklet.c shows it if the tray icon does not appear). */
	if (!pidgin_docklet_start_hidden()) {
		gtk_window_present(GTK_WINDOW(gtkblist->window));
		purple_prefs_set_bool(BLIST4_PREFS "/list_visible", TRUE);
	}

	purple_signal_emit(pidgin_blist_get_handle(), "gtkblist-created", list);

	/* No-op unless PIDGIN4_BLIST_SELFTEST is set. */
	pidgin_blist_selftest();
}

static void
pidgin_blist_update(PurpleBuddyList *list, PurpleBlistNode *node)
{
	if (gtkblist == NULL || node == NULL)
		return;
	pidgin_blist_model_update(gtkblist->model, node);
}

static void
pidgin_blist_remove(PurpleBuddyList *list, PurpleBlistNode *node)
{
	PurpleBlistNode *parent;

	purple_request_close_with_handle(node);

	if (gtkblist == NULL)
		return;

	if (gtkblist->popover_node == node)
		close_node_menu();

	g_hash_table_remove(gtkblist->icon_cache, node);
	pidgin_blist_model_remove(gtkblist->model, node);

	parent = purple_blist_node_get_parent(node);
	if (parent != NULL)
		pidgin_blist_model_update(gtkblist->model, parent);
}

static void
pidgin_blist_destroy(PurpleBuddyList *list)
{
	PidginBuddyList *b = gtkblist;

	if (b == NULL)
		return;

	save_window_size();
	purple_signals_disconnect_by_handle(b);
	purple_prefs_disconnect_by_handle(pidgin_blist_get_handle());

	if (b->refresh_timer != 0)
		g_source_remove(b->refresh_timer);

	close_node_menu();
	gtkblist = NULL;

	/* Error dialogs: the account errors stay (they are saved state). */
	g_hash_table_destroy(b->error_dialogs);

	gtk_window_destroy(GTK_WINDOW(b->window));
	pidgin_blist_model_clear(b->model);
	g_clear_object(&b->factory);
	g_clear_object(&b->selection);
	g_clear_object(&b->tree);
	g_clear_object(&b->model);
	g_clear_object(&b->sort_menu);
	g_clear_object(&b->accounts_menu);
	g_clear_object(&b->plugins_menu);
	g_hash_table_destroy(b->icon_cache);
	g_hash_table_destroy(b->prpl_icons);
	g_free(b);
}

static void
pidgin_blist_set_visible(PurpleBuddyList *list, gboolean show)
{
	if (gtkblist == NULL)
		return;

	if (show) {
		if (!gtk_widget_get_visible(gtkblist->window))
			purple_signal_emit(pidgin_blist_get_handle(), "gtkblist-unhiding", list);
		gtk_window_present(GTK_WINDOW(gtkblist->window));
		purple_prefs_set_bool(BLIST4_PREFS "/list_visible", TRUE);
	} else if (purple_prefs_get_bool(BLIST4_PREFS "/close_hides") ||
	           pidgin_docklet_is_embedded()) {
		/* M6: hidden into the tray. */
		purple_signal_emit(pidgin_blist_get_handle(), "gtkblist-hiding", list);
		gtk_widget_set_visible(gtkblist->window, FALSE);
		purple_prefs_set_bool(BLIST4_PREFS "/list_visible", FALSE);
	} else {
		gtk_window_minimize(GTK_WINDOW(gtkblist->window));
	}
}

void
pidgin_blist_toggle_visibility(void)
{
	if (gtkblist == NULL)
		return;
	purple_blist_set_visible(!(gtk_widget_get_visible(gtkblist->window) &&
	                           gtk_window_is_active(GTK_WINDOW(gtkblist->window))));
}

static PurpleBlistUiOps blist_ui_ops =
{
	pidgin_blist_new_list,
	pidgin_blist_new_node,
	pidgin_blist_show,
	pidgin_blist_update,
	pidgin_blist_remove,
	pidgin_blist_destroy,
	pidgin_blist_set_visible,
	pidgin_blist_request_add_buddy,
	pidgin_blist_request_add_chat,
	pidgin_blist_request_add_group,
	/* NULL: libpurple fills in its own savers, which write blist.xml. */
	NULL, /* save_node */
	NULL, /* remove_node */
	NULL, /* save_account */
	NULL
};

PurpleBlistUiOps *
pidgin_blist_get_ui_ops(void)
{
	return &blist_ui_ops;
}

/**************************************************************************
 * Sort methods
 **************************************************************************/

GList *
pidgin_blist_get_sort_methods(void)
{
	return sort_methods;
}

void
pidgin_blist_sort_method_reg(const char *id, const char *name, PidginBlistSortFunc func)
{
	PidginBlistSortMethod *method;

	g_return_if_fail(id != NULL);
	g_return_if_fail(name != NULL);

	method = g_new0(PidginBlistSortMethod, 1);
	method->id = g_strdup(id);
	method->name = g_strdup(name);
	method->func = func;
	sort_methods = g_list_append(sort_methods, method);
	rebuild_sort_menu();
}

static void
sort_method_free(PidginBlistSortMethod *method)
{
	g_free(method->id);
	g_free(method->name);
	g_free(method);
}

void
pidgin_blist_sort_method_unreg(const char *id)
{
	GList *l;

	g_return_if_fail(id != NULL);

	for (l = sort_methods; l != NULL; l = l->next) {
		PidginBlistSortMethod *method = l->data;

		if (purple_strequal(method->id, id)) {
			sort_methods = g_list_delete_link(sort_methods, l);
			if (current_sort_method == method)
				current_sort_method = NULL;
			sort_method_free(method);
			break;
		}
	}
	if (current_sort_method == NULL)
		pidgin_blist_sort_method_set("none");
	rebuild_sort_menu();
}

void
pidgin_blist_sort_method_set(const char *id)
{
	GList *l;

	if (id == NULL)
		id = "none";

	for (l = sort_methods; l != NULL; l = l->next) {
		if (purple_strequal(((PidginBlistSortMethod *)l->data)->id, id))
			break;
	}

	if (l != NULL)
		current_sort_method = l->data;
	else if (current_sort_method == NULL) {
		if (!purple_strequal(id, "none"))
			pidgin_blist_sort_method_set("none");
		return;
	}

	if (gtkblist != NULL) {
		pidgin_blist_model_set_sort_func(gtkblist->model,
			current_sort_method ? current_sort_method->func : NULL);
		pidgin_blist_model_update_all(gtkblist->model);
	}
}

/**************************************************************************
 * Selftest (PIDGIN4_BLIST_SELFTEST=1)
 **************************************************************************/

static int
count_nodes(PurpleBlistNodeType type)
{
	PurpleBlistNode *node;
	int n = 0;

	for (node = purple_blist_get_root(); node != NULL; node = purple_blist_node_next(node, TRUE))
		if (purple_blist_node_get_type(node) == type)
			n++;
	return n;
}

static void
selftest_log_counts(const char *when)
{
	purple_debug_info("gtkblist", "selftest: %s: visible %u groups, %u contacts, "
		"%u chats, %u buddies (of %d groups, %d contacts, %d chats, %d buddies); "
		"model %s\n", when,
		pidgin_blist_model_count_visible(gtkblist->model, PURPLE_BLIST_GROUP_NODE),
		pidgin_blist_model_count_visible(gtkblist->model, PURPLE_BLIST_CONTACT_NODE),
		pidgin_blist_model_count_visible(gtkblist->model, PURPLE_BLIST_CHAT_NODE),
		pidgin_blist_model_count_visible(gtkblist->model, PURPLE_BLIST_BUDDY_NODE),
		count_nodes(PURPLE_BLIST_GROUP_NODE), count_nodes(PURPLE_BLIST_CONTACT_NODE),
		count_nodes(PURPLE_BLIST_CHAT_NODE), count_nodes(PURPLE_BLIST_BUDDY_NODE),
		pidgin_blist_model_check(gtkblist->model) ? "consistent" : "INCONSISTENT");
}

typedef struct {
	PurpleBlistNode *group;
	gboolean had_setting;
	gboolean collapsed;
} SelftestGroupState;

static void
selftest_menus(void)
{
	PurpleBlistNode *node;
	int menus = 0, items = 0, tips = 0;

	for (node = purple_blist_get_root(); node != NULL; node = purple_blist_node_next(node, TRUE)) {
		GSimpleActionGroup *group = g_simple_action_group_new();
		GMenu *menu = g_menu_new();
		GtkWidget *tip;
		char *text;

		if (PURPLE_BLIST_NODE_IS_BUDDY(node) || PURPLE_BLIST_NODE_IS_CONTACT(node) ||
		    PURPLE_BLIST_NODE_IS_GROUP(node) || PURPLE_BLIST_NODE_IS_CHAT(node)) {
			pidgin_blist_build_node_menu(node, menu, group);
			menus++;
			items += g_menu_model_get_n_items(G_MENU_MODEL(menu));

			text = pidgin_blist_get_tooltip_text(node, TRUE);
			g_free(text);
			tip = tooltip_widget(node);
			g_object_ref_sink(tip);
			g_object_unref(tip);
			tips++;
		}
		g_object_unref(menu);
		g_object_unref(group);
	}
	purple_debug_info("gtkblist", "selftest: built %d context menus (%d sections) "
	                  "and %d tooltips\n", menus, items, tips);
}

static gboolean
selftest_run(gpointer data)
{
	static const char *const toggles[] = {
		BLIST_PREFS "/show_offline_buddies", BLIST_PREFS "/show_empty_groups",
		BLIST_PREFS "/show_buddy_icons", BLIST_PREFS "/show_idle_time",
		BLIST_PREFS "/show_protocol_icons", BLIST4_PREFS "/show_disconnected_accounts",
	};
	gboolean saved[G_N_ELEMENTS(toggles)];
	char *saved_sort;
	GList *groups = NULL, *l;
	PurpleBlistNode *gnode;
	gsize i;
	int expanded = 0;

	if (gtkblist == NULL)
		return G_SOURCE_REMOVE;

	selftest_log_counts("start");

	for (i = 0; i < G_N_ELEMENTS(toggles); i++)
		saved[i] = purple_prefs_get_bool(toggles[i]);
	saved_sort = g_strdup(purple_prefs_get_string(BLIST_PREFS "/sort_type"));

	/* Everything visible, through the menu actions. */
	g_action_group_change_action_state(G_ACTION_GROUP(gtkblist->window), "show-offline",
	                                   g_variant_new_boolean(TRUE));
	g_action_group_change_action_state(G_ACTION_GROUP(gtkblist->window), "show-empty-groups",
	                                   g_variant_new_boolean(TRUE));
	g_action_group_change_action_state(G_ACTION_GROUP(gtkblist->window), "show-disconnected",
	                                   g_variant_new_boolean(TRUE));
	selftest_log_counts("everything shown");

	/* Collapse and expand every group, then put them back. */
	for (gnode = purple_blist_get_root(); gnode; gnode = purple_blist_node_get_sibling_next(gnode)) {
		SelftestGroupState *st;
		GHashTable *settings;

		if (!PURPLE_BLIST_NODE_IS_GROUP(gnode))
			continue;
		st = g_new0(SelftestGroupState, 1);
		st->group = gnode;
		settings = gnode->settings;
		st->had_setting = settings && g_hash_table_lookup(settings, "collapsed") != NULL;
		st->collapsed = purple_blist_node_get_bool(gnode, "collapsed");
		groups = g_list_prepend(groups, st);

		expand_node(gnode, FALSE);
		expand_node(gnode, TRUE);
		expanded++;
	}
	purple_debug_info("gtkblist", "selftest: collapsed and expanded %d groups; "
	                  "%u rows now\n", expanded,
	                  g_list_model_get_n_items(G_LIST_MODEL(gtkblist->tree)));
	for (l = groups; l != NULL; l = l->next) {
		SelftestGroupState *st = l->data;

		expand_node(st->group, !st->collapsed);
		if (!st->had_setting)
			purple_blist_node_remove_setting(st->group, "collapsed");
	}
	g_list_free_full(groups, g_free);

	selftest_menus();

	/* Every Show option on and off. */
	for (i = 0; i < G_N_ELEMENTS(pref_toggles); i++) {
		const char *action = pref_toggles[i].action;
		gboolean state;

		if (purple_strequal(action, "mute"))
			continue;
		state = purple_prefs_get_bool(pref_toggles[i].pref);
		g_action_group_change_action_state(G_ACTION_GROUP(gtkblist->window), action,
		                                   g_variant_new_boolean(!state));
		if (!pidgin_blist_model_check(gtkblist->model))
			purple_debug_error("gtkblist", "selftest: model inconsistent after %s\n", action);
		g_action_group_change_action_state(G_ACTION_GROUP(gtkblist->window), action,
		                                   g_variant_new_boolean(state));
	}

	/* Every sort method. */
	for (l = sort_methods; l != NULL; l = l->next) {
		PidginBlistSortMethod *method = l->data;

		g_action_group_change_action_state(G_ACTION_GROUP(gtkblist->window), "sort",
		                                   g_variant_new_string(method->id));
		purple_debug_info("gtkblist", "selftest: sorted by %s: model %s\n", method->id,
			pidgin_blist_model_check(gtkblist->model) ? "consistent" : "INCONSISTENT");
	}

	/* Restore the prefs. */
	purple_prefs_set_string(BLIST_PREFS "/sort_type", saved_sort);
	g_free(saved_sort);
	for (i = 0; i < G_N_ELEMENTS(toggles); i++)
		purple_prefs_set_bool(toggles[i], saved[i]);

	selftest_log_counts("done");
	purple_debug_info("gtkblist", "selftest: finished, quitting\n");
	pidgin_application_quit();
	return G_SOURCE_REMOVE;
}

void
pidgin_blist_selftest(void)
{
	static gboolean done = FALSE;

	if (done || g_getenv("PIDGIN4_BLIST_SELFTEST") == NULL)
		return;
	done = TRUE;
	/* After the window is up and the main loop runs. */
	g_timeout_add_seconds(2, selftest_run, NULL);
}

/**************************************************************************
 * Init
 **************************************************************************/

void
pidgin_blist_init(void)
{
	void *handle = pidgin_blist_get_handle();

	/*
	 * Shared prefs: the same keys, types and defaults as Pidgin 2
	 * (pidgin/gtkblist.c: pidgin_blist_init), with the same meaning. They
	 * exist in any profile Pidgin 2 has used; adding them is a no-op then.
	 */
	purple_prefs_add_none(PIDGIN_PREFS_ROOT "/blist");
	purple_prefs_add_bool(BLIST_PREFS "/show_buddy_icons", TRUE);
	purple_prefs_add_bool(BLIST_PREFS "/show_empty_groups", FALSE);
	purple_prefs_add_bool(BLIST_PREFS "/show_idle_time", TRUE);
	purple_prefs_add_bool(BLIST_PREFS "/show_offline_buddies", FALSE);
	purple_prefs_add_bool(BLIST_PREFS "/show_protocol_icons", FALSE);
	purple_prefs_add_string(BLIST_PREFS "/sort_type", "alphabetical");
	purple_prefs_add_none(PIDGIN_PREFS_ROOT "/sound");
	purple_prefs_add_bool(PIDGIN_PREFS_ROOT "/sound/mute", FALSE);
	purple_prefs_add_none(PIDGIN_PREFS_ROOT "/accounts");
	purple_prefs_add_path(PIDGIN_PREFS_ROOT "/accounts/buddyicon", "");

	/* pidgin4's own: window size (no positions on Wayland), whether
	 * closing hides the window, and a filter Pidgin 2 does not have. */
	purple_prefs_add_none(BLIST4_PREFS);
	purple_prefs_add_int(BLIST4_PREFS "/width", 300);
	purple_prefs_add_int(BLIST4_PREFS "/height", 600);
	purple_prefs_add_bool(BLIST4_PREFS "/close_hides", FALSE);
	purple_prefs_add_bool(BLIST4_PREFS "/list_visible", TRUE);
	purple_prefs_add_bool(BLIST4_PREFS "/show_disconnected_accounts", FALSE);

	/* TODO(M5): blist themes. The theme loader is not ported; the theme's
	 * colours and fonts are CSS classes now (see resources/style.css). */

	purple_signal_register(handle, "gtkblist-hiding",
	                       purple_marshal_VOID__POINTER, NULL, 1,
	                       purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_BLIST));
	purple_signal_register(handle, "gtkblist-unhiding",
	                       purple_marshal_VOID__POINTER, NULL, 1,
	                       purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_BLIST));
	purple_signal_register(handle, "gtkblist-created",
	                       purple_marshal_VOID__POINTER, NULL, 1,
	                       purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_BLIST));
	purple_signal_register(handle, "drawing-tooltip",
	                       purple_marshal_VOID__POINTER_POINTER_UINT, NULL, 3,
	                       purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_BLIST_NODE),
	                       purple_value_new_outgoing(PURPLE_TYPE_BOXED, "GString *"),
	                       purple_value_new(PURPLE_TYPE_BOOLEAN));
	purple_signal_register(handle, "drawing-buddy",
	                       purple_marshal_POINTER__POINTER,
	                       purple_value_new(PURPLE_TYPE_STRING), 1,
	                       purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_BLIST_BUDDY));

	purple_signal_connect(purple_blist_get_handle(), "buddy-signed-on",
	                      handle, PURPLE_CALLBACK(buddy_signonoff_cb), NULL);
	purple_signal_connect(purple_blist_get_handle(), "buddy-signed-off",
	                      handle, PURPLE_CALLBACK(buddy_signonoff_cb), NULL);
	purple_signal_connect(purple_blist_get_handle(), "buddy-privacy-changed",
	                      handle, PURPLE_CALLBACK(buddy_privacy_changed_cb), NULL);
	purple_signal_connect_priority(purple_connections_get_handle(), "autojoin",
	                               handle, PURPLE_CALLBACK(autojoin_cb),
	                               NULL, PURPLE_SIGNAL_PRIORITY_HIGHEST);

	/* Pidgin 2's sort methods, in its order. */
	if (sort_methods == NULL) {
		pidgin_blist_sort_method_reg("none", N_("Manually"), NULL);
		pidgin_blist_sort_method_reg("alphabetical", N_("Alphabetically"),
		                             pidgin_blist_sort_alphabetical);
		pidgin_blist_sort_method_reg("status", N_("By status"), pidgin_blist_sort_status);
		pidgin_blist_sort_method_reg("log_size", N_("By recent log activity"),
		                             pidgin_blist_sort_log_activity);
	}
	pidgin_blist_sort_method_set(purple_prefs_get_string(BLIST_PREFS "/sort_type"));
}

void
pidgin_blist_uninit(void)
{
	purple_signals_unregister_by_instance(pidgin_blist_get_handle());
	purple_signals_disconnect_by_handle(pidgin_blist_get_handle());
	purple_prefs_disconnect_by_handle(pidgin_blist_get_handle());

	g_list_free_full(sort_methods, (GDestroyNotify)sort_method_free);
	sort_methods = NULL;
	current_sort_method = NULL;
}
