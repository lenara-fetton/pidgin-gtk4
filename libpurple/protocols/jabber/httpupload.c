/*
 * purple - Jabber Protocol Plugin
 *
 * XEP-0363 HTTP File Upload, over libsoup 3.
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

#include "internal.h"

#include <gio/gio.h>
#include <libsoup/soup.h>

#include "account.h"
#include "connection.h"
#include "conversation.h"
#include "debug.h"
#include "eventloop.h"
#include "ft.h"
#include "proxy.h"
#include "util.h"

#include "chat.h"
#include "httpupload.h"
#include "iq.h"
#include "jabber.h"
#include "namespaces.h"
#include "sfs.h"

/* What an upload service looks like, per connection. */
typedef struct {
	PurpleConnection *gc;
	char *jid;              /* NULL until disco found one */
	goffset max_file_size;  /* 0: no stated limit */
	GList *uploads;         /* JabberHttpUpload * */
} JabberHttpUploadService;

/* One transfer on its way through slot request and PUT. */
typedef struct {
	PurpleXfer *xfer;       /* we hold a reference */
	JabberStream *js;
	JabberHttpUploadService *service;
	char *to;               /* bare/full JID or room JID for the message */
	gboolean groupchat;

	JabberHttpUploadFallback fallback;
	void (*orig_cancel_send)(PurpleXfer *);
	void (*orig_end)(PurpleXfer *);

	char *content_type;
	char *iq_id;            /* slot request in flight */
	JabberHttpUploadSlot *slot;
	JabberHttpUploadPut *put;
	gint64 last_progress;   /* monotonic time of the last UI update */

	/* XEP-0447/0446: file metadata, computed in a thread (SHA-256) while
	 * the slot is requested and the file uploaded. */
	JabberSfsFile *sfs;
	gboolean sfs_done;      /* the thread has finished (sfs may be NULL) */
	gboolean put_done;      /* uploaded, waiting for the metadata */
} JabberHttpUpload;

struct _JabberHttpUploadPut {
	SoupSession *session;
	SoupMessage *msg;
	GCancellable *cancellable;
	goffset size;
	goffset sent;
	gboolean cancelled;
	JabberHttpUploadPutProgress progress_cb;
	JabberHttpUploadPutDone done_cb;
	gpointer data;
};

static GHashTable *services = NULL;  /* PurpleConnection * -> service */
static GHashTable *uploads = NULL;   /* PurpleXfer * -> JabberHttpUpload */
static int handle;

/**************************************************************************
 * Parsing and building
 **************************************************************************/

static gboolean
header_allowed(const char *name)
{
	static const char *allowed[] = { JABBER_HTTP_UPLOAD_ALLOWED_HEADERS };
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(allowed); i++)
		if (g_ascii_strcasecmp(name, allowed[i]) == 0)
			return TRUE;
	return FALSE;
}

/* XEP-0363: newlines in header names and values MUST be stripped. */
static char *
strip_newlines(const char *s)
{
	GString *out = g_string_new(NULL);

	for (; *s; s++)
		if (*s != '\r' && *s != '\n')
			g_string_append_c(out, *s);
	return g_string_free(out, FALSE);
}

static gboolean
url_usable(const char *url)
{
	GUri *uri;
	gboolean ok;

	if (url == NULL || *url == '\0')
		return FALSE;
	uri = g_uri_parse(url, G_URI_FLAGS_ENCODED, NULL);
	if (uri == NULL)
		return FALSE;
	/* The XEP says https; plain http is tolerated (local test servers). */
	ok = (g_ascii_strcasecmp(g_uri_get_scheme(uri), "https") == 0 ||
	      g_ascii_strcasecmp(g_uri_get_scheme(uri), "http") == 0) &&
	     g_uri_get_host(uri) != NULL && *g_uri_get_host(uri) != '\0';
	g_uri_unref(uri);
	return ok;
}

static goffset
parse_size(const char *s)
{
	gint64 v;
	char *end = NULL;

	if (s == NULL)
		return 0;
	while (g_ascii_isspace(*s))
		s++;
	v = g_ascii_strtoll(s, &end, 10);
	if (end == s || v < 0)
		return 0;
	return (goffset)v;
}

void
jabber_http_upload_error_clear(JabberHttpUploadError *error)
{
	if (error == NULL)
		return;
	g_free(error->condition);
	g_free(error->text);
	g_free(error->retry_stamp);
	memset(error, 0, sizeof(*error));
}

void
jabber_http_upload_slot_free(JabberHttpUploadSlot *slot)
{
	GList *l;

	if (slot == NULL)
		return;
	for (l = slot->headers; l; l = l->next) {
		PurpleKeyValuePair *kvp = l->data;
		g_free(kvp->key);
		g_free(kvp->value);
		g_free(kvp);
	}
	g_list_free(slot->headers);
	g_free(slot->put_url);
	g_free(slot->get_url);
	g_free(slot);
}

