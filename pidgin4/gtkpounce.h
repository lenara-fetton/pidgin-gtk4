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
#ifndef _PIDGINPOUNCE_H_
#define _PIDGINPOUNCE_H_

#include "pidgin.h"
#include "account.h"
#include "pounce.h"

/*
 * Buddy pounces (M5): the pounce manager, the pounce editor and the pounce
 * handler with its five actions (open-window, popup-notify, send-message,
 * execute-command, play-sound). pidgin_pounces_init() registers the
 * handler and the actions before pounces.xml is loaded (see
 * doc/PIDGIN-UPGRADE.md, M2 "Exceptions": without the registration the
 * actions' <param>s are dropped).
 */

/**
 * Shows the pounce editor. @account and @name prefill the target (either
 * may be NULL); @cur_pounce, if not NULL, is edited instead.
 */
void pidgin_pounce_editor_show(PurpleAccount *account, const char *name,
                               PurplePounce *cur_pounce);

/** Shows (or raises) the pounce manager window. */
void pidgin_pounces_manager_show(void);
void pidgin_pounces_manager_hide(void);

void *pidgin_pounces_get_handle(void);
void pidgin_pounces_init(void);
void pidgin_pounces_uninit(void);

/** PIDGIN4_WINDOWS_SELFTEST: opens the manager and the editor (new and
 * for each existing pounce) and closes them without saving. */
void pidgin_pounces_selftest(void);

#endif /* _PIDGINPOUNCE_H_ */
