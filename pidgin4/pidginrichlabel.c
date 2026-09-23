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
#include "imgstore.h"
#include "smiley.h"
#include "util.h"

#include "pidginanimation.h"
#include "pidginimageloader.h"
#include "pidginrichlabel.h"
#include "pidginsmileytheme.h"

#define SPOILER_KEY "pidgin-spoiler"

typedef struct
{
	GtkTextChildAnchor *anchor;     /* NULL until shown */
	GdkTexture *texture;            /* for "save image" */
	int id;                         /* imgstore id or 0 */
	/* remote images still loading: the fallback text's range */
	GtkTextMark *start;
	GtkTextMark *end;
	char *uri;
	PidginRichLabel *label;         /* weak, for the async callback */
	int width, height;
} ImageSlot;

struct _PidginRichLabel
{
	GtkWidget parent;

	GtkWidget *child;
	PidginMarkupResult *result;
	char *highlight;
	guint32 revealed;
	gboolean force_text;
	GMenuModel *extra_menu;
	int max_width;
	int max_height;

	GCancellable *cancellable;
	GPtrArray *images;              /* ImageSlot*, text view mode */
	GHashTable *tag_cache;          /* text view mode */
};

G_DEFINE_FINAL_TYPE(PidginRichLabel, pidgin_rich_label, GTK_TYPE_WIDGET)

static void render(PidginRichLabel *label);

/**************************************************************************
 * Helpers
 **************************************************************************/

/* Slots are reference counted (GRcBox): a pending remote load holds one. */
static void
image_slot_clear(ImageSlot *slot)
{
	g_clear_object(&slot->anchor);
	g_clear_object(&slot->texture);
	g_free(slot->uri);
	g_clear_weak_pointer(&slot->label);
}

static void
image_slot_unref(ImageSlot *slot)
{
	g_rc_box_release_full(slot, (GDestroyNotify)image_slot_clear);
}

static ImageSlot *
image_slot_new(void)
{
	return g_rc_box_new0(ImageSlot);
}

/* Byte ranges of @needle in @text, case-insensitively (ASCII + simple
 * Unicode folding via g_utf8_casefold on both sides when lengths match). */
static GArray *
find_matches(const char *text, const char *needle)
{
	GArray *out = g_array_new(FALSE, FALSE, sizeof(guint));
	char *hay, *pin;
	const char *p;
	size_t nlen;

	if (needle == NULL || *needle == '\0' || text == NULL)
		return out;

	hay = g_utf8_casefold(text, -1);
	pin = g_utf8_casefold(needle, -1);
	nlen = strlen(pin);

	/* casefold can change byte lengths; only use it when it didn't */
	if (strlen(hay) != strlen(text)) {
		g_free(hay);
		hay = g_ascii_strdown(text, -1);
		g_free(pin);
		pin = g_ascii_strdown(needle, -1);
		nlen = strlen(pin);
	}
	for (p = hay; nlen > 0 && (p = strstr(p, pin)) != NULL; p += nlen) {
		guint s = p - hay, e = s + nlen;
		g_array_append_val(out, s);
		g_array_append_val(out, e);
	}
	g_free(hay);
	g_free(pin);
	return out;
}

static gboolean
needs_text_view(PidginRichLabel *label)
{
	return label->force_text || pidgin_markup_result_has_graphics(label->result);
}

static void
set_child(PidginRichLabel *label, GtkWidget *child)
{
	if (label->child == child)
		return;
	if (label->child != NULL)
		gtk_widget_unparent(label->child);
	label->child = child;
	if (child != NULL)
		gtk_widget_set_parent(child, GTK_WIDGET(label));
}

/**************************************************************************
 * Label mode
 **************************************************************************/

static gboolean
label_activate_link_cb(GtkLabel *gl, const char *uri, PidginRichLabel *label)
{
	if (g_str_has_prefix(uri, "pidgin-spoiler:")) {
		int n = atoi(uri + strlen("pidgin-spoiler:"));
		if (n >= 0 && n < 32)
			label->revealed |= 1u << n;
		render(label);
		return TRUE;
	}
	pidgin_markup_activate_uri(GTK_WIDGET(gl), uri);
	return TRUE;
}

