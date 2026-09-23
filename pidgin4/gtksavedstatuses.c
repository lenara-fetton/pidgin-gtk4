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
 * Saved statuses: port of pidgin/gtksavedstatuses.c (the saved statuses
 * window, the status editor and the per-account substatus editor). The
 * status box menu helpers of the GTK 2 file are not needed: the pidgin4
 * status box builds its own list.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "account.h"
#include "debug.h"
#include "notify.h"
#include "prefs.h"
#include "request.h"
#include "savedstatuses.h"
#include "status.h"
#include "util.h"

#include "gtksavedstatuses.h"
#include "gtkutils.h"
#include "pidgincomposeentry.h"
#include "pidginselftest.h"

#define PREFS_DIALOG "/pidgin4/status"

static const char *
primitive_icon_name(PurpleStatusPrimitive primitive)
{
	switch (primitive) {
	case PURPLE_STATUS_AWAY:
		return "pidgin-status-away";
	case PURPLE_STATUS_UNAVAILABLE:
		return "pidgin-status-busy";
	case PURPLE_STATUS_EXTENDED_AWAY:
		return "pidgin-status-extended-away";
	case PURPLE_STATUS_INVISIBLE:
		return "pidgin-status-invisible";
	case PURPLE_STATUS_OFFLINE:
		return "pidgin-status-offline";
	default:
		return "pidgin-status-available";
	}
}

static gboolean
savedstatus_exists(PurpleSavedStatus *status)
{
	return status != NULL && g_list_find(purple_savedstatuses_get_all(), status) != NULL;
}

static gboolean
account_exists(PurpleAccount *account)
{
	return account != NULL && g_list_find(purple_accounts_get_all(), account) != NULL;
}

static char *
html_to_plain_line(const char *html)
{
	char *plain;

	if (html == NULL)
		return g_strdup("");
	plain = purple_markup_strip_html(html);
	/* One line in a list cell */
	g_strdelimit(plain, "\r\n", ' ');
	return plain;
}

/* The common cell: an optional icon and a label. */
static void
icon_label_setup_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	GtkWidget *box, *image, *label;

	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	image = gtk_image_new();
	gtk_image_set_pixel_size(GTK_IMAGE(image), 16);
	label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_box_append(GTK_BOX(box), image);
	gtk_box_append(GTK_BOX(box), label);
	gtk_list_item_set_child(li, box);
}

static void
icon_label_set(GtkListItem *li, GIcon *icon, const char *icon_name, const char *text)
{
	GtkWidget *image = gtk_widget_get_first_child(gtk_list_item_get_child(li));
	GtkWidget *label = gtk_widget_get_next_sibling(image);

	if (icon != NULL)
		gtk_image_set_from_gicon(GTK_IMAGE(image), icon);
	else if (icon_name != NULL)
		gtk_image_set_from_icon_name(GTK_IMAGE(image), icon_name);
	else
		gtk_image_clear(GTK_IMAGE(image));
	gtk_widget_set_visible(image, icon != NULL || icon_name != NULL);
	gtk_label_set_text(GTK_LABEL(label), text ? text : "");
}

static GtkColumnViewColumn *
add_column(GtkWidget *columnview, const char *title, GCallback setup,
           GCallback bind, gpointer data, gboolean expand)
{
	GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
	GtkColumnViewColumn *column;

	g_signal_connect(factory, "setup", setup, data);
	g_signal_connect(factory, "bind", bind, data);
	column = gtk_column_view_column_new(title, factory);
	gtk_column_view_column_set_expand(column, expand);
	gtk_column_view_column_set_resizable(column, TRUE);
	gtk_column_view_append_column(GTK_COLUMN_VIEW(columnview), column);
	g_object_unref(column);
	return column;
}

/**************************************************************************
 * Row types
 **************************************************************************/

/* A saved status in the window's list. */
#define PIDGIN_TYPE_SAVED_STATUS_ROW (pidgin_saved_status_row_get_type())
G_DECLARE_FINAL_TYPE(PidginSavedStatusRow, pidgin_saved_status_row, PIDGIN,
                     SAVED_STATUS_ROW, GObject)

struct _PidginSavedStatusRow {
	GObject parent;
	PurpleSavedStatus *status;
};

G_DEFINE_FINAL_TYPE(PidginSavedStatusRow, pidgin_saved_status_row, G_TYPE_OBJECT)

static void pidgin_saved_status_row_class_init(PidginSavedStatusRowClass *klass) { }
static void pidgin_saved_status_row_init(PidginSavedStatusRow *row) { }

/* An account in the editor's "different status" list. Rows are not
 * changed in place: an update replaces the row, which rebinds it. */
#define PIDGIN_TYPE_SUBSTATUS_ROW (pidgin_substatus_row_get_type())
G_DECLARE_FINAL_TYPE(PidginSubstatusRow, pidgin_substatus_row, PIDGIN,
                     SUBSTATUS_ROW, GObject)

struct _PidginSubstatusRow {
	GObject parent;
	PurpleAccount *account;
	gboolean enabled;
	char *id;
	char *name;
	char *message;
	PurpleStatusPrimitive primitive;
};

G_DEFINE_FINAL_TYPE(PidginSubstatusRow, pidgin_substatus_row, G_TYPE_OBJECT)

static void
pidgin_substatus_row_finalize(GObject *obj)
{
	PidginSubstatusRow *row = PIDGIN_SUBSTATUS_ROW(obj);

	g_free(row->id);
	g_free(row->name);
	g_free(row->message);
	G_OBJECT_CLASS(pidgin_substatus_row_parent_class)->finalize(obj);
}

static void
pidgin_substatus_row_class_init(PidginSubstatusRowClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = pidgin_substatus_row_finalize;
}

static void pidgin_substatus_row_init(PidginSubstatusRow *row) { }

static PidginSubstatusRow *
substatus_row_new(PurpleAccount *account, gboolean enabled, const char *id,
                  const char *name, const char *message, PurpleStatusPrimitive prim)
{
	PidginSubstatusRow *row = g_object_new(PIDGIN_TYPE_SUBSTATUS_ROW, NULL);

	row->account = account;
	row->enabled = enabled;
	row->id = g_strdup(id);
	row->name = g_strdup(name);
	row->message = g_strdup(message);
	row->primitive = prim;
	return row;
}

