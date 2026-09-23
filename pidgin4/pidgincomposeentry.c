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
#include "pidgin.h"

#include <libspelling.h>

#include "debug.h"
#include "imgstore.h"
#include "prefs.h"
#include "smiley.h"
#include "util.h"

#include "pidginanimation.h"
#include "pidgincomposeentry.h"
#include "pidginsmileytheme.h"

#define TYPED_TIMEOUT_SECONDS 5     /* libpurple's SEND_TYPED_TIMEOUT_SECONDS */
#define HISTORY_MAX 100

struct _PidginComposeEntry
{
	GtkSourceView parent;

	PidginFormatCaps caps;
	PidginMarkupFlags markup_flags;
	char *smiley_sml;
	gboolean return_newline;

	SpellingTextBufferAdapter *spelling;

	/* formatting for newly typed text */
	GHashTable *pending;            /* GtkTextTag* set */
	gboolean pending_locked;        /* set by a toggle, until the cursor moves */
	int internal;                   /* >0 while we change the buffer ourselves */

	PurpleTypingState typing;
	guint typing_timeout;

	GList *history;                 /* char* html, newest first */
	int history_pos;                /* -1 = the draft */
	char *draft;
};

enum {
	SIG_MESSAGE_SEND,
	SIG_TYPING_CHANGED,
	SIG_EDIT_LAST,
	SIG_FORMAT_CHANGED,
	N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(PidginComposeEntry, pidgin_compose_entry, GTK_SOURCE_TYPE_VIEW)

/**************************************************************************
 * Shared prefs (read only)
 **************************************************************************/

static gboolean
pref_bool(const char *name, gboolean def)
{
	return purple_prefs_exists(name) ? purple_prefs_get_bool(name) : def;
}

static const char *
pref_string(const char *name)
{
	return purple_prefs_exists(name) ? purple_prefs_get_string(name) : NULL;
}

static int
pref_int(const char *name, int def)
{
	return purple_prefs_exists(name) ? purple_prefs_get_int(name) : def;
}

/**************************************************************************
 * Tags
 **************************************************************************/

static GtkTextBuffer *
get_buffer(PidginComposeEntry *entry)
{
	return gtk_text_view_get_buffer(GTK_TEXT_VIEW(entry));
}

static GtkTextTag *
named_tag(PidginComposeEntry *entry, const char *name)
{
	return gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(get_buffer(entry)), name);
}

/* The formatting families; links are not "pending" formatting. */
static const char *
tag_family(GtkTextTag *tag)
{
	static const char *fixed[] = { "bold", "italic", "underline", "strike", "code", NULL };
	static const char *prefixed[] = { "size:", "face:", "fore:", "back:", NULL };
	char *name = NULL;
	const char *family = NULL;
	int i;

	g_object_get(tag, "name", &name, NULL);
	if (name != NULL) {
		for (i = 0; fixed[i] && !family; i++)
			if (!strcmp(name, fixed[i]))
				family = fixed[i];
		for (i = 0; prefixed[i] && !family; i++)
			if (g_str_has_prefix(name, prefixed[i]))
				family = prefixed[i];
	}
	g_free(name);
	return family;
}

static void
emit_format_changed(PidginComposeEntry *entry)
{
	g_signal_emit(entry, signals[SIG_FORMAT_CHANGED], 0);
}

/* The formatting in effect for text typed at the cursor. */
static void
recompute_pending(PidginComposeEntry *entry)
{
	GtkTextBuffer *buffer = get_buffer(entry);
	GtkTextIter iter;
	GSList *tags, *l;

	gtk_text_buffer_get_iter_at_mark(buffer, &iter, gtk_text_buffer_get_insert(buffer));
	/* the character before the cursor decides, as in word processors */
	if (!gtk_text_iter_is_start(&iter))
		gtk_text_iter_backward_char(&iter);
	else if (gtk_text_iter_is_end(&iter))
		return;         /* empty buffer: keep what was set */

	g_hash_table_remove_all(entry->pending);
	tags = gtk_text_iter_get_tags(&iter);
	for (l = tags; l; l = l->next)
		if (tag_family(l->data) != NULL)
			g_hash_table_add(entry->pending, l->data);
	g_slist_free(tags);
}

static void
mark_set_cb(GtkTextBuffer *buffer, GtkTextIter *location, GtkTextMark *mark,
            PidginComposeEntry *entry)
{
	if (mark != gtk_text_buffer_get_insert(buffer) || entry->internal > 0)
		return;
	entry->pending_locked = FALSE;
	recompute_pending(entry);
	emit_format_changed(entry);
}

/* New text gets the pending formatting. */
static void
insert_text_after_cb(GtkTextBuffer *buffer, GtkTextIter *location, char *text,
                     int len, PidginComposeEntry *entry)
{
	GtkTextIter start = *location;
	GHashTableIter it;
	gpointer tag;
	glong chars;

	if (entry->internal > 0)
		return;
	chars = g_utf8_strlen(text, len);
	gtk_text_iter_backward_chars(&start, chars);

	g_hash_table_iter_init(&it, entry->pending);
	while (g_hash_table_iter_next(&it, &tag, NULL))
		gtk_text_buffer_apply_tag(buffer, tag, &start, location);
}

