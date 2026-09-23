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
#ifndef _PIDGINPRIVACY_H_
#define _PIDGINPRIVACY_H_

#include "pidgin.h"
#include "account.h"
#include "privacy.h"

/* Privacy (M5): the allow/block lists per account. */

void pidgin_privacy_init(void);
void pidgin_privacy_dialog_show(void);
void pidgin_privacy_dialog_hide(void);

/** Asks for confirmation (or a name, if @name is NULL) and adds it to the
 * account's allow (permit) or block (deny) list. */
void pidgin_request_add_permit(PurpleAccount *account, const char *name);
void pidgin_request_add_block(PurpleAccount *account, const char *name);

PurplePrivacyUiOps *pidgin_privacy_get_ui_ops(void);

/** PIDGIN4_WINDOWS_SELFTEST: opens the dialog, cycles accounts and
 * policies without applying anything, closes it. */
void pidgin_privacy_selftest(void);

/** For the round-2 selftest: the open dialog's "account-menu",
 * "type-menu" or "modes-note" widget, or NULL. */
GtkWidget *pidgin_privacy_dialog_get_widget_for_tests(const char *name);

#endif /* _PIDGINPRIVACY_H_ */
