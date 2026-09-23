/*
 * pidgin4: the server features round 2 selftest (PIDGIN4_R2_SELFTEST=1).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The UI half of doc/PIDGIN-UPGRADE.md, M8 "Landed (server features round
 * 2)", on the in-process selftest protocol (tests/selftest-prpl.c): this
 * registers the four round-2 IPC commands (privacy-modes,
 * status-invisible-supported, report-spam-supported, report-spam) onto
 * that plugin, with results the test toggles, gives the protocol blocking
 * and an "invisible" status type for the time of the test, and then
 * checks the privacy window, the Report Spam menu items and dialog, the
 * invisible notes, file-sharing metadata (XEP-0447) turned into
 * attachments with thumbnails, captions and hash checks against a local
 * SoupServer, the explicit-encryption (XEP-0380) line, and the idle
 * display. It removes its account and quits with status 0 on success.
 * Run it on a scratch copy of a profile (pidgin4/TESTING.md).
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include <libsoup/soup.h>

#include "account.h"
#include "blist.h"
#include "connection.h"
#include "conversation.h"
#include "debug.h"
#include "plugin.h"
#include "prefs.h"
#include "privacy.h"
#include "prpl.h"
#include "server.h"
#include "signals.h"
#include "status.h"
#include "util.h"
#include "value.h"

#include "gtkblist.h"
#include "gtkconv.h"
#include "gtkconvwin.h"
#include "gtkprivacy.h"
#include "gtksavedstatuses.h"
#include "gtkstatusbox.h"
#include "gtkutils.h"
#include "pidginattachment.h"
#include "pidginconvmeta.h"
#include "pidginimageloader.h"
#include "pidginmessage.h"
#include "pidginmessageview.h"
#include "pidginselftest.h"
#include "pidginserverfeatures.h"

#include "tests/selftest-prpl.h"

#define R2 "PIDGIN4_R2_SELFTEST"
#define R2_USER "r2@example.invalid"
#define SPAMMER "spammer@example.invalid"
#define SPAMMER2 "spammer2@example.invalid"

static int failures = 0;
static int checks = 0;
static PurpleAccount *r2_account = NULL;
static PurplePlugin *r2_plugin = NULL;

#define CHECK(cond, ...) G_STMT_START { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		g_printerr(R2 ": FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
		g_printerr(__VA_ARGS__); \
		g_printerr("\n"); \
	} \
} G_STMT_END

static void
spin(guint ms)
{
	pidgin_selftest_spin(ms);
}

/**************************************************************************
 * The round-2 IPC commands on the selftest protocol
 **************************************************************************/

static gboolean ipc_deny_users = TRUE;      /* privacy-modes lists deny-users */
static gboolean ipc_invisible = FALSE;      /* status-invisible-supported */
static gboolean ipc_report = FALSE;         /* report-spam-supported */
static char *ipc_last_report = NULL;        /* "jid|reason|abuse" */
static int ipc_reports = 0;

static GList *
ipc_privacy_modes_cb(PurpleAccount *account)
{
	GList *l = NULL;

	if (account != r2_account)
		return NULL;
	l = g_list_append(l, g_strdup("allow-all"));
	if (ipc_deny_users)
		l = g_list_append(l, g_strdup("deny-users"));
	return l;
}

static gboolean
ipc_invisible_cb(PurpleAccount *account)
{
	return account == r2_account && ipc_invisible;
}

static gboolean
ipc_report_supported_cb(PurpleAccount *account)
{
	return account == r2_account && ipc_report;
}

static gboolean
ipc_report_spam_cb(PurpleAccount *account, const char *jid, const char *reason, guint abuse)
{
	if (account != r2_account || jid == NULL)
		return FALSE;
	g_free(ipc_last_report);
	ipc_last_report = g_strdup_printf("%s|%s|%u", jid, reason ? reason : "(null)", abuse);
	ipc_reports++;
	/* as the server's block push does */
	purple_privacy_deny_add(account, jid, TRUE);
	return TRUE;
}