/* The range formatting applies to: the selection, the whole buffer with
 * WBFO, or nothing (then only new text). */
static gboolean
format_range(PidginComposeEntry *entry, GtkTextIter *start, GtkTextIter *end)
{
	GtkTextBuffer *buffer = get_buffer(entry);

	if (entry->caps & PIDGIN_FORMAT_WBFO) {
		gtk_text_buffer_get_bounds(buffer, start, end);
		return TRUE;
	}
	return gtk_text_buffer_get_selection_bounds(buffer, start, end);
}

static gboolean
range_has_tag(GtkTextIter *start, GtkTextIter *end, GtkTextTag *tag)
{
	GtkTextIter iter = *start;

	if (gtk_text_iter_equal(start, end))
		return FALSE;
	if (!gtk_text_iter_has_tag(&iter, tag))
		return FALSE;
	/* no toggle (off) before the end */
	gtk_text_iter_forward_to_tag_toggle(&iter, tag);
	return gtk_text_iter_compare(&iter, end) >= 0;
}

static void
toggle_tag(PidginComposeEntry *entry, GtkTextTag *tag, PidginFormatCaps needed)
{
	GtkTextBuffer *buffer = get_buffer(entry);
	GtkTextIter start, end;
	gboolean on;

	if (!(entry->caps & needed) || tag == NULL)
		return;

	if (format_range(entry, &start, &end) && !gtk_text_iter_equal(&start, &end)) {
		on = !range_has_tag(&start, &end, tag);
		if (on)
			gtk_text_buffer_apply_tag(buffer, tag, &start, &end);
		else
			gtk_text_buffer_remove_tag(buffer, tag, &start, &end);
	} else {
		on = !g_hash_table_contains(entry->pending, tag);
	}

	if (on)
		g_hash_table_add(entry->pending, tag);
	else
		g_hash_table_remove(entry->pending, tag);
	entry->pending_locked = TRUE;
	emit_format_changed(entry);
}

/* Replaces the @family ("size:", "face:", ...) formatting with @tag
 * (NULL: none) on the format range and for new text. */
static void
set_family(PidginComposeEntry *entry, const char *family, GtkTextTag *tag)
{
	GtkTextBuffer *buffer = get_buffer(entry);
	GtkTextIter start, end;
	GHashTableIter it;
	gpointer t;

	if (format_range(entry, &start, &end)) {
		pidgin_markup_buffer_remove_family(buffer, family, &start, &end);
		if (tag != NULL)
			gtk_text_buffer_apply_tag(buffer, tag, &start, &end);
	}

	g_hash_table_iter_init(&it, entry->pending);
	while (g_hash_table_iter_next(&it, &t, NULL))
		if (purple_strequal(tag_family(t), family))
			g_hash_table_iter_remove(&it);
	if (tag != NULL)
		g_hash_table_add(entry->pending, tag);
	entry->pending_locked = TRUE;
	emit_format_changed(entry);
}

/**************************************************************************
 * Formatting API
 **************************************************************************/

void
pidgin_compose_entry_toggle_bold(PidginComposeEntry *entry)
{
	toggle_tag(entry, named_tag(entry, "bold"), PIDGIN_FORMAT_BOLD);
}

void
pidgin_compose_entry_toggle_italic(PidginComposeEntry *entry)
{
	toggle_tag(entry, named_tag(entry, "italic"), PIDGIN_FORMAT_ITALIC);
}

void
pidgin_compose_entry_toggle_underline(PidginComposeEntry *entry)
{
	toggle_tag(entry, named_tag(entry, "underline"), PIDGIN_FORMAT_UNDERLINE);
}

void
pidgin_compose_entry_toggle_strike(PidginComposeEntry *entry)
{
	toggle_tag(entry, named_tag(entry, "strike"), PIDGIN_FORMAT_STRIKE);
}

void
pidgin_compose_entry_toggle_code(PidginComposeEntry *entry)
{
	toggle_tag(entry, named_tag(entry, "code"), PIDGIN_FORMAT_CODE);
}

gboolean
pidgin_compose_entry_get_format(PidginComposeEntry *entry, PidginFormatCaps format)
{
	const char *name;

	g_return_val_if_fail(PIDGIN_IS_COMPOSE_ENTRY(entry), FALSE);

	switch (format) {
		case PIDGIN_FORMAT_BOLD: name = "bold"; break;
		case PIDGIN_FORMAT_ITALIC: name = "italic"; break;
		case PIDGIN_FORMAT_UNDERLINE: name = "underline"; break;
		case PIDGIN_FORMAT_STRIKE: name = "strike"; break;
		case PIDGIN_FORMAT_CODE: name = "code"; break;
		default: return FALSE;
	}
	return g_hash_table_contains(entry->pending, named_tag(entry, name));
}

