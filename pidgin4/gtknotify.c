/*
 * @file gtknotify.c GTK+ Notification API
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
 * GTK 4 port of the notify UI ops:
 *   - messages are GtkAlertDialogs;
 *   - formatted text and user info are windows with a PidginRichLabel
 *     (purple's HTML through PidginMarkup: formatting, links, smileys and
 *     imgstore images such as buddy icons in user info);
 *   - search results are a GtkColumnView;
 *   - mail notifications are collected in one simple "New Mail" window
 *     (TODO(M5): the GTK 2 mail dialog with per-account grouping);
 *   - URIs open through GtkUriLauncher (portal-aware), replacing the
 *     GNOME/KDE/custom browser command logic and its prefs.
 * Buddy pounce notifications (pidgin_notify_pounce_add) come with the
 * pounce UI, TODO(M5).
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "account.h"
#include "connection.h"
#include "debug.h"
#include "notify.h"
#include "prefs.h"
#include "util.h"

#include "gtknotify.h"
#include "gtkutils.h"
#include "pidginrichlabel.h"

/**************************************************************************
 * Messages
 **************************************************************************/

typedef struct {
	int ref;
	gboolean closed;          /* close_notify was called */
	GCancellable *cancellable;
} PidginNotifyMessage;

static void
notify_message_unref(PidginNotifyMessage *msg)
{
	if (--msg->ref > 0)
		return;
	g_object_unref(msg->cancellable);
	g_free(msg);
}

static void
message_response_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	PidginNotifyMessage *msg = data;

	gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, NULL);

	/* Dismissed by the user: tell libpurple, which calls the notify's
	 * callback and then pidgin_close_notify(). */
	if (!msg->closed)
		purple_notify_close(PURPLE_NOTIFY_MESSAGE, msg);

	notify_message_unref(msg);
}

static void *
pidgin_notify_message(PurpleNotifyMsgType type, const char *title,
						const char *primary, const char *secondary)
{
	PidginNotifyMessage *msg;
	GtkAlertDialog *dialog;
	const char *buttons[] = { _("_Close"), NULL };

	msg = g_new0(PidginNotifyMessage, 1);
	msg->ref = 2;   /* libpurple's handle and the pending dialog */
	msg->cancellable = g_cancellable_new();

	/* GtkAlertDialog has no title or icon; the primary text leads. */
	dialog = gtk_alert_dialog_new("%s", primary ? primary : (title ? title : ""));
	if (secondary != NULL && *secondary != '\0')
		gtk_alert_dialog_set_detail(dialog, secondary);
	else if (primary != NULL && title != NULL && !purple_strequal(title, primary))
		gtk_alert_dialog_set_detail(dialog, title);
	gtk_alert_dialog_set_buttons(dialog, buttons);
	gtk_alert_dialog_set_modal(dialog, FALSE);

	purple_debug_info("gtknotify", "%s message: %s%s%s\n",
		type == PURPLE_NOTIFY_MSG_ERROR ? "error" :
		type == PURPLE_NOTIFY_MSG_WARNING ? "warning" : "info",
		primary ? primary : "", secondary ? ": " : "", secondary ? secondary : "");

	gtk_alert_dialog_choose(dialog, pidgin_get_active_window(),
	                        msg->cancellable, message_response_cb, msg);
	g_object_unref(dialog);

	return msg;
}

/**************************************************************************
 * Rich text windows (formatted, userinfo)
 **************************************************************************/

/* Puts purple HTML into a PidginRichLabel. Links are made clickable as
 * GtkIMHtml did (purple_markup_linkify); smileys follow the theme. */
static void
set_label_html(GtkWidget *label, const char *html)
{
	char *linked = purple_markup_linkify(html ? html : "");
	PidginMarkupOptions opts = { PIDGIN_MARKUP_NO_LINKIFY, NULL, NULL, NULL };

	pidgin_rich_label_set_html(PIDGIN_RICH_LABEL(label), linked, &opts);
	g_free(linked);
}

