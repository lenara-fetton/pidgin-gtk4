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
#include "version.h"

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
#include "pidginomemo.h"
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

/* As find_class(), but only a visible one (rows are recycled). */
static GtkWidget *
find_visible_class(GtkWidget *widget, const char *css_class)
{
	GtkWidget *child, *found;

	if (widget == NULL || !gtk_widget_get_visible(widget))
		return NULL;
	if (gtk_widget_has_css_class(widget, css_class))
		return widget;
	for (child = gtk_widget_get_first_child(widget); child != NULL;
	     child = gtk_widget_get_next_sibling(child))
		if ((found = find_visible_class(child, css_class)) != NULL)
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
 * 3. File-sharing metadata (XEP-0447) → attachments
 **************************************************************************/

#define FRIEND "friend@example.invalid"

static SoupServer *server = NULL;
static char *server_base = NULL;
static GBytes *pic_png = NULL;

static GBytes *
make_png(int width, int height, guint8 red)
{
	guchar *pixels = g_malloc(width * height * 4);
	GBytes *bytes, *png;
	GdkTexture *texture;
	int i;

	for (i = 0; i < width * height * 4; i += 4) {
		pixels[i] = red;
		pixels[i + 1] = 0x44;
		pixels[i + 2] = 0x88;
		pixels[i + 3] = 0xff;
	}
	bytes = g_bytes_new_take(pixels, width * height * 4);
	texture = gdk_memory_texture_new(width, height, GDK_MEMORY_R8G8B8A8, bytes, width * 4);
	png = gdk_texture_save_to_png_bytes(texture);
	g_bytes_unref(bytes);
	g_object_unref(texture);
	return png;
}

static void
server_cb(SoupServer *srv, SoupServerMessage *msg, const char *path, GHashTable *query,
          gpointer data)
{
	if (g_str_has_prefix(path, "/r2/pic")) {
		soup_server_message_set_status(msg, 200, NULL);
		soup_server_message_set_response(msg, "image/png", SOUP_MEMORY_COPY,
		                                 g_bytes_get_data(pic_png, NULL),
		                                 g_bytes_get_size(pic_png));
	} else {
		soup_server_message_set_status(msg, 404, NULL);
	}
}

static gboolean
server_start(void)
{
	GError *error = NULL;
	GSList *uris;

	pic_png = make_png(64, 48, 0x22);
	server = soup_server_new(NULL, NULL);
	soup_server_add_handler(server, NULL, server_cb, NULL, NULL);
	if (!soup_server_listen_local(server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error)) {
		g_printerr(R2 ": server: %s\n", error->message);
		g_clear_error(&error);
		g_clear_object(&server);
		return FALSE;
	}
	uris = soup_server_get_uris(server);
	server_base = g_strdup_printf("http://127.0.0.1:%d", g_uri_get_port(uris->data));
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
	pidgin_image_loader_set_allow_http_for_tests(pidgin_image_loader_get_default(), TRUE);
	pidgin_conv_meta_set_share_protocol_for_tests(PIDGIN_SELFTEST_PRPL_ID);
	return TRUE;
}

static void
server_stop(void)
{
	if (server != NULL) {
		soup_server_disconnect(server);
		g_clear_object(&server);
	}
	g_clear_pointer(&server_base, g_free);
	g_clear_pointer(&pic_png, g_bytes_unref);
	pidgin_conv_meta_set_share_protocol_for_tests(NULL);
	pidgin_image_loader_set_allow_http_for_tests(pidgin_image_loader_get_default(), FALSE);
}

static char *
sha256_b64(GBytes *bytes)
{
	guint8 digest[32];
	gsize len = sizeof(digest);
	GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
	char *b64, *ret;

	g_checksum_update(sum, g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes));
	g_checksum_get_digest(sum, digest, &len);
	g_checksum_free(sum);
	b64 = g_base64_encode(digest, len);
	ret = g_strconcat("sha-256:", b64, NULL);
	g_free(b64);
	return ret;
}

