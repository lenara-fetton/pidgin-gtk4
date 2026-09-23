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
#ifndef _PIDGINTHEMES_H_
#define _PIDGINTHEMES_H_

#include "pidgin.h"

/*
 * CSS (M5): the built-in style.css (loaded at startup in gtkmain.c) and
 * the user's <profile>/pidgin4/gtk4.css, loaded after it at user priority
 * and reloaded when the file changes (GFileMonitor).
 */

/** Starts loading and watching gtk4.css. Needs the display and the
 * profile directory. */
void pidgin_themes_init(void);
void pidgin_themes_uninit(void);

/** <profile>/pidgin4/gtk4.css (owned by pidgin4; the file may not exist). */
const char *pidgin_themes_get_user_css_path(void);

/** Reloads gtk4.css now. */
void pidgin_themes_reload_user_css(void);

/** The last parse error of gtk4.css, or NULL. */
const char *pidgin_themes_get_user_css_error(void);

#endif /* _PIDGINTHEMES_H_ */
