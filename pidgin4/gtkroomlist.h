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
#ifndef _PIDGINROOMLIST_H_
#define _PIDGINROOMLIST_H_

#include "pidgin.h"
#include "account.h"
#include "roomlist.h"

/* The room list (M5). pidgin_roomlist_init() sets the PurpleRoomlistUiOps. */

void pidgin_roomlist_init(void);
void pidgin_roomlist_uninit(void);

/** TRUE if a connected account's prpl has a room list. */
gboolean pidgin_roomlist_is_showable(void);

void pidgin_roomlist_dialog_show(void);
/** Opens the room list with @account selected (may be NULL). */
void pidgin_roomlist_dialog_show_with_account(PurpleAccount *account);

/** PIDGIN4_WINDOWS_SELFTEST: opens and closes the dialog (with -n no
 * account is connected, so it only checks the empty state). */
void pidgin_roomlist_selftest(void);

#endif /* _PIDGINROOMLIST_H_ */
