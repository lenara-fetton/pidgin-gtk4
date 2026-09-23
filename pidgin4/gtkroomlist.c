/*
 * pidgin4
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
 * The room list: port of pidgin/gtkroomlist.c. Each PurpleRoomlist gets a
 * GtkColumnView over a GtkTreeListModel (categories are expanded through
 * the prpl the first time they are opened), with one column per visible
 * field. For XMPP accounts a Bookmark button uses the jabber prpl's
 * "bookmark-add" IPC (doc/PIDGIN-UPGRADE.md, M8).
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "account.h"
#include "blist.h"
#include "connection.h"
#include "debug.h"
#include "notify.h"
#include "plugin.h"
#include "prpl.h"
#include "roomlist.h"

#include "gtkroomlist.h"
#include "gtkutils.h"
#include "pidginselftest.h"

typedef struct _PidginRoomlistDialog {
	GtkWidget *window;
	GtkWidget *account_widget;
	GtkWidget *progress;
	GtkWidget *sw;
	GtkWidget *placeholder;
	GtkWidget *stop_button;
	GtkWidget *list_button;
	GtkWidget *add_button;
	GtkWidget *join_button;
	GtkWidget *bookmark_button;
	GtkWidget *close_button;

	PurpleAccount *account;
	PurpleRoomlist *roomlist;

	gboolean pg_needs_pulse;
	guint pg_update_to;
} PidginRoomlistDialog;

typedef struct _PidginRoomlist {
	PidginRoomlistDialog *dialog;
	GListStore *root;
	GtkSingleSelection *selection;
	GtkWidget *view;
	GHashTable *cats; /* PurpleRoomlistRoom (category) -> its children store */
	gint num_rooms, total_rooms;
} PidginRoomlist;

static GList *dialogs = NULL;

static void update_buttons(PidginRoomlistDialog *dialog);

/**************************************************************************
 * PidginRoomNode: a room or category in the tree
 **************************************************************************/

#define PIDGIN_TYPE_ROOM_NODE (pidgin_room_node_get_type())
G_DECLARE_FINAL_TYPE(PidginRoomNode, pidgin_room_node, PIDGIN, ROOM_NODE, GObject)

struct _PidginRoomNode {
	GObject parent;
	PurpleRoomlist *list;       /* not referenced: the node lives in its model */
	PurpleRoomlistRoom *room;
	GListStore *children;       /* categories only */
};

G_DEFINE_FINAL_TYPE(PidginRoomNode, pidgin_room_node, G_TYPE_OBJECT)

static void
pidgin_room_node_finalize(GObject *obj)
{
	g_clear_object(&PIDGIN_ROOM_NODE(obj)->children);
	G_OBJECT_CLASS(pidgin_room_node_parent_class)->finalize(obj);
}

static void
pidgin_room_node_class_init(PidginRoomNodeClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = pidgin_room_node_finalize;
}

static void pidgin_room_node_init(PidginRoomNode *node) { }

static GListModel *
create_children_cb(gpointer item, gpointer data)
{
	PidginRoomNode *node = item;

	return node->children ? G_LIST_MODEL(g_object_ref(node->children)) : NULL;
}

/**************************************************************************
 * Helpers
 **************************************************************************/

static gboolean
account_filter_func(PurpleAccount *account)
{
	PurpleConnection *conn = purple_account_get_connection(account);
	PurplePluginProtocolInfo *prpl_info = NULL;

	if (conn && PURPLE_CONNECTION_IS_CONNECTED(conn))
		prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(conn->prpl);

	return (prpl_info && prpl_info->roomlist_get_list != NULL);
}

gboolean
pidgin_roomlist_is_showable(void)
{
	GList *c;

	for (c = purple_connections_get_all(); c != NULL; c = c->next)
		if (account_filter_func(purple_connection_get_account(c->data)))
			return TRUE;

	return FALSE;
}

/* The field value of @room at @index, formatted (Pidgin 2 hid a zero). */
static char *
field_text(PurpleRoomlist *list, PurpleRoomlistRoom *room, guint index)
{
	PurpleRoomlistField *f = g_list_nth_data(list->fields, index);
	gpointer value = g_list_nth_data(room->fields, index);

	if (f == NULL)
		return g_strdup("");

	switch (f->type) {
		case PURPLE_ROOMLIST_FIELD_BOOL:
			return g_strdup(GPOINTER_TO_INT(value) ? "✓" : "");
		case PURPLE_ROOMLIST_FIELD_INT:
			return GPOINTER_TO_INT(value) ? g_strdup_printf("%d", GPOINTER_TO_INT(value))
			                              : g_strdup("");
		case PURPLE_ROOMLIST_FIELD_STRING:
		default:
			return g_strdup(value ? (const char *)value : "");
	}
}

