/*
 * purple - Jabber Protocol Plugin
 *
 * XEP-0388 Extensible SASL Profile (SASL2), XEP-0386 Bind 2 and
 * XEP-0484 Fast Authentication Streamlining Tokens (FAST)
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

/*
 * One round trip from TLS to a bound, SM-enabled session:
 *
 *   <authenticate mechanism='SCRAM-SHA-256-PLUS'>
 *     <initial-response/>
 *     <user-agent id='<stable uuid>'><software/><device/></user-agent>
 *     <request-token xmlns='urn:xmpp:fast:0' mechanism='HT-SHA-256-EXPR'/>
 *     <resume xmlns='urn:xmpp:sm:3' .../>          (if a session dropped)
 *     <bind xmlns='urn:xmpp:bind:0'><tag>pidgin4</tag>
 *       <enable xmlns='urn:xmpp:carbons:2'/> <enable xmlns='urn:xmpp:sm:3'/>
 *     </bind>
 *   </authenticate>
 *
 * The SASL mechanisms themselves are the ordinary JabberSaslMechs (SCRAM,
 * PLAIN): their <auth/>/<response/> output is re-wrapped in the SASL2
 * namespace.  FAST adds the HT-SHA-256-{NONE,ENDP,EXPR} mechanisms, used
 * only here.
 *
 * State that must survive restarts (the user-agent id and the FAST token)
 * goes through jabber_kv_load/store(): with no UI store behind it (the
 * GTK 2 UI) there is no user-agent id and no FAST, just SASL2 + Bind2.
 *
 * SASL2 is only used on encrypted streams and only with Bind2; everything
 * else takes the legacy SASL + stream restart + RFC 6120 bind path.
 */

#include "internal.h"

#include "account.h"
#include "debug.h"
#include "util.h"
#include "xmlnode.h"

#include "auth.h"
#include "disco.h"
#include "jabber.h"
#include "jutil.h"
#include "kvstore.h"
#include "namespaces.h"
#include "sasl2.h"
#include "stream_management.h"

#define HT_SHA256_LEN 32

/**************************************************************************
 * HT-SHA-256-* (draft-schmaus-kitten-sasl-ht)
 **************************************************************************/

guchar *
jabber_ht_sha256(const char *token, const char *label,
                 const guchar *cb, gsize cb_len)
{
	GHmac *hmac;
	guchar *out = g_new0(guchar, HT_SHA256_LEN);
	gsize out_len = HT_SHA256_LEN;

	hmac = g_hmac_new(G_CHECKSUM_SHA256, (const guchar *)token, strlen(token));
	g_hmac_update(hmac, (const guchar *)label, strlen(label));
	if (cb && cb_len)
		g_hmac_update(hmac, cb, cb_len);
	g_hmac_get_digest(hmac, out, &out_len);
	g_hmac_unref(hmac);

	return out;
}

const char *
jabber_ht_mech_cb_type(const char *mech)
{
	if (purple_strequal(mech, "HT-SHA-256-NONE"))
		return "";
	if (purple_strequal(mech, "HT-SHA-256-ENDP"))
		return "tls-server-end-point";
	if (purple_strequal(mech, "HT-SHA-256-EXPR"))
		return "tls-exporter";
	/* -UNIQ needs tls-unique, which ssl-nss can't give. */
	return NULL;
}

static gboolean
ht_cb_available(const char *mech, gboolean have_exporter, gboolean have_endpoint)
{
	const char *type = jabber_ht_mech_cb_type(mech);

	if (type == NULL)
		return FALSE;
	if (*type == '\0')
		return TRUE;
	if (purple_strequal(type, "tls-exporter"))
		return have_exporter;
	return have_endpoint;
}

static gboolean
mech_listed(GSList *mechs, const char *mech)
{
	return mech && g_slist_find_custom(mechs, mech, (GCompareFunc)strcmp) != NULL;
}

const char *
jabber_fast_pick_request_mech(GSList *server_mechs, gboolean have_exporter,
                              gboolean have_endpoint)
{
	static const char *const preferred[] = {
		"HT-SHA-256-EXPR",
		"HT-SHA-256-ENDP",
		"HT-SHA-256-NONE",
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(preferred); i++) {
		if (mech_listed(server_mechs, preferred[i]) &&
				ht_cb_available(preferred[i], have_exporter, have_endpoint))
			return preferred[i];
	}

	return NULL;
}