int
pidgin_compose_entry_get_font_size(PidginComposeEntry *entry)
{
	GHashTableIter it;
	gpointer tag;

	g_return_val_if_fail(PIDGIN_IS_COMPOSE_ENTRY(entry), 3);

	g_hash_table_iter_init(&it, entry->pending);
	while (g_hash_table_iter_next(&it, &tag, NULL)) {
		char *name = NULL;
		int size = 0;
		g_object_get(tag, "name", &name, NULL);
		if (name && g_str_has_prefix(name, "size:"))
			size = atoi(name + 5);
		g_free(name);
		if (size > 0)
			return size;
	}
	return 3;
}

static void
change_size(PidginComposeEntry *entry, int delta)
{
	int size;

	if (!(entry->caps & PIDGIN_FORMAT_SIZE))
		return;
	size = CLAMP(pidgin_compose_entry_get_font_size(entry) + delta, 1, 7);
	set_family(entry, "size:", size == 3 ? NULL :
	           pidgin_markup_buffer_get_size_tag(get_buffer(entry), size));
}

void
pidgin_compose_entry_grow_font(PidginComposeEntry *entry)
{
	g_return_if_fail(PIDGIN_IS_COMPOSE_ENTRY(entry));
	change_size(entry, 1);
}

void
pidgin_compose_entry_shrink_font(PidginComposeEntry *entry)
{
	g_return_if_fail(PIDGIN_IS_COMPOSE_ENTRY(entry));
	change_size(entry, -1);
}

void
pidgin_compose_entry_set_font_face(PidginComposeEntry *entry, const char *face)
{
	g_return_if_fail(PIDGIN_IS_COMPOSE_ENTRY(entry));
	if (!(entry->caps & PIDGIN_FORMAT_FACE))
		return;
	set_family(entry, "face:", (face && *face) ?
	           pidgin_markup_buffer_get_face_tag(get_buffer(entry), face) : NULL);
}

void
pidgin_compose_entry_set_forecolor(PidginComposeEntry *entry, const GdkRGBA *color)
{
	g_return_if_fail(PIDGIN_IS_COMPOSE_ENTRY(entry));
	if (!(entry->caps & PIDGIN_FORMAT_FORECOLOR))
		return;
	set_family(entry, "fore:", color ?
	           pidgin_markup_buffer_get_fore_tag(get_buffer(entry), color) : NULL);
}

void
pidgin_compose_entry_set_backcolor(PidginComposeEntry *entry, const GdkRGBA *color)
{
	g_return_if_fail(PIDGIN_IS_COMPOSE_ENTRY(entry));
	if (!(entry->caps & PIDGIN_FORMAT_BACKCOLOR))
		return;
	set_family(entry, "back:", color ?
	           pidgin_markup_buffer_get_back_tag(get_buffer(entry), color) : NULL);
}

void
pidgin_compose_entry_clear_formatting(PidginComposeEntry *entry)
{
	GtkTextBuffer *buffer;
	GtkTextIter start, end;
	static const char *families[] = { "bold", "italic", "underline", "strike",
		"code", "size:", "face:", "fore:", "back:", NULL };
	int i;

	g_return_if_fail(PIDGIN_IS_COMPOSE_ENTRY(entry));

	buffer = get_buffer(entry);
	if (!gtk_text_buffer_get_selection_bounds(buffer, &start, &end))
		gtk_text_buffer_get_bounds(buffer, &start, &end);
	for (i = 0; families[i]; i++)
		pidgin_markup_buffer_remove_family(buffer, families[i], &start, &end);
	g_hash_table_remove_all(entry->pending);
	entry->pending_locked = TRUE;
	emit_format_changed(entry);
}

/**************************************************************************
 * Inline objects
 **************************************************************************/

static void
unref_image_id(gpointer data)
{
	purple_imgstore_unref_by_id(GPOINTER_TO_INT(data));
}

static void
insert_anchor(PidginComposeEntry *entry, GtkTextIter *iter, GdkPaintable *paintable,
              const char *html, int image_id)
{
	GtkTextBuffer *buffer = get_buffer(entry);
	GtkTextChildAnchor *anchor = gtk_text_child_anchor_new();
	GtkWidget *picture = gtk_picture_new_for_paintable(paintable);
	int w = gdk_paintable_get_intrinsic_width(paintable);
	int h = gdk_paintable_get_intrinsic_height(paintable);
	double scale = 1.0;

	g_object_set_data_full(G_OBJECT(anchor), PIDGIN_MARKUP_HTML_KEY, g_strdup(html), g_free);
	if (image_id > 0) {
		purple_imgstore_ref_by_id(image_id);
		g_object_set_data_full(G_OBJECT(anchor), "pidgin-image-id",
		                       GINT_TO_POINTER(image_id), unref_image_id);
	}

	/* big pictures are shown small in the entry */
	if (w > 96 || h > 96)
		scale = 96.0 / MAX(w, h);
	if (w > 0 && h > 0)
		gtk_widget_set_size_request(picture, MAX(1, w * scale), MAX(1, h * scale));
	gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);

	gtk_text_buffer_insert_child_anchor(buffer, iter, anchor);
	gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(entry), picture, anchor);
	g_object_unref(anchor);
}

