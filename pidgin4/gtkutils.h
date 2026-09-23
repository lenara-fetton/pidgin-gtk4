/**
 * @file gtkutils.h GTK 4 utility functions
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
 * The pidgin4 subset of pidgin/gtkutils.h. Only what the M2 files need is
 * here; the rest of the GTK 2 file is ported by the milestone that needs it:
 * buddy icon choosers and scaling (M3), IMHtml/smiley/link helpers (M4),
 * the screenname autocomplete and DnD helpers (M3/M5).
 */
#ifndef _PIDGINUTILS_H_
#define _PIDGINUTILS_H_

#include "pidgin.h"

#include "account.h"
#include "imgstore.h"
#include "plugin.h"
#include "prpl.h"

typedef enum
{
	PIDGIN_PRPL_ICON_SMALL,    /* 16 px */
	PIDGIN_PRPL_ICON_MEDIUM,   /* 22 px */
	PIDGIN_PRPL_ICON_LARGE     /* 48 px */
} PidginPrplIconSize;

/**************************************************************************
 * PidginItem: a small GObject for GListModels (drop-downs, list views)
 **************************************************************************/

#define PIDGIN_TYPE_ITEM (pidgin_item_get_type())
G_DECLARE_FINAL_TYPE(PidginItem, pidgin_item, PIDGIN, ITEM, GObject)

/**
 * Creates an item with a label (property "label"), an optional string id
 * (property "id") and an opaque data pointer that is not owned.
 */
PidginItem *pidgin_item_new(const char *label, const char *id, gpointer data);
const char *pidgin_item_get_label(PidginItem *item);
const char *pidgin_item_get_id(PidginItem *item);
gpointer pidgin_item_get_data(PidginItem *item);
/** Property "icon" (a GIcon, may be NULL). */
void pidgin_item_set_icon(PidginItem *item, GIcon *icon);
GIcon *pidgin_item_get_icon(PidginItem *item);

/**
 * A GtkDropDown over a GListModel of PidginItem, showing each item's icon
 * and label and searchable by label. Takes ownership of @model.
 */
GtkWidget *pidgin_item_dropdown_new(GListModel *model);
PidginItem *pidgin_item_dropdown_get_selected_item(GtkWidget *dropdown);
gpointer pidgin_item_dropdown_get_selected_data(GtkWidget *dropdown);
const char *pidgin_item_dropdown_get_selected_id(GtkWidget *dropdown);
/** Selects the first item whose data is @data. Returns FALSE if none. */
gboolean pidgin_item_dropdown_select_data(GtkWidget *dropdown, gpointer data);
/** Selects the first item whose id is @id. Returns FALSE if none. */
gboolean pidgin_item_dropdown_select_id(GtkWidget *dropdown, const char *id);

/**************************************************************************
 * Account and protocol pickers (replace the GTK 2 option menus)
 **************************************************************************/

/**
 * A drop-down of accounts. With @show_all FALSE only connected accounts are
 * listed. @filter may be NULL. Watch "notify::selected" for changes.
 */
GtkWidget *pidgin_account_dropdown_new(PurpleAccount *default_account,
                                       gboolean show_all,
                                       PurpleFilterAccountFunc filter_func,
                                       gpointer user_data);
PurpleAccount *pidgin_account_dropdown_get_selected(GtkWidget *dropdown);
void pidgin_account_dropdown_set_selected(GtkWidget *dropdown,
                                          PurpleAccount *account);

/**
 * A non-editable drop-down of the loaded protocol plugins, sorted by name,
 * with @default_id selected (or the first one).
 */
GtkWidget *pidgin_protocol_dropdown_new(const char *default_id);
PurplePlugin *pidgin_protocol_dropdown_get_selected(GtkWidget *dropdown);
const char *pidgin_protocol_dropdown_get_selected_id(GtkWidget *dropdown);
gboolean pidgin_protocol_dropdown_set_selected_id(GtkWidget *dropdown,
                                                  const char *id);

/**************************************************************************
 * Icons and images
 **************************************************************************/

int pidgin_prpl_icon_size_to_pixels(PidginPrplIconSize size);

/**
 * Returns the protocol icon for an account or a prpl (either may be NULL,
 * but not both) as a GIcon: the GResource icon "pidgin4-protocol-<name>",
 * a <name>.png/.svg from a Pidgin 2 pixmaps/pidgin/protocols directory
 * (for third-party prpls), or a generic fallback. Never returns NULL.
 */
