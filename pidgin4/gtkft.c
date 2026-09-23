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
 * File transfers (the port of pidgin/gtkft.c): the PurpleXferUiOps and the
 * File Transfers window, a GtkColumnView of PidginXferRow objects. Each
 * row holds a reference on its PurpleXfer (xfer->ui_data points back at
 * the row, without a reference) and exposes the displayed values as
 * properties, refreshed from update_progress at most ~4 times a second.
 *
 * Incoming file requests are libpurple's purple_request_file/accept,
 * shown by gtkrequest.c. HTTP uploads (XEP-0363, jabber prpl) are plain
 * outgoing transfers that may have no remote user.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "account.h"
#include "debug.h"
#include "ft.h"
#include "notify.h"
#include "prefs.h"
#include "util.h"

#include "gtkft.h"
#include "gtkutils.h"
#include "pidginprefbinding.h"
#include "pidginselftest.h"

#define PREFS_ROOT PIDGIN4_PREFS_ROOT "/filetransfer"

/* At most one refresh of a row per this many microseconds. */
#define UPDATE_INTERVAL_US (250 * G_TIME_SPAN_MILLISECOND)

/**************************************************************************
 * Rows
 **************************************************************************/

#define PIDGIN_TYPE_XFER_ROW (pidgin_xfer_row_get_type())
G_DECLARE_FINAL_TYPE(PidginXferRow, pidgin_xfer_row, PIDGIN, XFER_ROW, GObject)

struct _PidginXferRow {
	GObject parent;
	PurpleXfer *xfer;       /* a reference */

	char *icon_name;
	char *filename;
	char *peer;
	double progress;
	char *size;
	char *speed;
	char *remaining;
	char *status;

	gint64 last_update;
	guint pending_id;       /* a throttled refresh */
	gboolean failed;        /* cancelled by an error, not by a user */
};

enum {
	PROP_0,
	PROP_ICON_NAME,
	PROP_FILENAME,
	PROP_PEER,
	PROP_PROGRESS,
	PROP_SIZE,
	PROP_SPEED,
	PROP_REMAINING,
	PROP_STATUS,
	N_PROPS
};
static GParamSpec *row_props[N_PROPS];

G_DEFINE_TYPE(PidginXferRow, pidgin_xfer_row, G_TYPE_OBJECT)

static void
pidgin_xfer_row_get_property(GObject *obj, guint id, GValue *value, GParamSpec *pspec)
{
	PidginXferRow *row = PIDGIN_XFER_ROW(obj);

	switch (id) {
	case PROP_ICON_NAME: g_value_set_string(value, row->icon_name); break;
	case PROP_FILENAME: g_value_set_string(value, row->filename); break;
	case PROP_PEER: g_value_set_string(value, row->peer); break;
	case PROP_PROGRESS: g_value_set_double(value, row->progress); break;
	case PROP_SIZE: g_value_set_string(value, row->size); break;
	case PROP_SPEED: g_value_set_string(value, row->speed); break;
	case PROP_REMAINING: g_value_set_string(value, row->remaining); break;
	case PROP_STATUS: g_value_set_string(value, row->status); break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
	}
}

static void
pidgin_xfer_row_dispose(GObject *obj)
{
	PidginXferRow *row = PIDGIN_XFER_ROW(obj);

	g_clear_handle_id(&row->pending_id, g_source_remove);
	if (row->xfer != NULL) {
		PurpleXfer *xfer = row->xfer;

		row->xfer = NULL;
		if (xfer->ui_data == row)
			xfer->ui_data = NULL;
		purple_xfer_unref(xfer);
	}
	G_OBJECT_CLASS(pidgin_xfer_row_parent_class)->dispose(obj);
}

static void
pidgin_xfer_row_finalize(GObject *obj)
{
	PidginXferRow *row = PIDGIN_XFER_ROW(obj);

	g_free(row->icon_name);
	g_free(row->filename);
	g_free(row->peer);
	g_free(row->size);
	g_free(row->speed);
	g_free(row->remaining);
	g_free(row->status);
	G_OBJECT_CLASS(pidgin_xfer_row_parent_class)->finalize(obj);
}

static void
pidgin_xfer_row_class_init(PidginXferRowClass *klass)
{
	GObjectClass *obj_class = G_OBJECT_CLASS(klass);
	GParamFlags flags = G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY;

	obj_class->get_property = pidgin_xfer_row_get_property;
	obj_class->dispose = pidgin_xfer_row_dispose;
	obj_class->finalize = pidgin_xfer_row_finalize;

	row_props[PROP_ICON_NAME] = g_param_spec_string("icon-name", NULL, NULL, NULL, flags);
	row_props[PROP_FILENAME] = g_param_spec_string("filename", NULL, NULL, NULL, flags);
	row_props[PROP_PEER] = g_param_spec_string("peer", NULL, NULL, NULL, flags);
	row_props[PROP_PROGRESS] = g_param_spec_double("progress", NULL, NULL,
		0.0, 1.0, 0.0, flags);
	row_props[PROP_SIZE] = g_param_spec_string("size", NULL, NULL, NULL, flags);
	row_props[PROP_SPEED] = g_param_spec_string("speed", NULL, NULL, NULL, flags);
	row_props[PROP_REMAINING] = g_param_spec_string("remaining", NULL, NULL, NULL, flags);
	row_props[PROP_STATUS] = g_param_spec_string("status", NULL, NULL, NULL, flags);
	g_object_class_install_properties(obj_class, N_PROPS, row_props);
}

