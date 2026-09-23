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
#ifndef _PIDGINSAVEDSTATUSES_H_
#define _PIDGINSAVEDSTATUSES_H_

#include "pidgin.h"
#include "savedstatuses.h"

/*
 * Saved statuses (M5): the manager window (title, type, message; Use,
 * Modify, Duplicate, Delete) and the status editor (title, type, message,
 * per-account substatuses).
 */

/** Shows (or raises) the saved statuses window. */
void pidgin_status_window_show(void);
void pidgin_status_window_hide(void);

/**
 * Shows the status editor. With @edit TRUE, @status is modified; with
 * @edit FALSE and @status not NULL, the editor starts as a copy of it
 * (a transient status gets saved under a title); with both FALSE/NULL it
 * creates a new status.
 */
void pidgin_status_editor_show(gboolean edit, PurpleSavedStatus *status);

void *pidgin_status_get_handle(void);
void pidgin_status_init(void);
void pidgin_status_uninit(void);

/** PIDGIN4_WINDOWS_SELFTEST: opens the window and an editor per saved
 * status (and a new one), and closes them without saving. */
void pidgin_status_selftest(void);

#endif /* _PIDGINSAVEDSTATUSES_H_ */