static GdkPaintable *
smiley_paintable(PidginComposeEntry *entry, const char *shortcut)
{
	PurpleSmiley *custom = purple_smileys_find_by_shortcut(shortcut);
	PidginSmiley *smiley;

	if (custom != NULL && (entry->caps & PIDGIN_FORMAT_CUSTOM_SMILEY))
		return pidgin_custom_smiley_get_paintable(custom);
	smiley = pidgin_smiley_theme_lookup(entry->smiley_sml, shortcut);
	return smiley ? pidgin_smiley_get_paintable(smiley) : NULL;
}

void
pidgin_compose_entry_insert_smiley(PidginComposeEntry *entry, const char *shortcut)
{
	GtkTextBuffer *buffer;
	GtkTextIter iter;
	GdkPaintable *paintable;
	char *html;

	g_return_if_fail(PIDGIN_IS_COMPOSE_ENTRY(entry));
	g_return_if_fail(shortcut != NULL);

	buffer = get_buffer(entry);
	gtk_text_buffer_delete_selection(buffer, TRUE, TRUE);
	gtk_text_buffer_get_iter_at_mark(buffer, &iter, gtk_text_buffer_get_insert(buffer));

	paintable = smiley_paintable(entry, shortcut);
	if (paintable == NULL) {
		gtk_text_buffer_insert(buffer, &iter, shortcut, -1);
		return;
	}
	html = g_markup_escape_text(shortcut, -1);
	insert_anchor(entry, &iter, paintable, html, 0);
	g_free(html);
}

void
pidgin_compose_entry_insert_image(PidginComposeEntry *entry, int imgstore_id)
{
	PurpleStoredImage *img;
	GdkPaintable *paintable;
	GtkTextBuffer *buffer;
	GtkTextIter iter;
	char *html;

	g_return_if_fail(PIDGIN_IS_COMPOSE_ENTRY(entry));

	img = purple_imgstore_find_by_id(imgstore_id);
	if (img == NULL || (paintable = pidgin_paintable_new_from_imgstore(img)) == NULL)
		return;

	buffer = get_buffer(entry);
	gtk_text_buffer_delete_selection(buffer, TRUE, TRUE);
	gtk_text_buffer_get_iter_at_mark(buffer, &iter, gtk_text_buffer_get_insert(buffer));
	html = g_strdup_printf("<IMG ID=\"%d\">", imgstore_id);
	insert_anchor(entry, &iter, paintable, html, imgstore_id);
	g_free(html);
	g_object_unref(paintable);
}

void
pidgin_compose_entry_insert_link(PidginComposeEntry *entry, const char *url,
                                 const char *description)
{
	GtkTextBuffer *buffer;
	GtkTextIter start, end;
	GtkTextTag *tag;
	GtkTextMark *mark;

	g_return_if_fail(PIDGIN_IS_COMPOSE_ENTRY(entry));
	g_return_if_fail(url != NULL && *url);

	buffer = get_buffer(entry);
	if (!(entry->caps & PIDGIN_FORMAT_LINK)) {
		/* no link markup: the URL as text */
		gtk_text_buffer_insert_at_cursor(buffer, url, -1);
		return;
	}
	if (!(entry->caps & PIDGIN_FORMAT_LINKDESC))
		description = NULL;

	tag = pidgin_markup_buffer_create_link_tag(buffer, url);
	if (description == NULL && gtk_text_buffer_get_selection_bounds(buffer, &start, &end)) {
		gtk_text_buffer_apply_tag(buffer, tag, &start, &end);
		return;
	}

	gtk_text_buffer_delete_selection(buffer, TRUE, TRUE);
	gtk_text_buffer_get_iter_at_mark(buffer, &start, gtk_text_buffer_get_insert(buffer));
	mark = gtk_text_buffer_create_mark(buffer, NULL, &start, TRUE);
	gtk_text_buffer_insert(buffer, &start, (description && *description) ? description : url, -1);
	gtk_text_buffer_get_iter_at_mark(buffer, &end, mark);
	gtk_text_buffer_apply_tag(buffer, tag, &end, &start);
	gtk_text_buffer_delete_mark(buffer, mark);
}

static void
insert_object_cb(GtkTextBuffer *buffer, GtkTextIter *iter, const PidginMarkupObject *o,
                 gpointer data)
{
	PidginComposeEntry *entry = data;
	GdkPaintable *paintable = NULL;
	char *html;

	if (o->type == PIDGIN_MARKUP_OBJECT_IMAGE) {
		PurpleStoredImage *img = purple_imgstore_find_by_id(o->id);
		if (img != NULL)
			paintable = pidgin_paintable_new_from_imgstore(img);
		if (paintable == NULL) {
			gtk_text_buffer_insert(buffer, iter, o->alt ? o->alt : _("[Image]"), -1);
			return;
		}
		html = g_strdup_printf("<IMG ID=\"%d\">", o->id);
		insert_anchor(entry, iter, paintable, html, o->id);
		g_object_unref(paintable);
	} else {
		paintable = smiley_paintable(entry, o->alt);
		if (paintable == NULL) {
			gtk_text_buffer_insert(buffer, iter, o->alt, -1);
			return;
		}
		html = g_markup_escape_text(o->alt, -1);
		insert_anchor(entry, iter, paintable, html, 0);
	}
	g_free(html);
}

