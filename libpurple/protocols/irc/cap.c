/**
 * @file cap.c IRCv3 capability negotiation (CAP 302)
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
 * Registration sequence:
 *
 *   CAP LS 302, [PASS], USER, NICK
 *   <- CAP * LS * :...        (zero or more continuation lines)
 *   <- CAP * LS :...
 *   CAP REQ :cap1 cap2 ...    (only the capabilities we want and the server offers)
 *   <- CAP * ACK|NAK :...
 *   [AUTHENTICATE ... <- 903] (sasl.c, when SASL is enabled on the account)
 *   CAP END
 *
 * Servers without CAP support either answer CAP with 421 (or 451), or just
 * register us (001); both end negotiation. If nothing at all comes back
 * within IRC_CAP_TIMEOUT seconds we send CAP END and carry on.
 */

#include "internal.h"

#include "debug.h"
#include "util.h"

#include "irc.h"
#include "sasl.h"

/* The capabilities requested when the server offers them, in REQ order. */
static const char *const irc_caps_wanted[] = {
	"sasl",			/* only if SASL is enabled on the account */
	"server-time",
	"echo-message",
	"away-notify",
	"account-notify",
	"multi-prefix",
	"message-tags",
	"cap-notify",
	"extended-join",
	"chghost",
	NULL
};

/* Keep REQ lines well below the 512-byte line limit. */
#define IRC_CAP_REQ_MAX 400

static void irc_cap_check_done(struct irc_conn *irc);

