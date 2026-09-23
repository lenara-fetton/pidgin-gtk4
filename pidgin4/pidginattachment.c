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

#include <glib/gstdio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include "debug.h"
#include "prefs.h"
#include "util.h"

#include "gtkutils.h"
#include "pidginattachment.h"

struct _PidginAttachment
{
	GObject parent;

	PidginAttachmentKind kind;
	char *path;
	char *uri;
	char *name;
	goffset size;
};

G_DEFINE_FINAL_TYPE(PidginAttachment, pidgin_attachment, G_TYPE_OBJECT)

static PidginAttachmentLaunchHook launch_hook = NULL;

static void
pidgin_attachment_finalize(GObject *obj)
{
	PidginAttachment *att = PIDGIN_ATTACHMENT(obj);

	g_free(att->path);
	g_free(att->uri);
	g_free(att->name);
	G_OBJECT_CLASS(pidgin_attachment_parent_class)->finalize(obj);
}

static void
pidgin_attachment_class_init(PidginAttachmentClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = pidgin_attachment_finalize;
}

static void
pidgin_attachment_init(PidginAttachment *att)
{
	att->size = -1;
}

/**************************************************************************
 * Classification
 **************************************************************************/

static const char *const image_exts[] = {
	"png", "jpg", "jpeg", "jpe", "gif", "webp", "bmp", "avif", "heic", "tif", "tiff", NULL
};

static const char *const audio_exts[] = {
	"mp3", "ogg", "oga", "opus", "m4a", "aac", "flac", "wav", "weba", "amr", "mka", NULL
};

static const char *const video_exts[] = {
	"mp4", "m4v", "webm", "mkv", "mov", "ogv", "avi", "3gp", "mpeg", "mpg", NULL
};

static PidginAttachmentKind
kind_for_type(const char *type)
{
	if (g_str_has_prefix(type, "image/"))
		return PIDGIN_ATTACHMENT_IMAGE;
	if (g_str_has_prefix(type, "audio/"))
		return PIDGIN_ATTACHMENT_AUDIO;
	if (g_str_has_prefix(type, "video/"))
		return PIDGIN_ATTACHMENT_VIDEO;
	return PIDGIN_ATTACHMENT_NONE;
}

/* The extension of a name, path or URL's path, lowercase (or NULL). */
static char *
extension_of(const char *name)
{
	char *path = NULL, *ext = NULL;
	const char *base, *dot;
	GUri *uri;

	if (g_uri_peek_scheme(name) != NULL &&
	    (uri = g_uri_parse(name, G_URI_FLAGS_ENCODED, NULL)) != NULL) {
		path = g_strdup(g_uri_get_path(uri));
		g_uri_unref(uri);
	} else {
		path = g_strdup(name);
	}
	base = strrchr(path, '/');
	base = base ? base + 1 : path;
	dot = strrchr(base, '.');
	if (dot != NULL && dot[1] != '\0')
		ext = g_ascii_strdown(dot + 1, -1);
	g_free(path);
	return ext;
}

static gboolean
in_list(const char *ext, const char *const *list)
{
	int i;

	for (i = 0; list[i] != NULL; i++)
		if (purple_strequal(ext, list[i]))
			return TRUE;
	return FALSE;
}

PidginAttachmentKind
pidgin_attachment_classify(const char *name, const char *content_type)
{
	PidginAttachmentKind kind = PIDGIN_ATTACHMENT_NONE;
	char *ext;

	if (content_type != NULL && *content_type != '\0' &&
	    g_ascii_strncasecmp(content_type, "application/octet-stream", 24) != 0) {
		char *type = g_ascii_strdown(content_type, -1);

		kind = kind_for_type(type);
		g_free(type);
		return kind;
	}
	if (name == NULL || (ext = extension_of(name)) == NULL)
		return PIDGIN_ATTACHMENT_NONE;
	if (in_list(ext, image_exts))
		kind = PIDGIN_ATTACHMENT_IMAGE;
	else if (in_list(ext, audio_exts))
		kind = PIDGIN_ATTACHMENT_AUDIO;
	else if (in_list(ext, video_exts))
		kind = PIDGIN_ATTACHMENT_VIDEO;
	g_free(ext);
	return kind;
}

