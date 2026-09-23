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
 * Privacy: port of pidgin/gtkprivacy.c. The account and policy pickers
 * are GtkDropDowns, the allow and block lists GtkListViews; as in
 * Pidgin 2, changes take effect immediately and the lists are shown
 * according to the policy.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "account.h"
#include "blist.h"
#include "connection.h"
#include "debug.h"
#include "privacy.h"
#include "prpl.h"
#include "request.h"
#include "server.h"
#include "util.h"

#include "gtkblist.h"
#include "gtkprivacy.h"
#include "gtkutils.h"
#include "pidginselftest.h"

typedef struct
{
	GtkWidget *win;
	GtkWidget *account_menu;
	GtkWidget *type_menu;
	GtkWidget *add_button;
	GtkWidget *remove_button;
	GtkWidget *removeall_button;
	GtkWidget *allow_widget;
	GtkWidget *block_widget;
	GtkStringList *allow_store;
	GtkStringList *block_store;
	GtkSingleSelection *allow_selection;
	GtkSingleSelection *block_selection;
	GtkWidget *empty_label;
	gboolean in_allow_list;
	PurpleAccount *account;
} PidginPrivacyDialog;

typedef struct
{
	PurpleAccount *account;
	char *name;
	gboolean block;
} PidginPrivacyRequestData;

static const struct
{
	const char *text;
	PurplePrivacyType type;
} menu_entries[] =
{
	{ N_("Allow all users to contact me"),         PURPLE_PRIVACY_ALLOW_ALL },
	{ N_("Allow only the users on my buddy list"), PURPLE_PRIVACY_ALLOW_BUDDYLIST },
	{ N_("Allow only the users below"),            PURPLE_PRIVACY_ALLOW_USERS },
	{ N_("Block all users"),                       PURPLE_PRIVACY_DENY_ALL },
	{ N_("Block only the users below"),            PURPLE_PRIVACY_DENY_USERS }
};

static PidginPrivacyDialog *privacy_dialog = NULL;

static void type_changed_cb(GObject *dropdown, GParamSpec *pspec, PidginPrivacyDialog *dialog);

static gboolean
account_exists(PurpleAccount *account)
{
	return account != NULL && g_list_find(purple_accounts_get_all(), account) != NULL;
}

/* Connected accounts whose protocol has a privacy list. */
static gboolean
check_account_func(PurpleAccount *account)
{
	PurpleConnection *gc = purple_account_get_connection(account);
	PurplePluginProtocolInfo *prpl_info;

	if (gc == NULL || gc->prpl == NULL)
		return FALSE;
	prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(gc->prpl);
	return prpl_info != NULL &&
		(prpl_info->set_permit_deny != NULL || prpl_info->add_permit != NULL ||
		 prpl_info->add_deny != NULL);
}

static void
rebuild_list(GtkStringList *store, GSList *names)
{
	GPtrArray *sorted = g_ptr_array_new();
	GSList *l;
	guint n = g_list_model_get_n_items(G_LIST_MODEL(store));

	for (l = names; l != NULL; l = l->next)
		g_ptr_array_add(sorted, l->data);
	g_ptr_array_sort_values(sorted, (GCompareFunc)g_utf8_collate);
	g_ptr_array_add(sorted, NULL);
	gtk_string_list_splice(store, 0, n, (const char * const *)sorted->pdata);
	g_ptr_array_unref(sorted);
}

static void
rebuild_allow_list(PidginPrivacyDialog *dialog)
{
	rebuild_list(dialog->allow_store,
		account_exists(dialog->account) ? dialog->account->permit : NULL);
}

static void
rebuild_block_list(PidginPrivacyDialog *dialog)
{
	rebuild_list(dialog->block_store,
		account_exists(dialog->account) ? dialog->account->deny : NULL);
}

static const char *
selected_name(PidginPrivacyDialog *dialog)
{
	GtkSingleSelection *sel = dialog->in_allow_list ? dialog->allow_selection
	                                                : dialog->block_selection;
	GtkStringObject *obj = gtk_single_selection_get_selected_item(sel);

	return obj ? gtk_string_object_get_string(obj) : NULL;
}

