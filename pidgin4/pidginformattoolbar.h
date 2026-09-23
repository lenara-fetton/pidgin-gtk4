/**
 * @file pidginformattoolbar.h Formatting toolbar for PidginComposeEntry
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
 * Replaces gtkimhtmltoolbar.c: bold, italic, underline, strike, code,
 * smaller/larger, font (GtkFontDialog), text and background colour
 * (GtkColorDialog), link, image (GtkFileDialog -> imgstore), a smiley
 * picker popover (the smiley theme plus custom smileys) and reset. Only
 * the buttons the entry's PidginFormatCaps allow are shown; the toggles
 * follow the formatting at the cursor.
 */
#ifndef _PIDGINFORMATTOOLBAR_H_
#define _PIDGINFORMATTOOLBAR_H_

#include <gtk/gtk.h>

#include "pidgincomposeentry.h"

G_BEGIN_DECLS

#define PIDGIN_TYPE_FORMAT_TOOLBAR (pidgin_format_toolbar_get_type())
G_DECLARE_FINAL_TYPE(PidginFormatToolbar, pidgin_format_toolbar, PIDGIN, FORMAT_TOOLBAR,
                     GtkWidget)

GtkWidget *pidgin_format_toolbar_new(PidginComposeEntry *entry);

void pidgin_format_toolbar_set_entry(PidginFormatToolbar *toolbar, PidginComposeEntry *entry);
PidginComposeEntry *pidgin_format_toolbar_get_entry(PidginFormatToolbar *toolbar);

/** Re-reads the entry's capabilities (call after pidgin_compose_entry_set_caps()). */
void pidgin_format_toolbar_update(PidginFormatToolbar *toolbar);

/** The smiley picker's content (built when first shown), for tests. */
GtkWidget *pidgin_format_toolbar_get_smiley_grid(PidginFormatToolbar *toolbar);

G_END_DECLS

#endif /* _PIDGINFORMATTOOLBAR_H_ */