static void
irc_cap_init(struct irc_conn *irc)
{
	if (irc->caps_ls == NULL)
		irc->caps_ls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	if (irc->caps == NULL)
		irc->caps = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

static void
irc_cap_stop_timer(struct irc_conn *irc)
{
	if (irc->cap_timer) {
		purple_timeout_remove(irc->cap_timer);
		irc->cap_timer = 0;
	}
}

/* Negotiation ended without SASL although the account requires it: fail
 * the connection rather than go on unauthenticated. */
static gboolean
irc_cap_sasl_missing(struct irc_conn *irc)
{
	if (!irc_sasl_enabled(irc) || irc->sasl_state == IRC_SASL_DONE)
		return FALSE;

	irc_cap_stop_timer(irc);
	irc->cap_negotiating = FALSE;
	purple_connection_error_reason(purple_account_get_connection(irc->account),
		PURPLE_CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE,
		_("SASL authentication failed: Server does not support SASL authentication."));
	return TRUE;
}

static gboolean
irc_cap_timeout_cb(gpointer data)
{
	struct irc_conn *irc = data;

	irc->cap_timer = 0;
	if (irc->cap_negotiating && !irc->cap_replied) {
		purple_debug_info("irc", "No reply to CAP LS within %d seconds; "
		                  "continuing without IRCv3 capabilities\n",
		                  IRC_CAP_TIMEOUT);
		/* CAP END in case a slow server is holding registration. */
		if (!irc_cap_sasl_missing(irc))
			irc_cap_end(irc);
	}
	return FALSE;
}

gboolean
irc_cap_enabled(struct irc_conn *irc, const char *cap)
{
	return irc != NULL && irc->caps != NULL &&
		g_hash_table_lookup(irc->caps, cap) != NULL;
}

gboolean
irc_cap_wanted(struct irc_conn *irc, const char *cap)
{
	int i;

	/* SASL is only possible before registration completes. */
	if (purple_strequal(cap, "sasl"))
		return irc_sasl_enabled(irc) && !irc->registered;

	for (i = 0; irc_caps_wanted[i]; i++)
		if (purple_strequal(cap, irc_caps_wanted[i]))
			return TRUE;
	return FALSE;
}

/* Splits the part of a CAP reply after the subcommand, "[*] [:]cap1 cap2=v",
 * into its tokens. *more is set when this is a continuation line ("*"). */
gchar **
irc_cap_parse_list(const char *params, gboolean *more)
{
	gchar **tokens, **in, **out;

	*more = FALSE;
	if (params == NULL)
		return g_new0(gchar *, 1);

	while (*params == ' ')
		params++;
	if (params[0] == '*' && params[1] == ' ') {
		*more = TRUE;
		params++;
		while (*params == ' ')
			params++;
	}
	if (*params == ':')
		params++;

	/* Drop the empty tokens that repeated spaces produce. */
	tokens = g_strsplit(params, " ", -1);
	for (in = out = tokens; *in; in++) {
		if (**in)
			*out++ = *in;
		else
			g_free(*in);
	}
	*out = NULL;

	return tokens;
}

/* Adds "name" or "name=value" tokens to an advertised-capability table. */
void
irc_cap_ls_add(GHashTable *ls, gchar **tokens)
{
	for (; tokens && *tokens; tokens++) {
		const char *eq = strchr(*tokens, '=');

		if (eq == *tokens)
			continue;
		if (eq)
			g_hash_table_replace(ls, g_strndup(*tokens, eq - *tokens),
			                     g_strdup(eq + 1));
		else
			g_hash_table_replace(ls, g_strdup(*tokens), g_strdup(""));
	}
}

static void
irc_cap_send_req(struct irc_conn *irc, const char *caps)
{
	char *buf = irc_format(irc, "vv:", "CAP", "REQ", caps);

	purple_debug_info("irc", "Requesting capabilities: %s\n", caps);
	irc_priority_send(irc, buf);
	g_free(buf);
	irc->caps_pending++;
}

/* Requests every wanted capability that @offered (name -> value) contains
 * and that isn't enabled yet. */
static void
irc_cap_request(struct irc_conn *irc, GHashTable *offered)
{
	GString *req = g_string_new(NULL);
	int i;

	for (i = 0; irc_caps_wanted[i]; i++) {
		const char *cap = irc_caps_wanted[i];

		if (!g_hash_table_lookup(offered, cap) || irc_cap_enabled(irc, cap) ||
		    !irc_cap_wanted(irc, cap))
			continue;

		if (req->len && req->len + strlen(cap) + 1 > IRC_CAP_REQ_MAX) {
			irc_cap_send_req(irc, req->str);
			g_string_truncate(req, 0);
		}
		if (req->len)
			g_string_append_c(req, ' ');
		g_string_append(req, cap);
	}

	if (req->len)
		irc_cap_send_req(irc, req->str);
	g_string_free(req, TRUE);
}

void
irc_cap_start(struct irc_conn *irc)
{
	char *buf;

	irc_cap_init(irc);
	irc->cap_negotiating = TRUE;
	irc->cap_replied = FALSE;
	irc->caps_pending = 0;

	buf = irc_format(irc, "vvv", "CAP", "LS", "302");
	irc_priority_send(irc, buf);
	g_free(buf);

	irc_cap_stop_timer(irc);
	irc->cap_timer = purple_timeout_add_seconds(IRC_CAP_TIMEOUT,
	                                            irc_cap_timeout_cb, irc);
}

void
irc_cap_end(struct irc_conn *irc)
{
	char *buf;

	irc_cap_stop_timer(irc);
	if (!irc->cap_negotiating)
		return;
	irc->cap_negotiating = FALSE;

	purple_debug_info("irc", "Capability negotiation done\n");
	buf = irc_format(irc, "vv", "CAP", "END");
	irc_priority_send(irc, buf);
	g_free(buf);
}

/* The server doesn't know CAP (421 for it), so it isn't holding
 * registration and there is nothing to end. */
void
irc_cap_unsupported(struct irc_conn *irc)
{
	irc_cap_stop_timer(irc);
	if (!irc->cap_negotiating)
		return;

	purple_debug_info("irc", "Server does not support IRCv3 capabilities\n");
	if (!irc_cap_sasl_missing(irc))
		irc->cap_negotiating = FALSE;
}

/* Ends negotiation once every REQ is answered, via SASL if that's on. */
static void
irc_cap_check_done(struct irc_conn *irc)
{
	if (!irc->cap_negotiating || irc->caps_pending > 0 || irc->caps_ls_more)
		return;

	if (irc_sasl_enabled(irc)) {
		if (irc->sasl_state == IRC_SASL_NONE) {
			if (irc_cap_enabled(irc, "sasl"))
				irc_sasl_start(irc);
			else
				irc_cap_sasl_missing(irc);
			return;
		}
		if (irc->sasl_state != IRC_SASL_DONE)
			return;	/* sasl.c calls irc_cap_end() */
	}

	irc_cap_end(irc);
}

static void
irc_cap_ack(struct irc_conn *irc, gchar **tokens)
{
	for (; *tokens; tokens++) {
		const char *cap = *tokens;

		if (*cap == '-') {
			g_hash_table_remove(irc->caps, cap + 1);
			purple_debug_info("irc", "Capability disabled: %s\n", cap + 1);
			continue;
		}
		/* Modifiers from the pre-3.1 draft. */
		while (*cap == '~' || *cap == '=')
			cap++;
		if (*cap) {
			g_hash_table_replace(irc->caps, g_strdup(cap), GINT_TO_POINTER(TRUE));
			purple_debug_info("irc", "Capability enabled: %s\n", cap);
		}
	}
}

void
irc_msg_cap(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	const char *sub = args[1];
	gboolean more = FALSE;
	gchar **tokens;

	irc_cap_init(irc);
	irc->cap_replied = TRUE;
	irc_cap_stop_timer(irc);

	tokens = irc_cap_parse_list(args[2], &more);

	if (!g_ascii_strcasecmp(sub, "LS")) {
		if (!irc->caps_ls_more)
			g_hash_table_remove_all(irc->caps_ls);
		irc_cap_ls_add(irc->caps_ls, tokens);
		irc->caps_ls_more = more;
		if (!more) {
			irc_cap_request(irc, irc->caps_ls);
			irc_cap_check_done(irc);
		}
	} else if (!g_ascii_strcasecmp(sub, "ACK")) {
		irc_cap_ack(irc, tokens);
		if (!more) {
			irc->caps_pending--;
			irc_cap_check_done(irc);
		}
	} else if (!g_ascii_strcasecmp(sub, "NAK")) {
		gchar *list = g_strjoinv(" ", tokens);

		purple_debug_info("irc", "Capabilities refused: %s\n", list);
		g_free(list);
		if (!more)
			irc->caps_pending--;
		/* A REQ is all-or-nothing; retry the members one by one. */
		if (g_strv_length(tokens) > 1) {
			gchar **cap;

			for (cap = tokens; *cap; cap++)
				if (irc_cap_wanted(irc, *cap) && !irc_cap_enabled(irc, *cap))
					irc_cap_send_req(irc, *cap);
		}
		irc_cap_check_done(irc);
	} else if (!g_ascii_strcasecmp(sub, "NEW")) {
		GHashTable *offered = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

		irc_cap_ls_add(irc->caps_ls, tokens);
		irc_cap_ls_add(offered, tokens);
		irc_cap_request(irc, offered);
		g_hash_table_destroy(offered);
	} else if (!g_ascii_strcasecmp(sub, "DEL")) {
		gchar **cap;

		for (cap = tokens; *cap; cap++) {
			g_hash_table_remove(irc->caps_ls, *cap);
			if (g_hash_table_remove(irc->caps, *cap))
				purple_debug_info("irc", "Capability removed by server: %s\n", *cap);
		}
	}
	/* LIST: informational only. */

	g_strfreev(tokens);
}

/* 001: registration is complete. */
void
irc_msg_welcome(struct irc_conn *irc, const char *name, const char *from, char **args)
{
	irc->registered = TRUE;

	/* Registered while still negotiating: the server ignored CAP.
	 * Without SASL that's fine, and there is nothing to end. */
	irc_cap_unsupported(irc);
}

void
irc_cap_free(struct irc_conn *irc)
{
	irc_cap_stop_timer(irc);
	if (irc->caps_ls) {
		g_hash_table_destroy(irc->caps_ls);
		irc->caps_ls = NULL;
	}
	if (irc->caps) {
		g_hash_table_destroy(irc->caps);
		irc->caps = NULL;
	}
}
