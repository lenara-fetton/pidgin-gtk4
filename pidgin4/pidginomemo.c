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

/* PLACEHOLDER (M5 scaffolding): replaced by the OMEMO fingerprints window */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "debug.h"
#include "plugin.h"
#include "pidginomemo.h"


gboolean pidgin_omemo_is_available(void) { return purple_plugins_find_with_id("core-omemo") != NULL; }
void pidgin_omemo_show_fingerprints(PurpleAccount *account, const char *jid) { purple_debug_info("pidginomemo", "placeholder\n"); }
void pidgin_omemo_init(void) { }
void pidgin_omemo_uninit(void) { }
void pidgin_omemo_selftest(void) { }