static PidginRoomNode *
selected_node(PidginRoomlistDialog *dialog)
{
	PidginRoomlist *rl;
	GtkTreeListRow *row;

	if (dialog->roomlist == NULL || (rl = dialog->roomlist->ui_data) == NULL ||
	    rl->selection == NULL)
		return NULL;

	row = gtk_single_selection_get_selected_item(rl->selection);
	if (row == NULL)
		return NULL;
	/* transfer full */
	return gtk_tree_list_row_get_item(row);
}

static PurpleRoomlistRoom *
selected_room(PidginRoomlistDialog *dialog)
{
	PidginRoomNode *node = selected_node(dialog);
	PurpleRoomlistRoom *room = NULL;

	if (node != NULL) {
		if (node->room->type & PURPLE_ROOMLIST_ROOMTYPE_ROOM)
			room = node->room;
		g_object_unref(node);
	}
	return room;
}

static PurplePlugin *
bookmark_prpl(PidginRoomlistDialog *dialog)
{
	PurplePlugin *prpl;

	if (dialog->account == NULL ||
	    !purple_strequal(purple_account_get_protocol_id(dialog->account), "prpl-jabber"))
		return NULL;
	prpl = purple_find_prpl("prpl-jabber");
	if (prpl == NULL || !purple_plugin_ipc_get_params(prpl, "bookmark-add", NULL, NULL, NULL))
		return NULL;
	return prpl;
}

static char *
room_serialize(PurpleRoomlist *list, PurpleRoomlistRoom *room)
{
	PurpleConnection *gc = purple_account_get_connection(list->account);
	PurplePluginProtocolInfo *prpl_info = NULL;

	if (gc != NULL)
		prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(gc->prpl);

	if (prpl_info != NULL && prpl_info->roomlist_room_serialize)
		return prpl_info->roomlist_room_serialize(room);
	return g_strdup(room->name);
}

/**************************************************************************
 * The dialog
 **************************************************************************/

static void
update_buttons(PidginRoomlistDialog *dialog)
{
	PurpleRoomlistRoom *room = selected_room(dialog);
	gboolean in_progress = dialog->roomlist != NULL &&
		purple_roomlist_get_in_progress(dialog->roomlist);
	gboolean connected = dialog->account != NULL &&
		purple_account_is_connected(dialog->account);

	gtk_widget_set_sensitive(dialog->account_widget, !in_progress);
	gtk_widget_set_sensitive(dialog->stop_button, in_progress);
	gtk_widget_set_sensitive(dialog->list_button, !in_progress && connected);
	gtk_widget_set_sensitive(dialog->add_button, room != NULL);
	gtk_widget_set_sensitive(dialog->join_button, room != NULL);
	gtk_widget_set_visible(dialog->bookmark_button, dialog->account != NULL &&
		purple_strequal(purple_account_get_protocol_id(dialog->account), "prpl-jabber"));
	gtk_widget_set_sensitive(dialog->bookmark_button,
		room != NULL && bookmark_prpl(dialog) != NULL);
}

static void
stop_pulse(PidginRoomlistDialog *dialog)
{
	if (dialog->pg_update_to > 0) {
		g_source_remove(dialog->pg_update_to);
		dialog->pg_update_to = 0;
		/* The pulse timeout held a reference. */
		if (dialog->roomlist)
			purple_roomlist_unref(dialog->roomlist);
	}
	dialog->pg_needs_pulse = FALSE;
	if (dialog->progress)
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(dialog->progress), 0.0);
}

/* Drops the dialog's room list (and its view). */
static void
drop_roomlist(PidginRoomlistDialog *dialog)
{
	PidginRoomlist *rl;

	if (dialog->roomlist == NULL)
		return;

	stop_pulse(dialog);
	rl = dialog->roomlist->ui_data;
	if (rl != NULL) {
		rl->dialog = NULL;
		if (rl->view != NULL && gtk_widget_get_parent(rl->view) != NULL)
			gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(dialog->sw), dialog->placeholder);
	}
	purple_roomlist_unref(dialog->roomlist);
	dialog->roomlist = NULL;
}

