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

/*
 * The global status box. The status logic (which saved status a selection
 * activates) is Pidgin 2's activate_currently_selected_status() for the
 * global box; the widget is a GtkMenuButton with a popover.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "account.h"
#include "buddyicon.h"
#include "connection.h"
#include "debug.h"
#include "prefs.h"
#include "prpl.h"
#include "savedstatuses.h"
#include "status.h"
#include "util.h"

#include "gtksavedstatuses.h"
#include "gtkstatusbox.h"
#include "gtkutils.h"

/* Seconds after the last keystroke before a status message is applied. */
#define TYPING_TIMEOUT 4

#define BUDDYICON_PREF PIDGIN_PREFS_ROOT "/accounts/buddyicon"

typedef struct {
	GtkWidget *box;
	GtkWidget *button;
	GtkWidget *spinner;
	GtkWidget *icon;
	GtkWidget *label;
	GtkWidget *popover;
	GtkWidget *list;
	GtkWidget *message_box;
	GtkWidget *message_view;
	GtkWidget *avatar;

	PurpleStatusPrimitive primitive;
	guint typing_timeout;
	gboolean updating;
	gboolean network_available;
} PidginStatusBox;

static PidginStatusBox *statusbox = NULL;

static int statusbox_handle;

/**************************************************************************
 * Status
 **************************************************************************/

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

/* Whether a message box makes sense for @primitive: some enabled account
 * has a status of that primitive with a "message" attribute (Pidgin 2's
 * rule), or no account is enabled (the saved status still keeps it). */
static gboolean
primitive_has_message(PurpleStatusPrimitive primitive)
{
	GList *accounts, *l;
	gboolean found;

	if (primitive == PURPLE_STATUS_OFFLINE || primitive == PURPLE_STATUS_INVISIBLE)
		return FALSE;

	accounts = purple_accounts_get_all_active();
	found = (accounts == NULL);
	for (l = accounts; l != NULL && !found; l = l->next) {
		PurpleStatusType *type =
			purple_account_get_status_type_with_primitive(l->data, primitive);

		if (type != NULL && purple_status_type_get_attr(type, "message") != NULL)
			found = TRUE;
	}
	g_list_free(accounts);
	return found;
}

/* The message box text as purple HTML, or NULL.
 * TODO(M4): PidginComposeEntry (formatting); this is plain text. */
static char *
get_message(void)
{
	GtkTextBuffer *buffer;
	GtkTextIter start, end;
	char *text, *escaped, *html;

	if (statusbox == NULL || !primitive_has_message(statusbox->primitive))
		return NULL;

	buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(statusbox->message_view));
	gtk_text_buffer_get_bounds(buffer, &start, &end);
	text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
	g_strstrip(text);
	if (*text == '\0') {
		g_free(text);
		return NULL;
	}

	escaped = g_markup_escape_text(text, -1);
	html = purple_strreplace(escaped, "\n", "<br>");
	g_free(escaped);
	g_free(text);
	return html;
}

static void
set_message_text(const char *html)
{
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(statusbox->message_view));
	char *text = html ? purple_markup_strip_html(html) : NULL;

	statusbox->updating = TRUE;
	gtk_text_buffer_set_text(buffer, text ? text : "", -1);
	statusbox->updating = FALSE;
	g_free(text);
}

/* Pidgin 2's activate_currently_selected_status() for the global box. */
static void
activate_status(PurpleStatusPrimitive primitive, const char *message)
{
	PurpleSavedStatus *saved_status = purple_savedstatus_get_current();

	if (purple_savedstatus_get_type(saved_status) == primitive &&
	    !purple_savedstatus_has_substatuses(saved_status) &&
	    purple_strequal(purple_savedstatus_get_message(saved_status), message))
		return;

	/* If we've used this type+message before, lookup the transient status */
	saved_status = purple_savedstatus_find_transient_by_type_and_message(primitive, message);

	/* If this type+message is unique then create a new transient saved status */
	if (saved_status == NULL) {
		saved_status = purple_savedstatus_new(NULL, primitive);
		purple_savedstatus_set_message(saved_status, message);
	}

	/* Set the status for each account */
	purple_savedstatus_activate(saved_status);
}

