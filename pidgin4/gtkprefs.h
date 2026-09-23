/**
 * @file gtkprefs.h GTK 4 Preferences
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
#ifndef _PIDGINPREFS_H_
#define _PIDGINPREFS_H_

#include "prefs.h"

/**
 * Registers the pidgin4 prefs (the /pidgin4 subtree). Called from
 * purple_core_init() through the core UI ops, after prefs.xml is loaded,
 * so only missing keys get their defaults.
 */
void pidgin_prefs_init(void);

#endif /* _PIDGINPREFS_H_ */