static void
dialog_destroy_cb(GtkWidget *w, PidginRoomlistDialog *dialog)
{
	dialogs = g_list_remove(dialogs, dialog);
	purple_signals_disconnect_by_handle(dialog);

	if (dialog->roomlist && purple_roomlist_get_in_progress(dialog->roomlist))
		purple_roomlist_cancel_get_list(dialog->roomlist);
	drop_roomlist(dialog);
	g_clear_object(&dialog->placeholder);
	dialog->progress = NULL;
	g_free(dialog);
}

static void
dialog_select_account_cb(GObject *dropdown, GParamSpec *pspec, PidginRoomlistDialog *dialog)
{
	PurpleAccount *account = pidgin_account_dropdown_get_selected(GTK_WIDGET(dropdown));
	gboolean change = (account != dialog->account);

	dialog->account = account;

	if (change)
		drop_roomlist(dialog);
	update_buttons(dialog);
}

static void
list_button_cb(GtkButton *button, PidginRoomlistDialog *dialog)
{
	PurpleConnection *gc;
	PidginRoomlist *rl;

	if (dialog->account == NULL)
		return;
	gc = purple_account_get_connection(dialog->account);
	if (!gc)
		return;

	drop_roomlist(dialog);

	dialog->roomlist = purple_roomlist_get_list(gc);
	if (!dialog->roomlist)
		return;
	purple_roomlist_ref(dialog->roomlist);
	rl = dialog->roomlist->ui_data;
	if (rl != NULL) {
		rl->dialog = dialog;
		if (rl->view != NULL)
			gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(dialog->sw), rl->view);
	}

	/* some protocols (not bundled with libpurple) finish getting their
	 * room list immediately */
	update_buttons(dialog);
}

static void
stop_button_cb(GtkButton *button, PidginRoomlistDialog *dialog)
{
	if (dialog->roomlist != NULL)
		purple_roomlist_cancel_get_list(dialog->roomlist);
	stop_pulse(dialog);
	update_buttons(dialog);
}

static void
close_button_cb(GtkButton *button, PidginRoomlistDialog *dialog)
{
	gtk_window_destroy(GTK_WINDOW(dialog->window));
}

static void
add_room_to_blist_cb(GtkButton *button, PidginRoomlistDialog *dialog)
{
	PurpleRoomlistRoom *room = selected_room(dialog);
	char *name;

	if (room == NULL)
		return;

	name = room_serialize(dialog->roomlist, room);
	purple_blist_request_add_chat(dialog->roomlist->account, NULL, NULL, name);
	g_free(name);
}

static void
join_button_cb(GtkButton *button, PidginRoomlistDialog *dialog)
{
	PurpleRoomlistRoom *room = selected_room(dialog);

	if (room != NULL)
		purple_roomlist_room_join(dialog->roomlist, room);
}

static void
bookmark_button_cb(GtkButton *button, PidginRoomlistDialog *dialog)
{
	PurpleRoomlistRoom *room = selected_room(dialog);
	PurplePlugin *prpl = bookmark_prpl(dialog);
	gboolean ok = FALSE;
	gpointer ret;
	char *jid;

	if (room == NULL || prpl == NULL)
		return;

	jid = room_serialize(dialog->roomlist, room);
	ret = purple_plugin_ipc_call(prpl, "bookmark-add", &ok, dialog->account, jid,
	                             (const char *)NULL, FALSE);
	if (!ok || !GPOINTER_TO_INT(ret))
		purple_notify_error(dialog, NULL, _("Unable to add the bookmark"), jid);
	g_free(jid);
}

static void
row_activated_cb(GtkColumnView *view, guint position, PurpleRoomlist *list)
{
	PidginRoomlist *rl = list->ui_data;
	PurpleRoomlistRoom *room;

	if (rl == NULL || rl->dialog == NULL)
		return;
	room = selected_room(rl->dialog);
	if (room != NULL)
		purple_roomlist_room_join(list, room);
}

static void
refresh_accounts(PidginRoomlistDialog *dialog)
{
	GtkWidget *tmp = pidgin_account_dropdown_new(NULL, FALSE,
		(PurpleFilterAccountFunc)account_filter_func, NULL);
	GListModel *model = gtk_drop_down_get_model(GTK_DROP_DOWN(tmp));
	PurpleAccount *account = dialog->account;

	g_object_ref_sink(tmp);
	g_signal_handlers_block_by_func(dialog->account_widget, dialog_select_account_cb, dialog);
	gtk_drop_down_set_model(GTK_DROP_DOWN(dialog->account_widget), model);
	if (account != NULL)
		pidgin_account_dropdown_set_selected(dialog->account_widget, account);
	g_signal_handlers_unblock_by_func(dialog->account_widget, dialog_select_account_cb, dialog);
	g_object_unref(tmp);

	dialog_select_account_cb(G_OBJECT(dialog->account_widget), NULL, dialog);
}