static char *
thumbnail_uri(void)
{
	GBytes *png = make_png(16, 12, 0xee);
	char *b64 = g_base64_encode(g_bytes_get_data(png, NULL), g_bytes_get_size(png));
	char *ret = g_strconcat("data:image/png;base64,", b64, NULL);

	g_free(b64);
	g_bytes_unref(png);
	return ret;
}

static GHashTable *
meta_new(const char *first_key, ...)
{
	GHashTable *meta = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	const char *key = first_key;
	va_list args;

	va_start(args, first_key);
	while (key != NULL) {
		const char *value = va_arg(args, const char *);

		if (value != NULL)
			g_hash_table_insert(meta, g_strdup(key), g_strdup(value));
		key = va_arg(args, const char *);
	}
	va_end(args);
	return meta;
}

static PidginMessageView *
view_of(PurpleConversation *conv)
{
	return PIDGIN_MESSAGE_VIEW(pidgin_conv_get_message_view(PIDGIN_CONVERSATION(conv)));
}

static PidginMessage *
last_message(PurpleConversation *conv)
{
	GListModel *model = pidgin_message_view_get_model(view_of(conv));
	guint n = g_list_model_get_n_items(model);
	PidginMessage *m = n ? g_list_model_get_item(model, n - 1) : NULL;

	if (m != NULL)
		g_object_unref(m);      /* the store keeps it */
	return m;
}

/* The meta, then the message, as the prpl does; the new last message. */
static PidginMessage *
receive_meta(PurpleConversation *conv, GHashTable *meta, const char *body)
{
	purple_signal_emit(purple_conversations_get_handle(), "receiving-message-meta",
	                   r2_account, FRIEND, meta);
	g_hash_table_destroy(meta);     /* as the prpl does */
	serv_got_im(purple_account_get_connection(r2_account), FRIEND, body, PURPLE_MESSAGE_RECV,
	            time(NULL));
	return last_message(conv);
}

static gboolean
hash_state_is_final(gpointer data)
{
	PidginAttachmentHashState s = pidgin_attachment_get_hash_state(data);

	return s != PIDGIN_ATTACHMENT_HASH_NONE && s != PIDGIN_ATTACHMENT_HASH_PENDING;
}

