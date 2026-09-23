/*
 * pidgin4: the XMPP Service Discovery window (M7 port of
 * pidgin/plugins/disco/gtkdisco.c).
 *
 * Purple - XMPP Service Disco Browser
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA
 *
 */

/*
 * The GtkTreeView/GtkTreeStore of Pidgin 2 is a GtkColumnView over a
 * GtkTreeListModel of PidginDiscoNode items (each wraps an
 * XmppDiscoService; browsable ones have a GListStore of children, filled
 * as the disco#items replies arrive). Expanding a row asks for its items,
 * as before. Double click (activate) expands, registers or adds; the row
 * menu (right click) has Add to Buddy List and Register. The account
 * drop-down lists the connected XMPP accounts.
 */
#include "pidgin4-plugin.h"

#include "gtkdisco.h"
#include "xmppdisco.h"

static GList *dialogs = NULL;

/**************************************************************************
 * PidginDiscoNode: a row
 **************************************************************************/

#define PIDGIN_TYPE_DISCO_NODE (pidgin_disco_node_get_type())
G_DECLARE_FINAL_TYPE(PidginDiscoNode, pidgin_disco_node, PIDGIN, DISCO_NODE, GObject)

struct _PidginDiscoNode
{
	GObject parent;
	XmppDiscoService *service;   /* not owned (xmppdisco.c's) */
	GListStore *children;        /* browsable services only */
};

G_DEFINE_FINAL_TYPE(PidginDiscoNode, pidgin_disco_node, G_TYPE_OBJECT)

static void
pidgin_disco_node_finalize(GObject *obj)
{
	g_clear_object(&PIDGIN_DISCO_NODE(obj)->children);
	G_OBJECT_CLASS(pidgin_disco_node_parent_class)->finalize(obj);
}

static void
pidgin_disco_node_class_init(PidginDiscoNodeClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = pidgin_disco_node_finalize;
}

static void
pidgin_disco_node_init(PidginDiscoNode *node)
{
}

static PidginDiscoNode *
pidgin_disco_node_new(XmppDiscoService *service)
{
	PidginDiscoNode *node = g_object_new(PIDGIN_TYPE_DISCO_NODE, NULL);

	node->service = service;
	if (service->flags & XMPP_DISCO_BROWSE)
		node->children = g_list_store_new(PIDGIN_TYPE_DISCO_NODE);
	return node;
}

/**************************************************************************
 * Lists
 **************************************************************************/

static void
remove_tree(PidginDiscoList *list)
{
	if (list->tree != NULL) {
		if (list->dialog != NULL && list->dialog->sw != NULL &&
		    gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(list->dialog->sw)) == list->tree)
			gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(list->dialog->sw), NULL);
		list->tree = NULL;
	}
}

static void
pidgin_disco_list_destroy(PidginDiscoList *list)
{
	g_hash_table_destroy(list->services);
	if (list->dialog && list->dialog->discolist == list)
		list->dialog->discolist = NULL;

	remove_tree(list);
	g_clear_object(&list->selection);
	g_clear_object(&list->model);

	g_free((gchar*)list->server);
	g_free(list);
}

PidginDiscoList *pidgin_disco_list_ref(PidginDiscoList *list)
{
	g_return_val_if_fail(list != NULL, NULL);

	++list->ref;
	purple_debug_misc("xmppdisco", "reffing list, ref count now %d\n", list->ref);

	return list;
}

void pidgin_disco_list_unref(PidginDiscoList *list)
{
	g_return_if_fail(list != NULL);

	--list->ref;

	purple_debug_misc("xmppdisco", "unreffing list, ref count now %d\n", list->ref);
	if (list->ref == 0)
		pidgin_disco_list_destroy(list);
}

void pidgin_disco_list_set_in_progress(PidginDiscoList *list, gboolean in_progress)
{
	PidginDiscoDialog *dialog = list->dialog;

	if (!dialog)
		return;

	list->in_progress = in_progress;

	if (in_progress) {
		gtk_widget_set_sensitive(dialog->account_widget, FALSE);
		gtk_widget_set_sensitive(dialog->stop_button, TRUE);
		gtk_widget_set_sensitive(dialog->browse_button, FALSE);
	} else {
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(dialog->progress), 0.0);

		gtk_widget_set_sensitive(dialog->account_widget, TRUE);

		gtk_widget_set_sensitive(dialog->stop_button, FALSE);
		gtk_widget_set_sensitive(dialog->browse_button, TRUE);
	}
}

