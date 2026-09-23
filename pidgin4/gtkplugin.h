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
#ifndef _PIDGINPLUGIN_H_
#define _PIDGINPLUGIN_H_

#include "pidgin.h"
#include "plugin.h"

/*
 * The plugins dialog (M5) and pidgin4's plugin list.
 *
 * pidgin4 keeps its own list of loaded plugins in /pidgin4/plugins/loaded
 * (profile contract, rule 3); /pidgin/plugins/loaded is never read or
 * written. Plugins linked against another GTK (GTK 2's libgtk-x11-2.0,
 * GTK 3) are never loaded: see pidgin_plugin_file_is_foreign_toolkit().
 */

typedef struct _PidginPluginUiInfo PidginPluginUiInfo;

/** A pidgin4 UI plugin's ui_info (same layout as Pidgin 2's). */
struct _PidginPluginUiInfo
{
	GtkWidget *(*get_config_frame)(PurplePlugin *plugin);

	int page_num;                                         /**< Reserved */

	/* padding */
	void (*_pidgin_reserved1)(void);
	void (*_pidgin_reserved2)(void);
	void (*_pidgin_reserved3)(void);
	void (*_pidgin_reserved4)(void);
};

#define PIDGIN_PLUGIN_TYPE PIDGIN_UI

#define PIDGIN_IS_PIDGIN_PLUGIN(plugin) \
	((plugin)->info != NULL && (plugin)->info->ui_info != NULL && \
	 purple_strequal((plugin)->info->ui_requirement, PIDGIN_PLUGIN_TYPE))

#define PIDGIN_PLUGIN_UI_INFO(plugin) \
	((PidginPluginUiInfo *)(plugin)->info->ui_info)

/**
 * The configuration widget of a loaded plugin: the UI plugin's
 * get_config_frame, or its PurplePluginUiInfo pref frame converted with
 * pidgin_plugin_pref_frame_to_widget(). NULL if it has none.
 */
GtkWidget *pidgin_plugin_get_config_frame(PurplePlugin *plugin);

/** Saves the loaded plugins to /pidgin4/plugins/loaded. */
void pidgin_plugins_save(void);

/**
 * Loads the plugins listed in @key (/pidgin4/plugins/loaded), like
 * purple_plugins_load_saved(), but never probes a file that
 * pidgin_plugin_file_is_foreign_toolkit() rejects; such entries are
 * dropped from the list.
 */
void pidgin_plugins_load_saved(const char *key);

/**
 * TRUE if the shared object at @path directly needs another GUI toolkit
 * (libgtk-x11-2.0, libgdk-x11-2.0, libgtk-3, libgdk-3, libgtk-win32...),
 * read from its ELF DT_NEEDED entries without loading it. Loading such a
 * plugin into a GTK 4 process would abort. @lib (may be NULL) receives
 * the offending library name (g_free() it).
 */
gboolean pidgin_plugin_file_is_foreign_toolkit(const char *path, char **lib);

/** Shows (or raises) the plugins dialog. */
void pidgin_plugin_dialog_show(void);

void pidgin_plugins_init(void);
void pidgin_plugins_uninit(void);

/**
 * PIDGIN4_WINDOWS_SELFTEST: opens the dialog, toggles the "psychic" core
 * plugin on and off (checking that /pidgin4/plugins/loaded changed and
 * /pidgin/plugins/loaded did not), opens every loaded plugin's config
 * frame, closes.
 */
void pidgin_plugins_selftest(void);

#endif /* _PIDGINPLUGIN_H_ */