/**************************************************************************
 * Construction, getters
 **************************************************************************/

PidginAttachment *
pidgin_attachment_new_for_file(const char *path, PidginAttachmentKind kind)
{
	PidginAttachment *att;
	GStatBuf st;

	g_return_val_if_fail(path != NULL, NULL);

	att = g_object_new(PIDGIN_TYPE_ATTACHMENT, NULL);
	att->kind = kind;
	att->path = g_strdup(path);
	att->uri = g_filename_to_uri(path, NULL, NULL);
	att->name = g_filename_display_basename(path);
	if (g_stat(path, &st) == 0)
		att->size = st.st_size;
	return att;
}

PidginAttachment *
pidgin_attachment_new_for_uri(const char *uri, PidginAttachmentKind kind, goffset size)
{
	PidginAttachment *att;
	GUri *guri;

	g_return_val_if_fail(uri != NULL, NULL);

	att = g_object_new(PIDGIN_TYPE_ATTACHMENT, NULL);
	att->kind = kind;
	att->uri = g_strdup(uri);
	att->size = size;
	if ((guri = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL)) != NULL) {
		const char *path = g_uri_get_path(guri);
		const char *slash = path ? strrchr(path, '/') : NULL;

		if (slash != NULL && slash[1] != '\0')
			att->name = g_strdup(slash + 1);
		g_uri_unref(guri);
	}
	if (att->name == NULL)
		att->name = g_strdup(uri);
	return att;
}

PidginAttachmentKind
pidgin_attachment_get_kind(PidginAttachment *att)
{
	g_return_val_if_fail(PIDGIN_IS_ATTACHMENT(att), PIDGIN_ATTACHMENT_NONE);
	return att->kind;
}

const char *
pidgin_attachment_get_path(PidginAttachment *att)
{
	g_return_val_if_fail(PIDGIN_IS_ATTACHMENT(att), NULL);
	return att->path;
}

const char *
pidgin_attachment_get_uri(PidginAttachment *att)
{
	g_return_val_if_fail(PIDGIN_IS_ATTACHMENT(att), NULL);
	return att->uri;
}

const char *
pidgin_attachment_get_name(PidginAttachment *att)
{
	g_return_val_if_fail(PIDGIN_IS_ATTACHMENT(att), NULL);
	return att->name;
}

goffset
pidgin_attachment_get_size(PidginAttachment *att)
{
	g_return_val_if_fail(PIDGIN_IS_ATTACHMENT(att), -1);
	return att->size;
}

void
pidgin_attachment_set_launch_hook(PidginAttachmentLaunchHook hook)
{
	launch_hook = hook;
}

/**************************************************************************
 * Launching
 **************************************************************************/

static GtkWindow *
parent_of(GtkWidget *widget)
{
	GtkRoot *root = widget ? gtk_widget_get_root(widget) : NULL;

	return GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;
}

static void
file_launch_cb(GObject *source, GAsyncResult *res, gpointer data)
{
	GError *error = NULL;

	if (!gtk_file_launcher_launch_finish(GTK_FILE_LAUNCHER(source), res, &error)) {
		if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
			purple_debug_warning("attachment", "opening the file: %s\n", error->message);
		g_clear_error(&error);
	}
}

void
pidgin_attachment_open(GtkWidget *widget, PidginAttachment *att)
{
	g_return_if_fail(PIDGIN_IS_ATTACHMENT(att));

	if (launch_hook != NULL) {
		launch_hook("open", att->path ? att->path : att->uri);
		return;
	}
	if (att->path != NULL) {
		GFile *file = g_file_new_for_path(att->path);
		GtkFileLauncher *launcher = gtk_file_launcher_new(file);

		gtk_file_launcher_launch(launcher, parent_of(widget), NULL, file_launch_cb, NULL);
		g_object_unref(launcher);
		g_object_unref(file);
	} else if (att->uri != NULL) {
		pidgin_open_uri(parent_of(widget), att->uri);
	}
}

