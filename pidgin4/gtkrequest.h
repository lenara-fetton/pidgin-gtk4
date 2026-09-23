/**
 * @file gtkrequest.h GTK+ Request API
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
#ifndef _PIDGINREQUEST_H_
#define _PIDGINREQUEST_H_

#include "request.h"

/**
 * Returns the UI operations structure for GTK+ request functions.
 *
 * @return The GTK+ UI request operations structure.
 */
PurpleRequestUiOps *pidgin_request_get_ui_ops(void);

/**
 * Developer aid: opens one request of every kind (input, masked input,
 * multi-line input, choice, action, action with icon, and a fields request
 * with every field type) so the dialogs can be checked by hand or run
 * headless. The callbacks only log what they receive (debug category
 * "gtkrequest-selftest"). Not used in normal operation; gtkmain.c calls it
 * when PIDGIN4_REQUEST_SELFTEST is set in the environment. With
 * PIDGIN4_REQUEST_FILE_SELFTEST it instead accepts a file-transfer-style
 * action whose callback opens a save dialog, checks the dialog is not
 * parented to the closing action window, and quits (status 1 on failure).
 */
void pidgin_request_selftest(void);

#endif /* _PIDGINREQUEST_H_ */