static void
apply_selected(void)
{
	char *message;

	if (statusbox->typing_timeout != 0) {
		g_source_remove(statusbox->typing_timeout);
		statusbox->typing_timeout = 0;
	}

	message = get_message();
	activate_status(statusbox->primitive, message);
	g_free(message);
}

static gboolean
typing_timeout_cb(gpointer data)
{
	statusbox->typing_timeout = 0;
	apply_selected();
	return G_SOURCE_REMOVE;
}

static void
message_changed_cb(GtkTextBuffer *buffer, gpointer data)
{
	if (statusbox->updating)
		return;
	if (statusbox->typing_timeout != 0)
		g_source_remove(statusbox->typing_timeout);
	statusbox->typing_timeout = g_timeout_add_seconds(TYPING_TIMEOUT, typing_timeout_cb, NULL);
}

/* Enter applies at once; Shift+Enter is a newline. */
static gboolean
message_key_cb(GtkEventControllerKey *controller, guint keyval, guint keycode,
               GdkModifierType state, gpointer data)
{
	if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) &&
	    !(state & GDK_SHIFT_MASK)) {
		apply_selected();
		gtk_popover_popdown(GTK_POPOVER(statusbox->popover));
		return TRUE;
	}
	return FALSE;
}

/**************************************************************************
 * Display
 **************************************************************************/

static void
refresh_display(void)
{
	PurpleSavedStatus *saved;
	PurpleStatusPrimitive primitive;
	const char *message, *name;
	char *text, *plain = NULL;

	if (statusbox == NULL)
		return;

	saved = purple_savedstatus_get_current();
	primitive = purple_savedstatus_get_type(saved);
	message = purple_savedstatus_get_message(saved);

	if (!statusbox->network_available) {
		gtk_image_set_from_icon_name(GTK_IMAGE(statusbox->icon), "pidgin-status-offline");
		gtk_label_set_text(GTK_LABEL(statusbox->label), _("Waiting for network connection"));
		return;
	}

	gtk_image_set_from_icon_name(GTK_IMAGE(statusbox->icon), primitive_icon_name(primitive));

	name = purple_savedstatus_is_transient(saved)
		? purple_primitive_get_name_from_type(primitive)
		: purple_savedstatus_get_title(saved);

	if (message != NULL && *message != '\0') {
		plain = purple_markup_strip_html(message);
		g_strdelimit(plain, "\n\r", ' ');
	}
	text = plain ? g_strdup_printf("%s — %s", name, plain) : g_strdup(name);
	gtk_label_set_text(GTK_LABEL(statusbox->label), text);
	gtk_widget_set_tooltip_text(statusbox->button, text);
	g_free(text);
	g_free(plain);
}

static void
update_connecting(void)
{
	gboolean connecting;

	if (statusbox == NULL)
		return;
	connecting = purple_connections_get_connecting() != NULL;
	gtk_widget_set_visible(statusbox->spinner, connecting);
	gtk_spinner_set_spinning(GTK_SPINNER(statusbox->spinner), connecting);
	gtk_widget_set_visible(statusbox->icon, !connecting);
}

void
pidgin_status_box_set_network_available(gboolean available)
{
	if (statusbox == NULL)
		return;
	statusbox->network_available = available;
	refresh_display();
}

/**************************************************************************
 * The popover
 **************************************************************************/

typedef enum {
	ROW_PRIMITIVE,
	ROW_SAVED,
	ROW_NEW,
	ROW_MANAGE
} RowType;

static GtkWidget *
status_row(RowType type, const char *icon_name, const char *label, int data)
{
	GtkWidget *row = gtk_list_box_row_new();
	GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	GtkWidget *text = gtk_label_new(label);

	if (icon_name != NULL)
		gtk_box_append(GTK_BOX(hbox), gtk_image_new_from_icon_name(icon_name));
	gtk_label_set_xalign(GTK_LABEL(text), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(text), 40);
	gtk_box_append(GTK_BOX(hbox), text);
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);

	g_object_set_data(G_OBJECT(row), "row-type", GINT_TO_POINTER(type));
	g_object_set_data(G_OBJECT(row), "row-data", GINT_TO_POINTER(data));
	return row;
}

