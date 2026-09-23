/**
 * @file gtkdialogs.h GTK 4 Dialogs
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
#ifndef _PIDGINDIALOGS_H_
#define _PIDGINDIALOGS_H_

#include "pidgin.h"

#include "account.h"
#include "conversation.h"

/*
 * The pidgin4 subset of pidgin/gtkdialogs.h. About, build info and the
 * credits are in pidginabout.c (M5).
 */
void pidgin_dialogs_destroy_all(void);
void pidgin_dialogs_about(void);

/* Buddy list dialogs (M3). They are purple_request_*() dialogs, so they
 * close with their node (purple_request_close_with_handle()). */
void pidgin_dialogs_im(void);
/** TODO(M4): opens the conversation; until then it only creates it. */
void pidgin_dialogs_im_with_user(PurpleAccount *account, const char *username);
void pidgin_dialogs_info(void);
/** Asks for a user, then opens the log viewer (gtklog.c). */
void pidgin_dialogs_log(void);
void pidgin_dialogs_alias_contact(PurpleContact *contact);
void pidgin_dialogs_alias_buddy(PurpleBuddy *buddy);
void pidgin_dialogs_alias_chat(PurpleChat *chat);
void pidgin_dialogs_rename_group(PurpleGroup *group);
void pidgin_dialogs_remove_buddy(PurpleBuddy *buddy);
void pidgin_dialogs_remove_group(PurpleGroup *group);
void pidgin_dialogs_remove_chat(PurpleChat *chat);
void pidgin_dialogs_remove_contact(PurpleContact *contact);
void pidgin_dialogs_merge_groups(PurpleGroup *source, const char *new_name);

#endif /* _PIDGINDIALOGS_H_ */
