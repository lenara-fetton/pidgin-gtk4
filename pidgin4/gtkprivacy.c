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

/* PLACEHOLDER (M5 scaffolding): replaced by the privacy dialog */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "debug.h"
#include "gtkprivacy.h"


void pidgin_privacy_init(void) { }
void pidgin_privacy_dialog_show(void) { purple_debug_info("gtkprivacy", "placeholder\n"); }
void pidgin_privacy_dialog_hide(void) { }
void pidgin_request_add_permit(PurpleAccount *account, const char *name) { }
void pidgin_request_add_block(PurpleAccount *account, const char *name) { }
PurplePrivacyUiOps *pidgin_privacy_get_ui_ops(void) { return NULL; }
void pidgin_privacy_selftest(void) { }