static void
render_label(PidginRichLabel *label)
{
	GtkWidget *gl;
	char *markup;
	GArray *matches;
	guint i;

	if (label->child != NULL && GTK_IS_LABEL(label->child)) {
		gl = label->child;
	} else {
		gl = gtk_label_new(NULL);
		gtk_label_set_wrap(GTK_LABEL(gl), TRUE);
		gtk_label_set_wrap_mode(GTK_LABEL(gl), PANGO_WRAP_WORD_CHAR);
		gtk_label_set_natural_wrap_mode(GTK_LABEL(gl), GTK_NATURAL_WRAP_WORD);
		gtk_label_set_selectable(GTK_LABEL(gl), TRUE);
		gtk_label_set_xalign(GTK_LABEL(gl), 0.0);
		gtk_label_set_yalign(GTK_LABEL(gl), 0.0);
		gtk_widget_set_hexpand(gl, TRUE);
		gtk_widget_add_css_class(gl, "pidgin-rich-label");
		g_signal_connect(gl, "activate-link", G_CALLBACK(label_activate_link_cb), label);
		set_child(label, gl);
	}
	gtk_label_set_extra_menu(GTK_LABEL(gl), label->extra_menu);

	markup = pidgin_markup_result_to_pango_markup(label->result, label->revealed);
	gtk_label_set_markup(GTK_LABEL(gl), markup);
	g_free(markup);

	matches = find_matches(label->result->text, label->highlight);
	if (matches->len > 0) {
		PangoAttrList *attrs = pango_attr_list_new();
		for (i = 0; i + 1 < matches->len; i += 2) {
			PangoAttribute *a = pango_attr_background_new(0xffff, 0xe0e0, 0x3333);
			a->start_index = g_array_index(matches, guint, i);
			a->end_index = g_array_index(matches, guint, i + 1);
			pango_attr_list_insert(attrs, a);
			a = pango_attr_foreground_new(0, 0, 0);
			a->start_index = g_array_index(matches, guint, i);
			a->end_index = g_array_index(matches, guint, i + 1);
			pango_attr_list_insert(attrs, a);
		}
		gtk_label_set_attributes(GTK_LABEL(gl), attrs);
		pango_attr_list_unref(attrs);
	} else {
		gtk_label_set_attributes(GTK_LABEL(gl), NULL);
	}
	g_array_free(matches, TRUE);
}

/**************************************************************************
 * Text view mode
 **************************************************************************/

static GtkTextTag *
cached_tag(PidginRichLabel *label, GtkTextBuffer *buffer, const char *key,
           const char *first_prop, ...)
{
	GtkTextTag *tag = g_hash_table_lookup(label->tag_cache, key);
	va_list args;

	if (tag != NULL)
		return tag;
	tag = gtk_text_tag_new(NULL);
	va_start(args, first_prop);
	g_object_set_valist(G_OBJECT(tag), first_prop, args);
	va_end(args);
	gtk_text_tag_table_add(gtk_text_buffer_get_tag_table(buffer), tag);
	g_hash_table_insert(label->tag_cache, g_strdup(key), tag);
	g_object_unref(tag);
	return tag;
}

