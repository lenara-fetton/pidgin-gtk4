/*
 * pidgin4
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
#ifndef _PIDGINSMILEY_H_
#define _PIDGINSMILEY_H_

#include "pidgin.h"
#include "smiley.h"

/*
 * Custom smileys (M5): the manager (purple_smileys_get_all()) and the
 * add/edit dialog. (PidginSmiley is the theme smiley type of
 * pidginsmileytheme.h; nothing here uses that name.)
 */

void pidgin_smileys_init(void);
void pidgin_smileys_uninit(void);

/** Shows (or raises) the custom smiley manager. */
void pidgin_smiley_manager_show(void);

/** Opens the editor for @smiley, or for a new smiley if NULL. */
void pidgin_smiley_edit(GtkWindow *parent, PurpleSmiley *smiley);

/**
 * "Add custom smiley" from a conversation (for M4b): opens the editor for
 * a new smiley with the image @data (@len bytes, copied) prefilled and
 * @shortcut (may be NULL) in the shortcut entry.
 */
void pidgin_smiley_add_from_image(GtkWindow *parent, gconstpointer data,
                                  gsize len, const char *shortcut);

/** PIDGIN4_WINDOWS_SELFTEST: opens the manager and an editor, closes. */
void pidgin_smileys_selftest(void);

#endif /* _PIDGINSMILEY_H_ */