static void
pidgin_xfer_row_init(PidginXferRow *row)
{
}

static gboolean
xfer_is_finished(PurpleXfer *xfer)
{
	PidginXferRow *row = xfer->ui_data;

	return purple_xfer_is_completed(xfer) || purple_xfer_is_canceled(xfer) ||
	       (row != NULL && PIDGIN_IS_XFER_ROW(row) && row->failed);
}

static gboolean
xfer_is_running(PurpleXfer *xfer)
{
	return !purple_xfer_is_completed(xfer) && !purple_xfer_is_canceled(xfer);
}

static void
row_set_string(PidginXferRow *row, char **field, char *value, guint prop)
{
	if (g_strcmp0(*field, value) != 0) {
		g_free(*field);
		*field = value;
		g_object_notify_by_pspec(G_OBJECT(row), row_props[prop]);
	} else {
		g_free(value);
	}
}

static char *
format_duration(double seconds)
{
	int secs = (int)(seconds + 0.5);

	if (secs >= 3600)
		return g_strdup_printf("%d:%02d:%02d", secs / 3600, (secs / 60) % 60, secs % 60);
	return g_strdup_printf("%d:%02d", secs / 60, secs % 60);
}

/* Recomputes every displayed value from the xfer. */
static void
row_refresh(PidginXferRow *row)
{
	PurpleXfer *xfer = row->xfer;
	PurpleXferType type;
	PurpleAccount *account;
	const char *who, *name, *icon;
	char *speed = NULL, *remaining = NULL, *status, *size, *filename, *peer;
	double progress;
	time_t start;

	if (xfer == NULL)
		return;

	row->last_update = g_get_monotonic_time();
	g_object_freeze_notify(G_OBJECT(row));

	type = purple_xfer_get_type(xfer);
	account = purple_xfer_get_account(xfer);
	who = purple_xfer_get_remote_user(xfer);

	/* Received files by their offered name, sent files by the local
	 * file's name (either may be missing early on). */
	if (type == PURPLE_XFER_RECEIVE || purple_xfer_get_local_filename(xfer) == NULL) {
		name = purple_xfer_get_filename(xfer);
		filename = g_strdup(name != NULL ? name : _("Unknown"));
	} else {
		char *base = g_path_get_basename(purple_xfer_get_local_filename(xfer));
		filename = g_filename_display_name(base);
		g_free(base);
	}
	row_set_string(row, &row->filename, filename, PROP_FILENAME);

	if (who != NULL && *who != '\0')
		peer = g_strdup_printf(type == PURPLE_XFER_RECEIVE ?
			_("Receiving from %s") : _("Sending to %s"), who);
	else if (account != NULL)
		peer = g_strdup_printf(type == PURPLE_XFER_RECEIVE ?
			_("Receiving as %s") : _("Sending as %s"),
			purple_account_get_username(account));
	else
		peer = g_strdup("");
	row_set_string(row, &row->peer, peer, PROP_PEER);

	progress = CLAMP(purple_xfer_get_progress(xfer), 0.0, 1.0);
	if (purple_xfer_is_completed(xfer))
		progress = 1.0;
	if (progress != row->progress) {
		row->progress = progress;
		g_object_notify_by_pspec(G_OBJECT(row), row_props[PROP_PROGRESS]);
	}

	size = purple_xfer_get_size(xfer) > 0 ?
		purple_str_size_to_units(purple_xfer_get_size(xfer)) : g_strdup(_("Unknown"));
	row_set_string(row, &row->size, size, PROP_SIZE);

	start = purple_xfer_get_start_time(xfer);
	if (xfer_is_running(xfer) && start > 0 &&
	    purple_xfer_get_status(xfer) == PURPLE_XFER_STATUS_STARTED) {
		double elapsed = difftime(time(NULL), start);
		double bps = elapsed > 0 ? purple_xfer_get_bytes_sent(xfer) / elapsed : 0;

		if (bps > 0) {
			char *units = purple_str_size_to_units((size_t)bps);
			speed = g_strdup_printf(_("%s/s"), units);
			g_free(units);
			if (purple_xfer_get_size(xfer) > 0)
				remaining = format_duration(purple_xfer_get_bytes_remaining(xfer) / bps);
		}
	} else if (purple_xfer_is_completed(xfer) && start > 0) {
		double elapsed = difftime(purple_xfer_get_end_time(xfer), start);

		if (elapsed > 0) {
			char *units = purple_str_size_to_units(
				(size_t)(purple_xfer_get_bytes_sent(xfer) / elapsed));
			speed = g_strdup_printf(_("%s/s"), units);
			g_free(units);
		}
	}
	row_set_string(row, &row->speed, speed ? speed : g_strdup(""), PROP_SPEED);

	if (purple_xfer_is_completed(xfer)) {
		status = g_strdup(_("Finished"));
		icon = "object-select-symbolic";
	} else if (purple_xfer_is_canceled(xfer)) {
		status = g_strdup(_("Cancelled"));
		icon = "process-stop-symbolic";
	} else if (row->failed) {
		status = g_strdup(_("Failed"));
		icon = "dialog-error-symbolic";
	} else if (purple_xfer_get_status(xfer) == PURPLE_XFER_STATUS_STARTED) {
		status = g_strdup_printf("%d%%", (int)(progress * 100));
		icon = type == PURPLE_XFER_RECEIVE ? "go-down-symbolic" : "go-up-symbolic";
	} else {
		status = g_strdup(_("Waiting for transfer to begin"));
		icon = type == PURPLE_XFER_RECEIVE ? "go-down-symbolic" : "go-up-symbolic";
	}
	row_set_string(row, &row->status, status, PROP_STATUS);
	row_set_string(row, &row->icon_name, g_strdup(icon), PROP_ICON_NAME);

	if (remaining == NULL && xfer_is_running(xfer) &&
	    purple_xfer_get_status(xfer) == PURPLE_XFER_STATUS_STARTED)
		remaining = g_strdup(_("Unknown"));
	row_set_string(row, &row->remaining, remaining ? remaining : g_strdup(""),
	               PROP_REMAINING);

	g_object_thaw_notify(G_OBJECT(row));
}