static gboolean
formatted_close_request_cb(GtkWindow *window, gpointer data)
{
	purple_notify_close(PURPLE_NOTIFY_FORMATTED, window);
	return TRUE;
}

static void
formatted_close_clicked_cb(GtkButton *button, GtkWidget *window)
{
	purple_notify_close(PURPLE_NOTIFY_FORMATTED, window);
}

static void *
pidgin_notify_formatted(const char *title, const char *primary,
						  const char *secondary, const char *text)
{
	GtkWidget *window, *content, *label, *sw, *button;

	window = pidgin_dialog_new(title, pidgin_get_active_window(), "notify_formatted", TRUE);
	gtk_window_set_default_size(GTK_WINDOW(window), 420, 360);
	g_signal_connect(window, "close-request", G_CALLBACK(formatted_close_request_cb), NULL);

	pidgin_dialog_add_message(window, NULL, primary, secondary, FALSE);

	content = pidgin_dialog_get_content_area(window);
	label = pidgin_rich_label_new();
	gtk_widget_set_valign(label, GTK_ALIGN_START);
	gtk_widget_set_margin_start(label, PIDGIN_HIG_BOX_SPACE);
	gtk_widget_set_margin_end(label, PIDGIN_HIG_BOX_SPACE);
	gtk_widget_set_margin_top(label, PIDGIN_HIG_BOX_SPACE);
	gtk_widget_set_margin_bottom(label, PIDGIN_HIG_BOX_SPACE);
	set_label_html(label, text);

	sw = pidgin_make_scrollable(label, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC, 300, 250);
	gtk_widget_set_vexpand(sw, TRUE);
	gtk_box_append(GTK_BOX(content), sw);

	button = pidgin_dialog_add_button(window, _("_Close"),
	                                  G_CALLBACK(formatted_close_clicked_cb), window);
	gtk_window_set_default_widget(GTK_WINDOW(window), button);

	g_object_set_data(G_OBJECT(window), "info-widget", label);

	gtk_window_present(GTK_WINDOW(window));
	gtk_widget_grab_focus(button);

	return window;
}

/** Xerox'ed from Finch! How the tables have turned!! ;) **/
/** User information. **/
typedef struct
{
	GtkWidget *window;
	int count;
} PidginUserInfo;

static GHashTable *userinfo;

static char *
userinfo_hash(PurpleAccount *account, const char *who)
{
	char key[256];
	snprintf(key, sizeof(key), "%s - %s", purple_account_get_username(account), purple_normalize(account, who));
	return g_utf8_strup(key, -1);
}

static void
remove_userinfo(GtkWidget *widget, gpointer key)
{
	PidginUserInfo *pinfo = g_hash_table_lookup(userinfo, key);

	/* The first close removes libpurple's record and destroys the window
	 * (we are in its destroy handler); the rest are no-ops then. */
	while (pinfo->count--)
		purple_notify_close(PURPLE_NOTIFY_USERINFO, widget);

	g_hash_table_remove(userinfo, key);
}

static void *
pidgin_notify_userinfo(PurpleConnection *gc, const char *who,
						 PurpleNotifyUserInfo *user_info)
{
	char *info;
	void *ui_handle;
	char *key = userinfo_hash(purple_connection_get_account(gc), who);
	PidginUserInfo *pinfo = NULL;

	if (!userinfo) {
		userinfo = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	}

	info = purple_notify_user_info_get_text_with_newline(user_info, "<br />");
	pinfo = g_hash_table_lookup(userinfo, key);
	if (pinfo != NULL) {
		GtkWidget *label = g_object_get_data(G_OBJECT(pinfo->window), "info-widget");
		set_label_html(label, info);
		g_free(key);
		ui_handle = pinfo->window;
		pinfo->count++;
	} else {
		char *primary = g_strdup_printf(_("Info for %s"), who);
		ui_handle = pidgin_notify_formatted(_("Buddy Information"), primary, NULL, info);
		g_signal_connect(G_OBJECT(ui_handle), "destroy", G_CALLBACK(remove_userinfo), key);
		g_free(primary);
		pinfo = g_new0(PidginUserInfo, 1);
		pinfo->window = ui_handle;
		pinfo->count = 1;
		g_hash_table_insert(userinfo, key, pinfo);
	}
	g_free(info);
	return ui_handle;
}