gboolean
jabber_fast_token_usable(GSList *server_mechs, const char *mech,
                         const char *token, const char *expiry, gint64 now,
                         gboolean have_exporter, gboolean have_endpoint)
{
	if (token == NULL || *token == '\0' || mech == NULL)
		return FALSE;
	if (!mech_listed(server_mechs, mech))
		return FALSE;
	if (!ht_cb_available(mech, have_exporter, have_endpoint))
		return FALSE;

	if (expiry != NULL && *expiry != '\0') {
		GDateTime *dt = g_date_time_new_from_iso8601(expiry, NULL);
		gboolean expired;

		if (dt == NULL)
			return FALSE;
		/* A minute of slack for clock skew and the round trip. */
		expired = g_date_time_to_unix(dt) <= now + 60;
		g_date_time_unref(dt);
		if (expired)
			return FALSE;
	}

	return TRUE;
}

typedef struct {
	gchar *token;
	guchar *cb;
	gsize cb_len;
} JabberHtData;

static void
ht_data_free(JabberHtData *data)
{
	if (data == NULL)
		return;
	if (data->token) {
		memset(data->token, 0, strlen(data->token));
		g_free(data->token);
	}
	g_free(data->cb);
	g_free(data);
}

static JabberSaslState
ht_start(JabberStream *js, xmlnode *mechanisms, xmlnode **out, char **error)
{
	JabberHtData *data = js->auth_mech_data;
	const char *cb_type = jabber_ht_mech_cb_type(js->auth_mech->name);
	gchar *authcid;
	guchar *hashed;
	GString *msg;
	gchar *enc;
	xmlnode *reply;

	g_return_val_if_fail(data != NULL && data->token != NULL, JABBER_SASL_STATE_FAIL);

	if (cb_type == NULL) {
		*error = g_strdup(_("Unsupported FAST mechanism"));
		return JABBER_SASL_STATE_FAIL;
	}
	if (*cb_type != '\0') {
		data->cb = jabber_auth_get_channel_binding(js, cb_type, NULL, &data->cb_len);
		if (data->cb == NULL) {
			*error = g_strdup(_("TLS channel binding is not available"));
			return JABBER_SASL_STATE_FAIL;
		}
	}

	authcid = jabber_saslprep(js->user->node);
	if (authcid == NULL) {
		*error = g_strdup(_("Unable to canonicalize username"));
		return JABBER_SASL_STATE_FAIL;
	}

	/* authcid NUL HMAC(token, "Initiator" || cb-data) */
	hashed = jabber_ht_sha256(data->token, "Initiator", data->cb, data->cb_len);
	msg = g_string_new(authcid);
	g_string_append_c(msg, '\0');
	g_string_append_len(msg, (gchar *)hashed, HT_SHA256_LEN);
	enc = purple_base64_encode((guchar *)msg->str, msg->len);
	g_free(hashed);
	g_free(authcid);
	g_string_free(msg, TRUE);

	reply = xmlnode_new("auth");
	xmlnode_set_namespace(reply, NS_XMPP_SASL);
	xmlnode_set_attrib(reply, "mechanism", js->auth_mech->name);
	xmlnode_insert_data(reply, enc, -1);
	g_free(enc);

	*out = reply;
	return JABBER_SASL_STATE_CONTINUE;
}

static JabberSaslState
ht_handle_success(JabberStream *js, xmlnode *packet, char **error)
{
	JabberHtData *data = js->auth_mech_data;
	gchar *enc;
	guchar *got, *expected;
	gsize len = 0;
	gboolean ok;

	/* Mutual authentication: the server proves it knows the token too. */
	enc = xmlnode_get_data(packet);
	got = enc ? purple_base64_decode(enc, &len) : NULL;
	g_free(enc);

	expected = jabber_ht_sha256(data->token, "Responder", data->cb, data->cb_len);
	ok = (got != NULL && len == HT_SHA256_LEN &&
	      memcmp(got, expected, HT_SHA256_LEN) == 0);
	g_free(got);
	g_free(expected);

	if (!ok) {
		*error = g_strdup(_("The server could not prove it knows the login token"));
		return JABBER_SASL_STATE_FAIL;
	}

	return JABBER_SASL_STATE_OK;
}