static void
parse_error(xmlnode *packet, JabberHttpUploadError *error)
{
	xmlnode *err = xmlnode_get_child(packet, "error");
	xmlnode *child;

	error->code = JABBER_HTTP_UPLOAD_ERROR_OTHER;
	if (err == NULL)
		return;

	for (child = err->child; child; child = child->next) {
		const char *xmlns;

		if (child->type != XMLNODE_TYPE_TAG)
			continue;
		xmlns = xmlnode_get_namespace(child);

		if (purple_strequal(xmlns, NS_XMPP_STANZAS)) {
			if (purple_strequal(child->name, "text")) {
				g_free(error->text);
				error->text = xmlnode_get_data(child);
			} else if (error->condition == NULL) {
				error->condition = g_strdup(child->name);
			}
		} else if (purple_strequal(xmlns, NS_HTTP_UPLOAD)) {
			if (purple_strequal(child->name, "file-too-large")) {
				xmlnode *max = xmlnode_get_child(child, "max-file-size");
				char *data = max ? xmlnode_get_data(max) : NULL;

				error->code = JABBER_HTTP_UPLOAD_ERROR_FILE_TOO_LARGE;
				error->max_file_size = parse_size(data);
				g_free(data);
			} else if (purple_strequal(child->name, "retry")) {
				error->code = JABBER_HTTP_UPLOAD_ERROR_RETRY;
				g_free(error->retry_stamp);
				error->retry_stamp = g_strdup(xmlnode_get_attrib(child, "stamp"));
			}
		}
	}

	if (error->code == JABBER_HTTP_UPLOAD_ERROR_OTHER &&
	    purple_strequal(error->condition, "not-acceptable"))
		error->code = JABBER_HTTP_UPLOAD_ERROR_NOT_ACCEPTABLE;
}

JabberHttpUploadSlot *
jabber_http_upload_parse_slot(JabberIqType type, xmlnode *packet,
                              JabberHttpUploadError *error)
{
	JabberHttpUploadSlot *slot;
	xmlnode *slotnode, *put, *get, *header;

	g_return_val_if_fail(error != NULL, NULL);
	memset(error, 0, sizeof(*error));

	if (packet == NULL) {
		error->code = JABBER_HTTP_UPLOAD_ERROR_MALFORMED;
		return NULL;
	}

	if (type == JABBER_IQ_ERROR) {
		parse_error(packet, error);
		return NULL;
	}

	if (type != JABBER_IQ_RESULT ||
	    !(slotnode = xmlnode_get_child_with_namespace(packet, "slot", NS_HTTP_UPLOAD)) ||
	    !(put = xmlnode_get_child(slotnode, "put")) ||
	    !(get = xmlnode_get_child(slotnode, "get")) ||
	    !url_usable(xmlnode_get_attrib(put, "url")) ||
	    !url_usable(xmlnode_get_attrib(get, "url"))) {
		error->code = JABBER_HTTP_UPLOAD_ERROR_MALFORMED;
		return NULL;
	}

	slot = g_new0(JabberHttpUploadSlot, 1);
	slot->put_url = g_strdup(xmlnode_get_attrib(put, "url"));
	slot->get_url = g_strdup(xmlnode_get_attrib(get, "url"));

	for (header = xmlnode_get_child(put, "header"); header;
	     header = xmlnode_get_next_twin(header)) {
		const char *name = xmlnode_get_attrib(header, "name");
		PurpleKeyValuePair *kvp;
		char *value;

		if (name == NULL || !header_allowed(name)) {
			purple_debug_warning("jabber", "http-upload: ignoring slot "
			                     "header '%s'\n", name ? name : "(null)");
			continue;
		}
		value = xmlnode_get_data(header);
		kvp = g_new0(PurpleKeyValuePair, 1);
		kvp->key = strip_newlines(name);
		kvp->value = strip_newlines(value ? value : "");
		g_free(value);
		slot->headers = g_list_append(slot->headers, kvp);
	}

	return slot;
}

char *
jabber_http_upload_error_to_string(const JabberHttpUploadError *error)
{
	switch (error->code) {
		case JABBER_HTTP_UPLOAD_ERROR_NONE:
			return g_strdup(_("No error"));
		case JABBER_HTTP_UPLOAD_ERROR_FILE_TOO_LARGE:
			if (error->max_file_size > 0) {
				char *max = purple_str_size_to_units(error->max_file_size);
				char *s = g_strdup_printf(_("The file is too large for the "
				        "server's upload service (at most %s)."), max);
				g_free(max);
				return s;
			}
			return g_strdup(_("The file is too large for the server's "
			                  "upload service."));
		case JABBER_HTTP_UPLOAD_ERROR_RETRY:
			if (error->retry_stamp)
				return g_strdup_printf(_("The upload quota is exhausted; "
				        "try again after %s."), error->retry_stamp);
			return g_strdup(_("The upload quota is exhausted; try again "
			                  "later."));
		case JABBER_HTTP_UPLOAD_ERROR_MALFORMED:
			return g_strdup(_("The server sent an invalid upload slot."));
		case JABBER_HTTP_UPLOAD_ERROR_NOT_ACCEPTABLE:
		case JABBER_HTTP_UPLOAD_ERROR_OTHER:
		default:
			if (error->text)
				return g_strdup_printf(_("The upload service refused the "
				        "file: %s"), error->text);
			return g_strdup_printf(_("The upload service refused the "
			        "file (%s)."),
			        error->condition ? error->condition : _("unknown error"));
	}
}

xmlnode *
jabber_http_upload_build_request(const char *filename, goffset size,
                                 const char *content_type)
{
	xmlnode *request = xmlnode_new("request");
	char *s;

	xmlnode_set_namespace(request, NS_HTTP_UPLOAD);
	xmlnode_set_attrib(request, "filename", filename);
	s = g_strdup_printf("%" G_GINT64_FORMAT, (gint64)size);
	xmlnode_set_attrib(request, "size", s);
	g_free(s);
	if (content_type && *content_type)
		xmlnode_set_attrib(request, "content-type", content_type);
	return request;
}