/**************************************************************************
 * The window and editor structures
 **************************************************************************/

typedef struct
{
	GtkWidget *window;
	GListStore *store;
	GtkSingleSelection *selection;
	GtkWidget *columnview;
	GtkWidget *use_button;
	GtkWidget *modify_button;
	GtkWidget *duplicate_button;
	GtkWidget *delete_button;
	GCancellable *cancellable;
} StatusWindow;

typedef struct
{
	GtkWidget *window;
	GListStore *store;
	GtkSingleSelection *selection;
	GtkWidget *columnview;
	GtkWidget *use_button;
	GtkWidget *saveanduse_button;
	GtkWidget *save_button;

	char *original_title;
	GtkWidget *title;
	GtkWidget *type;
	GtkWidget *message;

	/* PurpleAccount -> SubStatusEditor */
	GHashTable *substatus_editors;
	guint close_idle;
} StatusEditor;

typedef struct
{
	StatusEditor *status_editor;
	PurpleAccount *account;

	GtkWidget *window;
	GtkWidget *box;
	GtkWidget *message_box;
	GtkWidget *message;
} SubStatusEditor;

static StatusWindow *status_window = NULL;
static GList *status_editors = NULL;

/**************************************************************************
 * The saved statuses window
 **************************************************************************/

static PurpleSavedStatus *
status_window_selected(void)
{
	PidginSavedStatusRow *row;

	if (status_window == NULL)
		return NULL;
	row = gtk_single_selection_get_selected_item(status_window->selection);
	return (row != NULL && savedstatus_exists(row->status)) ? row->status : NULL;
}

static void
status_window_update_buttons(void)
{
	PurpleSavedStatus *status = status_window_selected();
	gboolean can_use = status != NULL && status != purple_savedstatus_get_current();

	if (status_window == NULL)
		return;

	gtk_widget_set_sensitive(status_window->use_button, can_use);
	gtk_widget_set_sensitive(status_window->modify_button, status != NULL);
	gtk_widget_set_sensitive(status_window->duplicate_button, status != NULL);
	gtk_widget_set_sensitive(status_window->delete_button, can_use);
}

static gint
saved_status_compare_func(gconstpointer a, gconstpointer b)
{
	const char *ta = purple_savedstatus_get_title((PurpleSavedStatus *)a);
	const char *tb = purple_savedstatus_get_title((PurpleSavedStatus *)b);
	char *ka = g_utf8_casefold(ta ? ta : "", -1);
	char *kb = g_utf8_casefold(tb ? tb : "", -1);
	gint ret = g_utf8_collate(ka, kb);

	g_free(ka);
	g_free(kb);
	return ret;
}

static void
populate_saved_status_list(StatusWindow *dialog)
{
	GList *statuses = NULL, *l;
	GPtrArray *rows = g_ptr_array_new_with_free_func(g_object_unref);
	PurpleSavedStatus *selected = status_window_selected();
	guint i, sel_pos = GTK_INVALID_LIST_POSITION;

	for (l = purple_savedstatuses_get_all(); l != NULL; l = l->next)
		if (!purple_savedstatus_is_transient(l->data))
			statuses = g_list_prepend(statuses, l->data);
	statuses = g_list_sort(statuses, saved_status_compare_func);

	for (l = statuses, i = 0; l != NULL; l = l->next, i++) {
		PidginSavedStatusRow *row = g_object_new(PIDGIN_TYPE_SAVED_STATUS_ROW, NULL);
		row->status = l->data;
		g_ptr_array_add(rows, row);
		if (l->data == selected)
			sel_pos = i;
	}
	g_list_free(statuses);

	g_list_store_splice(dialog->store, 0, g_list_model_get_n_items(G_LIST_MODEL(dialog->store)),
	                    rows->pdata, rows->len);
	g_ptr_array_unref(rows);
	gtk_single_selection_set_selected(dialog->selection, sel_pos);
	status_window_update_buttons();
}

static void
saved_status_updated_cb(PurpleSavedStatus *status, StatusWindow *sw)
{
	populate_saved_status_list(sw);
}

static void
current_status_changed(PurpleSavedStatus *old, PurpleSavedStatus *new_status,
                       StatusWindow *dialog)
{
	status_window_update_buttons();
}

static void
status_window_use_cb(GtkButton *button, gpointer data)
{
	PurpleSavedStatus *status = status_window_selected();

	if (status != NULL)
		purple_savedstatus_activate(status);
}

static void
status_window_add_cb(GtkButton *button, gpointer data)
{
	pidgin_status_editor_show(FALSE, NULL);
}

static void
status_window_modify_cb(GtkButton *button, gpointer data)
{
	PurpleSavedStatus *status = status_window_selected();

	if (status != NULL)
		pidgin_status_editor_show(TRUE, status);
}

static void
status_window_duplicate_cb(GtkButton *button, gpointer data)
{
	PurpleSavedStatus *status = status_window_selected();

	if (status != NULL)
		pidgin_status_editor_show(FALSE, status);
}

static void
status_activate_cb(GtkColumnView *view, guint position, gpointer data)
{
	PidginSavedStatusRow *row = g_list_model_get_item(G_LIST_MODEL(status_window->store), position);

	if (row != NULL && savedstatus_exists(row->status))
		pidgin_status_editor_show(TRUE, row->status);
	g_clear_object(&row);
}

static void
delete_response_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	char *title = data;
	int button = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, NULL);

	if (button == 1 && purple_savedstatus_find(title) != NULL &&
	    purple_savedstatus_find(title) != purple_savedstatus_get_current())
		purple_savedstatus_delete(title);
	g_free(title);
}

static void
status_window_delete_cb(GtkButton *button, gpointer data)
{
	PurpleSavedStatus *status = status_window_selected();
	GtkAlertDialog *alert;
	const char *buttons[] = { _("_Cancel"), _("_Delete"), NULL };

	if (status == NULL)
		return;

	alert = gtk_alert_dialog_new(_("Are you sure you want to delete %s?"),
	                             purple_savedstatus_get_title(status));
	gtk_alert_dialog_set_buttons(alert, buttons);
	gtk_alert_dialog_set_cancel_button(alert, 0);
	gtk_alert_dialog_set_default_button(alert, 0);
	gtk_alert_dialog_choose(alert, GTK_WINDOW(status_window->window),
	                        status_window->cancellable, delete_response_cb,
	                        g_strdup(purple_savedstatus_get_title(status)));
	g_object_unref(alert);
}

