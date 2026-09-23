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

#include <gdk-pixbuf/gdk-pixbuf.h>

#include "debug.h"
#include "prefs.h"
#include "util.h"

#include "pidginimageencode.h"

gboolean
pidgin_texture_has_alpha(GdkTexture *texture)
{
	GdkTextureDownloader *downloader;
	GBytes *bytes;
	const guint8 *data;
	gsize stride, x, y;
	int width, height;
	gboolean alpha = FALSE;

	g_return_val_if_fail(GDK_IS_TEXTURE(texture), FALSE);

	width = gdk_texture_get_width(texture);
	height = gdk_texture_get_height(texture);
	downloader = gdk_texture_downloader_new(texture);
	gdk_texture_downloader_set_format(downloader, GDK_MEMORY_R8G8B8A8);
	bytes = gdk_texture_downloader_download_bytes(downloader, &stride);
	gdk_texture_downloader_free(downloader);
	data = g_bytes_get_data(bytes, NULL);
	for (y = 0; y < (gsize)height && !alpha; y++)
		for (x = 0; x < (gsize)width; x++)
			if (data[y * stride + x * 4 + 3] != 0xff) {
				alpha = TRUE;
				break;
			}
	g_bytes_unref(bytes);
	return alpha;
}

static GBytes *
encode_jpeg(GdkTexture *texture, int quality)
{
	GdkTextureDownloader *downloader = gdk_texture_downloader_new(texture);
	GdkPixbuf *pixbuf;
	GBytes *rgb;
	gsize stride;
	char *buffer = NULL, *q;
	gsize len = 0;
	GError *error = NULL;
	gboolean ok;

	gdk_texture_downloader_set_format(downloader, GDK_MEMORY_R8G8B8);
	rgb = gdk_texture_downloader_download_bytes(downloader, &stride);
	gdk_texture_downloader_free(downloader);
	pixbuf = gdk_pixbuf_new_from_bytes(rgb, GDK_COLORSPACE_RGB, FALSE, 8,
	                                   gdk_texture_get_width(texture),
	                                   gdk_texture_get_height(texture), stride);
	g_bytes_unref(rgb);
	q = g_strdup_printf("%d", CLAMP(quality, 50, 100));
	ok = gdk_pixbuf_save_to_buffer(pixbuf, &buffer, &len, "jpeg", &error,
	                               "quality", q, NULL);
	g_free(q);
	g_object_unref(pixbuf);
	if (!ok) {
		purple_debug_warning("imageencode", "JPEG encoding failed: %s\n",
		                     error ? error->message : "?");
		g_clear_error(&error);
		return NULL;
	}
	return g_bytes_new_take(buffer, len);
}

GBytes *
pidgin_image_encode(GdkTexture *texture, const char *format, int quality,
                    const char **extension)
{
	GBytes *png = NULL, *jpeg = NULL;
	gint64 pixels;

	g_return_val_if_fail(GDK_IS_TEXTURE(texture), NULL);

	pixels = (gint64)gdk_texture_get_width(texture) * gdk_texture_get_height(texture);
	if (purple_strequal(format, "png")) {
		/* PNG */
	} else if (purple_strequal(format, "jpeg")) {
		jpeg = encode_jpeg(texture, quality);
	} else if (!pidgin_texture_has_alpha(texture)) {
		/* auto: the smaller of the two, for an opaque image */
		jpeg = encode_jpeg(texture, quality);
		if (jpeg != NULL && pixels <= PIDGIN_IMAGE_ENCODE_COMPARE_MAX) {
			png = gdk_texture_save_to_png_bytes(texture);
			if (g_bytes_get_size(png) <= g_bytes_get_size(jpeg))
				g_clear_pointer(&jpeg, g_bytes_unref);
		}
	}

	if (jpeg != NULL) {
		g_clear_pointer(&png, g_bytes_unref);
		if (extension != NULL)
			*extension = "jpg";
		return jpeg;
	}
	if (extension != NULL)
		*extension = "png";
	return png ? png : gdk_texture_save_to_png_bytes(texture);
}

GBytes *
pidgin_image_encode_for_paste(GdkTexture *texture, const char **extension)
{
	const char *format = "auto";
	int quality = PIDGIN_IMAGE_ENCODE_DEFAULT_QUALITY;

	if (purple_prefs_exists(PIDGIN4_PREFS_ROOT "/images/paste_format"))
		format = purple_prefs_get_string(PIDGIN4_PREFS_ROOT "/images/paste_format");
	if (purple_prefs_exists(PIDGIN4_PREFS_ROOT "/images/paste_jpeg_quality"))
		quality = purple_prefs_get_int(PIDGIN4_PREFS_ROOT "/images/paste_jpeg_quality");
	return pidgin_image_encode(texture, format, quality, extension);
}
