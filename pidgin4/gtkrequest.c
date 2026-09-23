/**
 * @file gtkrequest.c GTK+ Request API
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
 * The request UI ops, ported to GTK 4. Every request is a plain toplevel
 * GtkWindow whose buttons call the libpurple callbacks and then
 * purple_request_close(); nothing blocks (no gtk_dialog_run). File and
 * folder requests use GtkFileDialog (portal-aware).
 *
 * Behaviour kept from Pidgin 2:
 *  - A button runs its callback, then purple_request_close().
 *  - Closing an input or fields window runs the cancel callback; closing an
 *    action window runs no callback (Pidgin 2 sent GTK_RESPONSE_DELETE_EVENT,
 *    which matched no action). Closing a choice window runs its cancel
 *    callback with the selected choice.
 *  - Action callbacks receive the index Pidgin 2 passed, which counts the
 *    actions from the last one (Pidgin 2 added the buttons in reverse).
 *  - close_request (purple_request_close() from libpurple or a prpl)
 *    destroys the window without running any callback.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "debug.h"
#include "prefs.h"
#include "util.h"

#include "gtkconv.h"
#include "gtkconvwin.h"
#include "gtkrequest.h"
#include "gtkutils.h"
#include "pidgincompletion.h"
#include "pidgincomposeentry.h"
#include "pidginformattoolbar.h"

#define ICON_QUESTION "dialog-question"

/* Minimum on-screen size of an image field (QR codes must be scannable). */
#define IMAGE_FIELD_MIN_SIZE 256
/* Largest icon of an action-with-icon request. */
#define ACTION_ICON_MAX_SIZE 128

typedef struct
{
	GObject *instance;
	PurpleRequestField *field;
} FieldConnection;

typedef struct
{
	PurpleRequestType type;

	void *user_data;
	GtkWidget *dialog;

	GtkWidget *ok_button;

	size_t cb_count;
	GCallback *cbs;

	union
	{
		struct
		{
			GtkWidget *entry;

			gboolean multiline;
			gchar *hint;

		} input;

		struct
		{
			GPtrArray *radios;

		} choice;

		struct
		{
			PurpleRequestFields *fields;
			GArray *connections;   /* of FieldConnection */

		} multifield;

		struct
		{
			gboolean savedialog;
			GCancellable *cancellable;

		} file;

	} u;

} PidginRequestData;

/**************************************************************************
 * Helpers
 **************************************************************************/

static void
decorate_account(GtkWidget *hbox, PurpleAccount *account)
{
	GtkWidget *image;

	if (account == NULL)
		return;

	image = pidgin_create_prpl_image(account, NULL, PIDGIN_PRPL_ICON_SMALL);
	gtk_widget_set_tooltip_text(image, purple_account_get_username(account));
	gtk_widget_set_valign(image, GTK_ALIGN_START);
	gtk_box_append(GTK_BOX(hbox), image);
}

static gboolean request_window_close_cb(GtkWindow *window, PidginRequestData *data);

/*
 * The transient parent of a request: the window of the conversation it
 * belongs to. NULL leaves it to pidgin_window_set_secondary(), i.e. the
 * buddy list. (Not the active window: requests arrive asynchronously, and
 * chaining them onto whatever dialog has the focus is surprising.)
 */
static GtkWindow *
request_parent(PurpleConversation *conv)
{
	if (conv != NULL && g_list_find(purple_get_conversations(), conv) != NULL &&
	    PIDGIN_IS_PIDGIN_CONVERSATION(conv) && PIDGIN_CONVERSATION(conv) != NULL) {
		PidginWindow *win = pidgin_conv_get_window(PIDGIN_CONVERSATION(conv));
		GtkWidget *w = win ? pidgin_conv_window_get_window(win) : NULL;

		if (w != NULL && !pidgin_conv_window_is_hidden(win) &&
		    gtk_widget_get_visible(w))
			return GTK_WINDOW(w);
	}
	return NULL;
}

/*
 * Creates the request window with its header (icon, primary and secondary
 * text, account icon). Returns the header's horizontal box.
 */
static GtkWidget *
request_window_new(PidginRequestData *data, const char *title,
                   const char *role, const char *icon_name,
                   const char *primary, const char *secondary,
                   PurpleAccount *account, PurpleConversation *conv,
                   gboolean resizable)
{
	GtkWidget *dialog, *hbox;
	GtkWindow *parent = request_parent(conv);

	dialog = pidgin_dialog_new(title ? title : PIDGIN_ALERT_TITLE,
	                           parent, role, resizable);
	/* A conversation's request stays with its window; the others are
	 * dialogs of the buddy list. */
	if (parent == NULL)
		pidgin_window_set_secondary(GTK_WINDOW(dialog));
	data->dialog = dialog;
	g_object_add_weak_pointer(G_OBJECT(dialog), (gpointer *)&data->dialog);

	hbox = pidgin_dialog_add_message(dialog, icon_name, primary, secondary, FALSE);
	decorate_account(hbox, account);

	g_signal_connect(dialog, "close-request",
	                 G_CALLBACK(request_window_close_cb), data);

	return hbox;
}

/* A mnemonic button label for a prpl-supplied text. */
static GtkWidget *
add_button(PidginRequestData *data, const char *text, GCallback cb)
{
	return pidgin_dialog_add_button(data->dialog, text ? text : "", cb, data);
}

/* Multi-line text: a PidginComposeEntry, plain unless it is an "html"
 * input (then formatted, with a toolbar, and read back as HTML). */
static char *
text_view_get_text(GtkWidget *view)
{
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
	GtkTextIter start, end;

	if (pidgin_compose_entry_get_caps(PIDGIN_COMPOSE_ENTRY(view)) & PIDGIN_FORMAT_BOLD)
		return pidgin_compose_entry_get_markup(PIDGIN_COMPOSE_ENTRY(view));
	gtk_text_buffer_get_bounds(buffer, &start, &end);
	return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

static GtkWidget *
text_view_new(const char *text, gboolean editable, gboolean html)
{
	GtkWidget *view = pidgin_compose_entry_new();
	PidginComposeEntry *entry = PIDGIN_COMPOSE_ENTRY(view);

	/* Enter is a newline here; the dialog's buttons do the rest. */
	pidgin_compose_entry_set_return_inserts_newline(entry, TRUE);
	pidgin_compose_entry_set_caps(entry, html ? PIDGIN_FORMAT_HTML_ALL : 0);
	gtk_text_view_set_editable(GTK_TEXT_VIEW(view), editable);
	gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), editable);
	if (!editable)
		pidgin_compose_entry_set_spellcheck(entry, FALSE);
	if (text != NULL) {
		if (html)
			pidgin_compose_entry_set_markup(entry, text);
		else
			gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)),
			                         text, -1);
	}
	return view;
}

/* A single-line entry, masked or not, that activates the default button. */
static GtkWidget *
line_entry_new(const char *text, gboolean masked)
{
	GtkWidget *entry;

	if (masked) {
		entry = gtk_password_entry_new();
		gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(entry), TRUE);
		g_object_set(entry, "activates-default", TRUE, NULL);
	} else {
		entry = gtk_entry_new();
		gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
	}

	if (text != NULL)
		gtk_editable_set_text(GTK_EDITABLE(entry), text);

	return entry;
}

/*
 * Scales @texture up by an integer factor with nearest-neighbour sampling,
 * so small images (QR codes) stay crisp. Returns a new reference.
 */
