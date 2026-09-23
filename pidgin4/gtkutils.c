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

#include "account.h"
#include "buddyicon.h"
#include "connection.h"
#include "debug.h"
#include "notify.h"
#include "plugin.h"
#include "prefs.h"
#include "prpl.h"
#include "util.h"

#include "gtkutils.h"
#include "pidgincomposeentry.h"
#include "pidginformattoolbar.h"
#include "pidginmarkup.h"
#include "pidginmessageindex.h"
#include "pidginmessageview.h"
#include "pidginsmileytheme.h"

/**************************************************************************
 * PidginItem
 **************************************************************************/

struct _PidginItem {
	GObject parent;

	char *label;
	char *id;
	GIcon *icon;
	gpointer data;
};

enum {
	ITEM_PROP_0,
	ITEM_PROP_LABEL,
	ITEM_PROP_ID,
	ITEM_PROP_ICON,
	ITEM_N_PROPS
};

static GParamSpec *item_props[ITEM_N_PROPS];

G_DEFINE_FINAL_TYPE(PidginItem, pidgin_item, G_TYPE_OBJECT)

static void
pidgin_item_get_property(GObject *obj, guint prop_id, GValue *value,
                         GParamSpec *pspec)
{
	PidginItem *item = PIDGIN_ITEM(obj);

	switch (prop_id) {
	case ITEM_PROP_LABEL:
		g_value_set_string(value, item->label);
		break;
	case ITEM_PROP_ID:
		g_value_set_string(value, item->id);
		break;
	case ITEM_PROP_ICON:
		g_value_set_object(value, item->icon);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
	}
}