void
pidgin_attachment_play(GtkWidget *widget, PidginAttachment *att)
{
	g_return_if_fail(PIDGIN_IS_ATTACHMENT(att));

	if (launch_hook != NULL) {
		launch_hook("play", att->path ? att->path : att->uri);
		return;
	}
	pidgin_attachment_open(widget, att);
}

static void
folder_launch_cb(GObject *source, GAsyncResult *res, gpointer data)
{
	GError *error = NULL;

	if (!gtk_file_launcher_open_containing_folder_finish(GTK_FILE_LAUNCHER(source), res,
	                                                     &error)) {
		if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
			purple_debug_warning("attachment", "opening the folder: %s\n",
			                     error->message);
		g_clear_error(&error);
	}
}

static void
open_folder(GtkWidget *widget, PidginAttachment *att)
{
	GFile *file;
	GtkFileLauncher *launcher;

	if (att->path == NULL)
		return;
	if (launch_hook != NULL) {
		launch_hook("open-folder", att->path);
		return;
	}
	file = g_file_new_for_path(att->path);
	launcher = gtk_file_launcher_new(file);
	gtk_file_launcher_open_containing_folder(launcher, parent_of(widget), NULL,
	                                         folder_launch_cb, NULL);
	g_object_unref(launcher);
	g_object_unref(file);
}

/**************************************************************************
 * Media backend
 **************************************************************************/

#define MEDIA_PROBE_RESOURCE "/com/minowick/Pidgin4/media/silence.wav"

static int backend_state = -1;      /* -1 unknown, 0 no, 1 yes */
static GtkMediaStream *backend_probe = NULL;

static void
backend_probe_notify_cb(GtkMediaStream *stream, GParamSpec *pspec, gpointer data)
{
	const GError *error = gtk_media_stream_get_error(stream);

	if (error != NULL) {
		purple_debug_info("attachment", "the media backend can't play a WAV (%s): "
		                  "no inline playback\n", error->message);
		backend_state = 0;
	}
	if (error != NULL || gtk_media_stream_is_prepared(stream)) {
		g_signal_handlers_disconnect_by_func(stream, backend_probe_notify_cb, data);
		g_clear_object(&backend_probe);
	}
}

gboolean
pidgin_media_backend_available(void)
{
	const GError *error;

	if (backend_state >= 0)
		return backend_state == 1;

	backend_probe = gtk_media_file_new_for_resource(MEDIA_PROBE_RESOURCE);
	error = gtk_media_stream_get_error(backend_probe);
	if (error != NULL) {
		purple_debug_info("attachment", "no inline media playback: %s\n", error->message);
		backend_state = 0;
		g_clear_object(&backend_probe);
	} else {
		backend_state = 1;
		g_signal_connect(backend_probe, "notify::error",
		                 G_CALLBACK(backend_probe_notify_cb), NULL);
		g_signal_connect(backend_probe, "notify::prepared",
		                 G_CALLBACK(backend_probe_notify_cb), NULL);
	}
	return backend_state == 1;
}

/**************************************************************************
 * Images
 **************************************************************************/

static GdkTexture *
texture_from_pixbuf(GdkPixbuf *pixbuf)
{
	GdkTexture *texture;
	GBytes *pixels;

	if (gdk_pixbuf_get_bits_per_sample(pixbuf) != 8 ||
	    gdk_pixbuf_get_colorspace(pixbuf) != GDK_COLORSPACE_RGB)
		return NULL;
	pixels = gdk_pixbuf_read_pixel_bytes(pixbuf);
	texture = gdk_memory_texture_new(gdk_pixbuf_get_width(pixbuf),
	                                 gdk_pixbuf_get_height(pixbuf),
	                                 gdk_pixbuf_get_has_alpha(pixbuf) ?
	                                 GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
	                                 pixels, gdk_pixbuf_get_rowstride(pixbuf));
	g_bytes_unref(pixels);
	return texture;
}

