/**
 * @file gtkaccount.c GTK+ Account Editor UI
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
 * GTK 4 port of pidgin/gtkaccount.c: the accounts window (a GtkColumnView
 * over a GListStore), the add/modify account editor, and the account UI
 * ops. The editor's save logic follows Pidgin 2's ok_account_prefs_cb()
 * step by step so accounts.xml keeps the same semantics.
 *
 * Not ported yet:
 *   - buddy icon selection in the editor, and the default icon of new
 *     accounts (/pidgin/accounts/buddyicon): TODO(M3);
 *   - drag-and-drop reordering of accounts (purple_accounts_reorder):
 *     TODO(M5);
 *   - the voice/video tab: dropped (no voice/video in this build).
 * Pidgin 2 showed account requests (authorize, "added you", "add buddy?")
 * as mini-dialogs in the buddy list; here they are small windows.
 * TODO(M3): move them into the buddy list.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "account.h"
#include "accountopt.h"
#include "blist.h"
#include "connection.h"
#include "core.h"
#include "debug.h"
#include "notify.h"
#include "plugin.h"
#include "prefs.h"
#include "proxy.h"
#include "prpl.h"
#include "request.h"
#include "savedstatuses.h"
#include "server.h"
#include "signals.h"
#include "status.h"
#include "util.h"

#include "gtkaccount.h"
#include "gtkutils.h"
#include "pidginmenu.h"

#define PREFS_DIALOG PIDGIN4_PREFS_ROOT "/accounts/dialog"

/**************************************************************************
 * PidginAccountRow: one row of the accounts window
 **************************************************************************/

#define PIDGIN_TYPE_ACCOUNT_ROW (pidgin_account_row_get_type())
G_DECLARE_FINAL_TYPE(PidginAccountRow, pidgin_account_row, PIDGIN, ACCOUNT_ROW, GObject)

struct _PidginAccountRow {
	GObject parent;

	PurpleAccount *account;
};

enum {
	ROW_SIGNAL_CHANGED,
	ROW_N_SIGNALS
};

static guint row_signals[ROW_N_SIGNALS];

G_DEFINE_FINAL_TYPE(PidginAccountRow, pidgin_account_row, G_TYPE_OBJECT)

static void
pidgin_account_row_class_init(PidginAccountRowClass *klass)
{
	/* Emitted when anything shown for the account may have changed. */
	row_signals[ROW_SIGNAL_CHANGED] = g_signal_new("changed",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 0);
}

static void
pidgin_account_row_init(PidginAccountRow *row)
{
}

static PidginAccountRow *
pidgin_account_row_new(PurpleAccount *account)
{
	PidginAccountRow *row = g_object_new(PIDGIN_TYPE_ACCOUNT_ROW, NULL);

	row->account = account;
	return row;
}

/**************************************************************************
 * Types
 **************************************************************************/

typedef struct
{
	GtkWidget *window;
	GtkWidget *columnview;
	GtkSingleSelection *selection;
	GListStore *store;

	GtkWidget *modify_button;
	GtkWidget *delete_button;
	GtkWidget *actions_button;

} AccountsWindow;

typedef struct
{
	GtkWidget *widget;
	gchar *setting;
	PurplePrefType type;
	GPtrArray *list_values;   /* PURPLE_PREF_STRING_LIST: the values */
} ProtocolOptEntry;

typedef struct
{
	PidginAccountDialogType type;

	PurpleAccount *account;
	char *protocol_id;
	PurplePlugin *plugin;
	PurplePluginProtocolInfo *prpl_info;

	PurpleProxyType new_proxy_type;

	GList *user_split_entries;
	GList *protocol_opt_entries;

	GtkSizeGroup *sg;
	GtkWidget *window;

	GtkWidget *notebook;
	GtkWidget *top_vbox;
	GtkWidget *ok_button;
	GtkWidget *register_button;

	/* Login Options */
	GtkWidget *login_slot;
	GtkWidget *protocol_menu;
	GtkWidget *password_box;
	GtkWidget *username_entry;
	GtkWidget *password_entry;
	GtkWidget *alias_entry;
	GtkWidget *remember_pass_check;

	/* User Options */
	GtkWidget *user_slot;
	GtkWidget *new_mail_check;

	/* Protocol Options */
	GtkWidget *protocol_frame;

	/* Proxy Options */
	GtkWidget *proxy_frame;
	GtkWidget *proxy_vbox;
	GtkWidget *proxy_dropdown;
	GtkWidget *proxy_host_entry;
	GtkWidget *proxy_port_entry;
	GtkWidget *proxy_user_entry;
	GtkWidget *proxy_pass_entry;

} AccountPrefsDialog;

static AccountsWindow *accounts_window = NULL;
static GHashTable *account_pref_wins = NULL;
/* Open account request windows (authorize, added, add buddy). */
static GList *request_windows = NULL;
static gboolean uninitializing = FALSE;

static void accounts_window_refresh(PurpleAccount *account);
static void update_actions_menu(void);

/**************************************************************************
 * Small helpers
 **************************************************************************/

static const char *
entry_text(GtkWidget *entry)
{
	return gtk_editable_get_text(GTK_EDITABLE(entry));
}

static void
set_entry_text(GtkWidget *entry, const char *text)
{
	gtk_editable_set_text(GTK_EDITABLE(entry), text ? text : "");
}

static void
remove_all_children(GtkWidget *box)
{
	GtkWidget *child;

	while ((child = gtk_widget_get_first_child(box)) != NULL)
		gtk_box_remove(GTK_BOX(box), child);
}

static gboolean
account_exists(PurpleAccount *account)
{
	return account != NULL && g_list_find(purple_accounts_get_all(), account) != NULL;
}

/**************************************************************************
 * Add/Modify Account dialog
 **************************************************************************/
static void add_login_options(AccountPrefsDialog *dialog, GtkWidget *parent);
static void add_user_options(AccountPrefsDialog *dialog, GtkWidget *parent);
static void add_protocol_options(AccountPrefsDialog *dialog);
static void add_proxy_options(AccountPrefsDialog *dialog, GtkWidget *parent);

static GtkWidget *
add_pref_box(AccountPrefsDialog *dialog, GtkWidget *parent,
			 const char *text, GtkWidget *widget)
{
	return pidgin_add_widget_to_vbox(GTK_BOX(parent), text, dialog->sg, widget, TRUE, NULL);
}

/* A split without a default value must be filled in; everything else
 * falls back to its default when empty, as in Pidgin 2. */
static gboolean
dialog_input_valid(AccountPrefsDialog *dialog)
{
	GList *l, *l2;

	if (dialog->username_entry == NULL || *entry_text(dialog->username_entry) == '\0')
		return FALSE;

	if (dialog->prpl_info == NULL)
		return TRUE;

	for (l = dialog->prpl_info->user_splits, l2 = dialog->user_split_entries;
	     l != NULL && l2 != NULL; l = l->next, l2 = l2->next) {
		PurpleAccountUserSplit *split = l->data;
		const char *def = purple_account_user_split_get_default_value(split);

		if (*entry_text(l2->data) == '\0' && (def == NULL || *def == '\0'))
			return FALSE;
	}

	return TRUE;
}

static void
update_buttons(AccountPrefsDialog *dialog)
{
	gboolean have_username;

	if (dialog->username_entry == NULL)
		return;

	have_username = (*entry_text(dialog->username_entry) != '\0');

	if (dialog->ok_button)
		gtk_widget_set_sensitive(dialog->ok_button, dialog_input_valid(dialog));
	if (dialog->register_button) {
		if (dialog->prpl_info != NULL && (dialog->prpl_info->options & OPT_PROTO_REGISTER_NOSCREENNAME))
			gtk_widget_set_sensitive(dialog->register_button, TRUE);
		else
			gtk_widget_set_sensitive(dialog->register_button, have_username);
	}
}

static void
username_changed_cb(GtkEditable *entry, AccountPrefsDialog *dialog)
{
	update_buttons(dialog);
}

static void
update_register_button(AccountPrefsDialog *dialog)
{
	if (dialog->register_button == NULL)
		return;

	if (!dialog->prpl_info || !dialog->prpl_info->register_user) {
		gtk_widget_set_visible(dialog->register_button, FALSE);
		gtk_check_button_set_active(GTK_CHECK_BUTTON(dialog->register_button), FALSE);
	} else {
		gtk_widget_set_visible(dialog->register_button, TRUE);
	}
	update_buttons(dialog);
}

static void
set_account_protocol_cb(GObject *dropdown, GParamSpec *pspec,
						AccountPrefsDialog *dialog)
{
	PurplePlugin *new_plugin;
	const char *id;
	char *typed_username = NULL;

	id = pidgin_protocol_dropdown_get_selected_id(dialog->protocol_menu);
	if (id == NULL || purple_strequal(id, dialog->protocol_id))
		return;

	new_plugin = purple_find_prpl(id);

	dialog->plugin = new_plugin;

	if (dialog->plugin != NULL)
	{
		dialog->prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(dialog->plugin);

		g_free(dialog->protocol_id);
		dialog->protocol_id = g_strdup(dialog->plugin->info->id);
	}

	/*
	 * Pidgin 2 called purple_account_clear_settings() on the account right
	 * here, even if the dialog was then cancelled. We clear the settings
	 * when saving, if the protocol really changed (see ok_account_prefs_cb).
	 */

	/* Keep what was typed into a new account's username. */
	if (dialog->account == NULL && dialog->username_entry != NULL)
		typed_username = g_strdup(entry_text(dialog->username_entry));

	add_login_options(dialog,    dialog->login_slot);
	add_user_options(dialog,     dialog->user_slot);
	add_protocol_options(dialog);

	if (typed_username != NULL && *typed_username != '\0')
		set_entry_text(dialog->username_entry, typed_username);
	g_free(typed_username);

	gtk_widget_grab_focus(dialog->protocol_menu);

	update_register_button(dialog);
}