static void
register_ipc(PurplePlugin *plugin)
{
	purple_plugin_ipc_register(plugin, "privacy-modes",
	                           PURPLE_CALLBACK(ipc_privacy_modes_cb),
	                           purple_marshal_POINTER__POINTER,
	                           purple_value_new(PURPLE_TYPE_POINTER), 1,
	                           purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT));
	purple_plugin_ipc_register(plugin, "status-invisible-supported",
	                           PURPLE_CALLBACK(ipc_invisible_cb),
	                           purple_marshal_BOOLEAN__POINTER,
	                           purple_value_new(PURPLE_TYPE_BOOLEAN), 1,
	                           purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT));
	purple_plugin_ipc_register(plugin, "report-spam-supported",
	                           PURPLE_CALLBACK(ipc_report_supported_cb),
	                           purple_marshal_BOOLEAN__POINTER,
	                           purple_value_new(PURPLE_TYPE_BOOLEAN), 1,
	                           purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT));
	purple_plugin_ipc_register(plugin, "report-spam",
	                           PURPLE_CALLBACK(ipc_report_spam_cb),
	                           purple_marshal_BOOLEAN__POINTER_POINTER_POINTER_UINT,
	                           purple_value_new(PURPLE_TYPE_BOOLEAN), 4,
	                           purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
	                           purple_value_new(PURPLE_TYPE_STRING),
	                           purple_value_new(PURPLE_TYPE_STRING),
	                           purple_value_new(PURPLE_TYPE_UINT));
	pidgin_server_features_reset_cache();
}

/* Blocking and an invisible status, for the time of the test (the
 * protocol's info struct is static; restored at the end). */
static PurplePluginProtocolInfo saved_info;

static void
st_add_deny(PurpleConnection *gc, const char *name)
{
}

static void
st_rem_deny(PurpleConnection *gc, const char *name)
{
}

static void
st_set_permit_deny(PurpleConnection *gc)
{
}

static GList *
st_status_types(PurpleAccount *account)
{
	GList *types = NULL;

	types = g_list_append(types, purple_status_type_new(PURPLE_STATUS_AVAILABLE,
		"available", NULL, TRUE));
	types = g_list_append(types, purple_status_type_new(PURPLE_STATUS_INVISIBLE,
		"invisible", NULL, TRUE));
	types = g_list_append(types, purple_status_type_new(PURPLE_STATUS_OFFLINE,
		"offline", NULL, TRUE));
	return types;
}

static void
patch_protocol(PurplePlugin *plugin)
{
	PurplePluginProtocolInfo *info = PURPLE_PLUGIN_PROTOCOL_INFO(plugin);

	saved_info = *info;
	info->add_deny = st_add_deny;
	info->rem_deny = st_rem_deny;
	info->set_permit_deny = st_set_permit_deny;
	info->status_types = st_status_types;
}

static void
unpatch_protocol(PurplePlugin *plugin)
{
	PurplePluginProtocolInfo *info = PURPLE_PLUGIN_PROTOCOL_INFO(plugin);

	info->add_deny = saved_info.add_deny;
	info->rem_deny = saved_info.rem_deny;
	info->set_permit_deny = saved_info.set_permit_deny;
	info->status_types = saved_info.status_types;
}

/**************************************************************************
 * Widget helpers
 **************************************************************************/

static GtkWidget *
find_named(GtkWidget *widget, const char *name)
{
	GtkWidget *child, *found;

	if (widget == NULL)
		return NULL;
	if (purple_strequal(gtk_widget_get_name(widget), name))
		return widget;
	for (child = gtk_widget_get_first_child(widget); child != NULL;
	     child = gtk_widget_get_next_sibling(child))
		if ((found = find_named(child, name)) != NULL)
			return found;
	return NULL;
}

/* The first GtkLabel below @widget whose text is @text. */
static GtkWidget *
find_label(GtkWidget *widget, const char *text)
{
	GtkWidget *child, *found;

	if (widget == NULL)
		return NULL;
	if (GTK_IS_LABEL(widget) && purple_strequal(gtk_label_get_text(GTK_LABEL(widget)), text))
		return widget;
	for (child = gtk_widget_get_first_child(widget); child != NULL;
	     child = gtk_widget_get_next_sibling(child))
		if ((found = find_label(child, text)) != NULL)
			return found;
	return NULL;
}