static void
status_window_close_cb(GtkButton *button, gpointer data)
{
	pidgin_status_window_hide();
}

static void
status_selection_changed_cb(GtkSelectionModel *model, guint position, guint n_items,
                            gpointer data)
{
	status_window_update_buttons();
}

static void
label_setup_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	GtkWidget *label = gtk_label_new(NULL);

	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_list_item_set_child(li, label);
}

static void
title_bind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	PidginSavedStatusRow *row = gtk_list_item_get_item(li);

	if (savedstatus_exists(row->status))
		gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(li)),
		                   purple_savedstatus_get_title(row->status));
}

static void
type_bind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	PidginSavedStatusRow *row = gtk_list_item_get_item(li);
	PurpleStatusPrimitive prim;

	if (!savedstatus_exists(row->status))
		return;
	prim = purple_savedstatus_get_type(row->status);
	icon_label_set(li, NULL, primitive_icon_name(prim), purple_primitive_get_name_from_type(prim));
}

static void
message_bind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	PidginSavedStatusRow *row = gtk_list_item_get_item(li);
	GtkWidget *label = gtk_list_item_get_child(li);
	char *plain;

	if (!savedstatus_exists(row->status))
		return;
	plain = html_to_plain_line(purple_savedstatus_get_message(row->status));
	gtk_label_set_text(GTK_LABEL(label), plain);
	gtk_widget_set_tooltip_text(label, *plain ? plain : NULL);
	g_free(plain);
}

static gboolean
status_window_close_request_cb(GtkWindow *window, gpointer data)
{
	int width, height;

	gtk_window_get_default_size(window, &width, &height);
	if (width > 0 && height > 0) {
		purple_prefs_set_int(PREFS_DIALOG "/width", width);
		purple_prefs_set_int(PREFS_DIALOG "/height", height);
	}
	return FALSE;
}

static void
status_window_destroy_cb(GtkWidget *window, gpointer data)
{
	StatusWindow *dialog = status_window;

	if (dialog == NULL)
		return;

	status_window = NULL;
	purple_request_close_with_handle(dialog);
	purple_notify_close_with_handle(dialog);
	purple_signals_disconnect_by_handle(dialog);
	g_cancellable_cancel(dialog->cancellable);
	g_clear_object(&dialog->cancellable);
	g_clear_object(&dialog->store);
	g_clear_object(&dialog->selection);
	g_free(dialog);
}

void
pidgin_status_window_show(void)
{
	StatusWindow *dialog;
	GtkWidget *win, *vbox, *sw;

	if (status_window != NULL) {
		gtk_window_present(GTK_WINDOW(status_window->window));
		return;
	}

	status_window = dialog = g_new0(StatusWindow, 1);
	dialog->cancellable = g_cancellable_new();

	dialog->window = win = pidgin_dialog_new(_("Saved Statuses"), NULL, "statuses", TRUE);
	gtk_window_set_default_size(GTK_WINDOW(win),
		purple_prefs_get_int(PREFS_DIALOG "/width"),
		purple_prefs_get_int(PREFS_DIALOG "/height"));
	g_signal_connect(win, "close-request", G_CALLBACK(status_window_close_request_cb), NULL);
	g_signal_connect(win, "destroy", G_CALLBACK(status_window_destroy_cb), NULL);

	vbox = pidgin_dialog_get_content_area(win);

	dialog->store = g_list_store_new(PIDGIN_TYPE_SAVED_STATUS_ROW);
	dialog->selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(dialog->store)));
	gtk_single_selection_set_autoselect(dialog->selection, FALSE);
	gtk_single_selection_set_can_unselect(dialog->selection, TRUE);
	g_signal_connect(dialog->selection, "selection-changed",
	                 G_CALLBACK(status_selection_changed_cb), NULL);

	dialog->columnview = gtk_column_view_new(
		GTK_SELECTION_MODEL(g_object_ref(dialog->selection)));
	g_signal_connect(dialog->columnview, "activate", G_CALLBACK(status_activate_cb), NULL);

	add_column(dialog->columnview, _("Title"), G_CALLBACK(label_setup_cb),
	           G_CALLBACK(title_bind_cb), NULL, TRUE);
	add_column(dialog->columnview, _("Type"), G_CALLBACK(icon_label_setup_cb),
	           G_CALLBACK(type_bind_cb), NULL, FALSE);
	add_column(dialog->columnview, _("Message"), G_CALLBACK(label_setup_cb),
	           G_CALLBACK(message_bind_cb), NULL, TRUE);

	sw = pidgin_make_scrollable(dialog->columnview, GTK_POLICY_AUTOMATIC,
	                            GTK_POLICY_AUTOMATIC, -1, -1);
	gtk_widget_set_vexpand(sw, TRUE);
	gtk_box_append(GTK_BOX(vbox), sw);

	dialog->use_button = pidgin_dialog_add_button(win, _("_Use"),
		G_CALLBACK(status_window_use_cb), NULL);
	pidgin_dialog_add_button(win, _("_Add"), G_CALLBACK(status_window_add_cb), NULL);
	dialog->modify_button = pidgin_dialog_add_button(win, _("_Modify"),
		G_CALLBACK(status_window_modify_cb), NULL);
	dialog->duplicate_button = pidgin_dialog_add_button(win, _("D_uplicate"),
		G_CALLBACK(status_window_duplicate_cb), NULL);
	dialog->delete_button = pidgin_dialog_add_button(win, _("_Delete"),
		G_CALLBACK(status_window_delete_cb), NULL);
	pidgin_dialog_add_button(win, _("_Close"), G_CALLBACK(status_window_close_cb), NULL);

	purple_signal_connect(purple_savedstatuses_get_handle(),
			"savedstatus-changed", dialog,
			PURPLE_CALLBACK(current_status_changed), dialog);
	purple_signal_connect(purple_savedstatuses_get_handle(),
			"savedstatus-added", dialog,
			PURPLE_CALLBACK(saved_status_updated_cb), dialog);
	purple_signal_connect(purple_savedstatuses_get_handle(),
			"savedstatus-deleted", dialog,
			PURPLE_CALLBACK(saved_status_updated_cb), dialog);
	purple_signal_connect(purple_savedstatuses_get_handle(),
			"savedstatus-modified", dialog,
			PURPLE_CALLBACK(saved_status_updated_cb), dialog);

	populate_saved_status_list(dialog);

	gtk_window_present(GTK_WINDOW(win));
}