static void
connection_changed_cb(PurpleConnection *gc, PidginRoomlistDialog *dialog)
{
	refresh_accounts(dialog);
}

static PidginRoomlistDialog *
pidgin_roomlist_dialog_new_with_account(PurpleAccount *account)
{
	PidginRoomlistDialog *dialog;
	GtkWidget *window, *vbox;

	dialog = g_new0(PidginRoomlistDialog, 1);
	dialog->account = account;
	dialogs = g_list_prepend(dialogs, dialog);

	/* Create the window. */
	dialog->window = window = pidgin_dialog_new(_("Room List"), NULL, "room list", TRUE);
	gtk_window_set_default_size(GTK_WINDOW(window), 520, 400);
	g_signal_connect(window, "destroy", G_CALLBACK(dialog_destroy_cb), dialog);

	vbox = pidgin_dialog_get_content_area(window);

	/* accounts dropdown list */
	dialog->account_widget = pidgin_account_dropdown_new(dialog->account, FALSE,
		(PurpleFilterAccountFunc)account_filter_func, NULL);
	/* this is normally null, and we normally don't care what the first selected item is */
	dialog->account = pidgin_account_dropdown_get_selected(dialog->account_widget);
	pidgin_add_widget_to_vbox(GTK_BOX(vbox), _("_Account:"), NULL, dialog->account_widget,
	                          TRUE, NULL);

	/* scrolled window */
	dialog->placeholder = g_object_ref_sink(gtk_label_new(NULL));
	dialog->sw = pidgin_make_scrollable(dialog->placeholder, GTK_POLICY_AUTOMATIC,
	                                    GTK_POLICY_AUTOMATIC, -1, 250);
	gtk_widget_set_vexpand(dialog->sw, TRUE);
	gtk_box_append(GTK_BOX(vbox), dialog->sw);

	/* progress bar */
	dialog->progress = gtk_progress_bar_new();
	gtk_progress_bar_set_pulse_step(GTK_PROGRESS_BAR(dialog->progress), 0.1);
	gtk_box_append(GTK_BOX(vbox), dialog->progress);

	/* buttons */
	dialog->stop_button = pidgin_dialog_add_button(window, _("_Stop"),
		G_CALLBACK(stop_button_cb), dialog);
	dialog->list_button = pidgin_dialog_add_button(window, _("_Get List"),
		G_CALLBACK(list_button_cb), dialog);
	dialog->add_button = pidgin_dialog_add_button(window, _("_Add Chat"),
		G_CALLBACK(add_room_to_blist_cb), dialog);
	dialog->bookmark_button = pidgin_dialog_add_button(window, _("_Bookmark"),
		G_CALLBACK(bookmark_button_cb), dialog);
	gtk_widget_set_tooltip_text(dialog->bookmark_button,
		_("Add the room to the account's bookmarks on the server"));
	dialog->join_button = pidgin_dialog_add_button(window, _("_Join"),
		G_CALLBACK(join_button_cb), dialog);
	dialog->close_button = pidgin_dialog_add_button(window, _("_Close"),
		G_CALLBACK(close_button_cb), dialog);

	g_signal_connect(dialog->account_widget, "notify::selected",
	                 G_CALLBACK(dialog_select_account_cb), dialog);
	purple_signal_connect(purple_connections_get_handle(), "signed-on", dialog,
	                      PURPLE_CALLBACK(connection_changed_cb), dialog);
	purple_signal_connect(purple_connections_get_handle(), "signed-off", dialog,
	                      PURPLE_CALLBACK(connection_changed_cb), dialog);

	update_buttons(dialog);

	pidgin_window_set_secondary(GTK_WINDOW(window));
	gtk_window_present(GTK_WINDOW(window));
	return dialog;
}

void
pidgin_roomlist_dialog_show_with_account(PurpleAccount *account)
{
	PidginRoomlistDialog *dialog = pidgin_roomlist_dialog_new_with_account(account);

	if (!dialog)
		return;

	list_button_cb(GTK_BUTTON(dialog->list_button), dialog);
}

void
pidgin_roomlist_dialog_show(void)
{
	pidgin_roomlist_dialog_new_with_account(NULL);
}

/**************************************************************************
 * The room list view
 **************************************************************************/

static void
selection_changed_cb(GtkSelectionModel *model, guint position, guint n_items,
                     PurpleRoomlist *list)
{
	PidginRoomlist *rl = list->ui_data;

	if (rl != NULL && rl->dialog != NULL)
		update_buttons(rl->dialog);
}

