/*
 * purple - Jabber Protocol Plugin
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
 *
 */
#include "internal.h"
#include "account.h"
#include "cipher.h"
#include "conversation.h"
#include "debug.h"
#include "network.h"
#include "server.h"
#include "util.h"
#include "xmlnode.h"

#include "chat.h"
#include "presence.h"
#include "jutil.h"

/*
 * XMPP string preparation: the RFC 3920 nodeprep and resourceprep profiles,
 * the nameprep mapping for domains, and RFC 4013 SASLprep.
 *
 * These used to call libidn 1.x's stringprep, which libidn2 does not provide.
 * They are now implemented with GLib's Unicode support. Behavioural
 * differences from the libidn implementation:
 *  - GLib's Unicode tables are current, while stringprep is pinned to
 *    Unicode 3.2. Code points unassigned in 3.2 were already accepted (libidn
 *    was called without STRINGPREP_NO_UNASSIGNED); they are now mapped with
 *    current case folding and normalization data.
 *  - Table B.2 (case folding for use with NFKC) is approximated by full
 *    Unicode case folding applied before and after NFKC for nodes.
 *  - The bidi rule (RFC 3454 section 6) is not checked, since GLib has no
 *    bidi class API. Strings that mix right-to-left and left-to-right text in
 *    ways libidn rejected are accepted now.
 *  - Domains are no longer nameprep'd and checked with IDNA2003 ToASCII.
 *    They are lowercased (not case folded, so that IDNA2008 keeps "ß" and
 *    "straße.de" no longer turns into "strasse.de") and NFKC-normalized, kept
 *    in Unicode as before, and validated with
 *    purple_network_convert_idn_to_ascii() (IDNA2008 with UTS #46
 *    non-transitional processing, libidn2) plus jabber_domain_validate().
 *    IDNA2008 rejects some symbols IDNA2003 accepted (for example U+2603
 *    SNOWMAN).
 *  - Without libidn2, node, resource and SASLprep now get the full GLib
 *    implementation instead of the old ad-hoc checks.
 * The ASCII fast path in jabber_id_new_internal() is unchanged.
 */
typedef enum {
	JABBER_PREP_NODE,
	JABBER_PREP_NAME,
	JABBER_PREP_RESOURCE,
	JABBER_PREP_SASL
} JabberPrepProfile;

/* RFC 3454 table B.1: commonly mapped to nothing */
static gboolean
jabber_prep_maps_to_nothing(gunichar ch)
{
	return ch == 0x00AD || ch == 0x034F || ch == 0x1806 ||
		(ch >= 0x180B && ch <= 0x180D) ||
		(ch >= 0x200B && ch <= 0x200D) || ch == 0x2060 ||
		(ch >= 0xFE00 && ch <= 0xFE0F) || ch == 0xFEFF;
}

/* RFC 3454 table C.1.2: non-ASCII space characters */
static gboolean
jabber_prep_is_non_ascii_space(gunichar ch)
{
	return ch == 0x00A0 || ch == 0x1680 ||
		(ch >= 0x2000 && ch <= 0x200B) || ch == 0x202F ||
		ch == 0x205F || ch == 0x3000;
}

/* RFC 3454 tables C.1.2 to C.9, plus C.1.1 and the RFC 3920 appendix A.5
 * characters for nodeprep. */