static void
pidgin_item_set_property(GObject *obj, guint prop_id, const GValue *value,
                         GParamSpec *pspec)
{
	PidginItem *item = PIDGIN_ITEM(obj);

	switch (prop_id) {
	case ITEM_PROP_LABEL:
		g_free(item->label);
		item->label = g_value_dup_string(value);
		break;
	case ITEM_PROP_ID:
		g_free(item->id);
		item->id = g_value_dup_string(value);
		break;
	case ITEM_PROP_ICON:
		pidgin_item_set_icon(item, g_value_get_object(value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
	}
}

static void
pidgin_item_finalize(GObject *obj)
{
	PidginItem *item = PIDGIN_ITEM(obj);

	g_free(item->label);
	g_free(item->id);
	g_clear_object(&item->icon);

	G_OBJECT_CLASS(pidgin_item_parent_class)->finalize(obj);
}

static void
pidgin_item_class_init(PidginItemClass *klass)
{
	GObjectClass *obj_class = G_OBJECT_CLASS(klass);

	obj_class->get_property = pidgin_item_get_property;
	obj_class->set_property = pidgin_item_set_property;
	obj_class->finalize = pidgin_item_finalize;

	item_props[ITEM_PROP_LABEL] = g_param_spec_string("label", NULL, NULL,
		NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	item_props[ITEM_PROP_ID] = g_param_spec_string("id", NULL, NULL,
		NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	item_props[ITEM_PROP_ICON] = g_param_spec_object("icon", NULL, NULL,
		G_TYPE_ICON, G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY |
		G_PARAM_STATIC_STRINGS);
	g_object_class_install_properties(obj_class, ITEM_N_PROPS, item_props);
}

static void
pidgin_item_init(PidginItem *item)
{
}

PidginItem *
pidgin_item_new(const char *label, const char *id, gpointer data)
{
	PidginItem *item = g_object_new(PIDGIN_TYPE_ITEM,
	                                "label", label, "id", id, NULL);

	item->data = data;
	return item;
}

const char *
pidgin_item_get_label(PidginItem *item)
{
	g_return_val_if_fail(PIDGIN_IS_ITEM(item), NULL);
	return item->label;
}

const char *
pidgin_item_get_id(PidginItem *item)
{
	g_return_val_if_fail(PIDGIN_IS_ITEM(item), NULL);
	return item->id;
}

gpointer
pidgin_item_get_data(PidginItem *item)
{
	g_return_val_if_fail(PIDGIN_IS_ITEM(item), NULL);
	return item->data;
}

void
pidgin_item_set_icon(PidginItem *item, GIcon *icon)
{
	g_return_if_fail(PIDGIN_IS_ITEM(item));

	if (g_set_object(&item->icon, icon))
		g_object_notify_by_pspec(G_OBJECT(item), item_props[ITEM_PROP_ICON]);
}

GIcon *
pidgin_item_get_icon(PidginItem *item)
{
	g_return_val_if_fail(PIDGIN_IS_ITEM(item), NULL);
	return item->icon;
}

/**************************************************************************
 * Item drop-down
 **************************************************************************/

static void
item_factory_setup_cb(GtkSignalListItemFactory *factory, GtkListItem *li,
                      gpointer data)
{
	GtkWidget *box, *image, *label;

	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	image = gtk_image_new();
	label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_box_append(GTK_BOX(box), image);
	gtk_box_append(GTK_BOX(box), label);
	gtk_list_item_set_child(li, box);
}

static void
item_factory_bind_cb(GtkSignalListItemFactory *factory, GtkListItem *li,
                     gpointer data)
{
	GtkWidget *box = gtk_list_item_get_child(li);
	GtkWidget *image = gtk_widget_get_first_child(box);
	GtkWidget *label = gtk_widget_get_next_sibling(image);
	PidginItem *item = gtk_list_item_get_item(li);

	gtk_label_set_text(GTK_LABEL(label), item->label ? item->label : "");
	if (item->icon != NULL) {
		gtk_image_set_from_gicon(GTK_IMAGE(image), item->icon);
		gtk_widget_set_visible(image, TRUE);
	} else {
		gtk_image_clear(GTK_IMAGE(image));
		gtk_widget_set_visible(image, FALSE);
	}
}

GtkWidget *
pidgin_item_dropdown_new(GListModel *model)
{
	GtkListItemFactory *factory;
	GtkExpression *expr;
	GtkWidget *dropdown;

	factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(item_factory_setup_cb), NULL);
	g_signal_connect(factory, "bind", G_CALLBACK(item_factory_bind_cb), NULL);

	expr = gtk_property_expression_new(PIDGIN_TYPE_ITEM, NULL, "label");
	dropdown = gtk_drop_down_new(model, expr);
	gtk_drop_down_set_factory(GTK_DROP_DOWN(dropdown), factory);
	gtk_drop_down_set_enable_search(GTK_DROP_DOWN(dropdown), TRUE);
	g_object_unref(factory);

	return dropdown;
}

PidginItem *
pidgin_item_dropdown_get_selected_item(GtkWidget *dropdown)
{
	gpointer item;

	g_return_val_if_fail(GTK_IS_DROP_DOWN(dropdown), NULL);

	item = gtk_drop_down_get_selected_item(GTK_DROP_DOWN(dropdown));
	return PIDGIN_IS_ITEM(item) ? item : NULL;
}

gpointer
pidgin_item_dropdown_get_selected_data(GtkWidget *dropdown)
{
	PidginItem *item = pidgin_item_dropdown_get_selected_item(dropdown);

	return item ? item->data : NULL;
}

const char *
pidgin_item_dropdown_get_selected_id(GtkWidget *dropdown)
{
	PidginItem *item = pidgin_item_dropdown_get_selected_item(dropdown);

	return item ? item->id : NULL;
}

static gboolean
item_dropdown_select(GtkWidget *dropdown, gpointer data, const char *id)
{
	GListModel *model;
	guint i, n;

	g_return_val_if_fail(GTK_IS_DROP_DOWN(dropdown), FALSE);

	model = gtk_drop_down_get_model(GTK_DROP_DOWN(dropdown));
	if (model == NULL)
		return FALSE;

	n = g_list_model_get_n_items(model);
	for (i = 0; i < n; i++) {
		PidginItem *item = g_list_model_get_item(model, i);
		gboolean match;

		if (id != NULL)
			match = purple_strequal(item->id, id);
		else
			match = (item->data == data);
		g_object_unref(item);

		if (match) {
			gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), i);
			return TRUE;
		}
	}
	return FALSE;
}

gboolean
pidgin_item_dropdown_select_data(GtkWidget *dropdown, gpointer data)
{
	return item_dropdown_select(dropdown, data, NULL);
}

gboolean
pidgin_item_dropdown_select_id(GtkWidget *dropdown, const char *id)
{
	g_return_val_if_fail(id != NULL, FALSE);
	return item_dropdown_select(dropdown, NULL, id);
}

/**************************************************************************
 * Account and protocol drop-downs
 **************************************************************************/

static GListStore *
account_store_new(gboolean show_all, PurpleFilterAccountFunc filter_func,
                  gpointer user_data)
{
	GListStore *store = g_list_store_new(PIDGIN_TYPE_ITEM);
	GList *l;

	for (l = purple_accounts_get_all(); l != NULL; l = l->next) {
		PurpleAccount *account = l->data;
		PurplePlugin *prpl;
		PidginItem *item;
		GIcon *icon;
		char *label;

		if (!show_all && !purple_account_is_connected(account))
			continue;
		if (filter_func != NULL && !filter_func(account))
			continue;

		prpl = purple_find_prpl(purple_account_get_protocol_id(account));
		if (purple_account_get_alias(account) != NULL &&
		    *purple_account_get_alias(account) != '\0') {
			label = g_strdup_printf("%s (%s) (%s)",
				purple_account_get_username(account),
				purple_account_get_alias(account),
				purple_account_get_protocol_name(account));
		} else {
			label = g_strdup_printf("%s (%s)",
				purple_account_get_username(account),
				purple_account_get_protocol_name(account));
		}
		(void)prpl;

		item = pidgin_item_new(label, NULL, account);
		icon = pidgin_create_prpl_gicon(account, NULL);
		pidgin_item_set_icon(item, icon);
		g_object_unref(icon);
		g_list_store_append(store, item);
		g_object_unref(item);
		g_free(label);
	}

	return store;
}

GtkWidget *
pidgin_account_dropdown_new(PurpleAccount *default_account, gboolean show_all,
                            PurpleFilterAccountFunc filter_func,
                            gpointer user_data)
{
	GListStore *store = account_store_new(show_all, filter_func, user_data);
	GtkWidget *dropdown = pidgin_item_dropdown_new(G_LIST_MODEL(store));

	if (default_account != NULL)
		pidgin_item_dropdown_select_data(dropdown, default_account);

	return dropdown;
}

PurpleAccount *
pidgin_account_dropdown_get_selected(GtkWidget *dropdown)
{
	return pidgin_item_dropdown_get_selected_data(dropdown);
}

void
pidgin_account_dropdown_set_selected(GtkWidget *dropdown, PurpleAccount *account)
{
	pidgin_item_dropdown_select_data(dropdown, account);
}

GtkWidget *
pidgin_protocol_dropdown_new(const char *default_id)
{
	GListStore *store = g_list_store_new(PIDGIN_TYPE_ITEM);
	GtkWidget *dropdown;
	GList *l;

	/* purple_plugins_get_protocols() is sorted by name. */
	for (l = purple_plugins_get_protocols(); l != NULL; l = l->next) {
		PurplePlugin *plugin = l->data;
		PidginItem *item;
		GIcon *icon;

		if (plugin->info == NULL || plugin->info->name == NULL)
			continue;

		item = pidgin_item_new(plugin->info->name, plugin->info->id, plugin);
		icon = pidgin_create_prpl_gicon(NULL, plugin);
		pidgin_item_set_icon(item, icon);
		g_object_unref(icon);
		g_list_store_append(store, item);
		g_object_unref(item);
	}

	dropdown = pidgin_item_dropdown_new(G_LIST_MODEL(store));
	if (default_id != NULL)
		pidgin_item_dropdown_select_id(dropdown, default_id);

	return dropdown;
}

PurplePlugin *
pidgin_protocol_dropdown_get_selected(GtkWidget *dropdown)
{
	return pidgin_item_dropdown_get_selected_data(dropdown);
}

const char *
pidgin_protocol_dropdown_get_selected_id(GtkWidget *dropdown)
{
	return pidgin_item_dropdown_get_selected_id(dropdown);
}

gboolean
pidgin_protocol_dropdown_set_selected_id(GtkWidget *dropdown, const char *id)
{
	return pidgin_item_dropdown_select_id(dropdown, id);
}

/**************************************************************************
 * Icons and images
 **************************************************************************/

int
pidgin_prpl_icon_size_to_pixels(PidginPrplIconSize size)
{
	switch (size) {
	case PIDGIN_PRPL_ICON_LARGE:
		return 48;
	case PIDGIN_PRPL_ICON_MEDIUM:
		return 22;
	case PIDGIN_PRPL_ICON_SMALL:
	default:
		return 16;
	}
}

/* Pidgin 2 installs protocol icons as <pixmaps>/pidgin/protocols/<size>/
 * <name>.png; third-party prpls install theirs there too. */
static GIcon *
find_legacy_prpl_icon(const char *name)
{
	const char *const bases[] = {
		DATADIR,
		PURPLE_DATADIR,
		NULL,  /* g_get_user_data_dir() */
		"/usr/local/share",
		"/usr/share",
	};
	const char *const sizes[] = { "scalable", "48", "22", "16" };
	gsize i, j;

	for (i = 0; i < G_N_ELEMENTS(bases); i++) {
		const char *base = bases[i] ? bases[i] : g_get_user_data_dir();

		for (j = 0; j < G_N_ELEMENTS(sizes); j++) {
			char *file, *path;
			GIcon *icon = NULL;

			file = g_strconcat(name,
				g_str_equal(sizes[j], "scalable") ? ".svg" : ".png", NULL);
			path = g_build_filename(base, "pixmaps", "pidgin", "protocols",
			                        sizes[j], file, NULL);
			if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
				GFile *gfile = g_file_new_for_path(path);
				icon = g_file_icon_new(gfile);
				g_object_unref(gfile);
			}
			g_free(path);
			g_free(file);
			if (icon != NULL)
				return icon;
		}
	}
	return NULL;
}

