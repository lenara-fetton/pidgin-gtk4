/**
 * @file httpupload.h XEP-0363 HTTP File Upload
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
#ifndef PURPLE_JABBER_HTTPUPLOAD_H_
#define PURPLE_JABBER_HTTPUPLOAD_H_

#include "ft.h"
#include "xmlnode.h"

#include "jabber.h"
#include "iq.h"

/*
 * Sending files through the server's XEP-0363 upload service.
 *
 * On sign-on the server domain and its disco#items are queried for an
 * entity with the urn:xmpp:http:upload:0 feature; its JID and max-file-size
 * are kept per connection. send_file (si.c) and chat_send_file then request
 * a slot, PUT the file with libsoup 3 and send the GET URL as a message body
 * plus a jabber:x:oob <url> (XEP-0066). A 1:1 transfer falls back to SI when
 * anything goes wrong before the PUT starts.
 *
 * The account setting "http_upload" (bool, default TRUE) turns it off.
 */

/** The only request headers a slot may ask for (XEP-0363 section 5). */
#define JABBER_HTTP_UPLOAD_ALLOWED_HEADERS "Authorization", "Cookie", "Expires"

/** A parsed upload slot. */
typedef struct {
	char *put_url;
	char *get_url;
	/** PurpleKeyValuePair *, value a char *: only the allowed headers, with
	 *  CR/LF removed. */
	GList *headers;
} JabberHttpUploadSlot;

typedef enum {
	JABBER_HTTP_UPLOAD_ERROR_NONE = 0,
	/** <file-too-large/>; max_file_size is set when the server said. */
	JABBER_HTTP_UPLOAD_ERROR_FILE_TOO_LARGE,
	/** <retry stamp=…/>: quota reached, try again at retry_stamp. */
	JABBER_HTTP_UPLOAD_ERROR_RETRY,
	/** <not-acceptable/> without an upload-specific child (e.g. bad name). */
	JABBER_HTTP_UPLOAD_ERROR_NOT_ACCEPTABLE,
	/** Any other stanza error. */
	JABBER_HTTP_UPLOAD_ERROR_OTHER,
	/** A result without a usable <slot>. */
	JABBER_HTTP_UPLOAD_ERROR_MALFORMED
} JabberHttpUploadErrorCode;

typedef struct {
	JabberHttpUploadErrorCode code;
	char *condition;        /**< Stanza error condition element name.   */
	char *text;             /**< <text/>, if any.                       */
	goffset max_file_size;  /**< From <file-too-large>, 0 if not given. */
	char *retry_stamp;      /**< From <retry stamp=…>, NULL if not given. */
} JabberHttpUploadError;

/**
 * Parses the reply to a slot request. Returns the slot, or NULL and fills
 * @a error (which must be zero-initialized and later cleared with
 * jabber_http_upload_error_clear()).
 */
JabberHttpUploadSlot *jabber_http_upload_parse_slot(JabberIqType type,
		xmlnode *packet, JabberHttpUploadError *error);
void jabber_http_upload_slot_free(JabberHttpUploadSlot *slot);
void jabber_http_upload_error_clear(JabberHttpUploadError *error);
/** A human-readable description of @a error (newly allocated). */
char *jabber_http_upload_error_to_string(const JabberHttpUploadError *error);

/** Builds the <request xmlns='urn:xmpp:http:upload:0'/> element. */
xmlnode *jabber_http_upload_build_request(const char *filename, goffset size,
		const char *content_type);

/**
 * Checks a disco#info <query/> for the upload feature. Returns TRUE if the
 * entity is an upload service; @a max_file_size gets its max-file-size
 * (0 when not advertised, meaning no stated limit).
 */
gboolean jabber_http_upload_parse_disco_info(xmlnode *query,
		goffset *max_file_size);

/** The MIME type of a local file, "application/octet-stream" if unknown. */
char *jabber_http_upload_guess_content_type(const char *path);

/*
 * The HTTP part, independent of the XMPP stream (used by the tests too).
 */
typedef struct _JabberHttpUploadPut JabberHttpUploadPut;

typedef void (*JabberHttpUploadPutProgress)(goffset sent, gpointer data);
/** Called exactly once unless the PUT was cancelled. @a error is NULL on
 *  success (2xx). */
typedef void (*JabberHttpUploadPutDone)(const char *error, gpointer data);

/**
 * Starts an asynchronous PUT of the local file @a path (@a size bytes) to
 * @a slot's PUT URL, with the slot's headers. The proxy follows @a account's
 * libpurple proxy settings (NULL: the global ones). Returns NULL and sets
 * @a error_out if the file can't be opened or the URL is unusable; nothing
 * is called back in that case.
 */
JabberHttpUploadPut *jabber_http_upload_put_start(PurpleAccount *account,
		const char *path, goffset size, const char *content_type,
		const JabberHttpUploadSlot *slot,
		JabberHttpUploadPutProgress progress_cb,
		JabberHttpUploadPutDone done_cb, gpointer data, char **error_out);
/** Cancels a running PUT; no callbacks are made afterwards. */
void jabber_http_upload_put_cancel(JabberHttpUploadPut *put);

/*
 * The file transfer glue.
 */
typedef void (*JabberHttpUploadFallback)(PurpleXfer *xfer);

/** Whether the connection has an upload service and it's enabled. */
gboolean jabber_http_upload_available(PurpleConnection *gc);

/**
 * Sends @a xfer (type SEND, local file and size set) by HTTP upload if the
 * connection has an upload service and the file fits. Returns FALSE (and
 * does nothing) otherwise. If the upload fails before the PUT starts and
 * @a fallback is not NULL, the transfer's ops are restored and @a fallback is
 * called with it; otherwise the transfer is cancelled with an error.
 */
gboolean jabber_http_upload_send_xfer(JabberStream *js, PurpleXfer *xfer,
		JabberHttpUploadFallback fallback);

/** prpl chat_can_receive_file / chat_send_file. */
gboolean jabber_http_upload_chat_can_receive_file(PurpleConnection *gc, int id);
void jabber_http_upload_chat_send_file(PurpleConnection *gc, int id,
		const char *file);

/** For tests: set the service without disco. */
void jabber_http_upload_set_service(PurpleConnection *gc, const char *jid,
		goffset max_file_size);

/**
 * Registers the IPC commands on the prpl @a plugin:
 *   gboolean http-upload-available(PurpleAccount *): TRUE when the
 *     account is connected, has discovered an upload service and has it
 *     enabled ("http_upload");
 *   guint64 http-upload-max-size(PurpleAccount *): the service's
 *     max-file-size, 0 when unknown or unlimited (returned as a pointer by
 *     purple_plugin_ipc_call(): GPOINTER_TO_SIZE() it; 64-bit only).
 * The open conversations of an account get PURPLE_CONV_UPDATE_FEATURES
 * when its service is discovered.
 */
void jabber_http_upload_ipc_init(PurplePlugin *plugin);

void jabber_http_upload_init(void);
void jabber_http_upload_uninit(void);

#endif /* PURPLE_JABBER_HTTPUPLOAD_H_ */