static gboolean
jabber_prep_is_prohibited(gunichar ch, JabberPrepProfile profile)
{
	if (profile == JABBER_PREP_NODE &&
			(ch == ' ' || ch == '"' || ch == '&' || ch == '\'' ||
			 ch == '/' || ch == ':' || ch == '<' || ch == '>' || ch == '@'))
		return TRUE;

	return jabber_prep_is_non_ascii_space(ch) ||
		/* C.2.1 ASCII control characters */
		ch < 0x20 || ch == 0x7F ||
		/* C.2.2 non-ASCII control characters */
		(ch >= 0x80 && ch <= 0x9F) || ch == 0x06DD || ch == 0x070F ||
		ch == 0x180E || ch == 0x200C || ch == 0x200D || ch == 0x2028 ||
		ch == 0x2029 || (ch >= 0x2060 && ch <= 0x2063) ||
		(ch >= 0x206A && ch <= 0x206F) || ch == 0xFEFF ||
		(ch >= 0xFFF9 && ch <= 0xFFFC) || (ch >= 0x1D173 && ch <= 0x1D17A) ||
		/* C.3 private use */
		(ch >= 0xE000 && ch <= 0xF8FF) || (ch >= 0xF0000 && ch <= 0xFFFFD) ||
		(ch >= 0x100000 && ch <= 0x10FFFD) ||
		/* C.4 non-character code points */
		(ch >= 0xFDD0 && ch <= 0xFDEF) || (ch & 0xFFFE) == 0xFFFE ||
		/* C.5 surrogates */
		(ch >= 0xD800 && ch <= 0xDFFF) ||
		/* C.6 inappropriate for plain text */
		(ch >= 0xFFF9 && ch <= 0xFFFD) ||
		/* C.7 inappropriate for canonical representation */
		(ch >= 0x2FF0 && ch <= 0x2FFB) ||
		/* C.8 change display properties or deprecated */
		ch == 0x0340 || ch == 0x0341 || ch == 0x200E || ch == 0x200F ||
		(ch >= 0x202A && ch <= 0x202E) ||
		/* C.9 tagging characters */
		ch == 0xE0001 || (ch >= 0xE0020 && ch <= 0xE007F);
}

static void
jabber_prep_free(char *str, gboolean wipe)
{
	if (str != NULL && wipe)
		memset(str, 0, strlen(str));
	g_free(str);
}

/*
 * Apply a stringprep profile to the first len bytes of in (len < 0: all of
 * it). Returns a newly allocated string, or NULL if the input is not valid
 * UTF-8, contains a prohibited character, or is longer than 1023 bytes after
 * preparation.
 */
static char *
jabber_stringprep(const char *in, gssize len, JabberPrepProfile profile)
{
	gboolean fold = (profile == JABBER_PREP_NODE || profile == JABBER_PREP_NAME);
	gchar *(*fold_func)(const gchar *, gssize) =
		(profile == JABBER_PREP_NAME) ? g_utf8_strdown : g_utf8_casefold;
	gboolean wipe = (profile == JABBER_PREP_SASL);
	GString *mapped;
	const char *c, *end;
	char *tmp, *out;

	if (len < 0)
		len = strlen(in);

	if (!g_utf8_validate(in, len, NULL))
		return NULL;

	/* Mapping: B.1 to nothing; for SASLprep, C.1.2 to U+0020 */
	mapped = g_string_sized_new(len);
	for (c = in, end = in + len; c < end; c = g_utf8_next_char(c)) {
		gunichar ch = g_utf8_get_char(c);

		if (jabber_prep_maps_to_nothing(ch))
			continue;
		if (profile == JABBER_PREP_SASL && jabber_prep_is_non_ascii_space(ch))
			ch = ' ';
		g_string_append_unichar(mapped, ch);
	}

	/* Mapping (B.2 for nodes, lowercasing for domains) and NFKC */
	if (fold) {
		tmp = fold_func(mapped->str, mapped->len);
		out = g_utf8_normalize(tmp, -1, G_NORMALIZE_NFKC);
		g_free(tmp);
		if (out != NULL) {
			tmp = fold_func(out, -1);
			g_free(out);
			out = g_utf8_normalize(tmp, -1, G_NORMALIZE_NFKC);
			g_free(tmp);
		}
	} else {
		out = g_utf8_normalize(mapped->str, mapped->len, G_NORMALIZE_NFKC);
	}

	if (wipe)
		memset(mapped->str, 0, mapped->len);
	g_string_free(mapped, TRUE);

	if (out == NULL)
		return NULL;

	/* Prohibited output */
	for (c = out; *c; c = g_utf8_next_char(c)) {
		if (jabber_prep_is_prohibited(g_utf8_get_char(c), profile)) {
			jabber_prep_free(out, wipe);
			return NULL;
		}
	}

	if (strlen(out) > 1023) {
		jabber_prep_free(out, wipe);
		return NULL;
	}

	return out;
}

