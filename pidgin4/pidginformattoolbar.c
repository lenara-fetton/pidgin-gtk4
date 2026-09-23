/*
 * pidgin
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
#include "pidgin-internal.h"
#include "pidgin.h"

#include "debug.h"
#include "imgstore.h"
#include "smiley.h"
#include "util.h"

#include "pidginformattoolbar.h"
#include "pidginsmileytheme.h"

#define MAX_IMAGE_BYTES (8 * 1024 * 1024)

struct _PidginFormatToolbar
{
	GtkWidget parent;

	PidginComposeEntry *entry;      /* weak */
	gulong format_id;

	GtkWidget *bold, *italic, *underline, *strike, *code;
	GtkWidget *smaller, *larger, *font, *fore, *back;
	GtkWidget *link, *image, *smiley, *reset;
	GtkWidget *attach, *attention;
	GtkWidget *smiley_popover;
	GtkWidget *smiley_grid;
	GtkWidget *link_popover;
	GtkWidget *link_url, *link_desc;
	gboolean syncing;
};

G_DEFINE_FINAL_TYPE(PidginFormatToolbar, pidgin_format_toolbar, GTK_TYPE_WIDGET)

static GtkWindow *
parent_window(PidginFormatToolbar *tb)
{
	GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(tb));
	return GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;
}

static void
refocus(PidginFormatToolbar *tb)
{
	if (tb->entry)
		gtk_widget_grab_focus(GTK_WIDGET(tb->entry));
}

/**************************************************************************
 * Toggles
 **************************************************************************/

static void
sync_toggles(PidginFormatToolbar *tb)
{
	struct { GtkWidget *button; PidginFormatCaps cap; } toggles[] = {
		{ tb->bold, PIDGIN_FORMAT_BOLD }, { tb->italic, PIDGIN_FORMAT_ITALIC },
		{ tb->underline, PIDGIN_FORMAT_UNDERLINE }, { tb->strike, PIDGIN_FORMAT_STRIKE },
		{ tb->code, PIDGIN_FORMAT_CODE },
	};
	guint i;

	if (tb->entry == NULL)
		return;
	tb->syncing = TRUE;
	for (i = 0; i < G_N_ELEMENTS(toggles); i++)
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggles[i].button),
			pidgin_compose_entry_get_format(tb->entry, toggles[i].cap));
	tb->syncing = FALSE;
}

static void
toggle_cb(GtkToggleButton *button, PidginFormatToolbar *tb)
{
	if (tb->syncing || tb->entry == NULL)
		return;
	if (GTK_WIDGET(button) == tb->bold)
		pidgin_compose_entry_toggle_bold(tb->entry);
	else if (GTK_WIDGET(button) == tb->italic)
		pidgin_compose_entry_toggle_italic(tb->entry);
	else if (GTK_WIDGET(button) == tb->underline)
		pidgin_compose_entry_toggle_underline(tb->entry);
	else if (GTK_WIDGET(button) == tb->strike)
		pidgin_compose_entry_toggle_strike(tb->entry);
	else if (GTK_WIDGET(button) == tb->code)
		pidgin_compose_entry_toggle_code(tb->entry);
	sync_toggles(tb);
	refocus(tb);
}

static void
format_changed_cb(PidginComposeEntry *entry, PidginFormatToolbar *tb)
{
	/* the capabilities may have changed too (set_caps emits it) */
	pidgin_format_toolbar_update(tb);
}

/**************************************************************************
 * Buttons
 **************************************************************************/

static void
smaller_cb(GtkButton *b, PidginFormatToolbar *tb)
{
	if (tb->entry)
		pidgin_compose_entry_shrink_font(tb->entry);
	refocus(tb);
}

static void
larger_cb(GtkButton *b, PidginFormatToolbar *tb)
{
	if (tb->entry)
		pidgin_compose_entry_grow_font(tb->entry);
	refocus(tb);
}

static void
reset_cb(GtkButton *b, PidginFormatToolbar *tb)
{
	if (tb->entry)
		pidgin_compose_entry_clear_formatting(tb->entry);
	refocus(tb);
}

