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

/*
 * Encoding pasted images (and other raw image data) for sending, by the
 * prefs /pidgin4/images/paste_format ("auto", "png", "jpeg") and
 * /pidgin4/images/paste_jpeg_quality (50..100, default 85).
 *
 * "auto" chooses JPEG only for an image without transparency whose JPEG
 * is smaller than its PNG (photos), and PNG otherwise (screenshots of UI,
 * flat colour, anything with alpha). Both are encoded to compare, up to
 * PIDGIN_IMAGE_ENCODE_COMPARE_MAX pixels; a larger opaque image is taken
 * to be a photo and goes straight to JPEG. JPEG is written with
 * gdk-pixbuf; if that fails, PNG is used.
 */
#ifndef _PIDGINIMAGEENCODE_H_
#define _PIDGINIMAGEENCODE_H_

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define PIDGIN_IMAGE_ENCODE_COMPARE_MAX (16 * 1000 * 1000)
#define PIDGIN_IMAGE_ENCODE_DEFAULT_QUALITY 85

/** Whether any pixel of @texture is not fully opaque. */
gboolean pidgin_texture_has_alpha(GdkTexture *texture);

/**
 * Encodes @texture by @format ("auto", "png" or "jpeg"; anything else is
 * "auto") with JPEG @quality (clamped to 50..100). Sets @extension to
 * "png" or "jpg" (static). Returns the encoded bytes.
 */
GBytes *pidgin_image_encode(GdkTexture *texture, const char *format, int quality,
                            const char **extension);

/** As pidgin_image_encode() with the /pidgin4/images/paste_* prefs. */
GBytes *pidgin_image_encode_for_paste(GdkTexture *texture, const char **extension);

G_END_DECLS

#endif /* _PIDGINIMAGEENCODE_H_ */