/**************************************************************************
 * Content
 **************************************************************************/

gboolean
pidgin_compose_entry_is_empty(PidginComposeEntry *entry)
{
	g_return_val_if_fail(PIDGIN_IS_COMPOSE_ENTRY(entry), TRUE);
	return gtk_text_buffer_get_char_count(get_buffer(entry)) == 0;
}

char *
pidgin_compose_entry_get_markup(PidginComposeEntry *entry)
{
	GtkTextBuffer *buffer;
	GtkTextIter start, end;

	g_return_val_if_fail(PIDGIN_IS_COMPOSE_ENTRY(entry), NULL);

	buffer = get_buffer(entry);
	gtk_text_buffer_get_bounds(buffer, &start, &end);

	if (entry->caps & PIDGIN_FORMAT_STYLING) {
		char *text = pidgin_markup_buffer_to_styling(buffer, &start, &end);
		char *escaped = g_markup_escape_text(text, -1);
		char *html = purple_strreplace(escaped, "\n", "<br>");
		g_free(escaped);
		g_free(text);
		return html;
	}
	return pidgin_markup_buffer_to_html(buffer, &start, &end, entry->markup_flags |
	       ((entry->caps & PIDGIN_FORMAT_WBFO) ? PIDGIN_MARKUP_WBFO : 0));
}

char *
pidgin_compose_entry_get_text(PidginComposeEntry *entry)
{
	GtkTextBuffer *buffer;
	GtkTextIter start, end;
	char *text, *html;

	g_return_val_if_fail(PIDGIN_IS_COMPOSE_ENTRY(entry), NULL);

	buffer = get_buffer(entry);
	gtk_text_buffer_get_bounds(buffer, &start, &end);
	/* serialized without directives: plain text, anchors as their HTML
	 * (smiley shortcuts are escaped there) */
	html = pidgin_markup_buffer_to_html(buffer, &start, &end, PIDGIN_MARKUP_WBFO);
	text = pidgin_markup_html_to_plain(html);
	g_free(html);
	return text;
}

void
pidgin_compose_entry_clear(PidginComposeEntry *entry)
{
	g_return_if_fail(PIDGIN_IS_COMPOSE_ENTRY(entry));

	entry->internal++;
	gtk_text_buffer_set_text(get_buffer(entry), "", 0);
	entry->internal--;
}

void
pidgin_compose_entry_set_markup(PidginComposeEntry *entry, const char *html)
{
	GtkTextBuffer *buffer;
	GtkTextIter iter;
	PidginFormatCaps caps;

	g_return_if_fail(PIDGIN_IS_COMPOSE_ENTRY(entry));

	buffer = get_buffer(entry);
	pidgin_compose_entry_clear(entry);
	if (html == NULL)
		return;

	/* XEP-0393 text goes back in as typed: the directives are text. */
	caps = entry->caps & ~PIDGIN_FORMAT_STYLING;
	entry->internal++;
	gtk_text_buffer_get_end_iter(buffer, &iter);
	pidgin_markup_buffer_insert_html(buffer, &iter, html, caps,
	                                 entry->markup_flags & PIDGIN_MARKUP_USE_POINTSIZE,
	                                 insert_object_cb, entry);
	entry->internal--;
	gtk_text_buffer_get_end_iter(buffer, &iter);
	gtk_text_buffer_place_cursor(buffer, &iter);
}

/**************************************************************************
 * Sending, history
 **************************************************************************/

GList *
pidgin_compose_entry_get_history(PidginComposeEntry *entry)
{
	g_return_val_if_fail(PIDGIN_IS_COMPOSE_ENTRY(entry), NULL);
	return entry->history;
}

static void
history_add(PidginComposeEntry *entry, const char *markup)
{
	GList *last;

	entry->history = g_list_prepend(entry->history, g_strdup(markup));
	if (g_list_length(entry->history) > HISTORY_MAX) {
		last = g_list_last(entry->history);
		g_free(last->data);
		entry->history = g_list_delete_link(entry->history, last);
	}
	entry->history_pos = -1;
	g_clear_pointer(&entry->draft, g_free);
}

gboolean
pidgin_compose_entry_send(PidginComposeEntry *entry)
{
	gboolean handled = FALSE;
	char *markup;

	g_return_val_if_fail(PIDGIN_IS_COMPOSE_ENTRY(entry), FALSE);

	if (pidgin_compose_entry_is_empty(entry))
		return FALSE;

	markup = pidgin_compose_entry_get_markup(entry);
	g_signal_emit(entry, signals[SIG_MESSAGE_SEND], 0, markup, &handled);
	if (handled) {
		/* the history keeps what the buffer can load back */
		char *html;
		GtkTextIter s, e;
		gtk_text_buffer_get_bounds(get_buffer(entry), &s, &e);
		html = pidgin_markup_buffer_to_html(get_buffer(entry), &s, &e, 0);
		history_add(entry, html);
		g_free(html);
		pidgin_compose_entry_clear(entry);
	}
	g_free(markup);
	return handled;
}