static void
ht_dispose(JabberStream *js)
{
	ht_data_free(js->auth_mech_data);
	js->auth_mech_data = NULL;
}

#define HT_MECH(var, mechname) \
static JabberSaslMech var = { \
	0, /* priority; never in the legacy list */ \
	mechname, \
	ht_start, \
	NULL, /* handle_challenge */ \
	ht_handle_success, \
	NULL, /* handle_failure */ \
	ht_dispose \
}

HT_MECH(ht_sha256_none_mech, "HT-SHA-256-NONE");
HT_MECH(ht_sha256_endp_mech, "HT-SHA-256-ENDP");
HT_MECH(ht_sha256_expr_mech, "HT-SHA-256-EXPR");

#undef HT_MECH

static JabberSaslMech *
ht_mech_by_name(const char *name)
{
	if (purple_strequal(name, ht_sha256_none_mech.name))
		return &ht_sha256_none_mech;
	if (purple_strequal(name, ht_sha256_endp_mech.name))
		return &ht_sha256_endp_mech;
	if (purple_strequal(name, ht_sha256_expr_mech.name))
		return &ht_sha256_expr_mech;
	return NULL;
}

/**************************************************************************
 * Persistent state
 **************************************************************************/

/*
 * The user-agent id must be stable, so it only exists when the UI keeps
 * it (jabber_kv_*); NULL otherwise, and then FAST is off too.
 */
static gchar *
sasl2_user_agent_id(PurpleAccount *account)
{
	gchar *id = jabber_kv_load(account, JABBER_KV_SASL2_USER_AGENT_ID);
	gchar *check;

	if (id != NULL && *id != '\0')
		return id;
	g_free(id);

	id = g_uuid_string_random();
	jabber_kv_store(account, JABBER_KV_SASL2_USER_AGENT_ID, id);
	check = jabber_kv_load(account, JABBER_KV_SASL2_USER_AGENT_ID);
	if (!purple_strequal(check, id)) {
		purple_debug_info("jabber", "SASL2: no persistent store, "
		                  "no user-agent id and no FAST\n");
		g_free(check);
		g_free(id);
		return NULL;
	}
	g_free(check);
	return id;
}

static void
fast_forget_token(PurpleAccount *account)
{
	jabber_kv_store(account, JABBER_KV_FAST_TOKEN, NULL);
	jabber_kv_store(account, JABBER_KV_FAST_MECHANISM, NULL);
	jabber_kv_store(account, JABBER_KV_FAST_EXPIRY, NULL);
}

static void
channel_binding_availability(JabberStream *js, gboolean *have_exporter,
                             gboolean *have_endpoint)
{
	guchar *cb;
	gsize len;

	cb = jabber_auth_get_channel_binding(js, "tls-exporter", NULL, &len);
	*have_exporter = (cb != NULL);
	g_free(cb);
	cb = jabber_auth_get_channel_binding(js, "tls-server-end-point", NULL, &len);
	*have_endpoint = (cb != NULL);
	g_free(cb);
}

/**************************************************************************
 * The exchange
 **************************************************************************/

/* The text of child @a name, or NULL. */
static char *
child_data(xmlnode *parent, const char *name)
{
	xmlnode *child = xmlnode_get_child(parent, name);
	return child ? xmlnode_get_data(child) : NULL;
}

static void
sasl2_dispose_mech(JabberStream *js)
{
	if (js->auth_mech && js->auth_mech->dispose)
		js->auth_mech->dispose(js);
	js->auth_mech = NULL;
	js->auth_mech_data = NULL;
}

static GSList *
sasl2_mechanism_names(xmlnode *parent)
{
	GSList *names = NULL;
	xmlnode *mech;

	for (mech = xmlnode_get_child(parent, "mechanism"); mech;
			mech = xmlnode_get_next_twin(mech)) {
		char *name = xmlnode_get_data(mech);
		if (name && *name)
			names = g_slist_append(names, name);
		else
			g_free(name);
	}

	return names;
}