static void
font_chosen_cb(GObject *source, GAsyncResult *res, gpointer data)
{
	PidginFormatToolbar *tb = data;
	GError *error = NULL;
	PangoFontFamily *family = gtk_font_dialog_choose_family_finish(GTK_FONT_DIALOG(source),
	                                                               res, &error);

	if (family != NULL && tb->entry != NULL)
		pidgin_compose_entry_set_font_face(tb->entry, pango_font_family_get_name(family));
	g_clear_object(&family);
	g_clear_error(&error);
	refocus(tb);
	g_object_unref(tb);
}

static void
font_cb(GtkButton *b, PidginFormatToolbar *tb)
{
	GtkFontDialog *dialog = gtk_font_dialog_new();

	gtk_font_dialog_set_title(dialog, _("Select Font"));
	gtk_font_dialog_choose_family(dialog, parent_window(tb), NULL, NULL,
	                              font_chosen_cb, g_object_ref(tb));
	g_object_unref(dialog);
}

static void
color_chosen_cb(GObject *source, GAsyncResult *res, gpointer data)
{
	PidginFormatToolbar *tb = data;
	GError *error = NULL;
	GdkRGBA *rgba = gtk_color_dialog_choose_rgba_finish(GTK_COLOR_DIALOG(source), res, &error);
	gboolean back = GPOINTER_TO_INT(g_object_get_data(source, "back"));

	if (rgba != NULL && tb->entry != NULL) {
		if (back)
			pidgin_compose_entry_set_backcolor(tb->entry, rgba);
		else
			pidgin_compose_entry_set_forecolor(tb->entry, rgba);
	}
	if (rgba)
		gdk_rgba_free(rgba);
	g_clear_error(&error);
	refocus(tb);
	g_object_unref(tb);
}

static void
color_cb(GtkButton *b, PidginFormatToolbar *tb)
{
	GtkColorDialog *dialog = gtk_color_dialog_new();
	gboolean back = GTK_WIDGET(b) == tb->back;

	gtk_color_dialog_set_title(dialog, back ? _("Select Background Color") :
	                                          _("Select Text Color"));
	gtk_color_dialog_set_with_alpha(dialog, FALSE);
	g_object_set_data(G_OBJECT(dialog), "back", GINT_TO_POINTER(back));
	gtk_color_dialog_choose_rgba(dialog, parent_window(tb), NULL, NULL,
	                             color_chosen_cb, g_object_ref(tb));
	g_object_unref(dialog);
}

/* ---- link ---- */

static void
link_insert_cb(GtkWidget *w, PidginFormatToolbar *tb)
{
	const char *url = gtk_editable_get_text(GTK_EDITABLE(tb->link_url));
	const char *desc = gtk_editable_get_text(GTK_EDITABLE(tb->link_desc));

	if (url && *url && tb->entry != NULL)
		pidgin_compose_entry_insert_link(tb->entry, url, (desc && *desc) ? desc : NULL);
	gtk_popover_popdown(GTK_POPOVER(tb->link_popover));
	gtk_editable_set_text(GTK_EDITABLE(tb->link_url), "");
	gtk_editable_set_text(GTK_EDITABLE(tb->link_desc), "");
	refocus(tb);
}

static GtkWidget *
build_link_popover(PidginFormatToolbar *tb)
{
	GtkWidget *popover = gtk_popover_new();
	GtkWidget *grid = gtk_grid_new();
	GtkWidget *label, *button;

	gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 6);

	label = gtk_label_new_with_mnemonic(_("_URL"));
	gtk_label_set_xalign(GTK_LABEL(label), 1);
	tb->link_url = gtk_entry_new();
	gtk_label_set_mnemonic_widget(GTK_LABEL(label), tb->link_url);
	gtk_grid_attach(GTK_GRID(grid), label, 0, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), tb->link_url, 1, 0, 1, 1);

	label = gtk_label_new_with_mnemonic(_("_Description"));
	gtk_label_set_xalign(GTK_LABEL(label), 1);
	tb->link_desc = gtk_entry_new();
	gtk_label_set_mnemonic_widget(GTK_LABEL(label), tb->link_desc);
	gtk_grid_attach(GTK_GRID(grid), label, 0, 1, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), tb->link_desc, 1, 1, 1, 1);
	g_object_set_data(G_OBJECT(popover), "desc-label", label);

	button = gtk_button_new_with_mnemonic(_("_Insert"));
	gtk_widget_add_css_class(button, "suggested-action");
	gtk_widget_set_halign(button, GTK_ALIGN_END);
	g_signal_connect(button, "clicked", G_CALLBACK(link_insert_cb), tb);
	g_signal_connect(tb->link_url, "activate", G_CALLBACK(link_insert_cb), tb);
	g_signal_connect(tb->link_desc, "activate", G_CALLBACK(link_insert_cb), tb);
	gtk_grid_attach(GTK_GRID(grid), button, 1, 2, 1, 1);

	gtk_popover_set_child(GTK_POPOVER(popover), grid);
	return popover;
}