static PidginXferRow *
pidgin_xfer_row_new(PurpleXfer *xfer)
{
	PidginXferRow *row = g_object_new(PIDGIN_TYPE_XFER_ROW, NULL);

	purple_xfer_ref(xfer);
	row->xfer = xfer;
	xfer->ui_data = row;
	row_refresh(row);
	return row;
}

/**************************************************************************
 * The window
 **************************************************************************/

typedef struct {
	GtkWidget *window;
	GtkWidget *columnview;
	GListStore *store;
	GtkSingleSelection *selection;
	GtkWidget *open_button;
	GtkWidget *folder_button;
	GtkWidget *stop_button;
	GtkWidget *remove_button;
	GtkWidget *clear_button;
} XferDialog;

static XferDialog *xfer_dialog = NULL;

static void update_buttons(void);

static gboolean
find_row(PidginXferRow *row, guint *pos)
{
	return xfer_dialog != NULL &&
		g_list_store_find(xfer_dialog->store, row, pos);
}

static void
update_title(void)
{
	guint i, n;
	int active = 0;
	guint64 done = 0, total = 0;
	char *title;

	if (xfer_dialog == NULL)
		return;

	n = g_list_model_get_n_items(G_LIST_MODEL(xfer_dialog->store));
	for (i = 0; i < n; i++) {
		PidginXferRow *row = g_list_model_get_item(G_LIST_MODEL(xfer_dialog->store), i);

		if (row->xfer != NULL &&
		    purple_xfer_get_status(row->xfer) == PURPLE_XFER_STATUS_STARTED) {
			active++;
			done += purple_xfer_get_bytes_sent(row->xfer);
			total += purple_xfer_get_size(row->xfer);
		}
		g_object_unref(row);
	}

	if (active > 0) {
		int pct = total > 0 ? (int)(100 * done / total) : 0;

		title = g_strdup_printf(ngettext("File Transfers - %d%% of %d file",
		                                 "File Transfers - %d%% of %d files", active),
		                        pct, active);
		gtk_window_set_title(GTK_WINDOW(xfer_dialog->window), title);
		g_free(title);
	} else {
		gtk_window_set_title(GTK_WINDOW(xfer_dialog->window), _("File Transfers"));
	}
}

static void
remove_row(PidginXferRow *row)
{
	guint pos;

	if (find_row(row, &pos))
		g_list_store_remove(xfer_dialog->store, pos);   /* drops the xfer */
	update_title();
	update_buttons();
}

/* Hides the window when every transfer is completed, unless keep_open
 * (Pidgin 2: "Close this window when all transfers finish"). */
static void
maybe_hide(void)
{
	guint i, n;

	if (xfer_dialog == NULL || purple_prefs_get_bool(PREFS_ROOT "/keep_open"))
		return;

	n = g_list_model_get_n_items(G_LIST_MODEL(xfer_dialog->store));
	for (i = 0; i < n; i++) {
		PidginXferRow *row = g_list_model_get_item(G_LIST_MODEL(xfer_dialog->store), i);
		gboolean completed = row->xfer == NULL || purple_xfer_is_completed(row->xfer);

		g_object_unref(row);
		if (!completed)
			return;
	}
	pidgin_xfer_dialog_hide();
}

static PidginXferRow *
selected_row(void)
{
	PidginXferRow *row;

	if (xfer_dialog == NULL)
		return NULL;
	row = gtk_single_selection_get_selected_item(xfer_dialog->selection);
	return (row != NULL && row->xfer != NULL) ? row : NULL;
}