static void
update_editable(PurpleConnection *gc, AccountPrefsDialog *dialog)
{
	gboolean set;
	GList *l;

	if (dialog->account == NULL)
		return;

	if (gc != NULL && dialog->account != purple_connection_get_account(gc))
		return;

	set = !(purple_account_is_connected(dialog->account) || purple_account_is_connecting(dialog->account));
	gtk_widget_set_sensitive(dialog->protocol_menu, set);
	gtk_editable_set_editable(GTK_EDITABLE(dialog->username_entry), set);

	for (l = dialog->user_split_entries ; l != NULL ; l = l->next)
		gtk_editable_set_editable(GTK_EDITABLE(l->data), set);
}

static void
add_login_options(AccountPrefsDialog *dialog, GtkWidget *parent)
{
	GtkWidget *hbox;
	GtkWidget *vbox;
	GtkWidget *entry;
	GList *user_splits;
	GList *l, *l2;
	char *username = NULL;

	/* The protocol drop-down survives rebuilds of this frame. */
	if (dialog->protocol_menu != NULL)
	{
		g_object_ref(dialog->protocol_menu);
		hbox = gtk_widget_get_parent(dialog->protocol_menu);
		if (hbox != NULL)
			gtk_box_remove(GTK_BOX(hbox), dialog->protocol_menu);
	}

	remove_all_children(parent);

	/* Build the login options frame. */
	vbox = pidgin_make_frame(parent, _("Login Options"));

	/* Protocol */
	if (dialog->protocol_menu == NULL)
	{
		dialog->protocol_menu = pidgin_protocol_dropdown_new(dialog->protocol_id);
		g_object_ref(dialog->protocol_menu);
		g_signal_connect(dialog->protocol_menu, "notify::selected",
		                 G_CALLBACK(set_account_protocol_cb), dialog);
	}

	add_pref_box(dialog, vbox, _("Pro_tocol:"), dialog->protocol_menu);
	g_object_unref(dialog->protocol_menu);

	/* Username */
	dialog->username_entry = gtk_entry_new();

	add_pref_box(dialog, vbox, _("_Username:"), dialog->username_entry);

	if (dialog->account != NULL)
		username = g_strdup(purple_account_get_username(dialog->account));

	if (!username && dialog->prpl_info
			&& PURPLE_PROTOCOL_PLUGIN_HAS_FUNC(dialog->prpl_info, get_account_text_table)) {
		GHashTable *table;
		const char *label;
		table = dialog->prpl_info->get_account_text_table(NULL);
		label = g_hash_table_lookup(table, "login_label");

		/* Pidgin 2 faked a placeholder with focus handlers and a grey
		 * text colour; GTK 4 entries have one. */
		if (label != NULL)
			gtk_entry_set_placeholder_text(GTK_ENTRY(dialog->username_entry), label);
		g_hash_table_destroy(table);
	}

	/* Do the user split thang */
	if (dialog->prpl_info == NULL)
		user_splits = NULL;
	else
		user_splits = dialog->prpl_info->user_splits;

	if (dialog->user_split_entries != NULL) {
		g_list_free(dialog->user_split_entries);
		dialog->user_split_entries = NULL;
	}

	for (l = user_splits; l != NULL; l = l->next) {
		PurpleAccountUserSplit *split = l->data;
		char *buf;

		buf = g_strdup_printf("_%s:", purple_account_user_split_get_text(split));

		entry = gtk_entry_new();

		add_pref_box(dialog, vbox, buf, entry);

		g_free(buf);

		dialog->user_split_entries =
			g_list_append(dialog->user_split_entries, entry);
	}

	for (l = g_list_last(dialog->user_split_entries),
		 l2 = g_list_last(user_splits);
		 l != NULL && l2 != NULL;
		 l = l->prev, l2 = l2->prev) {

		GtkWidget *entry = l->data;
		PurpleAccountUserSplit *split = l2->data;
		const char *value = NULL;
		char *c;

		if (username != NULL && dialog->account != NULL) {
			if(purple_account_user_split_get_reverse(split))
				c = strrchr(username,
						purple_account_user_split_get_separator(split));
			else
				c = strchr(username,
						purple_account_user_split_get_separator(split));

			if (c != NULL) {
				*c = '\0';
				c++;

				value = c;
			}
		}
		if (value == NULL)
			value = purple_account_user_split_get_default_value(split);

		/* Pidgin 2's "Google Talk" fake protocol entry (default domain
		 * gmail.com) is not offered: the service is gone. */

		if (value != NULL)
			set_entry_text(entry, value);

		g_signal_connect(entry, "changed", G_CALLBACK(username_changed_cb), dialog);
	}

	if (username != NULL)
		set_entry_text(dialog->username_entry, username);

	g_free(username);

	g_signal_connect(dialog->username_entry, "changed",
					 G_CALLBACK(username_changed_cb), dialog);

	/* Password */
	dialog->password_entry = gtk_password_entry_new();
	gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(dialog->password_entry), TRUE);
	dialog->password_box = add_pref_box(dialog, vbox, _("_Password:"),
										  dialog->password_entry);

	if (dialog->prpl_info != NULL &&
	    (dialog->prpl_info->options & OPT_PROTO_PASSWORD_OPTIONAL))
		g_object_set(dialog->password_entry, "placeholder-text", _("(optional)"), NULL);

	/* Remember Password */
	dialog->remember_pass_check =
		gtk_check_button_new_with_mnemonic(_("Remember pass_word"));
	gtk_check_button_set_active(GTK_CHECK_BUTTON(dialog->remember_pass_check),
								 FALSE);
	gtk_box_append(GTK_BOX(vbox), dialog->remember_pass_check);

	/* Set the fields. */
	if (dialog->account != NULL) {
		if (purple_account_get_password(dialog->account) &&
		    purple_account_get_remember_password(dialog->account))
			set_entry_text(dialog->password_entry,
							   purple_account_get_password(dialog->account));

		gtk_check_button_set_active(
				GTK_CHECK_BUTTON(dialog->remember_pass_check),
				purple_account_get_remember_password(dialog->account));
	}

	if (dialog->prpl_info != NULL &&
		(dialog->prpl_info->options & OPT_PROTO_NO_PASSWORD)) {

		gtk_widget_set_visible(dialog->password_box, FALSE);
		gtk_widget_set_visible(dialog->remember_pass_check, FALSE);
	}

	/* Do not let the user change the protocol/username while connected. */
	update_editable(NULL, dialog);
	purple_signal_disconnect(purple_connections_get_handle(), "signing-on", dialog,
					PURPLE_CALLBACK(update_editable));
	purple_signal_disconnect(purple_connections_get_handle(), "signed-off", dialog,
					PURPLE_CALLBACK(update_editable));
	purple_signal_connect(purple_connections_get_handle(), "signing-on", dialog,
					PURPLE_CALLBACK(update_editable), dialog);
	purple_signal_connect(purple_connections_get_handle(), "signed-off", dialog,
					PURPLE_CALLBACK(update_editable), dialog);
}

static void
add_user_options(AccountPrefsDialog *dialog, GtkWidget *parent)
{
	GtkWidget *vbox;

	remove_all_children(parent);

	/* Build the user options frame. */
	vbox = pidgin_make_frame(parent, _("User Options"));

	/* Alias */
	dialog->alias_entry = gtk_entry_new();
	add_pref_box(dialog, vbox, _("_Local alias:"), dialog->alias_entry);

	/* New mail notifications */
	dialog->new_mail_check =
		gtk_check_button_new_with_mnemonic(_("New _mail notifications"));
	gtk_box_append(GTK_BOX(vbox), dialog->new_mail_check);

	/* TODO(M3): "Use this buddy icon for this account" with the icon
	 * chooser. Until then the account's icon settings are left alone. */

	if (dialog->prpl_info != NULL) {
		if (!(dialog->prpl_info->options & OPT_PROTO_MAIL_CHECK))
			gtk_widget_set_visible(dialog->new_mail_check, FALSE);
	}

	if (dialog->account != NULL) {
		if (purple_account_get_alias(dialog->account))
			set_entry_text(dialog->alias_entry,
							   purple_account_get_alias(dialog->account));

		gtk_check_button_set_active(GTK_CHECK_BUTTON(dialog->new_mail_check),
					     purple_account_get_check_mail(dialog->account));
	}
}

static void
protocol_opt_entry_free(ProtocolOptEntry *opt_entry)
{
	g_free(opt_entry->setting);
	if (opt_entry->list_values != NULL)
		g_ptr_array_free(opt_entry->list_values, TRUE);
	g_free(opt_entry);
}

