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
 */

/*
 * Server features over the prpl's round-2 IPC (see pidginserverfeatures.h):
 * what the privacy window, the status box, the saved-status editor and the
 * buddy and conversation menus ask before offering blocking modes,
 * invisibility and spam reports.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "account.h"
#include "connection.h"
#include "debug.h"
#include "notify.h"
#include "plugin.h"
#include "privacy.h"
#include "prpl.h"
#include "signals.h"
#include "status.h"
#include "util.h"

#include "gtkutils.h"
#include "pidginserverfeatures.h"

/**************************************************************************
 * IPC
 **************************************************************************/

static GHashTable *command_cache = NULL;    /* "<plugin>:command" -> 1 yes, 2 no */
static int handle;

void
pidgin_server_features_reset_cache(void)
{
	if (command_cache != NULL)
		g_hash_table_remove_all(command_cache);
}

static void
plugin_changed_cb(PurplePlugin *plugin, gpointer data)
{
	pidgin_server_features_reset_cache();
}

/* purple_plugin_ipc_get_params() logs an error for each miss, so each
 * prpl is asked once per command (again after a plugin (un)loads). */
static gboolean
has_command(PurplePlugin *prpl, const char *command)
{
	char *key;
	int v;

	if (command_cache == NULL) {
		command_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
		purple_signal_connect(purple_plugins_get_handle(), "plugin-load", &handle,
		                      PURPLE_CALLBACK(plugin_changed_cb), NULL);
		purple_signal_connect(purple_plugins_get_handle(), "plugin-unload", &handle,
		                      PURPLE_CALLBACK(plugin_changed_cb), NULL);
	}
	key = g_strdup_printf("%p:%s", (void *)prpl, command);
	v = GPOINTER_TO_INT(g_hash_table_lookup(command_cache, key));
	if (v == 0) {
		v = purple_plugin_ipc_get_params(prpl, command, NULL, NULL, NULL) ? 1 : 2;
		g_hash_table_insert(command_cache, key, GINT_TO_POINTER(v));
	} else {
		g_free(key);
	}
	return v == 1;
}

/* The connected account's prpl, if it has @command. */
static PurplePlugin *
connected_prpl_with(PurpleAccount *account, const char *command)
{
	PurpleConnection *gc;

	if (account == NULL || !purple_account_is_connected(account))
		return NULL;
	gc = purple_account_get_connection(account);
	if (gc == NULL || gc->prpl == NULL || !has_command(gc->prpl, command))
		return NULL;
	return gc->prpl;
}

/**************************************************************************
 * Privacy modes
 **************************************************************************/

static const struct {
	const char *name;
	PurplePrivacyType type;
} mode_names[] = {
	{ "allow-all", PURPLE_PRIVACY_ALLOW_ALL },
	{ "deny-all", PURPLE_PRIVACY_DENY_ALL },
	{ "allow-users", PURPLE_PRIVACY_ALLOW_USERS },
	{ "deny-users", PURPLE_PRIVACY_DENY_USERS },
	{ "allow-buddylist", PURPLE_PRIVACY_ALLOW_BUDDYLIST },
};

#define ALL_MODES ((1u << PURPLE_PRIVACY_ALLOW_ALL) | (1u << PURPLE_PRIVACY_DENY_ALL) | \
                   (1u << PURPLE_PRIVACY_ALLOW_USERS) | (1u << PURPLE_PRIVACY_DENY_USERS) | \
                   (1u << PURPLE_PRIVACY_ALLOW_BUDDYLIST))

guint
pidgin_account_privacy_modes(PurpleAccount *account)
{
	PurplePlugin *prpl = connected_prpl_with(account, "privacy-modes");
	gboolean ok = FALSE;
	GList *modes, *l;
	guint mask = 0;
	gsize i;

	if (prpl == NULL)
		return ALL_MODES;
	modes = purple_plugin_ipc_call(prpl, "privacy-modes", &ok, account);
	if (!ok || modes == NULL)
		return ALL_MODES;
	for (l = modes; l != NULL; l = l->next)
		for (i = 0; i < G_N_ELEMENTS(mode_names); i++)
			if (purple_strequal(l->data, mode_names[i].name))
				mask |= 1u << mode_names[i].type;
	g_list_free_full(modes, g_free);
	return mask != 0 ? mask : ALL_MODES;
}

