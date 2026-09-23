/**
 * @file pidgincomposeentry.h The message entry (GtkIMHtml replacement)
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
 * PidginComposeEntry is a GtkSourceView (GtkSourceView 5: a GtkSourceBuffer
 * with built-in undo) with libspelling, the PidginMarkup formatting tags and
 * GtkIMHtml's entry behaviour:
 *
 *  - formatting limited by a PidginFormatCaps set (setup() derives it from
 *    PurpleConnectionFlags like gtk_imhtml_setup_entry(); XMPP gets
 *    PIDGIN_FORMAT_STYLING_ALL and serializes to XEP-0393);
 *  - Enter sends (Shift/Ctrl+Enter, or set_return_inserts_newline(), make a
 *    newline), Ctrl+Up/Down walk the sent history, Up in an empty entry
 *    asks to edit the last message, Ctrl+B/I/U toggle formatting;
 *  - typing notifications from buffer changes (5 s pause = TYPED).
 *
 * Signals:
 *   "message-send"        gboolean (PidginComposeEntry *entry, const char *markup)
 *       Enter was pressed (or pidgin_compose_entry_send() called; the
 *       sendbutton plugin does that). @markup is the HTML (or, with
 *       PIDGIN_FORMAT_STYLING, the escaped XEP-0393 text with <br>
 *       newlines). Return TRUE if the message was sent: the entry is then
 *       cleared and the markup added to the history.
 *   "pre-send"            gboolean (PidginComposeEntry *entry)
 *       (M7) Emitted by pidgin_compose_entry_send() before the markup is
 *       taken, so plugins (spellchk) can rewrite the buffer. Return TRUE
 *       to hold the message back (the buffer is kept, nothing is sent).
 *   "typing-changed"      void (PidginComposeEntry *entry, PurpleTypingState state)
 *   "edit-last-requested" void (PidginComposeEntry *entry)
 *   "format-changed"      void (PidginComposeEntry *entry)
 *       The formatting at the cursor changed (for toolbars).
 *   "paste-image"         gboolean (PidginComposeEntry *entry, GdkTexture *texture)
 *       A paste chose the clipboard's image (only with
 *       set_paste_images(TRUE); see the paste rule there). Return TRUE
 *       if it was taken; otherwise the clipboard's text is pasted.
 */
#ifndef _PIDGINCOMPOSEENTRY_H_
#define _PIDGINCOMPOSEENTRY_H_

#include <gtksourceview/gtksource.h>

#include "connection.h"
#include "conversation.h"

#include "pidginmarkup.h"

G_BEGIN_DECLS

#define PIDGIN_TYPE_COMPOSE_ENTRY (pidgin_compose_entry_get_type())
G_DECLARE_FINAL_TYPE(PidginComposeEntry, pidgin_compose_entry, PIDGIN, COMPOSE_ENTRY,
                     GtkSourceView)

GtkWidget *pidgin_compose_entry_new(void);

/**
 * gtk_imhtml_setup_entry(): the capabilities from @features and, for HTML
 * protocols, the default formatting from the (shared) Pidgin 2 prefs
 * send_bold/send_italic/send_underline/font_face/font_size/fgcolor/bgcolor.
 */
void pidgin_compose_entry_setup(PidginComposeEntry *entry, PurpleConnectionFlags features);

/** Sets the formatting capabilities directly (drops unsupported formatting). */
void pidgin_compose_entry_set_caps(PidginComposeEntry *entry, PidginFormatCaps caps);
PidginFormatCaps pidgin_compose_entry_get_caps(PidginComposeEntry *entry);

/**
 * Serialization flags for get_markup(): PIDGIN_MARKUP_USE_POINTSIZE for
 * OPT_PROTO_USE_POINTSIZE prpls (WBFO comes from the caps).
 */
void pidgin_compose_entry_set_markup_flags(PidginComposeEntry *entry, PidginMarkupFlags flags);

/** The smiley category (the prpl's name) for the smiley picker and paste. */
void pidgin_compose_entry_set_smiley_category(PidginComposeEntry *entry, const char *sml);

void pidgin_compose_entry_set_return_inserts_newline(PidginComposeEntry *entry, gboolean newline);