static GIcon *
pidgin_disco_load_icon(XmppDiscoService *service)
{
	g_return_val_if_fail(service != NULL, NULL);

	if (service->type == XMPP_DISCO_SERVICE_TYPE_GATEWAY && service->gateway_type) {
		char *name = g_strdup_printf("pidgin4-protocol-%s", service->gateway_type);
		const char *names[] = { name, "network-server", NULL };
		GIcon *icon = g_themed_icon_new_from_names((char **)names, -1);

		g_free(name);
		return icon;
	} else if (service->type == XMPP_DISCO_SERVICE_TYPE_CHAT)
		return g_themed_icon_new("system-users");
	else if (service->type == XMPP_DISCO_SERVICE_TYPE_DIRECTORY)
		return g_themed_icon_new("system-search");

	return NULL;
}

static void pidgin_disco_create_tree(PidginDiscoList *pdl);

static void
discard_list(PidginDiscoDialog *dialog)
{
	if (dialog->discolist) {
		remove_tree(dialog->discolist);
		pidgin_disco_list_unref(dialog->discolist);
		dialog->discolist = NULL;
	}
	dialog->selected = NULL;
}

static void dialog_select_account_cb(GObject *w, GParamSpec *pspec,
                                     PidginDiscoDialog *dialog)
{
	PurpleAccount *account = pidgin_account_dropdown_get_selected(dialog->account_widget);
	gboolean change = (account != dialog->account);

	dialog->account = account;
	gtk_widget_set_sensitive(dialog->browse_button, account != NULL);

	if (change)
		discard_list(dialog);
}

static void register_button_cb(GtkWidget *unused, PidginDiscoDialog *dialog)
{
	if (dialog->selected != NULL)
		xmpp_disco_service_register(dialog->selected);
}

static void discolist_cancel_cb(PidginDiscoList *pdl, const char *server)
{
	pdl->dialog->prompt_handle = NULL;

	pidgin_disco_list_set_in_progress(pdl, FALSE);
	pidgin_disco_list_unref(pdl);
}

static void discolist_ok_cb(PidginDiscoList *pdl, const char *server)
{
	pdl->dialog->prompt_handle = NULL;
	gtk_widget_set_sensitive(pdl->dialog->browse_button, TRUE);

	if (!server || !*server) {
		purple_notify_error(my_plugin, _("Invalid Server"), _("Invalid Server"),
		                    NULL);

		pidgin_disco_list_set_in_progress(pdl, FALSE);
		pidgin_disco_list_unref(pdl);
		return;
	}

	pdl->server = g_strdup(server);
	pidgin_disco_list_set_in_progress(pdl, TRUE);
	xmpp_disco_start(pdl);
}

static void browse_button_cb(GtkWidget *button, PidginDiscoDialog *dialog)
{
	PurpleConnection *pc;
	PidginDiscoList *pdl;
	const char *username;
	const char *at, *slash;
	char *server = NULL;

	pc = purple_account_get_connection(dialog->account);
	if (!pc)
		return;

	gtk_widget_set_sensitive(dialog->browse_button, FALSE);
	gtk_widget_set_sensitive(dialog->add_button, FALSE);
	gtk_widget_set_sensitive(dialog->register_button, FALSE);

	discard_list(dialog);

	pdl = dialog->discolist = g_new0(PidginDiscoList, 1);
	pdl->services = g_hash_table_new_full(NULL, NULL, NULL, g_object_unref);
	pdl->pc = pc;
	/* We keep a copy... */
	pidgin_disco_list_ref(pdl);

	pdl->dialog = dialog;
	pidgin_disco_create_tree(pdl);

	if (dialog->account_widget)
		gtk_widget_set_sensitive(dialog->account_widget, FALSE);

	username = purple_account_get_username(dialog->account);
	at = strchr(username, '@');
	slash = strchr(username, '/');
	if (at && !slash) {
		server = g_strdup_printf("%s", at + 1);
	} else if (at && slash && at + 1 < slash) {
		server = g_strdup_printf("%.*s", (int)(slash - (at + 1)), at + 1);
	}

	if (server == NULL)
		/* This shouldn't ever happen since the account is connected */
		server = g_strdup("jabber.org");

	/* Note to translators: The string "Enter an XMPP Server" is asking the
	   user to type the name of an XMPP server which will then be queried */
	dialog->prompt_handle = purple_request_input(my_plugin, _("Server name request"), _("Enter an XMPP Server"),
			_("Select an XMPP server to query"),
			server, FALSE, FALSE, NULL,
			_("Find Services"), PURPLE_CALLBACK(discolist_ok_cb),
			_("Cancel"), PURPLE_CALLBACK(discolist_cancel_cb),
			purple_connection_get_account(pc), NULL, NULL, pdl);

	g_free(server);
}