static void
update_buttons(PidginPrivacyDialog *dialog)
{
	gboolean has_account = account_exists(dialog->account);
	GtkStringList *store = dialog->in_allow_list ? dialog->allow_store : dialog->block_store;

	gtk_widget_set_sensitive(dialog->type_menu, has_account);
	gtk_widget_set_sensitive(dialog->add_button, has_account);
	gtk_widget_set_sensitive(dialog->remove_button,
		has_account && selected_name(dialog) != NULL);
	gtk_widget_set_sensitive(dialog->removeall_button,
		has_account && g_list_model_get_n_items(G_LIST_MODEL(store)) > 0);
}

static void
user_selected_cb(GtkSelectionModel *model, guint position, guint n_items,
                 PidginPrivacyDialog *dialog)
{
	update_buttons(dialog);
}

static void
name_setup_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	GtkWidget *label = gtk_label_new(NULL);

	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_list_item_set_child(li, label);
}

static void
name_bind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(li)),
		gtk_string_object_get_string(gtk_list_item_get_item(li)));
}

static GtkWidget *
build_list(PidginPrivacyDialog *dialog, GtkStringList *store,
           GtkSingleSelection **ret_selection, const char *accessible_name)
{
	GtkListItemFactory *factory;
	GtkWidget *view, *sw;
	GtkSingleSelection *sel;

	sel = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(store)));
	gtk_single_selection_set_autoselect(sel, FALSE);
	gtk_single_selection_set_can_unselect(sel, TRUE);
	g_signal_connect(sel, "selection-changed", G_CALLBACK(user_selected_cb), dialog);
	*ret_selection = sel;

	/* Rows are plain labels showing the name. */
	factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(name_setup_cb), NULL);
	g_signal_connect(factory, "bind", G_CALLBACK(name_bind_cb), NULL);

	view = gtk_list_view_new(GTK_SELECTION_MODEL(g_object_ref(sel)), factory);
	gtk_accessible_update_property(GTK_ACCESSIBLE(view),
		GTK_ACCESSIBLE_PROPERTY_LABEL, accessible_name, -1);
	sw = pidgin_make_scrollable(view, GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC, -1, 200);
	gtk_widget_set_vexpand(sw, TRUE);
	return sw;
}

static void
select_account(PidginPrivacyDialog *dialog, PurpleAccount *account)
{
	gsize i;

	dialog->account = account;

	g_signal_handlers_block_by_func(dialog->type_menu, type_changed_cb, dialog);
	if (account != NULL) {
		for (i = 0; i < G_N_ELEMENTS(menu_entries); i++) {
			if (menu_entries[i].type == account->perm_deny) {
				gtk_drop_down_set_selected(GTK_DROP_DOWN(dialog->type_menu), i);
				break;
			}
		}
	}
	g_signal_handlers_unblock_by_func(dialog->type_menu, type_changed_cb, dialog);

	rebuild_allow_list(dialog);
	rebuild_block_list(dialog);
	type_changed_cb(G_OBJECT(dialog->type_menu), NULL, dialog);
}

static void
select_account_cb(GObject *dropdown, GParamSpec *pspec, PidginPrivacyDialog *dialog)
{
	select_account(dialog, pidgin_account_dropdown_get_selected(GTK_WIDGET(dropdown)));
}

/*
 * TODO: Setting the permit/deny setting needs to go through privacy.c
 *       Even better: the privacy API needs to not suck.
 */
