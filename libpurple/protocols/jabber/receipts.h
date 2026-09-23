/**
 * @file receipts.h XEP-0184 Message Delivery Receipts, XEP-0333 Chat Markers
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
#ifndef PURPLE_JABBER_RECEIPTS_H_
#define PURPLE_JABBER_RECEIPTS_H_

#include "message.h"

/**
 * Handles the XEP-0184 and XEP-0333 parts of an incoming message: answers
 * a receipt request and emits message-receipt for receipts and markers.
 * Never consumes the message.
 */
void jabber_receipts_handle(JabberMessage *jm, const JabberMessageTarget *t);

/**
 * Emits message-receipt(account, conv_name, id, state, sender) if the UI
 * handles message metadata.  Returns what the handlers returned.
 */
gboolean jabber_receipts_emit(JabberStream *js, const char *conv_name,
		const char *id, const char *state, const char *sender);

/** Registers the send-marker IPC command. */
void jabber_receipts_init(PurplePlugin *plugin);

#endif /* PURPLE_JABBER_RECEIPTS_H_ */