/* A thumbnail of at most THUMB_SIZE, never upscaled (worker thread). */
static void
thumbnail_thread(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
	const char *path = task_data;
	GdkPixbuf *pixbuf, *oriented;
	GError *error = NULL;
	GStatBuf st;
	int w = 0, h = 0;

	if (g_stat(path, &st) != 0 || st.st_size > PIDGIN_ATTACHMENT_MAX_FILE_SIZE ||
	    gdk_pixbuf_get_file_info(path, &w, &h) == NULL) {
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		                        "not a (small enough) image");
		return;
	}
	if (w > PIDGIN_ATTACHMENT_THUMB_SIZE || h > PIDGIN_ATTACHMENT_THUMB_SIZE)
		pixbuf = gdk_pixbuf_new_from_file_at_scale(path, PIDGIN_ATTACHMENT_THUMB_SIZE,
		                                           PIDGIN_ATTACHMENT_THUMB_SIZE, TRUE, &error);
	else
		pixbuf = gdk_pixbuf_new_from_file(path, &error);
	if (pixbuf == NULL) {
		g_task_return_error(task, error);
		return;
	}
	oriented = gdk_pixbuf_apply_embedded_orientation(pixbuf);
	g_object_unref(pixbuf);
	g_task_return_pointer(task, texture_from_pixbuf(oriented), g_object_unref);
	g_object_unref(oriented);
}

static void
thumbnail_cb(GObject *source, GAsyncResult *res, gpointer data)
{
	GWeakRef *ref = data;
	GtkWidget *picture = g_weak_ref_get(ref);
	GError *error = NULL;
	GdkTexture *texture = g_task_propagate_pointer(G_TASK(res), &error);

	if (texture == NULL) {
		purple_debug_info("attachment", "no preview: %s\n", error ? error->message : "?");
		g_clear_error(&error);
		if (picture != NULL)
			gtk_widget_set_visible(picture, FALSE);
	} else if (picture != NULL) {
		gtk_picture_set_paintable(GTK_PICTURE(picture), GDK_PAINTABLE(texture));
		gtk_widget_set_size_request(picture, gdk_texture_get_width(texture),
		                            gdk_texture_get_height(texture));
	}
	g_clear_object(&texture);
	g_clear_object(&picture);
	g_weak_ref_clear(ref);
	g_free(ref);
}

static void
picture_clicked_cb(GtkGestureClick *gesture, int n_press, double x, double y,
                   PidginAttachment *att)
{
	pidgin_attachment_open(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)), att);
}

static GtkWidget *
image_widget(PidginAttachment *att)
{
	GtkWidget *picture = gtk_picture_new();
	GtkGesture *click = gtk_gesture_click_new();
	GWeakRef *ref = g_new0(GWeakRef, 1);
	GTask *task;

	gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);
	gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
	gtk_widget_set_halign(picture, GTK_ALIGN_START);
	gtk_widget_add_css_class(picture, "pidgin-attachment-image");
	gtk_widget_set_tooltip_text(picture, att->name);
	gtk_widget_set_cursor_from_name(picture, "pointer");
	g_signal_connect_data(click, "released", G_CALLBACK(picture_clicked_cb),
	                      g_object_ref(att), (GClosureNotify)g_object_unref, 0);
	gtk_widget_add_controller(picture, GTK_EVENT_CONTROLLER(click));

	g_weak_ref_init(ref, picture);
	task = g_task_new(NULL, NULL, thumbnail_cb, ref);
	g_task_set_task_data(task, g_strdup(att->path), g_free);
	g_task_run_in_thread(task, thumbnail_thread);
	g_object_unref(task);
	return picture;
}

/**************************************************************************
 * Media cards
 **************************************************************************/

static void
play_clicked_cb(GtkButton *button, PidginAttachment *att)
{
	pidgin_attachment_play(GTK_WIDGET(button), att);
}

static void
folder_clicked_cb(GtkButton *button, PidginAttachment *att)
{
	open_folder(GTK_WIDGET(button), att);
}