gboolean
jabber_http_upload_parse_disco_info(xmlnode *query, goffset *max_file_size)
{
	xmlnode *child;
	gboolean found = FALSE;

	if (max_file_size)
		*max_file_size = 0;
	if (query == NULL)
		return FALSE;

	for (child = xmlnode_get_child(query, "feature"); child;
	     child = xmlnode_get_next_twin(child)) {
		if (purple_strequal(xmlnode_get_attrib(child, "var"), NS_HTTP_UPLOAD)) {
			found = TRUE;
			break;
		}
	}
	if (!found)
		return FALSE;

	/* XEP-0128 extended info: the form with FORM_TYPE urn:xmpp:http:upload:0 */
	for (child = xmlnode_get_child_with_namespace(query, "x", "jabber:x:data");
	     child; child = xmlnode_get_next_twin(child)) {
		xmlnode *field;
		gboolean ours = FALSE;
		goffset max = 0;

		if (!purple_strequal(xmlnode_get_namespace(child), "jabber:x:data"))
			continue;

		for (field = xmlnode_get_child(child, "field"); field;
		     field = xmlnode_get_next_twin(field)) {
			const char *var = xmlnode_get_attrib(field, "var");
			xmlnode *value = xmlnode_get_child(field, "value");
			char *data = value ? xmlnode_get_data(value) : NULL;

			if (purple_strequal(var, "FORM_TYPE"))
				ours = purple_strequal(data, NS_HTTP_UPLOAD);
			else if (purple_strequal(var, "max-file-size"))
				max = parse_size(data);
			g_free(data);
		}

		if (ours) {
			if (max_file_size)
				*max_file_size = max;
			break;
		}
	}

	return TRUE;
}

char *
jabber_http_upload_guess_content_type(const char *path)
{
	guchar buf[4096];
	gsize n = 0;
	FILE *fp;
	char *type, *mime;
	gboolean uncertain = FALSE;

	if ((fp = g_fopen(path, "rb")) != NULL) {
		n = fread(buf, 1, sizeof(buf), fp);
		fclose(fp);
	}

	type = g_content_type_guess(path, n ? buf : NULL, n, &uncertain);
	mime = type ? g_content_type_get_mime_type(type) : NULL;
	g_free(type);

	/* g_content_type_guess() says "text/plain" for anything textual and
	 * "application/octet-stream" when it knows nothing; both are fine. */
	if (mime == NULL || *mime == '\0' || strchr(mime, '/') == NULL) {
		g_free(mime);
		mime = g_strdup("application/octet-stream");
	}
	return mime;
}

/**************************************************************************
 * The PUT
 **************************************************************************/

static char *
proxy_uri(PurpleProxyInfo *info, const char *scheme)
{
	const char *host = purple_proxy_info_get_host(info);
	const char *user = purple_proxy_info_get_username(info);
	const char *pass = purple_proxy_info_get_password(info);
	char *userinfo = NULL, *uri;

	if (host == NULL || *host == '\0')
		return NULL;

	if (user && *user) {
		char *u = g_uri_escape_string(user, NULL, FALSE);
		char *p = (pass && *pass) ? g_uri_escape_string(pass, NULL, FALSE) : NULL;
		userinfo = p ? g_strdup_printf("%s:%s", u, p) : g_strdup(u);
		g_free(u);
		g_free(p);
	}

	uri = g_uri_join(G_URI_FLAGS_ENCODED,
	                 scheme, userinfo, host,
	                 purple_proxy_info_get_port(info) > 0 ?
	                     purple_proxy_info_get_port(info) : -1,
	                 "", NULL, NULL);
	g_free(userinfo);
	return uri;
}

/*
 * Applies libpurple's proxy settings for @account to @session. The account's
 * own setting wins; otherwise "Use GNOME Proxy Settings" and "Use
 * Environmental Settings" mean the default GProxyResolver (as libpurple's own
 * connections use since M1), and the explicit global types map onto a
 * GSimpleProxyResolver.
 */
static void
setup_proxy(SoupSession *session, PurpleAccount *account)
{
	PurpleProxyInfo *info = NULL;
	GProxyResolver *resolver = NULL;
	const char *scheme = NULL;
	char *uri;

	if (account != NULL) {
		info = purple_account_get_proxy_info(account);
		if (info && purple_proxy_info_get_type(info) == PURPLE_PROXY_USE_GLOBAL)
			info = NULL;
	}
	if (info == NULL) {
		if (purple_running_gnome())
			return;  /* keep the default resolver */
		info = purple_global_proxy_get_info();
	}
	if (info == NULL)
		return;

	switch (purple_proxy_info_get_type(info)) {
		case PURPLE_PROXY_NONE:
			g_object_set(session, "proxy-resolver", NULL, NULL);
			purple_debug_misc("jabber", "http-upload: no proxy\n");
			return;
		case PURPLE_PROXY_HTTP:
			scheme = "http";
			break;
		case PURPLE_PROXY_SOCKS4:
			scheme = "socks4";
			break;
		case PURPLE_PROXY_SOCKS5:
		case PURPLE_PROXY_TOR:
			scheme = "socks5";  /* GIO resolves names on the proxy */
			break;
		case PURPLE_PROXY_USE_ENVVAR:
		case PURPLE_PROXY_USE_GLOBAL:
		default:
			return;  /* the default resolver reads the environment */
	}

	uri = proxy_uri(info, scheme);
	if (uri == NULL)
		return;
	resolver = g_simple_proxy_resolver_new(uri, NULL);
	g_object_set(session, "proxy-resolver", resolver, NULL);
	purple_debug_misc("jabber", "http-upload: using %s proxy %s:%d\n", scheme,
	                  purple_proxy_info_get_host(info),
	                  purple_proxy_info_get_port(info));
	g_object_unref(resolver);
	g_free(uri);
}