/**************************************************************************
 * Search results
 **************************************************************************/

#define PIDGIN_TYPE_SEARCH_ROW (pidgin_search_row_get_type())
G_DECLARE_FINAL_TYPE(PidginSearchRow, pidgin_search_row, PIDGIN, SEARCH_ROW, GObject)

struct _PidginSearchRow {
	GObject parent;
	char **columns;
};

G_DEFINE_FINAL_TYPE(PidginSearchRow, pidgin_search_row, G_TYPE_OBJECT)

static void
pidgin_search_row_finalize(GObject *obj)
{
	g_strfreev(PIDGIN_SEARCH_ROW(obj)->columns);
	G_OBJECT_CLASS(pidgin_search_row_parent_class)->finalize(obj);
}

static void
pidgin_search_row_class_init(PidginSearchRowClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = pidgin_search_row_finalize;
}

static void
pidgin_search_row_init(PidginSearchRow *row)
{
}

typedef struct
{
	void *user_data;
	PurpleNotifySearchResults *results;
	PurpleAccount *account;
	GListStore *store;
	GtkSingleSelection *selection;
	GtkWidget *window;
	GtkWidget *view;
	guint n_columns;
} PidginNotifySearchResultsData;

typedef struct
{
	PurpleNotifySearchButton *button;
	PidginNotifySearchResultsData *data;
} PidginNotifySearchResultsButtonData;

static gboolean
searchresults_close_request_cb(GtkWindow *window, PidginNotifySearchResultsData *data)
{
	purple_notify_close(PURPLE_NOTIFY_SEARCHRESULTS, data);
	return TRUE;
}

static void
searchresults_close_clicked_cb(GtkButton *button, PidginNotifySearchResultsData *data)
{
	purple_notify_close(PURPLE_NOTIFY_SEARCHRESULTS, data);
}

static void
searchresults_callback_wrapper_cb(GtkWidget *widget, PidginNotifySearchResultsButtonData *bd)
{
	PidginNotifySearchResultsData *data = bd->data;
	PidginSearchRow *row_obj;
	PurpleNotifySearchButton *button;
	GList *row = NULL;
	guint i;

	g_return_if_fail(data != NULL);

	row_obj = gtk_single_selection_get_selected_item(data->selection);
	if (row_obj != NULL) {
		for (i = 0; row_obj->columns[i] != NULL; i++)
			row = g_list_append(row, g_strdup(row_obj->columns[i]));
	}

	button = bd->button;
	button->callback(purple_account_get_connection(data->account), row, data->user_data);
	g_list_free_full(row, (GDestroyNotify)g_free);
}

static void
column_setup_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	GtkWidget *label = gtk_label_new(NULL);

	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_list_item_set_child(li, label);
}

static void
column_bind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	PidginSearchRow *row = gtk_list_item_get_item(li);
	guint col = GPOINTER_TO_UINT(data);
	const char *text = NULL;

	if (row->columns != NULL && col < g_strv_length(row->columns))
		text = row->columns[col];
	gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(li)), text ? text : "");
}

static void
icon_setup_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	PidginNotifySearchResultsData *srd = data;
	GtkWidget *image = pidgin_create_prpl_image(srd->account, NULL, PIDGIN_PRPL_ICON_SMALL);

	gtk_list_item_set_child(li, image);
}