/* ---- image ---- */

static void
image_chosen_cb(GObject *source, GAsyncResult *res, gpointer data)
{
	PidginFormatToolbar *tb = data;
	GError *error = NULL;
	GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), res, &error);
	char *contents = NULL, *path = NULL, *base;
	gsize len = 0;

	if (file == NULL) {
		g_clear_error(&error);
		g_object_unref(tb);
		return;
	}
	path = g_file_get_path(file);
	if (path != NULL && g_file_get_contents(path, &contents, &len, &error) &&
	    len > 0 && len <= MAX_IMAGE_BYTES && tb->entry != NULL) {
		int id;
		base = g_path_get_basename(path);
		/* imgstore takes ownership of contents */
		id = purple_imgstore_add_with_id(contents, len, base);
		g_free(base);
		if (id > 0) {
			pidgin_compose_entry_insert_image(tb->entry, id);
			purple_imgstore_unref_by_id(id);    /* the entry holds its own */
		}
	} else {
		purple_debug_warning("formattoolbar", "Could not insert image %s: %s\n",
		                     path ? path : "?", error ? error->message : "too large");
		g_free(contents);
	}
	g_clear_error(&error);
	g_free(path);
	g_object_unref(file);
	refocus(tb);
	g_object_unref(tb);
}

static void
image_cb(GtkButton *b, PidginFormatToolbar *tb)
{
	GtkFileDialog *dialog = gtk_file_dialog_new();
	GtkFileFilter *filter = gtk_file_filter_new();
	GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);

	gtk_file_filter_set_name(filter, _("Images"));
	gtk_file_filter_add_pixbuf_formats(filter);
	g_list_store_append(filters, filter);
	gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
	gtk_file_dialog_set_title(dialog, _("Insert Image"));
	gtk_file_dialog_open(dialog, parent_window(tb), NULL, image_chosen_cb, g_object_ref(tb));
	g_object_unref(filters);
	g_object_unref(filter);
	g_object_unref(dialog);
}

/* ---- smileys ---- */

static void
smiley_clicked_cb(GtkButton *button, PidginFormatToolbar *tb)
{
	const char *shortcut = g_object_get_data(G_OBJECT(button), "shortcut");

	if (tb->entry != NULL)
		pidgin_compose_entry_insert_smiley(tb->entry, shortcut);
	gtk_popover_popdown(GTK_POPOVER(tb->smiley_popover));
	refocus(tb);
}

static void
add_smiley_button(PidginFormatToolbar *tb, GdkPaintable *paintable, const char *shortcut,
                  GHashTable *seen_files, const char *file)
{
	GtkWidget *button, *picture;

	/* one button per image, as gtkimhtmltoolbar did */
	if (file != NULL) {
		if (g_hash_table_contains(seen_files, file))
			return;
		g_hash_table_add(seen_files, g_strdup(file));
	}
	if (paintable == NULL)
		return;

	picture = gtk_picture_new_for_paintable(paintable);
	gtk_widget_set_size_request(picture, 24, 24);
	gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);
	button = gtk_button_new();
	gtk_button_set_child(GTK_BUTTON(button), picture);
	gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
	gtk_widget_set_tooltip_text(button, shortcut);
	g_object_set_data_full(G_OBJECT(button), "shortcut", g_strdup(shortcut), g_free);
	g_signal_connect(button, "clicked", G_CALLBACK(smiley_clicked_cb), tb);
	gtk_flow_box_append(GTK_FLOW_BOX(tb->smiley_grid), button);
}

