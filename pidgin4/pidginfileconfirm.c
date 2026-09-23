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
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "gtkutils.h"
#include "pidginfileconfirm.h"

typedef struct
{
	PidginFileConfirmFunc send;
	gpointer user_data;
	GDestroyNotify destroy;
} FileConfirm;

static void
file_confirm_free(gpointer data)
{
	FileConfirm *fc = data;

	if (fc->destroy != NULL)
		fc->destroy(fc->user_data);
	g_free(fc);
}

static void
send_clicked_cb(GtkButton *button, GtkWindow *win)
{
	FileConfirm *fc = g_object_steal_data(G_OBJECT(win), "pidgin-file-confirm");

	if (fc == NULL)
		return;
	if (fc->send != NULL)
		fc->send(fc->user_data);
	file_confirm_free(fc);
	gtk_window_destroy(win);
}

/* Cancel, Escape, closed, or destroyed by the owner: now, not at finalize */
static void
win_destroy_cb(GtkWindow *win, gpointer data)
{
	FileConfirm *fc = g_object_steal_data(G_OBJECT(win), "pidgin-file-confirm");

	if (fc != NULL)
		file_confirm_free(fc);
}

static void
cancel_clicked_cb(GtkButton *button, GtkWindow *win)
{
	gtk_window_destroy(win);
}

/* "PNG" for image/png, "SVG" for image/svg+xml; the description for
 * anything else. */
static char *
format_name(const char *content_type)
{
	char *mime = content_type ? g_content_type_get_mime_type(content_type) : NULL;
	char *ret = NULL;

	if (mime != NULL && g_str_has_prefix(mime, "image/")) {
		const char *sub = mime + strlen("image/");
		const char *plus = strchr(sub, '+');

		ret = g_ascii_strup(sub, plus ? plus - sub : -1);
	} else if (content_type != NULL) {
		ret = g_content_type_get_description(content_type);
	}
	g_free(mime);
	return ret;
}

