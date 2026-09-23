/**
 * @file gtkstatusbox.h The status selector at the bottom of the buddy list
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

/*
 * Replaces pidgin/gtkstatusbox.c (a custom GtkContainer) for the global
 * status: a GtkMenuButton showing the current saved status, whose popover
 * lists the primitive statuses, the popular saved statuses and a message
 * entry; plus the global buddy icon button. Per-account status boxes (the
 * account editor) are not ported.
 */
#ifndef _PIDGINSTATUSBOX_H_
#define _PIDGINSTATUSBOX_H_

#include <gtk/gtk.h>

/** The status box widget (one per buddy list). */
GtkWidget *pidgin_status_box_new(void);

/** Tells the status box whether the network is up (from gtkconn.c). */
void pidgin_status_box_set_network_available(gboolean available);

/**
 * Test hook. If PIDGIN4_STATUS_SELFTEST is set and no account is enabled
 * (so nothing can sign in), picks "Away" with a message through the status
 * box, checks that it became the current saved status, and quits. Does
 * nothing (logs why) if an account is enabled.
 */
void pidgin_status_box_selftest(void);

#endif /* _PIDGINSTATUSBOX_H_ */