static void
build_smileys(PidginFormatToolbar *tb)
{
	GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	GtkWidget *child;
	GSList *l;
	GList *custom, *c;
	PidginFormatCaps caps = tb->entry ? pidgin_compose_entry_get_caps(tb->entry) : 0;

	while ((child = gtk_widget_get_first_child(tb->smiley_grid)) != NULL)
		gtk_flow_box_remove(GTK_FLOW_BOX(tb->smiley_grid), child);

	for (l = pidgin_smiley_theme_get_smileys(NULL); l; l = l->next) {
		PidginSmiley *s = l->data;
		if (pidgin_smiley_is_hidden(s))
			continue;
		add_smiley_button(tb, pidgin_smiley_get_paintable(s),
		                  pidgin_smiley_get_shortcut(s), seen, pidgin_smiley_get_file(s));
	}
	if (caps & PIDGIN_FORMAT_CUSTOM_SMILEY) {
		custom = purple_smileys_get_all();
		for (c = custom; c; c = c->next)
			add_smiley_button(tb, pidgin_custom_smiley_get_paintable(c->data),
			                  purple_smiley_get_shortcut(c->data), seen, NULL);
		g_list_free(custom);
	}
	if (gtk_widget_get_first_child(tb->smiley_grid) == NULL) {
		GtkWidget *label = gtk_label_new(_("This theme has no available smileys."));
		gtk_flow_box_append(GTK_FLOW_BOX(tb->smiley_grid), label);
	}
	g_hash_table_destroy(seen);
}

static void
smiley_popover_show_cb(GtkWidget *popover, PidginFormatToolbar *tb)
{
	build_smileys(tb);
}

/**************************************************************************
 * Layout
 **************************************************************************/

static GtkWidget *
make_toggle(PidginFormatToolbar *tb, const char *icon, const char *tooltip)
{
	GtkWidget *b = gtk_toggle_button_new();

	gtk_button_set_icon_name(GTK_BUTTON(b), icon);
	gtk_widget_set_tooltip_text(b, tooltip);
	gtk_widget_set_focus_on_click(b, FALSE);
	gtk_button_set_has_frame(GTK_BUTTON(b), FALSE);
	g_signal_connect(b, "toggled", G_CALLBACK(toggle_cb), tb);
	gtk_widget_set_parent(b, GTK_WIDGET(tb));
	return b;
}

static GtkWidget *
make_button(PidginFormatToolbar *tb, const char *icon, const char *markup,
            const char *tooltip, GCallback cb)
{
	GtkWidget *b = gtk_button_new();

	if (icon != NULL) {
		gtk_button_set_icon_name(GTK_BUTTON(b), icon);
	} else {
		GtkWidget *label = gtk_label_new(NULL);
		gtk_label_set_markup(GTK_LABEL(label), markup);
		gtk_button_set_child(GTK_BUTTON(b), label);
	}
	gtk_widget_set_tooltip_text(b, tooltip);
	gtk_widget_set_focus_on_click(b, FALSE);
	gtk_button_set_has_frame(GTK_BUTTON(b), FALSE);
	if (cb)
		g_signal_connect(b, "clicked", cb, tb);
	gtk_widget_set_parent(b, GTK_WIDGET(tb));
	return b;
}

void
pidgin_format_toolbar_update(PidginFormatToolbar *tb)
{
	PidginFormatCaps caps;

	g_return_if_fail(PIDGIN_IS_FORMAT_TOOLBAR(tb));

	caps = tb->entry ? pidgin_compose_entry_get_caps(tb->entry) : 0;
	gtk_widget_set_visible(tb->bold, caps & PIDGIN_FORMAT_BOLD);
	gtk_widget_set_visible(tb->italic, caps & PIDGIN_FORMAT_ITALIC);
	gtk_widget_set_visible(tb->underline, caps & PIDGIN_FORMAT_UNDERLINE);
	gtk_widget_set_visible(tb->strike, caps & PIDGIN_FORMAT_STRIKE);
	gtk_widget_set_visible(tb->code, caps & PIDGIN_FORMAT_CODE);
	gtk_widget_set_visible(tb->smaller, caps & PIDGIN_FORMAT_SIZE);
	gtk_widget_set_visible(tb->larger, caps & PIDGIN_FORMAT_SIZE);
	gtk_widget_set_visible(tb->font, caps & PIDGIN_FORMAT_FACE);
	gtk_widget_set_visible(tb->fore, caps & PIDGIN_FORMAT_FORECOLOR);
	gtk_widget_set_visible(tb->back, caps & PIDGIN_FORMAT_BACKCOLOR);
	gtk_widget_set_visible(tb->link, caps & PIDGIN_FORMAT_LINK);
	gtk_widget_set_visible(tb->image, caps & PIDGIN_FORMAT_IMAGE);
	gtk_widget_set_visible(tb->smiley, (caps & (PIDGIN_FORMAT_SMILEY | PIDGIN_FORMAT_CUSTOM_SMILEY)) &&
	                       (!pidgin_smiley_themes_disabled() ||
	                        (caps & PIDGIN_FORMAT_CUSTOM_SMILEY)));
	gtk_widget_set_visible(tb->reset, caps & (PIDGIN_FORMAT_BOLD | PIDGIN_FORMAT_SIZE |
	                       PIDGIN_FORMAT_FACE | PIDGIN_FORMAT_FORECOLOR));
	if (tb->link_popover) {
		GtkWidget *desc_label = g_object_get_data(G_OBJECT(tb->link_popover), "desc-label");
		gtk_widget_set_visible(tb->link_desc, caps & PIDGIN_FORMAT_LINKDESC);
		gtk_widget_set_visible(desc_label, caps & PIDGIN_FORMAT_LINKDESC);
	}
	sync_toggles(tb);
}