/* A category is expanded through the prpl the first time it is opened. */
static void
row_expanded_cb(GtkTreeListRow *row, GParamSpec *pspec, PurpleRoomlist *list)
{
	PidginRoomNode *node;

	if (!gtk_tree_list_row_get_expanded(row))
		return;

	node = gtk_tree_list_row_get_item(row);
	if (node == NULL)
		return;
	if ((node->room->type & PURPLE_ROOMLIST_ROOMTYPE_CATEGORY) && !node->room->expanded_once) {
		node->room->expanded_once = TRUE;
		purple_roomlist_expand_category(list, node->room);
	}
	g_object_unref(node);
}

static void
cell_setup_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	GtkWidget *label = gtk_label_new(NULL);

	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_list_item_set_child(li, label);
}

static void
cell_bind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	GtkTreeListRow *row = gtk_list_item_get_item(li);
	PidginRoomNode *node = gtk_tree_list_row_get_item(row);
	guint index = GPOINTER_TO_UINT(data);
	char *text;

	if (node == NULL)
		return;
	text = field_text(node->list, node->room, index);
	gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(li)), text);
	g_free(text);
	g_object_unref(node);
}

static void
name_setup_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	GtkWidget *expander = gtk_tree_expander_new();
	GtkWidget *label = gtk_label_new(NULL);

	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), label);
	gtk_list_item_set_child(li, expander);
}

static void
name_bind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, PurpleRoomlist *list)
{
	GtkTreeListRow *row = gtk_list_item_get_item(li);
	GtkWidget *expander = gtk_list_item_get_child(li);
	GtkWidget *label = gtk_tree_expander_get_child(GTK_TREE_EXPANDER(expander));
	PidginRoomNode *node = gtk_tree_list_row_get_item(row);
	gulong id;

	gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(expander), row);
	if (node != NULL) {
		gtk_label_set_text(GTK_LABEL(label), node->room->name);
		g_object_unref(node);
	}
	id = g_signal_connect(row, "notify::expanded", G_CALLBACK(row_expanded_cb), list);
	g_object_set_data(G_OBJECT(li), "pidgin-expanded-id", GSIZE_TO_POINTER(id));
}

static void
name_unbind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, PurpleRoomlist *list)
{
	GtkTreeListRow *row = gtk_list_item_get_item(li);
	gulong id = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(li), "pidgin-expanded-id"));

	if (row != NULL && id != 0)
		g_signal_handler_disconnect(row, id);
	g_object_set_data(G_OBJECT(li), "pidgin-expanded-id", NULL);
	gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(gtk_list_item_get_child(li)), NULL);
}

/* Sorting: names alphabetically; ints backwards on purpose (as Pidgin 2:
 * the first click sorts users infinity-0). */
static int
name_compare_cb(gconstpointer a, gconstpointer b, gpointer data)
{
	const PidginRoomNode *na = a, *nb = b;
	char *ka = g_utf8_casefold(na->room->name ? na->room->name : "", -1);
	char *kb = g_utf8_casefold(nb->room->name ? nb->room->name : "", -1);
	int ret = g_utf8_collate(ka, kb);

	g_free(ka);
	g_free(kb);
	return ret;
}

static int
field_compare_cb(gconstpointer a, gconstpointer b, gpointer data)
{
	const PidginRoomNode *na = a, *nb = b;
	guint index = GPOINTER_TO_UINT(data);
	PurpleRoomlistField *f = g_list_nth_data(na->list->fields, index);
	gpointer va = g_list_nth_data(na->room->fields, index);
	gpointer vb = g_list_nth_data(nb->room->fields, index);

	if (f != NULL && f->type != PURPLE_ROOMLIST_FIELD_STRING) {
		int c = GPOINTER_TO_INT(va), d = GPOINTER_TO_INT(vb);
		return (c == d) ? 0 : (c > d) ? -1 : 1;
	}
	return g_utf8_collate(va ? (const char *)va : "", vb ? (const char *)vb : "");
}

static void
append_column(GtkWidget *view, const char *title, GtkListItemFactory *factory,
              GtkSorter *sorter, gboolean expand)
{
	GtkColumnViewColumn *column = gtk_column_view_column_new(title, factory);

	gtk_column_view_column_set_resizable(column, TRUE);
	gtk_column_view_column_set_expand(column, expand);
	gtk_column_view_column_set_sorter(column, sorter);
	g_object_unref(sorter);
	gtk_column_view_append_column(GTK_COLUMN_VIEW(view), column);
	g_object_unref(column);
}