static gboolean
row_has_local_file(PidginXferRow *row)
{
	const char *local;

	if (row == NULL || !purple_xfer_is_completed(row->xfer) ||
	    purple_xfer_get_type(row->xfer) != PURPLE_XFER_RECEIVE)
		return FALSE;
	local = purple_xfer_get_local_filename(row->xfer);
	return local != NULL && *local != '\0';
}

static void
update_buttons(void)
{
	PidginXferRow *row;
	gboolean any_finished = FALSE;
	guint i, n;

	if (xfer_dialog == NULL)
		return;

	row = selected_row();
	gtk_widget_set_sensitive(xfer_dialog->open_button, row_has_local_file(row));
	gtk_widget_set_sensitive(xfer_dialog->folder_button, row_has_local_file(row));
	gtk_widget_set_sensitive(xfer_dialog->stop_button,
		row != NULL && xfer_is_running(row->xfer));
	gtk_widget_set_sensitive(xfer_dialog->remove_button,
		row != NULL && xfer_is_finished(row->xfer));

	n = g_list_model_get_n_items(G_LIST_MODEL(xfer_dialog->store));
	for (i = 0; i < n && !any_finished; i++) {
		PidginXferRow *r = g_list_model_get_item(G_LIST_MODEL(xfer_dialog->store), i);

		any_finished = r->xfer != NULL && xfer_is_finished(r->xfer);
		g_object_unref(r);
	}
	gtk_widget_set_sensitive(xfer_dialog->clear_button, any_finished);
}

/* Actions */

static void
launch_done_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	GError *error = NULL;
	gboolean ok;

	if (GPOINTER_TO_INT(data))
		ok = gtk_file_launcher_open_containing_folder_finish(
			GTK_FILE_LAUNCHER(source), result, &error);
	else
		ok = gtk_file_launcher_launch_finish(GTK_FILE_LAUNCHER(source), result, &error);
	if (!ok) {
		if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)) {
			GFile *file = gtk_file_launcher_get_file(GTK_FILE_LAUNCHER(source));
			char *path = g_file_get_parse_name(file);
			char *msg = g_strdup_printf(_("Error launching %s: %s"), path,
			                            error->message);

			purple_notify_error(NULL, NULL, _("Unable to open file."), msg);
			g_free(msg);
			g_free(path);
		}
		g_error_free(error);
	}
}

static void
launch_file(gboolean folder)
{
	PidginXferRow *row = selected_row();
	GtkFileLauncher *launcher;
	GFile *file;

	if (!row_has_local_file(row))
		return;
	file = g_file_new_for_path(purple_xfer_get_local_filename(row->xfer));
	launcher = gtk_file_launcher_new(file);
	if (folder)
		gtk_file_launcher_open_containing_folder(launcher,
			GTK_WINDOW(xfer_dialog->window), NULL, launch_done_cb,
			GINT_TO_POINTER(TRUE));
	else
		gtk_file_launcher_launch(launcher, GTK_WINDOW(xfer_dialog->window), NULL,
		                         launch_done_cb, GINT_TO_POINTER(FALSE));
	g_object_unref(launcher);
	g_object_unref(file);
}

static void
open_cb(GtkWidget *button, gpointer data)
{
	launch_file(FALSE);
}

static void
folder_cb(GtkWidget *button, gpointer data)
{
	launch_file(TRUE);
}

static void
stop_cb(GtkWidget *button, gpointer data)
{
	PidginXferRow *row = selected_row();

	if (row != NULL && xfer_is_running(row->xfer))
		purple_xfer_cancel_local(row->xfer);
}

static void
remove_cb(GtkWidget *button, gpointer data)
{
	PidginXferRow *row = selected_row();

	if (row != NULL && xfer_is_finished(row->xfer))
		remove_row(row);
}

static void
clear_finished(void)
{
	guint i;

	if (xfer_dialog == NULL)
		return;
	for (i = g_list_model_get_n_items(G_LIST_MODEL(xfer_dialog->store)); i > 0; i--) {
		PidginXferRow *row = g_list_model_get_item(G_LIST_MODEL(xfer_dialog->store), i - 1);

		if (row->xfer == NULL || xfer_is_finished(row->xfer))
			g_list_store_remove(xfer_dialog->store, i - 1);
		g_object_unref(row);
	}
	update_title();
	update_buttons();
}

static void
clear_cb(GtkWidget *button, gpointer data)
{
	clear_finished();
}

static void
close_cb(GtkWidget *button, gpointer data)
{
	pidgin_xfer_dialog_hide();
}

static void
selection_changed_cb(GtkSelectionModel *model, guint position, guint n, gpointer data)
{
	update_buttons();
}

/* Cells: each column binds to one property of the row. */

typedef void (*CellUpdateFunc)(GtkWidget *child, PidginXferRow *row);