static void
pidgin_notify_searchresults_new_rows(PurpleConnection *gc, PurpleNotifySearchResults *results,
									   void *data_)
{
	PidginNotifySearchResultsData *data = data_;
	PidginSearchRow *selected;
	gchar *previous_selection = NULL;
	GList *row;
	guint n, select = GTK_INVALID_LIST_POSITION;

	selected = gtk_single_selection_get_selected_item(data->selection);
	if (selected != NULL && selected->columns != NULL)
		previous_selection = g_strdup(selected->columns[0]);

	g_list_store_remove_all(data->store);

	for (row = results->rows, n = 0; row != NULL; row = row->next, n++) {
		PidginSearchRow *obj = g_object_new(PIDGIN_TYPE_SEARCH_ROW, NULL);
		GList *column;
		GPtrArray *cols = g_ptr_array_new();

		for (column = row->data; column != NULL; column = column->next)
			g_ptr_array_add(cols, g_strdup(column->data));
		g_ptr_array_add(cols, NULL);
		obj->columns = (char **)g_ptr_array_free(cols, FALSE);

		/* Select this row, if the first column matches the previously
		 * selected row OR if there is only one row in the results. */
		if ((obj->columns[0] != NULL && !g_strcmp0(previous_selection, obj->columns[0])) ||
		    (row == results->rows && !row->next))
			select = n;

		g_list_store_append(data->store, obj);
		g_object_unref(obj);
	}

	gtk_single_selection_set_selected(data->selection, select);

	/* The first set of results need to stick around, as we reference
	 * the buttons from there. But any updates from later calls to
	 * purple_notify_searchresults_new_rows() must be freed. */
	if (results != data->results)
		purple_notify_searchresults_free(results);

	g_free(previous_selection);
}

