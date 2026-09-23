/*
 * Unit tests for pidginnickcolor.c: the XEP-0392 test vectors and Pidgin
 * 2's palette rules.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "pidgin-internal.h"

#include <math.h>

#include "pidginnickcolor.h"

/* XEP-0392 section 13.1 (saturation 100, lightness 50) */
static const struct {
	const char *text;
	double hue, r, g, b;
} vectors[] = {
	{ "Romeo", 327.255249, 0.865, 0.000, 0.686 },
	{ "juliet@capulet.lit", 209.410400, 0.000, 0.515, 0.573 },
	{ "\xf0\x9f\x98\xba", 331.199341, 0.872, 0.000, 0.659 },
	{ "council", 359.994507, 0.918, 0.000, 0.394 },
	{ "Board", 171.430664, 0.000, 0.527, 0.457 },
};

static void
test_xep0392_vectors(void)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(vectors); i++) {
		double hue = pidgin_nick_color_xep0392_hue(vectors[i].text);
		double r, g, b;

		g_assert_cmpfloat_with_epsilon(hue, vectors[i].hue, 0.000001);
		pidgin_hsluv_to_rgb(hue, 100, 50, &r, &g, &b);
		g_assert_cmpfloat_with_epsilon(r, vectors[i].r, 0.001);
		g_assert_cmpfloat_with_epsilon(g, vectors[i].g, 0.001);
		g_assert_cmpfloat_with_epsilon(b, vectors[i].b, 0.001);
	}
}

static void
test_hsluv_extremes(void)
{
	double r, g, b;

	pidgin_hsluv_to_rgb(0, 0, 100, &r, &g, &b);
	g_assert_cmpfloat_with_epsilon(r, 1, 0.001);
	g_assert_cmpfloat_with_epsilon(g, 1, 0.001);
	g_assert_cmpfloat_with_epsilon(b, 1, 0.001);
	pidgin_hsluv_to_rgb(120, 100, 0, &r, &g, &b);
	g_assert_cmpfloat(r + g + b, <, 0.001);
}

static void
test_palette(void)
{
	GdkRGBA white = { 1, 1, 1, 1 }, black = { 0, 0, 0, 1 }, colors[220], a, b;
	guint n, i;

	n = pidgin_nick_colors_generate(&white, colors, G_N_ELEMENTS(colors));
	g_assert_cmpuint(n, ==, G_N_ELEMENTS(colors));
	for (i = 0; i < n; i++)
		g_assert_true(pidgin_color_is_visible(&colors[i], &white, 200, 75));

	n = pidgin_nick_colors_generate(&black, colors, G_N_ELEMENTS(colors));
	g_assert_cmpuint(n, ==, G_N_ELEMENTS(colors));
	for (i = 0; i < n; i++)
		g_assert_true(pidgin_color_is_visible(&colors[i], &black, 200, 75));

	/* deterministic per name and background */
	pidgin_nick_color_get(PIDGIN_NICK_COLOR_PIDGIN, "alice", &white, &a);
	pidgin_nick_color_get(PIDGIN_NICK_COLOR_PIDGIN, "alice", &white, &b);
	g_assert_true(gdk_rgba_equal(&a, &b));

	pidgin_nick_color_get(PIDGIN_NICK_COLOR_XEP0392, "Romeo", &white, &a);
	g_assert_cmpfloat_with_epsilon(a.red, 0.865, 0.001);
	g_assert_cmpfloat_with_epsilon(a.blue, 0.686, 0.001);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/nickcolor/xep0392-vectors", test_xep0392_vectors);
	g_test_add_func("/nickcolor/hsluv-extremes", test_hsluv_extremes);
	g_test_add_func("/nickcolor/palette", test_palette);

	return g_test_run();
}