static void
add_protocol_options(AccountPrefsDialog *dialog)
{
	PurpleAccountOption *option;
	PurpleAccount *account;
	GtkWidget *vbox, *check, *entry, *combo, *scroll;
	GList *list, *node;
	gint i, idx, int_value;
	PurpleKeyValuePair *kvp;
	GList *l;
	char buf[1024];
	char *title, *tmp;
	const char *str_value;
	gboolean bool_value;
	ProtocolOptEntry *opt_entry;
	gboolean same_protocol;

	if (dialog->protocol_frame != NULL) {
		gtk_notebook_remove_page(GTK_NOTEBOOK(dialog->notebook),
			gtk_notebook_page_num(GTK_NOTEBOOK(dialog->notebook), dialog->protocol_frame));
		dialog->protocol_frame = NULL;
	}

	while (dialog->protocol_opt_entries != NULL) {
		protocol_opt_entry_free(dialog->protocol_opt_entries->data);
		dialog->protocol_opt_entries = g_list_delete_link(dialog->protocol_opt_entries, dialog->protocol_opt_entries);
	}

	if (dialog->prpl_info == NULL ||
			dialog->prpl_info->protocol_options == NULL)
		return;

	account = dialog->account;
	same_protocol = (account != NULL &&
		purple_strequal(purple_account_get_protocol_id(account), dialog->protocol_id));

	/* Main vbox */
	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BOX_SPACE);
	gtk_widget_set_margin_top(vbox, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_bottom(vbox, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_start(vbox, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_end(vbox, PIDGIN_HIG_BORDER);

	/* Some prpls have many options; scroll rather than grow off-screen. */
	scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
		GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
	gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 480);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), vbox);

	dialog->protocol_frame = scroll;
	gtk_notebook_insert_page(GTK_NOTEBOOK(dialog->notebook), scroll,
			gtk_label_new_with_mnemonic(_("Ad_vanced")), 1);

	for (l = dialog->prpl_info->protocol_options; l != NULL; l = l->next)
	{
		option = (PurpleAccountOption *)l->data;

		opt_entry = g_new0(ProtocolOptEntry, 1);
		opt_entry->type = purple_account_option_get_type(option);
		opt_entry->setting = g_strdup(purple_account_option_get_setting(option));

		switch (opt_entry->type)
		{
			case PURPLE_PREF_BOOLEAN:
				if (!same_protocol)
				{
					bool_value = purple_account_option_get_default_bool(option);
				}
				else
				{
					bool_value = purple_account_get_bool(account,
						purple_account_option_get_setting(option),
						purple_account_option_get_default_bool(option));
				}

				tmp = g_strconcat("_", purple_account_option_get_text(option), NULL);
				opt_entry->widget = check = gtk_check_button_new_with_mnemonic(tmp);
				g_free(tmp);

				gtk_check_button_set_active(GTK_CHECK_BUTTON(check),
											 bool_value);

				gtk_box_append(GTK_BOX(vbox), check);
				break;

			case PURPLE_PREF_INT:
				if (!same_protocol)
				{
					int_value = purple_account_option_get_default_int(option);
				}
				else
				{
					int_value = purple_account_get_int(account,
						purple_account_option_get_setting(option),
						purple_account_option_get_default_int(option));
				}

				g_snprintf(buf, sizeof(buf), "%d", int_value);

				/* An entry, not a spin button: saved with atoi() like
				 * Pidgin 2, so any int the prpl accepts round-trips. */
				opt_entry->widget = entry = gtk_entry_new();
				gtk_entry_set_input_purpose(GTK_ENTRY(entry), GTK_INPUT_PURPOSE_NUMBER);
				set_entry_text(entry, buf);

				title = g_strdup_printf("_%s:",
						purple_account_option_get_text(option));
				add_pref_box(dialog, vbox, title, entry);
				g_free(title);
				break;

			case PURPLE_PREF_STRING:
				if (!same_protocol)
				{
					str_value = purple_account_option_get_default_string(option);
				}
				else
				{
					str_value = purple_account_get_string(account,
						purple_account_option_get_setting(option),
						purple_account_option_get_default_string(option));
				}

				if (purple_account_option_get_masked(option)) {
					opt_entry->widget = entry = gtk_password_entry_new();
					gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(entry), TRUE);
				} else {
					opt_entry->widget = entry = gtk_entry_new();
				}

				if (str_value != NULL)
					set_entry_text(entry, str_value);

				title = g_strdup_printf("_%s:",
						purple_account_option_get_text(option));
				add_pref_box(dialog, vbox, title, entry);
				g_free(title);
				break;

			case PURPLE_PREF_STRING_LIST: {
				GtkStringList *labels;

				i = 0;
				idx = 0;

				if (!same_protocol)
				{
					str_value = purple_account_option_get_default_list_value(option);
				}
				else
				{
					str_value = purple_account_get_string(account,
						purple_account_option_get_setting(option),
						purple_account_option_get_default_list_value(option));
				}

				list = purple_account_option_get_list(option);
				labels = gtk_string_list_new(NULL);
				opt_entry->list_values = g_ptr_array_new_with_free_func(g_free);

				/* Loop through list of PurpleKeyValuePair items */
				for (node = list; node != NULL; node = node->next) {
					if (node->data != NULL) {
						kvp = (PurpleKeyValuePair *) node->data;
						if ((kvp->value != NULL) && (str_value != NULL) &&
						    !g_utf8_collate(kvp->value, str_value))
							idx = i;

						gtk_string_list_append(labels, kvp->key ? kvp->key : "");
						g_ptr_array_add(opt_entry->list_values, g_strdup(kvp->value));
						i++;
					}
				}

				opt_entry->widget = combo =
					gtk_drop_down_new(G_LIST_MODEL(labels), NULL);

				/* Set default */
				if (i > 0)
					gtk_drop_down_set_selected(GTK_DROP_DOWN(combo), idx);

				title = g_strdup_printf("_%s:",
						purple_account_option_get_text(option));
				add_pref_box(dialog, vbox, title, combo);
				g_free(title);
				break;
			}

			default:
				purple_debug_error("gtkaccount", "Invalid Account Option pref type (%d)\n",
						   opt_entry->type);
				protocol_opt_entry_free(opt_entry);
				continue;
		}

		dialog->protocol_opt_entries =
			g_list_append(dialog->protocol_opt_entries, opt_entry);

	}
}

/* In Pidgin 2's order (make_proxy_dropdown). */
static const struct {
	const char *label;
	PurpleProxyType type;
} proxy_types[] = {
	{ N_("Use Global Proxy Settings"), PURPLE_PROXY_USE_GLOBAL },
	{ N_("No Proxy"), PURPLE_PROXY_NONE },
	{ N_("SOCKS 4"), PURPLE_PROXY_SOCKS4 },
	{ N_("SOCKS 5"), PURPLE_PROXY_SOCKS5 },
	{ N_("Tor/Privacy (SOCKS5)"), PURPLE_PROXY_TOR },
	{ N_("HTTP"), PURPLE_PROXY_HTTP },
	{ N_("Use Environmental Settings"), PURPLE_PROXY_USE_ENVVAR },
};

static GtkWidget *
make_proxy_dropdown(void)
{
	GtkStringList *labels = gtk_string_list_new(NULL);
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(proxy_types); i++)
		gtk_string_list_append(labels, _(proxy_types[i].label));

	return gtk_drop_down_new(G_LIST_MODEL(labels), NULL);
}

static void
proxy_type_changed_cb(GObject *menu, GParamSpec *pspec, AccountPrefsDialog *dialog)
{
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(menu));

	if (sel < G_N_ELEMENTS(proxy_types))
		dialog->new_proxy_type = proxy_types[sel].type;

	gtk_widget_set_visible(dialog->proxy_vbox,
		!(dialog->new_proxy_type == PURPLE_PROXY_USE_GLOBAL ||
		  dialog->new_proxy_type == PURPLE_PROXY_NONE ||
		  dialog->new_proxy_type == PURPLE_PROXY_USE_ENVVAR));
}

static void
add_proxy_options(AccountPrefsDialog *dialog, GtkWidget *parent)
{
	PurpleProxyInfo *proxy_info;
	GtkWidget *vbox;
	GtkWidget *vbox2;
	gsize i;

	/* Main vbox */
	dialog->proxy_frame = vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BOX_SPACE);
	gtk_box_append(GTK_BOX(parent), vbox);

	/* Proxy Type drop-down. */
	dialog->proxy_dropdown = make_proxy_dropdown();

	add_pref_box(dialog, vbox, _("Proxy _type:"), dialog->proxy_dropdown);

	/* Setup the second vbox, which may be hidden at times. */
	dialog->proxy_vbox = vbox2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BOX_SPACE);
	gtk_widget_set_margin_top(vbox2, PIDGIN_HIG_BORDER);
	gtk_box_append(GTK_BOX(vbox), vbox2);

	/* Host */
	dialog->proxy_host_entry = gtk_entry_new();
	add_pref_box(dialog, vbox2, _("_Host:"), dialog->proxy_host_entry);

	/* Port */
	dialog->proxy_port_entry = gtk_entry_new();
	gtk_entry_set_input_purpose(GTK_ENTRY(dialog->proxy_port_entry), GTK_INPUT_PURPOSE_DIGITS);
	add_pref_box(dialog, vbox2, _("_Port:"), dialog->proxy_port_entry);

	/* User */
	dialog->proxy_user_entry = gtk_entry_new();

	add_pref_box(dialog, vbox2, _("_Username:"), dialog->proxy_user_entry);

	/* Password */
	dialog->proxy_pass_entry = gtk_password_entry_new();
	gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(dialog->proxy_pass_entry), TRUE);
	add_pref_box(dialog, vbox2, _("Pa_ssword:"), dialog->proxy_pass_entry);

	if (dialog->account != NULL &&
		(proxy_info = purple_account_get_proxy_info(dialog->account)) != NULL) {
		const char *value;
		int int_val;

		dialog->new_proxy_type = purple_proxy_info_get_type(proxy_info);

		if ((value = purple_proxy_info_get_host(proxy_info)) != NULL)
			set_entry_text(dialog->proxy_host_entry, value);

		if ((int_val = purple_proxy_info_get_port(proxy_info)) != 0) {
			char buf[11];

			g_snprintf(buf, sizeof(buf), "%d", int_val);

			set_entry_text(dialog->proxy_port_entry, buf);
		}

		if ((value = purple_proxy_info_get_username(proxy_info)) != NULL)
			set_entry_text(dialog->proxy_user_entry, value);

		if ((value = purple_proxy_info_get_password(proxy_info)) != NULL)
			set_entry_text(dialog->proxy_pass_entry, value);

	} else
		dialog->new_proxy_type = PURPLE_PROXY_USE_GLOBAL;

	for (i = 0; i < G_N_ELEMENTS(proxy_types); i++) {
		if (proxy_types[i].type == dialog->new_proxy_type) {
			gtk_drop_down_set_selected(GTK_DROP_DOWN(dialog->proxy_dropdown), i);
			break;
		}
	}

	proxy_type_changed_cb(G_OBJECT(dialog->proxy_dropdown), NULL, dialog);

	/* Connect signals. */
	g_signal_connect(dialog->proxy_dropdown, "notify::selected",
					 G_CALLBACK(proxy_type_changed_cb), dialog);
}

/* The window's "destroy": frees the dialog. Every way of closing the
 * editor ends here. */
static void
account_win_destroy_cb(GtkWidget *w, AccountPrefsDialog *dialog)
{
	if (dialog->account != NULL &&
	    account_pref_wins != NULL &&
	    g_hash_table_lookup(account_pref_wins, dialog->account) == dialog)
		g_hash_table_remove(account_pref_wins, dialog->account);

	g_list_free(dialog->user_split_entries);
	while (dialog->protocol_opt_entries != NULL) {
		protocol_opt_entry_free(dialog->protocol_opt_entries->data);
		dialog->protocol_opt_entries = g_list_delete_link(dialog->protocol_opt_entries, dialog->protocol_opt_entries);
	}
	g_free(dialog->protocol_id);
	g_object_unref(dialog->sg);

	purple_signals_disconnect_by_handle(dialog);

	g_free(dialog);
}