static void *
pidgin_notify_searchresults(PurpleConnection *gc, const char *title,
							  const char *primary, const char *secondary,
							  PurpleNotifySearchResults *results, gpointer user_data)
{
	GtkWidget *window, *view, *sw, *close_button;
	GtkColumnViewColumn *col;
	GtkListItemFactory *factory;
	PidginNotifySearchResultsData *data;
	GList *columniter, *l;
	guint i;

	g_return_val_if_fail(gc != NULL, NULL);
	g_return_val_if_fail(results != NULL, NULL);

	data = g_new0(PidginNotifySearchResultsData, 1);
	data->user_data = user_data;
	data->results = results;
	data->account = purple_connection_get_account(gc);

	/* Create the window */
	window = pidgin_dialog_new(title ? title : _("Search Results"),
	                           pidgin_get_active_window(), "searchresults", TRUE);
	gtk_window_set_default_size(GTK_WINDOW(window), 560, 480);
	g_signal_connect(window, "close-request",
	                 G_CALLBACK(searchresults_close_request_cb), data);
	data->window = window;

	pidgin_dialog_add_message(window, NULL, primary, secondary, FALSE);

	/* the list */
	data->store = g_list_store_new(PIDGIN_TYPE_SEARCH_ROW);
	data->selection = gtk_single_selection_new(G_LIST_MODEL(data->store));
	gtk_single_selection_set_autoselect(data->selection, FALSE);
	gtk_single_selection_set_can_unselect(data->selection, TRUE);
	view = gtk_column_view_new(GTK_SELECTION_MODEL(data->selection));
	gtk_column_view_set_show_row_separators(GTK_COLUMN_VIEW(view), TRUE);
	data->view = view;

	/* the protocol icon column */
	factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(icon_setup_cb), data);
	col = gtk_column_view_column_new("", factory);
	gtk_column_view_append_column(GTK_COLUMN_VIEW(view), col);
	g_object_unref(col);

	i = 0;
	for (columniter = results->columns; columniter != NULL; columniter = columniter->next) {
		PurpleNotifySearchColumn *column = columniter->data;

		factory = gtk_signal_list_item_factory_new();
		g_signal_connect(factory, "setup", G_CALLBACK(column_setup_cb), NULL);
		g_signal_connect(factory, "bind", G_CALLBACK(column_bind_cb), GUINT_TO_POINTER(i));
		col = gtk_column_view_column_new(column->title, factory);
		gtk_column_view_column_set_resizable(col, TRUE);
		gtk_column_view_column_set_expand(col, TRUE);
		gtk_column_view_append_column(GTK_COLUMN_VIEW(view), col);
		g_object_unref(col);
		i++;
	}
	data->n_columns = i;

	sw = pidgin_make_scrollable(view, GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS, 500, 300);
	gtk_widget_set_vexpand(sw, TRUE);
	gtk_box_append(GTK_BOX(pidgin_dialog_get_content_area(window)), sw);

	for (l = results->buttons; l; l = l->next) {
		PurpleNotifySearchButton *b = l->data;
		GtkWidget *button = NULL;
		const char *label = NULL;

		switch (b->type) {
			case PURPLE_NOTIFY_BUTTON_LABELED:
				if (b->label)
					label = b->label;
				else
					purple_debug_warning("gtknotify", "Missing button label\n");
				break;
			case PURPLE_NOTIFY_BUTTON_CONTINUE:
				label = _("_Forward");
				break;
			case PURPLE_NOTIFY_BUTTON_ADD:
				label = _("_Add");
				break;
			case PURPLE_NOTIFY_BUTTON_INFO:
				label = _("_Info");
				break;
			case PURPLE_NOTIFY_BUTTON_IM:
				label = _("I_M");
				break;
			case PURPLE_NOTIFY_BUTTON_JOIN:
				label = _("_Join");
				break;
			case PURPLE_NOTIFY_BUTTON_INVITE:
				label = _("_Invite");
				break;
			default:
				purple_debug_warning("gtknotify", "Incorrect button type: %d\n", b->type);
		}
		if (label != NULL) {
			PidginNotifySearchResultsButtonData *bd;

			bd = g_new0(PidginNotifySearchResultsButtonData, 1);
			bd->button = b;
			bd->data = data;

			button = pidgin_dialog_add_button(window, label,
				G_CALLBACK(searchresults_callback_wrapper_cb), bd);
			g_signal_connect_swapped(G_OBJECT(button), "destroy", G_CALLBACK(g_free), bd);
		}
	}

	/* Add the Close button */
	close_button = pidgin_dialog_add_button(window, _("_Close"),
		G_CALLBACK(searchresults_close_clicked_cb), data);
	(void)close_button;

	/* Insert rows. */
	pidgin_notify_searchresults_new_rows(gc, results, data);

	/* Show the window */
	gtk_window_present(GTK_WINDOW(window));
	return data;
}

/**************************************************************************
 * Mail
 **************************************************************************/

typedef struct {
	PurpleAccount *account;
	char *url;
} PidginNotifyMailData;

static GtkWidget *mail_window = NULL;
static GtkWidget *mail_list = NULL;

static void
mail_open_clicked_cb(GtkButton *button, gpointer data)
{
	const char *url = g_object_get_data(G_OBJECT(button), "url");

	if (url != NULL)
		pidgin_open_uri(GTK_WINDOW(mail_window), url);
}

static void
mail_close_clicked_cb(GtkButton *button, gpointer data)
{
	gtk_window_destroy(GTK_WINDOW(mail_window));
}

