/**
 * @file auth.h Authentication routines
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
#ifndef PURPLE_JABBER_AUTH_H_
#define PURPLE_JABBER_AUTH_H_

typedef struct _JabberSaslMech JabberSaslMech;

#include "jabber.h"
#include "xmlnode.h"

typedef enum {
	JABBER_SASL_STATE_FAIL = -1,    /* Abort, Retry, Fail? */
	JABBER_SASL_STATE_OK = 0,       /* Hooray! */
	JABBER_SASL_STATE_CONTINUE = 1  /* More authentication required */
} JabberSaslState;

struct _JabberSaslMech {
	gint8 priority; /* Higher priority will be tried before lower priority */
	const gchar *name;
	JabberSaslState (*start)(JabberStream *js, xmlnode *mechanisms, xmlnode **reply, char **msg);
	JabberSaslState (*handle_challenge)(JabberStream *js, xmlnode *packet, xmlnode **reply, char **msg);
	JabberSaslState (*handle_success)(JabberStream *js, xmlnode *packet, char **msg);
	JabberSaslState (*handle_failure)(JabberStream *js, xmlnode *packet, xmlnode **reply, char **msg);
	void (*dispose)(JabberStream *js);
};

void jabber_auth_start(JabberStream *js, xmlnode *packet);
void jabber_auth_start_old(JabberStream *js);
void jabber_auth_handle_challenge(JabberStream *js, xmlnode *packet);
void jabber_auth_handle_success(JabberStream *js, xmlnode *packet);
void jabber_auth_handle_failure(JabberStream *js, xmlnode *packet);

JabberSaslMech *jabber_auth_get_plain_mech(void);
JabberSaslMech *jabber_auth_get_digest_md5_mech(void);
JabberSaslMech **jabber_auth_get_scram_mechs(gint *count);
#ifdef HAVE_CYRUS_SASL
JabberSaslMech *jabber_auth_get_cyrus_mech(void);
#endif

/**
 * Picks the best mechanism from @a server_mechs (a list of names).  -PLUS
 * mechanisms are skipped when the connection has no channel binding data.
 * With @a sasl2, only mechanisms that work inside XEP-0388 are considered
 * (SCRAM-* and PLAIN; never Cyrus).
 */
JabberSaslMech *jabber_auth_pick_mech(JabberStream *js, GSList *server_mechs,
                                      gboolean sasl2);

/**
 * TLS channel binding data for this stream (RFC 5929 / RFC 9266).  With
 * @a type NULL, the preferred available type is used: tls-exporter, then
 * tls-server-end-point, restricted to what the server advertised via
 * XEP-0440 if it did.  Returns NULL (and *len = 0) if none is available.
 * @a type_out receives a static string.  Free the result with g_free().
 */
guchar *jabber_auth_get_channel_binding(JabberStream *js, const char *type,
                                        const char **type_out, gsize *len);

void jabber_auth_add_mech(JabberSaslMech *);
void jabber_auth_remove_mech(JabberSaslMech *);

void jabber_auth_init(void);
void jabber_auth_uninit(void);

#endif /* PURPLE_JABBER_AUTH_H_ */