static void
test_file_shares(void)
{
	PurpleConversation *conv;
	PidginMessage *msg;
	PidginAttachment *att;
	GtkWidget *view, *w;
	char *url, *hash, *thumb, *size;
	int width = 0, height = 0;
	gboolean playback;

	if (!server_start()) {
		CHECK(FALSE, "no local server");
		return;
	}
	conv = purple_conversation_new(PURPLE_CONV_TYPE_IM, r2_account, FRIEND);
	spin(200);
	CHECK(conv != NULL && PIDGIN_CONVERSATION(conv) != NULL, "no conversation");
	if (conv == NULL)
		goto out;
	pidgin_conv_window_switch_gtkconv(PIDGIN_CONVERSATION(conv)->win, PIDGIN_CONVERSATION(conv));
	view = GTK_WIDGET(view_of(conv));

	/* A thumbnail and reserved space, without any download (a URI the
	 * loader doesn't allow) */
	thumb = thumbnail_uri();
	att = pidgin_attachment_new_for_share(meta_new("sfs-url", "https://example.invalid/x.png",
		"sfs-name", "x.png", "sfs-media-type", "image/png", "sfs-width", "640",
		"sfs-height", "480", "sfs-thumbnail", thumb, NULL));
	CHECK(att != NULL && pidgin_attachment_get_thumbnail(att) != NULL &&
	      gdk_texture_get_width(pidgin_attachment_get_thumbnail(att)) == 16,
	      "no thumbnail decoded from the data: URI");
	if (att != NULL) {
		GtkWidget *card = g_object_ref_sink(pidgin_attachment_widget_new(att));
		GtkWidget *pic = find_class(card, "pidgin-share-thumbnail", NULL);
		int rw = 0, rh = 0;

		CHECK(pic != NULL && gtk_picture_get_paintable(GTK_PICTURE(pic)) ==
		      GDK_PAINTABLE(pidgin_attachment_get_thumbnail(att)), "the card has no thumbnail");
		if (pic != NULL)
			gtk_widget_get_size_request(pic, &rw, &rh);
		CHECK(rw == 320 && rh == 240, "reserved %dx%d, not 320x240", rw, rh);
		spin(100);
		CHECK(pidgin_attachment_get_texture(att) == NULL, "loaded a URI that isn't allowed");
		g_object_unref(card);
		g_object_unref(att);
	}
	CHECK(pidgin_attachment_new_for_share(meta_new("eme-name", "x", NULL)) == NULL,
	      "an attachment without sfs keys");

	/* 1: text plus a share: the text, and the card with the thumbnail at
	 * once; then the image, and the hash verified */
	url = g_strconcat(server_base, "/r2/pic.png", NULL);
	hash = sha256_b64(pic_png);
	size = g_strdup_printf("%" G_GSIZE_FORMAT, g_bytes_get_size(pic_png));
	msg = receive_meta(conv, meta_new("sfs-url", url, "sfs-name", "holiday photo.png",
		"sfs-size", size, "sfs-media-type", "image/png", "sfs-width", "64",
		"sfs-height", "48", "sfs-desc", "The beach at noon", "sfs-hash", hash,
		"sfs-thumbnail", thumb, "sfs-disposition", "inline", NULL),
		"Here is the photo");
	att = msg ? pidgin_message_get_attachment(msg) : NULL;
	CHECK(msg != NULL && strstr(pidgin_message_get_plain_text(msg), "Here is the photo") != NULL,
	      "the text is gone: %s", msg ? pidgin_message_get_plain_text(msg) : "-");
	CHECK(att != NULL && pidgin_attachment_is_share(att), "no share attachment");
	if (att != NULL) {
		pidgin_attachment_get_dimensions(att, &width, &height);
		CHECK(pidgin_attachment_get_kind(att) == PIDGIN_ATTACHMENT_IMAGE, "kind %d",
		      pidgin_attachment_get_kind(att));
		CHECK(purple_strequal(pidgin_attachment_get_name(att), "holiday photo.png") &&
		      pidgin_attachment_get_size(att) == (goffset)g_bytes_get_size(pic_png) &&
		      width == 64 && height == 48 &&
		      purple_strequal(pidgin_attachment_get_media_type(att), "image/png") &&
		      purple_strequal(pidgin_attachment_get_description(att), "The beach at noon"),
		      "attachment fields");
		CHECK(pidgin_attachment_get_thumbnail(att) != NULL, "no thumbnail before download");
		CHECK(pidgin_attachment_get_texture(att) == NULL, "downloaded already");
		CHECK(pidgin_image_loader_is_allowed(pidgin_image_loader_get_default(), url),
		      "the share's URI isn't allowed");
		g_object_ref(att);
		pidgin_selftest_wait(hash_state_is_final, att, 5000);
		CHECK(pidgin_attachment_get_texture(att) != NULL, "the image did not load");
		CHECK(pidgin_attachment_get_hash_state(att) == PIDGIN_ATTACHMENT_HASH_VERIFIED,
		      "hash state %d", pidgin_attachment_get_hash_state(att));
		spin(200);
		w = find_class(view, "pidgin-share-hash", NULL);
		CHECK(w != NULL && gtk_widget_get_visible(w) &&
		      strstr(gtk_label_get_text(GTK_LABEL(w)), "verified") != NULL &&
		      strstr(gtk_widget_get_tooltip_text(w), "sha-256") != NULL,
		      "no verified mark in the view: %s", w ? gtk_label_get_text(GTK_LABEL(w)) : "-");
		w = find_class(view, "pidgin-share-desc", NULL);
		CHECK(w != NULL && purple_strequal(gtk_label_get_text(GTK_LABEL(w)),
		                                   "The beach at noon"), "no caption");
		w = find_class(view, "pidgin-share-image", "holiday photo.png");
		CHECK(w != NULL && gtk_picture_get_paintable(GTK_PICTURE(w)) ==
		      GDK_PAINTABLE(pidgin_attachment_get_texture(att)), "the view shows no image");
		g_object_unref(att);
	}
	g_free(url);
	g_free(hash);

	/* 2: a lone URL body with a wrong hash: no inline copy of the URL,
	 * one card, "hash mismatch" */
	url = g_strconcat(server_base, "/r2/pic-other.png", NULL);
	msg = receive_meta(conv, meta_new("sfs-url", url, "sfs-name", "other.png",
		"sfs-media-type", "image/png",
		"sfs-hash", "sha-256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=", NULL), url);
	att = msg ? pidgin_message_get_attachment(msg) : NULL;
	CHECK(msg != NULL && strstr(pidgin_message_get_html(msg), "<img") == NULL,
	      "the share URL was inlined too: %s", msg ? pidgin_message_get_html(msg) : "-");
	CHECK(att != NULL && pidgin_attachment_is_share(att), "no share attachment for a URL body");
	if (att != NULL) {
		g_object_ref(att);
		pidgin_selftest_wait(hash_state_is_final, att, 5000);
		CHECK(pidgin_attachment_get_hash_state(att) == PIDGIN_ATTACHMENT_HASH_MISMATCH,
		      "hash state %d", pidgin_attachment_get_hash_state(att));
		spin(200);
		w = find_class(view, "error", NULL);
		CHECK(w != NULL && GTK_IS_LABEL(w) &&
		      strstr(gtk_label_get_text(GTK_LABEL(w)), "hash mismatch") != NULL &&
		      strstr(gtk_widget_get_tooltip_text(w), "does not match") != NULL,
		      "no mismatch mark in the view");
		g_object_unref(att);
	}
	g_free(url);

	/* 3: an algorithm we can't compute: not verified */
	url = g_strconcat(server_base, "/r2/pic-third.png", NULL);
	msg = receive_meta(conv, meta_new("sfs-url", url, "sfs-media-type", "image/png",
		"sfs-hash", "sha3-256:AAAA", NULL), url);
	att = msg ? pidgin_message_get_attachment(msg) : NULL;
	CHECK(att != NULL && purple_strequal(pidgin_attachment_get_name(att), "pic-third.png"),
	      "a share without sfs-name: %s", att ? pidgin_attachment_get_name(att) : "-");
	if (att != NULL) {
		g_object_ref(att);
		pidgin_selftest_wait(hash_state_is_final, att, 5000);
		CHECK(pidgin_attachment_get_hash_state(att) == PIDGIN_ATTACHMENT_HASH_UNCHECKED,
		      "sha3 state %d", pidgin_attachment_get_hash_state(att));
		g_object_unref(att);
	}
	g_free(url);

	/* 4: a video: the media card with the real name and size (no inline
	 * player: GTK's GStreamer backend criticals on an http source that
	 * isn't a video, and the selftest runs with fatal criticals) */
	playback = purple_prefs_exists(PIDGIN4_PREFS_ROOT "/media/inline_playback") &&
	           !purple_prefs_get_bool(PIDGIN4_PREFS_ROOT "/media/inline_playback") ? 0 : 1;
	if (purple_prefs_exists(PIDGIN4_PREFS_ROOT "/media/inline_playback"))
		purple_prefs_set_bool(PIDGIN4_PREFS_ROOT "/media/inline_playback", FALSE);
	url = g_strconcat(server_base, "/r2/upload/abc123", NULL);
	msg = receive_meta(conv, meta_new("sfs-url", url, "sfs-name", "holiday.mp4",
		"sfs-size", "1234567", "sfs-media-type", "video/mp4", NULL), url);
	att = msg ? pidgin_message_get_attachment(msg) : NULL;
	CHECK(att != NULL && pidgin_attachment_get_kind(att) == PIDGIN_ATTACHMENT_VIDEO &&
	      purple_strequal(pidgin_attachment_get_name(att), "holiday.mp4") &&
	      pidgin_attachment_get_size(att) == 1234567, "video attachment");
	spin(200);
	w = find_label(view, "holiday.mp4");
	CHECK(w != NULL && gtk_widget_has_css_class(w, "pidgin-media-name"),
	      "no media card named holiday.mp4");
	g_free(url);
	if (purple_prefs_exists(PIDGIN4_PREFS_ROOT "/media/inline_playback"))
		purple_prefs_set_bool(PIDGIN4_PREFS_ROOT "/media/inline_playback", playback);

	/* 5: another file: a file card with Open */
	url = g_strconcat(server_base, "/r2/report.pdf", NULL);
	msg = receive_meta(conv, meta_new("sfs-url", url, "sfs-name", "report.pdf",
		"sfs-size", "2048", "sfs-media-type", "application/pdf", NULL), url);
	att = msg ? pidgin_message_get_attachment(msg) : NULL;
	CHECK(att != NULL && pidgin_attachment_get_kind(att) == PIDGIN_ATTACHMENT_NONE,
	      "pdf attachment");
	spin(200);
	w = find_class(view, "pidgin-file-card", NULL);
	CHECK(w != NULL && find_label(w, "report.pdf") != NULL &&
	      find_class(w, "pidgin-share-open", NULL) != NULL, "no file card");
	g_free(url);

	/* the verification on its own */
	{
		GBytes *b = g_bytes_new_static("hello", 5);
		PidginAttachment *a = pidgin_attachment_new_for_share(meta_new(
			"sfs-name", "hello.txt",
			"sfs-hash", "sha-256:LPJNul+wow4m6DsqxbninhsWHlwfp0JecwQzYpOLmCQ=", NULL));

		pidgin_attachment_verify_bytes(a, b);
		pidgin_selftest_wait(hash_state_is_final, a, 3000);
		CHECK(pidgin_attachment_get_hash_state(a) == PIDGIN_ATTACHMENT_HASH_VERIFIED,
		      "sha-256 of hello: %d", pidgin_attachment_get_hash_state(a));
		g_object_unref(a);
		g_bytes_unref(b);
	}
	g_free(size);
	g_free(thumb);
	purple_conversation_destroy(conv);
	spin(100);
out:
	server_stop();
}

