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
 * Custom smileys (the port of pidgin/gtksmiley.c's manager and editor).
 * libpurple owns smileys.xml and <profile>/custom_smiley; nothing is
 * written there unless the user saves an editor (or deletes a smiley).
 * libpurple 2.14 has no smiley UI ops, so the manager refreshes itself
 * after its own changes. Rows refer to smileys by shortcut, never by
 * pointer.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "debug.h"
#include "imgstore.h"
#include "notify.h"
#include "smiley.h"
#include "util.h"

#include "gtksmiley.h"
#include "gtkutils.h"
#include "pidginanimation.h"
#include "pidginselftest.h"
#include "pidginsmileytheme.h"

#define PREVIEW_SIZE 64

typedef struct {
	GtkWidget *window;
	GtkWidget *listview;
	GListStore *store;              /* PidginItems: id = shortcut */
	GtkSingleSelection *selection;
	GtkWidget *edit_button;
	GtkWidget *delete_button;
} SmileyManager;

static SmileyManager *smiley_manager = NULL;

typedef struct {
	GtkWidget *window;
	GtkWidget *picture;
	GtkWidget *entry;
	GtkWidget *ok_button;
	char *original;         /* the edited smiley's shortcut; NULL: new */
	GBytes *data;           /* a newly chosen image, or NULL */
	gboolean has_image;
} SmileyEditor;

static GList *editors = NULL;

static void refresh_list(void);

/**************************************************************************
 * The editor
 **************************************************************************/

static void
editor_update_ok(SmileyEditor *ed)
{
	const char *text = gtk_editable_get_text(GTK_EDITABLE(ed->entry));

	gtk_widget_set_sensitive(ed->ok_button, ed->has_image && text != NULL && *text != '\0');
}

static void
editor_set_image(SmileyEditor *ed, GBytes *data)
{
	GdkPaintable *paintable;
	gsize len;
	gconstpointer bytes = g_bytes_get_data(data, &len);

	paintable = pidgin_paintable_new_from_data(bytes, len);
	if (paintable == NULL) {
		purple_notify_error(NULL, _("Custom Smiley"), _("Unable to open file."),
		                    _("The file is not a supported image."));
		return;
	}
	gtk_picture_set_paintable(GTK_PICTURE(ed->picture), paintable);
	g_object_unref(paintable);
	g_clear_pointer(&ed->data, g_bytes_unref);
	ed->data = g_bytes_ref(data);
	ed->has_image = TRUE;
	editor_update_ok(ed);
}

static void
image_chosen_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	GtkWidget *window = data;
	SmileyEditor *ed;
	GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
	GBytes *bytes;
	GError *error = NULL;

	/* The editor may have closed meanwhile. */
	ed = g_object_get_data(G_OBJECT(window), "pidgin-smiley-editor");
	if (file != NULL && ed != NULL) {
		bytes = g_file_load_bytes(file, NULL, NULL, &error);
		if (bytes == NULL) {
			purple_notify_error(NULL, _("Custom Smiley"), _("Unable to open file."),
			                    error->message);
			g_error_free(error);
		} else {
			editor_set_image(ed, bytes);
			g_bytes_unref(bytes);
		}
	}
	g_clear_object(&file);
	g_object_unref(window);
}

static void
choose_image_cb(GtkWidget *button, gpointer data)
{
	SmileyEditor *ed = data;
	GtkFileDialog *fd = gtk_file_dialog_new();
	GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
	GtkFileFilter *filter = gtk_file_filter_new();

	gtk_file_filter_set_name(filter, _("Images"));
	gtk_file_filter_add_pixbuf_formats(filter);
	gtk_file_filter_add_mime_type(filter, "image/*");
	g_list_store_append(filters, filter);
	gtk_file_dialog_set_default_filter(fd, filter);
	g_object_unref(filter);
	gtk_file_dialog_set_filters(fd, G_LIST_MODEL(filters));
	g_object_unref(filters);
	gtk_file_dialog_set_title(fd, _("Custom Smiley"));
	gtk_file_dialog_open(fd, GTK_WINDOW(ed->window), NULL, image_chosen_cb,
	                     g_object_ref(ed->window));
	g_object_unref(fd);
}