static void add_to_blist_cb(GtkWidget *unused, PidginDiscoDialog *dialog)
{
	XmppDiscoService *service = dialog->selected;
	PurpleAccount *account;
	const char *jid;

	g_return_if_fail(service != NULL);

	account = purple_connection_get_account(service->list->pc);
	jid = service->jid;

	if (service->type == XMPP_DISCO_SERVICE_TYPE_CHAT)
		purple_blist_request_add_chat(account, NULL, NULL, jid);
	else
		purple_blist_request_add_buddy(account, jid, NULL, NULL);
}

/**************************************************************************
 * The tree
 **************************************************************************/

static GListModel *
create_children_cb(gpointer item, gpointer data)
{
	PidginDiscoNode *node = item;

	/* (GtkTreeListModel also calls this to learn whether a row can be
	 * expanded; the items are asked for in row_expanded_cb.) */
	if (node->children == NULL)
		return NULL;
	return G_LIST_MODEL(g_object_ref(node->children));
}

static void
row_expanded_cb(GtkTreeListRow *row, GParamSpec *pspec, gpointer data)
{
	PidginDiscoNode *node;

	if (!gtk_tree_list_row_get_expanded(row))
		return;
	node = gtk_tree_list_row_get_item(row);
	/* ask for its items (once) */
	if (node->children != NULL && !node->service->expanded)
		xmpp_disco_service_expand(node->service);
	g_object_unref(node);
}

static PidginDiscoNode *
node_at(PidginDiscoList *pdl, guint position)
{
	GtkTreeListRow *row = g_list_model_get_item(G_LIST_MODEL(pdl->selection), position);
	PidginDiscoNode *node;

	if (row == NULL)
		return NULL;
	node = gtk_tree_list_row_get_item(row);
	g_object_unref(row);
	if (node != NULL)
		g_object_unref(node);  /* the store keeps it */
	return node;
}

static void
update_buttons(PidginDiscoDialog *dialog)
{
	XmppDiscoService *s = dialog->selected;

	gtk_widget_set_sensitive(dialog->add_button, s != NULL && (s->flags & XMPP_DISCO_ADD));
	gtk_widget_set_sensitive(dialog->register_button,
	                         s != NULL && (s->flags & XMPP_DISCO_REGISTER));
	g_simple_action_set_enabled(G_SIMPLE_ACTION(g_action_map_lookup_action(
		G_ACTION_MAP(dialog->actions), "add")), s != NULL && (s->flags & XMPP_DISCO_ADD));
	g_simple_action_set_enabled(G_SIMPLE_ACTION(g_action_map_lookup_action(
		G_ACTION_MAP(dialog->actions), "register")), s != NULL && (s->flags & XMPP_DISCO_REGISTER));
}

static void
selection_changed_cb(GtkSelectionModel *selection, guint position, guint n_items,
                     PidginDiscoList *pdl)
{
	PidginDiscoDialog *dialog = pdl->dialog;
	guint selected;
	PidginDiscoNode *node;

	if (dialog == NULL)
		return;
	selected = gtk_single_selection_get_selected(GTK_SINGLE_SELECTION(selection));
	node = selected != GTK_INVALID_LIST_POSITION ? node_at(pdl, selected) : NULL;
	dialog->selected = node ? node->service : NULL;
	update_buttons(dialog);
}

