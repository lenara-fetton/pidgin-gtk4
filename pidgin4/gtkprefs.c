/*
 * pidgin
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
#include "pidgin-internal.h"
#include "pidgin.h"

#include "prefs.h"

#include "gtkconv.h"
#include "gtkprefs.h"

/*
 * TODO(M5): the preferences window (pidgin/gtkprefs.c).
 *
 * Profile contract: pidgin4 never adds, renames or removes keys under
 * /pidgin (the GTK 2 UI owns them; they are still loaded from and saved to
 * prefs.xml untouched). Everything pidgin4-specific lives under /pidgin4.
 * Pidgin 2 keeps unknown prefs through a load/save cycle, so this subtree
 * survives it.
 */
void
pidgin_prefs_init(void)
{
	purple_prefs_add_none(PIDGIN4_PREFS_ROOT);

	/* Plugins: pidgin4's own list. /pidgin/plugins/loaded holds GTK 2
	 * plugin paths that must never be loaded into this process. */
	purple_prefs_add_none(PIDGIN4_PREFS_ROOT "/plugins");
	purple_prefs_add_path_list(PIDGIN4_PREFS_ROOT "/plugins/loaded", NULL);

	/* The debug window (gtkdebug.c adds the rest). */
	purple_prefs_add_none(PIDGIN4_PREFS_ROOT "/debug");

	/* Account manager window. */
	purple_prefs_add_none(PIDGIN4_PREFS_ROOT "/accounts");
	purple_prefs_add_none(PIDGIN4_PREFS_ROOT "/accounts/dialog");
	purple_prefs_add_int(PIDGIN4_PREFS_ROOT "/accounts/dialog/width", 560);
	purple_prefs_add_int(PIDGIN4_PREFS_ROOT "/accounts/dialog/height", 380);

	/* Per-profile marker: which pidgin4 last used this profile. Written on
	 * every start; handy when reading a profile that both UIs share. */
	purple_prefs_add_string(PIDGIN4_PREFS_ROOT "/last_version", "");
	purple_prefs_set_string(PIDGIN4_PREFS_ROOT "/last_version", VERSION);

	/* M4b: /pidgin/conversations (Pidgin 2's) and /pidgin4/conversations. */
	pidgin_conversations_prefs_init();
}
