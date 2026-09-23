/**
 * @file pidginanimation.h Animated images as GdkPaintables
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
#ifndef _PIDGINANIMATION_H_
#define _PIDGINANIMATION_H_

#include <gtk/gtk.h>

#include "imgstore.h"

G_BEGIN_DECLS

/*
 * PidginAnimation is a GdkPaintable that plays a GdkPixbufAnimation (GIF,
 * animated PNG/WebP where the loaders support it): smileys and inline
 * images. It only advances while something draws it, so animations
 * scrolled out of view cost nothing.
 */
#define PIDGIN_TYPE_ANIMATION (pidgin_animation_get_type())
G_DECLARE_FINAL_TYPE(PidginAnimation, pidgin_animation, PIDGIN, ANIMATION, GObject)

/**
 * Decodes image data. Returns a PidginAnimation for animated images and a
 * GdkTexture for still ones, or NULL if the data can't be decoded.
 */
GdkPaintable *pidgin_paintable_new_from_data(gconstpointer data, gsize len);

/** Same as pidgin_paintable_new_from_data() for a file. */
GdkPaintable *pidgin_paintable_new_from_file(const char *filename);

/** Same as pidgin_paintable_new_from_data() for a stored image. */
GdkPaintable *pidgin_paintable_new_from_imgstore(PurpleStoredImage *image);

/** Converts a GdkPixbuf to a GdkMemoryTexture (no deprecated API). */
GdkTexture *pidgin_texture_new_from_pixbuf(GdkPixbuf *pixbuf);

/** Pauses or resumes the animation. */
void pidgin_animation_set_playing(PidginAnimation *animation, gboolean playing);

G_END_DECLS

#endif /* _PIDGINANIMATION_H_ */