static void
cancel_account_prefs_cb(GtkWidget *w, AccountPrefsDialog *dialog)
{
	gtk_window_destroy(GTK_WINDOW(dialog->window));
}

static void
ok_account_prefs_cb(GtkWidget *w, AccountPrefsDialog *dialog)
{
	PurpleProxyInfo *proxy_info = NULL;
	GList *l, *l2;
	const char *value;
	char *username;
	char *tmp;
	gboolean new_acct = FALSE;
	PurpleAccount *account;

	if (!dialog_input_valid(dialog))
		return;

	/* Build the username string. */
	username = g_strdup(entry_text(dialog->username_entry));

	if (dialog->prpl_info != NULL)
	{
		for (l = dialog->prpl_info->user_splits,
			 l2 = dialog->user_split_entries;
			 l != NULL && l2 != NULL;
			 l = l->next, l2 = l2->next)
		{
			PurpleAccountUserSplit *split = l->data;
			GtkWidget *entry = l2->data;
			char sep[2] = " ";

			value = entry_text(entry);

			*sep = purple_account_user_split_get_separator(split);

			tmp = g_strconcat(username, sep,
					(*value ? value :
					 purple_account_user_split_get_default_value(split)),
					NULL);

			g_free(username);
			username = tmp;
		}
	}

	if (dialog->account == NULL)
	{
		if (purple_accounts_find(username, dialog->protocol_id) != NULL) {
			purple_debug_warning("gtkaccount", "Trying to add a duplicate %s account (%s).\n",
				dialog->protocol_id, username);

			purple_notify_error(NULL, NULL, _("Unable to save new account"),
				_("An account already exists with the specified criteria."));

			g_free(username);
			return;
		}

		if (purple_accounts_get_all() == NULL) {
			/* We're adding our first account.  Be polite and show the buddy list */
			purple_blist_set_visible(TRUE);
		}

		account = purple_account_new(username, dialog->protocol_id);
		new_acct = TRUE;
	}
	else
	{
		account = dialog->account;

		/* Pidgin 2 cleared the settings as soon as another protocol was
		 * picked in the drop-down; we do it only when saving. */
		if (!purple_strequal(purple_account_get_protocol_id(account),
		                     dialog->protocol_id))
			purple_account_clear_settings(account);

		/* Protocol */
		purple_account_set_protocol_id(account, dialog->protocol_id);
	}

	/* Alias */
	value = entry_text(dialog->alias_entry);

	if (*value != '\0')
		purple_account_set_alias(account, value);
	else
		purple_account_set_alias(account, NULL);

	/* Buddy Icon: TODO(M3). Pidgin 2 set "use-global-buddyicon" and the
	 * account icon here; pidgin4 leaves them unchanged for now (a new
	 * account then uses the global icon, the default of that setting). */

	/* Remember Password */
	purple_account_set_remember_password(account,
			gtk_check_button_get_active(
					GTK_CHECK_BUTTON(dialog->remember_pass_check)));

	/* Check Mail */
	if (dialog->prpl_info && dialog->prpl_info->options & OPT_PROTO_MAIL_CHECK)
		purple_account_set_check_mail(account,
			gtk_check_button_get_active(
					GTK_CHECK_BUTTON(dialog->new_mail_check)));

	/* Password */
	value = entry_text(dialog->password_entry);

	/*
	 * We set the password if this is a new account because new accounts
	 * will be set to online, and if the user has entered a password into
	 * the account editor (but has not checked the 'save' box), then we
	 * don't want to prompt them.
	 */
	if ((purple_account_get_remember_password(account) || new_acct) && (*value != '\0'))
		purple_account_set_password(account, value);
	else
		purple_account_set_password(account, NULL);

	purple_account_set_username(account, username);
	g_free(username);

	/* Add the protocol settings */
	if (dialog->prpl_info) {
		ProtocolOptEntry *opt_entry;
		const char *value2;
		guint sel;
		int int_value;
		gboolean bool_value;

		for (l2 = dialog->protocol_opt_entries; l2; l2 = l2->next) {

			opt_entry = l2->data;

			switch (opt_entry->type) {
				case PURPLE_PREF_STRING:
					value = entry_text(opt_entry->widget);
					purple_account_set_string(account, opt_entry->setting, value);
					break;

				case PURPLE_PREF_INT:
					int_value = atoi(entry_text(opt_entry->widget));
					purple_account_set_int(account, opt_entry->setting, int_value);
					break;

				case PURPLE_PREF_BOOLEAN:
					bool_value =
						gtk_check_button_get_active(GTK_CHECK_BUTTON(opt_entry->widget));
					purple_account_set_bool(account, opt_entry->setting, bool_value);
					break;

				case PURPLE_PREF_STRING_LIST:
					value2 = NULL;
					sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(opt_entry->widget));
					if (opt_entry->list_values != NULL && sel < opt_entry->list_values->len)
						value2 = g_ptr_array_index(opt_entry->list_values, sel);
					purple_account_set_string(account, opt_entry->setting, value2);
					break;

				default:
					break;
			}
		}
	}

	/* Set the proxy stuff. */
	proxy_info = purple_account_get_proxy_info(account);

	/* Create the proxy info if it doesn't exist. */
	if (proxy_info == NULL) {
		proxy_info = purple_proxy_info_new();
		purple_account_set_proxy_info(account, proxy_info);
	}

	/* Set the proxy info type. */
	purple_proxy_info_set_type(proxy_info, dialog->new_proxy_type);

	/* Host */
	value = entry_text(dialog->proxy_host_entry);

	if (*value != '\0')
		purple_proxy_info_set_host(proxy_info, value);
	else
		purple_proxy_info_set_host(proxy_info, NULL);

	/* Port */
	value = entry_text(dialog->proxy_port_entry);

	if (*value != '\0')
		purple_proxy_info_set_port(proxy_info, atoi(value));
	else
		purple_proxy_info_set_port(proxy_info, 0);

	/* Username */
	value = entry_text(dialog->proxy_user_entry);

	if (*value != '\0')
		purple_proxy_info_set_username(proxy_info, value);
	else
		purple_proxy_info_set_username(proxy_info, NULL);

	/* Password */
	value = entry_text(dialog->proxy_pass_entry);

	if (*value != '\0')
		purple_proxy_info_set_password(proxy_info, value);
	else
		purple_proxy_info_set_password(proxy_info, NULL);

	/* If there are no values set then proxy_info NULL */
	if ((purple_proxy_info_get_type(proxy_info) == PURPLE_PROXY_USE_GLOBAL) &&
		(purple_proxy_info_get_host(proxy_info) == NULL) &&
		(purple_proxy_info_get_port(proxy_info) == 0) &&
		(purple_proxy_info_get_username(proxy_info) == NULL) &&
		(purple_proxy_info_get_password(proxy_info) == NULL))
	{
		purple_account_set_proxy_info(account, NULL);
		proxy_info = NULL;
	}

	/* If this is a new account, add it to our list */
	if (new_acct)
		purple_accounts_add(account);
	else
		purple_signal_emit(pidgin_account_get_handle(), "account-modified", account);

	/* If this is a new account, then sign on! */
	if (gtk_widget_get_visible(dialog->register_button) &&
	    gtk_check_button_get_active(GTK_CHECK_BUTTON(dialog->register_button))) {
		purple_account_register(account);
	} else if (new_acct) {
		const PurpleSavedStatus *saved_status;

		saved_status = purple_savedstatus_get_current();
		if (saved_status != NULL) {
			purple_savedstatus_activate_for_account(saved_status, account);
			purple_account_set_enabled(account, PIDGIN_UI, TRUE);
		}
	}

	/* We no longer need the data from the dialog window */
	gtk_window_destroy(GTK_WINDOW(dialog->window));
}