/**
 * Whether pastes may choose the clipboard's image ("paste-image"). The
 * rule: the image is chosen if the clipboard has no text, or only empty
 * or blank text; otherwise the text is pasted, as GtkTextView would. The
 * context menu then also shows "Paste Image" while the clipboard holds an
 * image (GtkTextView's own Paste is disabled without text). Default FALSE.
 */
void pidgin_compose_entry_set_paste_images(PidginComposeEntry *entry, gboolean paste);
gboolean pidgin_compose_entry_get_paste_images(PidginComposeEntry *entry);

/** Spell checking; defaults to the shared /pidgin/conversations/spellcheck. */
void pidgin_compose_entry_set_spellcheck(PidginComposeEntry *entry, gboolean enabled);
gboolean pidgin_compose_entry_get_spellcheck(PidginComposeEntry *entry);

/** The content as HTML, or as escaped XEP-0393 text (see "message-send"). */
char *pidgin_compose_entry_get_markup(PidginComposeEntry *entry);
/** The content as plain text (smileys as their shortcuts). */
char *pidgin_compose_entry_get_text(PidginComposeEntry *entry);
/** Replaces the content with @html (e.g. to edit the last message). */
void pidgin_compose_entry_set_markup(PidginComposeEntry *entry, const char *html);
/** Empties the entry, keeping the formatting for new text. */
void pidgin_compose_entry_clear(PidginComposeEntry *entry);
gboolean pidgin_compose_entry_is_empty(PidginComposeEntry *entry);

/**
 * Emits "message-send" with the current markup; clears the entry and
 * records the history if a handler returned TRUE. Returns that.
 */
gboolean pidgin_compose_entry_send(PidginComposeEntry *entry);

/** Inserts a smiley (theme, or custom smiley if the shortcut is one). */
void pidgin_compose_entry_insert_smiley(PidginComposeEntry *entry, const char *shortcut);
/** Inserts a stored image (takes an imgstore reference while shown). */
void pidgin_compose_entry_insert_image(PidginComposeEntry *entry, int imgstore_id);
/** Inserts a link: @description (or @url) linked to @url; with a selection
 * and no @description, links the selection. */
void pidgin_compose_entry_insert_link(PidginComposeEntry *entry, const char *url,
                                      const char *description);

/* Formatting (on the selection, or for new text; the whole buffer with WBFO) */
void pidgin_compose_entry_toggle_bold(PidginComposeEntry *entry);
void pidgin_compose_entry_toggle_italic(PidginComposeEntry *entry);
void pidgin_compose_entry_toggle_underline(PidginComposeEntry *entry);
void pidgin_compose_entry_toggle_strike(PidginComposeEntry *entry);
void pidgin_compose_entry_toggle_code(PidginComposeEntry *entry);
void pidgin_compose_entry_grow_font(PidginComposeEntry *entry);
void pidgin_compose_entry_shrink_font(PidginComposeEntry *entry);
/** NULL clears. */
void pidgin_compose_entry_set_font_face(PidginComposeEntry *entry, const char *face);
void pidgin_compose_entry_set_forecolor(PidginComposeEntry *entry, const GdkRGBA *color);
void pidgin_compose_entry_set_backcolor(PidginComposeEntry *entry, const GdkRGBA *color);
void pidgin_compose_entry_clear_formatting(PidginComposeEntry *entry);

/**
 * Whether @format (one of BOLD, ITALIC, UNDERLINE, STRIKE, CODE) is on at
 * the cursor / for new text.
 */
gboolean pidgin_compose_entry_get_format(PidginComposeEntry *entry, PidginFormatCaps format);
/** The HTML font size (1..7) at the cursor. */
int pidgin_compose_entry_get_font_size(PidginComposeEntry *entry);

/** Sent-message history (newest first), for tests. Transfer none. */
GList *pidgin_compose_entry_get_history(PidginComposeEntry *entry);
void pidgin_compose_entry_history_up(PidginComposeEntry *entry);
void pidgin_compose_entry_history_down(PidginComposeEntry *entry);

G_END_DECLS

#endif /* _PIDGINCOMPOSEENTRY_H_ */
