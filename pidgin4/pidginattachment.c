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
#include "pidginimageloader.h"

struct _PidginAttachment
{
	GObject parent;

	PidginAttachmentKind kind;
	char *path;
	char *uri;
	char *name;
	goffset size;

	/* file shares from metadata (pidgin_attachment_new_for_share()) */
	gboolean share;
	char *media_type;
	int width, height;
	char *description;
	GdkTexture *thumbnail;
	GdkTexture *texture;            /* the full image, once loaded */
	char *hash;                     /* "algo:base64" */
	PidginAttachmentHashState hash_state;
	gboolean fetching;
};

enum {
	PROP_0,
	PROP_TEXTURE,
	PROP_HASH_STATE,
	N_PROPS
};

static GParamSpec *props[N_PROPS];

G_DEFINE_FINAL_TYPE(PidginAttachment, pidgin_attachment, G_TYPE_OBJECT)

static PidginAttachmentLaunchHook launch_hook = NULL;

static void
pidgin_attachment_finalize(GObject *obj)
{
	PidginAttachment *att = PIDGIN_ATTACHMENT(obj);

	g_free(att->path);
	g_free(att->uri);
	g_free(att->name);
	g_free(att->media_type);
	g_free(att->description);
	g_clear_object(&att->thumbnail);
	g_clear_object(&att->texture);
	g_free(att->hash);
	G_OBJECT_CLASS(pidgin_attachment_parent_class)->finalize(obj);
}

static void
pidgin_attachment_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec)
{
	PidginAttachment *att = PIDGIN_ATTACHMENT(obj);

	switch (prop_id) {
		case PROP_TEXTURE: g_value_set_object(value, att->texture); break;
		case PROP_HASH_STATE: g_value_set_int(value, att->hash_state); break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
	}
}