void
pidgin_account_dialog_show(PidginAccountDialogType type,
							 PurpleAccount *account)
{
	AccountPrefsDialog *dialog;
	GtkWidget *win;
	GtkWidget *main_vbox;
	GtkWidget *vbox;
	GtkWidget *dbox;
	GtkWidget *notebook;
	GtkWidget *button;
	GtkWindow *parent;

	if (account != NULL && account_pref_wins != NULL &&
		(dialog = g_hash_table_lookup(account_pref_wins, account)) != NULL)
	{
		gtk_window_present(GTK_WINDOW(dialog->window));
		return;
	}

	dialog = g_new0(AccountPrefsDialog, 1);

	if (account != NULL && account_pref_wins != NULL)
	{
		g_hash_table_insert(account_pref_wins, account, dialog);
	}

	dialog->account = account;
	dialog->type    = type;
	dialog->sg      = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);

	if (dialog->account == NULL) {
		/* Select the first prpl in the list*/
		GList *prpl_list = purple_plugins_get_protocols();
		if (prpl_list != NULL)
			dialog->protocol_id = g_strdup(((PurplePlugin *) prpl_list->data)->info->id);
	}
	else
	{
		dialog->protocol_id =
			g_strdup(purple_account_get_protocol_id(dialog->account));
	}

	if ((dialog->plugin = purple_find_prpl(dialog->protocol_id)) != NULL)
		dialog->prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(dialog->plugin);

	if (accounts_window != NULL && gtk_widget_get_visible(accounts_window->window))
		parent = GTK_WINDOW(accounts_window->window);
	else
		parent = pidgin_get_active_window();

	dialog->window = win = pidgin_dialog_new(
		(type == PIDGIN_ADD_ACCOUNT_DIALOG) ? _("Add Account") : _("Modify Account"),
		parent, "account", TRUE);

	g_signal_connect(win, "destroy", G_CALLBACK(account_win_destroy_cb), dialog);
	g_object_set_data(G_OBJECT(win), "pidgin-account-dialog", dialog);

	/* Setup the vbox */
	main_vbox = pidgin_dialog_get_content_area(win);

	dialog->notebook = notebook = gtk_notebook_new();
	gtk_widget_set_vexpand(notebook, TRUE);
	gtk_box_append(GTK_BOX(main_vbox), notebook);

	/* Setup the inner vbox */
	dialog->top_vbox = vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_top(vbox, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_bottom(vbox, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_start(vbox, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_end(vbox, PIDGIN_HIG_BORDER);
	gtk_notebook_append_page(GTK_NOTEBOOK(notebook), vbox,
			gtk_label_new_with_mnemonic(_("_Basic")));

	/* The two frames are rebuilt when the protocol changes; they live in
	 * fixed slots so their order stays put. */
	dialog->login_slot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(vbox), dialog->login_slot);
	dialog->user_slot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(vbox), dialog->user_slot);

	/* Setup the top frames. */
	add_login_options(dialog, dialog->login_slot);
	add_user_options(dialog, dialog->user_slot);

	button = gtk_check_button_new_with_mnemonic(
		_("Create _this new account on the server"));
	gtk_box_append(GTK_BOX(main_vbox), button);
	dialog->register_button = button;

	/* Setup the page with 'Advanced' (protocol options). */
	add_protocol_options(dialog);

	/* Setup the page with 'Proxy'. */
	dbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_top(dbox, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_bottom(dbox, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_start(dbox, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_end(dbox, PIDGIN_HIG_BORDER);
	gtk_notebook_append_page(GTK_NOTEBOOK(notebook), dbox,
			gtk_label_new_with_mnemonic(_("P_roxy")));
	add_proxy_options(dialog, dbox);

	/* Cancel button */
	pidgin_dialog_add_button(win, _("_Cancel"), G_CALLBACK(cancel_account_prefs_cb), dialog);

	/* Save button */
	button = pidgin_dialog_add_button(win,
	                                  (type == PIDGIN_ADD_ACCOUNT_DIALOG) ? _("_Add") : _("_Save"),
	                                  G_CALLBACK(ok_account_prefs_cb),
	                                  dialog);
	gtk_widget_add_css_class(button, "suggested-action");
	gtk_window_set_default_widget(GTK_WINDOW(win), button);
	dialog->ok_button = button;

	update_register_button(dialog);

	/* TODO(M3): dropping an image file on the editor set the buddy icon. */

	/* Show the window. */
	gtk_window_present(GTK_WINDOW(win));
	if (!account)
		gtk_widget_grab_focus(dialog->protocol_menu);
}

/**************************************************************************
 * Accounts Dialog
 **************************************************************************/

static guint
find_account_position(PurpleAccount *account, PidginAccountRow **row_out)
{
	guint i, n;

	if (row_out)
		*row_out = NULL;
	if (accounts_window == NULL)
		return GTK_INVALID_LIST_POSITION;

	n = g_list_model_get_n_items(G_LIST_MODEL(accounts_window->store));
	for (i = 0; i < n; i++) {
		PidginAccountRow *row = g_list_model_get_item(G_LIST_MODEL(accounts_window->store), i);
		gboolean match = (row->account == account);

		if (match && row_out)
			*row_out = row;
		else
			g_object_unref(row);
		if (match)
			return i;
	}
	return GTK_INVALID_LIST_POSITION;
}

static PurpleAccount *
selected_account(void)
{
	PidginAccountRow *row;

	if (accounts_window == NULL)
		return NULL;

	row = gtk_single_selection_get_selected_item(accounts_window->selection);
	return row ? row->account : NULL;
}

static char *
account_status_text(PurpleAccount *account)
{
	const PurpleConnectionErrorInfo *err = purple_account_get_current_error(account);

	if (purple_account_is_connected(account)) {
		PurpleStatus *status = purple_account_get_active_status(account);
		return g_strdup(status ? purple_status_get_name(status) : _("Online"));
	}
	if (purple_account_is_connecting(account))
		return g_strdup(_("Connecting"));
	if (err != NULL && err->description != NULL)
		return g_strdup(err->description);
	if (!purple_account_get_enabled(account, PIDGIN_UI))
		return g_strdup(_("Disabled"));
	return g_strdup(_("Offline"));
}

/* Column cells: setup/bind/unbind, with the row's "changed" signal
 * re-running the column's update function. */
typedef void (*CellUpdateFunc)(GtkWidget *child, PurpleAccount *account);

static void
cell_row_changed_cb(PidginAccountRow *row, GtkListItem *li)
{
	CellUpdateFunc update = g_object_get_data(G_OBJECT(li), "pidgin-update");
	GtkWidget *child = gtk_list_item_get_child(li);

	if (update != NULL && child != NULL)
		update(child, row->account);
}

static void
cell_bind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	PidginAccountRow *row = gtk_list_item_get_item(li);
	gulong id;

	g_object_set_data(G_OBJECT(li), "pidgin-update", data);
	cell_row_changed_cb(row, li);
	id = g_signal_connect(row, "changed", G_CALLBACK(cell_row_changed_cb), li);
	g_object_set_data(G_OBJECT(li), "pidgin-handler", GSIZE_TO_POINTER(id));
}

static void
cell_unbind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	PidginAccountRow *row = gtk_list_item_get_item(li);
	gulong id = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(li), "pidgin-handler"));

	if (row != NULL && id != 0)
		g_signal_handler_disconnect(row, id);
	g_object_set_data(G_OBJECT(li), "pidgin-handler", NULL);
}

/* Enabled column */
static void
enabled_toggled_cb(GtkCheckButton *check, gpointer data)
{
	PurpleAccount *account = g_object_get_data(G_OBJECT(check), "pidgin-account");
	gboolean enabled = gtk_check_button_get_active(check);

	if (!account_exists(account))
		return;
	if (enabled == purple_account_get_enabled(account, PIDGIN_UI))
		return;

	/*
	 * If we just enabled the account, then set the statuses
	 * to the current status.
	 */
	if (enabled)
	{
		const PurpleSavedStatus *saved_status = purple_savedstatus_get_current();
		purple_savedstatus_activate_for_account(saved_status, account);
	}

	purple_account_set_enabled(account, PIDGIN_UI, enabled);
}

static void
enabled_setup_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	GtkWidget *check = gtk_check_button_new();

	gtk_widget_set_halign(check, GTK_ALIGN_CENTER);
	gtk_accessible_update_property(GTK_ACCESSIBLE(check),
		GTK_ACCESSIBLE_PROPERTY_LABEL, _("Enabled"), -1);
	g_signal_connect(check, "toggled", G_CALLBACK(enabled_toggled_cb), NULL);
	gtk_list_item_set_child(li, check);
}

static void
enabled_update(GtkWidget *child, PurpleAccount *account)
{
	g_signal_handlers_block_by_func(child, enabled_toggled_cb, NULL);
	g_object_set_data(G_OBJECT(child), "pidgin-account", account);
	gtk_check_button_set_active(GTK_CHECK_BUTTON(child),
		purple_account_get_enabled(account, PIDGIN_UI));
	g_signal_handlers_unblock_by_func(child, enabled_toggled_cb, NULL);
}

/* Username column */
static void
username_setup_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	GtkWidget *box, *image, *label;

	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	image = gtk_image_new();
	gtk_image_set_pixel_size(GTK_IMAGE(image),
		pidgin_prpl_icon_size_to_pixels(PIDGIN_PRPL_ICON_MEDIUM));
	label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_box_append(GTK_BOX(box), image);
	gtk_box_append(GTK_BOX(box), label);
	gtk_list_item_set_child(li, box);
}

static void
username_update(GtkWidget *child, PurpleAccount *account)
{
	GtkWidget *image = gtk_widget_get_first_child(child);
	GtkWidget *label = gtk_widget_get_next_sibling(image);
	const char *alias = purple_account_get_alias(account);
	GIcon *icon;
	char *text;

	icon = pidgin_create_prpl_gicon(account, NULL);
	gtk_image_set_from_gicon(GTK_IMAGE(image), icon);
	g_object_unref(icon);
	/* Pidgin 2 desaturated the icon of disconnected accounts. */
	gtk_widget_set_opacity(image, purple_account_is_connected(account) ? 1.0 : 0.5);

	if (alias != NULL && *alias != '\0')
		text = g_strdup_printf("%s (%s)", purple_account_get_username(account), alias);
	else
		text = g_strdup(purple_account_get_username(account));
	gtk_label_set_text(GTK_LABEL(label), text);
	g_free(text);
}

/* Protocol and Status columns: plain labels */
static void
label_setup_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	GtkWidget *label = gtk_label_new(NULL);

	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_list_item_set_child(li, label);
}

static void
protocol_update(GtkWidget *child, PurpleAccount *account)
{
	gtk_label_set_text(GTK_LABEL(child), purple_account_get_protocol_name(account));
}

static void
status_update(GtkWidget *child, PurpleAccount *account)
{
	char *text = account_status_text(account);
	const PurpleConnectionErrorInfo *err = purple_account_get_current_error(account);

	gtk_label_set_text(GTK_LABEL(child), text);
	gtk_widget_set_tooltip_text(child, text);
	if (err != NULL && !purple_account_is_connected(account))
		gtk_widget_add_css_class(child, "error");
	else
		gtk_widget_remove_css_class(child, "error");
	g_free(text);
}

static GtkColumnViewColumn *
add_column(GtkWidget *columnview, const char *title, GCallback setup,
           CellUpdateFunc update, gboolean expand)
{
	GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
	GtkColumnViewColumn *column;

	g_signal_connect(factory, "setup", setup, NULL);
	g_signal_connect(factory, "bind", G_CALLBACK(cell_bind_cb), update);
	g_signal_connect(factory, "unbind", G_CALLBACK(cell_unbind_cb), NULL);

	column = gtk_column_view_column_new(title, factory);
	gtk_column_view_column_set_expand(column, expand);
	gtk_column_view_column_set_resizable(column, TRUE);
	gtk_column_view_append_column(GTK_COLUMN_VIEW(columnview), column);
	g_object_unref(column);

	return column;
}

static void
add_account_to_store(PurpleAccount *account, gpointer data)
{
	PidginAccountRow *row;
	int index;
	guint n;

	if (accounts_window == NULL)
		return;
	if (find_account_position(account, NULL) != GTK_INVALID_LIST_POSITION)
		return;

	row = pidgin_account_row_new(account);
	index = g_list_index(purple_accounts_get_all(), account);
	n = g_list_model_get_n_items(G_LIST_MODEL(accounts_window->store));
	if (index < 0 || (guint)index > n)
		index = n;
	g_list_store_insert(accounts_window->store, index, row);
	g_object_unref(row);
}