static GtkTextTag *
tag_for_attribute(PidginRichLabel *label, GtkTextBuffer *buffer, PangoAttribute *a)
{
	char key[64];

	switch (a->klass->type) {
		case PANGO_ATTR_WEIGHT:
			g_snprintf(key, sizeof(key), "w%d", ((PangoAttrInt *)a)->value);
			return cached_tag(label, buffer, key, "weight", ((PangoAttrInt *)a)->value, NULL);
		case PANGO_ATTR_STYLE:
			return cached_tag(label, buffer, "italic", "style", PANGO_STYLE_ITALIC, NULL);
		case PANGO_ATTR_UNDERLINE:
			return cached_tag(label, buffer, "underline", "underline", PANGO_UNDERLINE_SINGLE, NULL);
		case PANGO_ATTR_STRIKETHROUGH:
			return cached_tag(label, buffer, "strike", "strikethrough", TRUE, NULL);
		case PANGO_ATTR_FAMILY:
			g_snprintf(key, sizeof(key), "f%s", ((PangoAttrString *)a)->value);
			return cached_tag(label, buffer, key, "family", ((PangoAttrString *)a)->value, NULL);
		case PANGO_ATTR_FOREGROUND:
		case PANGO_ATTR_BACKGROUND: {
			PangoColor *c = &((PangoAttrColor *)a)->color;
			GdkRGBA rgba = { c->red / 65535., c->green / 65535., c->blue / 65535., 1 };
			gboolean fg = a->klass->type == PANGO_ATTR_FOREGROUND;
			g_snprintf(key, sizeof(key), "%c%04x%04x%04x", fg ? 'F' : 'B',
			           c->red, c->green, c->blue);
			return cached_tag(label, buffer, key,
			                  fg ? "foreground-rgba" : "background-rgba", &rgba, NULL);
		}
		case PANGO_ATTR_FOREGROUND_ALPHA: {
			GdkRGBA gray = { .5, .5, .5, 1 };
			return cached_tag(label, buffer, "dim", "foreground-rgba", &gray, NULL);
		}
		case PANGO_ATTR_SCALE:
			g_snprintf(key, sizeof(key), "s%.4f", ((PangoAttrFloat *)a)->value);
			return cached_tag(label, buffer, key, "scale", ((PangoAttrFloat *)a)->value, NULL);
		case PANGO_ATTR_SIZE:
			g_snprintf(key, sizeof(key), "p%d", ((PangoAttrInt *)a)->value);
			return cached_tag(label, buffer, key, "size", ((PangoAttrInt *)a)->value, NULL);
		case PANGO_ATTR_BASELINE_SHIFT:
		case PANGO_ATTR_FONT_SCALE: {
			/* GtkTextTag has no baseline shift; rise + scale look the
			 * same */
			gboolean sub = ((PangoAttrInt *)a)->value == PANGO_BASELINE_SHIFT_SUBSCRIPT ||
			               ((PangoAttrInt *)a)->value == PANGO_FONT_SCALE_SUBSCRIPT;
			if (a->klass->type == PANGO_ATTR_FONT_SCALE)
				return cached_tag(label, buffer, "subsupscale", "scale", 0.8, NULL);
			return cached_tag(label, buffer, sub ? "sub" : "sup",
			                  "rise", (sub ? -4 : 6) * PANGO_SCALE, NULL);
		}
		default:
			return NULL;
	}
}

static GtkTextIter
iter_at_byte(GtkTextBuffer *buffer, const char *text, guint byte)
{
	GtkTextIter iter;

	gtk_text_buffer_get_iter_at_offset(buffer, &iter,
		g_utf8_pointer_to_offset(text, text + MIN(byte, strlen(text))));
	return iter;
}

static GtkWidget *
picture_for(PidginRichLabel *label, GdkPaintable *paintable, int want_w, int want_h)
{
	GtkWidget *picture = gtk_picture_new_for_paintable(paintable);
	int w = gdk_paintable_get_intrinsic_width(paintable);
	int h = gdk_paintable_get_intrinsic_height(paintable);
	double scale = 1.0;

	if (want_w > 0 && want_h > 0) {
		w = want_w;
		h = want_h;
	}
	if (w > label->max_width)
		scale = (double)label->max_width / w;
	if (h * scale > label->max_height)
		scale = (double)label->max_height / h;
	if (w > 0 && h > 0)
		gtk_widget_set_size_request(picture, MAX(1, w * scale), MAX(1, h * scale));
	gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);
	gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
	gtk_widget_add_css_class(picture, "pidgin-inline-image");
	return picture;
}

static void
add_anchor_widget(PidginRichLabel *label, GtkTextBuffer *buffer, GtkTextIter *iter,
                  GtkWidget *widget, ImageSlot *slot)
{
	GtkTextChildAnchor *anchor = gtk_text_buffer_create_child_anchor(buffer, iter);

	gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(label->child), widget, anchor);
	if (slot != NULL)
		slot->anchor = g_object_ref(anchor);
}