static void
entry_changed_cb(GtkEditable *editable, gpointer data)
{
	editor_update_ok(data);
}

static void
editor_free(gpointer data)
{
	SmileyEditor *ed = data;

	editors = g_list_remove(editors, ed);
	g_clear_pointer(&ed->data, g_bytes_unref);
	g_free(ed->original);
	g_free(ed);
}

static void
cancel_cb(GtkWidget *button, gpointer data)
{
	SmileyEditor *ed = data;

	gtk_window_destroy(GTK_WINDOW(ed->window));
}

/* A new smiley from image data: libpurple reads it from a file, so write
 * a private temporary one (never into custom_smiley). */
static PurpleSmiley *
smiley_new_from_bytes(const char *shortcut, GBytes *data)
{
	PurpleSmiley *smiley = NULL;
	GError *error = NULL;
	char *tmpname = NULL;
	gsize len;
	gconstpointer bytes = g_bytes_get_data(data, &len);
	int fd;

	fd = g_file_open_tmp("pidgin4-smiley-XXXXXX", &tmpname, &error);
	if (fd < 0) {
		purple_debug_error("gtksmiley", "%s\n", error->message);
		g_error_free(error);
		return NULL;
	}
	close(fd);
	if (g_file_set_contents(tmpname, bytes, len, &error))
		smiley = purple_smiley_new_from_file(shortcut, tmpname);
	else {
		purple_debug_error("gtksmiley", "%s\n", error->message);
		g_error_free(error);
	}
	g_unlink(tmpname);
	g_free(tmpname);
	return smiley;
}

static void
ok_cb(GtkWidget *button, gpointer data)
{
	SmileyEditor *ed = data;
	char *shortcut = g_strdup(gtk_editable_get_text(GTK_EDITABLE(ed->entry)));
	PurpleSmiley *smiley = NULL, *other;

	g_strstrip(shortcut);
	if (*shortcut == '\0') {
		g_free(shortcut);
		return;
	}

	if (ed->original != NULL)
		smiley = purple_smileys_find_by_shortcut(ed->original);

	other = purple_smileys_find_by_shortcut(shortcut);
	if (other != NULL && other != smiley) {
		char *msg = g_strdup_printf(_("A custom smiley for '%s' already exists.  "
			"Please use a different shortcut."), shortcut);
		purple_notify_error(NULL, _("Custom Smiley"), _("Duplicate Shortcut"), msg);
		g_free(msg);
		g_free(shortcut);
		return;
	}

	if (smiley != NULL) {
		if (ed->data != NULL) {
			gsize len;
			gconstpointer bytes = g_bytes_get_data(ed->data, &len);

			/* libpurple takes the copy. */
			purple_smiley_set_data(smiley, g_memdup2(bytes, len), len);
		}
		if (!purple_strequal(purple_smiley_get_shortcut(smiley), shortcut))
			purple_smiley_set_shortcut(smiley, shortcut);
	} else if (ed->data != NULL) {
		purple_debug_info("gtksmiley", "adding a new smiley\n");
		if (smiley_new_from_bytes(shortcut, ed->data) == NULL)
			purple_notify_error(NULL, _("Custom Smiley"),
			                    _("Custom Smiley"), _("Unable to open file."));
	}

	g_free(shortcut);
	refresh_list();
	gtk_window_destroy(GTK_WINDOW(ed->window));
}