static void
separator_header(GtkListBoxRow *row, GtkListBoxRow *before, gpointer data)
{
	if (before != NULL &&
	    g_object_get_data(G_OBJECT(row), "row-type") !=
	    g_object_get_data(G_OBJECT(before), "row-type") &&
	    gtk_list_box_row_get_header(row) == NULL)
		gtk_list_box_row_set_header(row, gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
	else if (before == NULL || g_object_get_data(G_OBJECT(row), "row-type") ==
	         g_object_get_data(G_OBJECT(before), "row-type"))
		gtk_list_box_row_set_header(row, NULL);
}

static void
rebuild_list(void)
{
	static const PurpleStatusPrimitive primitives[] = {
		PURPLE_STATUS_AVAILABLE, PURPLE_STATUS_AWAY, PURPLE_STATUS_UNAVAILABLE,
		PURPLE_STATUS_INVISIBLE, PURPLE_STATUS_OFFLINE
	};
	GList *popular, *l;
	gsize i;

	gtk_list_box_remove_all(GTK_LIST_BOX(statusbox->list));

	for (i = 0; i < G_N_ELEMENTS(primitives); i++)
		gtk_list_box_append(GTK_LIST_BOX(statusbox->list),
			status_row(ROW_PRIMITIVE, primitive_icon_name(primitives[i]),
			           purple_primitive_get_name_from_type(primitives[i]),
			           primitives[i]));

	/* Popular statuses, as Pidgin 2 lists them. */
	popular = purple_savedstatuses_get_popular(6);
	for (l = popular; l != NULL; l = l->next) {
		PurpleSavedStatus *saved = l->data;
		const char *message = purple_savedstatus_get_message(saved);
		char *label, *plain = message ? purple_markup_strip_html(message) : NULL;

		if (plain != NULL)
			g_strdelimit(plain, "\n\r", ' ');
		if (purple_savedstatus_is_transient(saved))
			label = g_strdup_printf("%s%s%s",
				purple_primitive_get_name_from_type(purple_savedstatus_get_type(saved)),
				plain ? " — " : "", plain ? plain : "");
		else
			label = g_strdup(purple_savedstatus_get_title(saved));

		gtk_list_box_append(GTK_LIST_BOX(statusbox->list),
			status_row(ROW_SAVED, primitive_icon_name(purple_savedstatus_get_type(saved)),
			           label, (int)purple_savedstatus_get_creation_time(saved)));
		g_free(label);
		g_free(plain);
	}
	g_list_free(popular);

	gtk_list_box_append(GTK_LIST_BOX(statusbox->list),
		status_row(ROW_NEW, NULL, _("New status..."), 0));
	gtk_list_box_append(GTK_LIST_BOX(statusbox->list),
		status_row(ROW_MANAGE, NULL, _("Saved statuses..."), 0));
}

static void
select_primitive(PurpleStatusPrimitive primitive)
{
	statusbox->primitive = primitive;
	gtk_widget_set_visible(statusbox->message_box, primitive_has_message(primitive));
	apply_selected();
	if (primitive_has_message(primitive))
		gtk_widget_grab_focus(statusbox->message_view);
	else
		gtk_popover_popdown(GTK_POPOVER(statusbox->popover));
}

static void
row_activated_cb(GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
	RowType type = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "row-type"));
	int value = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "row-data"));
	PurpleSavedStatus *saved;

	switch (type) {
	case ROW_PRIMITIVE:
		select_primitive(value);
		break;
	case ROW_SAVED:
		saved = purple_savedstatus_find_by_creation_time(value);
		if (saved != NULL)
			purple_savedstatus_activate(saved);
		gtk_popover_popdown(GTK_POPOVER(statusbox->popover));
		break;
	case ROW_NEW:
		gtk_popover_popdown(GTK_POPOVER(statusbox->popover));
		pidgin_status_editor_show(FALSE, NULL);
		break;
	case ROW_MANAGE:
		gtk_popover_popdown(GTK_POPOVER(statusbox->popover));
		pidgin_status_window_show();
		break;
	}
}