static void
remote_loaded_cb(GObject *source, GAsyncResult *res, gpointer data)
{
	ImageSlot *slot = data;
	GError *error = NULL;
	GdkTexture *texture = pidgin_image_loader_load_finish(PIDGIN_IMAGE_LOADER(source),
	                                                      res, &error);
	PidginRichLabel *label = slot->label;
	GtkTextBuffer *buffer;
	GtkTextIter s, e;

	if (texture == NULL) {
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			purple_debug_info("richlabel", "Image %s not shown: %s\n", slot->uri,
			                  error ? error->message : "?");
		g_clear_error(&error);
		image_slot_unref(slot);
		return;
	}
	if (label == NULL || !GTK_IS_TEXT_VIEW(label->child) || slot->start == NULL ||
	    gtk_text_mark_get_deleted(slot->start)) {
		g_object_unref(texture);
		image_slot_unref(slot);
		return;
	}

	buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(label->child));
	gtk_text_buffer_get_iter_at_mark(buffer, &s, slot->start);
	gtk_text_buffer_get_iter_at_mark(buffer, &e, slot->end);
	gtk_text_buffer_delete(buffer, &s, &e);
	slot->texture = texture;
	add_anchor_widget(label, buffer, &s,
	                  picture_for(label, GDK_PAINTABLE(texture), slot->width, slot->height),
	                  slot);
	gtk_text_buffer_delete_mark(buffer, slot->start);
	gtk_text_buffer_delete_mark(buffer, slot->end);
	slot->start = slot->end = NULL;
	image_slot_unref(slot);
}

static void
replace_object(PidginRichLabel *label, GtkTextBuffer *buffer, const char *text,
               const PidginMarkupObject *o)
{
	GtkTextIter s = iter_at_byte(buffer, text, o->start);
	GtkTextIter e = iter_at_byte(buffer, text, o->end);

	switch (o->type) {
		case PIDGIN_MARKUP_OBJECT_IMAGE: {
			PurpleStoredImage *img = purple_imgstore_find_by_id(o->id);
			GdkPaintable *paintable = img ? pidgin_paintable_new_from_imgstore(img) : NULL;
			ImageSlot *slot;

			if (paintable == NULL)
				return;
			slot = image_slot_new();
			slot->id = o->id;
			if (GDK_IS_TEXTURE(paintable))
				slot->texture = g_object_ref(GDK_TEXTURE(paintable));
			else
				slot->texture = GDK_TEXTURE(gdk_paintable_get_current_image(paintable));
			gtk_text_buffer_delete(buffer, &s, &e);
			add_anchor_widget(label, buffer, &s,
			                  picture_for(label, paintable, o->width, o->height), slot);
			g_ptr_array_add(label->images, slot);
			g_object_unref(paintable);
			break;
		}
		case PIDGIN_MARKUP_OBJECT_SMILEY: {
			GdkPaintable *paintable = NULL;

			if (o->id == 1) {
				PurpleSmiley *smiley = purple_smileys_find_by_shortcut(o->alt);
				if (smiley)
					paintable = pidgin_custom_smiley_get_paintable(smiley);
			} else {
				PidginSmiley *smiley = pidgin_smiley_theme_lookup(o->sml, o->alt);
				if (smiley)
					paintable = pidgin_smiley_get_paintable(smiley);
			}
			if (paintable == NULL)
				return;
			gtk_text_buffer_delete(buffer, &s, &e);
			gtk_text_buffer_insert_paintable(buffer, &s, paintable);
			break;
		}
		case PIDGIN_MARKUP_OBJECT_HR: {
			GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
			gtk_widget_set_size_request(sep, 240, -1);
			gtk_text_buffer_delete(buffer, &s, &e);
			add_anchor_widget(label, buffer, &s, sep, NULL);
			break;
		}
		case PIDGIN_MARKUP_OBJECT_REMOTE_IMAGE: {
			PidginImageLoader *loader = pidgin_image_loader_get_default();
			ImageSlot *slot;
			GdkTexture *cached;

			if (loader == NULL || !pidgin_image_loader_is_allowed(loader, o->uri))
				return;         /* the alt text stays, linked */

			slot = image_slot_new();
			slot->uri = g_strdup(o->uri);
			slot->width = o->width;
			slot->height = o->height;
			g_ptr_array_add(label->images, slot);

			cached = pidgin_image_loader_lookup_cached(loader, o->uri);
			if (cached != NULL) {
				slot->texture = cached;
				gtk_text_buffer_delete(buffer, &s, &e);
				add_anchor_widget(label, buffer, &s,
				                  picture_for(label, GDK_PAINTABLE(cached), o->width, o->height),
				                  slot);
				break;
			}
			slot->start = gtk_text_buffer_create_mark(buffer, NULL, &s, TRUE);
			slot->end = gtk_text_buffer_create_mark(buffer, NULL, &e, FALSE);
			g_set_weak_pointer(&slot->label, label);
			pidgin_image_loader_load_async(loader, o->uri, label->cancellable,
			                               remote_loaded_cb, g_rc_box_acquire(slot));
			break;
		}
		default:
			break;
	}
}