GIcon *
pidgin_create_prpl_gicon(PurpleAccount *account, PurplePlugin *prpl)
{
	PurplePluginProtocolInfo *prpl_info;
	const char *name = NULL;
	GIcon *icon = NULL;
	char *icon_name;
	GtkIconTheme *theme;
	GdkDisplay *display;

	if (prpl == NULL && account != NULL)
		prpl = purple_find_prpl(purple_account_get_protocol_id(account));

	if (prpl != NULL) {
		prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(prpl);
		if (prpl_info != NULL && prpl_info->list_icon != NULL)
			name = prpl_info->list_icon(account, NULL);
	}

	if (name == NULL)
		return g_themed_icon_new_with_default_fallbacks("network-server-symbolic");

	icon_name = g_strdup_printf("pidgin4-protocol-%s", name);
	display = gdk_display_get_default();
	theme = display ? gtk_icon_theme_get_for_display(display) : NULL;
	if (theme != NULL && gtk_icon_theme_has_icon(theme, icon_name))
		icon = g_themed_icon_new(icon_name);
	g_free(icon_name);

	if (icon == NULL)
		icon = find_legacy_prpl_icon(name);

	if (icon == NULL)
		icon = g_themed_icon_new_with_default_fallbacks("network-server-symbolic");

	return icon;
}