static void
popover_show_cb(GtkWidget *popover, gpointer data)
{
	PurpleSavedStatus *saved = purple_savedstatus_get_current();

	rebuild_list();
	statusbox->primitive = purple_savedstatus_get_type(saved);
	if (statusbox->typing_timeout == 0)
		set_message_text(purple_savedstatus_get_message(saved));
	gtk_widget_set_visible(statusbox->message_box, primitive_has_message(statusbox->primitive));
}

static void
popover_closed_cb(GtkPopover *popover, gpointer data)
{
	/* Apply a message that is still being typed. */
	if (statusbox->typing_timeout != 0)
		apply_selected();
}

/**************************************************************************
 * Buddy icon (Pidgin 2's global buddy_icon_set_cb)
 **************************************************************************/

static void
update_avatar(const char *filename)
{
	GdkTexture *texture = NULL;

	if (filename != NULL && *filename != '\0') {
		GFile *file = g_file_new_for_path(filename);
		texture = gdk_texture_new_from_file(file, NULL);
		g_object_unref(file);
	}

	if (texture != NULL) {
		gtk_picture_set_paintable(GTK_PICTURE(statusbox->avatar), GDK_PAINTABLE(texture));
		g_object_unref(texture);
	} else {
		GtkIconTheme *theme = gtk_icon_theme_get_for_display(
			gtk_widget_get_display(statusbox->avatar));
		GtkIconPaintable *icon = gtk_icon_theme_lookup_icon(theme,
			"avatar-default-symbolic", NULL, 32,
			gtk_widget_get_scale_factor(statusbox->avatar),
			gtk_widget_get_direction(statusbox->avatar), 0);

		gtk_picture_set_paintable(GTK_PICTURE(statusbox->avatar), GDK_PAINTABLE(icon));
		g_object_unref(icon);
	}
}

/* Every account that uses the global icon gets it, converted for its
 * prpl. */
static void
buddy_icon_set(const char *filename)
{
	GList *accounts;

	if (filename != NULL && *filename == '\0')
		filename = NULL;

	for (accounts = purple_accounts_get_all(); accounts != NULL; accounts = accounts->next) {
		PurpleAccount *account = accounts->data;
		PurplePlugin *plug = purple_find_prpl(purple_account_get_protocol_id(account));
		PurplePluginProtocolInfo *prplinfo = plug ? PURPLE_PLUGIN_PROTOCOL_INFO(plug) : NULL;

		if (prplinfo != NULL &&
		    purple_account_get_bool(account, "use-global-buddyicon", TRUE) &&
		    prplinfo->icon_spec.format) {
			gpointer data = NULL;
			size_t len = 0;

			if (filename)
				data = pidgin_convert_buddy_icon(plug, filename, &len);
			purple_buddy_icons_set_account_icon(account, data, len);
			purple_account_set_buddy_icon_path(account, filename);
		}
	}

	if (statusbox != NULL)
		update_avatar(filename);
}

static void
buddyicon_pref_cb(const char *name, PurplePrefType type, gconstpointer value, gpointer data)
{
	buddy_icon_set(value);
}

static void
icon_chosen_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
	char *path;

	if (file == NULL)
		return;
	path = g_file_get_path(file);
	if (path != NULL) {
		GFile *parent = g_file_get_parent(file);

		if (parent != NULL) {
			char *folder = g_file_get_path(parent);
			purple_prefs_set_path(PIDGIN4_PREFS_ROOT "/filelocations/last_icon_folder", folder);
			g_free(folder);
			g_object_unref(parent);
		}
		/* The pref callback does the work. */
		purple_prefs_set_path(BUDDYICON_PREF, path);
	}
	g_free(path);
	g_object_unref(file);
}

