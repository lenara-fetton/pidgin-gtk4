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

/* PLACEHOLDER (M5 scaffolding): replaced by the custom smiley manager */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "debug.h"
#include "gtksmiley.h"


void pidgin_smileys_init(void) { }
void pidgin_smileys_uninit(void) { }
void pidgin_smiley_manager_show(void) { purple_debug_info("gtksmiley", "placeholder\n"); }
void pidgin_smiley_edit(GtkWindow *parent, PurpleSmiley *smiley) { }
void pidgin_smiley_add_from_image(GtkWindow *parent, gconstpointer data, gsize len, const char *shortcut) { }
void pidgin_smileys_selftest(void) { }