static GdkTexture *
texture_scale_nearest(GdkTexture *texture, int factor)
{
	GdkTextureDownloader *downloader;
	GBytes *bytes, *scaled;
	const guchar *src;
	guchar *dst;
	gsize stride, dst_stride;
	int width, height, x, y, k;

	if (factor <= 1)
		return g_object_ref(texture);

	width = gdk_texture_get_width(texture);
	height = gdk_texture_get_height(texture);
	if ((gint64)width * factor > 4096 || (gint64)height * factor > 4096)
		return g_object_ref(texture);

	downloader = gdk_texture_downloader_new(texture);
	gdk_texture_downloader_set_format(downloader, GDK_MEMORY_R8G8B8A8);
	bytes = gdk_texture_downloader_download_bytes(downloader, &stride);
	gdk_texture_downloader_free(downloader);

	src = g_bytes_get_data(bytes, NULL);
	dst_stride = (gsize)width * factor * 4;
	dst = g_malloc(dst_stride * height * factor);

	for (y = 0; y < height; y++) {
		guchar *row = dst + (gsize)y * factor * dst_stride;

		for (x = 0; x < width; x++) {
			const guchar *px = src + y * stride + x * 4;

			for (k = 0; k < factor; k++)
				memcpy(row + ((gsize)x * factor + k) * 4, px, 4);
		}
		for (k = 1; k < factor; k++)
			memcpy(row + k * dst_stride, row, dst_stride);
	}
	g_bytes_unref(bytes);

	scaled = g_bytes_new_take(dst, dst_stride * height * factor);
	texture = gdk_memory_texture_new(width * factor, height * factor,
	                                 GDK_MEMORY_R8G8B8A8, scaled, dst_stride);
	g_bytes_unref(scaled);

	return texture;
}

/**************************************************************************
 * Closing
 **************************************************************************/

static void
pidgin_close_request(PurpleRequestType type, void *ui_handle)
{
	PidginRequestData *data = (PidginRequestData *)ui_handle;
	guint i;

	/* libpurple passes the type it recorded; ours is the same. */
	type = data->type;

	if (type == PURPLE_REQUEST_FIELDS && data->u.multifield.connections != NULL) {
		/* No field callback may run once the fields are gone, even if a
		 * widget outlives the window for a moment. */
		for (i = 0; i < data->u.multifield.connections->len; i++) {
			FieldConnection *c = &g_array_index(data->u.multifield.connections,
			                                    FieldConnection, i);

			g_signal_handlers_disconnect_matched(c->instance,
				G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, c->field);
			g_object_unref(c->instance);
		}
		g_array_free(data->u.multifield.connections, TRUE);
		data->u.multifield.connections = NULL;
		data->u.multifield.fields->ui_data = NULL;
	}

	if (data->dialog != NULL) {
		GtkWidget *dialog = data->dialog;

		g_signal_handlers_disconnect_by_data(dialog, data);
		g_object_remove_weak_pointer(G_OBJECT(dialog), (gpointer *)&data->dialog);
		data->dialog = NULL;
		gtk_window_destroy(GTK_WINDOW(dialog));
	}

	switch (type) {
	case PURPLE_REQUEST_INPUT:
		g_free(data->u.input.hint);
		break;
	case PURPLE_REQUEST_CHOICE:
		if (data->u.choice.radios != NULL)
			g_ptr_array_free(data->u.choice.radios, TRUE);
		break;
	case PURPLE_REQUEST_FIELDS:
		purple_request_fields_destroy(data->u.multifield.fields);
		break;
	case PURPLE_REQUEST_FILE:
	case PURPLE_REQUEST_FOLDER:
		if (data->u.file.cancellable != NULL) {
			/* The pending GtkFileDialog finishes with G_IO_ERROR_CANCELLED
			 * and must not touch @data (see file_dialog_done_cb). */
			g_cancellable_cancel(data->u.file.cancellable);
			g_object_unref(data->u.file.cancellable);
		}
		break;
	default:
		break;
	}

	g_free(data->cbs);
	g_free(data);
}

/**************************************************************************
 * Input
 **************************************************************************/

static char *
input_get_value(PidginRequestData *data)
{
	if (data->u.input.multiline) {
		char *text = text_view_get_text(data->u.input.entry);

		/* "html" inputs are read back as HTML by text_view_get_text() */
		return text;
	}

	return g_strdup(gtk_editable_get_text(GTK_EDITABLE(data->u.input.entry)));
}

/* @which: 0 = OK, 1 = Cancel */
static void
input_respond(PidginRequestData *data, int which)
{
	char *value = input_get_value(data);

	if (data->cbs[which] != NULL)
		((PurpleRequestInputCb)data->cbs[which])(data->user_data, value);
	g_free(value);

	purple_request_close(PURPLE_REQUEST_INPUT, data);
}

static void
input_ok_cb(GtkButton *button, PidginRequestData *data)
{
	input_respond(data, 0);
}

static void
input_cancel_cb(GtkButton *button, PidginRequestData *data)
{
	input_respond(data, 1);
}

static void *
pidgin_request_input(const char *title, const char *primary,
					   const char *secondary, const char *default_value,
					   gboolean multiline, gboolean masked, gchar *hint,
					   const char *ok_text, GCallback ok_cb,
					   const char *cancel_text, GCallback cancel_cb,
					   PurpleAccount *account, const char *who, PurpleConversation *conv,
					   void *user_data)
{
	PidginRequestData *data;
	GtkWidget *content, *entry, *button;

	data            = g_new0(PidginRequestData, 1);
	data->type      = PURPLE_REQUEST_INPUT;
	data->user_data = user_data;

	data->cb_count = 2;
	data->cbs = g_new0(GCallback, 2);

	data->cbs[0] = ok_cb;
	data->cbs[1] = cancel_cb;

	/* An "html" input needs room for formatted text. */
	if (purple_strequal(hint, "html"))
		multiline = TRUE;

	data->u.input.multiline = multiline;
	data->u.input.hint = g_strdup(hint);

	request_window_new(data, title, "input", ICON_QUESTION, primary, secondary,
	                   account, conv, multiline);
	content = pidgin_dialog_get_content_area(data->dialog);

	if (multiline) {
		GtkWidget *sw;

		gboolean html = purple_strequal(hint, "html");

		entry = text_view_new(default_value, TRUE, html);
		if (html)
			gtk_box_append(GTK_BOX(content),
			               pidgin_format_toolbar_new(PIDGIN_COMPOSE_ENTRY(entry)));
		sw = pidgin_make_scrollable(entry, GTK_POLICY_NEVER,
		                            GTK_POLICY_AUTOMATIC, -1, -1);
		/* min-content-width has no effect without a horizontal scrollbar */
		gtk_widget_set_size_request(sw, 320, 130);
		gtk_widget_set_vexpand(sw, TRUE);
		gtk_box_append(GTK_BOX(content), sw);
	} else {
		entry = line_entry_new(default_value, masked);
		gtk_editable_set_width_chars(GTK_EDITABLE(entry), 30);
		gtk_box_append(GTK_BOX(content), entry);
	}
	data->u.input.entry = entry;

	add_button(data, cancel_text, G_CALLBACK(input_cancel_cb));
	button = add_button(data, ok_text, G_CALLBACK(input_ok_cb));
	gtk_widget_add_css_class(button, "suggested-action");
	data->ok_button = button;
	gtk_window_set_default_widget(GTK_WINDOW(data->dialog), button);

	gtk_window_set_focus(GTK_WINDOW(data->dialog), entry);
	gtk_window_present(GTK_WINDOW(data->dialog));

	return data;
}

/**************************************************************************
 * Choice
 **************************************************************************/

/* @which: 0 = Cancel, 1 = OK (as the cbs are stored) */
static void
choice_respond(PidginRequestData *data, int which)
{
	int choice = -1;
	guint i;

	for (i = 0; i < data->u.choice.radios->len; i++) {
		GtkWidget *radio = g_ptr_array_index(data->u.choice.radios, i);

		if (gtk_check_button_get_active(GTK_CHECK_BUTTON(radio))) {
			choice = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(radio), "choice_id"));
			break;
		}
	}

	if (choice != -1 && data->cbs[which] != NULL)
		((PurpleRequestChoiceCb)data->cbs[which])(data->user_data, choice);

	purple_request_close(PURPLE_REQUEST_CHOICE, data);
}

static void
choice_ok_cb(GtkButton *button, PidginRequestData *data)
{
	choice_respond(data, 1);
}

static void
choice_cancel_cb(GtkButton *button, PidginRequestData *data)
{
	choice_respond(data, 0);
}