static void
put_free(JabberHttpUploadPut *put)
{
	if (put->msg)
		g_signal_handlers_disconnect_by_data(put->msg, put);
	g_clear_object(&put->msg);
	g_clear_object(&put->session);
	g_clear_object(&put->cancellable);
	g_free(put);
}

static void
put_wrote_body_data_cb(SoupMessage *msg, guint chunk_size, gpointer data)
{
	JabberHttpUploadPut *put = data;

	if (put->cancelled)
		return;
	put->sent += chunk_size;
	if (put->progress_cb)
		put->progress_cb(MIN(put->sent, put->size), put->data);
}

static void
put_done_cb(GObject *source, GAsyncResult *res, gpointer data)
{
	JabberHttpUploadPut *put = data;
	GError *error = NULL;
	GBytes *body;
	char *msg = NULL;
	guint status;

	body = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &error);
	if (body)
		g_bytes_unref(body);

	if (put->cancelled ||
	    g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		g_clear_error(&error);
		put_free(put);
		return;
	}

	status = soup_message_get_status(put->msg);
	if (error != NULL) {
		msg = g_strdup(error->message);
		g_error_free(error);
	} else if (!SOUP_STATUS_IS_SUCCESSFUL(status)) {
		const char *reason = soup_message_get_reason_phrase(put->msg);
		msg = g_strdup_printf(_("The upload server answered: %u %s"), status,
		                      reason ? reason : "");
	}

	purple_debug_info("jabber", "http-upload: PUT finished, status %u%s%s\n",
	                  status, msg ? ": " : "", msg ? msg : "");

	if (put->done_cb)
		put->done_cb(msg, put->data);
	g_free(msg);
	put_free(put);
}

JabberHttpUploadPut *
jabber_http_upload_put_start(PurpleAccount *account, const char *path,
                             goffset size, const char *content_type,
                             const JabberHttpUploadSlot *slot,
                             JabberHttpUploadPutProgress progress_cb,
                             JabberHttpUploadPutDone done_cb, gpointer data,
                             char **error_out)
{
	JabberHttpUploadPut *put;
	SoupMessageHeaders *headers;
	GFile *file;
	GFileInputStream *stream;
	GError *error = NULL;
	GList *l;

	g_return_val_if_fail(path != NULL, NULL);
	g_return_val_if_fail(slot != NULL, NULL);

	if (!url_usable(slot->put_url)) {
		if (error_out)
			*error_out = g_strdup(_("The server sent an invalid upload slot."));
		return NULL;
	}

	file = g_file_new_for_path(path);
	stream = g_file_read(file, NULL, &error);
	g_object_unref(file);
	if (stream == NULL) {
		if (error_out)
			*error_out = g_strdup(error->message);
		g_error_free(error);
		return NULL;
	}

	put = g_new0(JabberHttpUploadPut, 1);
	put->size = size;
	put->progress_cb = progress_cb;
	put->done_cb = done_cb;
	put->data = data;
	put->cancellable = g_cancellable_new();
	put->session = soup_session_new_with_options(
			"user-agent", "libpurple/" VERSION " ",  /* soup appends its own */
			NULL);
	setup_proxy(put->session, account);

	put->msg = soup_message_new(SOUP_METHOD_PUT, slot->put_url);
	if (put->msg == NULL) {
		g_object_unref(stream);
		put_free(put);
		if (error_out)
			*error_out = g_strdup(_("The server sent an invalid upload slot."));
		return NULL;
	}

	headers = soup_message_get_request_headers(put->msg);
	for (l = slot->headers; l; l = l->next) {
		PurpleKeyValuePair *kvp = l->data;
		if (header_allowed(kvp->key))
			soup_message_headers_replace(headers, kvp->key, kvp->value);
	}

	/* Streams the file; sets Content-Type and Content-Length. */
	soup_message_set_request_body(put->msg,
			content_type ? content_type : "application/octet-stream",
			G_INPUT_STREAM(stream), size);
	g_object_unref(stream);

	g_signal_connect(put->msg, "wrote-body-data",
	                 G_CALLBACK(put_wrote_body_data_cb), put);

	purple_debug_info("jabber", "http-upload: PUT %" G_GINT64_FORMAT
	                  " bytes (%s)\n", (gint64)size,
	                  content_type ? content_type : "application/octet-stream");

	soup_session_send_and_read_async(put->session, put->msg,
	                                 G_PRIORITY_DEFAULT, put->cancellable,
	                                 put_done_cb, put);
	return put;
}

void
jabber_http_upload_put_cancel(JabberHttpUploadPut *put)
{
	g_return_if_fail(put != NULL);

	if (put->cancelled)
		return;
	/* put_done_cb() frees it once libsoup has let go. */
	put->cancelled = TRUE;
	g_cancellable_cancel(put->cancellable);
}