GtkWidget *
pidgin_create_prpl_image(PurpleAccount *account, PurplePlugin *prpl,
                         PidginPrplIconSize size)
{
	GIcon *icon = pidgin_create_prpl_gicon(account, prpl);
	GtkWidget *image = gtk_image_new_from_gicon(icon);

	gtk_image_set_pixel_size(GTK_IMAGE(image), pidgin_prpl_icon_size_to_pixels(size));
	g_object_unref(icon);
	return image;
}

GdkTexture *
pidgin_texture_new_from_data(gconstpointer data, gsize len)
{
	GBytes *bytes;
	GdkTexture *texture;
	GError *error = NULL;

	if (data == NULL || len == 0)
		return NULL;

	bytes = g_bytes_new(data, len);
	texture = gdk_texture_new_from_bytes(bytes, &error);
	g_bytes_unref(bytes);

	if (texture == NULL) {
		purple_debug_warning("gtkutils", "Could not decode image data: %s\n",
		                     error ? error->message : "unknown error");
		g_clear_error(&error);
	}
	return texture;
}

GdkTexture *
pidgin_texture_new_from_imgstore(PurpleStoredImage *image)
{
	g_return_val_if_fail(image != NULL, NULL);

	return pidgin_texture_new_from_data(purple_imgstore_get_data(image),
	                                    purple_imgstore_get_size(image));
}

/**************************************************************************
 * Windows and dialogs
 **************************************************************************/

GtkWindow *
pidgin_get_active_window(void)
{
	GtkApplication *app = pidgin_application_get();

	return app ? gtk_application_get_active_window(app) : NULL;
}

GtkWidget *
pidgin_dialog_new(const char *title, GtkWindow *parent, const char *role,
                  gboolean resizable)
{
	GtkWidget *window, *vbox, *content, *actions;
	GtkEventController *controller;
	GtkShortcut *shortcut;
	GtkApplication *app = pidgin_application_get();

	window = gtk_window_new();
	if (app != NULL)
		gtk_window_set_application(GTK_WINDOW(window), app);
	if (title != NULL)
		gtk_window_set_title(GTK_WINDOW(window), title);
	if (parent != NULL)
		gtk_window_set_transient_for(GTK_WINDOW(window), parent);
	if (role != NULL)
		gtk_widget_set_name(window, role);
	gtk_window_set_resizable(GTK_WINDOW(window), resizable);
	gtk_window_set_destroy_with_parent(GTK_WINDOW(window), FALSE);

	/* Escape closes, like GtkDialog did. */
	controller = gtk_shortcut_controller_new();
	shortcut = gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Escape, 0),
	                            gtk_named_action_new("window.close"));
	gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(controller), shortcut);
	gtk_widget_add_controller(window, controller);

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_top(vbox, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_bottom(vbox, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_start(vbox, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_end(vbox, PIDGIN_HIG_BORDER);
	gtk_window_set_child(GTK_WINDOW(window), vbox);

	content = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BOX_SPACE);
	gtk_widget_set_vexpand(content, TRUE);
	gtk_box_append(GTK_BOX(vbox), content);

	actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	gtk_widget_set_halign(actions, GTK_ALIGN_END);
	gtk_box_append(GTK_BOX(vbox), actions);

	g_object_set_data(G_OBJECT(window), "pidgin-dialog-content", content);
	g_object_set_data(G_OBJECT(window), "pidgin-dialog-actions", actions);

	return window;
}

GtkWidget *
pidgin_dialog_get_content_area(GtkWidget *dialog)
{
	return g_object_get_data(G_OBJECT(dialog), "pidgin-dialog-content");
}

GtkWidget *
pidgin_dialog_get_action_area(GtkWidget *dialog)
{
	return g_object_get_data(G_OBJECT(dialog), "pidgin-dialog-actions");
}

GtkWidget *
pidgin_dialog_add_button(GtkWidget *dialog, const char *label,
                         GCallback callback, gpointer data)
{
	GtkWidget *actions = pidgin_dialog_get_action_area(dialog);
	GtkWidget *button;

	g_return_val_if_fail(actions != NULL, NULL);

	button = gtk_button_new_with_mnemonic(label);
	if (callback != NULL)
		g_signal_connect(button, "clicked", callback, data);
	gtk_box_append(GTK_BOX(actions), button);

	return button;
}

GtkWidget *
pidgin_dialog_add_message(GtkWidget *dialog, const char *icon_name,
                          const char *primary, const char *secondary,
                          gboolean secondary_markup)
{
	GtkWidget *content = pidgin_dialog_get_content_area(dialog);
	GtkWidget *hbox, *vbox, *label;

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BORDER);

	if (icon_name != NULL) {
		GtkWidget *image = gtk_image_new_from_icon_name(icon_name);

		gtk_image_set_icon_size(GTK_IMAGE(image), GTK_ICON_SIZE_LARGE);
		gtk_widget_set_valign(image, GTK_ALIGN_START);
		gtk_box_append(GTK_BOX(hbox), image);
	}

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BOX_SPACE);
	gtk_widget_set_hexpand(vbox, TRUE);
	gtk_box_append(GTK_BOX(hbox), vbox);

	if (primary != NULL) {
		label = gtk_label_new(primary);
		gtk_widget_add_css_class(label, "pidgin-dialog-primary");
		gtk_label_set_wrap(GTK_LABEL(label), TRUE);
		gtk_label_set_max_width_chars(GTK_LABEL(label), 60);
		gtk_label_set_xalign(GTK_LABEL(label), 0.0);
		gtk_label_set_selectable(GTK_LABEL(label), FALSE);
		gtk_box_append(GTK_BOX(vbox), label);
	}

	if (secondary != NULL) {
		label = gtk_label_new(NULL);
		if (secondary_markup)
			gtk_label_set_markup(GTK_LABEL(label), secondary);
		else
			gtk_label_set_text(GTK_LABEL(label), secondary);
		gtk_label_set_wrap(GTK_LABEL(label), TRUE);
		gtk_label_set_max_width_chars(GTK_LABEL(label), 60);
		gtk_label_set_xalign(GTK_LABEL(label), 0.0);
		gtk_label_set_selectable(GTK_LABEL(label), TRUE);
		gtk_box_append(GTK_BOX(vbox), label);
	}

	gtk_box_prepend(GTK_BOX(content), hbox);
	return hbox;
}

