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
#ifndef _PIDGINFT_H_
#define _PIDGINFT_H_

#include "pidgin.h"
#include "ft.h"

/*
 * File transfers (M5): the PurpleXferUiOps and the File Transfers window.
 * pidgin_xfers_init() registers the /pidgin4/filetransfer prefs;
 * pidgin_xfers_get_ui_ops() is set in gtkmain.c.
 */

void pidgin_xfers_init(void);
void pidgin_xfers_uninit(void);
PurpleXferUiOps *pidgin_xfers_get_ui_ops(void);

/** Shows (or raises) the File Transfers window. */
void pidgin_xfer_dialog_show(void);
void pidgin_xfer_dialog_hide(void);

/** PIDGIN4_WINDOWS_SELFTEST: opens the window with a fake finished and a
 * fake running transfer row (no network), exercises Clear, closes. */
void pidgin_xfers_selftest(void);

#endif /* _PIDGINFT_H_ */