static void
row_activated_cb(GtkColumnView *view, guint position, PidginDiscoList *pdl)
{
	GtkTreeListRow *row = g_list_model_get_item(G_LIST_MODEL(pdl->selection), position);
	PidginDiscoNode *node;
	XmppDiscoService *service;

	if (row == NULL)
		return;
	node = gtk_tree_list_row_get_item(row);
	service = node->service;
	pdl->dialog->selected = service;

	if (service->flags & XMPP_DISCO_BROWSE)
		gtk_tree_list_row_set_expanded(row, !gtk_tree_list_row_get_expanded(row));
	else if (service->flags & XMPP_DISCO_REGISTER)
		register_button_cb(NULL, pdl->dialog);
	else if (service->flags & XMPP_DISCO_ADD)
		add_to_blist_cb(NULL, pdl->dialog);

	g_object_unref(node);
	g_object_unref(row);
}

static char *
service_tooltip(XmppDiscoService *service)
{
	const char *type = NULL;
	char *markup, *jid, *name, *desc = NULL;

	switch (service->type) {
		case XMPP_DISCO_SERVICE_TYPE_UNSET:
			type = _("Unknown");
			break;
		case XMPP_DISCO_SERVICE_TYPE_GATEWAY:
			type = _("Gateway");
			break;
		case XMPP_DISCO_SERVICE_TYPE_DIRECTORY:
			type = _("Directory");
			break;
		case XMPP_DISCO_SERVICE_TYPE_CHAT:
			type = _("Chat");
			break;
		case XMPP_DISCO_SERVICE_TYPE_PUBSUB_COLLECTION:
			type = _("PubSub Collection");
			break;
		case XMPP_DISCO_SERVICE_TYPE_PUBSUB_LEAF:
			type = _("PubSub Leaf");
			break;
		case XMPP_DISCO_SERVICE_TYPE_OTHER:
			type = _("Other");
			break;
	}

	markup = g_strdup_printf("<span size='x-large' weight='bold'>%s</span>\n<b>%s:</b> %s%s%s",
	                         name = g_markup_escape_text(service->name ? service->name : "", -1),
	                         type,
	                         jid = g_markup_escape_text(service->jid ? service->jid : "", -1),
	                         service->description ? _("\n<b>Description:</b> ") : "",
	                         service->description ? desc = g_markup_escape_text(service->description, -1) : "");
	g_free(jid);
	g_free(name);
	g_free(desc);
	return markup;
}

static void
setup_name_cb(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
	GtkWidget *expander = gtk_tree_expander_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *image = gtk_image_new();
	GtkWidget *label = gtk_label_new(NULL);

	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_box_append(GTK_BOX(box), image);
	gtk_box_append(GTK_BOX(box), label);
	gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), box);
	gtk_list_item_set_child(item, expander);
}

static void
bind_name_cb(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
	GtkWidget *expander = gtk_list_item_get_child(item);
	GtkTreeListRow *row = gtk_list_item_get_item(item);
	PidginDiscoNode *node = gtk_tree_list_row_get_item(row);
	GtkWidget *box = gtk_tree_expander_get_child(GTK_TREE_EXPANDER(expander));
	GtkWidget *image = gtk_widget_get_first_child(box);
	GtkWidget *label = gtk_widget_get_next_sibling(image);
	GIcon *icon = pidgin_disco_load_icon(node->service);
	char *tip = service_tooltip(node->service);

	gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(expander), row);
	g_object_set_data(G_OBJECT(expander), "disco-expanded-id",
		GSIZE_TO_POINTER(g_signal_connect(row, "notify::expanded",
		                                  G_CALLBACK(row_expanded_cb), NULL)));
	gtk_image_set_from_gicon(GTK_IMAGE(image), icon);
	gtk_widget_set_visible(image, icon != NULL);
	gtk_label_set_text(GTK_LABEL(label), node->service->name);
	gtk_widget_set_tooltip_markup(expander, tip);

	g_free(tip);
	if (icon)
		g_object_unref(icon);
	g_object_unref(node);
}