static void *
pidgin_request_choice(const char *title, const char *primary,
			const char *secondary, int default_value,
			const char *ok_text, GCallback ok_cb,
			const char *cancel_text, GCallback cancel_cb,
			PurpleAccount *account, const char *who, PurpleConversation *conv,
			void *user_data, va_list args)
{
	PidginRequestData *data;
	GtkWidget *content, *vbox, *button;
	GtkWidget *first = NULL;
	char *radio_text;

	data            = g_new0(PidginRequestData, 1);
	data->type      = PURPLE_REQUEST_CHOICE;
	data->user_data = user_data;

	data->cb_count = 2;
	data->cbs = g_new0(GCallback, 2);
	data->cbs[0] = cancel_cb;
	data->cbs[1] = ok_cb;
	data->u.choice.radios = g_ptr_array_new();

	request_window_new(data, title, "choice", ICON_QUESTION, primary, secondary,
	                   account, conv, FALSE);
	content = pidgin_dialog_get_content_area(data->dialog);

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BOX_SPACE);
	gtk_widget_set_margin_start(vbox, PIDGIN_HIG_BORDER);
	gtk_box_append(GTK_BOX(content), vbox);

	while ((radio_text = va_arg(args, char *)) != NULL) {
		int resp = va_arg(args, int);
		GtkWidget *radio = gtk_check_button_new_with_label(radio_text);

		if (first == NULL)
			first = radio;
		else
			gtk_check_button_set_group(GTK_CHECK_BUTTON(radio),
			                           GTK_CHECK_BUTTON(first));
		g_object_set_data(G_OBJECT(radio), "choice_id", GINT_TO_POINTER(resp));
		if (resp == default_value)
			gtk_check_button_set_active(GTK_CHECK_BUTTON(radio), TRUE);
		gtk_box_append(GTK_BOX(vbox), radio);
		g_ptr_array_add(data->u.choice.radios, radio);
	}

	add_button(data, cancel_text, G_CALLBACK(choice_cancel_cb));
	button = add_button(data, ok_text, G_CALLBACK(choice_ok_cb));
	gtk_widget_add_css_class(button, "suggested-action");
	data->ok_button = button;
	gtk_window_set_default_widget(GTK_WINDOW(data->dialog), button);

	/* A radio group always has one entry selected (GTK 2's did by
	 * itself); focus it rather than the selectable header text. */
	if (first != NULL) {
		GtkWidget *active = NULL;
		guint i;

		for (i = 0; i < data->u.choice.radios->len && active == NULL; i++) {
			GtkWidget *radio = g_ptr_array_index(data->u.choice.radios, i);

			if (gtk_check_button_get_active(GTK_CHECK_BUTTON(radio)))
				active = radio;
		}
		if (active == NULL) {
			active = first;
			gtk_check_button_set_active(GTK_CHECK_BUTTON(first), TRUE);
		}
		gtk_window_set_focus(GTK_WINDOW(data->dialog), active);
	} else {
		gtk_window_set_focus(GTK_WINDOW(data->dialog), button);
	}

	gtk_window_present(GTK_WINDOW(data->dialog));

	return data;
}

/**************************************************************************
 * Action
 **************************************************************************/

static void
action_clicked_cb(GtkButton *button, PidginRequestData *data)
{
	int id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "action_id"));

	if (id >= 0 && (gsize)id < data->cb_count && data->cbs[id] != NULL)
		((PurpleRequestActionCb)data->cbs[id])(data->user_data, id);

	purple_request_close(PURPLE_REQUEST_ACTION, data);
}

static GtkWidget *
action_icon_new(gconstpointer icon_data, gsize icon_size)
{
	GdkTexture *texture;
	GtkWidget *picture;
	int width, height;

	texture = pidgin_texture_new_from_data(icon_data, icon_size);
	if (texture == NULL) {
		purple_debug_info("gtkrequest", "failed to parse dialog icon\n");
		return NULL;
	}

	width = gdk_texture_get_width(texture);
	height = gdk_texture_get_height(texture);
	if (width > ACTION_ICON_MAX_SIZE || height > ACTION_ICON_MAX_SIZE) {
		if (width > height) {
			height = MAX(1, ACTION_ICON_MAX_SIZE * height / width);
			width = ACTION_ICON_MAX_SIZE;
		} else {
			width = MAX(1, ACTION_ICON_MAX_SIZE * width / height);
			height = ACTION_ICON_MAX_SIZE;
		}
	}

	picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(texture));
	gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
	gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);
	gtk_widget_set_size_request(picture, width, height);
	gtk_widget_set_valign(picture, GTK_ALIGN_START);
	g_object_unref(texture);

	return picture;
}

static void *
pidgin_request_action_with_icon(const char *title, const char *primary,
						const char *secondary, int default_action,
					    PurpleAccount *account, const char *who,
						PurpleConversation *conv, gconstpointer icon_data,
						gsize icon_size,
						void *user_data, size_t action_count, va_list actions)
{
	PidginRequestData *data;
	GtkWidget *hbox, *icon = NULL;
	GtkWidget **buttons;
	gsize i;

	data            = g_new0(PidginRequestData, 1);
	data->type      = PURPLE_REQUEST_ACTION;
	data->user_data = user_data;

	data->cb_count = action_count;
	data->cbs = g_new0(GCallback, action_count);

	if (icon_data != NULL && icon_size > 0)
		icon = action_icon_new(icon_data, icon_size);

	hbox = request_window_new(data, title, "action",
	                          icon ? NULL : ICON_QUESTION,
	                          primary, secondary, account, conv, FALSE);
	if (icon != NULL)
		gtk_box_prepend(GTK_BOX(hbox), icon);

	/*
	 * Pidgin 2 added the buttons in reverse (so the first action is the
	 * rightmost) and passed the callback that reversed position. Both are
	 * kept: plugins were written against that behaviour.
	 */
	buttons = g_new0(GtkWidget *, action_count);
	for (i = 0; i < action_count; i++) {
		const char *text = va_arg(actions, const char *);
		GCallback cb = va_arg(actions, GCallback);
		gsize id = action_count - 1 - i;

		data->cbs[id] = cb;
		buttons[id] = gtk_button_new_with_mnemonic(text ? text : "");
		g_object_set_data(G_OBJECT(buttons[id]), "action_id", GINT_TO_POINTER((int)id));
		g_signal_connect(buttons[id], "clicked", G_CALLBACK(action_clicked_cb), data);
	}
	for (i = 0; i < action_count; i++)
		gtk_box_append(GTK_BOX(pidgin_dialog_get_action_area(data->dialog)), buttons[i]);

	if (default_action != PURPLE_DEFAULT_ACTION_NONE &&
	    default_action >= 0 && (gsize)default_action < action_count) {
		GtkWidget *def = buttons[action_count - 1 - default_action];

		gtk_widget_add_css_class(def, "suggested-action");
		gtk_window_set_default_widget(GTK_WINDOW(data->dialog), def);
		gtk_window_set_focus(GTK_WINDOW(data->dialog), def);
	}
	g_free(buttons);

	gtk_window_present(GTK_WINDOW(data->dialog));

	return data;
}

static void *
pidgin_request_action(const char *title, const char *primary,
						const char *secondary, int default_action,
					    PurpleAccount *account, const char *who, PurpleConversation *conv,
						void *user_data, size_t action_count, va_list actions)
{
	return pidgin_request_action_with_icon(title, primary, secondary,
		default_action, account, who, conv, NULL, 0, user_data, action_count,
		actions);
}

/**************************************************************************
 * Fields
 **************************************************************************/

static PidginRequestData *
field_get_request(PurpleRequestField *field)
{
	PurpleRequestFieldGroup *group = purple_request_field_get_group(field);

	if (group == NULL || group->fields_list == NULL)
		return NULL;
	return group->fields_list->ui_data;
}

static void
update_ok_sensitivity(PidginRequestData *data)
{
	if (data != NULL && data->ok_button != NULL)
		gtk_widget_set_sensitive(data->ok_button,
			purple_request_fields_all_required_filled(data->u.multifield.fields));
}

static void
field_changed(PurpleRequestField *field)
{
	if (purple_request_field_is_required(field))
		update_ok_sensitivity(field_get_request(field));
}

/* Connects @cb to @instance with @field as its data and records it, so
 * close_request can disconnect it. */