GtkWidget *
pidgin_file_confirm_new(GtkWindow *parent, GBytes *data, const char *path,
                        const char *filename, const char *recipient,
                        const char *account, PidginFileConfirmFunc send,
                        gpointer user_data, GDestroyNotify destroy)
{
	GtkWidget *win, *content, *hbox, *vbox, *label, *preview, *button;
	GdkTexture *texture = NULL;
	FileConfirm *fc;
	char *content_type = NULL, *format, *size, *details, *markup, *to;
	guint64 bytes = 0;

	g_return_val_if_fail(data != NULL || path != NULL, NULL);

	if (filename == NULL)
		filename = path;

	if (data != NULL) {
		bytes = g_bytes_get_size(data);
		content_type = g_content_type_guess(filename, g_bytes_get_data(data, NULL), bytes,
		                                    NULL);
		if (g_content_type_is_a(content_type, "image/*"))
			texture = gdk_texture_new_from_bytes(data, NULL);
	} else {
		GFile *file = g_file_new_for_path(path);
		GFileInfo *info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE ","
		                                    G_FILE_ATTRIBUTE_STANDARD_SIZE,
		                                    G_FILE_QUERY_INFO_NONE, NULL, NULL);

		if (info != NULL) {
			bytes = g_file_info_get_size(info);
			content_type = g_strdup(g_file_info_get_content_type(info));
			g_object_unref(info);
		}
		if (content_type == NULL)
			content_type = g_content_type_guess(path, NULL, 0, NULL);
		if (g_content_type_is_a(content_type, "image/*"))
			texture = gdk_texture_new_from_file(file, NULL);
		g_object_unref(file);
	}

	win = pidgin_dialog_new(texture ? _("Send Image") : _("Send File"), parent,
	                        "file-confirm", FALSE);
	gtk_window_set_modal(GTK_WINDOW(win), TRUE);
	content = pidgin_dialog_get_content_area(win);

	/* the preview (the image, capped, aspect kept), or the type's icon */
	if (texture != NULL) {
		int w = gdk_texture_get_width(texture), h = gdk_texture_get_height(texture);
		double scale = MIN(1.0, (double)PIDGIN_FILE_CONFIRM_PREVIEW_MAX / MAX(MAX(w, h), 1));

		preview = gtk_picture_new_for_paintable(GDK_PAINTABLE(texture));
		gtk_picture_set_content_fit(GTK_PICTURE(preview), GTK_CONTENT_FIT_CONTAIN);
		gtk_picture_set_can_shrink(GTK_PICTURE(preview), TRUE);
		gtk_widget_set_size_request(preview, MAX(1, (int)(w * scale)),
		                            MAX(1, (int)(h * scale)));
		gtk_widget_set_halign(preview, GTK_ALIGN_CENTER);
		gtk_box_append(GTK_BOX(content), preview);
		vbox = content;
	} else {
		GIcon *icon = content_type ? g_content_type_get_icon(content_type) : NULL;

		hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BORDER);
		preview = icon ? gtk_image_new_from_gicon(icon)
		               : gtk_image_new_from_icon_name("text-x-generic");
		gtk_image_set_pixel_size(GTK_IMAGE(preview), 64);
		gtk_widget_set_valign(preview, GTK_ALIGN_START);
		gtk_box_append(GTK_BOX(hbox), preview);
		vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BOX_SPACE);
		gtk_widget_set_hexpand(vbox, TRUE);
		gtk_box_append(GTK_BOX(hbox), vbox);
		gtk_box_append(GTK_BOX(content), hbox);
		if (icon != NULL)
			g_object_unref(icon);
	}
	gtk_widget_set_name(preview, "file-confirm-preview");

	/* the name */
	label = gtk_label_new(NULL);
	markup = g_markup_printf_escaped("<b>%s</b>", filename);
	gtk_label_set_markup(GTK_LABEL(label), markup);
	g_free(markup);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
	gtk_label_set_max_width_chars(GTK_LABEL(label), 50);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_box_append(GTK_BOX(vbox), label);

	/* dimensions, size, format */
	format = format_name(content_type);
	size = g_format_size(bytes);
	if (texture != NULL)
		/* Translators: width × height pixels · size · format */
		details = g_strdup_printf(_("%d × %d pixels · %s · %s"),
		                          gdk_texture_get_width(texture),
		                          gdk_texture_get_height(texture), size,
		                          format ? format : "");
	else if (format != NULL)
		details = g_strdup_printf("%s · %s", size, format);
	else
		details = g_strdup(size);
	label = gtk_label_new(details);
	gtk_widget_set_name(label, "file-confirm-details");
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_widget_add_css_class(label, "dim-label");
	gtk_box_append(GTK_BOX(vbox), label);
	g_free(details);
	g_free(size);
	g_free(format);

	/* the recipient */
	if (account != NULL && *account != '\0')
		to = g_markup_printf_escaped(_("Send to <b>%s</b> via %s"),
		                             recipient ? recipient : "", account);
	else
		to = g_markup_printf_escaped(_("Send to <b>%s</b>"), recipient ? recipient : "");
	label = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(label), to);
	gtk_widget_set_name(label, "file-confirm-recipient");
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_box_append(GTK_BOX(vbox), label);
	g_free(to);

	fc = g_new0(FileConfirm, 1);
	fc->send = send;
	fc->user_data = user_data;
	fc->destroy = destroy;
	g_object_set_data(G_OBJECT(win), "pidgin-file-confirm", fc);
	g_signal_connect(win, "destroy", G_CALLBACK(win_destroy_cb), NULL);

	pidgin_dialog_add_button(win, _("_Cancel"), G_CALLBACK(cancel_clicked_cb), win);
	button = pidgin_dialog_add_button(win, _("_Send"), G_CALLBACK(send_clicked_cb), win);
	gtk_widget_add_css_class(button, "suggested-action");
	gtk_window_set_default_widget(GTK_WINDOW(win), button);
	gtk_window_set_focus(GTK_WINDOW(win), button);

	pidgin_window_set_secondary(GTK_WINDOW(win));
	gtk_window_present(GTK_WINDOW(win));

	if (texture != NULL)
		g_object_unref(texture);
	g_free(content_type);
	return win;
}