static void
populate_accounts_list(AccountsWindow *win)
{
	GList *l;

	g_list_store_remove_all(win->store);

	for (l = purple_accounts_get_all(); l != NULL; l = l->next) {
		PidginAccountRow *row = pidgin_account_row_new(l->data);

		g_list_store_append(win->store, row);
		g_object_unref(row);
	}
}

static void
update_actions_menu(void)
{
	PurpleAccount *account;
	PurpleConnection *gc;
	PurplePlugin *prpl;
	GSimpleActionGroup *group;
	GMenu *menu = NULL;

	if (accounts_window == NULL)
		return;

	account = selected_account();
	group = g_simple_action_group_new();

	if (account != NULL && purple_account_is_connected(account) &&
	    (gc = purple_account_get_connection(account)) != NULL &&
	    (prpl = purple_connection_get_prpl(gc)) != NULL)
		menu = pidgin_menu_from_plugin_actions(prpl, gc, group, "acct");

	gtk_widget_insert_action_group(accounts_window->actions_button, "acct",
	                               G_ACTION_GROUP(group));
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(accounts_window->actions_button),
	                               menu ? G_MENU_MODEL(menu) : NULL);
	gtk_widget_set_sensitive(accounts_window->actions_button, menu != NULL);

	if (menu != NULL)
		g_object_unref(menu);
	g_object_unref(group);
}

static void
update_buttons_sensitivity(void)
{
	gboolean have = (selected_account() != NULL);

	gtk_widget_set_sensitive(accounts_window->modify_button, have);
	gtk_widget_set_sensitive(accounts_window->delete_button, have);
	update_actions_menu();
}

static void
selection_changed_cb(GtkSelectionModel *model, guint position, guint n_items,
                     gpointer data)
{
	if (accounts_window != NULL)
		update_buttons_sensitivity();
}

static void
accounts_window_refresh(PurpleAccount *account)
{
	PidginAccountRow *row = NULL;

	if (accounts_window == NULL)
		return;

	find_account_position(account, &row);
	if (row != NULL) {
		g_signal_emit(row, row_signals[ROW_SIGNAL_CHANGED], 0);
		g_object_unref(row);
	}

	if (account == selected_account())
		update_actions_menu();
}

static void
add_account_cb(GtkWidget *w, gpointer data)
{
	pidgin_account_dialog_show(PIDGIN_ADD_ACCOUNT_DIALOG, NULL);
}

static void
modify_account_cb(GtkWidget *w, gpointer data)
{
	PurpleAccount *account = selected_account();

	if (account != NULL)
		pidgin_account_dialog_show(PIDGIN_MODIFY_ACCOUNT_DIALOG, account);
}

static void
row_activate_cb(GtkColumnView *view, guint position, gpointer data)
{
	PidginAccountRow *row;

	row = g_list_model_get_item(G_LIST_MODEL(accounts_window->selection), position);
	if (row != NULL) {
		pidgin_account_dialog_show(PIDGIN_MODIFY_ACCOUNT_DIALOG, row->account);
		g_object_unref(row);
	}
}

static void
delete_account_response_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	PurpleAccount *account = data;
	int button;

	button = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, NULL);
	if (button == 1 && account_exists(account))
		purple_accounts_delete(account);
}

static void
ask_delete_account_cb(GtkWidget *w, gpointer data)
{
	PurpleAccount *account = selected_account();
	GtkAlertDialog *alert;
	const char *buttons[] = { _("_Cancel"), _("_Delete"), NULL };

	if (account == NULL)
		return;

	alert = gtk_alert_dialog_new(_("Are you sure you want to delete %s?"),
	                             purple_account_get_username(account));
	gtk_alert_dialog_set_detail(alert, purple_account_get_protocol_name(account));
	gtk_alert_dialog_set_buttons(alert, buttons);
	gtk_alert_dialog_set_cancel_button(alert, 0);
	gtk_alert_dialog_set_default_button(alert, 0);
	gtk_alert_dialog_choose(alert, GTK_WINDOW(accounts_window->window), NULL,
	                        delete_account_response_cb, account);
	g_object_unref(alert);
}

static void
save_window_size(void)
{
	int width, height;

	if (accounts_window == NULL || accounts_window->window == NULL)
		return;

	gtk_window_get_default_size(GTK_WINDOW(accounts_window->window), &width, &height);
	if (width > 0 && height > 0) {
		purple_prefs_set_int(PREFS_DIALOG "/width", width);
		purple_prefs_set_int(PREFS_DIALOG "/height", height);
	}
}

static gboolean
accounts_close_request_cb(GtkWindow *window, gpointer data)
{
	/* The window only hides (hide-on-close); it is the main window until
	 * the buddy list exists (TODO(M3)), and pidgin4 keeps running. */
	save_window_size();
	return FALSE;
}

static void
close_accounts_cb(GtkWidget *w, gpointer data)
{
	gtk_window_close(GTK_WINDOW(accounts_window->window));
}

static void
accounts_window_destroy_cb(GtkWidget *w, gpointer data)
{
	if (accounts_window == NULL)
		return;

	g_clear_object(&accounts_window->store);
	g_free(accounts_window);
	accounts_window = NULL;
}

static GtkWidget *
make_primary_menu_button(void)
{
	GtkWidget *button = gtk_menu_button_new();
	GMenu *menu = g_menu_new();
	GMenu *section = g_menu_new();

	g_menu_append(section, _("_Debug Window"), "app.debug");
	g_menu_append(section, _("_About"), "app.about");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);

	section = g_menu_new();
	g_menu_append(section, _("_Quit"), "app.quit");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);

	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), "open-menu-symbolic");
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(button), G_MENU_MODEL(menu));
	gtk_menu_button_set_primary(GTK_MENU_BUTTON(button), TRUE);
	gtk_widget_set_tooltip_text(button, _("Main Menu"));
	g_object_unref(menu);

	return button;
}