static void
connect_field(PidginRequestData *data, gpointer instance, const char *signal,
              GCallback cb, PurpleRequestField *field)
{
	FieldConnection c;

	g_signal_connect(instance, signal, cb, field);
	c.instance = g_object_ref(instance);
	c.field = field;
	g_array_append_val(data->u.multifield.connections, c);
}

static void
field_string_entry_changed_cb(GtkEditable *editable, PurpleRequestField *field)
{
	const char *text = gtk_editable_get_text(editable);

	purple_request_field_string_set_value(field, (*text == '\0') ? NULL : text);
	field_changed(field);
}

static void
field_string_buffer_changed_cb(GtkTextBuffer *buffer, PurpleRequestField *field)
{
	GtkTextIter start, end;
	char *text;

	gtk_text_buffer_get_bounds(buffer, &start, &end);
	text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
	purple_request_field_string_set_value(field, (*text == '\0') ? NULL : text);
	g_free(text);
	field_changed(field);
}

static GtkWidget *
create_string_field(PidginRequestData *data, PurpleRequestField *field)
{
	const char *value = purple_request_field_string_get_default_value(field);
	gboolean editable = purple_request_field_string_is_editable(field);
	const char *hint = purple_request_field_get_type_hint(field);
	GtkWidget *widget;

	if (purple_request_field_string_is_multiline(field)) {
		GtkWidget *view = text_view_new(value, editable, FALSE);

		connect_field(data, gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)),
		              "changed", G_CALLBACK(field_string_buffer_changed_cb), field);
		widget = pidgin_make_scrollable(view, GTK_POLICY_NEVER,
		                                GTK_POLICY_AUTOMATIC, -1, 75);
		gtk_widget_set_vexpand(widget, TRUE);
		g_object_set_data(G_OBJECT(widget), "pidgin-focus-widget", view);
	} else {
		widget = line_entry_new(value,
		                        purple_request_field_string_is_masked(field));
		gtk_editable_set_editable(GTK_EDITABLE(widget), editable);
		gtk_widget_set_hexpand(widget, TRUE);
		connect_field(data, widget, "changed",
		              G_CALLBACK(field_string_entry_changed_cb), field);

		/* Buddy name completion (Pidgin 2's setup_screenname_autocomplete):
		 * picking a buddy also sets the group's "account" field. */
		if (editable && !purple_request_field_string_is_masked(field) &&
		    hint != NULL && purple_str_has_prefix(hint, "screenname")) {
			PurpleRequestField *account_field = NULL;
			GList *l;

			for (l = purple_request_field_group_get_fields(
			         purple_request_field_get_group(field));
			     l != NULL; l = l->next) {
				if (purple_request_field_get_type(l->data) == PURPLE_REQUEST_FIELD_ACCOUNT &&
				    purple_strequal(purple_request_field_get_type_hint(l->data), "account")) {
					account_field = l->data;
					break;
				}
			}
			pidgin_buddy_completion_attach_to_field(widget, account_field,
				purple_strequal(hint, "screenname-all"));
		}
	}

	return widget;
}

static void
field_int_changed_cb(GtkEditable *editable, PurpleRequestField *field)
{
	const char *text = gtk_editable_get_text(editable);

	purple_request_field_int_set_value(field, atoi(text));
	field_changed(field);
}

static GtkWidget *
create_int_field(PidginRequestData *data, PurpleRequestField *field)
{
	GtkWidget *widget;

	widget = gtk_spin_button_new_with_range(G_MININT, G_MAXINT, 1);
	gtk_spin_button_set_digits(GTK_SPIN_BUTTON(widget), 0);
	gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(widget), TRUE);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget),
		purple_request_field_int_get_default_value(field));
	gtk_spin_button_set_activates_default(GTK_SPIN_BUTTON(widget), TRUE);
	gtk_widget_set_hexpand(widget, TRUE);

	/* "changed" follows the typed text, not only committed values. */
	connect_field(data, widget, "changed", G_CALLBACK(field_int_changed_cb), field);

	return widget;
}

static void
field_bool_toggled_cb(GtkCheckButton *button, PurpleRequestField *field)
{
	purple_request_field_bool_set_value(field, gtk_check_button_get_active(button));
	field_changed(field);
}

static GtkWidget *
create_bool_field(PidginRequestData *data, PurpleRequestField *field)
{
	GtkWidget *widget;

	widget = gtk_check_button_new_with_mnemonic(purple_request_field_get_label(field));
	gtk_check_button_set_active(GTK_CHECK_BUTTON(widget),
		purple_request_field_bool_get_default_value(field));
	connect_field(data, widget, "toggled", G_CALLBACK(field_bool_toggled_cb), field);

	return widget;
}

static void
field_choice_dropdown_cb(GObject *dropdown, GParamSpec *pspec, PurpleRequestField *field)
{
	guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));

	if (selected != GTK_INVALID_LIST_POSITION)
		purple_request_field_choice_set_value(field, selected);
	field_changed(field);
}

static void
field_choice_radio_cb(GtkCheckButton *button, PurpleRequestField *field)
{
	if (gtk_check_button_get_active(button)) {
		purple_request_field_choice_set_value(field,
			GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "choice_index")));
		field_changed(field);
	}
}

static GtkWidget *
create_choice_field(PidginRequestData *data, PurpleRequestField *field)
{
	GList *labels = purple_request_field_choice_get_labels(field);
	int num_labels = g_list_length(labels);
	int def = purple_request_field_choice_get_default_value(field);
	GtkWidget *widget;
	GList *l;
	int i;

	if (num_labels > 5) {
		GtkStringList *list = gtk_string_list_new(NULL);

		for (l = labels; l != NULL; l = l->next)
			gtk_string_list_append(list, l->data);

		widget = gtk_drop_down_new(G_LIST_MODEL(list), NULL);
		gtk_drop_down_set_enable_search(GTK_DROP_DOWN(widget), TRUE);
		gtk_drop_down_set_expression(GTK_DROP_DOWN(widget),
			gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, NULL, "string"));
		if (def >= 0 && def < num_labels)
			gtk_drop_down_set_selected(GTK_DROP_DOWN(widget), def);
		gtk_widget_set_hexpand(widget, TRUE);
		connect_field(data, widget, "notify::selected",
		              G_CALLBACK(field_choice_dropdown_cb), field);
	} else {
		GtkWidget *first = NULL;

		widget = gtk_box_new(num_labels == 2 ? GTK_ORIENTATION_HORIZONTAL
		                                     : GTK_ORIENTATION_VERTICAL,
		                     PIDGIN_HIG_BOX_SPACE);

		for (l = labels, i = 0; l != NULL; l = l->next, i++) {
			GtkWidget *radio = gtk_check_button_new_with_label(l->data);

			if (first == NULL)
				first = radio;
			else
				gtk_check_button_set_group(GTK_CHECK_BUTTON(radio),
				                           GTK_CHECK_BUTTON(first));
			g_object_set_data(G_OBJECT(radio), "choice_index", GINT_TO_POINTER(i));
			if (i == def)
				gtk_check_button_set_active(GTK_CHECK_BUTTON(radio), TRUE);
			gtk_box_append(GTK_BOX(widget), radio);

			connect_field(data, radio, "toggled",
			              G_CALLBACK(field_choice_radio_cb), field);
		}
	}

	return widget;
}

typedef struct
{
	GHashTable *icons;   /* item text -> icon file path (not owned) */
} ListFieldInfo;

static void
list_item_setup_cb(GtkSignalListItemFactory *factory, GtkListItem *li,
                   gpointer user_data)
{
	GtkWidget *box, *image, *label;

	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand(label, TRUE);
	image = gtk_image_new();
	gtk_image_set_pixel_size(GTK_IMAGE(image), 32);
	gtk_box_append(GTK_BOX(box), label);
	gtk_box_append(GTK_BOX(box), image);
	gtk_list_item_set_child(li, box);
}