GtkWidget *
pidgin_make_scrollable(GtkWidget *child, GtkPolicyType hscroll,
                       GtkPolicyType vscroll, int width, int height)
{
	GtkWidget *sw = gtk_scrolled_window_new();

	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), hscroll, vscroll);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), child);
	gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(sw), TRUE);
	if (width != -1)
		gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(sw), width);
	if (height != -1)
		gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw), height);

	return sw;
}

GtkWidget *
pidgin_make_frame(GtkWidget *parent, const char *title)
{
	GtkWidget *outer, *label, *inner;
	char *markup;

	outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BOX_SPACE);
	gtk_box_append(GTK_BOX(parent), outer);

	markup = g_markup_printf_escaped("<b>%s</b>", title);
	label = gtk_label_new(NULL);
	gtk_label_set_markup_with_mnemonic(GTK_LABEL(label), markup);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	g_free(markup);
	gtk_box_append(GTK_BOX(outer), label);

	inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BOX_SPACE);
	gtk_widget_set_margin_start(inner, PIDGIN_HIG_BORDER);
	gtk_box_append(GTK_BOX(outer), inner);

	return inner;
}

GtkWidget *
pidgin_add_widget_to_vbox(GtkBox *vbox, const char *widget_label,
                          GtkSizeGroup *sg, GtkWidget *widget,
                          gboolean expand, GtkWidget **p_label)
{
	GtkWidget *hbox, *label = NULL;

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	gtk_box_append(vbox, hbox);

	if (widget_label != NULL) {
		label = gtk_label_new_with_mnemonic(widget_label);
		gtk_label_set_xalign(GTK_LABEL(label), 0.0);
		if (sg != NULL)
			gtk_size_group_add_widget(sg, label);
		gtk_box_append(GTK_BOX(hbox), label);
		pidgin_set_accessible_label(widget, label);
	}

	gtk_widget_set_hexpand(widget, expand);
	gtk_box_append(GTK_BOX(hbox), widget);

	if (p_label != NULL)
		*p_label = label;

	return hbox;
}

void
pidgin_set_accessible_label(GtkWidget *w, GtkWidget *label)
{
	g_return_if_fail(GTK_IS_WIDGET(w));
	g_return_if_fail(GTK_IS_LABEL(label));

	gtk_label_set_mnemonic_widget(GTK_LABEL(label), w);
	gtk_accessible_update_relation(GTK_ACCESSIBLE(w),
		GTK_ACCESSIBLE_RELATION_LABELLED_BY, label, NULL, -1);
}

/* Tags that have a Pango markup equivalent. */
static const char *
pango_tag_for(const char *tag)
{
	static const struct { const char *html, *pango; } map[] = {
		{ "b", "b" }, { "strong", "b" },
		{ "i", "i" }, { "em", "i" }, { "cite", "i" },
		{ "u", "u" }, { "s", "s" }, { "strike", "s" }, { "del", "s" },
		{ "sub", "sub" }, { "sup", "sup" }, { "tt", "tt" }, { "code", "tt" },
		{ "pre", "tt" },
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(map); i++)
		if (g_ascii_strcasecmp(tag, map[i].html) == 0)
			return map[i].pango;
	return NULL;
}