/* The first descendant with @css_class (and @tooltip, if given). */
static GtkWidget *
find_class(GtkWidget *widget, const char *css_class, const char *tooltip)
{
	GtkWidget *child, *found;

	if (widget == NULL)
		return NULL;
	if (gtk_widget_has_css_class(widget, css_class) &&
	    (tooltip == NULL || purple_strequal(gtk_widget_get_tooltip_text(widget), tooltip)))
		return widget;
	for (child = gtk_widget_get_first_child(widget); child != NULL;
	     child = gtk_widget_get_next_sibling(child))
		if ((found = find_class(child, css_class, tooltip)) != NULL)
			return found;
	return NULL;
}

/* The toplevel window named @name (pidgin_dialog_new()'s role). */
static GtkWidget *
find_window(const char *name)
{
	GListModel *list = gtk_window_get_toplevels();
	GtkWidget *ret = NULL;
	guint i;

	for (i = 0; ret == NULL && i < g_list_model_get_n_items(list); i++) {
		GtkWidget *w = g_list_model_get_item(list, i);

		if (purple_strequal(gtk_widget_get_name(w), name) && gtk_widget_get_visible(w))
			ret = w;
		g_object_unref(w);
	}
	return ret;
}

/* The action of the menu item labelled @label, anywhere in @model. */
static char *
menu_find_action(GMenuModel *model, const char *label)
{
	int i, n = g_menu_model_get_n_items(model);

	for (i = 0; i < n; i++) {
		char *l = NULL, *action = NULL;
		GMenuLinkIter *links;
		const char *name;
		GMenuModel *sub;

		g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_LABEL, "s", &l);
		if (purple_strequal(l, label) &&
		    g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_ACTION, "s", &action)) {
			g_free(l);
			return action;
		}
		g_free(l);
		links = g_menu_model_iterate_item_links(model, i);
		while (g_menu_link_iter_get_next(links, &name, &sub)) {
			action = menu_find_action(sub, label);
			g_object_unref(sub);
			if (action != NULL) {
				g_object_unref(links);
				return action;
			}
		}
		g_object_unref(links);
	}
	return NULL;
}

/* Fills the report dialog and presses Report. */
static gboolean
answer_report_dialog(const char *reason, gboolean abuse)
{
	GtkWidget *win = find_window("report-spam");

	if (win == NULL)
		return FALSE;
	gtk_editable_set_text(GTK_EDITABLE(find_named(win, "pidgin-report-reason")),
	                      reason ? reason : "");
	gtk_check_button_set_active(GTK_CHECK_BUTTON(find_named(win, "pidgin-report-abuse")),
	                            abuse);
	g_signal_emit_by_name(find_named(win, "pidgin-report-send"), "clicked");
	spin(100);
	return find_window("report-spam") == NULL;
}

/**************************************************************************
 * 1. Blocking: privacy modes, Report Spam
 **************************************************************************/

#define REPORT_LABEL "Report _Spam and Block..."