void
pidgin_format_toolbar_set_entry(PidginFormatToolbar *tb, PidginComposeEntry *entry)
{
	g_return_if_fail(PIDGIN_IS_FORMAT_TOOLBAR(tb));

	if (tb->entry != NULL)
		g_clear_signal_handler(&tb->format_id, tb->entry);
	g_set_weak_pointer(&tb->entry, entry);
	if (entry != NULL)
		tb->format_id = g_signal_connect(entry, "format-changed",
		                                 G_CALLBACK(format_changed_cb), tb);
	pidgin_format_toolbar_update(tb);
}

PidginComposeEntry *
pidgin_format_toolbar_get_entry(PidginFormatToolbar *tb)
{
	g_return_val_if_fail(PIDGIN_IS_FORMAT_TOOLBAR(tb), NULL);
	return tb->entry;
}

void
pidgin_format_toolbar_set_show_attach(PidginFormatToolbar *tb, gboolean show)
{
	g_return_if_fail(PIDGIN_IS_FORMAT_TOOLBAR(tb));
	gtk_widget_set_visible(tb->attach, show);
}

GtkWidget *
pidgin_format_toolbar_get_attach_button(PidginFormatToolbar *tb)
{
	g_return_val_if_fail(PIDGIN_IS_FORMAT_TOOLBAR(tb), NULL);
	return tb->attach;
}

void
pidgin_format_toolbar_set_show_attention(PidginFormatToolbar *tb, gboolean show)
{
	g_return_if_fail(PIDGIN_IS_FORMAT_TOOLBAR(tb));
	gtk_widget_set_visible(tb->attention, show);
}

GtkWidget *
pidgin_format_toolbar_get_attention_button(PidginFormatToolbar *tb)
{
	g_return_val_if_fail(PIDGIN_IS_FORMAT_TOOLBAR(tb), NULL);
	return tb->attention;
}

GtkWidget *
pidgin_format_toolbar_get_smiley_grid(PidginFormatToolbar *tb)
{
	g_return_val_if_fail(PIDGIN_IS_FORMAT_TOOLBAR(tb), NULL);
	build_smileys(tb);
	return tb->smiley_grid;
}

static void
pidgin_format_toolbar_dispose(GObject *obj)
{
	PidginFormatToolbar *tb = PIDGIN_FORMAT_TOOLBAR(obj);
	GtkWidget *child;

	if (tb->entry != NULL)
		g_clear_signal_handler(&tb->format_id, tb->entry);
	g_clear_weak_pointer(&tb->entry);
	while ((child = gtk_widget_get_first_child(GTK_WIDGET(tb))) != NULL)
		gtk_widget_unparent(child);

	G_OBJECT_CLASS(pidgin_format_toolbar_parent_class)->dispose(obj);
}

static void
pidgin_format_toolbar_class_init(PidginFormatToolbarClass *klass)
{
	G_OBJECT_CLASS(klass)->dispose = pidgin_format_toolbar_dispose;
	gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(klass), GTK_TYPE_BOX_LAYOUT);
	gtk_widget_class_set_css_name(GTK_WIDGET_CLASS(klass), "pidgin-format-toolbar");
}