char *
pidgin_html_to_pango_markup(const char *html)
{
	GString *out;
	const char *p;
	GSList *stack = NULL;   /* open Pango tags (const char *) */

	if (html == NULL)
		return g_strdup("");

	out = g_string_new(NULL);
	p = html;
	while (*p) {
		if (*p == '<') {
			const char *end = strchr(p, '>');
			gboolean closing;
			char *name, *attrs;
			const char *q;

			if (end == NULL) {
				g_string_append(out, "&lt;");
				p++;
				continue;
			}

			q = p + 1;
			closing = (*q == '/');
			if (closing)
				q++;
			attrs = g_strndup(q, end - q);
			g_strstrip(attrs);
			{
				char *sp = attrs;
				while (*sp && !g_ascii_isspace(*sp) && *sp != '/')
					sp++;
				name = g_strndup(attrs, sp - attrs);
			}

			if (g_ascii_strcasecmp(name, "br") == 0) {
				g_string_append_c(out, '\n');
			} else if (g_ascii_strcasecmp(name, "p") == 0 ||
			           g_ascii_strcasecmp(name, "div") == 0) {
				if (closing && out->len > 0)
					g_string_append_c(out, '\n');
			} else if (g_ascii_strcasecmp(name, "a") == 0 && !closing) {
				const char *href = strstr(attrs, "href=");
				if (href != NULL) {
					char quote = href[5];
					const char *start = href + 6, *stop;
					if (quote == '"' || quote == '\'') {
						stop = strchr(start, quote);
						if (stop != NULL) {
							char *url = g_strndup(start, stop - start);
							char *esc = g_markup_escape_text(url, -1);
							g_string_append_printf(out, "<a href=\"%s\">", esc);
							stack = g_slist_prepend(stack, (gpointer)"a");
							g_free(esc);
							g_free(url);
						}
					}
				}
			} else if (g_ascii_strcasecmp(name, "a") == 0 && closing) {
				if (stack != NULL && g_str_equal(stack->data, "a")) {
					g_string_append(out, "</a>");
					stack = g_slist_delete_link(stack, stack);
				}
			} else {
				const char *ptag = pango_tag_for(name);
				if (ptag != NULL) {
					if (!closing) {
						g_string_append_printf(out, "<%s>", ptag);
						stack = g_slist_prepend(stack, (gpointer)ptag);
					} else if (stack != NULL && g_str_equal(stack->data, ptag)) {
						g_string_append_printf(out, "</%s>", ptag);
						stack = g_slist_delete_link(stack, stack);
					}
				}
			}
			g_free(name);
			g_free(attrs);
			p = end + 1;
		} else if (*p == '&') {
			/* Keep valid entities; escape a bare '&'. */
			const char *semi = strchr(p, ';');
			if (semi != NULL && semi - p <= 10) {
				char *ent = g_strndup(p, semi - p + 1);
				const char *text = purple_markup_unescape_entity(ent, NULL);
				if (text != NULL) {
					char *esc = g_markup_escape_text(text, -1);
					g_string_append(out, esc);
					g_free(esc);
					g_free(ent);
					p = semi + 1;
					continue;
				}
				g_free(ent);
			}
			g_string_append(out, "&amp;");
			p++;
		} else if (*p == '>') {
			g_string_append(out, "&gt;");
			p++;
		} else {
			const char *next = g_utf8_next_char(p);
			g_string_append_len(out, p, next - p);
			p = next;
		}
	}

	/* Close whatever is still open. */
	while (stack != NULL) {
		g_string_append_printf(out, "</%s>", (const char *)stack->data);
		stack = g_slist_delete_link(stack, stack);
	}

	return g_string_free(out, FALSE);
}

/**************************************************************************
 * Misc
 **************************************************************************/

static void
uri_launch_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	GError *error = NULL;

	if (!gtk_uri_launcher_launch_finish(GTK_URI_LAUNCHER(source), result, &error)) {
		if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
			purple_debug_error("gtkutils", "Could not open URI %s: %s\n",
			                   gtk_uri_launcher_get_uri(GTK_URI_LAUNCHER(source)),
			                   error->message);
		g_error_free(error);
	}
}

/*
 * M5: the Browser preference (/pidgin4/browser/method "custom" and
 * /pidgin4/browser/command, "%s" for the URL or appended). Only web links
 * go to a custom browser; returns FALSE to fall back to the launcher.
 */