static void
sasl2_add_user_agent(xmlnode *auth, const char *id)
{
	xmlnode *ua, *node;
	const char *host = g_get_host_name();
	gchar *device;
	const char *dot;

	ua = xmlnode_new_child(auth, "user-agent");
	if (id)
		xmlnode_set_attrib(ua, "id", id);

	node = xmlnode_new_child(ua, "software");
	xmlnode_insert_data(node, "Pidgin", -1);

	/* The short host name, as for the __HOSTNAME__ resource. */
	dot = strchr(host, '.');
	device = dot ? g_strndup(host, dot - host) : g_strdup(host);
	if (*device) {
		node = xmlnode_new_child(ua, "device");
		xmlnode_insert_data(node, device, -1);
	}
	g_free(device);
}

static void
sasl2_send_authenticate(JabberStream *js)
{
	PurpleAccount *account = purple_connection_get_account(js->gc);
	xmlnode *auth, *node, *response = NULL;
	gchar *ua_id, *initial;
	gboolean use_fast = FALSE;
	gboolean have_exporter, have_endpoint;
	GSList *names;
	JabberSaslState state;
	char *msg = NULL;

	ua_id = sasl2_user_agent_id(account);
	channel_binding_availability(js, &have_exporter, &have_endpoint);

	sasl2_dispose_mech(js);
	g_free(js->fast_request_mech);
	js->fast_request_mech = NULL;

	/* FAST first, if we have a token for it. */
	if (ua_id && js->fast_mechs && !js->fast_failed) {
		gchar *token = jabber_kv_load(account, JABBER_KV_FAST_TOKEN);
		gchar *mech = jabber_kv_load(account, JABBER_KV_FAST_MECHANISM);
		gchar *expiry = jabber_kv_load(account, JABBER_KV_FAST_EXPIRY);

		if (jabber_fast_token_usable(js->fast_mechs, mech, token, expiry,
		                             g_get_real_time() / G_USEC_PER_SEC,
		                             have_exporter, have_endpoint)) {
			JabberHtData *data = g_new0(JabberHtData, 1);
			data->token = token;
			token = NULL;
			js->auth_mech = ht_mech_by_name(mech);
			js->auth_mech_data = data;
			use_fast = TRUE;
			purple_debug_info("jabber", "SASL2: logging in with a FAST token (%s)\n", mech);
		} else if (token != NULL) {
			purple_debug_info("jabber", "SASL2: stored FAST token is not usable "
			                  "(mechanism %s, expiry %s)\n",
			                  mech ? mech : "(none)", expiry ? expiry : "(none)");
		}

		if (token) {
			memset(token, 0, strlen(token));
			g_free(token);
		}
		g_free(mech);
		g_free(expiry);
	}

	if (!use_fast) {
		names = sasl2_mechanism_names(js->sasl2_features);
		js->auth_mech = jabber_auth_pick_mech(js, names, TRUE);
		g_slist_free_full(names, g_free);

		if (js->auth_mech == NULL) {
			g_free(ua_id);
			purple_connection_error_reason(js->gc,
				PURPLE_CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE,
				_("Server does not use any supported authentication method"));
			return;
		}
	}

	state = js->auth_mech->start(js, js->sasl2_features, &response, &msg);
	if (state == JABBER_SASL_STATE_FAIL || response == NULL) {
		if (use_fast) {
			purple_debug_warning("jabber", "SASL2: FAST failed to start (%s); "
			                     "using the password\n", msg ? msg : "?");
			g_free(msg);
			if (response)
				xmlnode_free(response);
			js->fast_failed = TRUE;
			g_free(ua_id);
			sasl2_send_authenticate(js);
			return;
		}
		g_free(ua_id);
		purple_connection_error_reason(js->gc,
				PURPLE_CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE,
				msg ? msg : _("Unknown Error"));
		g_free(msg);
		if (response)
			xmlnode_free(response);
		return;
	}
	g_free(msg);

	initial = xmlnode_get_data(response);
	xmlnode_free(response);

	auth = xmlnode_new("authenticate");
	xmlnode_set_namespace(auth, NS_SASL2);
	xmlnode_set_attrib(auth, "mechanism", js->auth_mech->name);

	node = xmlnode_new_child(auth, "initial-response");
	/* An empty initial response is sent as "=" (RFC 6120 6.4.2). */
	xmlnode_insert_data(node, (initial && *initial) ? initial : "=", -1);
	g_free(initial);

	sasl2_add_user_agent(auth, ua_id);

	if (use_fast) {
		node = xmlnode_new_child(auth, "fast");
		xmlnode_set_namespace(node, NS_FAST);
	} else if (ua_id && js->fast_mechs) {
		const char *req = jabber_fast_pick_request_mech(js->fast_mechs,
				have_exporter, have_endpoint);
		if (req) {
			node = xmlnode_new_child(auth, "request-token");
			xmlnode_set_namespace(node, NS_FAST);
			xmlnode_set_attrib(node, "mechanism", req);
			js->fast_request_mech = g_strdup(req);
		}
	}
	js->fast_attempt = use_fast;

	/* XEP-0198 in SASL2: resume a dropped session, else bind anew. */
	js->sasl2_sm_resume_sent = FALSE;
	if (js->sasl2_inline_sm && jabber_sm_can_resume(js)) {
		xmlnode_insert_child(auth, jabber_sm_resume_node(js));
		js->sasl2_sm_resume_sent = TRUE;
	}

	node = xmlnode_new_child(auth, "bind");
	xmlnode_set_namespace(node, NS_BIND2);
	xmlnode_insert_data(xmlnode_new_child(node, "tag"), JABBER_BIND2_TAG, -1);
	if (js->sasl2_bind2_carbons) {
		xmlnode *enable = xmlnode_new_child(node, "enable");
		xmlnode_set_namespace(enable, NS_CARBONS);
	}
	if (js->sasl2_bind2_sm)
		xmlnode_insert_child(node, jabber_sm_inline_enable_node(js));

	g_free(ua_id);

	jabber_send(js, auth);
	xmlnode_free(auth);
}