static void
ensure_mail_window(void)
{
	GtkWidget *sw;

	if (mail_window != NULL)
		return;

	mail_window = pidgin_dialog_new(_("New Mail"), NULL, "new_mail", TRUE);
	g_object_add_weak_pointer(G_OBJECT(mail_window), (gpointer *)&mail_window);
	gtk_window_set_default_size(GTK_WINDOW(mail_window), 480, 280);
	pidgin_dialog_add_message(mail_window, "mail-unread-symbolic",
	                          _("You have mail!"), NULL, FALSE);

	mail_list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(mail_list), GTK_SELECTION_NONE);
	gtk_widget_add_css_class(mail_list, "boxed-list");
	sw = pidgin_make_scrollable(mail_list, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC, -1, 150);
	gtk_widget_set_vexpand(sw, TRUE);
	gtk_box_append(GTK_BOX(pidgin_dialog_get_content_area(mail_window)), sw);

	pidgin_dialog_add_button(mail_window, _("_Close"),
	                         G_CALLBACK(mail_close_clicked_cb), NULL);
}

static void
add_mail_row(PurpleAccount *account, const char *text, const char *url)
{
	GtkWidget *row, *image, *label, *button;

	ensure_mail_window();

	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	gtk_widget_set_margin_start(row, PIDGIN_HIG_BOX_SPACE);
	gtk_widget_set_margin_end(row, PIDGIN_HIG_BOX_SPACE);
	gtk_widget_set_margin_top(row, PIDGIN_HIG_BOX_SPACE / 2);
	gtk_widget_set_margin_bottom(row, PIDGIN_HIG_BOX_SPACE / 2);
	g_object_set_data(G_OBJECT(row), "account", account);

	image = pidgin_create_prpl_image(account, NULL, PIDGIN_PRPL_ICON_SMALL);
	gtk_box_append(GTK_BOX(row), image);

	label = gtk_label_new(text);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_selectable(GTK_LABEL(label), TRUE);
	gtk_widget_set_hexpand(label, TRUE);
	gtk_box_append(GTK_BOX(row), label);

	if (url != NULL && *url != '\0') {
		button = gtk_button_new_with_mnemonic(_("_Open Mail"));
		g_object_set_data_full(G_OBJECT(button), "url", g_strdup(url), g_free);
		g_signal_connect(button, "clicked", G_CALLBACK(mail_open_clicked_cb), NULL);
		gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
		gtk_box_append(GTK_BOX(row), button);
	}

	gtk_list_box_append(GTK_LIST_BOX(mail_list), row);
}

/* Removes all rows of @account (a count of 0 clears them in Pidgin 2). */
static void
clear_mail_rows(PurpleAccount *account)
{
	GtkWidget *child;

	if (mail_list == NULL || mail_window == NULL)
		return;

	child = gtk_widget_get_first_child(mail_list);
	while (child != NULL) {
		GtkWidget *next = gtk_widget_get_next_sibling(child);
		GtkWidget *box = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(child));

		if (box != NULL && g_object_get_data(G_OBJECT(box), "account") == account)
			gtk_list_box_remove(GTK_LIST_BOX(mail_list), child);
		child = next;
	}

	if (gtk_widget_get_first_child(mail_list) == NULL)
		gtk_window_destroy(GTK_WINDOW(mail_window));
}

static void *
pidgin_notify_emails(PurpleConnection *gc, size_t count, gboolean detailed,
					   const char **subjects, const char **froms,
					   const char **tos, const char **urls)
{
	PurpleAccount *account;
	PidginNotifyMailData *data;
	size_t i;

	account = purple_connection_get_account(gc);

	if (count == 0) {
		clear_mail_rows(account);
		return NULL;
	}

	if (!detailed) {
		char *text = g_strdup_printf(ngettext("%s has %d new message.",
		                                      "%s has %d new messages.", (int)count),
		                             *tos, (int)count);
		clear_mail_rows(account);
		add_mail_row(account, text, urls ? *urls : NULL);
		g_free(text);
	} else {
		for (i = 0; i < count; i++) {
			GString *str = g_string_new(NULL);

			if (tos != NULL && tos[i] != NULL)
				g_string_append_printf(str, _("%s has 1 new message."), tos[i]);
			if (froms != NULL && froms[i] != NULL)
				g_string_append_printf(str, "\n%s %s", _("From:"), froms[i]);
			if (subjects != NULL && subjects[i] != NULL) {
				char *plain = purple_markup_strip_html(subjects[i]);
				g_string_append_printf(str, "\n%s %s", _("Subject:"), plain);
				g_free(plain);
			}
			add_mail_row(account, str->str, urls ? urls[i] : NULL);
			g_string_free(str, TRUE);
		}
	}

	gtk_window_present(GTK_WINDOW(mail_window));

	data = g_new0(PidginNotifyMailData, 1);
	data->account = account;
	data->url = g_strdup(urls ? urls[0] : NULL);
	return data;
}