/* Prepare a (non-IP-literal) domain. See the comment above. */
static char *
jabber_domain_prep(const char *in, gssize len)
{
	char *out = jabber_stringprep(in, len, JABBER_PREP_NAME);
	char *ascii = NULL;
	int rc;

	if (out == NULL)
		return NULL;

	rc = purple_network_convert_idn_to_ascii(out, &ascii);
	g_free(ascii);
	if (rc != 0 || !jabber_domain_validate(out)) {
		purple_debug_info("jabber", "Invalid domain '%s' (IDNA error %d)\n",
		                  out, rc);
		g_free(out);
		return NULL;
	}

	return out;
}

static JabberID*
jabber_id_prep(const char *str, const char *at, const char *slash,
               const char *null)
{
	const char *node = NULL;
	const char *domain = NULL;
	const char *resource = NULL;
	int node_len = 0;
	int domain_len = 0;
	int resource_len = 0;
	JabberID *jid;

	/* Ensure no parts are > 1023 bytes */
	if (at) {
		node = str;
		node_len = at - str;

		domain = at + 1;
		if (slash) {
			domain_len = slash - (at + 1);
			resource = slash + 1;
			resource_len = null - (slash + 1);
		} else {
			domain_len = null - (at + 1);
		}
	} else {
		domain = str;

		if (slash) {
			domain_len = slash - str;
			resource = slash + 1;
			resource_len = null - (slash + 1);
		} else {
			domain_len = null - str;
		}
	}

	if (node && node_len > 1023)
		return NULL;
	if (domain_len > 1023)
		return NULL;
	if (resource && resource_len > 1023)
		return NULL;

	jid = g_new0(JabberID, 1);

	if (node) {
		jid->node = jabber_stringprep(node, node_len, JABBER_PREP_NODE);
		if (jid->node == NULL)
			goto fail;
	}

	/* domain *must* be here */
	if (domain[0] == '[') { /* IPv6 address */
		gboolean valid = FALSE;

		if (domain_len > 2 && domain[domain_len - 1] == ']') {
			char *addr = g_strndup(domain + 1, domain_len - 2);
			valid = purple_ipv6_address_is_valid(addr);
			g_free(addr);
		}

		if (!valid)
			goto fail;

		jid->domain = g_strndup(domain, domain_len);
	} else {
		jid->domain = jabber_domain_prep(domain, domain_len);
		if (jid->domain == NULL)
			goto fail;
	}

	if (resource && resource_len > 0) {
		jid->resource = jabber_stringprep(resource, resource_len,
		                                  JABBER_PREP_RESOURCE);
		if (jid->resource == NULL)
			goto fail;
	}

	return jid;

fail:
	jabber_id_free(jid);
	return NULL;
}

static gboolean
jabber_prep_validate(const char *str, JabberPrepProfile profile)
{
	char *prepped;

	if(!str)
		return TRUE;

	if(strlen(str) > 1023)
		return FALSE;

	prepped = jabber_stringprep(str, -1, profile);
	g_free(prepped);

	return prepped != NULL;
}

gboolean jabber_nodeprep_validate(const char *str)
{
	return jabber_prep_validate(str, JABBER_PREP_NODE);
}

gboolean jabber_domain_validate(const char *str)
{
	const char *c;
	size_t len;

	if(!str)
		return TRUE;

	len = strlen(str);
	if (len > 1023)
		return FALSE;

	c = str;

	if (*c == '[') {
		/* Check if str is a valid IPv6 identifier */
		gboolean valid = FALSE;

		if (*(c + len - 1) != ']')
			return FALSE;

		/* Ugly, but in-place */
		*(gchar *)(c + len - 1) = '\0';
		valid = purple_ipv6_address_is_valid(c + 1);
		*(gchar *)(c + len - 1) = ']';

		return valid;
	}

	while(c && *c) {
		gunichar ch = g_utf8_get_char(c);
		/* The list of characters allowed in domain names is pretty small */
		if ((ch <= 0x7F && !( (ch >= 'a' && ch <= 'z')
				|| (ch >= '0' && ch <= '9')
				|| (ch >= 'A' && ch <= 'Z')
				|| ch == '.'
				|| ch == '-' )) || (ch >= 0x80 && !g_unichar_isgraph(ch)))
			return FALSE;

		c = g_utf8_next_char(c);
	}

	return TRUE;
}