static void
unbind_name_cb(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
	GtkWidget *expander = gtk_list_item_get_child(item);
	GtkTreeListRow *row = gtk_list_item_get_item(item);
	gulong id = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(expander), "disco-expanded-id"));

	if (id != 0 && row != NULL)
		g_signal_handler_disconnect(row, id);
	g_object_set_data(G_OBJECT(expander), "disco-expanded-id", NULL);
	gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(expander), NULL);
}

static void
setup_desc_cb(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
	GtkWidget *label = gtk_label_new(NULL);

	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_list_item_set_child(item, label);
}

static void
bind_desc_cb(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
	GtkTreeListRow *row = gtk_list_item_get_item(item);
	PidginDiscoNode *node = gtk_tree_list_row_get_item(row);

	gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)),
	                   node->service->description ? node->service->description : "");
	g_object_unref(node);
}

static void
service_click_cb(GtkGestureClick *gesture, int n_press, double x, double y,
                 PidginDiscoList *pdl)
{
	PidginDiscoDialog *dialog = pdl->dialog;
	XmppDiscoService *service;
	GMenu *menu;

	if (dialog == NULL || (service = dialog->selected) == NULL)
		return;

	menu = g_menu_new();
	if (service->flags & XMPP_DISCO_ADD)
		g_menu_append(menu, _("Add to Buddy List"), "disco.add");
	if (service->flags & XMPP_DISCO_REGISTER)
		g_menu_append(menu, _("Register"), "disco.register");
	if (g_menu_model_get_n_items(G_MENU_MODEL(menu)) > 0) {
		GdkRectangle rect = { (int)x, (int)y, 1, 1 };

		gtk_popover_menu_set_menu_model(GTK_POPOVER_MENU(dialog->menu), G_MENU_MODEL(menu));
		if (gtk_widget_get_parent(dialog->menu) != pdl->tree) {
			if (gtk_widget_get_parent(dialog->menu) != NULL)
				gtk_widget_unparent(dialog->menu);
			gtk_widget_set_parent(dialog->menu, pdl->tree);
		}
		gtk_popover_set_pointing_to(GTK_POPOVER(dialog->menu), &rect);
		gtk_popover_popup(GTK_POPOVER(dialog->menu));
	}
	g_object_unref(menu);
}

static void pidgin_disco_create_tree(PidginDiscoList *pdl)
{
	GtkTreeListModel *tree_model;
	GtkListItemFactory *factory;
	GtkColumnViewColumn *column;
	GtkGesture *gesture;

	pdl->model = g_list_store_new(PIDGIN_TYPE_DISCO_NODE);
	tree_model = gtk_tree_list_model_new(G_LIST_MODEL(g_object_ref(pdl->model)), FALSE, FALSE,
	                                     create_children_cb, pdl, NULL);
	pdl->selection = gtk_single_selection_new(G_LIST_MODEL(tree_model));
	gtk_single_selection_set_autoselect(pdl->selection, FALSE);
	gtk_single_selection_set_can_unselect(pdl->selection, TRUE);
	g_signal_connect(pdl->selection, "selection-changed", G_CALLBACK(selection_changed_cb), pdl);

	pdl->tree = gtk_column_view_new(GTK_SELECTION_MODEL(g_object_ref(pdl->selection)));
	gtk_column_view_set_show_row_separators(GTK_COLUMN_VIEW(pdl->tree), TRUE);
	gtk_column_view_set_reorderable(GTK_COLUMN_VIEW(pdl->tree), TRUE);

	factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(setup_name_cb), pdl);
	g_signal_connect(factory, "bind", G_CALLBACK(bind_name_cb), pdl);
	g_signal_connect(factory, "unbind", G_CALLBACK(unbind_name_cb), pdl);
	column = gtk_column_view_column_new(_("Name"), factory);
	gtk_column_view_column_set_resizable(column, TRUE);
	gtk_column_view_column_set_expand(column, TRUE);
	gtk_column_view_append_column(GTK_COLUMN_VIEW(pdl->tree), column);
	g_object_unref(column);

	factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(setup_desc_cb), pdl);
	g_signal_connect(factory, "bind", G_CALLBACK(bind_desc_cb), pdl);
	column = gtk_column_view_column_new(_("Description"), factory);
	gtk_column_view_column_set_resizable(column, TRUE);
	gtk_column_view_column_set_expand(column, TRUE);
	gtk_column_view_append_column(GTK_COLUMN_VIEW(pdl->tree), column);
	g_object_unref(column);

	g_signal_connect(pdl->tree, "activate", G_CALLBACK(row_activated_cb), pdl);

	gesture = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), GDK_BUTTON_SECONDARY);
	g_signal_connect(gesture, "pressed", G_CALLBACK(service_click_cb), pdl);
	gtk_widget_add_controller(pdl->tree, GTK_EVENT_CONTROLLER(gesture));

	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(pdl->dialog->sw), pdl->tree);
}