gboolean
pidgin_privacy_modes_restricted(guint mask)
{
	return (mask & ALL_MODES) != ALL_MODES;
}

/**************************************************************************
 * Invisible
 **************************************************************************/

int
pidgin_account_invisible_supported(PurpleAccount *account)
{
	PurplePlugin *prpl = connected_prpl_with(account, "status-invisible-supported");
	gboolean ok = FALSE;
	gpointer ret;

	if (prpl == NULL)
		return -1;
	ret = purple_plugin_ipc_call(prpl, "status-invisible-supported", &ok, account);
	if (!ok)
		return -1;
	return GPOINTER_TO_INT(ret) ? 1 : 0;
}

/* Connected, has an invisible status, and its server can't do it. */
static gboolean
invisible_falls_back(PurpleAccount *account)
{
	return purple_account_get_status_type_with_primitive(account,
	           PURPLE_STATUS_INVISIBLE) != NULL &&
	       pidgin_account_invisible_supported(account) == 0;
}

char *
pidgin_invisible_unsupported_note(PurpleAccount *account)
{
	GString *names;
	GList *l;
	guint n = 0;
	char *ret;

	if (account != NULL)
		return invisible_falls_back(account)
			? g_strdup(_("The server doesn't support invisibility; you will "
			             "appear available."))
			: NULL;

	names = g_string_new(NULL);
	for (l = purple_accounts_get_all_active(); l != NULL; l = g_list_delete_link(l, l)) {
		if (!invisible_falls_back(l->data))
			continue;
		if (n++ > 0)
			g_string_append(names, ", ");
		g_string_append(names, purple_account_get_username(l->data));
	}
	if (n == 0) {
		g_string_free(names, TRUE);
		return NULL;
	}
	ret = g_strdup_printf(ngettext(
		"The server of %s doesn't support invisibility; you will appear available there.",
		"The servers of %s don't support invisibility; you will appear available there.",
		n), names->str);
	g_string_free(names, TRUE);
	return ret;
}

/**************************************************************************
 * Spam reports
 **************************************************************************/

gboolean
pidgin_account_is_blocked(PurpleAccount *account, const char *name)
{
	char *norm;
	GSList *l;
	gboolean ret = FALSE;

	if (account == NULL || name == NULL)
		return FALSE;
	if (!purple_privacy_check(account, name))
		return TRUE;
	/* XEP-0191 mirrors the server's list into deny whatever the mode */
	norm = g_strdup(purple_normalize(account, name));
	for (l = account->deny; l != NULL && !ret; l = l->next)
		ret = purple_strequal(norm, purple_normalize(account, l->data));
	g_free(norm);
	return ret;
}

gboolean
pidgin_account_report_spam_supported(PurpleAccount *account)
{
	PurplePlugin *prpl = connected_prpl_with(account, "report-spam-supported");
	gboolean ok = FALSE;
	gpointer ret;

	if (prpl == NULL || connected_prpl_with(account, "report-spam") == NULL)
		return FALSE;
	ret = purple_plugin_ipc_call(prpl, "report-spam-supported", &ok, account);
	return ok && GPOINTER_TO_INT(ret);
}

gboolean
pidgin_account_report_spam(PurpleAccount *account, const char *jid, const char *reason,
                           gboolean abuse)
{
	PurplePlugin *prpl = connected_prpl_with(account, "report-spam");
	gboolean ok = FALSE;
	gpointer ret;

	if (prpl == NULL || jid == NULL || *jid == '\0')
		return FALSE;
	if (reason != NULL && *reason == '\0')
		reason = NULL;
	ret = purple_plugin_ipc_call(prpl, "report-spam", &ok, account, jid, reason,
	                             (guint)(abuse ? 1 : 0));
	return ok && GPOINTER_TO_INT(ret);
}

