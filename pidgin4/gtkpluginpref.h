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
#ifndef _PIDGINPLUGINPREF_H_
#define _PIDGINPLUGINPREF_H_

#include "pidgin.h"
#include "pluginpref.h"

/*
 * PurplePluginPrefFrame -> GTK 4 widgets (M5). Used by the plugins dialog
 * for plugins with prefs_info, and by the ported plugins (M7).
 *
 * Every pref type is supported: bool (GtkCheckButton), int (GtkSpinButton
 * within the pref's bounds), string (GtkEntry, masked or multiline with a
 * PidginComposeEntry for PURPLE_STRING_FORMAT_TYPE_HTML), choice
 * (GtkDropDown), path/path list are read-only labels, labels are bold
 * section titles. Widgets write the prefs immediately.
 */

/**
 * Builds a vertical box for @frame. The frame is NOT destroyed (the
 * caller owns it). Returns NULL if @frame is NULL.
 */
GtkWidget *pidgin_plugin_pref_frame_to_widget(PurplePluginPrefFrame *frame);

/** Pidgin 2's name for the same thing. */
#define pidgin_plugin_pref_create_frame pidgin_plugin_pref_frame_to_widget

#endif /* _PIDGINPLUGINPREF_H_ */