void
pidgin_compose_entry_history_up(PidginComposeEntry *entry)
{
	GList *item;

	g_return_if_fail(PIDGIN_IS_COMPOSE_ENTRY(entry));

	item = g_list_nth(entry->history, entry->history_pos + 1);
	if (item == NULL)
		return;
	if (entry->history_pos == -1) {
		GtkTextIter s, e;
		g_free(entry->draft);
		gtk_text_buffer_get_bounds(get_buffer(entry), &s, &e);
		entry->draft = pidgin_markup_buffer_to_html(get_buffer(entry), &s, &e, 0);
	}
	entry->history_pos++;
	pidgin_compose_entry_set_markup(entry, item->data);
}

void
pidgin_compose_entry_history_down(PidginComposeEntry *entry)
{
	g_return_if_fail(PIDGIN_IS_COMPOSE_ENTRY(entry));

	if (entry->history_pos < 0)
		return;
	entry->history_pos--;
	if (entry->history_pos == -1)
		pidgin_compose_entry_set_markup(entry, entry->draft);
	else
		pidgin_compose_entry_set_markup(entry,
			g_list_nth_data(entry->history, entry->history_pos));
}

/**************************************************************************
 * Typing notification
 **************************************************************************/

static void
set_typing(PidginComposeEntry *entry, PurpleTypingState state)
{
	if (entry->typing == state)
		return;
	entry->typing = state;
	g_signal_emit(entry, signals[SIG_TYPING_CHANGED], 0, state);
}

static gboolean
typed_timeout_cb(gpointer data)
{
	PidginComposeEntry *entry = data;

	entry->typing_timeout = 0;
	set_typing(entry, PURPLE_TYPED);
	return G_SOURCE_REMOVE;
}

static void
buffer_changed_cb(GtkTextBuffer *buffer, PidginComposeEntry *entry)
{
	g_clear_handle_id(&entry->typing_timeout, g_source_remove);

	if (gtk_text_buffer_get_char_count(buffer) == 0) {
		set_typing(entry, PURPLE_NOT_TYPING);
		return;
	}
	if (entry->internal > 0)
		return;
	set_typing(entry, PURPLE_TYPING);
	entry->typing_timeout = g_timeout_add_seconds(TYPED_TIMEOUT_SECONDS,
	                                              typed_timeout_cb, entry);
}

/**************************************************************************
 * Keys
 **************************************************************************/

static gboolean
key_pressed_cb(GtkEventControllerKey *controller, guint keyval, guint keycode,
               GdkModifierType state, PidginComposeEntry *entry)
{
	GdkModifierType mods = state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_ALT_MASK |
	                                GDK_SUPER_MASK);

	switch (keyval) {
		case GDK_KEY_Return:
		case GDK_KEY_KP_Enter:
		case GDK_KEY_ISO_Enter:
			if (mods == 0 && !entry->return_newline) {
				pidgin_compose_entry_send(entry);
				return TRUE;
			}
			if (mods & GDK_CONTROL_MASK) {
				/* GtkTextView ignores Ctrl+Enter; make it a newline */
				gtk_text_buffer_delete_selection(get_buffer(entry), TRUE, TRUE);
				gtk_text_buffer_insert_at_cursor(get_buffer(entry), "\n", 1);
				return TRUE;
			}
			return FALSE;
		case GDK_KEY_Up:
		case GDK_KEY_KP_Up:
			if (mods == GDK_CONTROL_MASK) {
				pidgin_compose_entry_history_up(entry);
				return TRUE;
			}
			if (mods == 0 && pidgin_compose_entry_is_empty(entry)) {
				g_signal_emit(entry, signals[SIG_EDIT_LAST], 0);
				return TRUE;
			}
			return FALSE;
		case GDK_KEY_Down:
		case GDK_KEY_KP_Down:
			if (mods == GDK_CONTROL_MASK) {
				pidgin_compose_entry_history_down(entry);
				return TRUE;
			}
			return FALSE;
		case GDK_KEY_b:
		case GDK_KEY_B:
			if (mods == GDK_CONTROL_MASK && (entry->caps & PIDGIN_FORMAT_BOLD)) {
				pidgin_compose_entry_toggle_bold(entry);
				return TRUE;
			}
			return FALSE;
		case GDK_KEY_i:
		case GDK_KEY_I:
			if (mods == GDK_CONTROL_MASK && (entry->caps & PIDGIN_FORMAT_ITALIC)) {
				pidgin_compose_entry_toggle_italic(entry);
				return TRUE;
			}
			return FALSE;
		case GDK_KEY_u:
		case GDK_KEY_U:
			if (mods == GDK_CONTROL_MASK && (entry->caps & PIDGIN_FORMAT_UNDERLINE)) {
				pidgin_compose_entry_toggle_underline(entry);
				return TRUE;
			}
			return FALSE;
		default:
			return FALSE;
	}
}

/**************************************************************************
 * Setup
 **************************************************************************/

