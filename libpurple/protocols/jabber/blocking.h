/**
 * @file blocking.h XEP-0191 blocking extras, XEP-0186 invisibility,
 *                  XEP-0377 spam reporting
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
#ifndef PURPLE_JABBER_BLOCKING_H_
#define PURPLE_JABBER_BLOCKING_H_

#include "jabber.h"
#include "xmlnode.h"

/*
 * The blocklist itself (fetch on connect, mirroring into libpurple's deny
 * list with local_only = TRUE, pushes, add_deny/rem_deny) lives in jabber.c
 * as it always did.  This file adds:
 *
 *  - set_permit_deny: only PURPLE_PRIVACY_ALLOW_ALL and _DENY_USERS can be
 *    expressed with XEP-0191; this tree has no XEP-0016 (privacy lists), so
 *    the other modes are only enforced locally by libpurple;
 *  - XEP-0186 <invisible/>/<visible/> for the "invisible" status;
 *  - XEP-0377 reports inside the XEP-0191 <block/>;
 *  - the IPC commands privacy-modes, status-invisible-supported,
 *    report-spam and report-spam-supported.
 */

/** Records a server (domain) disco#info feature this file cares about. */
void jabber_blocking_server_feature(JabberStream *js, const char *var);

/**
 * Builds <block xmlns='urn:xmpp:blocking'> (or <unblock/> when @a block is
 * FALSE) with one <item jid=''/> per entry of the NULL-terminated @a jids.
 * With @a report_reason (JABBER_REPORTING_SPAM or _ABUSE) each item gets a
 * <report xmlns='urn:xmpp:reporting:1' reason=''/>, with <text/> if @a text
 * is not NULL/empty.  An unblock with no JIDs unblocks everyone.
 */
xmlnode *jabber_blocking_build(gboolean block, const char * const *jids,
                               const char *report_reason, const char *text);

/** Builds <invisible xmlns='urn:xmpp:invisible:0' probe='true'/> or
 *  <visible xmlns='urn:xmpp:invisible:0'/>. */
xmlnode *jabber_invisible_build(gboolean invisible);

typedef enum {
	JABBER_INVISIBLE_SEND,        /**< send presence as usual */
	JABBER_INVISIBLE_SEND_FORCED, /**< <visible/> was sent: force presence */
	JABBER_INVISIBLE_HOLD         /**< don't send presence (yet) */
} JabberInvisibleAction;

/**
 * Called by jabber_presence_send() before it sends presence.  When the
 * server supports XEP-0186, sends <invisible/> when @a invisible and not yet
 * in effect, and <visible/> when leaving it.  Presence is held back until
 * the server confirmed <invisible/> (then it is sent, and not broadcast by
 * the server), and for good if the server refused it, so that a refusal
 * never broadcasts an "invisible" user.  Without XEP-0186 the invisible
 * status is sent as available (the behaviour before it existed).
 */
JabberInvisibleAction jabber_invisible_sync(JabberStream *js, gboolean invisible);

/** The prpl's set_permit_deny. */
void jabber_set_permit_deny(PurpleConnection *gc);

/** The privacy modes the server can enforce, as newly allocated strings:
 *  "allow-all" always, plus "deny-users" with XEP-0191. */
GList *jabber_blocking_privacy_modes(JabberStream *js);

/** Registers the IPC commands. */
void jabber_blocking_init(PurplePlugin *plugin);

#endif /* PURPLE_JABBER_BLOCKING_H_ */
