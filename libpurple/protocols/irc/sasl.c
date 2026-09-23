/**
 * @file sasl.c Built-in IRCv3 SASL (PLAIN and EXTERNAL)
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

/*
 * SASL runs between CAP ACK :sasl and CAP END (see cap.c):
 *
 *   AUTHENTICATE PLAIN
 *   <- AUTHENTICATE +
 *   AUTHENTICATE <base64, in 400-byte pieces; "+" after an exact multiple>
 *   <- 900 (logged in as), 903 (success) | 902/904/905/906 (failure)
 *   CAP END
 *
 * PLAIN and EXTERNAL are built in and need no library. When libpurple is
 * built with Cyrus SASL, the "cyrus" mechanism setting hands the exchange
 * to the Cyrus code in msgs.c, which picks from whatever Cyrus supports.
 *
 * EXTERNAL authenticates with the TLS client certificate. ssl-nss has no
 * client certificate support (it installs no GetClientAuthData hook), so
 * for now EXTERNAL only works where the server can identify the client
 * some other way; we send the standard empty response either way.
 */

#include "internal.h"

#include "connection.h"
#include "debug.h"
#include "util.h"

#include "irc.h"
#include "sasl.h"

#ifdef HAVE_CYRUS_SASL
#include <sasl/sasl.h>
#endif

gboolean
irc_sasl_enabled(struct irc_conn *irc)
{
	return purple_account_get_bool(irc->account, IRC_SASL_OPT_ENABLED, FALSE);
}

const char *
irc_sasl_mechanism(struct irc_conn *irc)
{
	const char *mech = purple_account_get_string(irc->account,
			IRC_SASL_OPT_MECHANISM, IRC_SASL_MECH_PLAIN);

	return (mech && *mech) ? mech : IRC_SASL_MECH_PLAIN;
}

/* Whether the account password is the SASL password (and so must not be
 * sent as the server password with PASS). */
gboolean
irc_sasl_uses_password(struct irc_conn *irc)
{
	return irc_sasl_enabled(irc) &&
		g_ascii_strcasecmp(irc_sasl_mechanism(irc), IRC_SASL_MECH_EXTERNAL) != 0;
}

guchar *
irc_sasl_plain_message(const char *authzid, const char *authcid,
                       const char *passwd, gsize *len)
{
	gsize zlen = authzid ? strlen(authzid) : 0;
	gsize clen = strlen(authcid);
	gsize plen = strlen(passwd);
	guchar *msg, *p;

	*len = zlen + 1 + clen + 1 + plen;
	p = msg = g_malloc(*len + 1);
	memcpy(p, authzid ? authzid : "", zlen);
	p += zlen;
	*p++ = '\0';
	memcpy(p, authcid, clen);
	p += clen;
	*p++ = '\0';
	memcpy(p, passwd, plen);
	p[plen] = '\0';

	return msg;
}

gchar **
irc_sasl_chunk(const char *b64)
{
	GPtrArray *chunks = g_ptr_array_new();
	gsize len = strlen(b64), off;

	for (off = 0; off < len; off += IRC_SASL_CHUNK_SIZE)
		g_ptr_array_add(chunks, g_strndup(b64 + off,
			MIN(IRC_SASL_CHUNK_SIZE, len - off)));
	if (len % IRC_SASL_CHUNK_SIZE == 0)
		g_ptr_array_add(chunks, g_strdup("+"));
	g_ptr_array_add(chunks, NULL);

	return (gchar **)g_ptr_array_free(chunks, FALSE);
}

static void
irc_sasl_error(struct irc_conn *irc, PurpleConnectionError reason, const char *msg)
{
	irc->sasl_state = IRC_SASL_FAILED;
	purple_connection_error_reason(purple_account_get_connection(irc->account),
	                               reason, msg);
}

/* Whether the comma-separated mechanism list @list contains @mech. */
static gboolean
irc_sasl_mech_listed(const char *list, const char *mech)
{
	gchar **mechs = g_strsplit(list, ",", -1);
	gboolean found = FALSE;
	int i;

	for (i = 0; mechs[i] && !found; i++)
		found = (g_ascii_strcasecmp(g_strstrip(mechs[i]), mech) == 0);
	g_strfreev(mechs);

	return found;
}

void
irc_sasl_start(struct irc_conn *irc)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);
	const char *mech = irc_sasl_mechanism(irc);
	const char *offered = irc->caps_ls ? g_hash_table_lookup(irc->caps_ls, "sasl") : NULL;
	char *buf;

#ifdef HAVE_CYRUS_SASL
	if (!g_ascii_strcasecmp(mech, IRC_SASL_MECH_CYRUS)) {
		irc->sasl_state = IRC_SASL_STARTED;
		irc_sasl_cyrus_start(irc);
		return;
	}
