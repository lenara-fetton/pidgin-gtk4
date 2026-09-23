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

#include <math.h>

#include "pidginnickcolor.h"

/* pidgin/gtkconv.c */
#define MIN_BRIGHTNESS_CONTRAST 75
#define MIN_COLOR_CONTRAST 200
#define DEFAULT_SEND_COLOR "#204a87"
#define DEFAULT_HIGHLIGHT_COLOR "#AF7F00"
#define NUM_NICK_COLORS 220

/* pidgin/gtknickcolors.h, as 16-bit values */
static const guint16 nick_seed_colors[][3] = {
	{64764, 59881, 20303},  /* Butter #1 */
	{60909, 54484, 0},      /* Butter #2 */
	{50372, 41120, 0},      /* Butter #3 */
	{64764, 44975, 15934},  /* Orange #1 */
	{62965, 31097, 0},      /* Orange #2 */
	{52942, 23644, 0},      /* Orange #3 */
	{59811, 47545, 28270},  /* Chocolate #1 */
	{49601, 32125, 4369},   /* Chocolate #2 */
	{36751, 22873, 514},    /* Chocolate #3 */
	{35466, 58082, 13364},  /* Chameleon #1 */
	{29555, 53970, 5654},   /* Chameleon #2 */
	{20046, 39578, 1542},   /* Chameleon #3 */
	{29289, 40863, 53199},  /* Sky Blue #1 */
	{13364, 25957, 42148},  /* Sky Blue #2 */
	{8224, 19018, 34695},   /* Sky Blue #3 */
	{44461, 32639, 43167},  /* Plum #1 */
	{30069, 20560, 31611},  /* Plum #2 */
	{23644, 13621, 26214},  /* Plum #3 */
	{61423, 10537, 10537},  /* Scarlet Red #1 */
	{52428, 0, 0},          /* Scarlet Red #2 */
	{42148, 0, 0},          /* Scarlet Red #3 */
	{34952, 35466, 34181},  /* Aluminium #4 */
	{21845, 22359, 21331},  /* Aluminium #5 */
	{11822, 13364, 13878}   /* Aluminium #6 */
};

gboolean
pidgin_color_is_visible(const GdkRGBA *fg, const GdkRGBA *bg,
                        guint color_contrast, guint brightness_contrast)
{
	int fr = fg->red * 255, fgr = fg->green * 255, fb = fg->blue * 255;
	int br = bg->red * 255, bgr = bg->green * 255, bb = bg->blue * 255;
	int fg_brightness = (fr * 299 + fgr * 587 + fb * 114) / 1000;
	int bg_brightness = (br * 299 + bgr * 587 + bb * 114) / 1000;
	guint br_diff = ABS(fg_brightness - bg_brightness);
	guint col_diff = ABS(fr - br) + ABS(fgr - bgr) + ABS(fb - bb);

	return col_diff > color_contrast && br_diff > brightness_contrast;
}

static gboolean
nick_color_ok(const GdkRGBA *c, const GdkRGBA *bg, const GdkRGBA *highlight,
              const GdkRGBA *send)
{
	return pidgin_color_is_visible(c, bg, MIN_COLOR_CONTRAST, MIN_BRIGHTNESS_CONTRAST) &&
	       pidgin_color_is_visible(c, highlight, MIN_COLOR_CONTRAST / 2, 0) &&
	       pidgin_color_is_visible(c, send, MIN_COLOR_CONTRAST / 4, 0);
}

guint
pidgin_nick_colors_generate(const GdkRGBA *background, GdkRGBA *colors, guint count)
{
	GdkRGBA highlight, send;
	GRand *rand;
	guint i = 0, j, tries = 0;

	gdk_rgba_parse(&highlight, DEFAULT_HIGHLIGHT_COLOR);
	gdk_rgba_parse(&send, DEFAULT_SEND_COLOR);

	for (j = 0; i < count && j < G_N_ELEMENTS(nick_seed_colors); j++) {
		GdkRGBA c = { nick_seed_colors[j][0] / 65535., nick_seed_colors[j][1] / 65535.,
		              nick_seed_colors[j][2] / 65535., 1. };
		if (nick_color_ok(&c, background, &highlight, &send))
			colors[i++] = c;
	}

	/* Pidgin 2 used srand(bg.red + bg.green + bg.blue + 1) and rand(); a
	 * private generator with the same idea keeps it deterministic
	 * without touching the global one. */
	rand = g_rand_new_with_seed((guint32)((background->red + background->green +
	                             background->blue) * 65535) + 1);
	while (i < count && tries++ < 200000) {
		GdkRGBA c = { g_rand_double(rand), g_rand_double(rand), g_rand_double(rand), 1. };
		if (nick_color_ok(&c, background, &highlight, &send))
			colors[i++] = c;
	}
	g_rand_free(rand);

	return i;
}

/**************************************************************************
 * XEP-0392 and HSLuv
 **************************************************************************/

double
pidgin_nick_color_xep0392_hue(const char *name)
{
	GChecksum *sha1 = g_checksum_new(G_CHECKSUM_SHA1);
	guint8 digest[20];
	gsize len = sizeof(digest);
	guint16 v;

	g_checksum_update(sha1, (const guchar *)name, strlen(name));
	g_checksum_get_digest(sha1, digest, &len);
	g_checksum_free(sha1);

	/* least-significant 16 bits, little endian */
	v = digest[0] | (digest[1] << 8);
	return v / 65536.0 * 360.0;
}