gboolean jabber_resourceprep_validate(const char *str)
{
	return jabber_prep_validate(str, JABBER_PREP_RESOURCE);
}

char *jabber_saslprep(const char *in)
{
	g_return_val_if_fail(in != NULL, NULL);
	g_return_val_if_fail(strlen(in) <= 1023, NULL);

	return jabber_stringprep(in, -1, JABBER_PREP_SASL);
}

static JabberID*
jabber_id_new_internal(const char *str, gboolean allow_terminating_slash)
{
	const char *at = NULL;
	const char *slash = NULL;
	const char *c;
	gboolean needs_validation = FALSE;
#if 0
	gboolean node_is_required = FALSE;
#endif
	JabberID *jid;

	if (!str)
		return NULL;

	for (c = str; *c != '\0'; c++)
	{
		switch (*c) {
			case '@':
				if (!slash) {
					if (at) {
						/* Multiple @'s in the node/domain portion, not a valid JID! */
						return NULL;
					}
					if (c == str) {
						/* JIDs cannot start with @ */
						return NULL;
					}
					if (c[1] == '\0') {
						/* JIDs cannot end with @ */
						return NULL;
					}
					at = c;
				}
				break;

			case '/':
				if (!slash) {
					if (c == str) {
						/* JIDs cannot start with / */
						return NULL;
					}
					if (c[1] == '\0' && !allow_terminating_slash) {
						/* JIDs cannot end with / */
						return NULL;
					}
					slash = c;
				}
				break;

			default:
				/* characters allowed everywhere */
				if ((*c >= 'a' && *c <= 'z')
						|| (*c >= '0' && *c <= '9')
						|| (*c >= 'A' && *c <= 'Z')
						|| *c == '.' || *c == '-')
					/* We're good */
					break;

#if 0
				if (slash != NULL) {
					/* characters allowed only in the resource */
					if (implement_me)
						/* We're good */
						break;
				}

				/* characters allowed only in the node */
				if (implement_me) {
					/*
					 * Ok, this character is valid, but only if it's a part
					 * of the node and not the domain.  But we don't know
					 * if "c" is a part of the node or the domain until after
					 * we've found the @.  So set a flag for now and check
					 * that we found an @ later.
					 */
					node_is_required = TRUE;
					break;
				}
#endif

				/*
				 * Hmm, this character is a bit more exotic.  Better fall
				 * back to using the more expensive UTF-8 compliant
				 * stringprep functions.
				 */
				needs_validation = TRUE;
				break;
		}
	}

#if 0
	if (node_is_required && at == NULL)
		/* Found invalid characters in the domain */
		return NULL;
#endif

	if (!needs_validation) {
		/* JID is made of only ASCII characters--just lowercase and return */
		jid = g_new0(JabberID, 1);

		if (at) {
			jid->node = g_ascii_strdown(str, at - str);
			if (slash) {
				jid->domain = g_ascii_strdown(at + 1, slash - (at + 1));
				if (*(slash + 1))
					jid->resource = g_strdup(slash + 1);
			} else {
				jid->domain = g_ascii_strdown(at + 1, -1);
			}
		} else {
			if (slash) {
				jid->domain = g_ascii_strdown(str, slash - str);
				if (*(slash + 1))
					jid->resource = g_strdup(slash + 1);
			} else {
				jid->domain = g_ascii_strdown(str, -1);
			}
		}
		return jid;
	}

	/*
	 * If we get here, there are some non-ASCII chars in the string, so
	 * we'll need to validate it, normalize, and finally do a full jabber
	 * nodeprep on the jid.
	 */

	if (!g_utf8_validate(str, -1, NULL))
		return NULL;

	return jabber_id_prep(str, at, slash, c /* points to the null */);
}

void
jabber_id_free(JabberID *jid)
{
	if(jid) {
		g_free(jid->node);
		g_free(jid->domain);
		g_free(jid->resource);
		g_free(jid);
	}
}