static void
cell_notify_cb(PidginXferRow *row, GParamSpec *pspec, GtkListItem *li)
{
	CellUpdateFunc update = g_object_get_data(G_OBJECT(li), "pidgin-update");
	GtkWidget *child = gtk_list_item_get_child(li);

	if (update != NULL && child != NULL)
		update(child, row);
}

static void
cell_bind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	PidginXferRow *row = gtk_list_item_get_item(li);
	gulong id;

	g_object_set_data(G_OBJECT(li), "pidgin-update", data);
	cell_notify_cb(row, NULL, li);
	id = g_signal_connect(row, "notify", G_CALLBACK(cell_notify_cb), li);
	g_object_set_data(G_OBJECT(li), "pidgin-handler", GSIZE_TO_POINTER(id));
}

static void
cell_unbind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	PidginXferRow *row = gtk_list_item_get_item(li);
	gulong id = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(li), "pidgin-handler"));

	if (row != NULL && id != 0)
		g_signal_handler_disconnect(row, id);
	g_object_set_data(G_OBJECT(li), "pidgin-handler", NULL);
}

static void
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
}

static void
icon_setup_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	gtk_list_item_set_child(li, gtk_image_new());
}

static void
icon_update(GtkWidget *child, PidginXferRow *row)
{
	gtk_image_set_from_icon_name(GTK_IMAGE(child), row->icon_name);
	gtk_accessible_update_property(GTK_ACCESSIBLE(child),
		GTK_ACCESSIBLE_PROPERTY_LABEL, row->status ? row->status : "", -1);
}

static void
file_setup_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *name = gtk_label_new(NULL), *peer = gtk_label_new(NULL);

	gtk_label_set_xalign(GTK_LABEL(name), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
	gtk_label_set_xalign(GTK_LABEL(peer), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(peer), PANGO_ELLIPSIZE_END);
	gtk_widget_add_css_class(peer, "dim-label");
	gtk_widget_add_css_class(peer, "caption");
	gtk_box_append(GTK_BOX(box), name);
	gtk_box_append(GTK_BOX(box), peer);
	gtk_list_item_set_child(li, box);
}

static void
file_update(GtkWidget *child, PidginXferRow *row)
{
	GtkWidget *name = gtk_widget_get_first_child(child);
	GtkWidget *peer = gtk_widget_get_next_sibling(name);

	gtk_label_set_text(GTK_LABEL(name), row->filename ? row->filename : "");
	gtk_label_set_text(GTK_LABEL(peer), row->peer ? row->peer : "");
	gtk_widget_set_tooltip_text(child,
		row->xfer ? purple_xfer_get_local_filename(row->xfer) : NULL);
}

static void
progress_setup_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	GtkWidget *bar = gtk_progress_bar_new();

	gtk_widget_set_valign(bar, GTK_ALIGN_CENTER);
	gtk_widget_set_size_request(bar, 100, -1);
	gtk_list_item_set_child(li, bar);
}

static void
progress_update(GtkWidget *child, PidginXferRow *row)
{
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(child), row->progress);
}

static void
label_setup_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	GtkWidget *label = gtk_label_new(NULL);

	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_list_item_set_child(li, label);
}

#define LABEL_UPDATE(field) \
static void \
field##_update(GtkWidget *child, PidginXferRow *row) \
{ \
	gtk_label_set_text(GTK_LABEL(child), row->field ? row->field : ""); \
}
LABEL_UPDATE(size)
LABEL_UPDATE(speed)
LABEL_UPDATE(remaining)
LABEL_UPDATE(status)

static gboolean
xfer_close_request_cb(GtkWindow *window, gpointer data)
{
	/* hide-on-close: the list stays */
	return FALSE;
}