gboolean
jabber_sasl2_start(JabberStream *js, xmlnode *features)
{
	xmlnode *authn, *inl, *bind2, *node;
	GSList *names;
	JabberSaslMech *mech;

	authn = xmlnode_get_child_with_namespace(features, "authentication", NS_SASL2);
	if (authn == NULL)
		return FALSE;

	/* Over TLS only (the plaintext-PLAIN prompt and friends stay on the
	 * legacy path), and not over BOSH. */
	if (js->bosh || !jabber_stream_is_ssl(js)) {
		purple_debug_info("jabber", "SASL2 offered, but not on an encrypted "
		                  "stream; using legacy SASL\n");
		return FALSE;
	}

	inl = xmlnode_get_child(authn, "inline");
	bind2 = inl ? xmlnode_get_child_with_namespace(inl, "bind", NS_BIND2) : NULL;
	if (bind2 == NULL) {
		purple_debug_info("jabber", "SASL2 offered without Bind2; using legacy SASL\n");
		return FALSE;
	}

	names = sasl2_mechanism_names(authn);
	mech = jabber_auth_pick_mech(js, names, TRUE);
	g_slist_free_full(names, g_free);
	if (mech == NULL) {
		purple_debug_info("jabber", "SASL2 offered without a mechanism we use "
		                  "in it; using legacy SASL\n");
		return FALSE;
	}

	/* What Bind2 can do inline. */
	js->sasl2_bind2_carbons = js->sasl2_bind2_sm = js->sasl2_bind2_csi = FALSE;
	node = xmlnode_get_child(bind2, "inline");
	for (node = node ? xmlnode_get_child(node, "feature") : NULL; node;
			node = xmlnode_get_next_twin(node)) {
		const char *var = xmlnode_get_attrib(node, "var");
		if (purple_strequal(var, NS_CARBONS))
			js->sasl2_bind2_carbons = TRUE;
		else if (purple_strequal(var, NS_STREAM_MANAGEMENT))
			js->sasl2_bind2_sm = TRUE;
		else if (purple_strequal(var, NS_CSI))
			js->sasl2_bind2_csi = TRUE;
	}

	js->sasl2_inline_sm =
		xmlnode_get_child_with_namespace(inl, "sm", NS_STREAM_MANAGEMENT) != NULL;

	g_slist_free_full(js->fast_mechs, g_free);
	node = xmlnode_get_child_with_namespace(inl, "fast", NS_FAST);
	js->fast_mechs = node ? sasl2_mechanism_names(node) : NULL;

	if (js->sasl2_features)
		xmlnode_free(js->sasl2_features);
	js->sasl2_features = xmlnode_copy(authn);
	js->sasl2 = TRUE;

	purple_debug_info("jabber", "Using SASL2 + Bind2 (inline: carbons %d, "
	                  "sm enable %d, sm resume %d, csi %d, fast %d)\n",
	                  js->sasl2_bind2_carbons, js->sasl2_bind2_sm,
	                  js->sasl2_inline_sm, js->sasl2_bind2_csi,
	                  js->fast_mechs != NULL);

	jabber_stream_set_state(js, JABBER_STREAM_AUTHENTICATING);
	sasl2_send_authenticate(js);
	return TRUE;
}