static SmileyEditor *
editor_new(GtkWindow *parent, PurpleSmiley *smiley)
{
	SmileyEditor *ed = g_new0(SmileyEditor, 1);
	GtkWidget *content, *grid, *label, *button, *frame;

	if (smiley != NULL)
		ed->original = g_strdup(purple_smiley_get_shortcut(smiley));

	ed->window = pidgin_dialog_new(smiley ? _("Edit Smiley") : _("Add Smiley"),
		parent, "smiley_dialog", FALSE);
	g_object_set_data_full(G_OBJECT(ed->window), "pidgin-smiley-editor", ed, editor_free);
	content = pidgin_dialog_get_content_area(ed->window);

	grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), PIDGIN_HIG_BOX_SPACE);
	gtk_grid_set_column_spacing(GTK_GRID(grid), PIDGIN_HIG_BORDER);
	gtk_box_append(GTK_BOX(content), grid);

	label = gtk_label_new_with_mnemonic(_("_Image:"));
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_grid_attach(GTK_GRID(grid), label, 0, 0, 1, 1);

	ed->picture = gtk_picture_new();
	gtk_picture_set_content_fit(GTK_PICTURE(ed->picture), GTK_CONTENT_FIT_SCALE_DOWN);
	gtk_picture_set_can_shrink(GTK_PICTURE(ed->picture), TRUE);
	gtk_widget_set_size_request(ed->picture, PREVIEW_SIZE, PREVIEW_SIZE);
	frame = gtk_frame_new(NULL);
	gtk_frame_set_child(GTK_FRAME(frame), ed->picture);
	button = gtk_button_new();
	gtk_button_set_child(GTK_BUTTON(button), frame);
	gtk_widget_set_halign(button, GTK_ALIGN_START);
	gtk_widget_set_tooltip_text(button, _("Choose an image"));
	g_signal_connect(button, "clicked", G_CALLBACK(choose_image_cb), ed);
	gtk_grid_attach(GTK_GRID(grid), button, 1, 0, 1, 1);
	pidgin_set_accessible_label(button, label);

	label = gtk_label_new_with_mnemonic(_("S_hortcut text:"));
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_grid_attach(GTK_GRID(grid), label, 0, 1, 1, 1);
	ed->entry = gtk_entry_new();
	gtk_entry_set_activates_default(GTK_ENTRY(ed->entry), TRUE);
	gtk_widget_set_hexpand(ed->entry, TRUE);
	gtk_grid_attach(GTK_GRID(grid), ed->entry, 1, 1, 1, 1);
	pidgin_set_accessible_label(ed->entry, label);
	if (smiley != NULL)
		gtk_editable_set_text(GTK_EDITABLE(ed->entry), purple_smiley_get_shortcut(smiley));
	g_signal_connect(ed->entry, "changed", G_CALLBACK(entry_changed_cb), ed);

	pidgin_dialog_add_button(ed->window, _("_Cancel"), G_CALLBACK(cancel_cb), ed);
	ed->ok_button = pidgin_dialog_add_button(ed->window, smiley ? _("_Save") : _("_Add"),
		G_CALLBACK(ok_cb), ed);
	gtk_window_set_default_widget(GTK_WINDOW(ed->window), ed->ok_button);

	if (smiley != NULL) {
		GdkPaintable *paintable = pidgin_custom_smiley_get_paintable(smiley);

		if (paintable != NULL)
			gtk_picture_set_paintable(GTK_PICTURE(ed->picture), paintable);
		ed->has_image = TRUE;
	}
	editor_update_ok(ed);

	editors = g_list_prepend(editors, ed);
	return ed;
}

static SmileyEditor *
editor_open(GtkWindow *parent, PurpleSmiley *smiley)
{
	SmileyEditor *ed;
	GList *l;

	if (smiley != NULL) {
		for (l = editors; l != NULL; l = l->next) {
			ed = l->data;
			if (purple_strequal(ed->original, purple_smiley_get_shortcut(smiley))) {
				gtk_window_present(GTK_WINDOW(ed->window));
				return ed;
			}
		}
	}
	if (parent == NULL && smiley_manager != NULL)
		parent = GTK_WINDOW(smiley_manager->window);
	ed = editor_new(parent, smiley);
	gtk_window_present(GTK_WINDOW(ed->window));
	gtk_widget_grab_focus(ed->entry);
	return ed;
}

