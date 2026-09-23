/*
 * @file gtkconn.c GTK+ Connection API
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
 *
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "account.h"
#include "connection.h"
#include "debug.h"
#include "notify.h"
#include "prefs.h"
#include "util.h"

#include "gtkaccount.h"
#include "gtkconn.h"
#include "gtkdialogs.h"
#include "gtkutils.h"

#define INITIAL_RECON_DELAY_MIN  8000
#define INITIAL_RECON_DELAY_MAX 60000

#define MAX_RECON_DELAY 600000
#define MAX_RACCOON_DELAY "shorter in urban areas"

typedef struct {
	int delay;
	guint timeout;
} PidginAutoRecon;

/**
 * Contains accounts that are auto-reconnecting.
 * The key is a pointer to the PurpleAccount and the
 * value is a pointer to a PidginAutoRecon.
 */
static GHashTable *auto_reconns = NULL;

/**
 * Open error alerts, one per account. The key is the PurpleAccount, the
 * value the GCancellable that dismisses its GtkAlertDialog.
 *
 * TODO(M3): Pidgin 2 shows connection errors as mini-dialogs in the buddy
 * list (gtkblist.c, "account-error-changed"). Until the buddy list exists
 * they are alerts, and the accounts window shows the current error.
 */
static GHashTable *error_alerts = NULL;

static void
pidgin_connection_connect_progress(PurpleConnection *gc,
		const char *text, size_t step, size_t step_count)
{
	/* TODO(M3): the status box's connecting throbber. */
	purple_debug_misc("gtkconn", "%s: %s (%" G_GSIZE_FORMAT "/%" G_GSIZE_FORMAT ")\n",
	                  purple_account_get_username(purple_connection_get_account(gc)),
	                  text ? text : "", step, step_count);
}

static void
close_error_alert(PurpleAccount *account)
{
	/* Cancelling makes the alert close itself; the callback then removes
	 * the entry. */
	GCancellable *cancellable = g_hash_table_lookup(error_alerts, account);

	if (cancellable != NULL)
		g_cancellable_cancel(cancellable);
}

static void
pidgin_connection_connected(PurpleConnection *gc)
{
	PurpleAccount *account;

	account  = purple_connection_get_account(gc);

	g_hash_table_remove(auto_reconns, account);
	close_error_alert(account);
}

static void
pidgin_connection_disconnected(PurpleConnection *gc)
{
	if (purple_connections_get_all() != NULL)
		return;

	pidgin_dialogs_destroy_all();
}

static void
free_auto_recon(gpointer data)
{
	PidginAutoRecon *info = data;

	if (info->timeout != 0)
		g_source_remove(info->timeout);

	g_free(info);
}

static gboolean
do_signon(gpointer data)
{
	PurpleAccount *account = data;
	PidginAutoRecon *info;
	PurpleStatus *status;

	purple_debug_info("autorecon", "do_signon called\n");
	g_return_val_if_fail(account != NULL, FALSE);
	info = g_hash_table_lookup(auto_reconns, account);

	if (info)
		info->timeout = 0;

	status = purple_account_get_active_status(account);
	if (purple_status_is_online(status))
	{
		purple_debug_info("autorecon", "calling purple_account_connect\n");
		purple_account_connect(account);
		purple_debug_info("autorecon", "done calling purple_account_connect\n");
	}

	return FALSE;
}

typedef struct {
	PurpleAccount *account;
	GCancellable *cancellable;
} ErrorAlert;

static void
error_alert_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	ErrorAlert *alert = data;
	GError *error = NULL;
	int button;

	button = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, &error);

	/* Only forget the entry if it is still ours (not replaced). */
	if (error_alerts != NULL &&
	    g_hash_table_lookup(error_alerts, alert->account) == alert->cancellable)
		g_hash_table_remove(error_alerts, alert->account);

	if (error == NULL && button == 1 &&
	    g_list_find(purple_accounts_get_all(), alert->account) != NULL) {
		pidgin_account_dialog_show(PIDGIN_MODIFY_ACCOUNT_DIALOG, alert->account);
	} else if (error == NULL && button == 2 &&
	           g_list_find(purple_accounts_get_all(), alert->account) != NULL) {
		purple_account_set_enabled(alert->account, PIDGIN_UI, TRUE);
	}

	g_clear_error(&error);
	g_object_unref(alert->cancellable);
	g_free(alert);
}

static void
show_error_alert(PurpleAccount *account, const char *text, gboolean fatal)
{
	static const char *fatal_buttons[] = { N_("_Close"), N_("_Modify Account"),
	                                       N_("Re-_enable"), NULL };
	const char *buttons[4];
	GtkAlertDialog *dialog;
	ErrorAlert *alert;
	char *primary, *detail;
	int i;

	close_error_alert(account);

	primary = g_strdup_printf(_("%s disconnected"),
	                          purple_account_get_username(account));
	if (fatal)
		detail = g_strdup_printf(_("%s\n\n%s will not attempt to reconnect "
			"the account until you correct the error and re-enable the "
			"account."), text ? text : "", PIDGIN_NAME);
	else
		detail = g_strdup(text ? text : "");

	for (i = 0; fatal_buttons[i] != NULL; i++)
		buttons[i] = _(fatal_buttons[i]);
	buttons[i] = NULL;

	dialog = gtk_alert_dialog_new("%s", primary);
	gtk_alert_dialog_set_detail(dialog, detail);
	gtk_alert_dialog_set_buttons(dialog, buttons);
	gtk_alert_dialog_set_cancel_button(dialog, 0);
	gtk_alert_dialog_set_default_button(dialog, 1);
	gtk_alert_dialog_set_modal(dialog, FALSE);

	alert = g_new0(ErrorAlert, 1);
	alert->account = account;
	alert->cancellable = g_cancellable_new();
	g_hash_table_insert(error_alerts, account, g_object_ref(alert->cancellable));

	gtk_alert_dialog_choose(dialog, pidgin_get_active_window(),
	                        alert->cancellable, error_alert_cb, alert);
	g_object_unref(dialog);

	g_free(primary);
	g_free(detail);
}