void
pidgin_compose_entry_set_caps(PidginComposeEntry *entry, PidginFormatCaps caps)
{
	GtkTextBuffer *buffer;
	GtkTextIter start, end;
	static const struct { PidginFormatCaps cap; const char *family; } drop[] = {
		{ PIDGIN_FORMAT_BOLD, "bold" }, { PIDGIN_FORMAT_ITALIC, "italic" },
		{ PIDGIN_FORMAT_UNDERLINE, "underline" }, { PIDGIN_FORMAT_STRIKE, "strike" },
		{ PIDGIN_FORMAT_CODE, "code" }, { PIDGIN_FORMAT_SIZE, "size:" },
		{ PIDGIN_FORMAT_FACE, "face:" }, { PIDGIN_FORMAT_FORECOLOR, "fore:" },
		{ PIDGIN_FORMAT_BACKCOLOR, "back:" }, { PIDGIN_FORMAT_LINK, "link" },
	};
	guint i;

	g_return_if_fail(PIDGIN_IS_COMPOSE_ENTRY(entry));

	entry->caps = caps;
	buffer = get_buffer(entry);
	gtk_text_buffer_get_bounds(buffer, &start, &end);
	for (i = 0; i < G_N_ELEMENTS(drop); i++) {
		if (caps & drop[i].cap)
			continue;
		pidgin_markup_buffer_remove_family(buffer, drop[i].family, &start, &end);
		{
			GHashTableIter it;
			gpointer t;
			g_hash_table_iter_init(&it, entry->pending);
			while (g_hash_table_iter_next(&it, &t, NULL))
				if (purple_strequal(tag_family(t), drop[i].family))
					g_hash_table_iter_remove(&it);
		}
	}
	emit_format_changed(entry);
}

PidginFormatCaps
pidgin_compose_entry_get_caps(PidginComposeEntry *entry)
{
	g_return_val_if_fail(PIDGIN_IS_COMPOSE_ENTRY(entry), 0);
	return entry->caps;
}

void
pidgin_compose_entry_setup(PidginComposeEntry *entry, PurpleConnectionFlags features)
{
	const char *s;
	GdkRGBA rgba;
	int size;

	g_return_if_fail(PIDGIN_IS_COMPOSE_ENTRY(entry));

	g_hash_table_remove_all(entry->pending);
	pidgin_compose_entry_set_caps(entry, pidgin_format_caps_from_features(features));
	if (!(features & PURPLE_CONNECTION_HTML))
		return;

	/* The default formatting Pidgin 2's prefs ask for (shared prefs). */
	if (pref_bool(PIDGIN_PREFS_ROOT "/conversations/send_bold", FALSE))
		g_hash_table_add(entry->pending, named_tag(entry, "bold"));
	if (pref_bool(PIDGIN_PREFS_ROOT "/conversations/send_italic", FALSE))
		g_hash_table_add(entry->pending, named_tag(entry, "italic"));
	if (pref_bool(PIDGIN_PREFS_ROOT "/conversations/send_underline", FALSE))
		g_hash_table_add(entry->pending, named_tag(entry, "underline"));
	s = pref_string(PIDGIN_PREFS_ROOT "/conversations/font_face");
	if (s && *s)
		g_hash_table_add(entry->pending, pidgin_markup_buffer_get_face_tag(get_buffer(entry), s));
	size = pref_int(PIDGIN_PREFS_ROOT "/conversations/font_size", 3);
	if (!(features & PURPLE_CONNECTION_NO_FONTSIZE) && size != 3 && size >= 1 && size <= 7)
		g_hash_table_add(entry->pending, pidgin_markup_buffer_get_size_tag(get_buffer(entry), size));
	s = pref_string(PIDGIN_PREFS_ROOT "/conversations/fgcolor");
	if (s && *s && pidgin_markup_parse_color(s, &rgba))
		g_hash_table_add(entry->pending, pidgin_markup_buffer_get_fore_tag(get_buffer(entry), &rgba));
	s = pref_string(PIDGIN_PREFS_ROOT "/conversations/bgcolor");
	if (s && *s && !(features & PURPLE_CONNECTION_NO_BGCOLOR) &&
	    pidgin_markup_parse_color(s, &rgba))
		g_hash_table_add(entry->pending, pidgin_markup_buffer_get_back_tag(get_buffer(entry), &rgba));
	entry->pending_locked = TRUE;
	emit_format_changed(entry);
}

void
pidgin_compose_entry_set_markup_flags(PidginComposeEntry *entry, PidginMarkupFlags flags)
{
	g_return_if_fail(PIDGIN_IS_COMPOSE_ENTRY(entry));
	entry->markup_flags = flags;
}

void
pidgin_compose_entry_set_smiley_category(PidginComposeEntry *entry, const char *sml)
{
	g_return_if_fail(PIDGIN_IS_COMPOSE_ENTRY(entry));
	g_free(entry->smiley_sml);
	entry->smiley_sml = g_strdup(sml);
}