void
pidgin_smiley_edit(GtkWindow *parent, PurpleSmiley *smiley)
{
	editor_open(parent, smiley);
}

void
pidgin_smiley_add_from_image(GtkWindow *parent, gconstpointer data, gsize len,
                             const char *shortcut)
{
	SmileyEditor *ed;
	GBytes *bytes;

	g_return_if_fail(data != NULL && len > 0);

	ed = editor_open(parent, NULL);
	bytes = g_bytes_new(data, len);
	editor_set_image(ed, bytes);
	g_bytes_unref(bytes);
	if (shortcut != NULL)
		gtk_editable_set_text(GTK_EDITABLE(ed->entry), shortcut);
}

/**************************************************************************
 * The manager
 **************************************************************************/

static int
item_compare(gconstpointer a, gconstpointer b, gpointer data)
{
	return g_utf8_collate(pidgin_item_get_id((PidginItem *)a),
	                      pidgin_item_get_id((PidginItem *)b));
}

static void
refresh_list(void)
{
	GList *smileys, *l;
	const char *keep = NULL;
	char *keep_copy;
	PidginItem *sel;
	guint i, n;

	if (smiley_manager == NULL)
		return;

	sel = gtk_single_selection_get_selected_item(smiley_manager->selection);
	if (sel != NULL)
		keep = pidgin_item_get_id(sel);
	keep_copy = g_strdup(keep);

	g_list_store_remove_all(smiley_manager->store);
	smileys = purple_smileys_get_all();
	for (l = smileys; l != NULL; l = l->next) {
		const char *shortcut = purple_smiley_get_shortcut(l->data);
		PidginItem *item = pidgin_item_new(shortcut, shortcut, NULL);

		g_list_store_insert_sorted(smiley_manager->store, item, item_compare, NULL);
		g_object_unref(item);
	}
	g_list_free(smileys);

	gtk_single_selection_set_selected(smiley_manager->selection, GTK_INVALID_LIST_POSITION);
	n = g_list_model_get_n_items(G_LIST_MODEL(smiley_manager->store));
	for (i = 0; keep_copy != NULL && i < n; i++) {
		PidginItem *item = g_list_model_get_item(G_LIST_MODEL(smiley_manager->store), i);
		gboolean match = purple_strequal(pidgin_item_get_id(item), keep_copy);

		g_object_unref(item);
		if (match) {
			gtk_single_selection_set_selected(smiley_manager->selection, i);
			break;
		}
	}
	g_free(keep_copy);
}

static PurpleSmiley *
selected_smiley(void)
{
	PidginItem *item;

	if (smiley_manager == NULL)
		return NULL;
	item = gtk_single_selection_get_selected_item(smiley_manager->selection);
	return item != NULL ? purple_smileys_find_by_shortcut(pidgin_item_get_id(item)) : NULL;
}

static void
update_buttons(void)
{
	gboolean sel = selected_smiley() != NULL;

	gtk_widget_set_sensitive(smiley_manager->edit_button, sel);
	gtk_widget_set_sensitive(smiley_manager->delete_button, sel);
}

static void
selection_changed_cb(GtkSelectionModel *model, guint position, guint n, gpointer data)
{
	update_buttons();
}

static void
add_cb(GtkWidget *button, gpointer data)
{
	pidgin_smiley_edit(GTK_WINDOW(smiley_manager->window), NULL);
}

static void
edit_cb(GtkWidget *button, gpointer data)
{
	PurpleSmiley *smiley = selected_smiley();

	if (smiley != NULL)
		pidgin_smiley_edit(GTK_WINDOW(smiley_manager->window), smiley);
}

static void
activate_cb(GtkListView *view, guint position, gpointer data)
{
	PidginItem *item = g_list_model_get_item(G_LIST_MODEL(smiley_manager->store), position);
	PurpleSmiley *smiley = purple_smileys_find_by_shortcut(pidgin_item_get_id(item));

	if (smiley != NULL)
		pidgin_smiley_edit(GTK_WINDOW(smiley_manager->window), smiley);
	g_object_unref(item);
}