static const char *
link_at_iter(const GtkTextIter *iter)
{
	GSList *tags = gtk_text_iter_get_tags(iter), *l;
	const char *uri = NULL;

	for (l = tags; l && uri == NULL; l = l->next)
		uri = g_object_get_data(G_OBJECT(l->data), PIDGIN_MARKUP_LINK_KEY);
	g_slist_free(tags);
	return uri;
}

static gboolean
iter_at_point(PidginRichLabel *label, double x, double y, GtkTextIter *iter)
{
	int bx, by;
	double tx, ty;
	graphene_point_t p, out;

	if (!GTK_IS_TEXT_VIEW(label->child))
		return FALSE;
	/* (x, y) are in @label's coordinates */
	graphene_point_init(&p, x, y);
	if (!gtk_widget_compute_point(GTK_WIDGET(label), label->child, &p, &out))
		return FALSE;
	tx = out.x;
	ty = out.y;
	gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(label->child),
	                                      GTK_TEXT_WINDOW_WIDGET, tx, ty, &bx, &by);
	return gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(label->child), iter, bx, by);
}

static void
text_click_released_cb(GtkGestureClick *gesture, int n_press, double x, double y,
                       PidginRichLabel *label)
{
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(label->child));
	GtkTextIter iter, s, e;
	GSList *tags, *l;
	graphene_point_t p, out;

	if (gtk_text_buffer_get_selection_bounds(buffer, &s, &e))
		return;         /* the user was selecting */

	graphene_point_init(&p, x, y);
	if (!gtk_widget_compute_point(label->child, GTK_WIDGET(label), &p, &out) ||
	    !iter_at_point(label, out.x, out.y, &iter))
		return;

	tags = gtk_text_iter_get_tags(&iter);
	for (l = tags; l; l = l->next) {
		GtkTextTag *tag = l->data;
		const char *uri;

		if (g_object_get_data(G_OBJECT(tag), SPOILER_KEY)) {
			s = e = iter;
			gtk_text_iter_backward_to_tag_toggle(&s, tag);
			gtk_text_iter_forward_to_tag_toggle(&e, tag);
			gtk_text_buffer_remove_tag(buffer, tag, &s, &e);
			break;
		}
		if ((uri = g_object_get_data(G_OBJECT(tag), PIDGIN_MARKUP_LINK_KEY)) != NULL) {
			pidgin_markup_activate_uri(label->child, uri);
			break;
		}
	}
	g_slist_free(tags);
}

static void
text_motion_cb(GtkEventControllerMotion *motion, double x, double y,
               PidginRichLabel *label)
{
	GtkTextIter iter;
	graphene_point_t p, out;
	gboolean on_link = FALSE;

	graphene_point_init(&p, x, y);
	if (gtk_widget_compute_point(label->child, GTK_WIDGET(label), &p, &out) &&
	    iter_at_point(label, out.x, out.y, &iter))
		on_link = link_at_iter(&iter) != NULL;
	gtk_widget_set_cursor_from_name(label->child, on_link ? "pointer" : NULL);
}