GIcon *pidgin_create_prpl_gicon(PurpleAccount *account, PurplePlugin *prpl);

/** A GtkImage of pidgin_create_prpl_gicon() at @size. */
GtkWidget *pidgin_create_prpl_image(PurpleAccount *account,
                                    PurplePlugin *prpl,
                                    PidginPrplIconSize size);

/** Decodes image data (PNG, JPEG, ...). Returns NULL on failure. */
GdkTexture *pidgin_texture_new_from_data(gconstpointer data, gsize len);

/** Decodes a stored image. Returns NULL on failure. */
GdkTexture *pidgin_texture_new_from_imgstore(PurpleStoredImage *image);

/**************************************************************************
 * Windows and dialogs (plain GtkWindows; never gtk_dialog_run)
 **************************************************************************/

/**
 * Returns the application's active window, or NULL. Replaces
 * pidgin_auto_parent_window(); use it as the transient parent of dialogs.
 */
GtkWindow *pidgin_get_active_window(void);

/**
 * Creates a dialog-like toplevel GtkWindow: attached to the application,
 * transient for @parent (may be NULL), closed by Escape, with a vertical
 * content box and a right-aligned button row. Present it with
 * gtk_window_present(). @role is only used as the widget name.
 */
GtkWidget *pidgin_dialog_new(const char *title, GtkWindow *parent,
                             const char *role, gboolean resizable);

/** The vertical GtkBox that holds the dialog's content. */
GtkWidget *pidgin_dialog_get_content_area(GtkWidget *dialog);

/** The horizontal GtkBox that holds the dialog's buttons. */
GtkWidget *pidgin_dialog_get_action_area(GtkWidget *dialog);

/**
 * Appends a button with a mnemonic @label to the dialog's button row.
 * @callback has the GtkButton "clicked" signature and may be NULL.
 */
GtkWidget *pidgin_dialog_add_button(GtkWidget *dialog, const char *label,
                                    GCallback callback, gpointer data);

/**
 * Adds an icon (icon name, may be NULL), a bold @primary line and a
 * @secondary line (either may be NULL) to the top of the content area.
 * If @secondary_markup is TRUE, @secondary is Pango markup.
 */
GtkWidget *pidgin_dialog_add_message(GtkWidget *dialog, const char *icon_name,
                                     const char *primary,
                                     const char *secondary,
                                     gboolean secondary_markup);

/**
 * Wraps @child in a GtkScrolledWindow with the given policies and minimum
 * content size (-1 for none).
 */
GtkWidget *pidgin_make_scrollable(GtkWidget *child, GtkPolicyType hscroll,
                                  GtkPolicyType vscroll, int width, int height);

/**
 * Appends a titled section to the vertical box @parent and returns the
 * section's inner vertical box.
 */
GtkWidget *pidgin_make_frame(GtkWidget *parent, const char *title);

/**
 * Appends a row "label: widget" to the vertical box @vbox. @sg (may be
 * NULL) aligns the labels. Returns the row; @p_label receives the label.
 */
GtkWidget *pidgin_add_widget_to_vbox(GtkBox *vbox, const char *widget_label,
                                     GtkSizeGroup *sg, GtkWidget *widget,
                                     gboolean expand, GtkWidget **p_label);

/** Marks @label as the accessible label of @w and its mnemonic widget. */
void pidgin_set_accessible_label(GtkWidget *w, GtkWidget *label);

/**
 * Converts purple's limited HTML (as used in notify/request text) to Pango
 * markup, dropping what Pango cannot show. For text that will become rich
 * in M4 (PidginRichLabel).
 */
char *pidgin_html_to_pango_markup(const char *html);

/**************************************************************************
 * Misc
 **************************************************************************/

/** Opens @uri with the desktop's handler (GtkUriLauncher, portal-aware). */
void pidgin_open_uri(GtkWindow *parent, const char *uri);

void pidgin_utils_init(void);
void pidgin_utils_uninit(void);

/**************************************************************************
 * Buddy icons (M3)
 **************************************************************************/

/**
 * Reads the image at @path and converts it to what @plugin's icon_spec
 * accepts (format, dimensions, file size), as Pidgin 2 did. Returns the
 * image data (g_free() it) and its length in @len, or NULL.
 */
gpointer pidgin_convert_buddy_icon(PurplePlugin *plugin, const char *path, size_t *len);

#endif /* _PIDGINUTILS_H_ */