static gboolean
inline_playback(void)
{
	return !purple_prefs_exists(PIDGIN4_PREFS_ROOT "/media/inline_playback") ||
	       purple_prefs_get_bool(PIDGIN4_PREFS_ROOT "/media/inline_playback");
}

static GtkWidget *
media_card(PidginAttachment *att)
{
	gboolean video = att->kind == PIDGIN_ATTACHMENT_VIDEO;
	GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *icon, *label, *button;

	gtk_widget_add_css_class(card, "pidgin-media-card");
	gtk_widget_add_css_class(card, "card");
	gtk_widget_set_halign(card, GTK_ALIGN_START);

	/* the player, when GTK has a media backend */
	if (inline_playback() && pidgin_media_backend_available()) {
		GFile *file = att->path ? g_file_new_for_path(att->path) : g_file_new_for_uri(att->uri);
		GtkWidget *player = gtk_video_new_for_file(file);

		gtk_video_set_autoplay(GTK_VIDEO(player), FALSE);
		gtk_widget_set_size_request(player, PIDGIN_ATTACHMENT_VIDEO_WIDTH,
		                            video ? PIDGIN_ATTACHMENT_VIDEO_WIDTH * 9 / 16 : -1);
		gtk_widget_add_css_class(player, "pidgin-media-player");
		gtk_box_append(GTK_BOX(card), player);
		g_object_unref(file);
	}

	icon = gtk_image_new_from_icon_name(video ? "video-x-generic-symbolic"
	                                          : "audio-x-generic-symbolic");
	gtk_image_set_icon_size(GTK_IMAGE(icon), GTK_ICON_SIZE_LARGE);
	gtk_box_append(GTK_BOX(row), icon);

	label = gtk_label_new(att->name);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
	gtk_label_set_max_width_chars(GTK_LABEL(label), 40);
	gtk_widget_add_css_class(label, "pidgin-media-name");
	gtk_box_append(GTK_BOX(texts), label);
	if (att->size >= 0) {
		char *size = g_format_size(att->size);

		label = gtk_label_new(size);
		gtk_label_set_xalign(GTK_LABEL(label), 0.0);
		gtk_widget_add_css_class(label, "dim-label");
		gtk_widget_add_css_class(label, "pidgin-media-size");
		gtk_box_append(GTK_BOX(texts), label);
		g_free(size);
	}
	gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(row), texts);

	button = gtk_button_new_with_mnemonic(_("_Play"));
	gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
	gtk_widget_add_css_class(button, "pidgin-media-play");
	gtk_widget_set_tooltip_text(button, _("Play with the default application"));
	g_signal_connect_data(button, "clicked", G_CALLBACK(play_clicked_cb), g_object_ref(att),
	                      (GClosureNotify)g_object_unref, 0);
	gtk_box_append(GTK_BOX(row), button);
	if (att->path != NULL) {
		button = gtk_button_new_with_mnemonic(_("Open _Folder"));
		gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
		gtk_widget_add_css_class(button, "pidgin-media-folder");
		g_signal_connect_data(button, "clicked", G_CALLBACK(folder_clicked_cb),
		                      g_object_ref(att), (GClosureNotify)g_object_unref, 0);
		gtk_box_append(GTK_BOX(row), button);
	}
	gtk_box_append(GTK_BOX(card), row);
	return card;
}

GtkWidget *
pidgin_attachment_widget_new(PidginAttachment *att)
{
	GtkWidget *box;

	g_return_val_if_fail(PIDGIN_IS_ATTACHMENT(att), NULL);

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_add_css_class(box, "pidgin-attachment");
	if (att->kind == PIDGIN_ATTACHMENT_IMAGE && att->path != NULL)
		gtk_box_append(GTK_BOX(box), image_widget(att));
	else if (att->kind == PIDGIN_ATTACHMENT_AUDIO || att->kind == PIDGIN_ATTACHMENT_VIDEO)
		gtk_box_append(GTK_BOX(box), media_card(att));
	return box;
}