static void
pidgin_connection_report_disconnect_reason (PurpleConnection *gc,
                                            PurpleConnectionError reason,
                                            const char *text)
{
	PurpleAccount *account = NULL;
	PidginAutoRecon *info;

	account = purple_connection_get_account(gc);
	info = g_hash_table_lookup(auto_reconns, account);

	if (!purple_connection_error_is_fatal (reason)) {
		if (info == NULL) {
			info = g_new0(PidginAutoRecon, 1);
			g_hash_table_insert(auto_reconns, account, info);
			info->delay = g_random_int_range(INITIAL_RECON_DELAY_MIN, INITIAL_RECON_DELAY_MAX);
		} else {
			info->delay = MIN(2 * info->delay, MAX_RECON_DELAY);
			if (info->timeout != 0)
				g_source_remove(info->timeout);
		}
		info->timeout = g_timeout_add(info->delay, do_signon, account);

		/* Pidgin 2 only showed these in the buddy list; the accounts
		 * window shows them (purple_account_get_current_error()). */
		purple_debug_info("gtkconn", "%s disconnected (%s); reconnecting in %d s\n",
		                  purple_account_get_username(account),
		                  text ? text : "", info->delay / 1000);
	} else {
		if (info != NULL)
			g_hash_table_remove(auto_reconns, account);

		purple_account_set_enabled(account, PIDGIN_UI, FALSE);

		show_error_alert(account, text, TRUE);
	}
}

static void pidgin_connection_network_connected (void)
{
	GList *list, *l;

	/* TODO(M3): pidgin_status_box_set_network_available(TRUE) */

	l = list = purple_accounts_get_all_active();
	while (l) {
		PurpleAccount *account = (PurpleAccount*)l->data;
		g_hash_table_remove(auto_reconns, account);
		if (purple_account_is_disconnected(account))
			do_signon(account);
		l = l->next;
	}
	g_list_free(list);
}

static void pidgin_connection_network_disconnected (void)
{
	GList *list, *l;

	/* TODO(M3): pidgin_status_box_set_network_available(FALSE) */

	l = list = purple_accounts_get_all_active();
	while (l) {
		PurpleAccount *a = (PurpleAccount*)l->data;
		if (!purple_account_is_disconnected(a)) {
			char *password = g_strdup(purple_account_get_password(a));
			purple_account_disconnect(a);
			purple_account_set_password(a, password);
			g_free(password);
		}
		l = l->next;
	}
	g_list_free(list);
}

static void pidgin_connection_notice(PurpleConnection *gc, const char *text)
{ }

static PurpleConnectionUiOps conn_ui_ops =
{
	pidgin_connection_connect_progress,
	pidgin_connection_connected,
	pidgin_connection_disconnected,
	pidgin_connection_notice,
	NULL, /* report_disconnect */
	pidgin_connection_network_connected,
	pidgin_connection_network_disconnected,
	pidgin_connection_report_disconnect_reason,
	NULL,
	NULL,
	NULL
};

PurpleConnectionUiOps *
pidgin_connections_get_ui_ops(void)
{
	return &conn_ui_ops;
}

static void
account_removed_cb(PurpleAccount *account, gpointer user_data)
{
	g_hash_table_remove(auto_reconns, account);
	close_error_alert(account);
}

static void
account_enabled_cb(PurpleAccount *account, gpointer user_data)
{
	/* Re-enabled from the accounts window: the old error no longer
	 * applies. */
	close_error_alert(account);
}


/**************************************************************************
* GTK+ connection glue
**************************************************************************/

void *
pidgin_connection_get_handle(void)
{
	static int handle;

	return &handle;
}

void
pidgin_connection_init(void)
{
	auto_reconns = g_hash_table_new_full(
							g_direct_hash, g_direct_equal,
							NULL, free_auto_recon);
	error_alerts = g_hash_table_new_full(g_direct_hash, g_direct_equal,
	                                     NULL, g_object_unref);

	purple_signal_connect(purple_accounts_get_handle(), "account-removed",
						pidgin_connection_get_handle(),
						PURPLE_CALLBACK(account_removed_cb), NULL);
	purple_signal_connect(purple_accounts_get_handle(), "account-enabled",
						pidgin_connection_get_handle(),
						PURPLE_CALLBACK(account_enabled_cb), NULL);
}

void
pidgin_connection_uninit(void)
{
	GHashTable *alerts = error_alerts;
	GHashTableIter iter;
	gpointer value;

	purple_signals_disconnect_by_handle(pidgin_connection_get_handle());

	g_hash_table_destroy(auto_reconns);
	auto_reconns = NULL;

	/* The callbacks run later, from the main loop; they see
	 * error_alerts == NULL and only free their own data. */
	error_alerts = NULL;
	g_hash_table_iter_init(&iter, alerts);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		g_cancellable_cancel(value);
	g_hash_table_destroy(alerts);
}
