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

/* PLACEHOLDER (M5 scaffolding): replaced by the plugins dialog */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "debug.h"
#include "gtkplugin.h"


GtkWidget *pidgin_plugin_get_config_frame(PurplePlugin *plugin) { return NULL; }
void pidgin_plugins_save(void) { purple_plugins_save_loaded(PIDGIN4_PREFS_ROOT "/plugins/loaded"); }
void pidgin_plugins_load_saved(const char *key) { purple_plugins_load_saved(key); }
void pidgin_plugin_dialog_show(void) { purple_debug_info("gtkplugin", "placeholder\n"); }
void pidgin_plugins_init(void) { }
void pidgin_plugins_uninit(void) { }
void pidgin_plugins_selftest(void) { }
