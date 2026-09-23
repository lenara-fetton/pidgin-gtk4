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
#ifndef _PIDGINCOMPLETION_H_
#define _PIDGINCOMPLETION_H_

#include "pidgin.h"
#include "account.h"

/*
 * Buddy name completion for entries (M5), replacing Pidgin 2's
 * pidgin_setup_screenname_autocomplete() (GtkEntryCompletion is
 * deprecated in GTK 4): a popover under the entry lists matching buddy
 * names and aliases; Up/Down/Enter/Escape and clicks pick one.
 */

/**
 * Attaches completion to @entry (a GtkEntry or GtkText). Buddies of every
 * account are offered (with @all_accounts FALSE only those of connected
 * accounts, like Pidgin 2); picking one selects its account in
 * @account_dropdown (a pidgin_account_dropdown_new(), may be NULL).
 */
void pidgin_buddy_completion_attach(GtkWidget *entry,
                                    GtkWidget *account_dropdown,
                                    gboolean all_accounts);

/**
 * The same for a purple_request_fields() dialog (gtkrequest.c): completes
 * on @entry and sets the request's account field @account_field (a
 * PurpleRequestField *, may be NULL) when a buddy is picked.
 */
void pidgin_buddy_completion_attach_to_field(GtkWidget *entry,
                                             gpointer account_field,
                                             gboolean all_accounts);

#endif /* _PIDGINCOMPLETION_H_ */