static void
choose_icon_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	GtkFileDialog *dialog = gtk_file_dialog_new();
	GtkFileFilter *filter = gtk_file_filter_new();
	const char *folder = purple_prefs_get_path(PIDGIN4_PREFS_ROOT "/filelocations/last_icon_folder");
	GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);

	gtk_file_filter_set_name(filter, _("Images"));
	gtk_file_filter_add_mime_type(filter, "image/*");
	g_list_store_append(filters, filter);
	gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
	gtk_file_dialog_set_default_filter(dialog, filter);
	gtk_file_dialog_set_title(dialog, _("Buddy Icon"));
	if (folder != NULL && *folder != '\0') {
		GFile *f = g_file_new_for_path(folder);
		gtk_file_dialog_set_initial_folder(dialog, f);
		g_object_unref(f);
	}

	gtk_file_dialog_open(dialog, GTK_WINDOW(gtk_widget_get_root(statusbox->box)), NULL,
	                     icon_chosen_cb, NULL);
	g_object_unref(filters);
	g_object_unref(filter);
	g_object_unref(dialog);
}

static void
remove_icon_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	purple_prefs_set_path(BUDDYICON_PREF, NULL);
}

static const GActionEntry statusbox_actions[] = {
	{ .name = "choose-icon", .activate = choose_icon_cb },
	{ .name = "remove-icon", .activate = remove_icon_cb },
};

/**************************************************************************
 * libpurple signals
 **************************************************************************/

static void
savedstatus_changed_cb(PurpleSavedStatus *now, PurpleSavedStatus *old, gpointer data)
{
	refresh_display();
}

static void
connecting_changed_cb(gpointer instance, gpointer data)
{
	update_connecting();
}

static void
account_status_changed_cb(PurpleAccount *account, PurpleStatus *old,
                          PurpleStatus *new, gpointer data)
{
	refresh_display();
}

/**************************************************************************
 * The widget
 **************************************************************************/

static void
statusbox_destroy_cb(GtkWidget *widget, gpointer data)
{
	if (statusbox == NULL || statusbox->box != widget)
		return;
	if (statusbox->typing_timeout != 0)
		g_source_remove(statusbox->typing_timeout);
	purple_signals_disconnect_by_handle(&statusbox_handle);
	purple_prefs_disconnect_by_handle(&statusbox_handle);
	g_free(statusbox);
	statusbox = NULL;
}