/**************************************************************************
 * Service discovery
 **************************************************************************/

static JabberHttpUploadService *
service_find(PurpleConnection *gc)
{
	return services ? g_hash_table_lookup(services, gc) : NULL;
}

static gboolean
account_enabled(PurpleConnection *gc)
{
	return purple_account_get_bool(purple_connection_get_account(gc),
	                               "http_upload", TRUE);
}

gboolean
jabber_http_upload_available(PurpleConnection *gc)
{
	JabberHttpUploadService *service = gc ? service_find(gc) : NULL;

	return service && service->jid && account_enabled(gc);
}

void
jabber_http_upload_set_service(PurpleConnection *gc, const char *jid,
                               goffset max_file_size)
{
	JabberHttpUploadService *service = service_find(gc);

	if (service == NULL) {
		service = g_new0(JabberHttpUploadService, 1);
		service->gc = gc;
		g_hash_table_insert(services, gc, service);
	}
	g_free(service->jid);
	service->jid = g_strdup(jid);
	service->max_file_size = max_file_size;
}

static void
disco_info_cb(JabberStream *js, const char *from, JabberIqType type,
              const char *id, xmlnode *packet, gpointer data)
{
	JabberHttpUploadService *service = service_find(js->gc);
	goffset max = 0;

	if (service == NULL || service->jid != NULL || from == NULL ||
	    type != JABBER_IQ_RESULT)
		return;

	if (!jabber_http_upload_parse_disco_info(
			xmlnode_get_child_with_namespace(packet, "query", NS_DISCO_INFO),
			&max))
		return;

	service->jid = g_strdup(from);
	service->max_file_size = max;
	purple_debug_info("jabber", "http-upload: service %s, max-file-size %"
	                  G_GINT64_FORMAT "\n", from, (gint64)max);
}

static void
disco_info_query(JabberStream *js, const char *jid)
{
	JabberIq *iq = jabber_iq_new_query(js, JABBER_IQ_GET, NS_DISCO_INFO);

	xmlnode_set_attrib(iq->node, "to", jid);
	jabber_iq_set_callback(iq, disco_info_cb, NULL);
	jabber_iq_send(iq);
}

static void
disco_items_cb(JabberStream *js, const char *from, JabberIqType type,
               const char *id, xmlnode *packet, gpointer data)
{
	xmlnode *query, *item;

	if (service_find(js->gc) == NULL || type != JABBER_IQ_RESULT)
		return;

	query = xmlnode_get_child_with_namespace(packet, "query", NS_DISCO_ITEMS);
	if (query == NULL)
		return;

	for (item = xmlnode_get_child(query, "item"); item;
	     item = xmlnode_get_next_twin(item)) {
		const char *jid = xmlnode_get_attrib(item, "jid");

		/* Components, not nodes. */
		if (jid == NULL || xmlnode_get_attrib(item, "node") != NULL)
			continue;
		disco_info_query(js, jid);
	}
}

static void
discover(JabberStream *js)
{
	JabberIq *iq;

	/* The server itself may offer the service (e.g. ejabberd's
	 * mod_http_upload on the host), or one of its components. */
	disco_info_query(js, js->user->domain);

	iq = jabber_iq_new_query(js, JABBER_IQ_GET, NS_DISCO_ITEMS);
	xmlnode_set_attrib(iq->node, "to", js->user->domain);
	jabber_iq_set_callback(iq, disco_items_cb, NULL);
	jabber_iq_send(iq);
}

/**************************************************************************
 * File transfers
 **************************************************************************/

static void upload_request_slot(JabberHttpUpload *up);

/* Detaches @up from its transfer and frees it. Doesn't touch the transfer's
 * ops. */
static void
upload_free(JabberHttpUpload *up)
{
	if (up->iq_id) {
		jabber_iq_remove_callback_by_id(up->js, up->iq_id);
		g_free(up->iq_id);
	}
	if (up->put)
		jabber_http_upload_put_cancel(up->put);
	if (up->service)
		up->service->uploads = g_list_remove(up->service->uploads, up);
	g_hash_table_remove(uploads, up->xfer);

	jabber_http_upload_slot_free(up->slot);
	jabber_sfs_file_free(up->sfs);
	g_free(up->content_type);
	g_free(up->to);
	purple_xfer_unref(up->xfer);
	g_free(up);
}

static void
xfer_cancel_send_cb(PurpleXfer *xfer)
{
	JabberHttpUpload *up = g_hash_table_lookup(uploads, xfer);
	void (*orig)(PurpleXfer *);

	if (up == NULL)
		return;
	purple_debug_info("jabber", "http-upload: transfer %p cancelled\n", xfer);
	orig = up->orig_cancel_send;
	upload_free(up);
	if (orig)
		orig(xfer);
}

static void
xfer_end_cb(PurpleXfer *xfer)
{
	JabberHttpUpload *up = g_hash_table_lookup(uploads, xfer);
	void (*orig)(PurpleXfer *);

	if (up == NULL)
		return;
	orig = up->orig_end;
	upload_free(up);
	if (orig)
		orig(xfer);
}

/* Fails the transfer. Before the PUT, a 1:1 transfer goes to its fallback
 * (SI) instead. */
