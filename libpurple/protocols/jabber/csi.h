/**
 * @file csi.h XEP-0352 Client State Indication
 *
 * purple
 *
 * Purple is the legal property of its developers, whose names are too numerous
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
#ifndef PURPLE_JABBER_CSI_H_
#define PURPLE_JABBER_CSI_H_

#include "jabber.h"

/*
 * The client is "inactive" when the account is idle, its status is away or
 * extended away, or the UI said nobody is looking (jabber_csi_set_active()
 * or the "csi-set-active" IPC command).  Otherwise it is "active".  The
 * state is only sent when the server advertises urn:xmpp:csi:0 and only
 * when it changes.
 */

/** Registers the "csi-set-active" IPC command:
 *    gboolean csi-set-active(PurpleAccount *account, gboolean active)
 *  (returns FALSE if the account isn't a connected XMPP account). */
void jabber_csi_init(PurplePlugin *plugin);

/** Hooks account-status-changed (needs the accounts subsystem, so not from
 *  plugin init). */
void jabber_csi_hook_signals(PurplePlugin *plugin);

/** For the UI (M6): whether a conversation window of this account is being
 *  looked at.  Defaults to TRUE. */
void jabber_csi_set_active(JabberStream *js, gboolean active);

/** Re-evaluates idle/away/UI state and sends <active/>/<inactive/> if it
 *  changed. */
void jabber_csi_update(JabberStream *js);

#endif /* PURPLE_JABBER_CSI_H_ */
