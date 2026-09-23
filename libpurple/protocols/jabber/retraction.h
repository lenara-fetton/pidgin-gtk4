/**
 * @file retraction.h XEP-0424 Message Retraction, XEP-0425 Moderation
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
#ifndef PURPLE_JABBER_RETRACTION_H_
#define PURPLE_JABBER_RETRACTION_H_

#include "message.h"

/**
 * Parses a <retract/> (urn:xmpp:message-retract:1, with an optional 0425
 * <moderated/>) or a XEP-0422 <apply-to/> carrying the older
 * message-retract:0 / message-moderate:0 forms into @a jm.  Returns TRUE
 * if @a child was one of them.
 */
gboolean jabber_retraction_parse(JabberMessage *jm, xmlnode *child,
		const char *xmlns);

/**
 * Handles an incoming retraction or moderation (jm->retract_id set).
 * Always consumes the message (its body is the fallback): emits
 * message-retracted, or writes "X retracted a message" when no handler
 * rendered it.
 */
gboolean jabber_retraction_handle(JabberMessage *jm, const JabberMessageTarget *t);

/** Registers the send-retraction and send-moderation IPC commands. */
void jabber_retraction_init(PurplePlugin *plugin);

#endif /* PURPLE_JABBER_RETRACTION_H_ */