static void
pidgin_roomlist_new(PurpleRoomlist *list)
{
	PidginRoomlist *rl = g_new0(PidginRoomlist, 1);

	list->ui_data = rl;
	rl->root = g_list_store_new(PIDGIN_TYPE_ROOM_NODE);
	rl->cats = g_hash_table_new_full(NULL, NULL, NULL, g_object_unref);
}

static void
pidgin_roomlist_set_fields(PurpleRoomlist *list, GList *fields)
{
	PidginRoomlist *rl = list->ui_data;
	GtkTreeListModel *tree;
	GtkSortListModel *sorted;
	GtkTreeListRowSorter *row_sorter;
	GtkListItemFactory *factory;
	GtkWidget *view;
	GList *l;
	guint j;

	g_return_if_fail(rl != NULL);
	g_return_if_fail(rl->view == NULL);

	tree = gtk_tree_list_model_new(G_LIST_MODEL(g_object_ref(rl->root)), FALSE, FALSE,
	                               create_children_cb, NULL, NULL);
	view = gtk_column_view_new(NULL);
	row_sorter = gtk_tree_list_row_sorter_new(
		g_object_ref(gtk_column_view_get_sorter(GTK_COLUMN_VIEW(view))));
	sorted = gtk_sort_list_model_new(G_LIST_MODEL(tree), GTK_SORTER(row_sorter));
	rl->selection = gtk_single_selection_new(G_LIST_MODEL(sorted));
	gtk_single_selection_set_autoselect(rl->selection, FALSE);
	gtk_single_selection_set_can_unselect(rl->selection, TRUE);
	g_signal_connect(rl->selection, "selection-changed", G_CALLBACK(selection_changed_cb), list);
	gtk_column_view_set_model(GTK_COLUMN_VIEW(view),
		GTK_SELECTION_MODEL(g_object_ref(rl->selection)));
	gtk_column_view_set_reorderable(GTK_COLUMN_VIEW(view), TRUE);

	factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(name_setup_cb), NULL);
	g_signal_connect(factory, "bind", G_CALLBACK(name_bind_cb), list);
	g_signal_connect(factory, "unbind", G_CALLBACK(name_unbind_cb), list);
	append_column(view, _("Name"), factory,
		GTK_SORTER(gtk_custom_sorter_new(name_compare_cb, NULL, NULL)), TRUE);

	for (j = 0, l = fields; l; l = l->next, j++) {
		PurpleRoomlistField *f = l->data;

		if (f->hidden)
			continue;

		factory = gtk_signal_list_item_factory_new();
		g_signal_connect(factory, "setup", G_CALLBACK(cell_setup_cb), NULL);
		g_signal_connect(factory, "bind", G_CALLBACK(cell_bind_cb), GUINT_TO_POINTER(j));
		append_column(view, f->label, factory,
			GTK_SORTER(gtk_custom_sorter_new(field_compare_cb, GUINT_TO_POINTER(j), NULL)),
			f->type == PURPLE_ROOMLIST_FIELD_STRING);
	}

	g_signal_connect(view, "activate", G_CALLBACK(row_activated_cb), list);
	rl->view = g_object_ref_sink(view);

	/* A list whose fields arrive after purple_roomlist_get_list() */
	if (rl->dialog != NULL)
		gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(rl->dialog->sw), rl->view);
}

static gboolean
pidgin_progress_bar_pulse(gpointer data)
{
	PurpleRoomlist *list = data;
	PidginRoomlist *rl = list->ui_data;

	if (!rl || !rl->dialog || !rl->dialog->pg_needs_pulse) {
		if (rl && rl->dialog) {
			rl->dialog->pg_update_to = 0;
			rl->dialog->pg_needs_pulse = FALSE;
		}
		purple_roomlist_unref(list);
		return G_SOURCE_REMOVE;
	}

	gtk_progress_bar_pulse(GTK_PROGRESS_BAR(rl->dialog->progress));
	rl->dialog->pg_needs_pulse = FALSE;
	return G_SOURCE_CONTINUE;
}