static void
type_changed_cb(GObject *dropdown, GParamSpec *pspec, PidginPrivacyDialog *dialog)
{
	guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
	PurplePrivacyType new_type;
	gboolean show_buttons = FALSE;

	if (selected >= G_N_ELEMENTS(menu_entries))
		return;
	new_type = menu_entries[selected].type;

	/* Only a change by the user is applied (take effect immediately). */
	if (account_exists(dialog->account) && dialog->account->perm_deny != new_type) {
		dialog->account->perm_deny = new_type;
		serv_set_permit_deny(purple_account_get_connection(dialog->account));
		purple_blist_schedule_save();
		pidgin_blist_refresh(purple_get_blist());
	}

	gtk_widget_set_visible(dialog->allow_widget, FALSE);
	gtk_widget_set_visible(dialog->block_widget, FALSE);

	if (new_type == PURPLE_PRIVACY_ALLOW_USERS) {
		gtk_widget_set_visible(dialog->allow_widget, TRUE);
		show_buttons = TRUE;
		dialog->in_allow_list = TRUE;
	}
	else if (new_type == PURPLE_PRIVACY_DENY_USERS) {
		gtk_widget_set_visible(dialog->block_widget, TRUE);
		show_buttons = TRUE;
		dialog->in_allow_list = FALSE;
	}

	gtk_widget_set_visible(dialog->add_button, show_buttons);
	gtk_widget_set_visible(dialog->remove_button, show_buttons);
	gtk_widget_set_visible(dialog->removeall_button, show_buttons);
	gtk_widget_set_visible(dialog->empty_label, !account_exists(dialog->account));
	update_buttons(dialog);
}

static void
add_cb(GtkWidget *button, PidginPrivacyDialog *dialog)
{
	if (!account_exists(dialog->account))
		return;
	if (dialog->in_allow_list)
		pidgin_request_add_permit(dialog->account, NULL);
	else
		pidgin_request_add_block(dialog->account, NULL);
}

static void
remove_cb(GtkWidget *button, PidginPrivacyDialog *dialog)
{
	char *name;

	if (!account_exists(dialog->account) || selected_name(dialog) == NULL)
		return;

	name = g_strdup(selected_name(dialog));
	if (dialog->in_allow_list)
		purple_privacy_permit_remove(dialog->account, name, FALSE);
	else
		purple_privacy_deny_remove(dialog->account, name, FALSE);
	g_free(name);

	purple_blist_schedule_save();
	pidgin_blist_refresh(purple_get_blist());
}

static void
removeall_cb(GtkWidget *button, PidginPrivacyDialog *dialog)
{
	GSList *l;

	if (!account_exists(dialog->account))
		return;

	if (dialog->in_allow_list)
		l = dialog->account->permit;
	else
		l = dialog->account->deny;
	while (l) {
		char *user;
		user = l->data;
		l = l->next;
		if (dialog->in_allow_list)
			purple_privacy_permit_remove(dialog->account, user, FALSE);
		else
			purple_privacy_deny_remove(dialog->account, user, FALSE);
	}
	purple_blist_schedule_save();
	pidgin_blist_refresh(purple_get_blist());
}

static void
close_cb(GtkWidget *button, PidginPrivacyDialog *dialog)
{
	pidgin_privacy_dialog_hide();
}

/* The account list follows sign-ons and sign-offs. */
static void
refresh_accounts(PidginPrivacyDialog *dialog)
{
	GtkWidget *tmp = pidgin_account_dropdown_new(NULL, FALSE,
		(PurpleFilterAccountFunc)check_account_func, NULL);
	GListModel *model = gtk_drop_down_get_model(GTK_DROP_DOWN(tmp));
	PurpleAccount *account = dialog->account;

	g_object_ref_sink(tmp);
	g_signal_handlers_block_by_func(dialog->account_menu, select_account_cb, dialog);
	gtk_drop_down_set_model(GTK_DROP_DOWN(dialog->account_menu), model);
	if (account != NULL)
		pidgin_account_dropdown_set_selected(dialog->account_menu, account);
	g_signal_handlers_unblock_by_func(dialog->account_menu, select_account_cb, dialog);
	g_object_unref(tmp);

	account = pidgin_account_dropdown_get_selected(dialog->account_menu);
	if (account != dialog->account)
		select_account(dialog, account);
	else
		update_buttons(dialog);
}

static void
connection_changed_cb(PurpleConnection *gc, PidginPrivacyDialog *dialog)
{
	refresh_accounts(dialog);
}