void
pidgin_status_window_hide(void)
{
	if (status_window == NULL)
		return;

	status_window_close_request_cb(GTK_WINDOW(status_window->window), NULL);
	/* The destroy handler frees status_window. */
	gtk_window_destroy(GTK_WINDOW(status_window->window));
}

/**************************************************************************
 * The status editor
 **************************************************************************/

static void edit_substatus(StatusEditor *status_editor, PurpleAccount *account);

static void
substatus_editor_destroy_window(gpointer data)
{
	SubStatusEditor *sub = data;

	/* Its destroy handler removes it from the hash table (already being
	 * cleared here), so detach it first. */
	sub->status_editor = NULL;
	gtk_window_destroy(GTK_WINDOW(sub->window));
}

static void
status_editor_destroy_cb(GtkWidget *widget, StatusEditor *dialog)
{
	status_editors = g_list_remove(status_editors, dialog);
	g_clear_handle_id(&dialog->close_idle, g_source_remove);

	/* Close any substatus editors that may be open */
	g_hash_table_destroy(dialog->substatus_editors);

	g_free(dialog->original_title);
	g_clear_object(&dialog->store);
	g_clear_object(&dialog->selection);
	g_free(dialog);
}

static gboolean
status_editor_close_idle_cb(gpointer data)
{
	StatusEditor *dialog = data;

	dialog->close_idle = 0;
	gtk_window_destroy(GTK_WINDOW(dialog->window));
	return G_SOURCE_REMOVE;
}

static void
status_editor_close(StatusEditor *dialog)
{
	gtk_widget_set_visible(dialog->window, FALSE);
	if (dialog->close_idle == 0)
		dialog->close_idle = g_idle_add(status_editor_close_idle_cb, dialog);
}

static void
status_editor_cancel_cb(GtkButton *button, StatusEditor *dialog)
{
	status_editor_close(dialog);
}

static PurpleStatusPrimitive
status_editor_get_type(StatusEditor *dialog)
{
	return GPOINTER_TO_INT(pidgin_item_dropdown_get_selected_data(dialog->type));
}

static void
status_editor_ok_cb(GtkButton *button, StatusEditor *dialog)
{
	const char *title;
	PurpleStatusPrimitive type;
	char *message, *unformatted;
	PurpleSavedStatus *saved_status = NULL;
	gboolean save = (GTK_WIDGET(button) == dialog->saveanduse_button) ||
	                (GTK_WIDGET(button) == dialog->save_button);
	guint i, n;

	if (dialog->close_idle != 0)
		return;

	title = gtk_editable_get_text(GTK_EDITABLE(dialog->title));

	/*
	 * If we're saving this status, and the title is already taken
	 * then show an error dialog and don't do anything.
	 */
	if (save && (purple_savedstatus_find(title) != NULL) &&
		((dialog->original_title == NULL) || (!purple_strequal(title, dialog->original_title))))
	{
		purple_notify_error(status_window, NULL, _("Title already in use.  You must "
						  "choose a unique title."), NULL);
		return;
	}

	type = status_editor_get_type(dialog);
	message = pidgin_compose_entry_get_markup(PIDGIN_COMPOSE_ENTRY(dialog->message));
	unformatted = purple_markup_strip_html(message);

	/*
	 * If we're editing an old status, then lookup the old status.
	 * Note: It is possible that it has been deleted or renamed
	 *       or something, and no longer exists.
	 */
	if (dialog->original_title != NULL)
		saved_status = purple_savedstatus_find(dialog->original_title);

	if (saved_status == NULL)
	{
		/* This is a new status */
		if (save)
			saved_status = purple_savedstatus_new(title, type);
		else
			saved_status = purple_savedstatus_new(NULL, type);
	}
	else
	{
		/* Modify the old status */
		if (!purple_strequal(title, dialog->original_title))
			purple_savedstatus_set_title(saved_status, title);
		purple_savedstatus_set_type(saved_status, type);
	}

	if (unformatted == NULL || *unformatted == '\0')
		purple_savedstatus_set_message(saved_status, NULL);
	else
		purple_savedstatus_set_message(saved_status, message);

	/* Set any substatuses */
	n = g_list_model_get_n_items(G_LIST_MODEL(dialog->store));
	for (i = 0; i < n; i++) {
		PidginSubstatusRow *row = g_list_model_get_item(G_LIST_MODEL(dialog->store), i);

		if (account_exists(row->account)) {
			if (row->enabled) {
				PurpleStatusType *stype = purple_account_get_status_type(row->account, row->id);
				if (stype != NULL)
					purple_savedstatus_set_substatus(saved_status, row->account,
					                                 stype, row->message);
			} else {
				purple_savedstatus_unset_substatus(saved_status, row->account);
			}
		}
		g_object_unref(row);
	}

	g_free(message);
	g_free(unformatted);

	/* If they clicked on "Save and Use" or "Use," then activate the status */
	if (GTK_WIDGET(button) != dialog->save_button)
		purple_savedstatus_activate(saved_status);

	status_editor_close(dialog);
}

static void
editor_title_changed_cb(GtkEditable *entry, StatusEditor *dialog)
{
	gboolean ok = *gtk_editable_get_text(entry) != '\0';

	gtk_widget_set_sensitive(dialog->saveanduse_button, ok);
	gtk_widget_set_sensitive(dialog->save_button, ok);
}

static GtkWidget *
create_status_type_menu(PurpleStatusPrimitive type)
{
	GListStore *store = g_list_store_new(PIDGIN_TYPE_ITEM);
	GtkWidget *dropdown;
	int i;

	for (i = PURPLE_STATUS_UNSET + 1; i < PURPLE_STATUS_NUM_PRIMITIVES; i++)
	{
		PidginItem *item;
		GIcon *icon;

		/*
		 * Special-case these.  They're intended to be independent
		 * status types, so don't show them in the list.
		 */
		if (i == PURPLE_STATUS_MOBILE ||
		    i == PURPLE_STATUS_MOOD ||
		    i == PURPLE_STATUS_TUNE)
			continue;

		item = pidgin_item_new(purple_primitive_get_name_from_type(i),
		                       purple_primitive_get_id_from_type(i), GINT_TO_POINTER(i));
		icon = g_themed_icon_new(primitive_icon_name(i));
		pidgin_item_set_icon(item, icon);
		g_object_unref(icon);
		g_list_store_append(store, item);
		g_object_unref(item);
	}

	dropdown = pidgin_item_dropdown_new(G_LIST_MODEL(store));
	pidgin_item_dropdown_select_data(dropdown, GINT_TO_POINTER(type));
	return dropdown;
}