#endif

	if (g_ascii_strcasecmp(mech, IRC_SASL_MECH_PLAIN) != 0 &&
	    g_ascii_strcasecmp(mech, IRC_SASL_MECH_EXTERNAL) != 0) {
		buf = g_strdup_printf(_("SASL authentication failed: The %s mechanism "
		                        "is not supported."), mech);
		irc_sasl_error(irc, PURPLE_CONNECTION_ERROR_INVALID_SETTINGS, buf);
		g_free(buf);
		return;
	}

	/* CAP 302 servers list their mechanisms in the sasl= value. */
	if (offered && *offered && !irc_sasl_mech_listed(offered, mech)) {
		buf = g_strdup_printf(_("SASL authentication failed: The server does "
		                        "not support %s (it supports %s)."), mech, offered);
		irc_sasl_error(irc, PURPLE_CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE, buf);
		g_free(buf);
		return;
	}

	if (!g_ascii_strcasecmp(mech, IRC_SASL_MECH_PLAIN)) {
		const char *pass = purple_connection_get_password(gc);

		if (irc->gsc == NULL && !purple_account_get_bool(irc->account,
				IRC_SASL_OPT_PLAIN_IN_CLEAR, FALSE)) {
			irc_sasl_error(irc, PURPLE_CONNECTION_ERROR_ENCRYPTION_ERROR,
				_("SASL PLAIN would send the password unencrypted. "
				  "Enable SSL, or allow plaintext SASL auth over an "
				  "unencrypted connection."));
			return;
		}
		if (pass == NULL || *pass == '\0') {
			irc_sasl_error(irc, PURPLE_CONNECTION_ERROR_INVALID_SETTINGS,
				_("SASL PLAIN authentication needs a password."));
			return;
		}
	}

	g_free(irc->sasl_mech);
	irc->sasl_mech = g_ascii_strup(mech, -1);
	irc->sasl_state = IRC_SASL_STARTED;
	purple_debug_info("irc", "Using SASL: %s\n", irc->sasl_mech);

	buf = irc_format(irc, "vv", "AUTHENTICATE", irc->sasl_mech);
	irc_priority_send(irc, buf);
	g_free(buf);
}

/* The SASL login name: the "SASL login name" setting, else the nick the
 * account is configured with. */
static char *
irc_sasl_login(struct irc_conn *irc)
{
	const char *login = purple_account_get_string(irc->account, IRC_SASL_OPT_LOGIN, NULL);
	const char *username, *at;

	if (login && *login)
		return g_strdup(login);

	username = purple_account_get_username(irc->account);
	at = strchr(username, '@');
	return at ? g_strndup(username, at - username) : g_strdup(username);
}

static void
irc_sasl_respond(struct irc_conn *irc)
{
	PurpleConnection *gc = purple_account_get_connection(irc->account);
	char *b64;
	gchar **chunks, **chunk;

	if (!strcmp(irc->sasl_mech, IRC_SASL_MECH_PLAIN)) {
		char *login = irc_sasl_login(irc);
		const char *pass = purple_connection_get_password(gc);
		guchar *msg;
		gsize len;

		/* authzid = authcid, which every common services package
		 * accepts (an empty authzid trips up a few). */
		msg = irc_sasl_plain_message(login, login, pass ? pass : "", &len);
		b64 = purple_base64_encode(msg, len);
		memset(msg, 0, len);
		g_free(msg);
		g_free(login);
	} else {
		/* EXTERNAL: empty response; the server takes the identity
		 * from the client certificate. */
		b64 = g_strdup("");
	}

	chunks = irc_sasl_chunk(b64);
	for (chunk = chunks; *chunk; chunk++) {
		char *buf = irc_format(irc, "vv", "AUTHENTICATE", *chunk);
		irc_priority_send(irc, buf);
		memset(buf, 0, strlen(buf));
		g_free(buf);
		memset(*chunk, 0, strlen(*chunk));
	}
	g_strfreev(chunks);
	memset(b64, 0, strlen(b64));
	g_free(b64);

	irc->sasl_state = IRC_SASL_RESPONDED;
}