static void
list_item_bind_cb(GtkSignalListItemFactory *factory, GtkListItem *li,
                  gpointer user_data)
{
	GHashTable *icons = user_data;
	GtkWidget *box = gtk_list_item_get_child(li);
	GtkWidget *label = gtk_widget_get_first_child(box);
	GtkWidget *image = gtk_widget_get_next_sibling(label);
	GtkStringObject *obj = gtk_list_item_get_item(li);
	const char *text = gtk_string_object_get_string(obj);
	const char *icon = icons ? g_hash_table_lookup(icons, text) : NULL;

	gtk_label_set_text(GTK_LABEL(label), text);
	if (icon != NULL) {
		gtk_image_set_from_file(GTK_IMAGE(image), icon);
		gtk_widget_set_visible(image, TRUE);
	} else {
		gtk_image_clear(GTK_IMAGE(image));
		gtk_widget_set_visible(image, FALSE);
	}
}

static void
field_list_selection_changed_cb(GtkSelectionModel *model, guint position,
                                guint n_items, PurpleRequestField *field)
{
	GtkBitset *set;
	GtkBitsetIter iter;
	guint i;

	purple_request_field_list_clear_selected(field);

	set = gtk_selection_model_get_selection(model);
	if (gtk_bitset_iter_init_first(&iter, set, &i)) {
		do {
			GtkStringObject *obj = g_list_model_get_item(G_LIST_MODEL(model), i);

			purple_request_field_list_add_selected(field,
				gtk_string_object_get_string(obj));
			g_object_unref(obj);
		} while (gtk_bitset_iter_next(&iter, &i));
	}
	gtk_bitset_unref(set);

	field_changed(field);
}

static GtkWidget *
create_list_field(PidginRequestData *data, PurpleRequestField *field)
{
	GtkStringList *list = gtk_string_list_new(NULL);
	GtkSelectionModel *selection;
	GtkListItemFactory *factory;
	GHashTable *icons = NULL;
	GtkWidget *view, *sw;
	GList *l, *icon_l;
	guint i;

	icon_l = purple_request_field_list_get_icons(field);
	if (icon_l != NULL)
		icons = g_hash_table_new(g_str_hash, g_str_equal);

	for (l = purple_request_field_list_get_items(field); l != NULL; l = l->next) {
		gtk_string_list_append(list, l->data);
		if (icon_l != NULL) {
			if (icon_l->data != NULL)
				g_hash_table_insert(icons, l->data, icon_l->data);
			icon_l = icon_l->next;
		}
	}

	if (purple_request_field_list_get_multi_select(field)) {
		selection = GTK_SELECTION_MODEL(gtk_multi_selection_new(G_LIST_MODEL(list)));
	} else {
		GtkSingleSelection *single = gtk_single_selection_new(G_LIST_MODEL(list));

		gtk_single_selection_set_autoselect(single, FALSE);
		gtk_single_selection_set_can_unselect(single, TRUE);
		gtk_single_selection_set_selected(single, GTK_INVALID_LIST_POSITION);
		selection = GTK_SELECTION_MODEL(single);
	}

	/* Pre-selected items, before the handler is connected (as in Pidgin 2)
	 * so the field's selection is not rewritten while we go. */
	for (i = 0, l = purple_request_field_list_get_items(field); l != NULL; l = l->next, i++) {
		if (purple_request_field_list_is_selected(field, l->data))
			gtk_selection_model_select_item(selection, i, FALSE);
	}

	factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(list_item_setup_cb), NULL);
	g_signal_connect(factory, "bind", G_CALLBACK(list_item_bind_cb), icons);
	if (icons != NULL)
		g_object_set_data_full(G_OBJECT(factory), "pidgin-icons", icons,
		                       (GDestroyNotify)g_hash_table_unref);

	view = gtk_list_view_new(selection, factory);

	connect_field(data, selection, "selection-changed",
	              G_CALLBACK(field_list_selection_changed_cb), field);

	sw = pidgin_make_scrollable(view, GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC,
	                            icons ? 200 : -1, icons ? 300 : 120);
	gtk_widget_set_vexpand(sw, TRUE);
	g_object_set_data(G_OBJECT(sw), "pidgin-focus-widget", view);

	return sw;
}

static GtkWidget *
create_image_field(PidginRequestData *data, PurpleRequestField *field)
{
	GdkTexture *texture, *scaled;
	GtkWidget *picture;
	int width, height, factor;
	unsigned int sx, sy;

	texture = pidgin_texture_new_from_data(
		purple_request_field_image_get_buffer(field),
		purple_request_field_image_get_size(field));
	if (texture == NULL)
		return gtk_label_new(_("(The image could not be shown.)"));

	width = gdk_texture_get_width(texture);
	height = gdk_texture_get_height(texture);
	sx = purple_request_field_image_get_scale_x(field);
	sy = purple_request_field_image_get_scale_y(field);

	/*
	 * Honour the requested scale, and make small images (Discord's login
	 * QR code) at least IMAGE_FIELD_MIN_SIZE so a phone can scan them.
	 * Integer nearest-neighbour scaling keeps the modules sharp.
	 */
	factor = MAX(1, (int)MAX(sx, sy));
	while (MAX(width, height) * factor < IMAGE_FIELD_MIN_SIZE)
		factor++;
	scaled = texture_scale_nearest(texture, factor);
	g_object_unref(texture);

	picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(scaled));
	gtk_picture_set_can_shrink(GTK_PICTURE(picture), FALSE);
	gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_SCALE_DOWN);
	gtk_widget_set_halign(picture, GTK_ALIGN_CENTER);
	g_object_unref(scaled);

	return picture;
}

static void
field_account_changed_cb(GObject *dropdown, GParamSpec *pspec, PurpleRequestField *field)
{
	purple_request_field_account_set_value(field,
		pidgin_account_dropdown_get_selected(GTK_WIDGET(dropdown)));
	field_changed(field);
}

static GtkWidget *
create_account_field(PidginRequestData *data, PurpleRequestField *field)
{
	GtkWidget *widget;

	widget = pidgin_account_dropdown_new(
		purple_request_field_account_get_default_value(field),
		purple_request_field_account_get_show_all(field),
		purple_request_field_account_get_filter(field),
		NULL);
	gtk_widget_set_hexpand(widget, TRUE);

	/* The field's value is whatever the drop-down shows. */
	purple_request_field_account_set_value(field,
		pidgin_account_dropdown_get_selected(widget));

	connect_field(data, widget, "notify::selected",
	              G_CALLBACK(field_account_changed_cb), field);

	return widget;
}

static void
multifield_respond(PidginRequestData *data, int which)
{
	if (data->cbs[which] != NULL)
		((PurpleRequestFieldsCb)data->cbs[which])(data->user_data,
		                                          data->u.multifield.fields);

	purple_request_close(PURPLE_REQUEST_FIELDS, data);
}

static void
multifield_ok_cb(GtkButton *button, PidginRequestData *data)
{
	/* The Enter key can activate OK while a required field is empty. */
	if (!purple_request_fields_all_required_filled(data->u.multifield.fields))
		return;
	multifield_respond(data, 0);
}

static void
multifield_cancel_cb(GtkButton *button, PidginRequestData *data)
{
	multifield_respond(data, 1);
}

/* A label for the grid: "Label:" with a mnemonic, as in Pidgin 2. */
static GtkWidget *
field_label_new(const char *field_label, gboolean add_colon)
{
	GtkWidget *label;
	char *text = NULL;

	if (add_colon && field_label[0] != '\0' &&
	    field_label[strlen(field_label) - 1] != ':')
		text = g_strdup_printf("%s:", field_label);

	label = gtk_label_new(NULL);
	/* Pidgin 2 used markup here and prpls rely on it (e.g. <b>). */
	gtk_label_set_markup_with_mnemonic(GTK_LABEL(label), text ? text : field_label);
	if (gtk_label_get_text(GTK_LABEL(label))[0] == '\0' && field_label[0] != '\0')
		gtk_label_set_text_with_mnemonic(GTK_LABEL(label), text ? text : field_label);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	g_free(text);

	return label;
}