static void
delete_response_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	char *shortcut = data;
	int button = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, NULL);
	PurpleSmiley *smiley;
	GList *l;

	if (button == 1 && (smiley = purple_smileys_find_by_shortcut(shortcut)) != NULL) {
		/* Close its editor first. */
		for (l = editors; l != NULL; l = l->next) {
			SmileyEditor *ed = l->data;
			if (purple_strequal(ed->original, shortcut)) {
				gtk_window_destroy(GTK_WINDOW(ed->window));
				break;
			}
		}
		purple_smiley_delete(smiley);
		refresh_list();
	}
	g_free(shortcut);
}

static void
delete_cb(GtkWidget *button, gpointer data)
{
	PurpleSmiley *smiley = selected_smiley();
	const char *buttons[] = { _("_Cancel"), _("_Delete"), NULL };
	GtkAlertDialog *alert;

	if (smiley == NULL)
		return;
	alert = gtk_alert_dialog_new(_("Delete the custom smiley \"%s\"?"),
	                             purple_smiley_get_shortcut(smiley));
	gtk_alert_dialog_set_buttons(alert, buttons);
	gtk_alert_dialog_set_cancel_button(alert, 0);
	gtk_alert_dialog_set_default_button(alert, 0);
	gtk_alert_dialog_choose(alert, GTK_WINDOW(smiley_manager->window), NULL,
	                        delete_response_cb,
	                        g_strdup(purple_smiley_get_shortcut(smiley)));
	g_object_unref(alert);
}

static void
close_cb(GtkWidget *button, gpointer data)
{
	gtk_window_destroy(GTK_WINDOW(smiley_manager->window));
}

static void
manager_destroy_cb(GtkWidget *window, gpointer data)
{
	SmileyManager *mgr = smiley_manager;

	if (mgr == NULL)
		return;
	smiley_manager = NULL;
	g_clear_object(&mgr->selection);
	g_clear_object(&mgr->store);
	g_free(mgr);
}

static void
row_setup_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BORDER);
	GtkWidget *picture = gtk_picture_new();
	GtkWidget *label = gtk_label_new(NULL);

	gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_SCALE_DOWN);
	gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);
	gtk_widget_set_size_request(picture, 32, 32);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand(label, TRUE);
	gtk_box_append(GTK_BOX(box), picture);
	gtk_box_append(GTK_BOX(box), label);
	gtk_list_item_set_child(li, box);
}

static void
row_bind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	PidginItem *item = gtk_list_item_get_item(li);
	GtkWidget *box = gtk_list_item_get_child(li);
	GtkWidget *picture = gtk_widget_get_first_child(box);
	GtkWidget *label = gtk_widget_get_next_sibling(picture);
	PurpleSmiley *smiley = purple_smileys_find_by_shortcut(pidgin_item_get_id(item));

	gtk_picture_set_paintable(GTK_PICTURE(picture),
		smiley ? pidgin_custom_smiley_get_paintable(smiley) : NULL);
	gtk_label_set_text(GTK_LABEL(label), pidgin_item_get_id(item));
}