static void
test_privacy_window(void)
{
	GtkWidget *types, *note, *popover;
	guint mask, i, n_dim = 0, n_plain = 0;
	static const char *const labels[] = {
		"Allow all users to contact me", "Allow only the users on my buddy list",
		"Allow only the users below", "Block all users", "Block only the users below"
	};
	static const PurplePrivacyType types_of[] = {
		PURPLE_PRIVACY_ALLOW_ALL, PURPLE_PRIVACY_ALLOW_BUDDYLIST,
		PURPLE_PRIVACY_ALLOW_USERS, PURPLE_PRIVACY_DENY_ALL, PURPLE_PRIVACY_DENY_USERS
	};

	/* The mask itself */
	ipc_deny_users = FALSE;
	mask = pidgin_account_privacy_modes(r2_account);
	CHECK(mask == (1u << PURPLE_PRIVACY_ALLOW_ALL), "mask without deny-users: %x", mask);
	ipc_deny_users = TRUE;
	mask = pidgin_account_privacy_modes(r2_account);
	CHECK(mask == ((1u << PURPLE_PRIVACY_ALLOW_ALL) | (1u << PURPLE_PRIVACY_DENY_USERS)),
	      "mask: %x", mask);
	CHECK(pidgin_privacy_modes_restricted(mask), "not restricted");
	CHECK(!pidgin_privacy_modes_restricted(pidgin_account_privacy_modes(NULL)),
	      "no account: restricted");

	pidgin_privacy_dialog_show();
	spin(200);
	pidgin_account_dropdown_set_selected(pidgin_privacy_dialog_get_widget_for_tests("account-menu"),
	                                     r2_account);
	spin(100);
	types = pidgin_privacy_dialog_get_widget_for_tests("type-menu");
	note = pidgin_privacy_dialog_get_widget_for_tests("modes-note");
	CHECK(types != NULL && note != NULL, "no privacy dialog");
	if (types == NULL || note == NULL)
		return;
	CHECK(gtk_widget_get_visible(note), "no note about unsupported modes");

	/* The popup: the three unsupported modes greyed, with the tooltip */
	g_signal_emit_by_name(types, "activate");
	spin(300);
	popover = find_class(types, "menu", NULL);
	if (popover == NULL)
		popover = types;
	for (i = 0; i < G_N_ELEMENTS(labels); i++) {
		GtkWidget *label = find_label(popover, labels[i]);
		gboolean ok = PIDGIN_PRIVACY_MODE_SUPPORTED(mask, types_of[i]);

		CHECK(label != NULL, "no row for %s", labels[i]);
		if (label == NULL)
			continue;
		CHECK(gtk_widget_has_css_class(label, "dim-label") == !ok, "%s: dim %d", labels[i],
		      gtk_widget_has_css_class(label, "dim-label"));
		CHECK(purple_strequal(gtk_widget_get_tooltip_text(label),
		                      ok ? NULL : "Not supported by this server"),
		      "%s: tooltip %s", labels[i], gtk_widget_get_tooltip_text(label));
		if (ok)
			n_plain++;
		else
			n_dim++;
	}
	CHECK(n_plain == 2 && n_dim == 3, "%u offered, %u greyed", n_plain, n_dim);
	g_signal_emit_by_name(types, "activate");   /* toggles it closed, if open */
	spin(100);
	{
		GtkWidget *p = find_class(types, "menu", NULL);

		if (p != NULL && GTK_IS_POPOVER(p))
			gtk_popover_popdown(GTK_POPOVER(p));
	}

	/* An unsupported mode chosen anyway (keyboard): not applied */
	r2_account->perm_deny = PURPLE_PRIVACY_ALLOW_ALL;
	gtk_drop_down_set_selected(GTK_DROP_DOWN(types), 3);   /* Block all users */
	spin(50);
	CHECK(r2_account->perm_deny == PURPLE_PRIVACY_ALLOW_ALL, "unsupported mode applied: %d",
	      r2_account->perm_deny);
	CHECK(gtk_drop_down_get_selected(GTK_DROP_DOWN(types)) == 0, "not reverted: %u",
	      gtk_drop_down_get_selected(GTK_DROP_DOWN(types)));
	/* a supported one is */
	gtk_drop_down_set_selected(GTK_DROP_DOWN(types), 4);   /* Block only the users below */
	spin(50);
	CHECK(r2_account->perm_deny == PURPLE_PRIVACY_DENY_USERS, "deny-users not applied: %d",
	      r2_account->perm_deny);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(types), 0);
	spin(50);
	CHECK(r2_account->perm_deny == PURPLE_PRIVACY_ALLOW_ALL, "allow-all not applied");

	pidgin_privacy_dialog_hide();
	spin(100);
}

static gboolean
buddy_menu_has_report(PurpleBuddy *buddy, GSimpleActionGroup **group_out, char **action_out)
{
	GMenu *menu = g_menu_new();
	GSimpleActionGroup *group = g_simple_action_group_new();
	char *action;

	pidgin_blist_build_node_menu((PurpleBlistNode *)buddy, menu, group);
	action = menu_find_action(G_MENU_MODEL(menu), REPORT_LABEL);
	g_object_unref(menu);
	if (group_out != NULL && action != NULL) {
		*group_out = group;
		*action_out = action;
	} else {
		g_object_unref(group);
		g_free(action);
	}
	return action != NULL;
}