static void *
pidgin_request_fields(const char *title, const char *primary,
						const char *secondary, PurpleRequestFields *fields,
						const char *ok_text, GCallback ok_cb,
						const char *cancel_text, GCallback cancel_cb,
					    PurpleAccount *account, const char *who, PurpleConversation *conv,
						void *user_data)
{
	PidginRequestData *data;
	GtkWidget *content, *vbox, *sw, *button;
	GtkWidget *first_focus = NULL;
	GtkSizeGroup *sg;
	GList *gl, *fl;

	data            = g_new0(PidginRequestData, 1);
	data->type      = PURPLE_REQUEST_FIELDS;
	data->user_data = user_data;
	data->u.multifield.fields = fields;
	data->u.multifield.connections = g_array_new(FALSE, FALSE, sizeof(FieldConnection));

	fields->ui_data = data;

	data->cb_count = 2;
	data->cbs = g_new0(GCallback, 2);

	data->cbs[0] = ok_cb;
	data->cbs[1] = cancel_cb;

	request_window_new(data, title, "multifield", ICON_QUESTION, primary,
	                   secondary, account, conv, TRUE);
	gtk_window_set_default_size(GTK_WINDOW(data->dialog), 460, -1);
	content = pidgin_dialog_get_content_area(data->dialog);

	/* Fields scroll when there are many; small requests keep their
	 * natural size. */
	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BORDER);
	sw = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER,
	                               GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(sw), TRUE);
	gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(sw), TRUE);
	gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(sw), 600);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), vbox);
	gtk_widget_set_vexpand(sw, TRUE);
	gtk_box_append(GTK_BOX(content), sw);

	sg = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);

	for (gl = purple_request_fields_get_groups(fields); gl != NULL; gl = gl->next) {
		PurpleRequestFieldGroup *group = gl->data;
		GtkWidget *frame, *grid;
		int row = 0;

		if (purple_request_field_group_get_title(group) != NULL)
			frame = pidgin_make_frame(vbox, purple_request_field_group_get_title(group));
		else
			frame = vbox;

		grid = gtk_grid_new();
		gtk_grid_set_row_spacing(GTK_GRID(grid), PIDGIN_HIG_BOX_SPACE);
		gtk_grid_set_column_spacing(GTK_GRID(grid), PIDGIN_HIG_BOX_SPACE);
		gtk_box_append(GTK_BOX(frame), grid);

		for (fl = purple_request_field_group_get_fields(group); fl != NULL; fl = fl->next) {
			PurpleRequestField *field = fl->data;
			PurpleRequestFieldType type;
			const char *field_label;
			GtkWidget *label = NULL, *widget = NULL, *focus;
			gboolean wide;

			if (!purple_request_field_is_visible(field))
				continue;

			type = purple_request_field_get_type(field);
			field_label = purple_request_field_get_label(field);

			/* These take the whole row, with their label above. */
			wide = (type == PURPLE_REQUEST_FIELD_LABEL ||
			        type == PURPLE_REQUEST_FIELD_LIST ||
			        type == PURPLE_REQUEST_FIELD_IMAGE ||
			        (type == PURPLE_REQUEST_FIELD_STRING &&
			         purple_request_field_string_is_multiline(field)));

			if (type != PURPLE_REQUEST_FIELD_BOOLEAN && field_label != NULL) {
				label = field_label_new(field_label,
				                        type != PURPLE_REQUEST_FIELD_LABEL);
				if (wide) {
					gtk_grid_attach(GTK_GRID(grid), label, 0, row, 2, 1);
					row++;
				} else {
					gtk_size_group_add_widget(sg, label);
					gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
				}
			}

			switch (type) {
			case PURPLE_REQUEST_FIELD_STRING:
				widget = create_string_field(data, field);
				break;
			case PURPLE_REQUEST_FIELD_INTEGER:
				widget = create_int_field(data, field);
				break;
			case PURPLE_REQUEST_FIELD_BOOLEAN:
				widget = create_bool_field(data, field);
				break;
			case PURPLE_REQUEST_FIELD_CHOICE:
				widget = create_choice_field(data, field);
				break;
			case PURPLE_REQUEST_FIELD_LIST:
				widget = create_list_field(data, field);
				break;
			case PURPLE_REQUEST_FIELD_IMAGE:
				widget = create_image_field(data, field);
				break;
			case PURPLE_REQUEST_FIELD_ACCOUNT:
				widget = create_account_field(data, field);
				break;
			case PURPLE_REQUEST_FIELD_LABEL:
			default:
				break;
			}

			if (widget == NULL) {
				if (label == NULL)
					continue;
				row++;
				continue;
			}

			focus = g_object_get_data(G_OBJECT(widget), "pidgin-focus-widget");
			if (focus == NULL)
				focus = widget;

			if (label != NULL)
				pidgin_set_accessible_label(focus, label);

			if (wide || type == PURPLE_REQUEST_FIELD_BOOLEAN)
				gtk_grid_attach(GTK_GRID(grid), widget, 0, row, 2, 1);
			else
				gtk_grid_attach(GTK_GRID(grid), widget, 1, row, 1, 1);
			row++;

			if (first_focus == NULL &&
			    (type == PURPLE_REQUEST_FIELD_STRING ||
			     type == PURPLE_REQUEST_FIELD_INTEGER) &&
			    (type != PURPLE_REQUEST_FIELD_STRING ||
			     purple_request_field_string_is_editable(field)))
				first_focus = focus;

			purple_request_field_set_ui_data(field, widget);
		}
	}

	g_object_unref(sg);

	add_button(data, cancel_text, G_CALLBACK(multifield_cancel_cb));
	button = add_button(data, ok_text, G_CALLBACK(multifield_ok_cb));
	gtk_widget_add_css_class(button, "suggested-action");
	data->ok_button = button;
	gtk_window_set_default_widget(GTK_WINDOW(data->dialog), button);

	if (!purple_request_fields_all_required_filled(fields))
		gtk_widget_set_sensitive(data->ok_button, FALSE);

	if (first_focus != NULL)
		gtk_window_set_focus(GTK_WINDOW(data->dialog), first_focus);

	gtk_window_present(GTK_WINDOW(data->dialog));

	return data;
}

/**************************************************************************
 * The window's close button / Escape
 **************************************************************************/

static gboolean
request_window_close_cb(GtkWindow *window, PidginRequestData *data)
{
	switch (data->type) {
	case PURPLE_REQUEST_INPUT:
		input_respond(data, 1);
		break;
	case PURPLE_REQUEST_CHOICE:
		choice_respond(data, 0);
		break;
	case PURPLE_REQUEST_FIELDS:
		multifield_respond(data, 1);
		break;
	case PURPLE_REQUEST_ACTION:
	default:
		/* Pidgin 2: no action matched GTK_RESPONSE_DELETE_EVENT. */
		purple_request_close(data->type, data);
		break;
	}

	/* The window is already destroyed by close_request. */
	return TRUE;
}

/**************************************************************************
 * Files and folders
 **************************************************************************/

#define FILELOCATIONS PIDGIN4_PREFS_ROOT "/filelocations"

typedef struct
{
	PidginRequestData *data;
	GCancellable *cancellable;
} FileDialogClosure;

/* The last folders are pidgin4's own (/pidgin4/filelocations); Pidgin 2's
 * /pidgin/filelocations keys are only read, as the starting point. */
static const char *
last_folder_get(gboolean save)
{
	const char *key = save ? "last_save_folder" : "last_open_folder";
	char *pref = g_strdup_printf(FILELOCATIONS "/%s", key);
	const char *value = NULL;

	if (purple_prefs_exists(pref))
		value = purple_prefs_get_path(pref);
	g_free(pref);

	if (value == NULL || *value == '\0') {
		pref = g_strdup_printf(PIDGIN_PREFS_ROOT "/filelocations/%s", key);
		if (purple_prefs_exists(pref))
			value = purple_prefs_get_path(pref);
		g_free(pref);
	}

	return value;
}

static void
last_folder_set(gboolean save, const char *folder)
{
	const char *pref = save ? FILELOCATIONS "/last_save_folder"
	                        : FILELOCATIONS "/last_open_folder";

	purple_prefs_add_none(FILELOCATIONS);
	purple_prefs_add_path(FILELOCATIONS "/last_save_folder", "");
	purple_prefs_add_path(FILELOCATIONS "/last_open_folder", "");
	purple_prefs_set_path(pref, folder);
}