static void
upload_fail(JabberHttpUpload *up, const char *reason, gboolean before_put)
{
	PurpleXfer *xfer = up->xfer;

	purple_debug_warning("jabber", "http-upload: %s failed: %s\n",
	                     purple_xfer_get_filename(xfer), reason);

	if (before_put && up->fallback) {
		JabberHttpUploadFallback fallback = up->fallback;

		purple_debug_info("jabber", "http-upload: falling back to SI\n");
		xfer->ops.cancel_send = up->orig_cancel_send;
		xfer->ops.end = up->orig_end;
		purple_xfer_ref(xfer);
		upload_free(up);
		fallback(xfer);
		purple_xfer_unref(xfer);
		return;
	}

	purple_xfer_error(PURPLE_XFER_SEND, purple_xfer_get_account(xfer),
	                  purple_xfer_get_remote_user(xfer), reason);
	/* Calls xfer_cancel_send_cb(), which frees @up. */
	purple_xfer_cancel_local(xfer);
}

static void
send_url_message(JabberHttpUpload *up)
{
	const char *url = up->slot->get_url;
	xmlnode *message, *child, *x;
	char *id = jabber_get_next_id(up->js);

	message = xmlnode_new("message");
	xmlnode_set_attrib(message, "type", up->groupchat ? "groupchat" : "chat");
	xmlnode_set_attrib(message, "to", up->to);
	xmlnode_set_attrib(message, "id", id);
	g_free(id);

	child = xmlnode_new_child(message, "body");
	xmlnode_insert_data(child, url, -1);

	x = xmlnode_new_child(message, "x");
	xmlnode_set_namespace(x, NS_OOB_X_DATA);
	child = xmlnode_new_child(x, "url");
	xmlnode_insert_data(child, url, -1);

	/* XEP-0447 stateless file sharing: the same URL with the file's
	 * metadata, for clients that show a file card instead of a link. */
	if (up->sfs) {
		g_free(up->sfs->url);
		up->sfs->url = g_strdup(url);
		xmlnode_insert_child(message, jabber_sfs_build(up->sfs));
		purple_debug_info("jabber", "http-upload: with file-sharing metadata "
		                  "(%s, %dx%d)\n", up->sfs->hash ? up->sfs->hash : "no hash",
		                  up->sfs->width, up->sfs->height);
	}

	jabber_send(up->js, message);
	xmlnode_free(message);

	/* A MUC echoes the message back; a 1:1 message needs a local echo. */
	if (!up->groupchat) {
		PurpleAccount *account = purple_connection_get_account(up->js->gc);
		PurpleConversation *conv;
		char *escaped;

		conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM,
		                                             up->to, account);
		if (conv == NULL)
			conv = purple_conversation_new(PURPLE_CONV_TYPE_IM, account,
			                               up->to);
		escaped = g_markup_escape_text(url, -1);
		purple_conv_im_write(PURPLE_CONV_IM(conv), NULL, escaped,
		                     PURPLE_MESSAGE_SEND, time(NULL));
		g_free(escaped);
	}
}

static void
put_progress_cb(goffset sent, gpointer data)
{
	JabberHttpUpload *up = data;
	gint64 now = g_get_monotonic_time();

	purple_xfer_set_bytes_sent(up->xfer, sent);
	/* libsoup reports every chunk it writes; the UI needs far fewer. */
	if (now - up->last_progress >= G_USEC_PER_SEC / 10) {
		up->last_progress = now;
		purple_xfer_update_progress(up->xfer);
	}
}

/* Uploaded and the metadata is known: send the URL, finish the transfer. */
static void
upload_complete(JabberHttpUpload *up)
{
	PurpleXfer *xfer = up->xfer;

	purple_debug_info("jabber", "http-upload: %s uploaded to %s\n",
	                  purple_xfer_get_filename(xfer), up->slot->get_url);
	send_url_message(up);

	purple_xfer_set_bytes_sent(xfer, purple_xfer_get_size(xfer));
	purple_xfer_update_progress(xfer);
	purple_xfer_set_completed(xfer, TRUE);
	/* Calls xfer_end_cb(), which frees @up. */
	purple_xfer_end(xfer);
}

static void
put_finished_cb(const char *error, gpointer data)
{
	JabberHttpUpload *up = data;

	up->put = NULL;  /* frees itself */

	if (error != NULL) {
		upload_fail(up, error, FALSE);
		return;
	}

	up->put_done = TRUE;
	if (!up->sfs_done) {
		purple_debug_info("jabber", "http-upload: %s uploaded, waiting for "
		                  "its metadata\n", purple_xfer_get_filename(up->xfer));
		return;
	}
	upload_complete(up);
}

typedef struct {
	char *path;
	char *name;
	char *content_type;
} SfsJob;

static void
sfs_job_free(SfsJob *job)
{
	g_free(job->path);
	g_free(job->name);
	g_free(job->content_type);
	g_free(job);
}

static void
sfs_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
	SfsJob *job = data;

	g_task_return_pointer(task,
			jabber_sfs_file_from_path(job->path, job->name, job->content_type),
			(GDestroyNotify)jabber_sfs_file_free);
}