typedef struct {
	GtkWidget *window;
	GtkWidget *reason;
	GtkWidget *abuse;
	PurpleAccount *account;
	char *jid;
} ReportDialog;

static void
report_dialog_free(gpointer data)
{
	ReportDialog *d = data;

	g_free(d->jid);
	g_free(d);
}

static void
report_cancel_cb(GtkButton *button, ReportDialog *d)
{
	gtk_window_destroy(GTK_WINDOW(d->window));
}

static void
report_send_cb(GtkButton *button, ReportDialog *d)
{
	const char *reason = gtk_editable_get_text(GTK_EDITABLE(d->reason));
	char *text = g_strstrip(g_strdup(reason ? reason : ""));
	gboolean abuse = gtk_check_button_get_active(GTK_CHECK_BUTTON(d->abuse));

	if (!g_list_find(purple_accounts_get_all(), d->account) ||
	    !pidgin_account_report_spam(d->account, d->jid, text, abuse)) {
		char *secondary = g_strdup_printf(_("%s was not reported or blocked: the "
		                                    "server doesn't support blocking, or the "
		                                    "account is offline."), d->jid);

		purple_notify_error(NULL, _("Report Spam"), _("The report could not be sent."),
		                    secondary);
		g_free(secondary);
	} else {
		purple_debug_info("pidgin4", "reported %s as %s\n", d->jid,
		                  abuse ? "abuse" : "spam");
	}
	g_free(text);
	gtk_window_destroy(GTK_WINDOW(d->window));
}

GtkWidget *
pidgin_report_spam_dialog_show(PurpleAccount *account, const char *jid, GtkWindow *parent)
{
	ReportDialog *d;
	GtkWidget *vbox, *label, *button;
	char *bare, *primary;
	const char *slash;

	g_return_val_if_fail(account != NULL && jid != NULL, NULL);

	slash = strchr(jid, '/');
	bare = slash ? g_strndup(jid, slash - jid) : g_strdup(jid);

	d = g_new0(ReportDialog, 1);
	d->account = account;
	d->jid = bare;
	d->window = pidgin_dialog_new(_("Report Spam"), parent, "report-spam", FALSE);
	g_object_set_data_full(G_OBJECT(d->window), "pidgin-report-dialog", d, report_dialog_free);

	primary = g_strdup_printf(_("Report %s and block them?"), bare);
	pidgin_dialog_add_message(d->window, "dialog-warning-symbolic", primary,
		_("Your server is told that this contact sent spam, and blocks them. "
		  "Unblock them later from the Privacy window or their menu."), FALSE);
	g_free(primary);

	vbox = pidgin_dialog_get_content_area(d->window);
	label = gtk_label_new_with_mnemonic(_("_Reason (optional):"));
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_box_append(GTK_BOX(vbox), label);
	d->reason = gtk_entry_new();
	gtk_widget_set_name(d->reason, "pidgin-report-reason");
	gtk_entry_set_activates_default(GTK_ENTRY(d->reason), TRUE);
	gtk_label_set_mnemonic_widget(GTK_LABEL(label), d->reason);
	gtk_box_append(GTK_BOX(vbox), d->reason);

	d->abuse = gtk_check_button_new_with_mnemonic(
		_("This is _abuse (harassment, threats), not just spam"));
	gtk_widget_set_name(d->abuse, "pidgin-report-abuse");
	gtk_box_append(GTK_BOX(vbox), d->abuse);

	button = pidgin_dialog_add_button(d->window, _("_Cancel"), G_CALLBACK(report_cancel_cb), d);
	gtk_widget_set_name(button, "pidgin-report-cancel");
	button = pidgin_dialog_add_button(d->window, _("_Report and Block"),
	                                  G_CALLBACK(report_send_cb), d);
	gtk_widget_set_name(button, "pidgin-report-send");
	gtk_widget_add_css_class(button, "destructive-action");
	gtk_window_set_default_widget(GTK_WINDOW(d->window), button);

	pidgin_window_set_secondary(GTK_WINDOW(d->window));
	gtk_window_present(GTK_WINDOW(d->window));
	return d->window;
}