static void
sasl2_handle_challenge(JabberStream *js, xmlnode *packet)
{
	xmlnode *response = NULL;
	char *msg = NULL;
	JabberSaslState state;

	if (js->auth_mech == NULL || js->auth_mech->handle_challenge == NULL) {
		purple_connection_error_reason(js->gc,
			PURPLE_CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE,
			_("Invalid challenge from server"));
		return;
	}

	state = js->auth_mech->handle_challenge(js, packet, &response, &msg);
	if (response) {
		/* <response/> or <abort/>, same names in SASL2 */
		xmlnode_set_namespace(response, NS_SASL2);
		jabber_send(js, response);
		xmlnode_free(response);
	}
	if (state == JABBER_SASL_STATE_FAIL)
		purple_connection_error_reason(js->gc,
			PURPLE_CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE,
			msg ? msg : _("Invalid challenge from server"));
	g_free(msg);
}

static void
sasl2_handle_success(JabberStream *js, xmlnode *packet)
{
	PurpleAccount *account = purple_connection_get_account(js->gc);
	xmlnode *node, *bound, *sm;
	gchar *authz;

	/* Let the mechanism check the server's final message. */
	if (js->auth_mech && js->auth_mech->handle_success) {
		xmlnode *tmp = xmlnode_new("success");
		char *data = child_data(packet, "additional-data");
		char *msg = NULL;
		JabberSaslState state;

		xmlnode_set_namespace(tmp, NS_XMPP_SASL);
		if (data)
			xmlnode_insert_data(tmp, data, -1);
		g_free(data);

		state = js->auth_mech->handle_success(js, tmp, &msg);
		xmlnode_free(tmp);
		if (state != JABBER_SASL_STATE_OK) {
			if (js->fast_attempt)
				fast_forget_token(account);
			purple_connection_error_reason(js->gc,
				PURPLE_CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE,
				msg ? msg : _("Invalid response from server"));
			g_free(msg);
			return;
		}
		g_free(msg);
	}

	js->sasl2 = FALSE;
	purple_debug_info("jabber", "SASL2: authenticated with %s\n", js->auth_mech->name);

	/* FAST: a new token, asked for or rotated. */
	node = xmlnode_get_child_with_namespace(packet, "token", NS_FAST);
	if (node) {
		const char *token = xmlnode_get_attrib(node, "token");
		const char *expiry = xmlnode_get_attrib(node, "expiry");
		const char *mech = js->fast_attempt ? js->auth_mech->name
		                                    : js->fast_request_mech;
		if (token && *token && mech) {
			jabber_kv_store(account, JABBER_KV_FAST_TOKEN, token);
			jabber_kv_store(account, JABBER_KV_FAST_MECHANISM, mech);
			jabber_kv_store(account, JABBER_KV_FAST_EXPIRY, expiry);
			purple_debug_info("jabber", "FAST: stored a %s token (expires %s)\n",
			                  mech, expiry ? expiry : "never");
		}
	}

	/* XEP-0198 resumption takes the place of binding. */
	sm = xmlnode_get_child_with_namespace(packet, "resumed", NS_STREAM_MANAGEMENT);
	if (sm && js->sasl2_sm_resume_sent) {
		jabber_sm_resumed(js, sm);
		return;
	}
	if (js->sasl2_sm_resume_sent) {
		jabber_sm_resume_failed(js,
			xmlnode_get_child_with_namespace(packet, "failed", NS_STREAM_MANAGEMENT));
	}

	bound = xmlnode_get_child_with_namespace(packet, "bound", NS_BIND2);
	authz = child_data(packet, "authorization-identifier");
	if (bound == NULL || authz == NULL) {
		g_free(authz);
		purple_connection_error_reason(js->gc,
			PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
			_("Invalid response from server"));
		return;
	}
	if (!jabber_stream_set_bound_jid(js, authz)) {
		g_free(authz);
		return;
	}
	g_free(authz);

	/* Inline stream management enable */
	sm = xmlnode_get_child_with_namespace(bound, "enabled", NS_STREAM_MANAGEMENT);
	if (sm) {
		jabber_sm_inline_enable_sent(js);
		jabber_sm_process_packet(js, sm);
	} else {
		if (js->sasl2_bind2_sm)
			purple_debug_warning("jabber", "Bind2: stream management was not enabled\n");
		js->sm_state = SM_DISABLED;
	}

	js->carbons_enabled_inline = js->sasl2_bind2_carbons;
	if (js->sasl2_bind2_csi)
		js->csi_supported = TRUE;

	jabber_stream_set_state(js, JABBER_STREAM_POST_AUTH);
	jabber_sm_session_started(js);
	jabber_disco_items_server(js);
}