void
pidgin_compose_entry_set_return_inserts_newline(PidginComposeEntry *entry, gboolean newline)
{
	g_return_if_fail(PIDGIN_IS_COMPOSE_ENTRY(entry));
	entry->return_newline = newline;
}

void
pidgin_compose_entry_set_spellcheck(PidginComposeEntry *entry, gboolean enabled)
{
	g_return_if_fail(PIDGIN_IS_COMPOSE_ENTRY(entry));
	spelling_text_buffer_adapter_set_enabled(entry->spelling, enabled);
}

gboolean
pidgin_compose_entry_get_spellcheck(PidginComposeEntry *entry)
{
	g_return_val_if_fail(PIDGIN_IS_COMPOSE_ENTRY(entry), FALSE);
	return spelling_text_buffer_adapter_get_enabled(entry->spelling);
}

/**************************************************************************
 * GObject
 **************************************************************************/

static void
pidgin_compose_entry_dispose(GObject *obj)
{
	PidginComposeEntry *entry = PIDGIN_COMPOSE_ENTRY(obj);

	g_clear_handle_id(&entry->typing_timeout, g_source_remove);
	g_clear_object(&entry->spelling);

	G_OBJECT_CLASS(pidgin_compose_entry_parent_class)->dispose(obj);
}

static void
pidgin_compose_entry_finalize(GObject *obj)
{
	PidginComposeEntry *entry = PIDGIN_COMPOSE_ENTRY(obj);

	g_hash_table_destroy(entry->pending);
	g_list_free_full(entry->history, g_free);
	g_free(entry->draft);
	g_free(entry->smiley_sml);

	G_OBJECT_CLASS(pidgin_compose_entry_parent_class)->finalize(obj);
}

static void
pidgin_compose_entry_class_init(PidginComposeEntryClass *klass)
{
	GObjectClass *obj_class = G_OBJECT_CLASS(klass);

	obj_class->dispose = pidgin_compose_entry_dispose;
	obj_class->finalize = pidgin_compose_entry_finalize;

	signals[SIG_MESSAGE_SEND] = g_signal_new("message-send",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
		g_signal_accumulator_true_handled, NULL, NULL,
		G_TYPE_BOOLEAN, 1, G_TYPE_STRING);
	signals[SIG_TYPING_CHANGED] = g_signal_new("typing-changed",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, G_TYPE_INT);
	signals[SIG_EDIT_LAST] = g_signal_new("edit-last-requested",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 0);
	signals[SIG_FORMAT_CHANGED] = g_signal_new("format-changed",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 0);

	spelling_init();
}

static void
pidgin_compose_entry_init(PidginComposeEntry *entry)
{
	GtkSourceBuffer *buffer = gtk_source_buffer_new(NULL);
	GtkEventController *keys;

	entry->pending = g_hash_table_new(g_direct_hash, g_direct_equal);
	entry->history_pos = -1;
	entry->typing = PURPLE_NOT_TYPING;
	entry->caps = PIDGIN_FORMAT_HTML_ALL;

	gtk_source_buffer_set_highlight_syntax(buffer, FALSE);
	gtk_source_buffer_set_highlight_matching_brackets(buffer, FALSE);
	pidgin_markup_buffer_ensure_tags(GTK_TEXT_BUFFER(buffer));
	gtk_text_view_set_buffer(GTK_TEXT_VIEW(entry), GTK_TEXT_BUFFER(buffer));
	g_object_unref(buffer);

	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(entry), GTK_WRAP_WORD_CHAR);
	gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(entry), FALSE);
	gtk_text_view_set_left_margin(GTK_TEXT_VIEW(entry), 4);
	gtk_text_view_set_right_margin(GTK_TEXT_VIEW(entry), 4);
	gtk_text_view_set_top_margin(GTK_TEXT_VIEW(entry), 2);
	gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(entry), 2);
	gtk_widget_add_css_class(GTK_WIDGET(entry), "pidgin-compose-entry");

	g_signal_connect_after(buffer, "insert-text", G_CALLBACK(insert_text_after_cb), entry);
	g_signal_connect(buffer, "mark-set", G_CALLBACK(mark_set_cb), entry);
	g_signal_connect(buffer, "changed", G_CALLBACK(buffer_changed_cb), entry);

	/* libspelling: suggestions in the context menu, the shared pref */
	entry->spelling = spelling_text_buffer_adapter_new(buffer, spelling_checker_get_default());
	gtk_text_view_set_extra_menu(GTK_TEXT_VIEW(entry),
		spelling_text_buffer_adapter_get_menu_model(entry->spelling));
	gtk_widget_insert_action_group(GTK_WIDGET(entry), "spelling",
	                               G_ACTION_GROUP(entry->spelling));
	spelling_text_buffer_adapter_set_enabled(entry->spelling,
		pref_bool(PIDGIN_PREFS_ROOT "/conversations/spellcheck", TRUE));

	keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed_cb), entry);
	gtk_widget_add_controller(GTK_WIDGET(entry), keys);
}

GtkWidget *
pidgin_compose_entry_new(void)
{
	return g_object_new(PIDGIN_TYPE_COMPOSE_ENTRY, NULL);
}