static void
create_dialog(void)
{
	XferDialog *dialog;
	GtkWidget *win, *content, *sw, *hbox, *check, *spacer;

	xfer_dialog = dialog = g_new0(XferDialog, 1);
	dialog->window = win = pidgin_dialog_new(_("File Transfers"), NULL,
	                                         "file transfer", TRUE);
	gtk_window_set_default_size(GTK_WINDOW(win), 720, 320);
	gtk_window_set_hide_on_close(GTK_WINDOW(win), TRUE);
	g_signal_connect(win, "close-request", G_CALLBACK(xfer_close_request_cb), NULL);
	content = pidgin_dialog_get_content_area(win);

	dialog->store = g_list_store_new(PIDGIN_TYPE_XFER_ROW);
	dialog->selection = gtk_single_selection_new(
		G_LIST_MODEL(g_object_ref(dialog->store)));
	gtk_single_selection_set_can_unselect(dialog->selection, TRUE);
	g_signal_connect(dialog->selection, "selection-changed",
	                 G_CALLBACK(selection_changed_cb), NULL);

	dialog->columnview = gtk_column_view_new(
		GTK_SELECTION_MODEL(g_object_ref(dialog->selection)));
	gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(dialog->columnview), FALSE);
	add_column(dialog->columnview, NULL, G_CALLBACK(icon_setup_cb), icon_update, FALSE);
	add_column(dialog->columnview, _("Filename"), G_CALLBACK(file_setup_cb),
	           file_update, TRUE);
	add_column(dialog->columnview, _("Progress"), G_CALLBACK(progress_setup_cb),
	           progress_update, FALSE);
	add_column(dialog->columnview, _("Size"), G_CALLBACK(label_setup_cb),
	           size_update, FALSE);
	add_column(dialog->columnview, _("Speed"), G_CALLBACK(label_setup_cb),
	           speed_update, FALSE);
	add_column(dialog->columnview, _("Remaining"), G_CALLBACK(label_setup_cb),
	           remaining_update, FALSE);
	add_column(dialog->columnview, _("Status"), G_CALLBACK(label_setup_cb),
	           status_update, FALSE);

	sw = pidgin_make_scrollable(dialog->columnview, GTK_POLICY_AUTOMATIC,
	                            GTK_POLICY_AUTOMATIC, -1, 160);
	gtk_widget_set_vexpand(sw, TRUE);
	gtk_box_append(GTK_BOX(content), sw);

	/* Per-row actions */
	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	gtk_box_append(GTK_BOX(content), hbox);
	dialog->open_button = gtk_button_new_with_mnemonic(_("_Open File"));
	g_signal_connect(dialog->open_button, "clicked", G_CALLBACK(open_cb), NULL);
	gtk_box_append(GTK_BOX(hbox), dialog->open_button);
	dialog->folder_button = gtk_button_new_with_mnemonic(_("Open _Folder"));
	g_signal_connect(dialog->folder_button, "clicked", G_CALLBACK(folder_cb), NULL);
	gtk_box_append(GTK_BOX(hbox), dialog->folder_button);
	dialog->stop_button = gtk_button_new_with_mnemonic(_("_Stop"));
	g_signal_connect(dialog->stop_button, "clicked", G_CALLBACK(stop_cb), NULL);
	gtk_box_append(GTK_BOX(hbox), dialog->stop_button);
	dialog->remove_button = gtk_button_new_with_mnemonic(_("_Remove"));
	g_signal_connect(dialog->remove_button, "clicked", G_CALLBACK(remove_cb), NULL);
	gtk_box_append(GTK_BOX(hbox), dialog->remove_button);
	spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_hexpand(spacer, TRUE);
	gtk_box_append(GTK_BOX(hbox), spacer);
	dialog->clear_button = gtk_button_new_with_mnemonic(_("Clear _Finished"));
	g_signal_connect(dialog->clear_button, "clicked", G_CALLBACK(clear_cb), NULL);
	gtk_box_append(GTK_BOX(hbox), dialog->clear_button);

	/* Options */
	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BORDER);
	gtk_box_append(GTK_BOX(content), hbox);
	check = pidgin_pref_checkbox_new(_("_Keep the dialog open"), PREFS_ROOT "/keep_open");
	gtk_box_append(GTK_BOX(hbox), check);
	check = pidgin_pref_checkbox_new(_("C_lear finished transfers"),
	                                 PREFS_ROOT "/clear_finished");
	gtk_box_append(GTK_BOX(hbox), check);

	pidgin_dialog_add_button(win, _("_Close"), G_CALLBACK(close_cb), NULL);

	update_buttons();
}

void
pidgin_xfer_dialog_show(void)
{
	if (xfer_dialog == NULL)
		create_dialog();
	pidgin_window_set_secondary(GTK_WINDOW(xfer_dialog->window));
	gtk_window_present(GTK_WINDOW(xfer_dialog->window));
}

void
pidgin_xfer_dialog_hide(void)
{
	if (xfer_dialog != NULL)
		gtk_widget_set_visible(xfer_dialog->window, FALSE);
}

static void
destroy_dialog(void)
{
	XferDialog *dialog = xfer_dialog;

	if (dialog == NULL)
		return;
	xfer_dialog = NULL;
	gtk_window_destroy(GTK_WINDOW(dialog->window));
	g_list_store_remove_all(dialog->store);
	g_clear_object(&dialog->selection);
	g_clear_object(&dialog->store);
	g_free(dialog);
}

/**************************************************************************
 * UI ops
 **************************************************************************/

static PidginXferRow *
xfer_row(PurpleXfer *xfer)
{
	PidginXferRow *row = xfer->ui_data;

	return (row != NULL && PIDGIN_IS_XFER_ROW(row) && row->xfer == xfer) ? row : NULL;
}

static void
pidgin_xfer_new_xfer(PurpleXfer *xfer)
{
	xfer->ui_data = NULL;
}

static void
pidgin_xfer_destroy(PurpleXfer *xfer)
{
	PidginXferRow *row = xfer_row(xfer);

	/* A row holds a reference, so it should be gone already. */
	if (row != NULL) {
		row->xfer = NULL;
		remove_row(row);
	}
	xfer->ui_data = NULL;
}

