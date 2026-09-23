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

#include "debug.h"

#include "pidginanimation.h"

/*
 * GdkPixbufAnimation is deprecated since gdk-pixbuf 2.44 ("use a different
 * image loading library for animatable assets"), but GTK 4 has nothing to
 * replace it with here: GtkMediaFile needs GStreamer, which this GTK is
 * built without. The use is confined to this file.
 */
G_GNUC_BEGIN_IGNORE_DEPRECATIONS

/* Stop advancing when nobody has drawn us for this long (ms). */
#define IDLE_STOP_MS 1500

struct _PidginAnimation
{
	GObject parent;

	GdkPixbufAnimation *animation;
	GdkPixbufAnimationIter *iter;
	GdkTexture *frame;
	int width;
	int height;

	guint timeout;
	gboolean playing;
	gint64 last_drawn;
};

static void pidgin_animation_paintable_init(GdkPaintableInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(PidginAnimation, pidgin_animation, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(GDK_TYPE_PAINTABLE, pidgin_animation_paintable_init))

GdkTexture *
pidgin_texture_new_from_pixbuf(GdkPixbuf *pixbuf)
{
	GBytes *bytes;
	GdkTexture *texture;
	GdkMemoryFormat format;
	int width, height, stride;

	g_return_val_if_fail(GDK_IS_PIXBUF(pixbuf), NULL);

	width = gdk_pixbuf_get_width(pixbuf);
	height = gdk_pixbuf_get_height(pixbuf);
	stride = gdk_pixbuf_get_rowstride(pixbuf);
	format = gdk_pixbuf_get_has_alpha(pixbuf) ?
		GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8;

	bytes = gdk_pixbuf_read_pixel_bytes(pixbuf);
	texture = gdk_memory_texture_new(width, height, format, bytes, stride);
	g_bytes_unref(bytes);

	return texture;
}

static void
update_frame(PidginAnimation *anim)
{
	GdkPixbuf *pixbuf = gdk_pixbuf_animation_iter_get_pixbuf(anim->iter);

	g_clear_object(&anim->frame);
	if (pixbuf != NULL)
		anim->frame = pidgin_texture_new_from_pixbuf(pixbuf);
}

static void schedule_next(PidginAnimation *anim);

static gboolean
advance_cb(gpointer data)
{
	PidginAnimation *anim = data;

	anim->timeout = 0;

	/* Nobody looked at us for a while (scrolled away, window hidden):
	 * stop until the next snapshot. */
	if (g_get_monotonic_time() - anim->last_drawn > IDLE_STOP_MS * 1000)
		return G_SOURCE_REMOVE;

	if (gdk_pixbuf_animation_iter_advance(anim->iter, NULL)) {
		update_frame(anim);
		gdk_paintable_invalidate_contents(GDK_PAINTABLE(anim));
	}
	schedule_next(anim);

	return G_SOURCE_REMOVE;
}

static void
schedule_next(PidginAnimation *anim)
{
	int delay;

	if (anim->timeout != 0 || !anim->playing)
		return;

	delay = gdk_pixbuf_animation_iter_get_delay_time(anim->iter);
	if (delay < 0)
		return;         /* the last frame stays forever */
	anim->timeout = g_timeout_add(MAX(delay, 20), advance_cb, anim);
}

static void
pidgin_animation_snapshot(GdkPaintable *paintable, GdkSnapshot *snapshot,
                          double width, double height)
{
	PidginAnimation *anim = PIDGIN_ANIMATION(paintable);

	anim->last_drawn = g_get_monotonic_time();
	if (anim->frame != NULL)
		gdk_paintable_snapshot(GDK_PAINTABLE(anim->frame), snapshot, width, height);
	schedule_next(anim);
}

static int
pidgin_animation_get_intrinsic_width(GdkPaintable *paintable)
{
	return PIDGIN_ANIMATION(paintable)->width;
}

static int
pidgin_animation_get_intrinsic_height(GdkPaintable *paintable)
{
	return PIDGIN_ANIMATION(paintable)->height;
}

static GdkPaintable *
pidgin_animation_get_current_image(GdkPaintable *paintable)
{
	PidginAnimation *anim = PIDGIN_ANIMATION(paintable);

	if (anim->frame != NULL)
		return GDK_PAINTABLE(g_object_ref(anim->frame));
	return gdk_paintable_new_empty(anim->width, anim->height);
}

static void
pidgin_animation_paintable_init(GdkPaintableInterface *iface)
{
	iface->snapshot = pidgin_animation_snapshot;
	iface->get_intrinsic_width = pidgin_animation_get_intrinsic_width;
	iface->get_intrinsic_height = pidgin_animation_get_intrinsic_height;
	iface->get_current_image = pidgin_animation_get_current_image;
}

static void
pidgin_animation_dispose(GObject *obj)
{
	PidginAnimation *anim = PIDGIN_ANIMATION(obj);

	g_clear_handle_id(&anim->timeout, g_source_remove);
	g_clear_object(&anim->frame);
	g_clear_object(&anim->iter);
	g_clear_object(&anim->animation);

	G_OBJECT_CLASS(pidgin_animation_parent_class)->dispose(obj);
}

static void
pidgin_animation_class_init(PidginAnimationClass *klass)
{
	G_OBJECT_CLASS(klass)->dispose = pidgin_animation_dispose;
}

static void
pidgin_animation_init(PidginAnimation *anim)
{
	anim->playing = TRUE;
}

void
pidgin_animation_set_playing(PidginAnimation *animation, gboolean playing)
{
	g_return_if_fail(PIDGIN_IS_ANIMATION(animation));

	animation->playing = playing;
	if (!playing)
		g_clear_handle_id(&animation->timeout, g_source_remove);
	else
		schedule_next(animation);
}

static GdkPaintable *
paintable_from_animation(GdkPixbufAnimation *animation)
{
	PidginAnimation *anim;

	if (gdk_pixbuf_animation_is_static_image(animation)) {
		GdkPixbuf *pixbuf = gdk_pixbuf_animation_get_static_image(animation);

		return pixbuf ? GDK_PAINTABLE(pidgin_texture_new_from_pixbuf(pixbuf)) : NULL;
	}

	anim = g_object_new(PIDGIN_TYPE_ANIMATION, NULL);
	anim->animation = g_object_ref(animation);
	anim->width = gdk_pixbuf_animation_get_width(animation);
	anim->height = gdk_pixbuf_animation_get_height(animation);
	anim->iter = gdk_pixbuf_animation_get_iter(animation, NULL);
	update_frame(anim);

	return GDK_PAINTABLE(anim);
}

GdkPaintable *
pidgin_paintable_new_from_data(gconstpointer data, gsize len)
{
	GdkPixbufLoader *loader;
	GdkPixbufAnimation *animation;
	GdkPaintable *paintable = NULL;
	GError *error = NULL;

	if (data == NULL || len == 0)
		return NULL;

	loader = gdk_pixbuf_loader_new();
	if (!gdk_pixbuf_loader_write(loader, data, len, &error) ||
	    !gdk_pixbuf_loader_close(loader, &error)) {
		purple_debug_warning("pidginanimation", "Could not decode image: %s\n",
		                     error ? error->message : "unknown error");
		g_clear_error(&error);
		gdk_pixbuf_loader_close(loader, NULL);
		g_object_unref(loader);
		return NULL;
	}

	animation = gdk_pixbuf_loader_get_animation(loader);
	if (animation != NULL)
		paintable = paintable_from_animation(animation);
	g_object_unref(loader);

	return paintable;
}

GdkPaintable *
pidgin_paintable_new_from_file(const char *filename)
{
	char *contents = NULL;
	gsize len = 0;
	GdkPaintable *paintable;

	g_return_val_if_fail(filename != NULL, NULL);

	if (!g_file_get_contents(filename, &contents, &len, NULL))
		return NULL;
	paintable = pidgin_paintable_new_from_data(contents, len);
	g_free(contents);

	return paintable;
}

GdkPaintable *
pidgin_paintable_new_from_imgstore(PurpleStoredImage *image)
{
	g_return_val_if_fail(image != NULL, NULL);

	return pidgin_paintable_new_from_data(purple_imgstore_get_data(image),
	                                      purple_imgstore_get_size(image));
}

G_GNUC_END_IGNORE_DEPRECATIONS