/**************************************************************************
 * 4. Explicit message encryption (XEP-0380)
 **************************************************************************/

#define OMEMO2_JID "omemo2@example.invalid"
#define OMEMO1_JID "omemo1@example.invalid"

static GList *
fake_list_devices(PurpleAccount *account, const char *jid)
{
	GHashTable *h;

	if (jid == NULL || (!purple_strequal(jid, OMEMO2_JID) && !purple_strequal(jid, OMEMO1_JID)))
		return NULL;
	h = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_hash_table_insert(h, g_strdup("device-id"), g_strdup("4711"));
	g_hash_table_insert(h, g_strdup("fingerprint"), g_strdup("05aabbccddeeff00112233"));
	g_hash_table_insert(h, g_strdup("trust"), g_strdup("undecided"));
	g_hash_table_insert(h, g_strdup("active"), g_strdup("1"));
	g_hash_table_insert(h, g_strdup("session"), g_strdup("0"));
	g_hash_table_insert(h, g_strdup("own"), g_strdup("0"));
	return g_list_append(NULL, h);
}

static char *
fake_own_fingerprint(PurpleAccount *account)
{
	return g_strdup("05ffeeddccbbaa99887766");
}

static gboolean
fake_omemo_load(PurplePlugin *plugin)
{
	purple_plugin_ipc_register(plugin, "omemo-list-devices", PURPLE_CALLBACK(fake_list_devices),
	                           purple_marshal_POINTER__POINTER_POINTER,
	                           purple_value_new(PURPLE_TYPE_POINTER), 2,
	                           purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
	                           purple_value_new(PURPLE_TYPE_STRING));
	purple_plugin_ipc_register(plugin, "omemo-own-fingerprint",
	                           PURPLE_CALLBACK(fake_own_fingerprint),
	                           purple_marshal_POINTER__POINTER,
	                           purple_value_new(PURPLE_TYPE_STRING), 1,
	                           purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT));
	/* the window connects to it (never emitted here) */
	purple_signal_register(plugin, "omemo-new-device", purple_marshal_VOID__POINTER_POINTER_UINT,
	                       NULL, 4,
	                       purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
	                       purple_value_new(PURPLE_TYPE_STRING),
	                       purple_value_new(PURPLE_TYPE_UINT),
	                       purple_value_new(PURPLE_TYPE_STRING));
	return TRUE;
}