static void
pidgin_xfer_add_xfer(PurpleXfer *xfer)
{
	PidginXferRow *row;

	if (xfer_row(xfer) != NULL)
		return;
	pidgin_xfer_dialog_show();
	row = pidgin_xfer_row_new(xfer);
	g_list_store_append(xfer_dialog->store, row);
	if (gtk_single_selection_get_selected(xfer_dialog->selection) ==
	    GTK_INVALID_LIST_POSITION)
		gtk_single_selection_set_selected(xfer_dialog->selection,
			g_list_model_get_n_items(G_LIST_MODEL(xfer_dialog->store)) - 1);
	g_object_unref(row);
	update_title();
	update_buttons();
}

/* After a refresh: auto-clear, buttons, title, auto-hide. */
static void
row_updated(PidginXferRow *row)
{
	PurpleXfer *xfer = row->xfer;

	if (xfer == NULL)
		return;
	if (purple_xfer_is_completed(xfer) &&
	    purple_prefs_get_bool(PREFS_ROOT "/clear_finished")) {
		remove_row(row);
	} else {
		update_buttons();
		update_title();
	}
	if (purple_xfer_is_completed(xfer))
		maybe_hide();
}

static gboolean
pending_refresh_cb(gpointer data)
{
	PidginXferRow *row = data;

	row->pending_id = 0;
	g_object_ref(row);
	row_refresh(row);
	row_updated(row);
	g_object_unref(row);
	return G_SOURCE_REMOVE;
}

static void
pidgin_xfer_update_progress(PurpleXfer *xfer, double percent)
{
	PidginXferRow *row = xfer_row(xfer);
	gint64 now = g_get_monotonic_time();

	if (row == NULL)
		return;

	/* Throttle, but never drop the final state. */
	if (!xfer_is_finished(xfer) && now - row->last_update < UPDATE_INTERVAL_US) {
		if (row->pending_id == 0)
			row->pending_id = g_timeout_add(
				(guint)((UPDATE_INTERVAL_US - (now - row->last_update)) / 1000) + 1,
				pending_refresh_cb, row);
		return;
	}
	g_clear_handle_id(&row->pending_id, g_source_remove);
	g_object_ref(row);
	row_refresh(row);
	row_updated(row);
	g_object_unref(row);
}

static void
pidgin_xfer_cancel(PurpleXfer *xfer)
{
	PidginXferRow *row = xfer_row(xfer);

	if (row == NULL)
		return;
	g_clear_handle_id(&row->pending_id, g_source_remove);
	/* Like Pidgin 2: "Cancelled", or "Failed" when the transfer ended
	 * without being cancelled (an error). */
	row->failed = !purple_xfer_is_canceled(xfer);
	/* Pidgin 2 cleared only transfers cancelled here. */
	if (purple_xfer_get_status(xfer) == PURPLE_XFER_STATUS_CANCEL_LOCAL &&
	    purple_prefs_get_bool(PREFS_ROOT "/clear_finished")) {
		remove_row(row);
		return;
	}
	row_refresh(row);
	update_buttons();
	update_title();
}

static void
pidgin_xfer_cancel_local(PurpleXfer *xfer)
{
	pidgin_xfer_cancel(xfer);
}

static void
pidgin_xfer_cancel_remote(PurpleXfer *xfer)
{
	pidgin_xfer_cancel(xfer);
}

static PurpleXferUiOps ops = {
	.new_xfer = pidgin_xfer_new_xfer,
	.destroy = pidgin_xfer_destroy,
	.add_xfer = pidgin_xfer_add_xfer,
	.update_progress = pidgin_xfer_update_progress,
	.cancel_local = pidgin_xfer_cancel_local,
	.cancel_remote = pidgin_xfer_cancel_remote,
};

PurpleXferUiOps *
pidgin_xfers_get_ui_ops(void)
{
	return &ops;
}

void
pidgin_xfers_init(void)
{
	purple_prefs_add_none(PREFS_ROOT);
	purple_prefs_add_bool(PREFS_ROOT "/clear_finished", TRUE);
	purple_prefs_add_bool(PREFS_ROOT "/keep_open", FALSE);
}

void
pidgin_xfers_uninit(void)
{
	destroy_dialog();
}

/**************************************************************************
 * Selftest
 **************************************************************************/

#define MODULE "xfers"

static guint
n_rows(void)
{
	return xfer_dialog ? g_list_model_get_n_items(G_LIST_MODEL(xfer_dialog->store)) : 0;
}

static gboolean
xfer_listed(PurpleXfer *xfer)
{
	return xfer_row(xfer) != NULL && find_row(xfer_row(xfer), NULL);
}