static void *
pidgin_notify_email(PurpleConnection *gc, const char *subject, const char *from,
					  const char *to, const char *url)
{
	return pidgin_notify_emails(gc, 1, (subject != NULL),
	                            (subject == NULL ? NULL : &subject),
	                            (from    == NULL ? NULL : &from),
	                            (to      == NULL ? NULL : &to),
	                            (url     == NULL ? NULL : &url));
}

/**************************************************************************
 * URIs
 **************************************************************************/

static void *
pidgin_notify_uri(const char *uri)
{
	/* GtkUriLauncher asks the desktop (the OpenURI portal when there is
	 * one). This replaces Pidgin 2's /pidgin/browsers prefs. */
	pidgin_open_uri(NULL, uri);
	return NULL;
}

/**************************************************************************
 * Closing
 **************************************************************************/

static void
pidgin_close_notify(PurpleNotifyType type, void *ui_handle)
{
	if (type == PURPLE_NOTIFY_EMAIL || type == PURPLE_NOTIFY_EMAILS)
	{
		PidginNotifyMailData *data = (PidginNotifyMailData *)ui_handle;

		if (data) {
			g_free(data->url);
			g_free(data);
		}
	}
	else if (type == PURPLE_NOTIFY_MESSAGE)
	{
		PidginNotifyMessage *msg = ui_handle;

		if (msg != NULL) {
			msg->closed = TRUE;
			g_cancellable_cancel(msg->cancellable);
			notify_message_unref(msg);
		}
	}
	else if (type == PURPLE_NOTIFY_SEARCHRESULTS)
	{
		PidginNotifySearchResultsData *data = (PidginNotifySearchResultsData *)ui_handle;

		gtk_window_destroy(GTK_WINDOW(data->window));
		g_object_unref(data->selection);
		g_object_unref(data->store);
		purple_notify_searchresults_free(data->results);

		g_free(data);
	}
	else if (ui_handle != NULL)
		gtk_window_destroy(GTK_WINDOW(ui_handle));
}

static void
signed_off_cb(PurpleConnection *gc, gpointer unused)
{
	/* Clear any pending emails for this account */
	clear_mail_rows(purple_connection_get_account(gc));
}

static void*
pidgin_notify_get_handle(void)
{
	static int handle;
	return &handle;
}

void pidgin_notify_init(void)
{
	void *handle = pidgin_notify_get_handle();

	purple_signal_connect(purple_connections_get_handle(), "signed-off",
			handle, PURPLE_CALLBACK(signed_off_cb), NULL);
}

void pidgin_notify_uninit(void)
{
	purple_signals_disconnect_by_handle(pidgin_notify_get_handle());

	if (mail_window != NULL)
		gtk_window_destroy(GTK_WINDOW(mail_window));
}

static PurpleNotifyUiOps ops =
{
	pidgin_notify_message,
	pidgin_notify_email,
	pidgin_notify_emails,
	pidgin_notify_formatted,
	pidgin_notify_searchresults,
	pidgin_notify_searchresults_new_rows,
	pidgin_notify_userinfo,
	pidgin_notify_uri,
	pidgin_close_notify,
	NULL,
	NULL,
	NULL,
	NULL
};

PurpleNotifyUiOps *
pidgin_notify_get_ui_ops(void)
{
	return &ops;
}