GtkWidget *
pidgin_status_box_new(void)
{
	GtkWidget *box, *child, *vbox, *scroll, *label, *avatar_button;
	GtkEventController *key;
	GSimpleActionGroup *group;
	GMenu *menu;

	g_return_val_if_fail(statusbox == NULL, NULL);

	statusbox = g_new0(PidginStatusBox, 1);
	statusbox->network_available = TRUE;

	purple_prefs_add_none(PIDGIN4_PREFS_ROOT "/filelocations");
	purple_prefs_add_path(PIDGIN4_PREFS_ROOT "/filelocations/last_icon_folder", "");

	statusbox->box = box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	gtk_widget_add_css_class(box, "pidgin-status-box");
	g_signal_connect(box, "destroy", G_CALLBACK(statusbox_destroy_cb), NULL);

	/* The status button */
	statusbox->button = gtk_menu_button_new();
	gtk_widget_set_hexpand(statusbox->button, TRUE);
	gtk_menu_button_set_direction(GTK_MENU_BUTTON(statusbox->button), GTK_ARROW_UP);
	gtk_box_append(GTK_BOX(box), statusbox->button);

	child = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	statusbox->spinner = gtk_spinner_new();
	gtk_widget_set_visible(statusbox->spinner, FALSE);
	gtk_box_append(GTK_BOX(child), statusbox->spinner);
	statusbox->icon = gtk_image_new();
	gtk_box_append(GTK_BOX(child), statusbox->icon);
	statusbox->label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(statusbox->label), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(statusbox->label), PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand(statusbox->label, TRUE);
	gtk_box_append(GTK_BOX(child), statusbox->label);
	gtk_menu_button_set_child(GTK_MENU_BUTTON(statusbox->button), child);

	/* The popover: statuses and the message */
	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BOX_SPACE);

	statusbox->list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(statusbox->list), GTK_SELECTION_NONE);
	gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(statusbox->list), TRUE);
	gtk_list_box_set_header_func(GTK_LIST_BOX(statusbox->list), separator_header, NULL, NULL);
	gtk_widget_add_css_class(statusbox->list, "pidgin-status-list");
	g_signal_connect(statusbox->list, "row-activated", G_CALLBACK(row_activated_cb), NULL);
	gtk_box_append(GTK_BOX(vbox), statusbox->list);

	statusbox->message_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	label = gtk_label_new_with_mnemonic(_("Status _message:"));
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_box_append(GTK_BOX(statusbox->message_box), label);
	/* TODO(M4): PidginComposeEntry instead of a plain text view. */
	statusbox->message_view = gtk_text_view_new();
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(statusbox->message_view), GTK_WRAP_WORD_CHAR);
	gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(statusbox->message_view), FALSE);
	gtk_widget_add_css_class(statusbox->message_view, "pidgin-status-message");
	gtk_label_set_mnemonic_widget(GTK_LABEL(label), statusbox->message_view);
	g_signal_connect(gtk_text_view_get_buffer(GTK_TEXT_VIEW(statusbox->message_view)),
	                 "changed", G_CALLBACK(message_changed_cb), NULL);
	key = gtk_event_controller_key_new();
	g_signal_connect(key, "key-pressed", G_CALLBACK(message_key_cb), NULL);
	gtk_widget_add_controller(statusbox->message_view, key);
	scroll = pidgin_make_scrollable(statusbox->message_view, GTK_POLICY_NEVER,
	                                GTK_POLICY_AUTOMATIC, 260, 60);
	gtk_box_append(GTK_BOX(statusbox->message_box), scroll);
	gtk_box_append(GTK_BOX(vbox), statusbox->message_box);

	statusbox->popover = gtk_popover_new();
	gtk_popover_set_child(GTK_POPOVER(statusbox->popover), vbox);
	g_signal_connect(statusbox->popover, "show", G_CALLBACK(popover_show_cb), NULL);
	g_signal_connect(statusbox->popover, "closed", G_CALLBACK(popover_closed_cb), NULL);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(statusbox->button), statusbox->popover);

	/* The buddy icon */
	group = g_simple_action_group_new();
	g_action_map_add_action_entries(G_ACTION_MAP(group), statusbox_actions,
	                                G_N_ELEMENTS(statusbox_actions), NULL);
	gtk_widget_insert_action_group(box, "statusbox", G_ACTION_GROUP(group));
	g_object_unref(group);

	menu = g_menu_new();
	g_menu_append(menu, _("_Choose Buddy Icon..."), "statusbox.choose-icon");
	g_menu_append(menu, _("_Remove Buddy Icon"), "statusbox.remove-icon");
	avatar_button = gtk_menu_button_new();
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(avatar_button), G_MENU_MODEL(menu));
	gtk_menu_button_set_direction(GTK_MENU_BUTTON(avatar_button), GTK_ARROW_UP);
	gtk_menu_button_set_always_show_arrow(GTK_MENU_BUTTON(avatar_button), FALSE);
	gtk_widget_set_tooltip_text(avatar_button,
		_("Click to change your buddyicon for all accounts."));
	g_object_unref(menu);
	statusbox->avatar = gtk_picture_new();
	gtk_picture_set_content_fit(GTK_PICTURE(statusbox->avatar), GTK_CONTENT_FIT_CONTAIN);
	gtk_widget_set_size_request(statusbox->avatar, 32, 32);
	gtk_widget_add_css_class(statusbox->avatar, "pidgin-status-box-avatar");
	gtk_menu_button_set_child(GTK_MENU_BUTTON(avatar_button), statusbox->avatar);
	gtk_box_append(GTK_BOX(box), avatar_button);

	update_avatar(purple_prefs_get_path(BUDDYICON_PREF));

	purple_prefs_connect_callback(&statusbox_handle, BUDDYICON_PREF, buddyicon_pref_cb, NULL);
	purple_signal_connect(purple_savedstatuses_get_handle(), "savedstatus-changed",
	                      &statusbox_handle, PURPLE_CALLBACK(savedstatus_changed_cb), NULL);
	purple_signal_connect(purple_accounts_get_handle(), "account-status-changed",
	                      &statusbox_handle, PURPLE_CALLBACK(account_status_changed_cb), NULL);
	purple_signal_connect(purple_accounts_get_handle(), "account-connecting",
	                      &statusbox_handle, PURPLE_CALLBACK(connecting_changed_cb), NULL);
	purple_signal_connect(purple_accounts_get_handle(), "account-disconnected",
	                      &statusbox_handle, PURPLE_CALLBACK(connecting_changed_cb), NULL);
	purple_signal_connect(purple_connections_get_handle(), "signed-on",
	                      &statusbox_handle, PURPLE_CALLBACK(connecting_changed_cb), NULL);
	purple_signal_connect(purple_connections_get_handle(), "signed-off",
	                      &statusbox_handle, PURPLE_CALLBACK(connecting_changed_cb), NULL);
	purple_signal_connect(purple_connections_get_handle(), "connection-error",
	                      &statusbox_handle, PURPLE_CALLBACK(connecting_changed_cb), NULL);

	refresh_display();
	update_connecting();

	/* No-op unless PIDGIN4_STATUS_SELFTEST is set. */
	pidgin_status_box_selftest();

	return box;
}