static gboolean
open_uri_custom_browser(const char *uri)
{
	const char *command;
	char *quoted, *cmdline, **argv = NULL;
	GError *error = NULL;
	gboolean ok;

	if (!purple_prefs_exists("/pidgin4/browser/method") ||
	    !purple_strequal(purple_prefs_get_string("/pidgin4/browser/method"), "custom"))
		return FALSE;
	command = purple_prefs_get_string("/pidgin4/browser/command");
	if (command == NULL || *command == '\0' ||
	    !(g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://") ||
	      g_str_has_prefix(uri, "ftp://")))
		return FALSE;

	quoted = g_shell_quote(uri);
	if (strstr(command, "%s") != NULL) {
		char **parts = g_strsplit(command, "%s", -1);
		cmdline = g_strjoinv(quoted, parts);
		g_strfreev(parts);
	} else {
		cmdline = g_strdup_printf("%s %s", command, quoted);
	}
	g_free(quoted);

	ok = g_shell_parse_argv(cmdline, NULL, &argv, &error) &&
	     g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error);
	if (!ok) {
		purple_debug_error("gtkutils", "Could not run the browser command "
		                   "'%s': %s\n", cmdline, error ? error->message : "");
		purple_notify_error(NULL, NULL, _("Unable to open URL"),
		                    error ? error->message : cmdline);
		g_clear_error(&error);
	}
	g_strfreev(argv);
	g_free(cmdline);
	return TRUE;
}

void
pidgin_open_uri(GtkWindow *parent, const char *uri)
{
	GtkUriLauncher *launcher;

	g_return_if_fail(uri != NULL);

	if (open_uri_custom_browser(uri))
		return;

	if (parent == NULL)
		parent = pidgin_get_active_window();

	launcher = gtk_uri_launcher_new(uri);
	gtk_uri_launcher_launch(launcher, parent, NULL, uri_launch_cb, NULL);
	g_object_unref(launcher);
}

/**************************************************************************
 * Message view and compose entry (M4)
 **************************************************************************/

GtkWidget *
pidgin_create_message_view(void)
{
	pidgin_message_view_signals_init();
	return pidgin_message_view_new();
}

GtkWidget *
pidgin_create_compose_entry(PurpleConnectionFlags features, gboolean with_toolbar,
                            GtkWidget **entry_ret, GtkWidget **toolbar_ret)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *entry = pidgin_compose_entry_new();
	GtkWidget *toolbar = NULL;
	GtkWidget *sw;

	pidgin_compose_entry_setup(PIDGIN_COMPOSE_ENTRY(entry), features);
	if (with_toolbar) {
		toolbar = pidgin_format_toolbar_new(PIDGIN_COMPOSE_ENTRY(entry));
		gtk_box_append(GTK_BOX(box), toolbar);
	}
	sw = pidgin_make_scrollable(entry, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC, -1, -1);
	gtk_widget_set_vexpand(sw, TRUE);
	gtk_box_append(GTK_BOX(box), sw);

	if (entry_ret)
		*entry_ret = entry;
	if (toolbar_ret)
		*toolbar_ret = toolbar;
	return box;
}

void
pidgin_utils_init(void)
{
	/* M4 components: link schemes, smiley theme, the conversation UI
	 * signals, the message index (and its backfill). */
	pidgin_markup_init();
	pidgin_smiley_themes_init();
	pidgin_message_view_signals_init();
	pidgin_message_index_ui_init();
}

void
pidgin_utils_uninit(void)
{
	pidgin_message_index_ui_uninit();
	pidgin_message_view_signals_uninit();
	pidgin_smiley_themes_uninit();
	pidgin_markup_uninit();
}

/**************************************************************************
 * Buddy icons (M3), from pidgin/gtkutils.c
 **************************************************************************/

/* TRUE if any string from array a exists in array b. */
static gboolean
str_array_match(char **a, char **b)
{
	int i, j;

	if (!a || !b)
		return FALSE;
	for (i = 0; a[i] != NULL; i++)
		for (j = 0; b[j] != NULL; j++)
			if (!g_ascii_strcasecmp(a[i], b[j]))
				return TRUE;
	return FALSE;
}

