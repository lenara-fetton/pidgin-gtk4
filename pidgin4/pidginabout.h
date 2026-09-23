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
#ifndef _PIDGINABOUT_H_
#define _PIDGINABOUT_H_

#include "pidgin.h"

/*
 * About (M5): version, libpurple/GTK/GLib versions, build information and
 * the credits from Pidgin 2 (a custom window with PidginRichLabels).
 */

void pidgin_about_show(void);

/** The build information as HTML (for the About window and bug reports). */
char *pidgin_about_get_build_info_html(void);

/** PIDGIN4_WINDOWS_SELFTEST: opens every About page and closes it. */
void pidgin_about_selftest(void);

#endif /* _PIDGINABOUT_H_ */
