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
#ifndef _PIDGINCERTMGR_H_
#define _PIDGINCERTMGR_H_

#include "pidgin.h"

/* The certificate manager (M5): the tls_peers pool. */

void pidgin_certmgr_show(void);
void pidgin_certmgr_hide(void);

/** PIDGIN4_WINDOWS_SELFTEST: opens the manager, views every certificate
 * of the tls_peers pool, closes. Never deletes or imports anything. */
void pidgin_certmgr_selftest(void);

#endif /* _PIDGINCERTMGR_H_ */