/* The HSLuv reference implementation (hsluv.org, MIT licence),
 * restated. */
static const double m[3][3] = {
	{  3.240969941904521, -1.537383177570093, -0.498610760293    },
	{ -0.96924363628087,   1.87596750150772,   0.041555057407175 },
	{  0.055630079696993, -0.20397695888897,   1.056971514242878 }
};
static const double ref_u = 0.19783000664283;
static const double ref_v = 0.46831999493879;
static const double kappa = 903.2962962;
static const double epsilon = 0.0088564516;

static double
max_chroma_for_lh(double l, double h)
{
	double hrad = h / 360.0 * 2 * G_PI;
	double sub1 = pow(l + 16, 3) / 1560896;
	double sub2 = sub1 > epsilon ? sub1 : l / kappa;
	double min = G_MAXDOUBLE;
	int c, t;

	for (c = 0; c < 3; c++) {
		double m1 = m[c][0], m2 = m[c][1], m3 = m[c][2];
		for (t = 0; t < 2; t++) {
			double top1 = (284517 * m1 - 94839 * m3) * sub2;
			double top2 = (838422 * m3 + 769860 * m2 + 731718 * m1) * l * sub2 -
			              769860 * t * l;
			double bottom = (632260 * m3 - 126452 * m2) * sub2 + 126452 * t;
			double slope = top1 / bottom, intercept = top2 / bottom;
			double length = intercept / (sin(hrad) - slope * cos(hrad));
			if (length >= 0 && length < min)
				min = length;
		}
	}
	return min;
}

static double
from_linear(double c)
{
	return c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1 / 2.4) - 0.055;
}

void
pidgin_hsluv_to_rgb(double h, double s, double l, double *r, double *g, double *b)
{
	double c, hrad, u, v, y, var_u, var_v, x, z, xyz[3], rgb[3];
	int i;

	/* HSLuv -> LCh */
	if (l > 99.9999999) {
		l = 100;
		c = 0;
	} else if (l < 0.00000001) {
		l = 0;
		c = 0;
	} else {
		c = max_chroma_for_lh(l, h) / 100 * s;
	}

	/* LCh -> Luv */
	hrad = h / 360.0 * 2 * G_PI;
	u = cos(hrad) * c;
	v = sin(hrad) * c;

	/* Luv -> XYZ */
	if (l == 0) {
		xyz[0] = xyz[1] = xyz[2] = 0;
	} else {
		var_u = u / (13 * l) + ref_u;
		var_v = v / (13 * l) + ref_v;
		y = (l <= 8) ? l / kappa : pow((l + 16) / 116, 3);
		x = 0 - (9 * y * var_u) / ((var_u - 4) * var_v - var_u * var_v);
		z = (9 * y - 15 * var_v * y - var_v * x) / (3 * var_v);
		xyz[0] = x;
		xyz[1] = y;
		xyz[2] = z;
	}

	/* XYZ -> sRGB */
	for (i = 0; i < 3; i++)
		rgb[i] = from_linear(m[i][0] * xyz[0] + m[i][1] * xyz[1] + m[i][2] * xyz[2]);

	*r = CLAMP(rgb[0], 0, 1);
	*g = CLAMP(rgb[1], 0, 1);
	*b = CLAMP(rgb[2], 0, 1);
}

/**************************************************************************
 * Lookup
 **************************************************************************/

static GdkRGBA palette[NUM_NICK_COLORS];
static guint palette_len = 0;
static GdkRGBA palette_bg;

static double
luminance(const GdkRGBA *c)
{
	return 0.3 * c->red + 0.59 * c->green + 0.11 * c->blue;
}

void
pidgin_nick_color_get(PidginNickColorScheme scheme, const char *name,
                      const GdkRGBA *background, GdkRGBA *color)
{
	g_return_if_fail(name != NULL && background != NULL && color != NULL);

	if (scheme == PIDGIN_NICK_COLOR_XEP0392) {
		double r, g, b;
		double l = luminance(background) < 0.5 ? 70 : 50;

		pidgin_hsluv_to_rgb(pidgin_nick_color_xep0392_hue(name), 100, l, &r, &g, &b);
		color->red = r;
		color->green = g;
		color->blue = b;
		color->alpha = 1.0;
		return;
	}

	if (palette_len == 0 || !gdk_rgba_equal(&palette_bg, background)) {
		palette_bg = *background;
		palette_len = pidgin_nick_colors_generate(background, palette, NUM_NICK_COLORS);
	}
	if (palette_len == 0) {
		gdk_rgba_parse(color, "#808080");
		return;
	}

	*color = palette[g_str_hash(name) % palette_len];

	/* As get_nick_color(): the palette suits white; lighten it for dark
	 * backgrounds (never darken). */
	{
		double scale = (1 - luminance(background)) /
		               MAX(MAX(color->red, color->green), MAX(color->blue, 0.01));
		if (scale > 1) {
			color->red = MIN(color->red * scale, 1.);
			color->green = MIN(color->green * scale, 1.);
			color->blue = MIN(color->blue * scale, 1.);
		}
	}
}