static guint
substatus_find_account(StatusEditor *dialog, PurpleAccount *account)
{
	guint i, n = g_list_model_get_n_items(G_LIST_MODEL(dialog->store));

	for (i = 0; i < n; i++) {
		PidginSubstatusRow *row = g_list_model_get_item(G_LIST_MODEL(dialog->store), i);
		gboolean match = row->account == account;

		g_object_unref(row);
		if (match)
			return i;
	}
	return GTK_INVALID_LIST_POSITION;
}

static PidginSubstatusRow *
substatus_get_row(StatusEditor *dialog, PurpleAccount *account)
{
	guint pos = substatus_find_account(dialog, account);

	if (pos == GTK_INVALID_LIST_POSITION)
		return NULL;
	return g_list_model_get_item(G_LIST_MODEL(dialog->store), pos);
}

static void
substatus_replace_row(StatusEditor *dialog, PidginSubstatusRow *row)
{
	guint pos = substatus_find_account(dialog, row->account);

	if (pos != GTK_INVALID_LIST_POSITION)
		g_list_store_splice(dialog->store, pos, 1, (gpointer *)&row, 1);
}

static PidginSubstatusRow *
substatus_row_for(PurpleAccount *account, PurpleSavedStatusSub *substatus)
{
	const char *id = NULL, *name = NULL, *message = NULL;
	PurpleStatusPrimitive prim = PURPLE_STATUS_UNSET;

	if (substatus != NULL)
	{
		const PurpleStatusType *type;

		type = purple_savedstatus_substatus_get_type(substatus);
		id = purple_status_type_get_id(type);
		name = purple_status_type_get_name(type);
		prim = purple_status_type_get_primitive(type);
		if (purple_status_type_get_attr(type, "message"))
			message = purple_savedstatus_substatus_get_message(substatus);
	}

	return substatus_row_new(account, substatus != NULL, id, name, message, prim);
}

static void
status_editor_populate_list(StatusEditor *dialog, PurpleSavedStatus *saved_status)
{
	GList *iter;

	g_list_store_remove_all(dialog->store);

	for (iter = purple_accounts_get_all(); iter != NULL; iter = iter->next)
	{
		PurpleAccount *account = (PurpleAccount *)iter->data;
		PurpleSavedStatusSub *substatus = NULL;
		PidginSubstatusRow *row;

		if (saved_status != NULL)
			substatus = purple_savedstatus_get_substatus(saved_status, account);

		row = substatus_row_for(account, substatus);
		g_list_store_append(dialog->store, row);
		g_object_unref(row);
	}
}

/* The "Different" column */
static void
substatus_toggled_cb(GtkCheckButton *check, StatusEditor *dialog)
{
	PurpleAccount *account = g_object_get_data(G_OBJECT(check), "pidgin-account");
	PidginSubstatusRow *row;

	if (!account_exists(account))
		return;

	if (gtk_check_button_get_active(check))
	{
		/* Stays off until the substatus editor's OK. */
		g_signal_handlers_block_by_func(check, substatus_toggled_cb, dialog);
		gtk_check_button_set_active(check, FALSE);
		g_signal_handlers_unblock_by_func(check, substatus_toggled_cb, dialog);
		edit_substatus(dialog, account);
	}
	else
	{
		/* Remove the substatus */
		row = substatus_row_new(account, FALSE, NULL, NULL, NULL, PURPLE_STATUS_UNSET);
		substatus_replace_row(dialog, row);
		g_object_unref(row);
	}
}

static void
enabled_setup_cb(GtkSignalListItemFactory *factory, GtkListItem *li, StatusEditor *dialog)
{
	GtkWidget *check = gtk_check_button_new();

	gtk_widget_set_halign(check, GTK_ALIGN_CENTER);
	gtk_accessible_update_property(GTK_ACCESSIBLE(check),
		GTK_ACCESSIBLE_PROPERTY_LABEL, _("Different"), -1);
	g_signal_connect(check, "toggled", G_CALLBACK(substatus_toggled_cb), dialog);
	gtk_list_item_set_child(li, check);
}

static void
enabled_bind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, StatusEditor *dialog)
{
	PidginSubstatusRow *row = gtk_list_item_get_item(li);
	GtkWidget *check = gtk_list_item_get_child(li);

	g_signal_handlers_block_by_func(check, substatus_toggled_cb, dialog);
	g_object_set_data(G_OBJECT(check), "pidgin-account", row->account);
	gtk_check_button_set_active(GTK_CHECK_BUTTON(check), row->enabled);
	g_signal_handlers_unblock_by_func(check, substatus_toggled_cb, dialog);
}

static void
username_bind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, StatusEditor *dialog)
{
	PidginSubstatusRow *row = gtk_list_item_get_item(li);
	GtkWidget *image = gtk_widget_get_first_child(gtk_list_item_get_child(li));
	GIcon *icon;

	if (!account_exists(row->account))
		return;
	icon = pidgin_create_prpl_gicon(row->account, NULL);
	icon_label_set(li, icon, NULL, purple_account_get_username(row->account));
	g_object_unref(icon);
	/* Pidgin 2 desaturated the icon of disconnected accounts. */
	gtk_widget_set_opacity(image, purple_account_is_connected(row->account) ? 1.0 : 0.5);
}

static void
substatus_bind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, StatusEditor *dialog)
{
	PidginSubstatusRow *row = gtk_list_item_get_item(li);

	if (row->enabled)
		icon_label_set(li, NULL, primitive_icon_name(row->primitive), row->name);
	else
		icon_label_set(li, NULL, NULL, NULL);
}

static void
submessage_bind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, StatusEditor *dialog)
{
	PidginSubstatusRow *row = gtk_list_item_get_item(li);
	char *plain = html_to_plain_line(row->enabled ? row->message : NULL);

	gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(li)), plain);
	g_free(plain);
}