static void
pidgin_format_toolbar_init(PidginFormatToolbar *tb)
{
	GtkWidget *scroll;

	gtk_box_layout_set_spacing(GTK_BOX_LAYOUT(gtk_widget_get_layout_manager(GTK_WIDGET(tb))), 2);
	gtk_widget_add_css_class(GTK_WIDGET(tb), "toolbar");

	tb->bold = make_toggle(tb, "format-text-bold-symbolic", _("Bold"));
	tb->italic = make_toggle(tb, "format-text-italic-symbolic", _("Italic"));
	tb->underline = make_toggle(tb, "format-text-underline-symbolic", _("Underline"));
	tb->strike = make_toggle(tb, "format-text-strikethrough-symbolic", _("Strikethrough"));
	tb->code = make_toggle(tb, "text-x-script-symbolic", _("Code"));
	tb->smaller = make_button(tb, NULL, "<small>A</small>", _("Decrease font size"),
	                          G_CALLBACK(smaller_cb));
	tb->larger = make_button(tb, NULL, "<big>A</big>", _("Increase font size"),
	                         G_CALLBACK(larger_cb));
	tb->font = make_button(tb, "font-select-symbolic", NULL, _("Font face"), G_CALLBACK(font_cb));
	tb->fore = make_button(tb, NULL, "<span foreground=\"#cc0000\" underline=\"single\">A</span>",
	                       _("Foreground font color"), G_CALLBACK(color_cb));
	tb->back = make_button(tb, NULL, "<span background=\"#fce94f\">A</span>",
	                       _("Background color"), G_CALLBACK(color_cb));

	tb->link = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(tb->link), "insert-link-symbolic");
	gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(tb->link), FALSE);
	gtk_widget_set_tooltip_text(tb->link, _("Insert link"));
	tb->link_popover = build_link_popover(tb);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(tb->link), tb->link_popover);
	gtk_widget_set_parent(tb->link, GTK_WIDGET(tb));

	tb->image = make_button(tb, "insert-image-symbolic", NULL, _("Insert IM image"),
	                        G_CALLBACK(image_cb));

	tb->smiley = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(tb->smiley), "face-smile-symbolic");
	gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(tb->smiley), FALSE);
	gtk_widget_set_tooltip_text(tb->smiley, _("Insert smiley"));
	tb->smiley_grid = gtk_flow_box_new();
	gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(tb->smiley_grid), GTK_SELECTION_NONE);
	gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(tb->smiley_grid), 10);
	scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), tb->smiley_grid);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
	gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroll), TRUE);
	gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 300);
	tb->smiley_popover = gtk_popover_new();
	gtk_popover_set_child(GTK_POPOVER(tb->smiley_popover), scroll);
	g_signal_connect(tb->smiley_popover, "show", G_CALLBACK(smiley_popover_show_cb), tb);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(tb->smiley), tb->smiley_popover);
	gtk_widget_set_parent(tb->smiley, GTK_WIDGET(tb));

	tb->reset = make_button(tb, "edit-clear-all-symbolic", NULL, _("Reset formatting"),
	                        G_CALLBACK(reset_cb));

	/* Attach: the conversation window's conv.send-file action (the file
	 * chooser, then serv_send_file / serv_chat_send_file); shown by the
	 * conversation when its prpl can send it a file. */
	tb->attach = make_button(tb, "mail-attachment-symbolic", NULL, _("Send File"), NULL);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(tb->attach), "conv.send-file");
	gtk_widget_set_visible(tb->attach, FALSE);

	/* Pidgin 2's "Attention!" button: the conversation window's
	 * conv.get-attention action; shown by the conversation for prpls
	 * with send_attention. */
	tb->attention = make_button(tb, "preferences-system-notifications-symbolic", NULL,
	                            _("Get Attention"), NULL);
	{
		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

		gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(
			"preferences-system-notifications-symbolic"));
		gtk_box_append(GTK_BOX(box), gtk_label_new(_("Attention!")));
		gtk_button_set_child(GTK_BUTTON(tb->attention), box);
	}
	gtk_actionable_set_action_name(GTK_ACTIONABLE(tb->attention), "conv.get-attention");
	gtk_widget_set_visible(tb->attention, FALSE);
}

GtkWidget *
pidgin_format_toolbar_new(PidginComposeEntry *entry)
{
	PidginFormatToolbar *tb = g_object_new(PIDGIN_TYPE_FORMAT_TOOLBAR, NULL);

	pidgin_format_toolbar_set_entry(tb, entry);
	return GTK_WIDGET(tb);
}
