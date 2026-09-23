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
 * The M2 subset of pidgin/gtkdialogs.h. The IM/info/alias/remove dialogs
 * come with the buddy list (TODO(M3)); build info, developers and
 * translators with the other windows (TODO(M5)).
 */
void pidgin_dialogs_destroy_all(void);
void pidgin_dialogs_about(void);

#endif /* _PIDGINDIALOGS_H_ */