static void
privacy_dialog_destroy_cb(GtkWidget *win, gpointer data)
{
	PidginPrivacyDialog *dialog = privacy_dialog;

	if (dialog == NULL)
		return;

	privacy_dialog = NULL;
	purple_signals_disconnect_by_handle(dialog);
	g_clear_object(&dialog->allow_selection);
	g_clear_object(&dialog->block_selection);
	g_clear_object(&dialog->allow_store);
	g_clear_object(&dialog->block_store);
	g_free(dialog);
}

static PidginPrivacyDialog *
privacy_dialog_new(void)
{
	PidginPrivacyDialog *dialog;
	GtkWidget *vbox, *label;
	GtkStringList *types;
	gsize i;

	dialog = g_new0(PidginPrivacyDialog, 1);

	dialog->win = pidgin_dialog_new(_("Privacy"), NULL, "privacy", TRUE);
	gtk_window_set_default_size(GTK_WINDOW(dialog->win), 400, -1);
	g_signal_connect(dialog->win, "destroy", G_CALLBACK(privacy_dialog_destroy_cb), NULL);

	vbox = pidgin_dialog_get_content_area(dialog->win);

	/* Description label */
	label = gtk_label_new(_("Changes to privacy settings take effect immediately."));
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_box_append(GTK_BOX(vbox), label);

	/* Accounts drop-down */
	dialog->account_menu = pidgin_account_dropdown_new(NULL, FALSE,
		(PurpleFilterAccountFunc)check_account_func, NULL);
	pidgin_add_widget_to_vbox(GTK_BOX(vbox), _("Set privacy for:"), NULL,
	                          dialog->account_menu, TRUE, NULL);

	dialog->empty_label = gtk_label_new(_("You are not currently signed on with an "
		"account that can use the privacy settings."));
	gtk_label_set_wrap(GTK_LABEL(dialog->empty_label), TRUE);
	gtk_label_set_xalign(GTK_LABEL(dialog->empty_label), 0.0);
	gtk_widget_add_css_class(dialog->empty_label, "dim-label");
	gtk_box_append(GTK_BOX(vbox), dialog->empty_label);

	/* The drop-down list with the allow/block types. */
	types = gtk_string_list_new(NULL);
	for (i = 0; i < G_N_ELEMENTS(menu_entries); i++)
		gtk_string_list_append(types, _(menu_entries[i].text));
	dialog->type_menu = gtk_drop_down_new(G_LIST_MODEL(types), NULL);
	gtk_accessible_update_property(GTK_ACCESSIBLE(dialog->type_menu),
		GTK_ACCESSIBLE_PROPERTY_LABEL, _("Privacy"), -1);
	gtk_box_append(GTK_BOX(vbox), dialog->type_menu);

	/* The allow and block lists. */
	dialog->allow_store = gtk_string_list_new(NULL);
	dialog->allow_widget = build_list(dialog, dialog->allow_store,
		&dialog->allow_selection, _("Allow List"));
	gtk_box_append(GTK_BOX(vbox), dialog->allow_widget);

	dialog->block_store = gtk_string_list_new(NULL);
	dialog->block_widget = build_list(dialog, dialog->block_store,
		&dialog->block_selection, _("Block List"));
	gtk_box_append(GTK_BOX(vbox), dialog->block_widget);

	/* The buttons: Add, Remove, Remove All, Close */
	dialog->add_button = pidgin_dialog_add_button(dialog->win, _("_Add"),
		G_CALLBACK(add_cb), dialog);
	dialog->remove_button = pidgin_dialog_add_button(dialog->win, _("_Remove"),
		G_CALLBACK(remove_cb), dialog);
	dialog->removeall_button = pidgin_dialog_add_button(dialog->win, _("Remove Al_l"),
		G_CALLBACK(removeall_cb), dialog);
	pidgin_dialog_add_button(dialog->win, _("_Close"), G_CALLBACK(close_cb), dialog);

	g_signal_connect(dialog->account_menu, "notify::selected",
	                 G_CALLBACK(select_account_cb), dialog);
	g_signal_connect(dialog->type_menu, "notify::selected",
	                 G_CALLBACK(type_changed_cb), dialog);

	purple_signal_connect(purple_connections_get_handle(), "signed-on", dialog,
	                      PURPLE_CALLBACK(connection_changed_cb), dialog);
	purple_signal_connect(purple_connections_get_handle(), "signed-off", dialog,
	                      PURPLE_CALLBACK(connection_changed_cb), dialog);

	select_account(dialog, pidgin_account_dropdown_get_selected(dialog->account_menu));

	return dialog;
}