static gboolean
fake_omemo_unload(PurplePlugin *plugin)
{
	purple_plugin_ipc_unregister_all(plugin);
	purple_signals_unregister_by_instance(plugin);
	return TRUE;
}

static PurplePluginInfo fake_omemo_info = {
	.magic = PURPLE_PLUGIN_MAGIC,
	.major_version = PURPLE_MAJOR_VERSION,
	.minor_version = PURPLE_MINOR_VERSION,
	.type = PURPLE_PLUGIN_STANDARD,
	.priority = PURPLE_PRIORITY_DEFAULT,
	.id = "core-pidgin4-r2-omemo-standin",
	.name = "OMEMO (selftest stand-in)",
	.version = "0",
	.summary = "pidgin4 round-2 selftest",
	.description = "Lists one device for two JIDs.",
	.load = fake_omemo_load,
	.unload = fake_omemo_unload,
};

static void
test_omemo2_note(void)
{
	PurplePlugin *fake;
	PurpleAccount *xmpp;
	GtkWidget *win, *note;

	if (purple_find_prpl("prpl-jabber") == NULL) {
		g_print(R2 ": no XMPP protocol; OMEMO 2 note not checked\n");
		return;
	}
	fake = purple_plugin_new(TRUE, NULL);
	fake->info = &fake_omemo_info;
	purple_plugin_register(fake);
	CHECK(purple_plugin_load(fake), "the stand-in didn't load");
	pidgin_omemo_set_plugin_for_tests(fake);
	CHECK(pidgin_omemo_is_available(), "the stand-in isn't used");

	/* an XMPP account (never enabled or connected) that got OMEMO 2 */
	xmpp = purple_account_new("r2-omemo@example.invalid", "prpl-jabber");
	purple_accounts_add(xmpp);
	purple_signal_emit(purple_conversations_get_handle(), "receiving-message-meta", xmpp,
	                   OMEMO2_JID, meta_new("eme-namespace", "urn:xmpp:omemo:2",
	                                        "eme-name", "OMEMO", NULL));
	CHECK(pidgin_conv_meta_saw_encryption(xmpp, OMEMO2_JID "/phone", "urn:xmpp:omemo:2"),
	      "the OMEMO 2 message wasn't noted");
	CHECK(!pidgin_conv_meta_saw_encryption(xmpp, OMEMO1_JID, "urn:xmpp:omemo:2"),
	      "noted for someone else");

	pidgin_omemo_show_fingerprints(xmpp, OMEMO2_JID);
	spin(200);
	win = find_window("omemo");
	note = find_named(win, "pidgin-omemo2-note");
	CHECK(note != NULL && gtk_widget_get_visible(note) &&
	      strstr(gtk_label_get_text(GTK_LABEL(note)), OMEMO2_JID) != NULL &&
	      strstr(gtk_label_get_text(GTK_LABEL(note)), "OMEMO 2") != NULL,
	      "no OMEMO 2 note: %s", note ? gtk_label_get_text(GTK_LABEL(note)) : "-");
	/* a contact with devices but no OMEMO 2 message: no note */
	pidgin_omemo_show_fingerprints(xmpp, OMEMO1_JID);
	spin(100);
	CHECK(note != NULL && !gtk_widget_get_visible(note), "a note for an OMEMO 1 contact");
	if (win != NULL)
		gtk_window_destroy(GTK_WINDOW(win));
	spin(100);

	purple_accounts_delete(xmpp);
	pidgin_omemo_set_plugin_for_tests(NULL);
	purple_plugin_unload(fake);
	purple_plugin_destroy(fake);
	spin(50);
}