static void
test_report_spam(void)
{
	PurpleGroup *group = purple_group_new("pidgin4 r2 selftest");
	PurpleBuddy *buddy;
	PurpleConversation *conv;
	PidginWindow *win;
	GSimpleActionGroup *actions = NULL;
	char *action = NULL;

	purple_blist_add_group(group, NULL);
	buddy = purple_buddy_new(r2_account, SPAMMER, "Spammer");
	purple_blist_add_buddy(buddy, NULL, group, NULL);

	/* not supported: no item */
	ipc_report = FALSE;
	CHECK(!pidgin_account_report_spam_supported(r2_account), "supported while off");
	CHECK(!buddy_menu_has_report(buddy, NULL, NULL), "Report item without support");
	/* supported */
	ipc_report = TRUE;
	CHECK(pidgin_account_report_spam_supported(r2_account), "not supported while on");
	CHECK(buddy_menu_has_report(buddy, &actions, &action), "no Report item");
	if (action != NULL) {
		const char *name = strchr(action, '.') ? strchr(action, '.') + 1 : action;

		g_clear_pointer(&ipc_last_report, g_free);
		g_action_group_activate_action(G_ACTION_GROUP(actions), name, NULL);
		spin(200);
		CHECK(find_window("report-spam") != NULL, "no report dialog");
		CHECK(answer_report_dialog("  unsolicited ads ", TRUE), "the dialog stayed open");
		CHECK(purple_strequal(ipc_last_report, SPAMMER "|unsolicited ads|1"),
		      "report-spam called with %s", ipc_last_report);
		g_object_unref(actions);
		g_free(action);
	}
	/* blocked now (the fake push): no Report item, Unblock instead */
	CHECK(pidgin_account_is_blocked(r2_account, SPAMMER), "not blocked after the report");
	CHECK(!buddy_menu_has_report(buddy, NULL, NULL), "Report item for a blocked buddy");
	purple_privacy_deny_remove(r2_account, SPAMMER, TRUE);

	/* Cancel sends nothing */
	ipc_reports = 0;
	pidgin_report_spam_dialog_show(r2_account, SPAMMER "/res", NULL);
	spin(100);
	{
		GtkWidget *w = find_window("report-spam");

		CHECK(w != NULL, "no dialog");
		if (w != NULL)
			g_signal_emit_by_name(find_named(w, "pidgin-report-cancel"), "clicked");
		spin(100);
	}
	CHECK(ipc_reports == 0 && find_window("report-spam") == NULL, "cancel reported");

	/* The conversation window's Conversation menu, for an IM */
	conv = purple_conversation_new(PURPLE_CONV_TYPE_IM, r2_account, SPAMMER2);
	spin(200);
	win = conv && PIDGIN_CONVERSATION(conv) ? PIDGIN_CONVERSATION(conv)->win : NULL;
	CHECK(win != NULL, "no conversation window");
	if (win != NULL) {
		pidgin_conv_window_switch_gtkconv(win, PIDGIN_CONVERSATION(conv));
		pidgin_conv_window_update_menu(win);
		CHECK(g_action_group_get_action_enabled(G_ACTION_GROUP(win->actions), "report-spam"),
		      "conv.report-spam disabled");
		CHECK(menu_find_action(G_MENU_MODEL(win->menu.model), REPORT_LABEL) != NULL,
		      "no Report item in the Conversation menu");
		g_clear_pointer(&ipc_last_report, g_free);
		gtk_widget_activate_action(win->window, "conv.report-spam", NULL);
		spin(200);
		CHECK(answer_report_dialog(NULL, FALSE), "no dialog from the conversation");
		CHECK(purple_strequal(ipc_last_report, SPAMMER2 "|(null)|0"),
		      "report-spam called with %s", ipc_last_report);
		pidgin_conv_window_update_menu(win);
		CHECK(!g_action_group_get_action_enabled(G_ACTION_GROUP(win->actions), "report-spam"),
		      "conv.report-spam enabled for a blocked contact");
		purple_privacy_deny_remove(r2_account, SPAMMER2, TRUE);
		ipc_report = FALSE;
		pidgin_conv_window_update_menu(win);
		CHECK(!g_action_group_get_action_enabled(G_ACTION_GROUP(win->actions), "report-spam"),
		      "conv.report-spam enabled without support");
	}
	if (conv != NULL)
		purple_conversation_destroy(conv);
	spin(100);

	purple_blist_remove_buddy(buddy);
	purple_blist_remove_group(group);
}

