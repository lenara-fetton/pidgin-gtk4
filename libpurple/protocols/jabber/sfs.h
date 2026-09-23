/**
 * @file sfs.h XEP-0447 Stateless File Sharing, XEP-0446 File Metadata,
 *             XEP-0264 thumbnails, XEP-0300 hashes (XEP-0385 SIMS on receipt)
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
#ifndef PURPLE_JABBER_SFS_H_
#define PURPLE_JABBER_SFS_H_

#include <glib.h>

#include "jabber.h"
#include "xmlnode.h"

typedef struct _JabberSfsFile JabberSfsFile;

struct _JabberSfsFile {
	char *url;          /**< <sources><url-data target=''/> (SIMS: reference uri) */
	char *name;
	goffset size;       /**< -1 when unknown */
	char *media_type;
	int width, height;  /**< 0 when unknown */
	char *desc;
	char *hash;         /**< "algo:base64", e.g. "sha-256:47DEQpj8…" */
	char *thumbnail;    /**< a data: URI, or the cid: URI if unresolved */
	char *disposition;  /**< "inline", "attachment" or NULL */
	gboolean sims;      /**< it came as XEP-0385 SIMS */
};

void jabber_sfs_file_free(JabberSfsFile *file);

/**
 * The first XEP-0447 <file-sharing/> (or XEP-0385 <reference type='data'>
 * <media-sharing/>) child of @a message, or NULL.  A thumbnail's cid: URI
 * is resolved from a <data xmlns='urn:xmpp:bob'/> in the message, else from
 * the BoB cache for @a from (when @a js is not NULL).
 */
JabberSfsFile *jabber_sfs_parse(JabberStream *js, const char *from,
                                xmlnode *message);

/** Parses one <file-sharing/> or SIMS <reference/> element. */
JabberSfsFile *jabber_sfs_parse_element(JabberStream *js, const char *from,
                                        xmlnode *message, xmlnode *element);

/** Adds the sfs-* keys for @a file to a receiving-message-meta table. */
void jabber_sfs_to_meta(const JabberSfsFile *file, GHashTable *meta);

/** Builds <file-sharing xmlns='urn:xmpp:sfs:0'> with <file/> metadata and
 *  <sources><url-data target=url/></sources>. */
xmlnode *jabber_sfs_build(const JabberSfsFile *file);

/**
 * Reads the local file @a path: its size, the SHA-256 hash and, for PNG,
 * JPEG, GIF and WebP, the dimensions from the header.  Blocking; the upload
 * runs it in a thread.  NULL if the file can't be read.
 */
JabberSfsFile *jabber_sfs_file_from_path(const char *path, const char *name,
                                         const char *media_type);

/** Width and height from a PNG, JPEG, GIF or WebP header in @a buf.
 *  Returns FALSE when neither is recognized. */
gboolean jabber_sfs_image_size(const guchar *buf, gsize len,
                               int *width, int *height);

#endif /* PURPLE_JABBER_SFS_H_ */
