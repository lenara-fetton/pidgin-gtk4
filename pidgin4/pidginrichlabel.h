/**
 * @file pidginrichlabel.h Read-only rich text (purple HTML) widget
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

/*
 * PidginRichLabel shows a PidginMarkupResult: as a selectable GtkLabel with
 * Pango markup when it is only text (cheap, the common case), or as a
 * read-only GtkTextView when it has images, smileys, rules or quotes to
 * lay out. Links go through the PidginMarkup scheme registry; Discord
 * spoilers reveal on click; remote images load through PidginImageLoader
 * (allowlisted hosts only, else the alt text stays as a link).
 *
 * Used for message bodies (PidginMessageView), notify, user info and
 * about text.
 */
#ifndef _PIDGINRICHLABEL_H_
#define _PIDGINRICHLABEL_H_

#include <gtk/gtk.h>

#include "pidginmarkup.h"

G_BEGIN_DECLS

#define PIDGIN_TYPE_RICH_LABEL (pidgin_rich_label_get_type())
G_DECLARE_FINAL_TYPE(PidginRichLabel, pidgin_rich_label, PIDGIN, RICH_LABEL, GtkWidget)

GtkWidget *pidgin_rich_label_new(void);

/** Parses @html with @options (may be NULL) and shows it. */
void pidgin_rich_label_set_html(PidginRichLabel *label, const char *html,
                                const PidginMarkupOptions *options);

/** Shows an already parsed result (takes a reference). */
void pidgin_rich_label_set_result(PidginRichLabel *label,
                                  PidginMarkupResult *result);

/** Shows plain text. */
void pidgin_rich_label_set_text(PidginRichLabel *label, const char *text);

/** The shown text, plain. */
const char *pidgin_rich_label_get_text(PidginRichLabel *label);

/**
 * Highlights every case-insensitive occurrence of @needle (NULL or ""
 * for none), e.g. find in a conversation.
 */
void pidgin_rich_label_set_highlight(PidginRichLabel *label, const char *needle);

/**
 * Always use the text view (long text such as user info, or when the
 * caller wants caret selection across paragraphs).
 */
void pidgin_rich_label_set_force_text_view(PidginRichLabel *label, gboolean force);

/** Extra items for the context menu (e.g. the message view's actions). */
void pidgin_rich_label_set_extra_menu(PidginRichLabel *label, GMenuModel *menu);

/** Maximum size of inline images (default 320x240); smileys are never scaled. */
void pidgin_rich_label_set_max_image_size(PidginRichLabel *label, int width, int height);

/**
 * TRUE while the text view is used (for tests and callers that need the
 * GtkTextView, e.g. to add a highlight tag).
 */
gboolean pidgin_rich_label_is_text_view(PidginRichLabel *label);

/** The inner GtkLabel or GtkTextView. */
GtkWidget *pidgin_rich_label_get_inner(PidginRichLabel *label);

/**
 * The first image under (x, y) in the widget's coordinates, or with
 * @x < 0 the first image at all: its texture (a new reference) and, for
 * imgstore images, the imgstore id through @id (else 0). NULL if none.
 */
GdkTexture *pidgin_rich_label_get_image(PidginRichLabel *label, double x, double y,
                                        int *id);

/**
 * The link (URI) under (x, y), or NULL. Only the text view knows; in
 * label mode GtkLabel's own context menu offers the link items.
 */
const char *pidgin_rich_label_get_link_at(PidginRichLabel *label, double x, double y);

G_END_DECLS

#endif /* _PIDGINRICHLABEL_H_ */