void
irc_msg_authenticate(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	const char *arg = args[0];

#ifdef HAVE_CYRUS_SASL
	if (irc->sasl_conn) {
		irc_msg_auth(irc, (char *)arg);
		return;
	}
#endif

	if (irc->sasl_state != IRC_SASL_STARTED) {
		purple_debug_warning("irc", "Unexpected AUTHENTICATE from server\n");
		return;
	}

	/* A server challenge comes in 400-byte pieces, ended by a shorter
	 * piece or "+". */
	if (strcmp(arg, "+") != 0) {
		if (irc->sasl_inbuf == NULL)
			irc->sasl_inbuf = g_string_new(NULL);
		g_string_append(irc->sasl_inbuf, arg);
		if (strlen(arg) == IRC_SASL_CHUNK_SIZE)
			return;
	}

	/* PLAIN and EXTERNAL expect an empty challenge. */
	if (irc->sasl_inbuf) {
		if (irc->sasl_inbuf->len)
			purple_debug_warning("irc", "Ignoring a non-empty SASL %s "
			                     "challenge\n", irc->sasl_mech);
		g_string_free(irc->sasl_inbuf, TRUE);
		irc->sasl_inbuf = NULL;
	}

	irc_sasl_respond(irc);
}

/* 900 RPL_LOGGEDIN: <nick> <nick!user@host> <account> :You are now logged in as <account> */
void
irc_msg_sasl_loggedin(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	if (args[2])
		purple_debug_info("irc", "Logged in to services as %s\n", args[2]);
}

/* 903 RPL_SASLSUCCESS, 907 ERR_SASLALREADY */
void
irc_msg_sasl_success(struct irc_conn *irc, const char *name, const char *from, char **args)
{
#ifdef HAVE_CYRUS_SASL
	if (irc->sasl_conn)
		irc_msg_authok(irc, name, from, args);
#endif

	if (irc->sasl_state == IRC_SASL_NONE || irc->sasl_state == IRC_SASL_FAILED) {
		purple_debug_warning("irc", "Unexpected SASL numeric %s\n", name);
		return;
	}

	purple_debug_info("irc", "Successfully authenticated using SASL%s%s\n",
	                  irc->sasl_mech ? " " : "", irc->sasl_mech ? irc->sasl_mech : "");
	irc->sasl_state = IRC_SASL_DONE;
	irc_cap_end(irc);
}

/* 902 ERR_NICKLOCKED, 904 ERR_SASLFAIL, 905 ERR_SASLTOOLONG, 906 ERR_SASLABORTED */
void
irc_msg_sasl_fail(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	const char *text = (args[1] && *args[1]) ? args[1] : NULL;
	PurpleConnectionError reason = PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED;
	char *msg;

#ifdef HAVE_CYRUS_SASL
	if (irc->sasl_conn) {
		if (purple_strequal(name, "904"))
			irc_msg_authtryagain(irc, name, from, args);
		else
			irc_msg_authfail(irc, name, from, args);
		return;
	}
#endif

	if (irc->sasl_state != IRC_SASL_STARTED && irc->sasl_state != IRC_SASL_RESPONDED) {
		purple_debug_warning("irc", "Unexpected SASL numeric %s\n", name);
		return;
	}

	if (purple_strequal(name, "905")) {
		msg = g_strdup(_("SASL authentication failed: The authentication message was too long."));
		reason = PURPLE_CONNECTION_ERROR_OTHER_ERROR;
	} else if (purple_strequal(name, "906")) {
		msg = g_strdup(_("SASL authentication failed: The server aborted authentication."));
		reason = PURPLE_CONNECTION_ERROR_OTHER_ERROR;
	} else if (irc->sasl_server_mechs && irc->sasl_state == IRC_SASL_STARTED) {
		/* 908 came first: the server refused the mechanism itself. */
		msg = g_strdup_printf(_("SASL authentication failed: The server does "
		                        "not support %s (it supports %s)."),
		                      irc->sasl_mech, irc->sasl_server_mechs);
		reason = PURPLE_CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE;
	} else if (purple_strequal(name, "904")) {
		if (text)
			msg = g_strdup_printf(_("SASL authentication failed: Incorrect "
			                        "account name or password (the server said: %s)."), text);
		else
			msg = g_strdup(_("SASL authentication failed: Incorrect account name or password."));
	} else {
		/* 902: the nick is locked by services. */
		msg = g_strdup_printf(_("SASL authentication failed: %s"),
		                      text ? text : name);
	}

	irc_sasl_error(irc, reason, msg);
	g_free(msg);
}

/* 908 RPL_SASLMECHS: <nick> <mechanisms> :are available SASL mechanisms */
void
irc_msg_sasl_mechs(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	g_free(irc->sasl_server_mechs);
	irc->sasl_server_mechs = g_strdup(args[1]);
}

void
irc_sasl_free(struct irc_conn *irc)
{
	g_free(irc->sasl_mech);
	irc->sasl_mech = NULL;
	g_free(irc->sasl_server_mechs);
	irc->sasl_server_mechs = NULL;
	if (irc->sasl_inbuf) {
		g_string_free(irc->sasl_inbuf, TRUE);
		irc->sasl_inbuf = NULL;
	}
}