static void
sasl2_handle_failure(JabberStream *js, xmlnode *packet)
{
	PurpleConnectionError reason = PURPLE_CONNECTION_ERROR_NETWORK_ERROR;
	xmlnode *legacy;
	char *msg;

	if (js->fast_attempt) {
		/* The token is dead (expired, revoked, password changed).  Drop it
		 * and try again with the password on this stream. */
		char *text = child_data(packet, "text");
		purple_debug_info("jabber", "FAST login rejected (%s); using the password\n",
		                  text ? text : "no reason given");
		g_free(text);
		fast_forget_token(purple_connection_get_account(js->gc));
		js->fast_failed = TRUE;
		js->fast_attempt = FALSE;
		sasl2_send_authenticate(js);
		return;
	}

	/* See jabber_auth_handle_failure(): retry once without channel
	 * binding. */
	if (js->auth_mech && g_str_has_suffix(js->auth_mech->name, "-PLUS") &&
			!js->auth_plus_failed) {
		purple_debug_warning("jabber", "SASL2: %s failed; retrying without channel binding\n",
		                     js->auth_mech->name);
		js->auth_plus_failed = TRUE;
		sasl2_send_authenticate(js);
		return;
	}

		/* Same conditions as legacy SASL <failure/>. */
	legacy = xmlnode_copy(packet);
	xmlnode_set_namespace(legacy, NS_XMPP_SASL);
	msg = jabber_parse_error(js, legacy, &reason);
	xmlnode_free(legacy);

	purple_connection_error_reason(js->gc, reason,
		msg ? msg : _("Invalid response from server"));
	g_free(msg);
}

void
jabber_sasl2_process_packet(JabberStream *js, xmlnode *packet)
{
	const char *name = packet->name;

	if (purple_strequal(name, "challenge")) {
		sasl2_handle_challenge(js, packet);
	} else if (purple_strequal(name, "success")) {
		sasl2_handle_success(js, packet);
	} else if (purple_strequal(name, "failure")) {
		sasl2_handle_failure(js, packet);
	} else if (purple_strequal(name, "continue")) {
		/* A task such as a second factor or a password change.  Not
		 * implemented yet. */
		xmlnode *tasks = xmlnode_get_child(packet, "tasks");
		xmlnode *task;
		GString *list = g_string_new(NULL);

		for (task = tasks ? xmlnode_get_child(tasks, "task") : NULL; task;
				task = xmlnode_get_next_twin(task)) {
			char *t = xmlnode_get_data(task);
			if (list->len)
				g_string_append(list, ", ");
			g_string_append(list, t ? t : "?");
			g_free(t);
		}
		purple_debug_error("jabber", "SASL2: server wants additional tasks (%s), "
		                   "which are not supported\n", list->str);
		g_string_free(list, TRUE);

		purple_connection_error_reason(js->gc,
			PURPLE_CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE,
			_("The server requires an additional login step (such as a "
			  "second factor) that is not supported"));
	} else {
		purple_debug_warning("jabber", "Unknown SASL2 element %s\n", name);
	}
}