static void
render_text_view(PidginRichLabel *label)
{
	GtkWidget *view;
	GtkTextBuffer *buffer;
	const char *text = label->result->text;
	GSList *attrs, *l;
	GArray *matches;
	guint i, spoiler_index = 0;
	GtkTextIter s, e;

	g_cancellable_cancel(label->cancellable);
	g_clear_object(&label->cancellable);
	label->cancellable = g_cancellable_new();
	g_ptr_array_set_size(label->images, 0);
	g_hash_table_remove_all(label->tag_cache);

	/* A fresh view each time: anchors and their widgets go with it. */
	view = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
	gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
	gtk_widget_set_hexpand(view, TRUE);
	gtk_widget_add_css_class(view, "pidgin-rich-text");
	gtk_text_view_set_extra_menu(GTK_TEXT_VIEW(view), label->extra_menu);
	{
		GtkGesture *click = gtk_gesture_click_new();
		GtkEventController *motion = gtk_event_controller_motion_new();
		gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
		g_signal_connect(click, "released", G_CALLBACK(text_click_released_cb), label);
		gtk_widget_add_controller(view, GTK_EVENT_CONTROLLER(click));
		g_signal_connect(motion, "motion", G_CALLBACK(text_motion_cb), label);
		gtk_widget_add_controller(view, motion);
	}
	set_child(label, view);

	buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
	gtk_text_buffer_set_enable_undo(buffer, FALSE);
	gtk_text_buffer_set_text(buffer, text, -1);

	attrs = pango_attr_list_get_attributes(label->result->attrs);
	for (l = attrs; l; l = l->next) {
		PangoAttribute *a = l->data;
		GtkTextTag *tag = tag_for_attribute(label, buffer, a);
		if (tag == NULL)
			continue;
		s = iter_at_byte(buffer, text, a->start_index);
		e = iter_at_byte(buffer, text, a->end_index);
		gtk_text_buffer_apply_tag(buffer, tag, &s, &e);
	}
	g_slist_free_full(attrs, (GDestroyNotify)pango_attribute_destroy);

	for (i = 0; i < label->result->objects->len; i++) {
		PidginMarkupObject *o = g_ptr_array_index(label->result->objects, i);
		GtkTextTag *tag = NULL;

		s = iter_at_byte(buffer, text, o->start);
		e = iter_at_byte(buffer, text, o->end);
		switch (o->type) {
			case PIDGIN_MARKUP_OBJECT_LINK: {
				GdkRGBA blue = { .11, .44, .85, 1 };
				tag = gtk_text_buffer_create_tag(buffer, NULL,
					"underline", PANGO_UNDERLINE_SINGLE, "foreground-rgba", &blue, NULL);
				g_object_set_data_full(G_OBJECT(tag), PIDGIN_MARKUP_LINK_KEY,
				                       g_strdup(o->uri), g_free);
				break;
			}
			case PIDGIN_MARKUP_OBJECT_SPOILER:
				if (spoiler_index >= 32 || !(label->revealed & (1u << spoiler_index))) {
					GdkRGBA black = { 0, 0, 0, 1 };
					tag = gtk_text_buffer_create_tag(buffer, NULL,
						"foreground-rgba", &black, "background-rgba", &black, NULL);
					g_object_set_data(G_OBJECT(tag), SPOILER_KEY, GINT_TO_POINTER(1));
				}
				spoiler_index++;
				break;
			case PIDGIN_MARKUP_OBJECT_QUOTE:
				tag = cached_tag(label, buffer, o->id > 1 ? "quote2" : "quote1",
				                 "left-margin", 14 * MIN(o->id, 4), NULL);
				break;
			case PIDGIN_MARKUP_OBJECT_CODE_BLOCK: {
				GdkRGBA bg = { .5, .5, .5, .15 };
				tag = cached_tag(label, buffer, "codeblock", "paragraph-background-rgba",
				                 &bg, "family", "monospace", NULL);
				break;
			}
			default:
				break;
		}
		if (tag != NULL)
			gtk_text_buffer_apply_tag(buffer, tag, &s, &e);
	}

	matches = find_matches(text, label->highlight);
	if (matches->len > 0) {
		GdkRGBA yellow = { 1, .88, .2, 1 }, black = { 0, 0, 0, 1 };
		GtkTextTag *hl = gtk_text_buffer_create_tag(buffer, NULL,
			"background-rgba", &yellow, "foreground-rgba", &black, NULL);
		for (i = 0; i + 1 < matches->len; i += 2) {
			s = iter_at_byte(buffer, text, g_array_index(matches, guint, i));
			e = iter_at_byte(buffer, text, g_array_index(matches, guint, i + 1));
			gtk_text_buffer_apply_tag(buffer, hl, &s, &e);
		}
	}
	g_array_free(matches, TRUE);

	/* Graphics last, back to front, so earlier byte offsets stay valid. */
	for (i = label->result->objects->len; i-- > 0; )
		replace_object(label, buffer, text, g_ptr_array_index(label->result->objects, i));
}