gboolean
jabber_id_equal(const JabberID *jid1, const JabberID *jid2)
{
	if (!jid1 && !jid2) {
		/* Both are null therefore equal */
		return TRUE;
	}

	if (!jid1 || !jid2) {
		/* One is null, other is non-null, therefore not equal */
		return FALSE;
	}

	return purple_strequal(jid1->node, jid2->node) &&
			purple_strequal(jid1->domain, jid2->domain) &&
			purple_strequal(jid1->resource, jid2->resource);
}

char *jabber_get_domain(const char *in)
{
	JabberID *jid = jabber_id_new(in);
	char *out;

	if (!jid)
		return NULL;

	out = g_strdup(jid->domain);
	jabber_id_free(jid);

	return out;
}

char *jabber_get_resource(const char *in)
{
	JabberID *jid = jabber_id_new(in);
	char *out;

	if(!jid)
		return NULL;

	out = g_strdup(jid->resource);
	jabber_id_free(jid);

	return out;
}

JabberID *
jabber_id_to_bare_jid(const JabberID *jid)
{
	JabberID *result = g_new0(JabberID, 1);

	result->node = g_strdup(jid->node);
	result->domain = g_strdup(jid->domain);

	return result;
}

char *
jabber_get_bare_jid(const char *in)
{
	JabberID *jid = jabber_id_new(in);
	char *out;

	if (!jid)
		return NULL;
	out = jabber_id_get_bare_jid(jid);
	jabber_id_free(jid);

	return out;
}

char *
jabber_id_get_bare_jid(const JabberID *jid)
{
	g_return_val_if_fail(jid != NULL, NULL);

	return g_strconcat(jid->node ? jid->node : "",
	                   jid->node ? "@" : "",
	                   jid->domain,
	                   NULL);
}

char *
jabber_id_get_full_jid(const JabberID *jid)
{
	g_return_val_if_fail(jid != NULL, NULL);

	return g_strconcat(jid->node ? jid->node : "",
	                   jid->node ? "@" : "",
	                   jid->domain,
	                   jid->resource ? "/" : "",
	                   jid->resource ? jid->resource : "",
	                   NULL);
}

gboolean
jabber_jid_is_domain(const char *jid)
{
	const char *c;

	for (c = jid; *c; ++c) {
		if (*c == '@' || *c == '/')
			return FALSE;
	}

	return TRUE;
}


JabberID *
jabber_id_new(const char *str)
{
	return jabber_id_new_internal(str, FALSE);
}

const char *jabber_normalize(const PurpleAccount *account, const char *in)
{
	PurpleConnection *gc = account ? account->gc : NULL;
	JabberStream *js = gc ? gc->proto_data : NULL;
	static char buf[3072]; /* maximum legal length of a jabber jid */
	JabberID *jid;

	jid = jabber_id_new_internal(in, TRUE);
	if(!jid)
		return NULL;

	if(js && jid->node && jid->resource &&
			jabber_chat_find(js, jid->node, jid->domain))
		g_snprintf(buf, sizeof(buf), "%s@%s/%s", jid->node, jid->domain,
				jid->resource);
	else
		g_snprintf(buf, sizeof(buf), "%s%s%s", jid->node ? jid->node : "",
				jid->node ? "@" : "", jid->domain);

	jabber_id_free(jid);

	return buf;
}

gboolean
jabber_is_own_server(JabberStream *js, const char *str)
{
	JabberID *jid;
	gboolean equal;

	if (str == NULL)
		return FALSE;

	g_return_val_if_fail(*str != '\0', FALSE);

	jid = jabber_id_new(str);
	if (!jid)
		return FALSE;

	equal = (jid->node == NULL &&
	         purple_strequal(jid->domain, js->user->domain) &&
	         jid->resource == NULL);
	jabber_id_free(jid);
	return equal;
}