void
pidgin_smiley_manager_show(void)
{
	SmileyManager *mgr;
	GtkWidget *win, *content, *sw, *label;
	GtkListItemFactory *factory;
	char *markup;

	if (smiley_manager != NULL) {
		gtk_window_present(GTK_WINDOW(smiley_manager->window));
		return;
	}

	smiley_manager = mgr = g_new0(SmileyManager, 1);
	mgr->window = win = pidgin_dialog_new(_("Custom Smiley Manager"),
		pidgin_get_active_window(), "custom_smileys", TRUE);
	gtk_window_set_default_size(GTK_WINDOW(win), 360, 400);
	g_signal_connect(win, "destroy", G_CALLBACK(manager_destroy_cb), NULL);
	content = pidgin_dialog_get_content_area(win);

	label = gtk_label_new(NULL);
	markup = g_markup_printf_escaped("<b>%s</b>", _("Smiley"));
	gtk_label_set_markup(GTK_LABEL(label), markup);
	g_free(markup);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_box_append(GTK_BOX(content), label);

	mgr->store = g_list_store_new(PIDGIN_TYPE_ITEM);
	mgr->selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(mgr->store)));
	gtk_single_selection_set_autoselect(mgr->selection, FALSE);
	gtk_single_selection_set_can_unselect(mgr->selection, TRUE);
	g_signal_connect(mgr->selection, "selection-changed",
	                 G_CALLBACK(selection_changed_cb), NULL);

	factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(row_setup_cb), NULL);
	g_signal_connect(factory, "bind", G_CALLBACK(row_bind_cb), NULL);
	mgr->listview = gtk_list_view_new(
		GTK_SELECTION_MODEL(g_object_ref(mgr->selection)), factory);
	gtk_accessible_update_property(GTK_ACCESSIBLE(mgr->listview),
		GTK_ACCESSIBLE_PROPERTY_LABEL, _("Custom Smiley Manager"), -1);
	g_signal_connect(mgr->listview, "activate", G_CALLBACK(activate_cb), NULL);
	sw = pidgin_make_scrollable(mgr->listview, GTK_POLICY_NEVER,
	                            GTK_POLICY_AUTOMATIC, -1, 240);
	gtk_widget_add_css_class(sw, "frame");
	gtk_widget_set_vexpand(sw, TRUE);
	gtk_box_append(GTK_BOX(content), sw);

	pidgin_dialog_add_button(win, _("_Add..."), G_CALLBACK(add_cb), NULL);
	mgr->edit_button = pidgin_dialog_add_button(win, _("_Edit..."),
		G_CALLBACK(edit_cb), NULL);
	mgr->delete_button = pidgin_dialog_add_button(win, _("_Delete"),
		G_CALLBACK(delete_cb), NULL);
	pidgin_dialog_add_button(win, _("_Close"), G_CALLBACK(close_cb), NULL);

	refresh_list();
	update_buttons();
	gtk_window_present(GTK_WINDOW(win));
}

void
pidgin_smileys_init(void)
{
}

void
pidgin_smileys_uninit(void)
{
	while (editors != NULL) {
		SmileyEditor *ed = editors->data;
		gtk_window_destroy(GTK_WINDOW(ed->window));  /* frees and unlinks */
	}
	if (smiley_manager != NULL)
		gtk_window_destroy(GTK_WINDOW(smiley_manager->window));
}

/**************************************************************************
 * Selftest
 **************************************************************************/

#define MODULE "smileys"

/* The names and sizes in custom_smiley plus smileys.xml's size and mtime:
 * nothing may change. */
static char *
profile_state(void)
{
	GString *str = g_string_new(NULL);
	char *dirname = g_build_filename(purple_user_dir(), "custom_smiley", NULL);
	char *xml = g_build_filename(purple_user_dir(), "smileys.xml", NULL);
	GDir *dir = g_dir_open(dirname, 0, NULL);
	GStatBuf st;
	const char *name;

	if (dir != NULL) {
		GList *names = NULL, *l;

		while ((name = g_dir_read_name(dir)) != NULL)
			names = g_list_insert_sorted(names, g_strdup(name), (GCompareFunc)strcmp);
		g_dir_close(dir);
		for (l = names; l != NULL; l = l->next)
			g_string_append_printf(str, "%s\n", (char *)l->data);
		g_list_free_full(names, g_free);
	}
	if (g_stat(xml, &st) == 0)
		g_string_append_printf(str, "xml %" G_GINT64_FORMAT " %" G_GINT64_FORMAT "\n",
		                       (gint64)st.st_size, (gint64)st.st_mtime);
	g_free(dirname);
	g_free(xml);
	return g_string_free(str, FALSE);
}