static void
render(PidginRichLabel *label)
{
	if (label->result == NULL)
		label->result = pidgin_markup_parse_html("", NULL);

	if (needs_text_view(label))
		render_text_view(label);
	else
		render_label(label);
}

/**************************************************************************
 * GObject
 **************************************************************************/

static void
pidgin_rich_label_dispose(GObject *obj)
{
	PidginRichLabel *label = PIDGIN_RICH_LABEL(obj);

	if (label->cancellable)
		g_cancellable_cancel(label->cancellable);
	g_clear_object(&label->cancellable);
	set_child(label, NULL);
	g_clear_pointer(&label->images, g_ptr_array_unref);
	g_clear_pointer(&label->tag_cache, g_hash_table_destroy);
	g_clear_pointer(&label->result, pidgin_markup_result_unref);
	g_clear_object(&label->extra_menu);
	g_clear_pointer(&label->highlight, g_free);

	G_OBJECT_CLASS(pidgin_rich_label_parent_class)->dispose(obj);
}

static void
pidgin_rich_label_class_init(PidginRichLabelClass *klass)
{
	GObjectClass *obj_class = G_OBJECT_CLASS(klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

	obj_class->dispose = pidgin_rich_label_dispose;
	gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
	gtk_widget_class_set_css_name(widget_class, "pidgin-rich-label");
}

static void
pidgin_rich_label_init(PidginRichLabel *label)
{
	label->images = g_ptr_array_new_with_free_func((GDestroyNotify)image_slot_unref);
	label->tag_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	label->max_width = 320;
	label->max_height = 240;
	gtk_widget_set_hexpand(GTK_WIDGET(label), TRUE);
}

/**************************************************************************
 * API
 **************************************************************************/

GtkWidget *
pidgin_rich_label_new(void)
{
	return g_object_new(PIDGIN_TYPE_RICH_LABEL, NULL);
}

void
pidgin_rich_label_set_result(PidginRichLabel *label, PidginMarkupResult *result)
{
	g_return_if_fail(PIDGIN_IS_RICH_LABEL(label));
	g_return_if_fail(result != NULL);

	if (label->result == result && label->child != NULL)
		return;
	pidgin_markup_result_ref(result);
	g_clear_pointer(&label->result, pidgin_markup_result_unref);
	label->result = result;
	label->revealed = 0;
	render(label);
}

void
pidgin_rich_label_set_html(PidginRichLabel *label, const char *html,
                           const PidginMarkupOptions *options)
{
	PidginMarkupResult *result;

	g_return_if_fail(PIDGIN_IS_RICH_LABEL(label));

	result = pidgin_markup_parse_html(html, options);
	pidgin_rich_label_set_result(label, result);
	pidgin_markup_result_unref(result);
}

void
pidgin_rich_label_set_text(PidginRichLabel *label, const char *text)
{
	char *escaped = g_markup_escape_text(text ? text : "", -1);
	char *html = purple_strreplace(escaped, "\n", "<br>");
	PidginMarkupOptions opts = { PIDGIN_MARKUP_NO_SMILEYS, NULL, NULL, NULL };

	pidgin_rich_label_set_html(label, html, &opts);
	g_free(html);
	g_free(escaped);
}

const char *
pidgin_rich_label_get_text(PidginRichLabel *label)
{
	g_return_val_if_fail(PIDGIN_IS_RICH_LABEL(label), NULL);
	return label->result ? label->result->text : "";
}

void
pidgin_rich_label_set_highlight(PidginRichLabel *label, const char *needle)
{
	g_return_if_fail(PIDGIN_IS_RICH_LABEL(label));

	if (needle != NULL && *needle == '\0')
		needle = NULL;
	if (g_strcmp0(label->highlight, needle) == 0)
		return;
	g_free(label->highlight);
	label->highlight = g_strdup(needle);
	if (label->result != NULL)
		render(label);
}

void
pidgin_rich_label_set_force_text_view(PidginRichLabel *label, gboolean force)
{
	g_return_if_fail(PIDGIN_IS_RICH_LABEL(label));
	if (label->force_text == force)
		return;
	label->force_text = force;
	if (label->result != NULL)
		render(label);
}

void
pidgin_rich_label_set_extra_menu(PidginRichLabel *label, GMenuModel *menu)
{
	g_return_if_fail(PIDGIN_IS_RICH_LABEL(label));

	g_set_object(&label->extra_menu, menu);
	if (label->child == NULL)
		return;
	if (GTK_IS_LABEL(label->child))
		gtk_label_set_extra_menu(GTK_LABEL(label->child), menu);
	else
		gtk_text_view_set_extra_menu(GTK_TEXT_VIEW(label->child), menu);
}

void
pidgin_rich_label_set_max_image_size(PidginRichLabel *label, int width, int height)
{
	g_return_if_fail(PIDGIN_IS_RICH_LABEL(label));
	label->max_width = MAX(16, width);
	label->max_height = MAX(16, height);
}

gboolean
pidgin_rich_label_is_text_view(PidginRichLabel *label)
{
	g_return_val_if_fail(PIDGIN_IS_RICH_LABEL(label), FALSE);
	return label->child != NULL && GTK_IS_TEXT_VIEW(label->child);
}

GtkWidget *
pidgin_rich_label_get_inner(PidginRichLabel *label)
{
	g_return_val_if_fail(PIDGIN_IS_RICH_LABEL(label), NULL);
	return label->child;
}

GdkTexture *
pidgin_rich_label_get_image(PidginRichLabel *label, double x, double y, int *id)
{
	GtkTextIter iter;
	guint i;

	g_return_val_if_fail(PIDGIN_IS_RICH_LABEL(label), NULL);

	if (id)
		*id = 0;
	if (label->images == NULL || label->images->len == 0)
		return NULL;

	if (x >= 0) {
		GtkTextChildAnchor *anchor;
		if (!iter_at_point(label, x, y, &iter))
			return NULL;
		anchor = gtk_text_iter_get_child_anchor(&iter);
		for (i = 0; anchor && i < label->images->len; i++) {
			ImageSlot *slot = g_ptr_array_index(label->images, i);
			if (slot->anchor == anchor && slot->texture) {
				if (id)
					*id = slot->id;
				return g_object_ref(slot->texture);
			}
		}
		return NULL;
	}

	for (i = 0; i < label->images->len; i++) {
		ImageSlot *slot = g_ptr_array_index(label->images, i);
		if (slot->texture) {
			if (id)
				*id = slot->id;
			return g_object_ref(slot->texture);
		}
	}
	return NULL;
}

const char *
pidgin_rich_label_get_link_at(PidginRichLabel *label, double x, double y)
{
	GtkTextIter iter;

	g_return_val_if_fail(PIDGIN_IS_RICH_LABEL(label), NULL);
	if (!iter_at_point(label, x, y, &iter))
		return NULL;
	return link_at_iter(&iter);
}