static void
substatus_activate_cb(GtkColumnView *view, guint position, StatusEditor *dialog)
{
	PidginSubstatusRow *row = g_list_model_get_item(G_LIST_MODEL(dialog->store), position);

	if (row != NULL && account_exists(row->account))
		edit_substatus(dialog, row->account);
	g_clear_object(&row);
}

static StatusEditor *
find_editor(const char *original_title)
{
	GList *l;

	for (l = status_editors; l != NULL; l = l->next) {
		StatusEditor *dialog = l->data;
		if (dialog->close_idle == 0 && dialog->original_title != NULL &&
		    purple_strequal(dialog->original_title, original_title))
			return dialog;
	}
	return NULL;
}

void
pidgin_status_editor_show(gboolean edit, PurpleSavedStatus *saved_status)
{
	StatusEditor *dialog;
	GtkSizeGroup *sg;
	GtkWidget *win, *vbox, *hbox, *label, *frame, *expander, *dbox, *sw, *button;

	if (edit)
	{
		g_return_if_fail(saved_status != NULL);
		g_return_if_fail(!purple_savedstatus_is_transient(saved_status));

		/* Find a possible window for this saved status and present it */
		dialog = find_editor(purple_savedstatus_get_title(saved_status));
		if (dialog != NULL) {
			gtk_window_present(GTK_WINDOW(dialog->window));
			return;
		}
	}

	dialog = g_new0(StatusEditor, 1);
	dialog->substatus_editors = g_hash_table_new_full(g_direct_hash, g_direct_equal,
		NULL, substatus_editor_destroy_window);
	status_editors = g_list_prepend(status_editors, dialog);

	if (edit)
		dialog->original_title = g_strdup(purple_savedstatus_get_title(saved_status));

	dialog->window = win = pidgin_dialog_new(_("Status"), pidgin_get_active_window(),
	                                         "status", TRUE);
	gtk_window_set_default_size(GTK_WINDOW(win), 480, -1);
	g_signal_connect(win, "destroy", G_CALLBACK(status_editor_destroy_cb), dialog);

	vbox = pidgin_dialog_get_content_area(win);
	sg = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);

	/* Title */
	dialog->title = gtk_entry_new();
	if ((saved_status != NULL)
			&& !purple_savedstatus_is_transient(saved_status)
			&& (purple_savedstatus_get_title(saved_status) != NULL))
		gtk_editable_set_text(GTK_EDITABLE(dialog->title),
		                      purple_savedstatus_get_title(saved_status));
	pidgin_add_widget_to_vbox(GTK_BOX(vbox), _("_Title:"), sg, dialog->title, TRUE, NULL);

	/* Status type */
	dialog->type = create_status_type_menu(saved_status != NULL
		? purple_savedstatus_get_type(saved_status) : PURPLE_STATUS_AWAY);
	pidgin_add_widget_to_vbox(GTK_BOX(vbox), _("_Status:"), sg, dialog->type, TRUE, NULL);

	/* Status message */
	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	gtk_widget_set_vexpand(hbox, TRUE);
	label = gtk_label_new_with_mnemonic(_("_Message:"));
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_widget_set_valign(label, GTK_ALIGN_START);
	gtk_size_group_add_widget(sg, label);
	gtk_box_append(GTK_BOX(hbox), label);
	frame = pidgin_create_compose_entry(PURPLE_CONNECTION_HTML, TRUE, &dialog->message, NULL);
	gtk_widget_set_hexpand(frame, TRUE);
	gtk_widget_set_size_request(frame, -1, 100);
	gtk_box_append(GTK_BOX(hbox), frame);
	gtk_label_set_mnemonic_widget(GTK_LABEL(label), dialog->message);
	gtk_box_append(GTK_BOX(vbox), hbox);
	g_object_unref(sg);

	pidgin_compose_entry_set_return_inserts_newline(PIDGIN_COMPOSE_ENTRY(dialog->message), TRUE);
	if ((saved_status != NULL) && (purple_savedstatus_get_message(saved_status) != NULL))
		pidgin_compose_entry_set_markup(PIDGIN_COMPOSE_ENTRY(dialog->message),
		                                purple_savedstatus_get_message(saved_status));

	/* Different status message expander */
	expander = gtk_expander_new_with_mnemonic(_("Use a _different status for some accounts"));
	gtk_box_append(GTK_BOX(vbox), expander);

	dbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_CAT_SPACE);
	gtk_expander_set_child(GTK_EXPANDER(expander), dbox);

	dialog->store = g_list_store_new(PIDGIN_TYPE_SUBSTATUS_ROW);
	dialog->selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(dialog->store)));
	gtk_single_selection_set_autoselect(dialog->selection, FALSE);
	gtk_single_selection_set_can_unselect(dialog->selection, TRUE);
	dialog->columnview = gtk_column_view_new(
		GTK_SELECTION_MODEL(g_object_ref(dialog->selection)));
	g_signal_connect(dialog->columnview, "activate", G_CALLBACK(substatus_activate_cb), dialog);

	add_column(dialog->columnview, _("Different"), G_CALLBACK(enabled_setup_cb),
	           G_CALLBACK(enabled_bind_cb), dialog, FALSE);
	add_column(dialog->columnview, _("Username"), G_CALLBACK(icon_label_setup_cb),
	           G_CALLBACK(username_bind_cb), dialog, TRUE);
	add_column(dialog->columnview, _("Status"), G_CALLBACK(icon_label_setup_cb),
	           G_CALLBACK(substatus_bind_cb), dialog, FALSE);
	add_column(dialog->columnview, _("Message"), G_CALLBACK(label_setup_cb),
	           G_CALLBACK(submessage_bind_cb), dialog, TRUE);

	sw = pidgin_make_scrollable(dialog->columnview, GTK_POLICY_AUTOMATIC,
	                            GTK_POLICY_AUTOMATIC, -1, 150);
	gtk_widget_set_vexpand(sw, TRUE);
	gtk_box_append(GTK_BOX(dbox), sw);

	status_editor_populate_list(dialog, saved_status);

	/* Expand the list if we have substatuses */
	gtk_expander_set_expanded(GTK_EXPANDER(expander),
		(saved_status != NULL) && purple_savedstatus_has_substatuses(saved_status));

	/* Buttons */
	pidgin_dialog_add_button(win, _("_Cancel"), G_CALLBACK(status_editor_cancel_cb), dialog);
	dialog->use_button = pidgin_dialog_add_button(win, _("_Use"),
		G_CALLBACK(status_editor_ok_cb), dialog);
	dialog->saveanduse_button = pidgin_dialog_add_button(win, _("Sa_ve and Use"),
		G_CALLBACK(status_editor_ok_cb), dialog);
	dialog->save_button = button = pidgin_dialog_add_button(win, _("_Save"),
		G_CALLBACK(status_editor_ok_cb), dialog);
	gtk_window_set_default_widget(GTK_WINDOW(win), button);

	g_signal_connect(dialog->title, "changed", G_CALLBACK(editor_title_changed_cb), dialog);
	editor_title_changed_cb(GTK_EDITABLE(dialog->title), dialog);

	gtk_window_present(GTK_WINDOW(win));
}