static void
pidgin_roomlist_add_room(PurpleRoomlist *list, PurpleRoomlistRoom *room)
{
	PidginRoomlist *rl = list->ui_data;
	PidginRoomNode *node;
	GListStore *parent = NULL;

	g_return_if_fail(rl != NULL);

	rl->total_rooms++;
	if (room->type == PURPLE_ROOMLIST_ROOMTYPE_ROOM)
		rl->num_rooms++;

	if (rl->dialog) {
		if (rl->dialog->pg_update_to == 0) {
			purple_roomlist_ref(list);
			rl->dialog->pg_update_to = g_timeout_add(100, pidgin_progress_bar_pulse, list);
			gtk_progress_bar_pulse(GTK_PROGRESS_BAR(rl->dialog->progress));
		} else
			rl->dialog->pg_needs_pulse = TRUE;
	}

	if (room->parent != NULL)
		parent = g_hash_table_lookup(rl->cats, room->parent);
	if (parent == NULL)
		parent = rl->root;

	node = g_object_new(PIDGIN_TYPE_ROOM_NODE, NULL);
	node->list = list;
	node->room = room;
	if (room->type & PURPLE_ROOMLIST_ROOMTYPE_CATEGORY) {
		node->children = g_list_store_new(PIDGIN_TYPE_ROOM_NODE);
		g_hash_table_insert(rl->cats, room, g_object_ref(node->children));
	}
	g_list_store_append(parent, node);
	g_object_unref(node);
}

static void
pidgin_roomlist_in_progress(PurpleRoomlist *list, gboolean in_progress)
{
	PidginRoomlist *rl = list->ui_data;

	if (!rl || !rl->dialog)
		return;

	if (!in_progress) {
		rl->dialog->pg_needs_pulse = FALSE;
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(rl->dialog->progress), 0.0);
	}
	update_buttons(rl->dialog);
}

static void
pidgin_roomlist_destroy(PurpleRoomlist *list)
{
	PidginRoomlist *rl = list->ui_data;

	g_return_if_fail(rl != NULL);

	if (rl->view != NULL) {
		GtkWidget *parent = gtk_widget_get_parent(rl->view);
		if (parent != NULL && rl->dialog != NULL)
			gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(rl->dialog->sw),
			                              rl->dialog->placeholder);
		g_clear_object(&rl->view);
	}
	g_clear_object(&rl->selection);
	g_hash_table_destroy(rl->cats);
	g_clear_object(&rl->root);
	g_free(rl);
	list->ui_data = NULL;
}

static PurpleRoomlistUiOps ops = {
	pidgin_roomlist_dialog_show_with_account,
	pidgin_roomlist_new,
	pidgin_roomlist_set_fields,
	pidgin_roomlist_add_room,
	pidgin_roomlist_in_progress,
	pidgin_roomlist_destroy,
	NULL,
	NULL,
	NULL,
	NULL
};

void
pidgin_roomlist_init(void)
{
	purple_roomlist_set_ui_ops(&ops);
}

void
pidgin_roomlist_uninit(void)
{
	while (dialogs != NULL) {
		PidginRoomlistDialog *dialog = dialogs->data;
		/* The destroy handler removes it from the list. */
		gtk_window_destroy(GTK_WINDOW(dialog->window));
	}
}

/**************************************************************************
 * Selftest
 **************************************************************************/

static PurpleRoomlistRoom *
selftest_add_room(PurpleRoomlist *list, PurpleRoomlistRoomType type, const char *name,
                  PurpleRoomlistRoom *parent, const char *topic, int users)
{
	PurpleRoomlistRoom *room = purple_roomlist_room_new(type, name, parent);

	purple_roomlist_room_add_field(list, room, "hidden-id");
	purple_roomlist_room_add_field(list, room, topic);
	purple_roomlist_room_add_field(list, room, GINT_TO_POINTER(users));
	purple_roomlist_room_add(list, room);
	return room;
}

/* A room list built here (no prpl involved), shown in @dialog: checks the
 * tree, the columns, selection and the buttons, then drops it. */
