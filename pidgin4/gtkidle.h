/**
 * @file gtkidle.h GTK+ Idle API
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
#ifndef _PIDGIN_IDLE_H_
#define _PIDGIN_IDLE_H_

#include "idle.h"

/**************************************************************************/
/** @name GTK+ Idle API                                                  */
/**************************************************************************/
/*@{*/

/**
 * Returns the GTK+ idle UI ops.
 *
 * @return The UI operations structure.
 */
PurpleIdleUiOps *pidgin_idle_get_ui_ops(void);

/**
 * pidgin4: stops the idle sources (ext-idle-notify, the Mutter poll).
 * The first pidgin_idle_get_ui_ops() registers /pidgin4/idle/method
 * ("system", "purple" or "none"); see gtkidle.c.
 */
void pidgin_idle_uninit(void);

/*@}*/

#endif /* _PIDGIN_IDLE_H_ */