/**************************************************************************
 * The substatus editor
 **************************************************************************/

static void
substatus_selection_changed_cb(GObject *dropdown, GParamSpec *pspec, SubStatusEditor *select)
{
	const char *id = pidgin_item_dropdown_get_selected_id(select->box);
	PurpleStatusType *type;

	if (id == NULL || !account_exists(select->account))
		return;
	type = purple_account_get_status_type(select->account, id);

	gtk_widget_set_sensitive(select->message_box,
		type != NULL && purple_status_type_get_attr(type, "message") != NULL);
}

static void
substatus_editor_destroy_cb(GtkWidget *widget, SubStatusEditor *dialog)
{
	if (dialog->status_editor != NULL)
		g_hash_table_steal(dialog->status_editor->substatus_editors, dialog->account);
	g_free(dialog);
}

static void
substatus_editor_cancel_cb(GtkButton *button, SubStatusEditor *dialog)
{
	gtk_window_destroy(GTK_WINDOW(dialog->window));
}

static void
substatus_editor_ok_cb(GtkButton *button, SubStatusEditor *dialog)
{
	StatusEditor *status_editor = dialog->status_editor;
	PurpleStatusType *type;
	const char *id;
	char *message = NULL;
	PidginSubstatusRow *row;

	id = pidgin_item_dropdown_get_selected_id(dialog->box);
	if (id == NULL || status_editor == NULL || !account_exists(dialog->account))
	{
		gtk_window_destroy(GTK_WINDOW(dialog->window));
		return;
	}

	type = purple_account_get_status_type(dialog->account, id);
	if (type == NULL) {
		gtk_window_destroy(GTK_WINDOW(dialog->window));
		return;
	}
	if (purple_status_type_get_attr(type, "message") != NULL)
		message = pidgin_compose_entry_get_markup(PIDGIN_COMPOSE_ENTRY(dialog->message));

	row = substatus_row_new(dialog->account, TRUE, id, purple_status_type_get_name(type),
	                        message, purple_status_type_get_primitive(type));
	substatus_replace_row(status_editor, row);
	g_object_unref(row);
	g_free(message);

	gtk_window_destroy(GTK_WINDOW(dialog->window));
}

static void
edit_substatus(StatusEditor *status_editor, PurpleAccount *account)
{
	SubStatusEditor *dialog;
	GtkSizeGroup *sg;
	GtkWidget *win, *vbox, *hbox, *label, *frame;
	GListStore *store;
	PidginSubstatusRow *row;
	const char *status_id = NULL;
	const char *message = NULL;
	GList *list;
	char *tmp;

	g_return_if_fail(status_editor != NULL);
	g_return_if_fail(account       != NULL);

	dialog = g_hash_table_lookup(status_editor->substatus_editors, account);
	if (dialog != NULL)
	{
		gtk_window_present(GTK_WINDOW(dialog->window));
		return;
	}

	dialog = g_new0(SubStatusEditor, 1);
	dialog->status_editor = status_editor;
	dialog->account = account;
	g_hash_table_insert(status_editor->substatus_editors, account, dialog);

	tmp = g_strdup_printf(_("Status for %s"), purple_account_get_username(account));
	dialog->window = win = pidgin_dialog_new(tmp, GTK_WINDOW(status_editor->window),
	                                         "substatus", TRUE);
	g_free(tmp);
	g_signal_connect(win, "destroy", G_CALLBACK(substatus_editor_destroy_cb), dialog);

	vbox = pidgin_dialog_get_content_area(win);
	sg = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);

	/* Seed the input widgets with the current values: from the parent
	 * editor's list if it has a substatus for the account, else from the
	 * saved status being edited. */
	row = substatus_get_row(status_editor, account);
	if (row != NULL && row->enabled) {
		status_id = row->id;
		message = row->message;
	} else if (status_editor->original_title != NULL) {
		PurpleSavedStatus *saved_status;
		PurpleSavedStatusSub *substatus;

		if ((saved_status = purple_savedstatus_find(status_editor->original_title)) != NULL &&
		    (substatus = purple_savedstatus_get_substatus(saved_status, account)) != NULL) {
			message = purple_savedstatus_substatus_get_message(substatus);
			status_id = purple_status_type_get_id(purple_savedstatus_substatus_get_type(substatus));
		}
	}

	/* Status type */
	store = g_list_store_new(PIDGIN_TYPE_ITEM);
	for (list = purple_account_get_status_types(account); list; list = list->next)
	{
		PurpleStatusType *status_type = list->data;
		PidginItem *item;
		GIcon *icon;

		/*
		 * Only allow users to select statuses that are flagged as
		 * "user settable" and that aren't independent.
		 */
		if (!purple_status_type_is_user_settable(status_type) ||
				purple_status_type_is_independent(status_type))
			continue;

		item = pidgin_item_new(purple_status_type_get_name(status_type),
		                       purple_status_type_get_id(status_type), NULL);
		icon = g_themed_icon_new(primitive_icon_name(
			purple_status_type_get_primitive(status_type)));
		pidgin_item_set_icon(item, icon);
		g_object_unref(icon);
		g_list_store_append(store, item);
		g_object_unref(item);
	}
	dialog->box = pidgin_item_dropdown_new(G_LIST_MODEL(store));
	if (status_id != NULL)
		pidgin_item_dropdown_select_id(dialog->box, status_id);
	pidgin_add_widget_to_vbox(GTK_BOX(vbox), _("_Status:"), sg, dialog->box, FALSE, NULL);

	/* Status message */
	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	gtk_widget_set_vexpand(hbox, TRUE);
	label = gtk_label_new_with_mnemonic(_("_Message:"));
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_widget_set_valign(label, GTK_ALIGN_START);
	gtk_size_group_add_widget(sg, label);
	gtk_box_append(GTK_BOX(hbox), label);
	dialog->message_box = frame = pidgin_create_compose_entry(PURPLE_CONNECTION_HTML, TRUE,
		&dialog->message, NULL);
	gtk_widget_set_hexpand(frame, TRUE);
	gtk_widget_set_size_request(frame, 320, 100);
	gtk_box_append(GTK_BOX(hbox), frame);
	gtk_label_set_mnemonic_widget(GTK_LABEL(label), dialog->message);
	gtk_box_append(GTK_BOX(vbox), hbox);
	g_object_unref(sg);

	pidgin_compose_entry_set_return_inserts_newline(PIDGIN_COMPOSE_ENTRY(dialog->message), TRUE);
	if (message)
		pidgin_compose_entry_set_markup(PIDGIN_COMPOSE_ENTRY(dialog->message), message);
	g_clear_object(&row);

	pidgin_dialog_add_button(win, _("_Cancel"), G_CALLBACK(substatus_editor_cancel_cb), dialog);
	pidgin_dialog_add_button(win, _("_OK"), G_CALLBACK(substatus_editor_ok_cb), dialog);

	g_signal_connect(dialog->box, "notify::selected",
	                 G_CALLBACK(substatus_selection_changed_cb), dialog);
	substatus_selection_changed_cb(G_OBJECT(dialog->box), NULL, dialog);

	gtk_window_present(GTK_WINDOW(win));
}

