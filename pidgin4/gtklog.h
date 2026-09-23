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
#ifndef _PIDGINLOG_H_
#define _PIDGINLOG_H_

#include "pidgin.h"
#include "account.h"
#include "blist.h"
#include "log.h"

/*
 * The log viewer (M5): logs of a buddy, contact or chat, and the system
 * log. Read-only: nothing here writes to logs/ except "Delete", which
 * asks first.
 */

void pidgin_log_show(PurpleLogType type, const char *buddyname,
                     PurpleAccount *account);
void pidgin_log_show_contact(PurpleContact *contact);
void pidgin_syslog_show(void);

void *pidgin_log_get_handle(void);
void pidgin_log_init(void);
void pidgin_log_uninit(void);

/** PIDGIN4_WINDOWS_SELFTEST: opens the system log and the log of the
 * first buddy that has logs, shows the newest log, runs a search, and
 * closes the windows. Never modifies logs. */
void pidgin_log_selftest(void);

#endif /* _PIDGINLOG_H_ */