void pidgin_disco_signed_off_cb(PurpleConnection *pc)
{
	GList *node;

	for (node = dialogs; node; node = node->next) {
		PidginDiscoDialog *dialog = node->data;
		PidginDiscoList *list = dialog->discolist;

		if (list && list->pc == pc) {
			if (list->in_progress)
				pidgin_disco_list_set_in_progress(list, FALSE);

			discard_list(dialog);

			gtk_widget_set_sensitive(dialog->browse_button,
					pidgin_account_dropdown_get_selected(dialog->account_widget) != NULL);

			gtk_widget_set_sensitive(dialog->register_button, FALSE);
			gtk_widget_set_sensitive(dialog->add_button, FALSE);
		}
	}
}

void pidgin_disco_dialogs_destroy_all(void)
{
	while (dialogs) {
		PidginDiscoDialog *dialog = dialogs->data;

		gtk_window_destroy(GTK_WINDOW(dialog->window));
		/* destroy_win_cb removes the dialog from the list */
	}
}

/**************************************************************************
 * The window
 **************************************************************************/

static void
destroy_win_cb(GtkWidget *window, gpointer d)
{
	PidginDiscoDialog *dialog = d;
	PidginDiscoList *list = dialog->discolist;

	if (dialog->prompt_handle)
		purple_request_close(PURPLE_REQUEST_INPUT, dialog->prompt_handle);

	if (list) {
		if (list->in_progress)
			list->in_progress = FALSE;
		remove_tree(list);
		list->dialog = NULL;

		pidgin_disco_list_unref(list);
	}
	if (dialog->menu != NULL && gtk_widget_get_parent(dialog->menu) != NULL)
		gtk_widget_unparent(dialog->menu);
	g_clear_object(&dialog->menu);
	g_clear_object(&dialog->actions);

	dialogs = g_list_remove(dialogs, d);
	g_free(dialog);
}

static void stop_button_cb(GtkButton *button, PidginDiscoDialog *dialog)
{
	if (dialog->discolist != NULL)
		pidgin_disco_list_set_in_progress(dialog->discolist, FALSE);
}

static void close_button_cb(GtkButton *button, PidginDiscoDialog *dialog)
{
	gtk_window_destroy(GTK_WINDOW(dialog->window));
}

static gboolean account_filter_func(PurpleAccount *account)
{
	return purple_strequal(purple_account_get_protocol_id(account), XMPP_PLUGIN_ID);
}

static void
add_action_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	add_to_blist_cb(NULL, data);
}

static void
register_action_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	register_button_cb(NULL, data);
}