gboolean
jabber_is_own_account(JabberStream *js, const char *str)
{
	JabberID *jid;
	gboolean equal;

	if (str == NULL)
		return TRUE;

	g_return_val_if_fail(*str != '\0', FALSE);

	jid = jabber_id_new(str);
	if (!jid)
		return FALSE;

	equal = (purple_strequal(jid->node, js->user->node) &&
	         purple_strequal(jid->domain, js->user->domain) &&
	         (jid->resource == NULL ||
	             purple_strequal(jid->resource, js->user->resource)));
	jabber_id_free(jid);
	return equal;
}

static const struct {
		const char *status_id; /* link to core */
		const char *show; /* The show child's cdata in a presence stanza */
		const char *readable; /* readable representation */
		JabberBuddyState state;
} jabber_statuses[] = {
	{ "offline",       NULL,   N_("Offline"),        JABBER_BUDDY_STATE_UNAVAILABLE },
	{ "available",     NULL,   N_("Available"),      JABBER_BUDDY_STATE_ONLINE},
	{ "freeforchat",   "chat", N_("Chatty"),         JABBER_BUDDY_STATE_CHAT },
	{ "away",          "away", N_("Away"),           JABBER_BUDDY_STATE_AWAY },
	{ "extended_away", "xa",   N_("Extended Away"),  JABBER_BUDDY_STATE_XA },
	{ "dnd",           "dnd",  N_("Do Not Disturb"), JABBER_BUDDY_STATE_DND },
	{ "error",         NULL,   N_("Error"),          JABBER_BUDDY_STATE_ERROR }
};

const char *
jabber_buddy_state_get_name(const JabberBuddyState state)
{
	gsize i;
	for (i = 0; i < G_N_ELEMENTS(jabber_statuses); ++i)
		if (jabber_statuses[i].state == state)
			return _(jabber_statuses[i].readable);

	return _("Unknown");
}

JabberBuddyState
jabber_buddy_status_id_get_state(const char *id)
{
	gsize i;
	if (!id)
		return JABBER_BUDDY_STATE_UNKNOWN;

	for (i = 0; i < G_N_ELEMENTS(jabber_statuses); ++i)
		if (purple_strequal(id, jabber_statuses[i].status_id))
			return jabber_statuses[i].state;

	return JABBER_BUDDY_STATE_UNKNOWN;
}

JabberBuddyState jabber_buddy_show_get_state(const char *id)
{
	gsize i;

	g_return_val_if_fail(id != NULL, JABBER_BUDDY_STATE_UNKNOWN);

	for (i = 0; i < G_N_ELEMENTS(jabber_statuses); ++i)
		if (jabber_statuses[i].show && purple_strequal(id, jabber_statuses[i].show))
			return jabber_statuses[i].state;

	purple_debug_warning("jabber", "Invalid value of presence <show/> "
	                     "attribute: %s\n", id);
	return JABBER_BUDDY_STATE_UNKNOWN;
}

const char *
jabber_buddy_state_get_show(JabberBuddyState state)
{
	gsize i;
	for (i = 0; i < G_N_ELEMENTS(jabber_statuses); ++i)
		if (state == jabber_statuses[i].state)
			return jabber_statuses[i].show;

	return NULL;
}

const char *
jabber_buddy_state_get_status_id(JabberBuddyState state)
{
	gsize i;
	for (i = 0; i < G_N_ELEMENTS(jabber_statuses); ++i)
		if (state == jabber_statuses[i].state)
			return jabber_statuses[i].status_id;

	return NULL;
}

char *
jabber_calculate_data_hash(gconstpointer data, size_t len,
    const gchar *hash_algo)
{
	PurpleCipherContext *context;
	static gchar digest[129]; /* 512 bits hex + \0 */

	context = purple_cipher_context_new_by_name(hash_algo, NULL);
	if (context == NULL)
	{
		purple_debug_error("jabber", "Could not find %s cipher\n", hash_algo);
		g_return_val_if_reached(NULL);
	}

	/* Hash the data */
	purple_cipher_context_append(context, data, len);
	if (!purple_cipher_context_digest_to_str(context, sizeof(digest), digest, NULL))
	{
		purple_debug_error("jabber", "Failed to get digest for %s cipher.\n",
		    hash_algo);
		g_return_val_if_reached(NULL);
	}
	purple_cipher_context_destroy(context);

	return g_strdup(digest);
}

