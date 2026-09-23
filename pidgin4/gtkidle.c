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

#include "gtkidle.h"

/*
 * TODO(M6): system idle time from Wayland ext-idle-notify-v1 (Sway) and
 * org.gnome.Mutter.IdleMonitor (GNOME), in pidginidle-wayland.c.
 *
 * Until then there are no idle UI ops (the GTK 2 version used XScreenSaver,
 * which does not exist on Wayland). libpurple's idle.c then uses its own
 * "purple" idle method: with /purple/away/idle_reporting = "system" (the
 * shared pref, which must not be changed) and no UI ops it does not report
 * idle time to servers, but auto-away still works from the time of the
 * last message sent (purple_idle_touch()).
 */
PurpleIdleUiOps *
pidgin_idle_get_ui_ops(void)
{
	return NULL;
}