static void
file_dialog_done_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
	FileDialogClosure *closure = user_data;
	PidginRequestData *data;
	GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
	GError *error = NULL;
	GFile *file = NULL;
	char *path = NULL;

	if (g_cancellable_is_cancelled(closure->cancellable)) {
		/* close_request freed the request: finish and forget. */
		if (closure->data == NULL || TRUE) {
			GFile *f = NULL;

			if (G_IS_OBJECT(source)) {
				/* Collect the result so GTK can free it. */
				if (g_object_get_data(source, "pidgin-kind") == GINT_TO_POINTER(2))
					f = gtk_file_dialog_select_folder_finish(dialog, result, NULL);
				else if (g_object_get_data(source, "pidgin-kind") == GINT_TO_POINTER(1))
					f = gtk_file_dialog_save_finish(dialog, result, NULL);
				else
					f = gtk_file_dialog_open_finish(dialog, result, NULL);
			}
			g_clear_object(&f);
		}
		g_object_unref(closure->cancellable);
		g_free(closure);
		return;
	}

	data = closure->data;

	switch (GPOINTER_TO_INT(g_object_get_data(source, "pidgin-kind"))) {
	case 2:
		file = gtk_file_dialog_select_folder_finish(dialog, result, &error);
		break;
	case 1:
		file = gtk_file_dialog_save_finish(dialog, result, &error);
		break;
	default:
		file = gtk_file_dialog_open_finish(dialog, result, &error);
		break;
	}

	g_object_unref(closure->cancellable);
	g_free(closure);

	if (file != NULL)
		path = g_file_get_path(file);

	if (path == NULL) {
		if (error != NULL &&
		    !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) &&
		    !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED))
			purple_debug_error("gtkrequest", "File dialog failed: %s\n",
			                   error->message);
		else if (file != NULL)
			purple_debug_error("gtkrequest", "The chosen file is not local\n");

		if (data->cbs[0] != NULL)
			((PurpleRequestFileCb)data->cbs[0])(data->user_data, NULL);
	} else {
		if (data->type == PURPLE_REQUEST_FOLDER) {
			last_folder_set(FALSE, path);
		} else {
			char *folder = g_path_get_dirname(path);

			last_folder_set(data->u.file.savedialog, folder);
			g_free(folder);
		}

		/* GtkFileDialog already asked before overwriting a file. */
		if (data->cbs[1] != NULL)
			((PurpleRequestFileCb)data->cbs[1])(data->user_data, path);
	}

	g_clear_error(&error);
	g_clear_object(&file);
	g_free(path);

	purple_request_close(data->type, data);
}

static FileDialogClosure *
file_closure_new(PidginRequestData *data)
{
	FileDialogClosure *closure = g_new0(FileDialogClosure, 1);

	closure->data = data;
	closure->cancellable = g_object_ref(data->u.file.cancellable);
	return closure;
}

static void *
pidgin_request_file(const char *title, const char *filename,
					  gboolean savedialog,
					  GCallback ok_cb, GCallback cancel_cb,
					  PurpleAccount *account, const char *who, PurpleConversation *conv,
					  void *user_data)
{
	PidginRequestData *data;
	GtkFileDialog *dialog;
	const char *current_folder;
	gboolean have_file = (filename != NULL && *filename != '\0');

	data = g_new0(PidginRequestData, 1);
	data->type = PURPLE_REQUEST_FILE;
	data->user_data = user_data;
	data->cb_count = 2;
	data->cbs = g_new0(GCallback, 2);
	data->cbs[0] = cancel_cb;
	data->cbs[1] = ok_cb;
	data->u.file.savedialog = savedialog;
	data->u.file.cancellable = g_cancellable_new();

	dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, title ? title :
		(savedialog ? _("Save File...") : _("Open File...")));
	gtk_file_dialog_set_modal(dialog, FALSE);
	g_object_set_data(G_OBJECT(dialog), "pidgin-kind", GINT_TO_POINTER(savedialog ? 1 : 0));

	current_folder = last_folder_get(savedialog);

	if (have_file && g_path_is_absolute(filename) &&
	    g_file_test(filename, G_FILE_TEST_EXISTS)) {
		GFile *file = g_file_new_for_path(filename);

		gtk_file_dialog_set_initial_file(dialog, file);
		g_object_unref(file);
	} else {
		if (current_folder != NULL && *current_folder != '\0' &&
		    g_file_test(current_folder, G_FILE_TEST_IS_DIR)) {
			GFile *folder = g_file_new_for_path(current_folder);

			gtk_file_dialog_set_initial_folder(dialog, folder);
			g_object_unref(folder);
		}
		if (have_file && savedialog) {
			char *base = g_path_get_basename(filename);

			gtk_file_dialog_set_initial_name(dialog, base);
			g_free(base);
		}
	}

	if (savedialog)
		gtk_file_dialog_save(dialog, pidgin_get_dialog_parent(),
		                     data->u.file.cancellable, file_dialog_done_cb,
		                     file_closure_new(data));
	else
		gtk_file_dialog_open(dialog, pidgin_get_dialog_parent(),
		                     data->u.file.cancellable, file_dialog_done_cb,
		                     file_closure_new(data));
	g_object_unref(dialog);

	return (void *)data;
}

static void *
pidgin_request_folder(const char *title, const char *dirname,
					  GCallback ok_cb, GCallback cancel_cb,
					  PurpleAccount *account, const char *who, PurpleConversation *conv,
					  void *user_data)
{
	PidginRequestData *data;
	GtkFileDialog *dialog;

	data = g_new0(PidginRequestData, 1);
	data->type = PURPLE_REQUEST_FOLDER;
	data->user_data = user_data;
	data->cb_count = 2;
	data->cbs = g_new0(GCallback, 2);
	data->cbs[0] = cancel_cb;
	data->cbs[1] = ok_cb;
	data->u.file.savedialog = FALSE;
	data->u.file.cancellable = g_cancellable_new();

	dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, title ? title : _("Select Folder..."));
	gtk_file_dialog_set_modal(dialog, FALSE);
	g_object_set_data(G_OBJECT(dialog), "pidgin-kind", GINT_TO_POINTER(2));

	if (dirname != NULL && *dirname != '\0') {
		GFile *folder = g_file_new_for_path(dirname);

		gtk_file_dialog_set_initial_folder(dialog, folder);
		g_object_unref(folder);
	}

	gtk_file_dialog_select_folder(dialog, pidgin_get_dialog_parent(),
	                              data->u.file.cancellable, file_dialog_done_cb,
	                              file_closure_new(data));
	g_object_unref(dialog);

	return (void *)data;
}

/**************************************************************************
 * UI ops
 **************************************************************************/

static PurpleRequestUiOps ops =
{
	pidgin_request_input,
	pidgin_request_choice,
	pidgin_request_action,
	pidgin_request_fields,
	pidgin_request_file,
	pidgin_close_request,
	pidgin_request_folder,
	pidgin_request_action_with_icon,
	NULL, /* request_screenshare_media: no voice/video */
	NULL,
	NULL
};

PurpleRequestUiOps *
pidgin_request_get_ui_ops(void)
{
	return &ops;
}

/**************************************************************************
 * Self-test (developer aid)
 **************************************************************************/

#define SELFTEST "gtkrequest-selftest"

static GBytes *
selftest_png(void)
{
	static GBytes *png = NULL;

	if (png == NULL) {
		/* A 21x21 checkerboard-ish pattern, like a QR code's size. */
		const int size = 21;
		guchar *pixels = g_malloc(size * size * 4);
		GBytes *bytes;
		GdkTexture *texture;
		int x, y;

		for (y = 0; y < size; y++) {
			for (x = 0; x < size; x++) {
				guchar *p = pixels + (y * size + x) * 4;
				guchar v = ((x / 3 + y / 3) % 2) ? 0x00 : 0xff;

				p[0] = p[1] = p[2] = v;
				p[3] = 0xff;
			}
		}
		bytes = g_bytes_new_take(pixels, size * size * 4);
		texture = gdk_memory_texture_new(size, size, GDK_MEMORY_R8G8B8A8,
		                                 bytes, size * 4);
		png = gdk_texture_save_to_png_bytes(texture);
		g_object_unref(texture);
		g_bytes_unref(bytes);
	}

	return png;
}