void
pidgin_smileys_selftest(void)
{
	char *before = profile_state(), *after;
	GdkTexture *texture;
	GBytes *png, *pixels;
	guint8 rgba[16 * 16 * 4];
	SmileyEditor *ed;
	GList *all;
	guint n;
	gsize i;

	pidgin_smiley_manager_show();
	pidgin_selftest_iterate(100);
	if (smiley_manager == NULL) {
		pidgin_selftest_fail(MODULE, "the manager did not open");
		g_free(before);
		return;
	}
	all = purple_smileys_get_all();
	n = g_list_model_get_n_items(G_LIST_MODEL(smiley_manager->store));
	if (n != g_list_length(all))
		pidgin_selftest_fail(MODULE, "%u rows for %u smileys", n, g_list_length(all));
	pidgin_selftest_log(MODULE, "%u custom smileys", n);
	g_list_free(all);

	/* A new smiley: OK stays disabled without image and shortcut. */
	add_cb(NULL, NULL);
	pidgin_selftest_iterate(50);
	ed = editors ? editors->data : NULL;
	if (ed == NULL) {
		pidgin_selftest_fail(MODULE, "no editor");
	} else {
		if (gtk_widget_get_sensitive(ed->ok_button))
			pidgin_selftest_fail(MODULE, "Add is enabled on an empty editor");
		gtk_editable_set_text(GTK_EDITABLE(ed->entry), ":selftest:");
		if (gtk_widget_get_sensitive(ed->ok_button))
			pidgin_selftest_fail(MODULE, "Add is enabled without an image");
		cancel_cb(NULL, ed);
	}

	/* From image data (M4b's "Save as custom smiley"). */
	for (i = 0; i < sizeof(rgba); i += 4) {
		rgba[i] = 0xff; rgba[i + 1] = 0xc0; rgba[i + 2] = 0x20; rgba[i + 3] = 0xff;
	}
	pixels = g_bytes_new(rgba, sizeof(rgba));
	texture = gdk_memory_texture_new(16, 16, GDK_MEMORY_R8G8B8A8, pixels, 16 * 4);
	png = gdk_texture_save_to_png_bytes(texture);
	pidgin_smiley_add_from_image(NULL, g_bytes_get_data(png, NULL),
	                             g_bytes_get_size(png), ":fromimage:");
	pidgin_selftest_iterate(50);
	ed = editors ? editors->data : NULL;
	if (ed == NULL) {
		pidgin_selftest_fail(MODULE, "no editor from image");
	} else {
		if (gtk_picture_get_paintable(GTK_PICTURE(ed->picture)) == NULL)
			pidgin_selftest_fail(MODULE, "the image is not previewed");
		if (!gtk_widget_get_sensitive(ed->ok_button))
			pidgin_selftest_fail(MODULE, "Add is disabled with image and shortcut");
		cancel_cb(NULL, ed);
	}
	g_bytes_unref(png);
	g_bytes_unref(pixels);
	g_object_unref(texture);

	/* Edit the first smiley, if any. */
	if (n > 0) {
		gtk_single_selection_set_selected(smiley_manager->selection, 0);
		edit_cb(NULL, NULL);
		pidgin_selftest_iterate(50);
		ed = editors ? editors->data : NULL;
		if (ed == NULL || ed->original == NULL)
			pidgin_selftest_fail(MODULE, "no editor for the first smiley");
		else
			cancel_cb(NULL, ed);
	}

	pidgin_selftest_iterate(50);
	if (editors != NULL)
		pidgin_selftest_fail(MODULE, "editors left open");
	close_cb(NULL, NULL);
	pidgin_selftest_iterate(50);
	if (smiley_manager != NULL)
		pidgin_selftest_fail(MODULE, "the manager did not close");

	after = profile_state();
	if (!purple_strequal(before, after))
		pidgin_selftest_fail(MODULE, "the profile's smileys changed");
	g_free(before);
	g_free(after);
}