/**************************************************************************
 * 2. Invisible
 **************************************************************************/

static GtkWidget *
find_type(GtkWidget *widget, GType type)
{
	GtkWidget *child, *found;

	if (widget == NULL)
		return NULL;
	if (G_TYPE_CHECK_INSTANCE_TYPE(widget, type))
		return widget;
	for (child = gtk_widget_get_first_child(widget); child != NULL;
	     child = gtk_widget_get_next_sibling(child))
		if ((found = find_type(child, type)) != NULL)
			return found;
	return NULL;
}

/* The status box's Invisible row. */
static GtkWidget *
invisible_row(void)
{
	GtkWidget *list = pidgin_status_box_get_list_for_tests();
	GtkListBoxRow *row;
	int i;

	for (i = 0; list && (row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), i)); i++)
		if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "row-data")) ==
		        PURPLE_STATUS_INVISIBLE &&
		    GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "row-type")) == 0)
			return GTK_WIDGET(row);
	return NULL;
}

static void
test_invisible(void)
{
	GtkWidget *row, *win, *note, *dropdown;
	char *text;

	/* the helpers */
	ipc_invisible = FALSE;
	CHECK(pidgin_account_invisible_supported(r2_account) == 0, "supported: %d",
	      pidgin_account_invisible_supported(r2_account));
	text = pidgin_invisible_unsupported_note(NULL);
	CHECK(text != NULL && strstr(text, R2_USER) != NULL &&
	      strstr(text, "you will appear available") != NULL, "note: %s", text);
	g_free(text);
	text = pidgin_invisible_unsupported_note(r2_account);
	CHECK(purple_strequal(text, "The server doesn't support invisibility; you will "
	                            "appear available."), "account note: %s", text);
	g_free(text);
	ipc_invisible = TRUE;
	CHECK(pidgin_account_invisible_supported(r2_account) == 1, "not supported");
	text = pidgin_invisible_unsupported_note(NULL);
	CHECK(text == NULL || strstr(text, R2_USER) == NULL, "note while supported: %s", text);
	g_free(text);

	/* the status box: a tooltip and an icon on Invisible, still usable */
	ipc_invisible = FALSE;
	row = invisible_row();
	CHECK(row != NULL, "no Invisible row in the status box");
	if (row != NULL) {
		const char *tip = gtk_widget_get_tooltip_text(row);

		CHECK(tip != NULL && strstr(tip, "doesn't support invisibility") != NULL &&
		      strstr(tip, R2_USER) != NULL, "row tooltip: %s", tip);
		CHECK(find_class(row, "pidgin-invisible-note", NULL) != NULL, "no note icon");
		CHECK(gtk_list_box_row_get_activatable(GTK_LIST_BOX_ROW(row)) &&
		      gtk_widget_get_sensitive(row), "Invisible not selectable");
	}
	ipc_invisible = TRUE;
	row = invisible_row();
	CHECK(row != NULL && gtk_widget_get_tooltip_text(row) == NULL &&
	      find_class(row, "pidgin-invisible-note", NULL) == NULL,
	      "a note while supported: %s", row ? gtk_widget_get_tooltip_text(row) : "-");
	ipc_invisible = FALSE;

	/* the saved-status editor: a note under Status when Invisible */
	pidgin_status_editor_show(FALSE, NULL);
	spin(200);
	win = find_window("status");
	note = find_named(win, "pidgin-invisible-note");
	dropdown = find_type(win, GTK_TYPE_DROP_DOWN);
	CHECK(win != NULL && note != NULL && dropdown != NULL, "no status editor");
	if (note != NULL && dropdown != NULL) {
		CHECK(!gtk_widget_get_visible(note), "a note for Away");
		pidgin_item_dropdown_select_data(dropdown, GINT_TO_POINTER(PURPLE_STATUS_INVISIBLE));
		spin(50);
		CHECK(gtk_widget_get_visible(note) &&
		      strstr(gtk_label_get_text(GTK_LABEL(note)), R2_USER) != NULL,
		      "no note for Invisible: %s", gtk_label_get_text(GTK_LABEL(note)));
		ipc_invisible = TRUE;
		pidgin_item_dropdown_select_data(dropdown, GINT_TO_POINTER(PURPLE_STATUS_AVAILABLE));
		pidgin_item_dropdown_select_data(dropdown, GINT_TO_POINTER(PURPLE_STATUS_INVISIBLE));
		spin(50);
		CHECK(!gtk_widget_get_visible(note), "a note while supported");
		ipc_invisible = FALSE;
	}

	/* its per-account editor */
	if (win != NULL) {
		GtkWidget *view;
		GListModel *model;
		GList *l;
		guint pos = 0;

		if (find_type(win, GTK_TYPE_EXPANDER) != NULL)
			gtk_expander_set_expanded(GTK_EXPANDER(find_type(win, GTK_TYPE_EXPANDER)), TRUE);
		spin(100);
		view = find_type(win, GTK_TYPE_COLUMN_VIEW);
		model = view ? G_LIST_MODEL(gtk_column_view_get_model(GTK_COLUMN_VIEW(view))) : NULL;

		for (l = purple_accounts_get_all(); l != NULL && l->data != r2_account; l = l->next)
			pos++;
		CHECK(model != NULL && pos < g_list_model_get_n_items(model), "no account row");
		if (model != NULL && pos < g_list_model_get_n_items(model)) {
			GtkWidget *sub, *subnote, *box;

			g_signal_emit_by_name(view, "activate", pos);
			spin(200);
			sub = find_window("substatus");
			subnote = find_named(sub, "pidgin-invisible-note");
			box = find_type(sub, GTK_TYPE_DROP_DOWN);
			CHECK(sub != NULL && subnote != NULL && box != NULL, "no substatus editor");
			if (subnote != NULL && box != NULL) {
				pidgin_item_dropdown_select_id(box, "available");
				spin(50);
				CHECK(!gtk_widget_get_visible(subnote), "a note for Available");
				pidgin_item_dropdown_select_id(box, "invisible");
				spin(50);
				CHECK(gtk_widget_get_visible(subnote) &&
				      purple_strequal(gtk_label_get_text(GTK_LABEL(subnote)),
				          "The server doesn't support invisibility; you will "
				          "appear available."),
				      "substatus note: %s", gtk_label_get_text(GTK_LABEL(subnote)));
			}
			if (sub != NULL)
				gtk_window_destroy(GTK_WINDOW(sub));
			spin(50);
		}
		gtk_window_destroy(GTK_WINDOW(win));
		spin(100);
	}
}