/**************************************************************************
 * Selftest (PIDGIN4_STATUS_SELFTEST=1)
 **************************************************************************/

#define SELFTEST_MESSAGE "pidgin4 status selftest"

static gboolean
selftest_check_cb(gpointer data)
{
	PurpleSavedStatus *saved = purple_savedstatus_get_current();
	gboolean ok = purple_savedstatus_get_type(saved) == PURPLE_STATUS_AWAY &&
		purple_strequal(purple_savedstatus_get_message(saved), SELFTEST_MESSAGE);

	purple_debug_info("gtkstatusbox", "selftest: current status is %s with message "
		"'%s': %s\n",
		purple_primitive_get_id_from_type(purple_savedstatus_get_type(saved)),
		purple_savedstatus_get_message(saved) ? purple_savedstatus_get_message(saved) : "",
		ok ? "PASS" : "FAIL");
	if (!ok)
		g_printerr("pidgin4: status selftest FAILED\n");
	pidgin_application_quit();
	return G_SOURCE_REMOVE;
}

static gboolean
selftest_run_cb(gpointer data)
{
	GtkListBoxRow *row;
	GtkTextBuffer *buffer;
	GList *active;
	int i;

	if (statusbox == NULL)
		return G_SOURCE_REMOVE;

	active = purple_accounts_get_all_active();
	if (active != NULL) {
		purple_debug_warning("gtkstatusbox", "selftest: skipped: %d accounts are "
			"enabled and would sign in\n", g_list_length(active));
		g_list_free(active);
		return G_SOURCE_REMOVE;
	}

	/* Open the popover, pick "Away", type a message and let the typing
	 * timeout apply it, as a user would. */
	gtk_menu_button_popup(GTK_MENU_BUTTON(statusbox->button));
	for (i = 0; (row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(statusbox->list), i)); i++) {
		if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "row-type")) == ROW_PRIMITIVE &&
		    GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "row-data")) == PURPLE_STATUS_AWAY) {
			g_signal_emit_by_name(statusbox->list, "row-activated", row);
			break;
		}
	}
	buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(statusbox->message_view));
	gtk_text_buffer_set_text(buffer, SELFTEST_MESSAGE, -1);
	purple_debug_info("gtkstatusbox", "selftest: chose Away, typed a message\n");

	g_timeout_add_seconds(TYPING_TIMEOUT + 2, selftest_check_cb, NULL);
	return G_SOURCE_REMOVE;
}

void
pidgin_status_box_selftest(void)
{
	static gboolean done = FALSE;

	if (done || g_getenv("PIDGIN4_STATUS_SELFTEST") == NULL)
		return;
	done = TRUE;
	g_timeout_add_seconds(2, selftest_run_cb, NULL);
}