static void
selftest_input_cb(void *user_data, const char *value)
{
	purple_debug_info(SELFTEST, "%s: \"%s\"\n", (const char *)user_data,
	                  value ? value : "(null)");
}

static void
selftest_choice_cb(void *user_data, int choice)
{
	purple_debug_info(SELFTEST, "%s: choice %d\n", (const char *)user_data, choice);
}

static void
selftest_action_cb(void *user_data, int id)
{
	purple_debug_info(SELFTEST, "%s: action %d\n", (const char *)user_data, id);
}

static void
selftest_fields_cb(void *user_data, PurpleRequestFields *fields)
{
	GList *sel = purple_request_field_list_get_selected(
		purple_request_fields_get_field(fields, "list"));
	PurpleAccount *account = purple_request_fields_get_account(fields, "account");

	purple_debug_info(SELFTEST, "%s: string=\"%s\" required=\"%s\" "
		"multiline=\"%s\" password=\"%s\" int=%d bool=%d choice=%d "
		"bigchoice=%d list=%u selected account=%s\n",
		(const char *)user_data,
		purple_request_fields_get_string(fields, "string") ?: "",
		purple_request_fields_get_string(fields, "required") ?: "",
		purple_request_fields_get_string(fields, "multiline") ?: "",
		purple_request_fields_get_string(fields, "password") ?: "",
		purple_request_fields_get_integer(fields, "int"),
		purple_request_fields_get_bool(fields, "bool"),
		purple_request_fields_get_choice(fields, "choice"),
		purple_request_fields_get_choice(fields, "bigchoice"),
		g_list_length(sel),
		account ? purple_account_get_username(account) : "(none)");
}

void
pidgin_request_selftest(void)
{
	PurpleRequestFields *fields;
	PurpleRequestFieldGroup *group;
	PurpleRequestField *field;
	GBytes *png = selftest_png();
	int i;

	purple_debug_info(SELFTEST, "opening one request of every kind\n");

	purple_request_input(NULL, "Self-test: input", "Enter some text",
		"This is a single-line input.", "default text", FALSE, FALSE, NULL,
		_("_OK"), G_CALLBACK(selftest_input_cb),
		_("_Cancel"), G_CALLBACK(selftest_input_cb),
		NULL, NULL, NULL, "input");

	purple_request_input(NULL, "Self-test: masked input", "Steam Guard",
		"Enter the code from your authenticator.", NULL, FALSE, TRUE, NULL,
		_("_OK"), G_CALLBACK(selftest_input_cb),
		_("_Cancel"), G_CALLBACK(selftest_input_cb),
		NULL, NULL, NULL, "masked input");

	purple_request_input(NULL, "Self-test: multi-line input", "Set a profile",
		NULL, "line one\nline two", TRUE, FALSE, "html",
		_("_Save"), G_CALLBACK(selftest_input_cb),
		_("_Cancel"), G_CALLBACK(selftest_input_cb),
		NULL, NULL, NULL, "multi-line input");

	purple_request_choice(NULL, "Self-test: choice", "Pick one", "Choices:", 2,
		_("_OK"), G_CALLBACK(selftest_choice_cb),
		_("_Cancel"), G_CALLBACK(selftest_choice_cb),
		NULL, NULL, NULL, "choice",
		"First", 1, "Second", 2, "Third", 3, NULL);

	purple_request_action(NULL, "Self-test: action", "Approve this login",
		"The login will continue once it is approved.", 1,
		NULL, NULL, NULL, "action", 3,
		"_Yes", G_CALLBACK(selftest_action_cb),
		"_No", G_CALLBACK(selftest_action_cb),
		"_Later", G_CALLBACK(selftest_action_cb));

	purple_request_action_with_icon(NULL, "Self-test: action with icon",
		"Accept this certificate?", "An icon should show on the left.", 0,
		NULL, NULL, NULL, g_bytes_get_data(png, NULL), g_bytes_get_size(png),
		"action with icon", 2,
		_("_Accept"), G_CALLBACK(selftest_action_cb),
		_("_Reject"), G_CALLBACK(selftest_action_cb));

	fields = purple_request_fields_new();

	group = purple_request_field_group_new(NULL);
	purple_request_fields_add_group(fields, group);
	field = purple_request_field_label_new("label", "A label field with <b>markup</b>.");
	purple_request_field_group_add_field(group, field);
	field = purple_request_field_string_new("string", "_String", "value", FALSE);
	purple_request_field_group_add_field(group, field);
	field = purple_request_field_string_new("required", "_Required", NULL, FALSE);
	purple_request_field_set_required(field, TRUE);
	purple_request_field_group_add_field(group, field);
	field = purple_request_field_string_new("password", "_Password", "secret", FALSE);
	purple_request_field_string_set_masked(field, TRUE);
	purple_request_field_group_add_field(group, field);
	field = purple_request_field_string_new("readonly", "Read-only", "not editable", FALSE);
	purple_request_field_string_set_editable(field, FALSE);
	purple_request_field_group_add_field(group, field);
	field = purple_request_field_string_new("hidden", "Hidden", "invisible", FALSE);
	purple_request_field_set_visible(field, FALSE);
	purple_request_field_group_add_field(group, field);
	field = purple_request_field_string_new("multiline", "_Multi-line", "one\ntwo", TRUE);
	purple_request_field_group_add_field(group, field);

	group = purple_request_field_group_new("Other types");
	purple_request_fields_add_group(fields, group);
	field = purple_request_field_int_new("int", "_Integer", 42);
	purple_request_field_group_add_field(group, field);
	field = purple_request_field_bool_new("bool", "_Boolean", TRUE);
	purple_request_field_group_add_field(group, field);
	field = purple_request_field_choice_new("choice", "_Choice", 1);
	purple_request_field_choice_add(field, "Alpha");
	purple_request_field_choice_add(field, "Beta");
	purple_request_field_choice_add(field, "Gamma");
	purple_request_field_group_add_field(group, field);
	field = purple_request_field_choice_new("bigchoice", "Big c_hoice", 4);
	for (i = 0; i < 8; i++) {
		char *s = g_strdup_printf("Option %d", i);
		purple_request_field_choice_add(field, s);
		g_free(s);
	}
	purple_request_field_group_add_field(group, field);
	field = purple_request_field_list_new("list", "_List (multi-select)");
	purple_request_field_list_set_multi_select(field, TRUE);
	for (i = 0; i < 6; i++) {
		char *s = g_strdup_printf("Item %d", i);
		purple_request_field_list_add_icon(field, s, NULL, GINT_TO_POINTER(i));
		g_free(s);
	}
	purple_request_field_list_add_selected(field, "Item 1");
	purple_request_field_list_add_selected(field, "Item 3");
	purple_request_field_group_add_field(group, field);
	field = purple_request_field_list_new("single", "Single-select list");
	purple_request_field_list_add_icon(field, "One", NULL, NULL);
	purple_request_field_list_add_icon(field, "Two", NULL, NULL);
	purple_request_field_list_add_selected(field, "Two");
	purple_request_field_group_add_field(group, field);
	field = purple_request_field_account_new("account", "_Account", NULL);
	purple_request_field_account_set_show_all(field, TRUE);
	purple_request_field_group_add_field(group, field);

	group = purple_request_field_group_new("Image (QR code)");
	purple_request_fields_add_group(fields, group);
	field = purple_request_field_image_new("image", "QR Code Image",
		g_bytes_get_data(png, NULL), g_bytes_get_size(png));
	purple_request_field_image_set_scale(field, 4, 4);
	purple_request_field_group_add_field(group, field);

	purple_request_fields(NULL, "Self-test: fields", "Every field type",
		"OK stays disabled until Required is filled.", fields,
		_("_OK"), G_CALLBACK(selftest_fields_cb),
		_("_Cancel"), G_CALLBACK(selftest_fields_cb),
		NULL, NULL, NULL, "fields");
}
