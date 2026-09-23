/**
 * @file displayed.h XEP-0490 Message Displayed Synchronization
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
#ifndef PURPLE_JABBER_DISPLAYED_H_
#define PURPLE_JABBER_DISPLAYED_H_

#include "jabber.h"

/** Registers the urn:xmpp:mds:displayed:0 PEP handler (+notify). */
void jabber_displayed_pep_init(void);

/**
 * Builds the <publish/> for "the last displayed message in @a conv_jid
 * (bare) is the one @a by (the archive: our bare JID, or the room) gave
 * the stanza-id @a stanza_id".
 */
xmlnode *jabber_displayed_publish_node(const char *conv_jid,
		const char *stanza_id, const char *by);

/**
 * Fetches all items of the node (the initial sync on sign-on; the node is
 * published with send_last_published_item=never).  Only when the UI
 * handles message metadata.
 */
void jabber_displayed_fetch(JabberStream *js);

/** Registers the mds-publish IPC command. */
void jabber_displayed_init(PurplePlugin *plugin);
void jabber_displayed_uninit(void);

#endif /* PURPLE_JABBER_DISPLAYED_H_ */
