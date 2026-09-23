/**
 * @file carbons.h XEP-0280 Message Carbons
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
#ifndef PURPLE_JABBER_CARBONS_H_
#define PURPLE_JABBER_CARBONS_H_

#include "jabber.h"
#include "xmlnode.h"

typedef enum {
	JABBER_CARBON_NONE = 0,
	JABBER_CARBON_RECEIVED,
	JABBER_CARBON_SENT
} JabberCarbonDirection;

/**
 * Pure: if @a packet is a carbon copy, sets @a dir and returns the
 * forwarded <message/> (owned by @a packet), or NULL if the carbon is
 * malformed.  For anything else sets @a dir to JABBER_CARBON_NONE and
 * returns NULL.  @a forwarded, if not NULL, receives the <forwarded/>.
 */
xmlnode *jabber_carbons_unwrap(xmlnode *packet, JabberCarbonDirection *dir,
                               xmlnode **forwarded);

/**
 * Pure: whether a carbon with this outer "from" may be accepted: only
 * our own bare JID (or no from at all) may send us carbons.
 */
gboolean jabber_carbons_from_is_valid(const char *from, const char *own_bare);

/**
 * Handles @a packet if it is a carbon: validates it and parses the inner
 * message (sent carbons are shown as sent from another device).  Returns
 * TRUE if @a packet was a carbon (handled or dropped), FALSE otherwise.
 */
gboolean jabber_carbons_handle(JabberStream *js, xmlnode *packet);

/** Enables carbons on the stream (the server advertised them). */
void jabber_carbons_enable(JabberStream *js);

#endif /* PURPLE_JABBER_CARBONS_H_ */