static void
sfs_done_cb(GObject *source, GAsyncResult *res, gpointer data)
{
	PurpleXfer *xfer = data;
	JabberHttpUpload *up = uploads ? g_hash_table_lookup(uploads, xfer) : NULL;
	JabberSfsFile *sfs = g_task_propagate_pointer(G_TASK(res), NULL);

	if (up == NULL || up->sfs_done) {
		/* cancelled, failed or handed to SI meanwhile */
		jabber_sfs_file_free(sfs);
		purple_xfer_unref(xfer);
		return;
	}

	up->sfs = sfs;
	up->sfs_done = TRUE;
	if (sfs == NULL)
		purple_debug_warning("jabber", "http-upload: couldn't read %s for "
		                     "its metadata\n", purple_xfer_get_local_filename(xfer));
	if (up->put_done)
		upload_complete(up);
	purple_xfer_unref(xfer);
}

static void
sfs_start(JabberHttpUpload *up)
{
	SfsJob *job = g_new0(SfsJob, 1);
	GTask *task;

	job->path = g_strdup(purple_xfer_get_local_filename(up->xfer));
	job->name = g_strdup(purple_xfer_get_filename(up->xfer));
	job->content_type = g_strdup(up->content_type);

	purple_xfer_ref(up->xfer);
	task = g_task_new(NULL, NULL, sfs_done_cb, up->xfer);
	g_task_set_task_data(task, job, (GDestroyNotify)sfs_job_free);
	g_task_run_in_thread(task, sfs_thread);
	g_object_unref(task);
}

static void
upload_start_put(JabberHttpUpload *up)
{
	PurpleXfer *xfer = up->xfer;
	char *error = NULL;

	up->put = jabber_http_upload_put_start(purple_xfer_get_account(xfer),
			purple_xfer_get_local_filename(xfer), purple_xfer_get_size(xfer),
			up->content_type, up->slot, put_progress_cb, put_finished_cb, up,
			&error);
	if (up->put == NULL) {
		upload_fail(up, error, TRUE);
		g_free(error);
		return;
	}

	/* Shows the transfer as started. With fd -1 there is no watcher; the
	 * default read/write ops are never used. */
	purple_xfer_ref(xfer);
	purple_xfer_start(xfer, -1, NULL, 0);
	purple_xfer_unref(xfer);
}

static void
slot_cb(JabberStream *js, const char *from, JabberIqType type,
        const char *id, xmlnode *packet, gpointer data)
{
	JabberHttpUpload *up = data;
	JabberHttpUploadError error;
	char *reason;

	g_free(up->iq_id);
	up->iq_id = NULL;

	up->slot = jabber_http_upload_parse_slot(type, packet, &error);
	if (up->slot) {
		purple_debug_info("jabber", "http-upload: got slot, PUT %s\n",
		                  up->slot->put_url);
		upload_start_put(up);
		return;
	}

	if (error.code == JABBER_HTTP_UPLOAD_ERROR_FILE_TOO_LARGE &&
	    error.max_file_size > 0 && up->service)
		up->service->max_file_size = error.max_file_size;

	reason = jabber_http_upload_error_to_string(&error);
	jabber_http_upload_error_clear(&error);
	upload_fail(up, reason, TRUE);
	g_free(reason);
}

static void
upload_request_slot(JabberHttpUpload *up)
{
	PurpleXfer *xfer = up->xfer;
	JabberIq *iq;

	iq = jabber_iq_new(up->js, JABBER_IQ_GET);
	xmlnode_set_attrib(iq->node, "to", up->service->jid);
	xmlnode_insert_child(iq->node, jabber_http_upload_build_request(
			purple_xfer_get_filename(xfer), purple_xfer_get_size(xfer),
			up->content_type));
	jabber_iq_set_callback(iq, slot_cb, up);
	up->iq_id = g_strdup(iq->id);
	jabber_iq_send(iq);
}

gboolean
jabber_http_upload_send_xfer(JabberStream *js, PurpleXfer *xfer,
                             JabberHttpUploadFallback fallback)
{
	JabberHttpUploadService *service;
	JabberHttpUpload *up;
	goffset size;

	g_return_val_if_fail(js != NULL, FALSE);
	g_return_val_if_fail(xfer != NULL, FALSE);

	if (!jabber_http_upload_available(js->gc) ||
	    purple_xfer_get_type(xfer) != PURPLE_XFER_SEND ||
	    purple_xfer_get_local_filename(xfer) == NULL)
		return FALSE;

	service = service_find(js->gc);
	size = purple_xfer_get_size(xfer);
	if (service->max_file_size > 0 && size > service->max_file_size) {
		purple_debug_info("jabber", "http-upload: %" G_GINT64_FORMAT
		                  " bytes is over the limit of %" G_GINT64_FORMAT "\n",
		                  (gint64)size, (gint64)service->max_file_size);
		return FALSE;
	}

	up = g_new0(JabberHttpUpload, 1);
	up->xfer = xfer;
	purple_xfer_ref(xfer);
	up->js = js;
	up->service = service;
	up->to = g_strdup(purple_xfer_get_remote_user(xfer));
	up->fallback = fallback;
	up->content_type = jabber_http_upload_guess_content_type(
			purple_xfer_get_local_filename(xfer));

	up->orig_cancel_send = xfer->ops.cancel_send;
	up->orig_end = xfer->ops.end;
	purple_xfer_set_cancel_send_fnc(xfer, xfer_cancel_send_cb);
	purple_xfer_set_end_fnc(xfer, xfer_end_cb);

	g_hash_table_insert(uploads, xfer, up);
	service->uploads = g_list_prepend(service->uploads, up);
	sfs_start(up);

	purple_debug_info("jabber", "http-upload: sending %s (%" G_GINT64_FORMAT
	                  " bytes, %s) to %s via %s\n",
	                  purple_xfer_get_filename(xfer), (gint64)size,
	                  up->content_type, up->to, service->jid);
	upload_request_slot(up);
	return TRUE;
}

