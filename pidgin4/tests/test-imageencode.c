/*
 * pidgin4: pasted image encoding (pidginimageencode.c): the "auto" choice
 * between PNG and JPEG, and the forced formats. Headless.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "pidgin-internal.h"

#include "pidginimageencode.h"

#include "test-support.h"

/* A photo-like opaque image: smooth gradients with a little noise. */
static GdkTexture *
make_gradient(int width, int height)
{
	guint8 *data = g_malloc(width * height * 4);
	GBytes *bytes;
	GdkTexture *texture;
	guint32 seed = 1;
	int x, y;

	for (y = 0; y < height; y++)
		for (x = 0; x < width; x++) {
			guint8 *p = data + (y * width + x) * 4;

			seed = seed * 1103515245 + 12345;
			p[0] = (guint8)(x * 255 / width + ((seed >> 16) & 7));
			p[1] = (guint8)(y * 255 / height + ((seed >> 20) & 7));
			p[2] = (guint8)((x + y) * 127 / (width + height) + ((seed >> 24) & 7));
			p[3] = 0xff;
		}
	bytes = g_bytes_new_take(data, width * height * 4);
	texture = gdk_memory_texture_new(width, height, GDK_MEMORY_R8G8B8A8, bytes, width * 4);
	g_bytes_unref(bytes);
	return texture;
}

/* The same with a transparent hole. */
static GdkTexture *
make_alpha(int width, int height)
{
	guint8 *data = g_malloc(width * height * 4);
	GBytes *bytes;
	GdkTexture *texture;
	int x, y;

	for (y = 0; y < height; y++)
		for (x = 0; x < width; x++) {
			guint8 *p = data + (y * width + x) * 4;

			p[0] = (guint8)(x * 255 / width);
			p[1] = (guint8)(y * 255 / height);
			p[2] = 0x80;
			p[3] = (x > width / 4 && x < width / 2) ? 0x00 : 0xff;
		}
	bytes = g_bytes_new_take(data, width * height * 4);
	texture = gdk_memory_texture_new(width, height, GDK_MEMORY_R8G8B8A8, bytes, width * 4);
	g_bytes_unref(bytes);
	return texture;
}

/* A flat UI-like screenshot: large single-colour areas. */
static GdkTexture *
make_flat(int width, int height)
{
	guint8 *data = g_malloc(width * height * 4);
	GBytes *bytes;
	GdkTexture *texture;
	int x, y;

	for (y = 0; y < height; y++)
		for (x = 0; x < width; x++) {
			guint8 *p = data + (y * width + x) * 4;
			gboolean bar = y < height / 8;

			p[0] = bar ? 0x33 : 0xf6;
			p[1] = bar ? 0x33 : 0xf5;
			p[2] = bar ? 0x44 : 0xf4;
			p[3] = 0xff;
		}
	bytes = g_bytes_new_take(data, width * height * 4);
	texture = gdk_memory_texture_new(width, height, GDK_MEMORY_R8G8B8A8, bytes, width * 4);
	g_bytes_unref(bytes);
	return texture;
}

static gboolean
is_png(GBytes *bytes)
{
	const guint8 *d = g_bytes_get_data(bytes, NULL);

	return g_bytes_get_size(bytes) > 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G';
}

static gboolean
is_jpeg(GBytes *bytes)
{
	const guint8 *d = g_bytes_get_data(bytes, NULL);

	return g_bytes_get_size(bytes) > 3 && d[0] == 0xff && d[1] == 0xd8 && d[2] == 0xff;
}

static void
test_alpha_detection(void)
{
	GdkTexture *opaque = make_gradient(64, 48);
	GdkTexture *alpha = make_alpha(64, 48);

	g_assert_false(pidgin_texture_has_alpha(opaque));
	g_assert_true(pidgin_texture_has_alpha(alpha));
	g_object_unref(opaque);
	g_object_unref(alpha);
}

static void
test_auto_opaque_gradient(void)
{
	GdkTexture *texture = make_gradient(320, 240);
	const char *ext = NULL;
	GBytes *bytes = pidgin_image_encode(texture, "auto", 85, &ext);

	g_assert_cmpstr(ext, ==, "jpg");
	g_assert_true(is_jpeg(bytes));
	g_bytes_unref(bytes);
	g_object_unref(texture);
}

static void
test_auto_alpha(void)
{
	GdkTexture *texture = make_alpha(320, 240);
	const char *ext = NULL;
	GBytes *bytes = pidgin_image_encode(texture, "auto", 85, &ext);

	g_assert_cmpstr(ext, ==, "png");
	g_assert_true(is_png(bytes));
	g_bytes_unref(bytes);
	g_object_unref(texture);
}

static void
test_auto_flat(void)
{
	/* opaque, but the PNG is smaller: stays PNG */
	GdkTexture *texture = make_flat(320, 240);
	const char *ext = NULL;
	GBytes *bytes = pidgin_image_encode(texture, "auto", 85, &ext);

	g_assert_cmpstr(ext, ==, "png");
	g_assert_true(is_png(bytes));
	g_bytes_unref(bytes);
	g_object_unref(texture);
}

static void
test_forced(void)
{
	GdkTexture *gradient = make_gradient(64, 48);
	GdkTexture *alpha = make_alpha(64, 48);
	const char *ext = NULL;
	GBytes *bytes;

	bytes = pidgin_image_encode(gradient, "png", 85, &ext);
	g_assert_cmpstr(ext, ==, "png");
	g_assert_true(is_png(bytes));
	g_bytes_unref(bytes);

	/* forced JPEG flattens alpha */
	bytes = pidgin_image_encode(alpha, "jpeg", 60, &ext);
	g_assert_cmpstr(ext, ==, "jpg");
	g_assert_true(is_jpeg(bytes));
	g_bytes_unref(bytes);

	/* quality matters (and is clamped) */
	{
		GBytes *lo = pidgin_image_encode(gradient, "jpeg", 10, NULL);
		GBytes *hi = pidgin_image_encode(gradient, "jpeg", 100, NULL);

		g_assert_cmpuint(g_bytes_get_size(lo), <, g_bytes_get_size(hi));
		g_bytes_unref(lo);
		g_bytes_unref(hi);
	}

	/* unknown formats are auto */
	bytes = pidgin_image_encode(alpha, "webp", 85, &ext);
	g_assert_cmpstr(ext, ==, "png");
	g_bytes_unref(bytes);

	g_object_unref(gradient);
	g_object_unref(alpha);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/imageencode/alpha", test_alpha_detection);
	g_test_add_func("/imageencode/auto-opaque-gradient", test_auto_opaque_gradient);
	g_test_add_func("/imageencode/auto-alpha", test_auto_alpha);
	g_test_add_func("/imageencode/auto-flat", test_auto_flat);
	g_test_add_func("/imageencode/forced", test_forced);

	return g_test_run();
}