static void
pidgin_attachment_class_init(PidginAttachmentClass *klass)
{
	GObjectClass *obj_class = G_OBJECT_CLASS(klass);

	obj_class->finalize = pidgin_attachment_finalize;
	obj_class->get_property = pidgin_attachment_get_property;
	props[PROP_TEXTURE] = g_param_spec_object("texture", NULL, NULL, GDK_TYPE_TEXTURE,
		G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
	props[PROP_HASH_STATE] = g_param_spec_int("hash-state", NULL, NULL,
		PIDGIN_ATTACHMENT_HASH_NONE, PIDGIN_ATTACHMENT_HASH_UNCHECKED,
		PIDGIN_ATTACHMENT_HASH_NONE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
	g_object_class_install_properties(obj_class, N_PROPS, props);
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
 * File shares from metadata (XEP-0447)
 **************************************************************************/

/* A data: URI's image (the thumbnail), or NULL. */
static GdkTexture *
texture_from_data_uri(const char *uri)
{
	const char *comma;
	GdkTexture *texture;
	GBytes *bytes;
	GError *error = NULL;
	char *header;

	if (uri == NULL || g_ascii_strncasecmp(uri, "data:", 5) != 0 ||
	    (comma = strchr(uri, ',')) == NULL)
		return NULL;
	header = g_ascii_strdown(uri + 5, comma - uri - 5);
	if (strstr(header, ";base64") != NULL) {
		gsize len = 0;
		guchar *data = g_base64_decode(comma + 1, &len);

		bytes = g_bytes_new_take(data, len);
	} else {
		bytes = g_uri_unescape_bytes(comma + 1, -1, NULL, NULL);
	}
	g_free(header);
	if (bytes == NULL || g_bytes_get_size(bytes) == 0) {
		g_clear_pointer(&bytes, g_bytes_unref);
		return NULL;
	}
	texture = gdk_texture_new_from_bytes(bytes, &error);
	if (texture == NULL) {
		purple_debug_info("attachment", "the share's thumbnail: %s\n", error->message);
		g_clear_error(&error);
	}
	g_bytes_unref(bytes);
	return texture;
}

PidginAttachment *
pidgin_attachment_new_for_share(GHashTable *meta)
{
	const char *url, *name, *v;
	PidginAttachment *att;

	if (meta == NULL)
		return NULL;
	url = g_hash_table_lookup(meta, "sfs-url");
	name = g_hash_table_lookup(meta, "sfs-name");
	if ((url == NULL || *url == '\0') && (name == NULL || *name == '\0'))
		return NULL;

	att = g_object_new(PIDGIN_TYPE_ATTACHMENT, NULL);
	att->share = TRUE;
	att->uri = g_strdup(url && *url ? url : NULL);
	att->media_type = g_strdup(g_hash_table_lookup(meta, "sfs-media-type"));
	att->kind = pidgin_attachment_classify(name && *name ? name : url, att->media_type);
	if (name != NULL && *name != '\0') {
		att->name = g_strdup(name);
	} else {
		/* the URL's last path segment */
		PidginAttachment *tmp = pidgin_attachment_new_for_uri(url, att->kind, -1);

		att->name = g_strdup(pidgin_attachment_get_name(tmp));
		g_object_unref(tmp);
	}
	if ((v = g_hash_table_lookup(meta, "sfs-size")) != NULL)
		att->size = g_ascii_strtoll(v, NULL, 10);
	if (att->size < 0)
		att->size = -1;
	if ((v = g_hash_table_lookup(meta, "sfs-width")) != NULL)
		att->width = CLAMP(atoi(v), 0, 65535);
	if ((v = g_hash_table_lookup(meta, "sfs-height")) != NULL)
		att->height = CLAMP(atoi(v), 0, 65535);
	if ((v = g_hash_table_lookup(meta, "sfs-desc")) != NULL && *v != '\0')
		att->description = g_strdup(v);
	if ((v = g_hash_table_lookup(meta, "sfs-hash")) != NULL && strchr(v, ':') != NULL)
		att->hash = g_strdup(v);
	att->thumbnail = texture_from_data_uri(g_hash_table_lookup(meta, "sfs-thumbnail"));
	return att;
}

gboolean
pidgin_attachment_is_share(PidginAttachment *att)
{
	g_return_val_if_fail(PIDGIN_IS_ATTACHMENT(att), FALSE);
	return att->share;
}

const char *
pidgin_attachment_get_media_type(PidginAttachment *att)
{
	g_return_val_if_fail(PIDGIN_IS_ATTACHMENT(att), NULL);
	return att->media_type;
}

void
pidgin_attachment_get_dimensions(PidginAttachment *att, int *width, int *height)
{
	g_return_if_fail(PIDGIN_IS_ATTACHMENT(att));
	if (width != NULL)
		*width = att->width;
	if (height != NULL)
		*height = att->height;
}

const char *
pidgin_attachment_get_description(PidginAttachment *att)
{
	g_return_val_if_fail(PIDGIN_IS_ATTACHMENT(att), NULL);
	return att->description;
}

GdkTexture *
pidgin_attachment_get_thumbnail(PidginAttachment *att)
{
	g_return_val_if_fail(PIDGIN_IS_ATTACHMENT(att), NULL);
	return att->thumbnail;
}

GdkTexture *
pidgin_attachment_get_texture(PidginAttachment *att)
{
	g_return_val_if_fail(PIDGIN_IS_ATTACHMENT(att), NULL);
	return att->texture;
}

const char *
pidgin_attachment_get_hash(PidginAttachment *att)
{
	g_return_val_if_fail(PIDGIN_IS_ATTACHMENT(att), NULL);
	return att->hash;
}

PidginAttachmentHashState
pidgin_attachment_get_hash_state(PidginAttachment *att)
{
	g_return_val_if_fail(PIDGIN_IS_ATTACHMENT(att), PIDGIN_ATTACHMENT_HASH_NONE);
	return att->hash_state;
}

static void
set_hash_state(PidginAttachment *att, PidginAttachmentHashState state)
{
	if (att->hash_state == state)
		return;
	att->hash_state = state;
	g_object_notify_by_pspec(G_OBJECT(att), props[PROP_HASH_STATE]);
}

/* The hash's algorithm (the part before ':'), lowercase. */
static char *
hash_algo(const char *hash)
{
	const char *colon = hash ? strchr(hash, ':') : NULL;

	return colon ? g_ascii_strdown(hash, colon - hash) : NULL;
}

static gboolean
checksum_type(const char *algo, GChecksumType *type)
{
	if (purple_strequal(algo, "sha-256"))
		*type = G_CHECKSUM_SHA256;
	else if (purple_strequal(algo, "sha-512"))
		*type = G_CHECKSUM_SHA512;
	else if (purple_strequal(algo, "sha-384"))
		*type = G_CHECKSUM_SHA384;
	else if (purple_strequal(algo, "sha-1"))
		*type = G_CHECKSUM_SHA1;
	else
		return FALSE;
	return TRUE;
}

typedef struct {
	char *hash;
	GBytes *data;       /* or */
	char *path;         /* the image cache's copy */
} VerifyJob;

static void
verify_job_free(VerifyJob *job)
{
	g_free(job->hash);
	g_clear_pointer(&job->data, g_bytes_unref);
	g_free(job->path);
	g_free(job);
}

static void
verify_thread(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
	VerifyJob *job = task_data;
	char *algo = hash_algo(job->hash);
	GChecksumType type;
	PidginAttachmentHashState state = PIDGIN_ATTACHMENT_HASH_UNCHECKED;
	GBytes *data = job->data ? g_bytes_ref(job->data) : NULL;

	if (data == NULL && job->path != NULL) {
		GMappedFile *file = g_mapped_file_new(job->path, FALSE, NULL);

		if (file != NULL) {
			data = g_mapped_file_get_bytes(file);
			g_mapped_file_unref(file);
		}
	}
	if (data != NULL && checksum_type(algo, &type)) {
		gsize expected_len = 0, len = g_checksum_type_get_length(type);
		guchar *expected = g_base64_decode(strchr(job->hash, ':') + 1, &expected_len);
		guint8 *digest = g_malloc(len);
		GChecksum *sum = g_checksum_new(type);

		g_checksum_update(sum, g_bytes_get_data(data, NULL), g_bytes_get_size(data));
		g_checksum_get_digest(sum, digest, &len);
		state = (expected_len == len && memcmp(expected, digest, len) == 0)
			? PIDGIN_ATTACHMENT_HASH_VERIFIED : PIDGIN_ATTACHMENT_HASH_MISMATCH;
		g_checksum_free(sum);
		g_free(digest);
		g_free(expected);
	}
	g_clear_pointer(&data, g_bytes_unref);
	g_free(algo);
	g_task_return_int(task, state);
}

static void
verify_cb(GObject *source, GAsyncResult *res, gpointer data)
{
	PidginAttachment *att = PIDGIN_ATTACHMENT(source);
	PidginAttachmentHashState state = g_task_propagate_int(G_TASK(res), NULL);

	purple_debug_info("attachment", "%s: hash %s\n", att->name,
		state == PIDGIN_ATTACHMENT_HASH_VERIFIED ? "verified" :
		state == PIDGIN_ATTACHMENT_HASH_MISMATCH ? "MISMATCH" : "not checked");
	set_hash_state(att, state);
}

static void
verify_start(PidginAttachment *att, GBytes *data, const char *path)
{
	VerifyJob *job;
	GTask *task;

	if (att->hash == NULL || att->hash_state == PIDGIN_ATTACHMENT_HASH_PENDING)
		return;
	job = g_new0(VerifyJob, 1);
	job->hash = g_strdup(att->hash);
	job->data = data ? g_bytes_ref(data) : NULL;
	job->path = g_strdup(path);
	set_hash_state(att, PIDGIN_ATTACHMENT_HASH_PENDING);
	task = g_task_new(att, NULL, verify_cb, NULL);
	g_task_set_task_data(task, job, (GDestroyNotify)verify_job_free);
	g_task_run_in_thread(task, verify_thread);
	g_object_unref(task);
}

void
pidgin_attachment_verify_bytes(PidginAttachment *att, GBytes *data)
{
	g_return_if_fail(PIDGIN_IS_ATTACHMENT(att));
	g_return_if_fail(data != NULL);
	verify_start(att, data, NULL);
}

static void
share_loaded_cb(GObject *source, GAsyncResult *res, gpointer data)
{
	PidginAttachment *att = data;
	GError *error = NULL;
	GdkTexture *texture = pidgin_image_loader_load_finish(PIDGIN_IMAGE_LOADER(source), res,
	                                                      &error);

	att->fetching = FALSE;
	if (texture == NULL) {
		purple_debug_info("attachment", "loading %s: %s\n", att->uri,
		                  error ? error->message : "?");
		g_clear_error(&error);
		g_object_unref(att);
		return;
	}
	g_set_object(&att->texture, texture);
	g_object_notify_by_pspec(G_OBJECT(att), props[PROP_TEXTURE]);
	g_object_unref(texture);
	/* The loader keeps the downloaded bytes in its disk cache: check
	 * those (not the decoded pixels). */
	if (att->hash != NULL && att->hash_state == PIDGIN_ATTACHMENT_HASH_NONE) {
		char *path = pidgin_image_loader_cache_path(PIDGIN_IMAGE_LOADER(source), att->uri);

		if (path != NULL && g_file_test(path, G_FILE_TEST_IS_REGULAR))
			verify_start(att, NULL, path);
		else
			set_hash_state(att, PIDGIN_ATTACHMENT_HASH_UNCHECKED);
		g_free(path);
	}
	g_object_unref(att);
}

/* Loads a shared image once, when the loader allows its URI. */
static void
share_fetch(PidginAttachment *att)
{
	PidginImageLoader *loader;

	if (att->fetching || att->texture != NULL || att->kind != PIDGIN_ATTACHMENT_IMAGE ||
	    att->uri == NULL || (loader = pidgin_image_loader_get_default()) == NULL ||
	    !pidgin_image_loader_is_allowed(loader, att->uri))
		return;
	att->fetching = TRUE;
	pidgin_image_loader_load_async(loader, att->uri, NULL, share_loaded_cb, g_object_ref(att));
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

void
pidgin_media_backend_disable_for_tests(void)
{
	backend_state = 0;
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

/*
 * Inline playback. The card starts with a "Play Here" poster and the
 * player is made only when it is pressed: a conversation full of clips
 * doesn't run a GStreamer pipeline (a dozen threads and a GL context) for
 * each one.
 *
 * The player never reads from the network. GTK 4.22's GStreamer backend
 * tears a player down on the main thread, joining its streaming threads;
 * a thread stuck in a read blocks that forever. A URL read through gvfs
 * (gvfsd-http) that stalls does just that, cancel or not, and froze the
 * whole UI when a list row holding such a player was recycled. So a
 * remote file is fetched first by the image loader (libsoup: timeouts,
 * cancellable, its allowlist; the press allows this one URI) and played
 * from memory, and a local file from a stream opened here (the backend
 * also asserts when it can't g_file_read() a GtkMediaFile's GFile). If
 * loading fails, or the backend reports an error (not media, no codec),
 * a short note takes the player's place and the card stays.
 */
#define POSTER_CANCEL_KEY "pidgin-media-poster-cancel"

static void
weak_ref_free(gpointer data)
{
	g_weak_ref_clear(data);
	g_free(data);
}

typedef struct
{
	GWeakRef widget;
	char *message;
} PlayError;

static void
play_error_free(gpointer data)
{
	PlayError *pe = data;

	g_weak_ref_clear(&pe->widget);
	g_free(pe->message);
	g_free(pe);
}

static gboolean
play_error_idle(gpointer data)
{
	PlayError *pe = data;
	GtkWidget *widget = g_weak_ref_get(&pe->widget);
	GtkWidget *card = widget ? gtk_widget_get_parent(widget) : NULL;

	if (GTK_IS_BOX(card)) {
		GtkWidget *label = gtk_label_new(_("This can't be played here."));

		gtk_label_set_xalign(GTK_LABEL(label), 0.0);
		gtk_widget_add_css_class(label, "dim-label");
		gtk_widget_add_css_class(label, "pidgin-media-error");
		gtk_widget_set_tooltip_text(label, pe->message);
		gtk_box_insert_child_after(GTK_BOX(card), label, widget);
		gtk_box_remove(GTK_BOX(card), widget);
	}
	g_clear_object(&widget);
	return G_SOURCE_REMOVE;
}

/* The poster or player gives way to a note, after the current emission
 * (not from inside GtkVideo's own calls). */
static void
show_play_error(GtkWidget *widget, const char *message)
{
	PlayError *pe = g_new0(PlayError, 1);

	purple_debug_info("attachment", "inline playback failed (%s): the card only\n", message);
	g_weak_ref_init(&pe->widget, widget);
	pe->message = g_strdup(message);
	g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, play_error_idle, pe, play_error_free);
}

static void
player_stream_error_cb(GtkMediaStream *stream, GParamSpec *pspec, gpointer data)
{
	const GError *error = gtk_media_stream_get_error(stream);
	GtkWidget *player;

	if (error == NULL || (player = g_weak_ref_get(data)) == NULL)
		return;
	show_play_error(player, error->message);
	g_object_unref(player);
}

static void
player_error_ref_free(gpointer data, GClosure *closure)
{
	weak_ref_free(data);
}

/* The player for @in, in @poster's place, playing. */
static void
poster_play(GtkWidget *poster, GInputStream *in)
{
	GtkWidget *card = gtk_widget_get_parent(poster);
	GtkWidget *player;
	GtkMediaStream *stream;
	GWeakRef *error_ref;
	int w, h;

	if (!GTK_IS_BOX(card))
		return;
	player = gtk_video_new();
	gtk_widget_get_size_request(poster, &w, &h);
	gtk_widget_set_size_request(player, w, h);
	gtk_widget_add_css_class(player, "pidgin-media-player");
	gtk_video_set_autoplay(GTK_VIDEO(player), TRUE);
	gtk_box_insert_child_after(GTK_BOX(card), player, poster);
	gtk_box_remove(GTK_BOX(card), poster);

	stream = gtk_media_file_new_for_input_stream(in);
	error_ref = g_new0(GWeakRef, 1);
	g_weak_ref_init(error_ref, player);
	g_signal_connect_data(stream, "notify::error", G_CALLBACK(player_stream_error_cb),
	                      error_ref, player_error_ref_free, 0);
	gtk_video_set_media_stream(GTK_VIDEO(player), stream);
	/* an error right away (no notify then) */
	if (gtk_media_stream_get_error(stream) != NULL)
		player_stream_error_cb(stream, NULL, error_ref);
	g_object_unref(stream);
}

/* The file is open (@in) or failed (@error); @poster may be gone. */
static void
poster_loaded(GtkWidget *poster, GInputStream *in, const GError *error)
{
	if (poster == NULL)
		return;
	g_object_set_data(G_OBJECT(poster), POSTER_CANCEL_KEY, NULL);
	if (in != NULL)
		poster_play(poster, in);
	else if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		show_play_error(poster, error->message);
}

static void
poster_read_cb(GObject *source, GAsyncResult *res, gpointer data)
{
	GError *error = NULL;
	GFileInputStream *in = g_file_read_finish(G_FILE(source), res, &error);
	GtkWidget *poster = g_weak_ref_get(data);

	poster_loaded(poster, G_INPUT_STREAM(in), error);
	g_clear_object(&in);
	g_clear_error(&error);
	g_clear_object(&poster);
	weak_ref_free(data);
}

static void
poster_fetch_cb(GObject *source, GAsyncResult *res, gpointer data)
{
	GError *error = NULL;
	GBytes *bytes = pidgin_image_loader_fetch_finish(PIDGIN_IMAGE_LOADER(source), res, &error);
	GInputStream *in = bytes ? g_memory_input_stream_new_from_bytes(bytes) : NULL;
	GtkWidget *poster = g_weak_ref_get(data);

	poster_loaded(poster, in, error);
	g_clear_object(&in);
	g_clear_pointer(&bytes, g_bytes_unref);
	g_clear_error(&error);
	g_clear_object(&poster);
	weak_ref_free(data);
}

static void
cancel_and_unref(gpointer data)
{
	g_cancellable_cancel(data);
	g_object_unref(data);
}

static void
poster_clicked_cb(GtkButton *button, PidginAttachment *att)
{
	GCancellable *cancellable;
	GtkWidget *spinner;
	GWeakRef *ref;

	if (g_object_get_data(G_OBJECT(button), POSTER_CANCEL_KEY) != NULL)
		return;     /* loading */
	cancellable = g_cancellable_new();
	/* cancelled if the poster goes before the file is open */
	g_object_set_data_full(G_OBJECT(button), POSTER_CANCEL_KEY, g_object_ref(cancellable),
	                       cancel_and_unref);
	spinner = gtk_spinner_new();
	gtk_spinner_start(GTK_SPINNER(spinner));
	gtk_button_set_child(button, spinner);
	gtk_widget_set_tooltip_text(GTK_WIDGET(button), _("Loading\xe2\x80\xa6"));

	ref = g_new0(GWeakRef, 1);
	g_weak_ref_init(ref, button);
	if (att->path != NULL) {
		GFile *file = g_file_new_for_path(att->path);

		g_file_read_async(file, G_PRIORITY_DEFAULT, cancellable, poster_read_cb, ref);
		g_object_unref(file);
	} else {
		PidginImageLoader *loader = pidgin_image_loader_get_default();

		/* asked for: this URI, whatever its host */
		if (!pidgin_image_loader_is_allowed(loader, att->uri))
			pidgin_image_loader_allow_uri(loader, att->uri);
		pidgin_image_loader_fetch_async(loader, att->uri, PIDGIN_ATTACHMENT_MAX_FILE_SIZE,
		                                cancellable, poster_fetch_cb, ref);
	}
	g_object_unref(cancellable);
}

/* "Play Here", the player's size for a video. */
static GtkWidget *
media_poster_new(PidginAttachment *att)
{
	gboolean video = att->kind == PIDGIN_ATTACHMENT_VIDEO;
	GtkWidget *poster = gtk_button_new();
	GtkWidget *icon = gtk_image_new_from_icon_name("media-playback-start-symbolic");

	if (video) {
		gtk_image_set_pixel_size(GTK_IMAGE(icon), 48);
		gtk_button_set_child(GTK_BUTTON(poster), icon);
		gtk_widget_set_tooltip_text(poster, _("Play Here"));
	} else {
		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

		gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
		gtk_box_append(GTK_BOX(box), icon);
		gtk_box_append(GTK_BOX(box), gtk_label_new(_("Play Here")));
		gtk_button_set_child(GTK_BUTTON(poster), box);
	}
	gtk_widget_set_size_request(poster, PIDGIN_ATTACHMENT_VIDEO_WIDTH,
	                            video ? PIDGIN_ATTACHMENT_VIDEO_WIDTH * 9 / 16 : -1);
	gtk_widget_add_css_class(poster, "pidgin-media-poster");
	g_signal_connect_data(poster, "clicked", G_CALLBACK(poster_clicked_cb), g_object_ref(att),
	                      (GClosureNotify)g_object_unref, 0);
	return poster;
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

	/* the player, on demand, when GTK has a media backend; not for a
	 * remote file known to be too large to fetch */
	if (inline_playback() && pidgin_media_backend_available() &&
	    (att->path != NULL || att->size <= PIDGIN_ATTACHMENT_MAX_FILE_SIZE))
		gtk_box_append(GTK_BOX(card), media_poster_new(att));

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

/**************************************************************************
 * File share cards
 **************************************************************************/

/* The size to reserve for a shared image: the sender's dimensions (or the
 * thumbnail's) scaled to fit THUMB_SIZE. */
static void
share_image_size(PidginAttachment *att, int *w, int *h)
{
	GdkTexture *t = att->texture ? att->texture : att->thumbnail;
	double scale;

	*w = att->width;
	*h = att->height;
	if ((*w <= 0 || *h <= 0) && t != NULL) {
		*w = gdk_texture_get_width(t);
		*h = gdk_texture_get_height(t);
		/* a thumbnail stands for a larger image: keep its shape */
		if (t == att->thumbnail && MAX(*w, *h) < PIDGIN_ATTACHMENT_THUMB_SIZE / 2) {
			scale = (double)(PIDGIN_ATTACHMENT_THUMB_SIZE / 2) / MAX(*w, *h);
			*w = *w * scale;
			*h = *h * scale;
		}
	}
	if (*w <= 0 || *h <= 0) {
		*w = *h = -1;
		return;
	}
	scale = MIN(1.0, (double)PIDGIN_ATTACHMENT_THUMB_SIZE / MAX(*w, *h));
	*w = MAX(1, (int)(*w * scale));
	*h = MAX(1, (int)(*h * scale));
}

static void
share_texture_cb(PidginAttachment *att, GParamSpec *pspec, GtkWidget *picture)
{
	int w, h;

	if (att->texture == NULL)
		return;
	gtk_picture_set_paintable(GTK_PICTURE(picture), GDK_PAINTABLE(att->texture));
	gtk_widget_remove_css_class(picture, "pidgin-share-thumbnail");
	share_image_size(att, &w, &h);
	gtk_widget_set_size_request(picture, w, h);
}

static GtkWidget *
share_image_widget(PidginAttachment *att)
{
	GtkWidget *picture = gtk_picture_new();
	GtkGesture *click = gtk_gesture_click_new();
	int w, h;

	gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);
	gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
	gtk_widget_set_halign(picture, GTK_ALIGN_START);
	gtk_widget_add_css_class(picture, "pidgin-attachment-image");
	gtk_widget_add_css_class(picture, "pidgin-share-image");
	gtk_widget_set_tooltip_text(picture, att->name);
	gtk_widget_set_cursor_from_name(picture, "pointer");
	g_signal_connect_data(click, "released", G_CALLBACK(picture_clicked_cb),
	                      g_object_ref(att), (GClosureNotify)g_object_unref, 0);
	gtk_widget_add_controller(picture, GTK_EVENT_CONTROLLER(click));

	if (att->texture != NULL) {
		gtk_picture_set_paintable(GTK_PICTURE(picture), GDK_PAINTABLE(att->texture));
	} else if (att->thumbnail != NULL) {
		gtk_picture_set_paintable(GTK_PICTURE(picture), GDK_PAINTABLE(att->thumbnail));
		gtk_widget_add_css_class(picture, "pidgin-share-thumbnail");
	}
	/* the space the image will take, before it loads */
	share_image_size(att, &w, &h);
	gtk_widget_set_size_request(picture, w, h);
	gtk_widget_set_visible(picture, att->texture != NULL || att->thumbnail != NULL || w > 0);

	g_signal_connect_object(att, "notify::texture", G_CALLBACK(share_texture_cb), picture, 0);
	share_fetch(att);
	return picture;
}

static void
share_hash_cb(PidginAttachment *att, GParamSpec *pspec, GtkWidget *label)
{
	char *algo = hash_algo(att->hash);
	char *tip = NULL;

	gtk_widget_remove_css_class(label, "error");
	gtk_widget_remove_css_class(label, "success");
	switch (att->hash_state) {
		case PIDGIN_ATTACHMENT_HASH_VERIFIED:
			gtk_label_set_text(GTK_LABEL(label), _("\xe2\x9c\x93 verified"));
			tip = g_strdup_printf(_("The downloaded file matches the %s hash the sender "
			                        "gave."), algo);
			gtk_widget_add_css_class(label, "success");
			break;
		case PIDGIN_ATTACHMENT_HASH_MISMATCH:
			gtk_label_set_text(GTK_LABEL(label), _("\xe2\x9a\xa0 hash mismatch"));
			tip = g_strdup_printf(_("The downloaded file does not match the %s hash the "
			                        "sender gave: it is not the file they shared, or it "
			                        "was changed on the server."), algo);
			gtk_widget_add_css_class(label, "error");
			break;
		case PIDGIN_ATTACHMENT_HASH_UNCHECKED:
			gtk_label_set_text(GTK_LABEL(label), _("not verified"));
			tip = g_strdup_printf(_("The %s hash the sender gave could not be checked."),
			                      algo);
			break;
		default:
			break;
	}
	gtk_widget_set_tooltip_text(label, tip);
	gtk_widget_set_visible(label, tip != NULL);
	g_free(tip);
	g_free(algo);
}

static void
open_clicked_cb(GtkButton *button, PidginAttachment *att)
{
	pidgin_attachment_open(GTK_WIDGET(button), att);
}

/* The name, "size · type", the hash state and (for other files) Open. */
static GtkWidget *
share_info_row(PidginAttachment *att, gboolean with_name, gboolean with_open)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *label, *button;
	GString *details = g_string_new(NULL);

	if (with_open) {
		GIcon *icon = NULL;
		char *type = att->media_type ? g_content_type_from_mime_type(att->media_type) : NULL;

		if (type == NULL)
			type = g_content_type_guess(att->name, NULL, 0, NULL);
		if (type != NULL)
			icon = g_content_type_get_symbolic_icon(type);
		label = icon ? gtk_image_new_from_gicon(icon) :
		               gtk_image_new_from_icon_name("text-x-generic-symbolic");
		gtk_image_set_icon_size(GTK_IMAGE(label), GTK_ICON_SIZE_LARGE);
		gtk_box_append(GTK_BOX(row), label);
		g_clear_object(&icon);
		g_free(type);
	}
	if (with_name) {
		label = gtk_label_new(att->name);
		gtk_label_set_xalign(GTK_LABEL(label), 0.0);
		gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
		gtk_label_set_max_width_chars(GTK_LABEL(label), 40);
		gtk_widget_add_css_class(label, "pidgin-share-name");
		gtk_box_append(GTK_BOX(texts), label);
	}
	if (with_name && att->size >= 0) {
		char *size = g_format_size(att->size);

		g_string_append(details, size);
		g_free(size);
	}
	if (with_name && att->media_type != NULL)
		g_string_append_printf(details, "%s%s", details->len ? " \xc2\xb7 " : "",
		                       att->media_type);
	if (details->len > 0) {
		label = gtk_label_new(details->str);
		gtk_label_set_xalign(GTK_LABEL(label), 0.0);
		gtk_widget_add_css_class(label, "dim-label");
		gtk_widget_add_css_class(label, "pidgin-share-size");
		gtk_box_append(GTK_BOX(texts), label);
	}
	g_string_free(details, TRUE);
	gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(row), texts);

	label = gtk_label_new(NULL);
	gtk_widget_add_css_class(label, "pidgin-share-hash");
	gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(row), label);
	g_signal_connect_object(att, "notify::hash-state", G_CALLBACK(share_hash_cb), label, 0);
	share_hash_cb(att, NULL, label);

	if (with_open && att->uri != NULL) {
		button = gtk_button_new_with_mnemonic(_("_Open"));
		gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
		gtk_widget_add_css_class(button, "pidgin-share-open");
		gtk_widget_set_tooltip_text(button, att->uri);
		g_signal_connect_data(button, "clicked", G_CALLBACK(open_clicked_cb),
		                      g_object_ref(att), (GClosureNotify)g_object_unref, 0);
		gtk_box_append(GTK_BOX(row), button);
	}
	return row;
}

static GtkWidget *
share_widget(PidginAttachment *att)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

	gtk_widget_add_css_class(box, "pidgin-attachment");
	gtk_widget_add_css_class(box, "pidgin-share");
	gtk_widget_set_halign(box, GTK_ALIGN_START);
	switch (att->kind) {
		case PIDGIN_ATTACHMENT_IMAGE:
			gtk_box_append(GTK_BOX(box), share_image_widget(att));
			gtk_box_append(GTK_BOX(box), share_info_row(att, TRUE, FALSE));
			break;
		case PIDGIN_ATTACHMENT_AUDIO:
		case PIDGIN_ATTACHMENT_VIDEO:
			if (att->uri != NULL)
				gtk_box_append(GTK_BOX(box), media_card(att));
			gtk_box_append(GTK_BOX(box), share_info_row(att, att->uri == NULL, FALSE));
			break;
		default: {
			GtkWidget *card = share_info_row(att, TRUE, TRUE);

			gtk_widget_add_css_class(card, "card");
			gtk_widget_add_css_class(card, "pidgin-file-card");
			gtk_box_append(GTK_BOX(box), card);
			break;
		}
	}
	if (att->description != NULL) {
		GtkWidget *caption = gtk_label_new(att->description);

		gtk_label_set_xalign(GTK_LABEL(caption), 0.0);
		gtk_label_set_wrap(GTK_LABEL(caption), TRUE);
		gtk_label_set_selectable(GTK_LABEL(caption), TRUE);
		gtk_label_set_max_width_chars(GTK_LABEL(caption), 60);
		gtk_widget_add_css_class(caption, "pidgin-share-desc");
		gtk_box_append(GTK_BOX(box), caption);
	}
	return box;
}

GtkWidget *
pidgin_attachment_widget_new(PidginAttachment *att)
{
	GtkWidget *box;

	g_return_val_if_fail(PIDGIN_IS_ATTACHMENT(att), NULL);

	if (att->share)
		return share_widget(att);

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_add_css_class(box, "pidgin-attachment");
	if (att->kind == PIDGIN_ATTACHMENT_IMAGE && att->path != NULL)
		gtk_box_append(GTK_BOX(box), image_widget(att));
	else if (att->kind == PIDGIN_ATTACHMENT_AUDIO || att->kind == PIDGIN_ATTACHMENT_VIDEO)
		gtk_box_append(GTK_BOX(box), media_card(att));
	return box;
}