/**************************************************************************
 * Groupchats
 **************************************************************************/

gboolean
jabber_http_upload_chat_can_receive_file(PurpleConnection *gc, int id)
{
	return jabber_http_upload_available(gc);
}

static void
chat_xfer_init(PurpleXfer *xfer)
{
	PurpleConnection *gc = purple_account_get_connection(
			purple_xfer_get_account(xfer));
	JabberStream *js = gc ? gc->proto_data : NULL;
	JabberHttpUploadService *service = gc ? service_find(gc) : NULL;
	char *msg;

	if (js && jabber_http_upload_send_xfer(js, xfer, NULL)) {
		JabberHttpUpload *up = g_hash_table_lookup(uploads, xfer);
		if (up)
			up->groupchat = TRUE;
		return;
	}

	if (service && service->jid && service->max_file_size > 0 &&
	    (goffset)purple_xfer_get_size(xfer) > service->max_file_size) {
		char *max = purple_str_size_to_units(service->max_file_size);
		msg = g_strdup_printf(_("The file is too large for the server's "
		                        "upload service (at most %s)."), max);
		g_free(max);
	} else {
		msg = g_strdup(_("The server has no HTTP upload service."));
	}
	purple_xfer_error(PURPLE_XFER_SEND, purple_xfer_get_account(xfer),
	                  purple_xfer_get_remote_user(xfer), msg);
	g_free(msg);
	purple_xfer_cancel_local(xfer);
}

void
jabber_http_upload_chat_send_file(PurpleConnection *gc, int id,
                                  const char *file)
{
	JabberStream *js = gc->proto_data;
	JabberChat *chat = js ? jabber_chat_find_by_id(js, id) : NULL;
	PurpleXfer *xfer;
	char *room;

	if (chat == NULL || chat->left)
		return;

	room = g_strdup_printf("%s@%s", chat->room, chat->server);
	xfer = purple_xfer_new(purple_connection_get_account(gc),
	                       PURPLE_XFER_SEND, room);
	g_free(room);
	if (xfer == NULL)
		return;

	purple_xfer_set_init_fnc(xfer, chat_xfer_init);

	if (file)
		purple_xfer_request_accepted(xfer, file);
	else
		purple_xfer_request(xfer);
}

/**************************************************************************
 * Setup
 **************************************************************************/

static gboolean
is_jabber(PurpleConnection *gc)
{
	return purple_strequal(purple_account_get_protocol_id(
			purple_connection_get_account(gc)), "prpl-jabber");
}

static void
signed_on_cb(PurpleConnection *gc, gpointer data)
{
	JabberStream *js;

	if (!is_jabber(gc) || (js = gc->proto_data) == NULL)
		return;

	jabber_http_upload_set_service(gc, NULL, 0);
	if (account_enabled(gc))
		discover(js);
}

static void
signing_off_cb(PurpleConnection *gc, gpointer data)
{
	JabberHttpUploadService *service = service_find(gc);

	if (service == NULL)
		return;

	while (service->uploads) {
		JabberHttpUpload *up = service->uploads->data;
		/* Frees @up and unlinks it from the list. */
		purple_xfer_cancel_local(up->xfer);
		if (service->uploads && service->uploads->data == up) {
			/* Not ours any more?  Don't loop forever. */
			up->service = NULL;
			service->uploads = g_list_delete_link(service->uploads,
			                                      service->uploads);
		}
	}
	g_hash_table_remove(services, gc);
}

static void
service_free(gpointer data)
{
	JabberHttpUploadService *service = data;

	g_list_free(service->uploads);
	g_free(service->jid);
	g_free(service);
}

/*
 * prpls are loaded by purple_plugins_probe(), before purple_core_init() has
 * run purple_connections_init(), so the connection signals don't exist yet
 * when jabber_http_upload_init() runs. Connect from the event loop instead:
 * no account can finish signing on before the loop has run once.
 */
static guint connect_timer = 0;

static gboolean
connect_signals_cb(gpointer data)
{
	connect_timer = 0;
	purple_signal_connect(purple_connections_get_handle(), "signed-on",
	                      &handle, PURPLE_CALLBACK(signed_on_cb), NULL);
	purple_signal_connect(purple_connections_get_handle(), "signing-off",
	                      &handle, PURPLE_CALLBACK(signing_off_cb), NULL);
	return FALSE;
}

void
jabber_http_upload_init(void)
{
	if (services != NULL)
		return;

	services = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
	                                 service_free);
	uploads = g_hash_table_new(g_direct_hash, g_direct_equal);

	connect_timer = purple_timeout_add(0, connect_signals_cb, NULL);
}

void
jabber_http_upload_uninit(void)
{
	if (connect_timer) {
		purple_timeout_remove(connect_timer);
		connect_timer = 0;
	}
	purple_signals_disconnect_by_handle(&handle);

	if (services) {
		g_hash_table_destroy(services);
		services = NULL;
	}
	if (uploads) {
		g_hash_table_destroy(uploads);
		uploads = NULL;
	}
}