void
pidgin_xfers_selftest(void)
{
	GList *accounts = purple_accounts_get_all();
	PurpleAccount *account;
	PurpleXfer *done, *running, *upload;
	PidginXferRow *row;
	gboolean clear, keep;
	guint pos, base;

	if (accounts == NULL) {
		pidgin_selftest_log(MODULE, "no account; skipped");
		return;
	}
	account = accounts->data;

	/* Don't let auto-clear/auto-hide interfere; restore at the end. */
	clear = purple_prefs_get_bool(PREFS_ROOT "/clear_finished");
	keep = purple_prefs_get_bool(PREFS_ROOT "/keep_open");
	purple_prefs_set_bool(PREFS_ROOT "/clear_finished", FALSE);
	purple_prefs_set_bool(PREFS_ROOT "/keep_open", TRUE);

	pidgin_xfer_dialog_show();
	base = n_rows();

	/* A received, finished file. */
	done = purple_xfer_new(account, PURPLE_XFER_RECEIVE, "someone@example.com");
	purple_xfer_set_filename(done, "photo.jpg");
	purple_xfer_set_local_filename(done, "/tmp/pidgin4-selftest-photo.jpg");
	purple_xfer_set_size(done, 123456);
	purple_xfer_add(done);
	purple_xfer_set_bytes_sent(done, 123456);
	purple_xfer_set_completed(done, TRUE);

	/* A running upload. */
	running = purple_xfer_new(account, PURPLE_XFER_SEND, "someone");
	purple_xfer_set_local_filename(running, "/tmp/pidgin4-selftest-doc.pdf");
	purple_xfer_set_filename(running, "doc.pdf");
	purple_xfer_set_size(running, 1000000);
	purple_xfer_add(running);
	purple_xfer_set_bytes_sent(running, 400000);
	purple_xfer_update_progress(running);
	/* Throttled updates in a burst: the last one must land. */
	purple_xfer_set_bytes_sent(running, 500000);
	purple_xfer_update_progress(running);
	purple_xfer_set_bytes_sent(running, 600000);
	purple_xfer_update_progress(running);

	/* An HTTP-upload-like transfer: empty remote user, no local name. */
	upload = purple_xfer_new(account, PURPLE_XFER_SEND, "");
	purple_xfer_set_size(upload, 0);
	purple_xfer_add(upload);

	pidgin_selftest_iterate(400);

	if (n_rows() != base + 3)
		pidgin_selftest_fail(MODULE, "%u rows, expected %u", n_rows(), base + 3);
	if ((row = xfer_row(running)) == NULL)
		pidgin_selftest_fail(MODULE, "the running transfer has no row");
	else if (row->progress < 0.59 || row->progress > 0.61)
		pidgin_selftest_fail(MODULE, "throttled progress is %.2f, expected 0.60",
		                     row->progress);
	if ((row = xfer_row(done)) == NULL || !purple_strequal(row->status, _("Finished")))
		pidgin_selftest_fail(MODULE, "the finished transfer is not shown as finished");
	if ((row = xfer_row(upload)) == NULL || row->filename == NULL || row->peer == NULL)
		pidgin_selftest_fail(MODULE, "the upload row is incomplete");

	/* Buttons on the finished row. */
	if (xfer_row(done) != NULL && find_row(xfer_row(done), &pos)) {
		gtk_single_selection_set_selected(xfer_dialog->selection, pos);
		if (!gtk_widget_get_sensitive(xfer_dialog->open_button) ||
		    !gtk_widget_get_sensitive(xfer_dialog->remove_button) ||
		    gtk_widget_get_sensitive(xfer_dialog->stop_button))
			pidgin_selftest_fail(MODULE, "wrong buttons for a finished transfer");
	}

	/* Stop the running one (the Stop button). */
	if (xfer_row(running) != NULL && find_row(xfer_row(running), &pos)) {
		gtk_single_selection_set_selected(xfer_dialog->selection, pos);
		if (!gtk_widget_get_sensitive(xfer_dialog->stop_button))
			pidgin_selftest_fail(MODULE, "Stop is not enabled for a running transfer");
		/* The creator's reference: cancel_local drops one, keep ours. */
		purple_xfer_ref(running);
		g_signal_emit_by_name(xfer_dialog->stop_button, "clicked");
		if (!purple_xfer_is_canceled(running))
			pidgin_selftest_fail(MODULE, "Stop did not cancel");
	}
	pidgin_selftest_iterate(100);

	/* Clear Finished removes the finished and the cancelled rows. */
	g_signal_emit_by_name(xfer_dialog->clear_button, "clicked");
	if (xfer_listed(done) || xfer_listed(running))
		pidgin_selftest_fail(MODULE, "Clear Finished left finished rows");
	if (!xfer_listed(upload))
		pidgin_selftest_fail(MODULE, "Clear Finished removed a waiting row");

	/* Drop the creators' references; the rows' are gone with the rows.
	 * The waiting upload: cancel it (drops its creator reference), then
	 * remove its row. */
	purple_xfer_unref(done);
	purple_xfer_unref(running);
	purple_xfer_cancel_local(upload);
	clear_finished();
	if (n_rows() != base)
		pidgin_selftest_fail(MODULE, "%u rows left, expected %u", n_rows(), base);

	pidgin_selftest_iterate(50);
	purple_prefs_set_bool(PREFS_ROOT "/clear_finished", clear);
	purple_prefs_set_bool(PREFS_ROOT "/keep_open", keep);
	pidgin_xfer_dialog_hide();
	pidgin_selftest_log(MODULE, "3 fake transfers shown, stopped, cleared");
}
