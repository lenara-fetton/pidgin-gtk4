/**
 * @file pidginnickcolor.h Chat nick colours
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
#ifndef _PIDGINNICKCOLOR_H_
#define _PIDGINNICKCOLOR_H_

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum
{
	/** Pidgin 2's palette (pidgin/gtkconv.c generate_nick_colors()). */
	PIDGIN_NICK_COLOR_PIDGIN,
	/** XEP-0392 Consistent Color Generation (HSLuv), as Dino/Conversations. */
	PIDGIN_NICK_COLOR_XEP0392
} PidginNickColorScheme;

/**
 * Pidgin 2's nick colours: the Tango seed colours that contrast with
 * @background, the default highlight and the send colour (W3C AERT
 * brightness/colour difference), topped up with random colours from a
 * generator seeded by the background, up to @count. Returns the number of
 * colours written to @colors.
 */
guint pidgin_nick_colors_generate(const GdkRGBA *background, GdkRGBA *colors,
                                  guint count);

/**
 * The colour for @name on @background with @scheme. PIDGIN picks from the
 * generated palette by g_str_hash(); XEP0392 hashes the name (normalise
 * JIDs first) and uses lightness 50 on light backgrounds, 70 on dark ones.
 */
void pidgin_nick_color_get(PidginNickColorScheme scheme, const char *name,
                           const GdkRGBA *background, GdkRGBA *color);

/** XEP-0392 section 5.1: the hue angle (0..360) of @name (UTF-8). */
double pidgin_nick_color_xep0392_hue(const char *name);

/**
 * HSLuv (hue 0..360, saturation and lightness 0..100) to sRGB (0..1),
 * the reference hsluvToRgb.
 */
void pidgin_hsluv_to_rgb(double h, double s, double l,
                         double *r, double *g, double *b);

/** W3C AERT visibility test of @fg on @bg (colours 0..1). */
gboolean pidgin_color_is_visible(const GdkRGBA *fg, const GdkRGBA *bg,
                                 guint color_contrast, guint brightness_contrast);

G_END_DECLS

#endif /* _PIDGINNICKCOLOR_H_ */
