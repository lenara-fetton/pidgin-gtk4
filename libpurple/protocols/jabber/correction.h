/**
 * @file correction.h XEP-0308 Last Message Correction
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
#ifndef PURPLE_JABBER_CORRECTION_H_
#define PURPLE_JABBER_CORRECTION_H_

#include "message.h"

/** Prefix of the text fallback for a correction no UI rendered. */
#define JABBER_CORRECTION_PREFIX _("edit: ")

/**
 * Handles an incoming <replace/> with a body.  @a body is the markup that
 * would be written.  Returns TRUE if the message was consumed: rendered
 * by the UI through message-corrected, or dropped because it came from a
 * different occupant than the original.  Otherwise the body of @a jm has
 * been given the JABBER_CORRECTION_PREFIX and is written as usual.
 */
gboolean jabber_correction_handle(JabberMessage *jm, const JabberMessageTarget *t,
		const char *body);

/** Adds JABBER_CORRECTION_PREFIX to the body (and XHTML body) of @a jm. */
void jabber_correction_prefix_body(JabberMessage *jm);

/** Registers the send-correction IPC command. */
void jabber_correction_init(PurplePlugin *plugin);

#endif /* PURPLE_JABBER_CORRECTION_H_ */