void
pidgin_privacy_dialog_show(void)
{
	if (privacy_dialog == NULL)
		privacy_dialog = privacy_dialog_new();

	pidgin_window_set_secondary(GTK_WINDOW(privacy_dialog->win));
	gtk_window_present(GTK_WINDOW(privacy_dialog->win));
}

void
pidgin_privacy_dialog_hide(void)
{
	if (privacy_dialog == NULL)
		return;

	/* The destroy handler frees privacy_dialog. */
	gtk_window_destroy(GTK_WINDOW(privacy_dialog->win));
}

/**************************************************************************
 * Permit/block requests
 **************************************************************************/

static void
destroy_request_data(PidginPrivacyRequestData *data)
{
	g_free(data->name);
	g_free(data);
}

static void
confirm_permit_block_cb(PidginPrivacyRequestData *data, int option)
{
	if (account_exists(data->account)) {
		if (data->block)
			purple_privacy_deny(data->account, data->name, FALSE, FALSE);
		else
			purple_privacy_allow(data->account, data->name, FALSE, FALSE);
	}
	destroy_request_data(data);
	purple_blist_schedule_save();
	pidgin_blist_refresh(purple_get_blist());
}

static void
add_permit_block_cb(PidginPrivacyRequestData *data, const char *name)
{
	data->name = g_strdup(name);
	confirm_permit_block_cb(data, 0);
}

void
pidgin_request_add_permit(PurpleAccount *account, const char *name)
{
	PidginPrivacyRequestData *data;

	g_return_if_fail(account != NULL);

	data = g_new0(PidginPrivacyRequestData, 1);
	data->account = account;
	data->name    = g_strdup(name);
	data->block   = FALSE;

	if (name == NULL) {
		purple_request_input(account, _("Permit User"),
			_("Type a user you permit to contact you."),
			_("Please enter the name of the user you wish to be "
			  "able to contact you."),
			NULL, FALSE, FALSE, NULL,
			_("_Permit"), G_CALLBACK(add_permit_block_cb),
			_("Cancel"), G_CALLBACK(destroy_request_data),
			account, name, NULL,
			data);
	}
	else {
		char *primary = g_strdup_printf(_("Allow %s to contact you?"), name);
		char *secondary =
			g_strdup_printf(_("Are you sure you wish to allow "
							  "%s to contact you?"), name);

		purple_request_action(account, _("Permit User"), primary, secondary,
							0,
							account, name, NULL,
							data, 2,
							_("_Permit"), G_CALLBACK(confirm_permit_block_cb),
							_("Cancel"), G_CALLBACK(destroy_request_data));

		g_free(primary);
		g_free(secondary);
	}
}

void
pidgin_request_add_block(PurpleAccount *account, const char *name)
{
	PidginPrivacyRequestData *data;

	g_return_if_fail(account != NULL);

	data = g_new0(PidginPrivacyRequestData, 1);
	data->account = account;
	data->name    = g_strdup(name);
	data->block   = TRUE;

	if (name == NULL) {
		purple_request_input(account, _("Block User"),
			_("Type a user to block."),
			_("Please enter the name of the user you wish to block."),
			NULL, FALSE, FALSE, NULL,
			_("_Block"), G_CALLBACK(add_permit_block_cb),
			_("Cancel"), G_CALLBACK(destroy_request_data),
			account, name, NULL,
			data);
	}
	else {
		char *primary = g_strdup_printf(_("Block %s?"), name);
		char *secondary =
			g_strdup_printf(_("Are you sure you want to block %s?"), name);

		purple_request_action(account, _("Block User"), primary, secondary,
							0,
							account, name, NULL,
							data, 2,
							_("_Block"), G_CALLBACK(confirm_permit_block_cb),
							_("Cancel"), G_CALLBACK(destroy_request_data));

		g_free(primary);
		g_free(secondary);
	}
}