static void
test_encryption_hint(void)
{
	PurpleConversation *conv;
	PidginMessage *msg;
	GtkWidget *view, *hint, *label;

	conv = purple_conversation_new(PURPLE_CONV_TYPE_IM, r2_account, FRIEND);
	spin(200);
	CHECK(conv != NULL && PIDGIN_CONVERSATION(conv) != NULL, "no conversation");
	if (conv == NULL)
		return;
	pidgin_conv_window_switch_gtkconv(PIDGIN_CONVERSATION(conv)->win, PIDGIN_CONVERSATION(conv));
	view = GTK_WIDGET(view_of(conv));

	msg = receive_meta(conv, meta_new("eme-namespace", "urn:xmpp:openpgp:0",
		"eme-name", "OpenPGP for XMPP", "stanza-id", "r2-eme-1", NULL),
		"This message is encrypted with OpenPGP for XMPP.");
	CHECK(msg != NULL && purple_strequal(pidgin_message_get_encryption(msg), "OpenPGP for XMPP") &&
	      purple_strequal(pidgin_message_get_encryption_namespace(msg), "urn:xmpp:openpgp:0"),
	      "encryption: %s", msg ? pidgin_message_get_encryption(msg) : "-");
	CHECK(msg != NULL && strstr(pidgin_message_get_plain_text(msg), "encrypted with") != NULL,
	      "the body changed");
	spin(200);
	hint = find_visible_class(view, "pidgin-eme-hint");
	label = hint ? find_type(hint, GTK_TYPE_LABEL) : NULL;
	CHECK(hint != NULL && gtk_widget_get_visible(hint) && label != NULL &&
	      purple_strequal(gtk_label_get_text(GTK_LABEL(label)),
	                      "Encrypted with OpenPGP for XMPP, which this client doesn't support") &&
	      g_str_has_prefix(gtk_label_get_label(GTK_LABEL(label)), "<i>"),
	      "no hint line: %s", label ? gtk_label_get_label(GTK_LABEL(label)) : "-");
	CHECK(hint != NULL && find_type(hint, GTK_TYPE_IMAGE) != NULL &&
	      purple_strequal(gtk_image_get_icon_name(GTK_IMAGE(find_type(hint, GTK_TYPE_IMAGE))),
	                      "channel-insecure-symbolic"), "no lock-slash icon");
	CHECK(hint != NULL && purple_strequal(gtk_widget_get_tooltip_text(hint),
	                                      "urn:xmpp:openpgp:0"), "no namespace tooltip");

	/* no name: the namespace; a plain message: none */
	msg = receive_meta(conv, meta_new("eme-namespace", "urn:example:crypto", NULL), "?CRYPTO");
	CHECK(msg != NULL && purple_strequal(pidgin_message_get_encryption(msg),
	                                     "urn:example:crypto"), "unnamed scheme");
	msg = receive_meta(conv, meta_new("stanza-id", "r2-plain", NULL), "plain text");
	CHECK(msg != NULL && pidgin_message_get_encryption(msg) == NULL, "a plain message has one");
	purple_conversation_destroy(conv);
	spin(100);

	test_omemo2_note();
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
	g_print(R2 ": file shares\n");
	test_file_shares();
	g_print(R2 ": encryption hint\n");
	test_encryption_hint();

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