void
pidgin_accounts_window_show(void)
{
	AccountsWindow *dialog;
	GtkWidget *win, *vbox, *hbox, *sw, *button, *spacer;
	int width, height;

	if (accounts_window != NULL) {
		gtk_window_present(GTK_WINDOW(accounts_window->window));
		return;
	}

	accounts_window = dialog = g_new0(AccountsWindow, 1);

	width  = purple_prefs_get_int(PREFS_DIALOG "/width");
	height = purple_prefs_get_int(PREFS_DIALOG "/height");

	dialog->window = win = gtk_window_new();
	gtk_window_set_application(GTK_WINDOW(win), pidgin_application_get());
	gtk_window_set_title(GTK_WINDOW(win), _("Accounts"));
	gtk_widget_set_name(win, "accounts");
	gtk_window_set_default_size(GTK_WINDOW(win), width, height);
	gtk_window_set_hide_on_close(GTK_WINDOW(win), TRUE);
	g_signal_connect(win, "close-request", G_CALLBACK(accounts_close_request_cb), NULL);
	g_signal_connect(win, "destroy", G_CALLBACK(accounts_window_destroy_cb), NULL);

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_top(vbox, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_bottom(vbox, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_start(vbox, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_end(vbox, PIDGIN_HIG_BORDER);
	gtk_window_set_child(GTK_WINDOW(win), vbox);

	/* The list */
	dialog->store = g_list_store_new(PIDGIN_TYPE_ACCOUNT_ROW);
	dialog->selection = gtk_single_selection_new(
		G_LIST_MODEL(g_object_ref(dialog->store)));
	gtk_single_selection_set_autoselect(dialog->selection, FALSE);
	gtk_single_selection_set_can_unselect(dialog->selection, TRUE);
	g_signal_connect(dialog->selection, "selection-changed",
	                 G_CALLBACK(selection_changed_cb), NULL);

	dialog->columnview = gtk_column_view_new(GTK_SELECTION_MODEL(dialog->selection));
	gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(dialog->columnview), FALSE);
	g_signal_connect(dialog->columnview, "activate", G_CALLBACK(row_activate_cb), NULL);

	add_column(dialog->columnview, _("Enabled"), G_CALLBACK(enabled_setup_cb),
	           enabled_update, FALSE);
	add_column(dialog->columnview, _("Username"), G_CALLBACK(username_setup_cb),
	           username_update, TRUE);
	add_column(dialog->columnview, _("Protocol"), G_CALLBACK(label_setup_cb),
	           protocol_update, FALSE);
	add_column(dialog->columnview, _("Status"), G_CALLBACK(label_setup_cb),
	           status_update, TRUE);

	sw = pidgin_make_scrollable(dialog->columnview, GTK_POLICY_AUTOMATIC,
	                            GTK_POLICY_AUTOMATIC, -1, -1);
	gtk_widget_set_vexpand(sw, TRUE);
	gtk_box_append(GTK_BOX(vbox), sw);

	/* TODO(M5): drag-and-drop reordering (purple_accounts_reorder). */

	/* The buttons */
	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	gtk_box_append(GTK_BOX(vbox), hbox);

	gtk_box_append(GTK_BOX(hbox), make_primary_menu_button());

	dialog->actions_button = gtk_menu_button_new();
	gtk_menu_button_set_label(GTK_MENU_BUTTON(dialog->actions_button), _("Account _Actions"));
	gtk_menu_button_set_use_underline(GTK_MENU_BUTTON(dialog->actions_button), TRUE);
	gtk_widget_set_tooltip_text(dialog->actions_button,
		_("Protocol actions of the selected account (when it is connected)"));
	gtk_box_append(GTK_BOX(hbox), dialog->actions_button);

	spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_hexpand(spacer, TRUE);
	gtk_box_append(GTK_BOX(hbox), spacer);

	button = gtk_button_new_with_mnemonic(_("_Add..."));
	g_signal_connect(button, "clicked", G_CALLBACK(add_account_cb), NULL);
	gtk_box_append(GTK_BOX(hbox), button);

	dialog->modify_button = button = gtk_button_new_with_mnemonic(_("_Modify..."));
	g_signal_connect(button, "clicked", G_CALLBACK(modify_account_cb), NULL);
	gtk_box_append(GTK_BOX(hbox), button);

	dialog->delete_button = button = gtk_button_new_with_mnemonic(_("_Delete"));
	g_signal_connect(button, "clicked", G_CALLBACK(ask_delete_account_cb), NULL);
	gtk_box_append(GTK_BOX(hbox), button);

	button = gtk_button_new_with_mnemonic(_("_Close"));
	g_signal_connect(button, "clicked", G_CALLBACK(close_accounts_cb), NULL);
	gtk_box_append(GTK_BOX(hbox), button);

	populate_accounts_list(dialog);
	update_buttons_sensitivity();

	gtk_window_present(GTK_WINDOW(win));

	/* No-op unless PIDGIN4_ACCOUNT_SELFTEST is set. */
	pidgin_account_selftest();
}

void
pidgin_accounts_window_hide(void)
{
	if (accounts_window == NULL)
		return;

	save_window_size();
	/* The destroy handler frees accounts_window. */
	gtk_window_destroy(GTK_WINDOW(accounts_window->window));
}

/**************************************************************************
 * libpurple signal handlers
 **************************************************************************/

/* Callbacks for signals whose first argument is the account. The other
 * arguments (and the user data, which follows them) are not used. */
static void
account_changed_cb(PurpleAccount *account, gpointer unused)
{
	accounts_window_refresh(account);
}

static void
connection_changed_cb(PurpleConnection *gc, gpointer unused)
{
	accounts_window_refresh(purple_connection_get_account(gc));
}

static void
account_added_cb(PurpleAccount *account, gpointer unused)
{
	add_account_to_store(account, NULL);
}

static void
account_removed_cb(PurpleAccount *account, gpointer unused)
{
	AccountPrefsDialog *dialog;
	guint pos;

	/* If the account was being modified, close the edit window */
	if (account_pref_wins != NULL &&
	    (dialog = g_hash_table_lookup(account_pref_wins, account)) != NULL)
		gtk_window_destroy(GTK_WINDOW(dialog->window));

	if (accounts_window == NULL)
		return;

	/* Remove the account from the list */
	pos = find_account_position(account, NULL);
	if (pos != GTK_INVALID_LIST_POSITION)
		g_list_store_remove(accounts_window->store, pos);
}

/**************************************************************************
 * Account UI ops: requests from the prpls
 **************************************************************************/

typedef struct
{
	PurpleAccount *account;
	char *username;
	char *alias;

} PidginAccountAddUserData;

struct auth_request
{
	PurpleAccountRequestAuthorizationCb auth_cb;
	PurpleAccountRequestAuthorizationCb deny_cb;
	void *data;
	char *username;
	char *alias;
	PurpleAccount *account;
	gboolean add_buddy_after_auth;
	GtkWidget *window;
	gboolean answered;
};

static GtkWidget *
request_window_new(const char *title)
{
	GtkWindow *parent = NULL;
	GtkWidget *win;

	if (accounts_window != NULL && gtk_widget_get_visible(accounts_window->window))
		parent = GTK_WINDOW(accounts_window->window);
	else
		parent = pidgin_get_active_window();

	win = pidgin_dialog_new(title, parent, "account-request", FALSE);
	request_windows = g_list_prepend(request_windows, win);
	return win;
}

static void
request_window_forget(GtkWidget *win)
{
	request_windows = g_list_remove(request_windows, win);
}

static void
request_close_button_cb(GtkWidget *button, GtkWidget *win)
{
	gtk_window_destroy(GTK_WINDOW(win));
}

static void
request_window_destroy_cb(GtkWidget *win, gpointer data)
{
	request_window_forget(win);
}

static void
request_add_buddy(PurpleAccount *account, const char *username, const char *alias)
{
	PurpleBlistUiOps *ops = purple_blist_get_ui_ops();

	/* TODO(M3): the buddy list provides the add-buddy dialog. With the
	 * M2 stub blist ops purple_blist_request_add_buddy() does nothing. */
	if (ops == NULL || ops->request_add_buddy == NULL)
		purple_debug_warning("gtkaccount", "Cannot add %s to the buddy list of %s "
			"yet: the buddy list is not ported (M3)\n", username,
			purple_account_get_username(account));

	purple_blist_request_add_buddy(account, username, NULL, alias);
}

static char *
make_info(PurpleAccount *account, PurpleConnection *gc, const char *remote_user,
          const char *id, const char *alias, const char *msg)
{
	if (msg != NULL && *msg == '\0')
		msg = NULL;

	return g_strdup_printf(_("%s%s%s%s has made %s his or her buddy%s%s"),
	                       remote_user,
	                       (alias != NULL ? " ("  : ""),
	                       (alias != NULL ? alias : ""),
	                       (alias != NULL ? ")"   : ""),
	                       (id != NULL
	                        ? id
	                        : (gc != NULL && purple_connection_get_display_name(gc) != NULL
	                           ? purple_connection_get_display_name(gc)
	                           : purple_account_get_username(account))),
	                       (msg != NULL ? ": " : "."),
	                       (msg != NULL ? msg  : ""));
}

static void
pidgin_accounts_notify_added(PurpleAccount *account, const char *remote_user,
                               const char *id, const char *alias,
                               const char *msg)
{
	char *buffer;
	PurpleConnection *gc;
	GtkWidget *win, *button;

	gc = purple_account_get_connection(account);

	buffer = make_info(account, gc, remote_user, id, alias, msg);

	win = request_window_new(purple_account_get_username(account));
	g_signal_connect(win, "destroy", G_CALLBACK(request_window_destroy_cb), NULL);
	pidgin_dialog_add_message(win, "dialog-information", NULL, buffer, FALSE);
	button = pidgin_dialog_add_button(win, _("_Close"),
		G_CALLBACK(request_close_button_cb), win);
	gtk_window_set_default_widget(GTK_WINDOW(win), button);
	gtk_window_present(GTK_WINDOW(win));

	g_free(buffer);
}

static void
free_add_user_data(PidginAccountAddUserData *data)
{
	g_free(data->username);
	g_free(data->alias);
	g_free(data);
}

static void
add_user_cb(GtkWidget *button, GtkWidget *win)
{
	PidginAccountAddUserData *data = g_object_get_data(G_OBJECT(win), "pidgin-add-data");
	PurpleConnection *gc = purple_account_get_connection(data->account);

	if (g_list_find(purple_connections_get_all(), gc))
		request_add_buddy(data->account, data->username, data->alias);

	gtk_window_destroy(GTK_WINDOW(win));
}

static void
pidgin_accounts_request_add(PurpleAccount *account, const char *remote_user,
                              const char *id, const char *alias,
                              const char *msg)
{
	char *buffer;
	PurpleConnection *gc;
	PidginAccountAddUserData *data;
	GtkWidget *win, *button;

	gc = purple_account_get_connection(account);

	data = g_new0(PidginAccountAddUserData, 1);
	data->account  = account;
	data->username = g_strdup(remote_user);
	data->alias    = g_strdup(alias);

	buffer = make_info(account, gc, remote_user, id, alias, msg);

	win = request_window_new(purple_account_get_username(account));
	g_object_set_data_full(G_OBJECT(win), "pidgin-add-data", data,
	                       (GDestroyNotify)free_add_user_data);
	g_signal_connect(win, "destroy", G_CALLBACK(request_window_destroy_cb), NULL);
	pidgin_dialog_add_message(win, "dialog-question",
		_("Add buddy to your list?"), buffer, FALSE);
	pidgin_dialog_add_button(win, _("_Cancel"), G_CALLBACK(request_close_button_cb), win);
	button = pidgin_dialog_add_button(win, _("_Add"), G_CALLBACK(add_user_cb), win);
	gtk_window_set_default_widget(GTK_WINDOW(win), button);
	gtk_window_present(GTK_WINDOW(win));

	g_free(buffer);
}

static void
free_auth_request(struct auth_request *ar)
{
	g_free(ar->username);
	g_free(ar->alias);
	g_free(ar);
}

static void
authorize_and_add_cb(GtkWidget *button, struct auth_request *ar)
{
	ar->answered = TRUE;
	ar->auth_cb(ar->data);
	if (ar->add_buddy_after_auth) {
		request_add_buddy(ar->account, ar->username, ar->alias);
	}
	gtk_window_destroy(GTK_WINDOW(ar->window));
}

static void
deny_no_add_cb(GtkWidget *button, struct auth_request *ar)
{
	ar->answered = TRUE;
	ar->deny_cb(ar->data);
	gtk_window_destroy(GTK_WINDOW(ar->window));
}

static gboolean
auth_activate_link_cb(GtkLabel *label, const char *uri, struct auth_request *ar)
{
	if (purple_strequal(uri, "viewinfo")) {
		PurpleConnection *gc = purple_account_get_connection(ar->account);

		if (gc != NULL)
			serv_get_info(gc, ar->username);
		return TRUE;
	}
	return FALSE;
}

/* The request window is going away: tell libpurple (unless it is the one
 * closing it), which is a no-op after authorize/deny. */
static void
auth_window_destroy_cb(GtkWidget *win, struct auth_request *ar)
{
	request_window_forget(win);

	if (!ar->answered && !uninitializing &&
	    g_object_get_data(G_OBJECT(win), "pidgin-closing") == NULL)
		purple_account_request_close(win);

	free_auth_request(ar);
}

static void *
pidgin_accounts_request_authorization(PurpleAccount *account,
                                      const char *remote_user,
                                      const char *id,
                                      const char *alias,
                                      const char *message,
                                      gboolean on_list,
                                      PurpleAccountRequestAuthorizationCb auth_cb,
                                      PurpleAccountRequestAuthorizationCb deny_cb,
                                      void *user_data)
{
	char *buffer;
	PurpleConnection *gc;
	GtkWidget *win, *button, *hbox, *image, *content;
	struct auth_request *aa;
	const char *our_name;
	gboolean have_valid_alias = alias && *alias;
	char *escaped_remote_user, *escaped_alias, *escaped_our_name, *escaped_message;

	gc = purple_account_get_connection(account);
	if (message != NULL && *message == '\0')
		message = NULL;

	our_name = (id != NULL) ? id :
			(gc != NULL && purple_connection_get_display_name(gc) != NULL) ? purple_connection_get_display_name(gc) :
			purple_account_get_username(account);

	escaped_remote_user = g_markup_escape_text(remote_user, -1);
	escaped_alias = alias != NULL ? g_markup_escape_text(alias, -1) : g_strdup("");
	escaped_our_name = g_markup_escape_text(our_name, -1);
	escaped_message = message != NULL ? g_markup_escape_text(message, -1) : g_strdup("");
	buffer = g_strdup_printf(_("<a href=\"viewinfo\">%s</a>%s%s%s wants to add you (%s) to his or her buddy list%s%s"),
				escaped_remote_user,
				(have_valid_alias ? " ("  : ""),
				escaped_alias,
				(have_valid_alias ? ")"   : ""),
				escaped_our_name,
				(message != NULL ? ": " : "."),
				escaped_message);
	g_free(escaped_remote_user);
	g_free(escaped_alias);
	g_free(escaped_our_name);
	g_free(escaped_message);

	aa = g_new0(struct auth_request, 1);
	aa->auth_cb = auth_cb;
	aa->deny_cb = deny_cb;
	aa->data = user_data;
	aa->username = g_strdup(remote_user);
	aa->alias = g_strdup(alias);
	aa->account = account;
	aa->add_buddy_after_auth = !on_list;

	aa->window = win = request_window_new(_("Authorize buddy?"));
	g_signal_connect(win, "destroy", G_CALLBACK(auth_window_destroy_cb), aa);

	hbox = pidgin_dialog_add_message(win, NULL, _("Authorize buddy?"), buffer, TRUE);
	image = pidgin_create_prpl_image(account, NULL, PIDGIN_PRPL_ICON_LARGE);
	gtk_widget_set_valign(image, GTK_ALIGN_START);
	gtk_box_prepend(GTK_BOX(hbox), image);

	/* The "viewinfo" link in the description */
	content = gtk_widget_get_last_child(hbox);
	for (content = gtk_widget_get_first_child(content); content != NULL;
	     content = gtk_widget_get_next_sibling(content)) {
		if (GTK_IS_LABEL(content))
			g_signal_connect(content, "activate-link",
			                 G_CALLBACK(auth_activate_link_cb), aa);
	}

	/* TODO(M4): "Send Instant Message" needs the conversation window. */
	pidgin_dialog_add_button(win, _("_Deny"), G_CALLBACK(deny_no_add_cb), aa);
	button = pidgin_dialog_add_button(win, _("_Authorize"), G_CALLBACK(authorize_and_add_cb), aa);
	gtk_window_set_default_widget(GTK_WINDOW(win), button);

	gtk_window_present(GTK_WINDOW(win));

	g_free(buffer);

	return win;
}

static void
pidgin_accounts_request_close(void *ui_handle)
{
	GtkWidget *win = ui_handle;

	/* libpurple is closing it: don't call back into it. */
	if (g_list_find(request_windows, win) == NULL)
		return;
	g_object_set_data(G_OBJECT(win), "pidgin-closing", GINT_TO_POINTER(1));
	gtk_window_destroy(GTK_WINDOW(win));
}

static PurpleAccountUiOps ui_ops =
{
	pidgin_accounts_notify_added,
	NULL,
	pidgin_accounts_request_add,
	pidgin_accounts_request_authorization,
	pidgin_accounts_request_close,
	NULL,
	NULL,
	NULL,
	NULL
};

PurpleAccountUiOps *
pidgin_accounts_get_ui_ops(void)
{
	return &ui_ops;
}

void *
pidgin_account_get_handle(void) {
	static int handle;

	return &handle;
}

void
pidgin_account_init(void)
{
	void *handle = pidgin_account_get_handle();
	void *accounts = purple_accounts_get_handle();
	void *conns = purple_connections_get_handle();

	/* Pidgin 2 registered /pidgin/accounts/dialog/{width,height} and
	 * /pidgin/accounts/buddyicon here. pidgin4 keeps its window size under
	 * /pidgin4/accounts (gtkprefs.c) and adds nothing under /pidgin. */

	purple_signal_register(handle, "account-modified",
						 purple_marshal_VOID__POINTER, NULL, 1,
						 purple_value_new(PURPLE_TYPE_SUBTYPE,
										PURPLE_SUBTYPE_ACCOUNT));

	/* Keep the accounts window in sync. */
	purple_signal_connect(conns, "signed-on", handle,
						PURPLE_CALLBACK(connection_changed_cb), NULL);
	purple_signal_connect(conns, "signed-off", handle,
						PURPLE_CALLBACK(connection_changed_cb), NULL);
	purple_signal_connect(accounts, "account-added", handle,
						PURPLE_CALLBACK(account_added_cb), NULL);
	purple_signal_connect(accounts, "account-removed", handle,
						PURPLE_CALLBACK(account_removed_cb), NULL);
	purple_signal_connect(accounts, "account-disabled", handle,
						PURPLE_CALLBACK(account_changed_cb), NULL);
	purple_signal_connect(accounts, "account-enabled", handle,
						PURPLE_CALLBACK(account_changed_cb), NULL);
	purple_signal_connect(accounts, "account-connecting", handle,
						PURPLE_CALLBACK(account_changed_cb), NULL);
	purple_signal_connect(accounts, "account-disconnected", handle,
						PURPLE_CALLBACK(account_changed_cb), NULL);
	purple_signal_connect(accounts, "account-status-changed", handle,
						PURPLE_CALLBACK(account_changed_cb), NULL);
	purple_signal_connect(accounts, "account-error-changed", handle,
						PURPLE_CALLBACK(account_changed_cb), NULL);
	purple_signal_connect(accounts, "account-alias-changed", handle,
						PURPLE_CALLBACK(account_changed_cb), NULL);
	purple_signal_connect(handle, "account-modified", handle,
						PURPLE_CALLBACK(account_changed_cb), NULL);

	account_pref_wins =
		g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, NULL);
}

void
pidgin_account_uninit(void)
{
	GList *dialogs, *l;

	uninitializing = TRUE;

	/* Close the editors (their destroy handlers remove them). */
	dialogs = g_hash_table_get_values(account_pref_wins);
	for (l = dialogs; l != NULL; l = l->next) {
		AccountPrefsDialog *dialog = l->data;
		gtk_window_destroy(GTK_WINDOW(dialog->window));
	}
	g_list_free(dialogs);

	while (request_windows != NULL)
		gtk_window_destroy(GTK_WINDOW(request_windows->data));

	pidgin_accounts_window_hide();

	g_hash_table_destroy(account_pref_wins);
	account_pref_wins = NULL;

	purple_signals_disconnect_by_handle(pidgin_account_get_handle());
	purple_signals_unregister_by_instance(pidgin_account_get_handle());

	uninitializing = FALSE;
}

/**************************************************************************
 * Self test (PIDGIN4_ACCOUNT_SELFTEST=1): opens and closes editors only.
 **************************************************************************/

static AccountPrefsDialog *selftest_add_dialog = NULL;
static guint selftest_protocol_index = 0;

static gboolean
selftest_close_cb(gpointer data)
{
	GList *dialogs, *l;
	int n = 0;

	dialogs = account_pref_wins ? g_hash_table_get_values(account_pref_wins) : NULL;
	for (l = dialogs; l != NULL; l = l->next) {
		AccountPrefsDialog *dialog = l->data;
		/* The same path as the Cancel button. */
		cancel_account_prefs_cb(NULL, dialog);
		n++;
	}
	g_list_free(dialogs);

	if (selftest_add_dialog != NULL) {
		cancel_account_prefs_cb(NULL, selftest_add_dialog);
		selftest_add_dialog = NULL;
		n++;
	}

	purple_debug_info("gtkaccount", "selftest: closed %d account editors\n", n);
	return G_SOURCE_REMOVE;
}

static gboolean
selftest_cycle_protocols_cb(gpointer data)
{
	GListModel *model;
	guint n;

	if (selftest_add_dialog == NULL)
		return G_SOURCE_REMOVE;

	model = gtk_drop_down_get_model(GTK_DROP_DOWN(selftest_add_dialog->protocol_menu));
	n = g_list_model_get_n_items(model);
	if (selftest_protocol_index >= n) {
		purple_debug_info("gtkaccount", "selftest: cycled the Add dialog through "
		                  "%u protocols\n", n);
		g_timeout_add_seconds(2, selftest_close_cb, NULL);
		return G_SOURCE_REMOVE;
	}

	gtk_drop_down_set_selected(GTK_DROP_DOWN(selftest_add_dialog->protocol_menu),
	                           selftest_protocol_index++);
	purple_debug_info("gtkaccount", "selftest: Add dialog protocol %s\n",
	                  selftest_add_dialog->protocol_id);
	return G_SOURCE_CONTINUE;
}

void
pidgin_account_selftest(void)
{
	static gboolean done = FALSE;
	GList *l;
	int n = 0;

	if (done || g_getenv("PIDGIN4_ACCOUNT_SELFTEST") == NULL)
		return;
	done = TRUE;

	for (l = purple_accounts_get_all(); l != NULL; l = l->next) {
		pidgin_account_dialog_show(PIDGIN_MODIFY_ACCOUNT_DIALOG, l->data);
		n++;
	}
	purple_debug_info("gtkaccount", "selftest: opened %d Modify dialogs\n", n);

	pidgin_account_dialog_show(PIDGIN_ADD_ACCOUNT_DIALOG, NULL);
	/* The Add dialog is not in account_pref_wins; find its window. */
	for (l = gtk_application_get_windows(pidgin_application_get()); l != NULL; l = l->next) {
		AccountPrefsDialog *d = g_object_get_data(G_OBJECT(l->data), "pidgin-account-dialog");

		if (d != NULL && d->account == NULL) {
			selftest_add_dialog = d;
			break;
		}
	}

	if (selftest_add_dialog != NULL)
		g_timeout_add(200, selftest_cycle_protocols_cb, NULL);
	else
		g_timeout_add_seconds(2, selftest_close_cb, NULL);
}