/**************************************************************************
 * Driver
 **************************************************************************/

static gboolean
selftest_run(gpointer data)
{
	r2_plugin = pidgin_selftest_prpl_register();
	CHECK(r2_plugin != NULL, "the selftest protocol did not load");
	if (r2_plugin == NULL)
		goto done;
	patch_protocol(r2_plugin);
	register_ipc(r2_plugin);
	r2_account = pidgin_selftest_account_new(R2_USER);
	spin(200);
	CHECK(purple_account_is_connected(r2_account), "the account did not connect");
	if (!purple_account_is_connected(r2_account))
		goto done;

	g_print(R2 ": privacy window\n");
	test_privacy_window();
	g_print(R2 ": report spam\n");
	test_report_spam();
	g_print(R2 ": invisible\n");
	test_invisible();

done:
	while (purple_get_conversations() != NULL)
		purple_conversation_destroy(purple_get_conversations()->data);
	spin(100);
	if (r2_account != NULL)
		pidgin_selftest_account_remove(r2_account);
	r2_account = NULL;
	if (r2_plugin != NULL)
		unpatch_protocol(r2_plugin);
	pidgin_selftest_prpl_unregister();
	pidgin_server_features_reset_cache();
	g_clear_pointer(&ipc_last_report, g_free);

	if (failures == 0)
		g_print(R2 ": PASS (%d checks)\n", checks);
	else
		g_print(R2 ": %d of %d checks FAILED\n", failures, checks);
	pidgin_application_set_exit_status(failures == 0 ? 0 : 1);
	pidgin_application_quit();
	return G_SOURCE_REMOVE;
}

void
pidgin_r2_selftest(void)
{
	if (g_getenv(R2) == NULL)
		return;
	/* After startup settles (the buddy list is shown). */
	g_timeout_add(500, selftest_run, NULL);
}