static void
selftest_local_list(PidginRoomlistDialog *dialog, PurpleAccount *account)
{
	PurpleRoomlist *list = purple_roomlist_new(account);
	PidginRoomlist *rl = list->ui_data;
	PurpleRoomlistRoom *cat;
	GList *fields = NULL;
	GListModel *model;
	GtkTreeListRow *row;
	guint n, i, n_columns;

	fields = g_list_append(fields, purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_STRING,
		"", "id", TRUE));
	fields = g_list_append(fields, purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_STRING,
		_("Topic"), "topic", FALSE));
	fields = g_list_append(fields, purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_INT,
		_("Users"), "users", FALSE));
	purple_roomlist_set_fields(list, fields);
	if (rl == NULL || rl->view == NULL) {
		pidgin_selftest_fail("roomlist", "set_fields made no view");
		purple_roomlist_unref(list);
		return;
	}

	/* Attach it as list_button_cb() does (the dialog takes the reference). */
	dialog->roomlist = list;
	rl->dialog = dialog;
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(dialog->sw), rl->view);
	purple_roomlist_set_in_progress(list, TRUE);

	cat = selftest_add_room(list, PURPLE_ROOMLIST_ROOMTYPE_CATEGORY, "Category", NULL, "", 0);
	selftest_add_room(list, PURPLE_ROOMLIST_ROOMTYPE_ROOM, "inner", cat, "inside", 3);
	selftest_add_room(list, PURPLE_ROOMLIST_ROOMTYPE_ROOM, "alpha", NULL, "first room", 12);
	purple_roomlist_set_in_progress(list, FALSE);
	pidgin_selftest_iterate(200);

	n_columns = g_list_model_get_n_items(gtk_column_view_get_columns(GTK_COLUMN_VIEW(rl->view)));
	if (n_columns != 3)
		pidgin_selftest_fail("roomlist", "%u columns, expected Name, Topic, Users", n_columns);

	model = G_LIST_MODEL(rl->selection);
	n = g_list_model_get_n_items(model);
	if (n != 2)
		pidgin_selftest_fail("roomlist", "%u top-level rows, expected 2", n);

	/* Open the category (marked as expanded once: no prpl call). */
	cat->expanded_once = TRUE;
	for (i = 0; i < n; i++) {
		PidginRoomNode *node;

		row = g_list_model_get_item(model, i);
		node = gtk_tree_list_row_get_item(row);
		if (node->room == cat)
			gtk_tree_list_row_set_expanded(row, TRUE);
		g_object_unref(node);
		g_object_unref(row);
	}
	pidgin_selftest_iterate(100);
	n = g_list_model_get_n_items(model);
	if (n != 3)
		pidgin_selftest_fail("roomlist", "%u rows with the category open, expected 3", n);

	/* A room enables Join and Add Chat; the category does not. */
	for (i = 0; i < n; i++) {
		PurpleRoomlistRoom *room;

		gtk_single_selection_set_selected(rl->selection, i);
		pidgin_selftest_iterate(20);
		room = selected_room(dialog);
		if (gtk_widget_get_sensitive(dialog->join_button) != (room != NULL))
			pidgin_selftest_fail("roomlist", "row %u: Join sensitivity is wrong", i);
	}
	pidgin_selftest_log("roomlist", "local list: %u rows, %u columns, %d rooms",
	                    n, n_columns, rl->num_rooms);

	/* The dialog drops its reference; the list is destroyed. */
	drop_roomlist(dialog);
	update_buttons(dialog);
	pidgin_selftest_iterate(50);
}

void
pidgin_roomlist_selftest(void)
{
	PidginRoomlistDialog *dialog;
	guint n_accounts;

	pidgin_roomlist_dialog_show();
	pidgin_selftest_iterate(300);
	if (dialogs == NULL) {
		pidgin_selftest_fail("roomlist", "the dialog did not open");
		return;
	}
	dialog = dialogs->data;

	n_accounts = g_list_model_get_n_items(
		gtk_drop_down_get_model(GTK_DROP_DOWN(dialog->account_widget)));
	pidgin_selftest_log("roomlist", "%u account(s) with a room list; showable: %s",
	                    n_accounts, pidgin_roomlist_is_showable() ? "yes" : "no");
	if ((n_accounts > 0) != pidgin_roomlist_is_showable())
		pidgin_selftest_fail("roomlist", "the account list and is_showable() disagree");

	if (dialog->account == NULL) {
		/* The empty state: nothing to list, join or add. */
		if (gtk_widget_get_sensitive(dialog->list_button))
			pidgin_selftest_fail("roomlist", "Get List is sensitive without an account");
		if (gtk_widget_get_sensitive(dialog->join_button) ||
		    gtk_widget_get_sensitive(dialog->add_button) ||
		    gtk_widget_get_sensitive(dialog->stop_button))
			pidgin_selftest_fail("roomlist", "room buttons are sensitive without a list");
		/* Harmless without an account. */
		g_signal_emit_by_name(dialog->list_button, "clicked");
		pidgin_selftest_iterate(50);
		if (dialog->roomlist != NULL)
			pidgin_selftest_fail("roomlist", "a room list without an account");
	}

	if (purple_accounts_get_all() != NULL)
		selftest_local_list(dialog, purple_accounts_get_all()->data);

	g_signal_emit_by_name(dialog->close_button, "clicked");
	pidgin_selftest_iterate(100);
	if (dialogs != NULL)
		pidgin_selftest_fail("roomlist", "the dialog is still open");
}