gpointer
pidgin_convert_buddy_icon(PurplePlugin *plugin, const char *path, size_t *len)
{
	PurplePluginProtocolInfo *prpl_info;
	PurpleBuddyIconSpec *spec;
	int orig_width, orig_height, new_width, new_height;
	GdkPixbufFormat *format;
	char **pixbuf_formats;
	char **prpl_formats;
	GError *error = NULL;
	gchar *contents;
	gsize length;
	GdkPixbuf *pixbuf, *original;
	float scale_factor;
	int i;
	char *tmp;

	prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(plugin);
	spec = &prpl_info->icon_spec;
	g_return_val_if_fail(spec->format != NULL, NULL);

	format = gdk_pixbuf_get_file_info(path, &orig_width, &orig_height);
	if (format == NULL) {
		purple_debug_warning("buddyicon", "Could not get file info of %s\n", path);
		return NULL;
	}

	pixbuf_formats = gdk_pixbuf_format_get_extensions(format);
	prpl_formats = g_strsplit(spec->format, ",", 0);

	if (str_array_match(pixbuf_formats, prpl_formats) && /* This is an acceptable format AND */
		 (!(spec->scale_rules & PURPLE_ICON_SCALE_SEND) || /* The prpl doesn't scale before it sends OR */
		  (spec->min_width <= orig_width && spec->max_width >= orig_width &&
		   spec->min_height <= orig_height && spec->max_height >= orig_height))) /* The icon is the correct size */
	{
		g_strfreev(pixbuf_formats);

		if (!g_file_get_contents(path, &contents, &length, &error)) {
			purple_debug_warning("buddyicon", "Could not get file contents "
					"of %s: %s\n", path, error->message);
			g_error_free(error);
			g_strfreev(prpl_formats);
			return NULL;
		}

		if (spec->max_filesize == 0 || length < spec->max_filesize) {
			/* The supplied image fits the file size, dimensions and type
			   constraints.  Great!  Return it without making any changes. */
			if (len)
				*len = length;
			g_strfreev(prpl_formats);
			return contents;
		}

		/* The image was too big.  Fall-through and try scaling it down. */
		g_free(contents);
	} else {
		g_strfreev(pixbuf_formats);
	}

	/* The original image wasn't compatible.  Scale it or convert file type. */
	pixbuf = gdk_pixbuf_new_from_file(path, &error);
	if (error) {
		purple_debug_warning("buddyicon", "Could not open icon '%s' for "
				"conversion: %s\n", path, error->message);
		g_error_free(error);
		g_strfreev(prpl_formats);
		return NULL;
	}
	original = g_object_ref(pixbuf);

	new_width = orig_width;
	new_height = orig_height;

	/* Make sure the image is the correct dimensions */
	if (spec->scale_rules & PURPLE_ICON_SCALE_SEND &&
		(orig_width < spec->min_width || orig_width > spec->max_width ||
		 orig_height < spec->min_height || orig_height > spec->max_height))
	{
		purple_buddy_icon_get_scale_size(spec, &new_width, &new_height);

		g_object_unref(G_OBJECT(pixbuf));
		pixbuf = gdk_pixbuf_scale_simple(original, new_width, new_height, GDK_INTERP_HYPER);
	}

	scale_factor = 1;
	do {
		for (i = 0; prpl_formats[i]; i++) {
			int quality = 100;
			do {
				const char *key = NULL;
				const char *value = NULL;
				gchar tmp_buf[4];

				purple_debug_info("buddyicon", "Converting buddy icon to %s\n", prpl_formats[i]);

				if (purple_strequal(prpl_formats[i], "png")) {
					key = "compression";
					value = "9";
				} else if (purple_strequal(prpl_formats[i], "jpeg")) {
					g_snprintf(tmp_buf, sizeof(tmp_buf), "%u", quality);
					key = "quality";
					value = tmp_buf;
				}

				if (!gdk_pixbuf_save_to_buffer(pixbuf, &contents, &length,
						prpl_formats[i], &error, key, value, NULL))
				{
					purple_debug_warning("buddyicon",
							"Could not convert to %s: %s\n", prpl_formats[i],
							(error && error->message) ? error->message : "Unknown error");
					g_clear_error(&error);

					/* We couldn't convert to this image type.  Try the next
					   image type. */
					break;
				}

				if (spec->max_filesize == 0 || length <= spec->max_filesize) {
					/* We were able to save the image as this image type and
					   have it be within the size constraints.  Great!  Return
					   the image. */
					purple_debug_info("buddyicon", "Converted image from "
							"%dx%d to %dx%d, format=%s, quality=%u, "
							"filesize=%" G_GSIZE_FORMAT "\n", orig_width, orig_height,
							new_width, new_height, prpl_formats[i], quality,
							length);
					if (len)
						*len = length;
					g_strfreev(prpl_formats);
					g_object_unref(G_OBJECT(pixbuf));
					g_object_unref(G_OBJECT(original));
					return contents;
				}

				g_free(contents);

				if (!purple_strequal(prpl_formats[i], "jpeg")) {
					/* File size was too big and we can't lower the quality,
					   so skip to the next image type. */
					break;
				}

				/* File size was too big, but we're dealing with jpeg so try
				   lowering the quality. */
				quality -= 5;
			} while (quality >= 70);
		}

		/* We couldn't save the image in any format that was below the max
		   file size.  Maybe we can reduce the image dimensions? */
		scale_factor *= 0.8;
		new_width = orig_width * scale_factor;
		new_height = orig_height * scale_factor;
		g_object_unref(G_OBJECT(pixbuf));
		pixbuf = gdk_pixbuf_scale_simple(original, new_width, new_height, GDK_INTERP_HYPER);
	} while ((new_width > 10 || new_height > 10) && new_width > spec->min_width && new_height > spec->min_height);
	g_strfreev(prpl_formats);
	g_object_unref(G_OBJECT(pixbuf));
	g_object_unref(G_OBJECT(original));

	tmp = g_strdup_printf(_("The file '%s' is too large for %s.  Please try a smaller image.\n"),
			path, plugin->info->name);
	purple_notify_error(NULL, _("Icon Error"), _("Could not set icon"), tmp);
	g_free(tmp);

	return NULL;
}
