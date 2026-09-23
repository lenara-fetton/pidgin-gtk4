/**
 * @file pidgin.h UI definitions and includes
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
#ifndef _PIDGIN_H_
#define _PIDGIN_H_

#include <gtk/gtk.h>

/**
 * Our UI's identifier.
 *
 * It stays "gtk-gaim", the same as Pidgin 2, so per-UI account state
 * (enabled, buddy icon) is shared with it. See the profile compatibility
 * contract in doc/PIDGIN-UPGRADE.md.
 */
#define PIDGIN_UI "gtk-gaim"

/**
 * The ui_requirement of pidgin4 UI plugins (M7). Pidgin 2 defines it in
 * gtkplugin.h with the same value; the definitions are identical, so both
 * headers may be included.
 */
#define PIDGIN_PLUGIN_TYPE PIDGIN_UI

/**
 * The GTK 2 UI's pref subtree. pidgin4 only reads keys whose meaning is the
 * same in both UIs from here; it never writes GTK 2-specific keys.
 */
#define PIDGIN_PREFS_ROOT "/pidgin"

/** pidgin4-only prefs (plugin list, debug window, window geometry, ...). */
#define PIDGIN4_PREFS_ROOT "/pidgin4"

/** The profile subdirectory for every new pidgin4 file. */
#define PIDGIN4_PROFILE_SUBDIR "pidgin4"

/* Translators may want to transliterate the name.
 It is not to be translated. */
#define PIDGIN_NAME _("Pidgin")

#define PIDGIN_ALERT_TITLE ""

/*
 * Spacings between components, as defined by the
 * GNOME Human Interface Guidelines.
 */
#define PIDGIN_HIG_CAT_SPACE     18
#define PIDGIN_HIG_BORDER        12
#define PIDGIN_HIG_BOX_SPACE      6

#define PIDGIN_INVISIBLE_CHAR (gunichar)0x25cf

/**
 * Returns the GtkApplication. Valid from startup until shutdown.
 */
GtkApplication *pidgin_application_get(void);

/**
 * Returns the profile's pidgin4 directory, <profile>/pidgin4, creating it
 * if needed. The string is owned by pidgin4.
 */
const char *pidgin_user_dir(void);

/**
 * Saves everything and quits: purple_core_quit(), then g_application_quit().
 * Safe to call more than once.
 */
void pidgin_application_quit(void);

/** The process exit status once the application quits (selftests). */
void pidgin_application_set_exit_status(int status);

#endif /* _PIDGIN_H_ */
