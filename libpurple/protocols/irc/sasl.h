/**
 * @file sasl.h Built-in IRCv3 SASL (PLAIN and EXTERNAL)
 *
 * purple
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

#ifndef _PURPLE_IRC_SASL_H
#define _PURPLE_IRC_SASL_H

#include "irc.h"

/* Account setting keys. "sasl", "saslname" and "auth_plain_in_clear" are
 * the keys the Cyrus SASL code has always used; "sasl_mechanism" is new. */
#define IRC_SASL_OPT_ENABLED		"sasl"
#define IRC_SASL_OPT_LOGIN		"saslname"
#define IRC_SASL_OPT_PLAIN_IN_CLEAR	"auth_plain_in_clear"
#define IRC_SASL_OPT_MECHANISM		"sasl_mechanism"

#define IRC_SASL_MECH_PLAIN	"PLAIN"
#define IRC_SASL_MECH_EXTERNAL	"EXTERNAL"
#define IRC_SASL_MECH_CYRUS	"cyrus"	/* whatever Cyrus SASL negotiates */

/* The largest AUTHENTICATE payload chunk (IRCv3 SASL 3.1). */
#define IRC_SASL_CHUNK_SIZE 400

gboolean irc_sasl_enabled(struct irc_conn *irc);
const char *irc_sasl_mechanism(struct irc_conn *irc);
gboolean irc_sasl_uses_password(struct irc_conn *irc);
void irc_sasl_start(struct irc_conn *irc);
void irc_sasl_free(struct irc_conn *irc);

/* Builds the RFC 4616 PLAIN message "authzid NUL authcid NUL passwd". */
guchar *irc_sasl_plain_message(const char *authzid, const char *authcid,
                               const char *passwd, gsize *len);

/* Splits a base64 payload into AUTHENTICATE arguments: 400-byte chunks,
 * plus a final "+" when the last chunk is exactly 400 bytes long or the
 * payload is empty. Free with g_strfreev(). */
gchar **irc_sasl_chunk(const char *b64);

void irc_msg_authenticate(struct irc_conn *irc, const char *name, const char *from, char **args);
void irc_msg_sasl_loggedin(struct irc_conn *irc, const char *name, const char *from, char **args);
void irc_msg_sasl_success(struct irc_conn *irc, const char *name, const char *from, char **args);
void irc_msg_sasl_fail(struct irc_conn *irc, const char *name, const char *from, char **args);
void irc_msg_sasl_mechs(struct irc_conn *irc, const char *name, const char *from, char **args);

#endif /* _PURPLE_IRC_SASL_H */