/**************************************************************************
 * GTK+ saved status glue
 **************************************************************************/

void *
pidgin_status_get_handle(void)
{
	static int handle;

	return &handle;
}

void
pidgin_status_init(void)
{
	/* The window's size (Pidgin 2 kept it in /pidgin/status/dialog). */
	purple_prefs_add_none("/pidgin4");
	purple_prefs_add_none(PREFS_DIALOG);
	purple_prefs_add_int(PREFS_DIALOG "/width",  550);
	purple_prefs_add_int(PREFS_DIALOG "/height", 250);
}

void
pidgin_status_uninit(void)
{
	while (status_editors != NULL) {
		StatusEditor *dialog = status_editors->data;
		/* The destroy handler removes it from the list. */
		gtk_window_destroy(GTK_WINDOW(dialog->window));
	}
	pidgin_status_window_hide();
}

/**************************************************************************
 * Selftest
 **************************************************************************/

static void
selftest_close_editors(void)
{
	GList *l, *copy = g_list_copy(status_editors);

	for (l = copy; l != NULL; l = l->next)
		status_editor_cancel_cb(NULL, l->data);
	g_list_free(copy);
	pidgin_selftest_iterate(100);
}

void
pidgin_status_selftest(void)
{
	GList *l;
	guint n_saved = 0, n_rows;
	int n = 0;
	gboolean sub_done = FALSE;

	pidgin_status_window_show();
	pidgin_selftest_iterate(300);
	if (status_window == NULL) {
		pidgin_selftest_fail("status", "the saved statuses window did not open");
		return;
	}

	for (l = purple_savedstatuses_get_all(); l != NULL; l = l->next)
		if (!purple_savedstatus_is_transient(l->data))
			n_saved++;
	n_rows = g_list_model_get_n_items(G_LIST_MODEL(status_window->store));
	pidgin_selftest_log("status", "window: %u rows for %u saved statuses", n_rows, n_saved);
	if (n_rows != n_saved)
		pidgin_selftest_fail("status", "%u rows for %u saved statuses", n_rows, n_saved);

	if (n_rows > 0) {
		gtk_single_selection_set_selected(status_window->selection, 0);
		pidgin_selftest_iterate(50);
		if (!gtk_widget_get_sensitive(status_window->modify_button))
			pidgin_selftest_fail("status", "Modify is not sensitive with a selection");
	}

	for (l = purple_savedstatuses_get_all(); l != NULL; l = l->next) {
		PurpleSavedStatus *status = l->data;
		StatusEditor *dialog;

		if (purple_savedstatus_is_transient(status))
			continue;
		pidgin_status_editor_show(TRUE, status);
		pidgin_selftest_iterate(150);
		dialog = find_editor(purple_savedstatus_get_title(status));
		if (dialog == NULL) {
			pidgin_selftest_fail("status", "no editor for \"%s\"",
			                     purple_savedstatus_get_title(status));
			continue;
		}
		pidgin_selftest_log("status", "editor for \"%s\": open",
		                    purple_savedstatus_get_title(status));
		if (status_editor_get_type(dialog) != purple_savedstatus_get_type(status))
			pidgin_selftest_fail("status", "\"%s\": the type drop-down shows %d, not %d",
				purple_savedstatus_get_title(status), status_editor_get_type(dialog),
				purple_savedstatus_get_type(status));

		/* A substatus editor for the first account, cancelled. */
		if (!sub_done && purple_accounts_get_all() != NULL) {
			PurpleAccount *account = purple_accounts_get_all()->data;

			edit_substatus(dialog, account);
			pidgin_selftest_iterate(150);
			if (g_hash_table_lookup(dialog->substatus_editors, account) == NULL)
				pidgin_selftest_fail("status", "the substatus editor did not open");
			else
				pidgin_selftest_log("status", "substatus editor for %s: open",
				                    purple_account_get_username(account));
			sub_done = TRUE;
		}
		selftest_close_editors();
		if (++n >= 20)
			break;
	}

	/* A new status, and a copy of the current one. */
	pidgin_status_editor_show(FALSE, NULL);
	pidgin_selftest_iterate(150);
	if (status_editors == NULL)
		pidgin_selftest_fail("status", "the new-status editor did not open");
	else
		pidgin_selftest_log("status", "new-status editor: open");
	selftest_close_editors();

	pidgin_status_editor_show(FALSE, purple_savedstatus_get_current());
	pidgin_selftest_iterate(150);
	if (status_editors == NULL)
		pidgin_selftest_fail("status", "the editor for the current status did not open");
	selftest_close_editors();

	pidgin_status_window_hide();
	pidgin_selftest_iterate(100);
	if (status_window != NULL || status_editors != NULL)
		pidgin_selftest_fail("status", "windows left open");
}