/**************************************************************************
 * UI ops
 **************************************************************************/

static void
pidgin_permit_added_removed(PurpleAccount *account, const char *name)
{
	if (privacy_dialog != NULL && privacy_dialog->account == account) {
		rebuild_allow_list(privacy_dialog);
		update_buttons(privacy_dialog);
	}
}

static void
pidgin_deny_added_removed(PurpleAccount *account, const char *name)
{
	if (privacy_dialog != NULL && privacy_dialog->account == account) {
		rebuild_block_list(privacy_dialog);
		update_buttons(privacy_dialog);
	}
}

static PurplePrivacyUiOps privacy_ops =
{
	pidgin_permit_added_removed,
	pidgin_permit_added_removed,
	pidgin_deny_added_removed,
	pidgin_deny_added_removed,
	NULL,
	NULL,
	NULL,
	NULL
};

PurplePrivacyUiOps *
pidgin_privacy_get_ui_ops(void)
{
	return &privacy_ops;
}

void
pidgin_privacy_init(void)
{
}

/**************************************************************************
 * Selftest
 **************************************************************************/

void
pidgin_privacy_selftest(void)
{
	PurpleAccount *account;
	PurplePrivacyType before = PURPLE_PRIVACY_ALLOW_ALL;
	guint n_accounts, i;

	pidgin_privacy_dialog_show();
	pidgin_selftest_iterate(300);
	if (privacy_dialog == NULL) {
		pidgin_selftest_fail("privacy", "the dialog did not open");
		return;
	}

	n_accounts = g_list_model_get_n_items(
		gtk_drop_down_get_model(GTK_DROP_DOWN(privacy_dialog->account_menu)));
	account = privacy_dialog->account;
	pidgin_selftest_log("privacy", "%u account(s) can use privacy settings", n_accounts);

	if (account == NULL) {
		/* The empty state: nothing to apply, the notice is shown. */
		if (!gtk_widget_get_visible(privacy_dialog->empty_label))
			pidgin_selftest_fail("privacy", "no account, but no notice");
		if (gtk_widget_get_sensitive(privacy_dialog->add_button))
			pidgin_selftest_fail("privacy", "no account, but Add is sensitive");

		/* Cycle the policies: only the visibility of the lists changes. */
		for (i = 0; i < G_N_ELEMENTS(menu_entries); i++) {
			gboolean list_shown;

			gtk_drop_down_set_selected(GTK_DROP_DOWN(privacy_dialog->type_menu), i);
			pidgin_selftest_iterate(50);
			list_shown = gtk_widget_get_visible(privacy_dialog->allow_widget) ||
			             gtk_widget_get_visible(privacy_dialog->block_widget);
			if (list_shown != (menu_entries[i].type == PURPLE_PRIVACY_ALLOW_USERS ||
			                   menu_entries[i].type == PURPLE_PRIVACY_DENY_USERS))
				pidgin_selftest_fail("privacy", "policy %u: wrong list visibility", i);
		}
		pidgin_selftest_log("privacy", "cycled %u policies without an account",
		                    (guint)G_N_ELEMENTS(menu_entries));
	} else {
		/* With a signed-on account nothing is changed: just check that
		 * the drop-down shows the account's policy. */
		before = account->perm_deny;
		i = gtk_drop_down_get_selected(GTK_DROP_DOWN(privacy_dialog->type_menu));
		if (i >= G_N_ELEMENTS(menu_entries) || menu_entries[i].type != before)
			pidgin_selftest_fail("privacy", "the policy drop-down does not match");
	}

	pidgin_privacy_dialog_hide();
	pidgin_selftest_iterate(100);
	if (privacy_dialog != NULL)
		pidgin_selftest_fail("privacy", "the dialog is still open");
	if (account != NULL && account_exists(account) && account->perm_deny != before)
		pidgin_selftest_fail("privacy", "the policy was changed");
}