PidginDiscoDialog *pidgin_disco_dialog_new(void)
{
	PidginDiscoDialog *dialog;
	GtkWidget *window, *vbox;
	const GActionEntry entries[] = {
		{ .name = "add", .activate = add_action_cb },
		{ .name = "register", .activate = register_action_cb },
	};

	dialog = g_new0(PidginDiscoDialog, 1);
	dialogs = g_list_prepend(dialogs, dialog);

	/* Create the window. */
	dialog->window = window = pidgin_dialog_new(_("Service Discovery"), NULL,
	                                            "service discovery", TRUE);
	gtk_window_set_default_size(GTK_WINDOW(window), 560, 420);

	g_signal_connect(window, "destroy", G_CALLBACK(destroy_win_cb), dialog);

	dialog->actions = g_simple_action_group_new();
	g_action_map_add_action_entries(G_ACTION_MAP(dialog->actions), entries,
	                                G_N_ELEMENTS(entries), dialog);
	gtk_widget_insert_action_group(window, "disco", G_ACTION_GROUP(dialog->actions));
	dialog->menu = g_object_ref_sink(gtk_popover_menu_new_from_model(NULL));
	gtk_popover_set_has_arrow(GTK_POPOVER(dialog->menu), FALSE);

	/* Create the parent vbox for everything. */
	vbox = pidgin_dialog_get_content_area(window);

	/* accounts dropdown list (connected XMPP accounts) */
	dialog->account_widget = pidgin_account_dropdown_new(NULL, FALSE, account_filter_func, NULL);
	dialog->account = pidgin_account_dropdown_get_selected(dialog->account_widget);
	g_signal_connect(dialog->account_widget, "notify::selected",
	                 G_CALLBACK(dialog_select_account_cb), dialog);
	pidgin_add_widget_to_vbox(GTK_BOX(vbox), _("_Account:"), NULL, dialog->account_widget, TRUE, NULL);

	/* scrolled window */
	dialog->sw = gtk_scrolled_window_new();
	gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(dialog->sw), 250);
	gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(dialog->sw), TRUE);
	gtk_widget_set_vexpand(dialog->sw, TRUE);
	gtk_box_append(GTK_BOX(vbox), dialog->sw);

	/* progress bar */
	dialog->progress = gtk_progress_bar_new();
	gtk_progress_bar_set_pulse_step(GTK_PROGRESS_BAR(dialog->progress), 0.1);
	gtk_box_append(GTK_BOX(vbox), dialog->progress);

	/* buttons */
	dialog->stop_button = pidgin_dialog_add_button(window, _("_Stop"),
	                                               G_CALLBACK(stop_button_cb), dialog);
	gtk_widget_set_sensitive(dialog->stop_button, FALSE);

	dialog->browse_button = pidgin_dialog_add_button(window, _("_Browse"),
	                                                 G_CALLBACK(browse_button_cb), dialog);
	gtk_widget_set_sensitive(dialog->browse_button, dialog->account != NULL);

	dialog->register_button = pidgin_dialog_add_button(window, _("Register"),
	                                                   G_CALLBACK(register_button_cb), dialog);
	gtk_widget_set_sensitive(dialog->register_button, FALSE);

	dialog->add_button = pidgin_dialog_add_button(window, _("_Add"),
	                                              G_CALLBACK(add_to_blist_cb), dialog);
	gtk_widget_set_sensitive(dialog->add_button, FALSE);

	dialog->close_button = pidgin_dialog_add_button(window, _("_Close"),
	                                                G_CALLBACK(close_button_cb), dialog);
	update_buttons(dialog);

	/* show the dialog window and return the dialog */
	gtk_window_present(GTK_WINDOW(dialog->window));

	return dialog;
}

void pidgin_disco_add_service(PidginDiscoList *pdl, XmppDiscoService *service, XmppDiscoService *parent)
{
	PidginDiscoDialog *dialog;
	PidginDiscoNode *node;
	GListStore *store = pdl->model;

	dialog = pdl->dialog;
	g_return_if_fail(dialog != NULL);

	if (service != NULL)
		purple_debug_info("xmppdisco", "Adding service \"%s\"\n", service->name);
	else
		purple_debug_info("xmppdisco", "Service \"%s\" has no childrens\n", parent->name);

	gtk_progress_bar_pulse(GTK_PROGRESS_BAR(dialog->progress));

	if (parent) {
		PidginDiscoNode *parent_node = g_hash_table_lookup(pdl->services, parent);

		if (parent_node == NULL || parent_node->children == NULL)
			return;
		store = parent_node->children;
	}

	/* no children: the (expanded) parent just stays empty */
	if (service == NULL || store == NULL)
		return;

	node = pidgin_disco_node_new(service);
	g_list_store_append(store, node);
	if (service->flags & XMPP_DISCO_BROWSE)
		g_hash_table_insert(pdl->services, service, g_object_ref(node));
	g_object_unref(node);
}
