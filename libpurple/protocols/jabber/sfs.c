/*
 * purple - Jabber Protocol Plugin
 *
 * XEP-0447 Stateless File Sharing with XEP-0446 file metadata, XEP-0264
 * thumbnails and XEP-0300 hashes; XEP-0385 SIMS on receipt.  See sfs.h.
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

#include <glib/gstdio.h>
#include <string.h>
#include <stdlib.h>

#include "debug.h"

#include "data.h"
#include "namespaces.h"
#include "sfs.h"

/* Larger thumbnails are dropped: they are meant to be small previews and the
 * meta table goes through every signal handler. */
#define SFS_MAX_THUMBNAIL_URI (128 * 1024)

void
jabber_sfs_file_free(JabberSfsFile *file)
{
	if (file == NULL)
		return;
	g_free(file->url);
	g_free(file->name);
	g_free(file->media_type);
	g_free(file->desc);
	g_free(file->hash);
	g_free(file->thumbnail);
	g_free(file->disposition);
	g_free(file);
}

/**************************************************************************
 * Parsing
 **************************************************************************/

static char *
child_text(xmlnode *parent, const char *name)
{
	xmlnode *child = xmlnode_get_child(parent, name);
	char *text = child ? xmlnode_get_data(child) : NULL;

	if (text) {
		g_strstrip(text);
		if (*text == '\0') {
			g_free(text);
			text = NULL;
		}
	}
	return text;
}

static int
child_int(xmlnode *parent, const char *name)
{
	char *text = child_text(parent, name);
	long v = text ? strtol(text, NULL, 10) : 0;

	g_free(text);
	return (v > 0 && v < G_MAXINT) ? (int)v : 0;
}

/* data:<type>;base64,<data> from base64 text (whitespace removed) */
static char *
data_uri_from_base64(const char *type, const char *b64)
{
	GString *uri = g_string_new("data:");
	const char *p;

	g_string_append(uri, (type && *type && !strpbrk(type, ";,\"'<> ")) ?
	                     type : "application/octet-stream");
	g_string_append(uri, ";base64,");
	for (p = b64; *p; p++)
		if (!g_ascii_isspace(*p))
			g_string_append_c(uri, *p);
	return g_string_free(uri, FALSE);
}

static char *
resolve_thumbnail(JabberStream *js, const char *from, xmlnode *message,
                  const char *uri)
{
	char *result = NULL;

	if (uri == NULL)
		return NULL;

	if (g_ascii_strncasecmp(uri, "data:", 5) == 0) {
		result = g_strdup(uri);
	} else if (g_ascii_strncasecmp(uri, "cid:", 4) == 0) {
		const char *cid = uri + 4;
		xmlnode *data;

		/* A <data/> element sent along in the same message */
		for (data = message ? message->child : NULL; data; data = data->next) {
			if (data->type != XMLNODE_TYPE_TAG ||
			    !purple_strequal(data->name, "data") ||
			    !purple_strequal(xmlnode_get_namespace(data), NS_BOB) ||
			    !purple_strequal(xmlnode_get_attrib(data, "cid"), cid))
				continue;
			{
				char *b64 = xmlnode_get_data(data);
				if (b64 && *b64)
					result = data_uri_from_base64(
						xmlnode_get_attrib(data, "type"), b64);
				g_free(b64);
			}
			break;
		}
		/* ... or already cached */
		if (result == NULL && js != NULL && from != NULL) {
			const JabberData *cached = jabber_data_find_remote_by_cid(js, from, cid);
			if (cached) {
				char *b64 = g_base64_encode(jabber_data_get_data(cached),
				                            jabber_data_get_size(cached));
				result = data_uri_from_base64(jabber_data_get_type(cached), b64);
				g_free(b64);
			}
		}
		if (result == NULL)
			result = g_strdup(uri);
	}
	/* Other URIs (http: …) would make the UI fetch something just for a
	 * preview; they are not passed on. */

	if (result && strlen(result) > SFS_MAX_THUMBNAIL_URI) {
		purple_debug_info("jabber", "sfs: thumbnail too large, ignored\n");
		g_free(result);
		result = NULL;
	}
	return result;
}

/* The algorithms we prefer to report, strongest well-known first. */
static int
hash_rank(const char *algo)
{
	static const char * const order[] = {
		"sha-256", "sha-512", "sha3-256", "sha3-512", "blake2b-256",
		"blake2b-512", NULL
	};
	int i;

	for (i = 0; order[i]; i++)
		if (purple_strequal(algo, order[i]))
			return i;
	return 100;
}

/* <file/> of XEP-0446 (or XEP-0234 inside SIMS): same children */
static void
parse_file(JabberSfsFile *f, JabberStream *js, const char *from,
           xmlnode *message, xmlnode *file)
{
	xmlnode *child;
	int best = G_MAXINT;
	char *text;

	f->media_type = child_text(file, "media-type");
	f->name = child_text(file, "name");
	f->desc = child_text(file, "desc");
	f->width = child_int(file, "width");
	f->height = child_int(file, "height");
	text = child_text(file, "size");
	if (text) {
		gint64 v = g_ascii_strtoll(text, NULL, 10);
		f->size = v >= 0 ? v : -1;
		g_free(text);
	}

	/* A name is a file name, never a path. */
	if (f->name && (strchr(f->name, '/') || strchr(f->name, '\\'))) {
		char *base = g_path_get_basename(f->name);
		g_free(f->name);
		f->name = base;
	}

	for (child = file->child; child; child = child->next) {
		const char *xmlns;

		if (child->type != XMLNODE_TYPE_TAG)
			continue;
		xmlns = xmlnode_get_namespace(child);

		if (purple_strequal(child->name, "hash") &&
		    purple_strequal(xmlns, NS_HASHES_2)) {
			const char *algo = xmlnode_get_attrib(child, "algo");
			int rank = hash_rank(algo);
			char *value;

			if (algo == NULL || *algo == '\0' || rank >= best)
				continue;
			value = xmlnode_get_data(child);
			if (value) {
				g_strstrip(value);
				if (*value) {
					g_free(f->hash);
					f->hash = g_strdup_printf("%s:%s", algo, value);
					best = rank;
				}
				g_free(value);
			}
		} else if (purple_strequal(child->name, "thumbnail") &&
		           (purple_strequal(xmlns, NS_THUMBS_1) ||
		            purple_strequal(xmlns, NS_THUMBS)) &&
		           f->thumbnail == NULL) {
			f->thumbnail = resolve_thumbnail(js, from, message,
			                                 xmlnode_get_attrib(child, "uri"));
		}
	}
}

static gboolean
url_ok(const char *url)
{
	return url && (g_ascii_strncasecmp(url, "https://", 8) == 0 ||
	               g_ascii_strncasecmp(url, "http://", 7) == 0);
}

JabberSfsFile *
jabber_sfs_parse_element(JabberStream *js, const char *from,
                         xmlnode *message, xmlnode *element)
{
	JabberSfsFile *f;
	xmlnode *file, *sources, *src;
	const char *xmlns = xmlnode_get_namespace(element);

	if (purple_strequal(element->name, "file-sharing") &&
	    purple_strequal(xmlns, NS_SFS)) {
		file = xmlnode_get_child_with_namespace(element, "file", NS_FILE_METADATA);
		if (file == NULL)
			return NULL;

		f = g_new0(JabberSfsFile, 1);
		f->size = -1;
		f->disposition = g_strdup(xmlnode_get_attrib(element, "disposition"));
		parse_file(f, js, from, message, file);

		sources = xmlnode_get_child(element, "sources");
		for (src = sources ? sources->child : NULL; src; src = src->next) {
			const char *target;

			if (src->type != XMLNODE_TYPE_TAG ||
			    !purple_strequal(src->name, "url-data") ||
			    !purple_strequal(xmlnode_get_namespace(src), NS_URL_DATA))
				continue;
			target = xmlnode_get_attrib(src, "target");
			if (url_ok(target)) {
				f->url = g_strdup(target);
				break;
			}
		}
		return f;
	}

	if (purple_strequal(element->name, "reference") &&
	    purple_strequal(xmlns, NS_REFERENCE) &&
	    purple_strequal(xmlnode_get_attrib(element, "type"), "data")) {
		xmlnode *ms = xmlnode_get_child_with_namespace(element,
		                                               "media-sharing", NS_SIMS);

		file = ms ? xmlnode_get_child_with_namespace(ms, "file",
		                                             NS_JINGLE_FT_5) : NULL;
		if (file == NULL)
			return NULL;

		f = g_new0(JabberSfsFile, 1);
		f->size = -1;
		f->sims = TRUE;
		parse_file(f, js, from, message, file);

		sources = xmlnode_get_child(ms, "sources");
		for (src = sources ? sources->child : NULL; src; src = src->next) {
			const char *uri;

			if (src->type != XMLNODE_TYPE_TAG ||
			    !purple_strequal(src->name, "reference"))
				continue;
			uri = xmlnode_get_attrib(src, "uri");
			if (url_ok(uri)) {
				f->url = g_strdup(uri);
				break;
			}
		}
		return f;
	}

	return NULL;
}

JabberSfsFile *
jabber_sfs_parse(JabberStream *js, const char *from, xmlnode *message)
{
	xmlnode *child;

	for (child = message ? message->child : NULL; child; child = child->next) {
		JabberSfsFile *f;

		if (child->type != XMLNODE_TYPE_TAG)
			continue;
		f = jabber_sfs_parse_element(js, from, message, child);
		if (f)
			return f;
	}
	return NULL;
}

static void
meta_put(GHashTable *meta, const char *key, const char *value)
{
	if (value != NULL)
		g_hash_table_insert(meta, g_strdup(key), g_strdup(value));
}

static void
meta_put_int(GHashTable *meta, const char *key, gint64 value)
{
	char *tmp = g_strdup_printf("%" G_GINT64_FORMAT, value);
	meta_put(meta, key, tmp);
	g_free(tmp);
}

void
jabber_sfs_to_meta(const JabberSfsFile *f, GHashTable *meta)
{
	if (f == NULL)
		return;
	meta_put(meta, "sfs-url", f->url);
	meta_put(meta, "sfs-name", f->name);
	if (f->size >= 0)
		meta_put_int(meta, "sfs-size", f->size);
	meta_put(meta, "sfs-media-type", f->media_type);
	if (f->width > 0)
		meta_put_int(meta, "sfs-width", f->width);
	if (f->height > 0)
		meta_put_int(meta, "sfs-height", f->height);
	meta_put(meta, "sfs-desc", f->desc);
	meta_put(meta, "sfs-hash", f->hash);
	meta_put(meta, "sfs-thumbnail", f->thumbnail);
	meta_put(meta, "sfs-disposition", f->disposition);
	if (f->sims)
		meta_put(meta, "sfs-sims", "1");
}

/**************************************************************************
 * Building
 **************************************************************************/

static void
add_text_child(xmlnode *parent, const char *name, const char *text)
{
	if (text && *text)
		xmlnode_insert_data(xmlnode_new_child(parent, name), text, -1);
}

static void
add_int_child(xmlnode *parent, const char *name, gint64 value)
{
	char *tmp = g_strdup_printf("%" G_GINT64_FORMAT, value);
	add_text_child(parent, name, tmp);
	g_free(tmp);
}

xmlnode *
jabber_sfs_build(const JabberSfsFile *f)
{
	xmlnode *sfs, *file, *sources, *node;

	sfs = xmlnode_new("file-sharing");
	xmlnode_set_namespace(sfs, NS_SFS);
	if (f->disposition)
		xmlnode_set_attrib(sfs, "disposition", f->disposition);

	file = xmlnode_new_child(sfs, "file");
	xmlnode_set_namespace(file, NS_FILE_METADATA);
	add_text_child(file, "media-type", f->media_type);
	add_text_child(file, "name", f->name);
	if (f->size >= 0)
		add_int_child(file, "size", f->size);
	if (f->width > 0 && f->height > 0) {
		add_int_child(file, "width", f->width);
		add_int_child(file, "height", f->height);
	}
	add_text_child(file, "desc", f->desc);
	if (f->hash) {
		const char *colon = strchr(f->hash, ':');
		if (colon && colon[1]) {
			char *algo = g_strndup(f->hash, colon - f->hash);
			node = xmlnode_new_child(file, "hash");
			xmlnode_set_namespace(node, NS_HASHES_2);
			xmlnode_set_attrib(node, "algo", algo);
			xmlnode_insert_data(node, colon + 1, -1);
			g_free(algo);
		}
	}

	sources = xmlnode_new_child(sfs, "sources");
	if (f->url) {
		node = xmlnode_new_child(sources, "url-data");
		xmlnode_set_namespace(node, NS_URL_DATA);
		xmlnode_set_attrib(node, "target", f->url);
	}

	return sfs;
}

/**************************************************************************
 * Local files
 **************************************************************************/

static guint32
be32(const guchar *p)
{
	return ((guint32)p[0] << 24) | ((guint32)p[1] << 16) |
	       ((guint32)p[2] << 8) | p[3];
}

static guint
be16(const guchar *p)
{
	return ((guint)p[0] << 8) | p[1];
}

static guint
le16(const guchar *p)
{
	return p[0] | ((guint)p[1] << 8);
}

static guint32
le24(const guchar *p)
{
	return p[0] | ((guint32)p[1] << 8) | ((guint32)p[2] << 16);
}

static gboolean
jpeg_size(const guchar *buf, gsize len, int *w, int *h)
{
	gsize i = 2;

	while (i + 4 <= len) {
		guchar marker;
		guint seglen;

		if (buf[i] != 0xFF)
			return FALSE;
		marker = buf[i + 1];
		if (marker == 0xFF) {        /* fill byte */
			i++;
			continue;
		}
		if (marker == 0xD8 || marker == 0x01 ||
		    (marker >= 0xD0 && marker <= 0xD7)) {  /* no length */
			i += 2;
			continue;
		}
		if (marker == 0xD9 || marker == 0xDA)      /* EOI, SOS: too far */
			return FALSE;
		seglen = be16(buf + i + 2);
		if (seglen < 2)
			return FALSE;
		if (marker >= 0xC0 && marker <= 0xCF &&
		    marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
			if (i + 9 > len)
				return FALSE;
			*h = be16(buf + i + 5);
			*w = be16(buf + i + 7);
			return *w > 0 && *h > 0;
		}
		i += 2 + seglen;
	}
	return FALSE;
}

gboolean
jabber_sfs_image_size(const guchar *buf, gsize len, int *width, int *height)
{
	int w = 0, h = 0;
	gboolean ok = FALSE;

	if (len >= 24 && memcmp(buf, "\x89PNG\r\n\x1a\n", 8) == 0 &&
	    memcmp(buf + 12, "IHDR", 4) == 0) {
		guint32 uw = be32(buf + 16), uh = be32(buf + 20);
		if (uw > 0 && uh > 0 && uw < G_MAXINT && uh < G_MAXINT) {
			w = uw;
			h = uh;
			ok = TRUE;
		}
	} else if (len >= 10 && (memcmp(buf, "GIF87a", 6) == 0 ||
	                         memcmp(buf, "GIF89a", 6) == 0)) {
		w = le16(buf + 6);
		h = le16(buf + 8);
		ok = w > 0 && h > 0;
	} else if (len >= 4 && buf[0] == 0xFF && buf[1] == 0xD8 && buf[2] == 0xFF) {
		ok = jpeg_size(buf, len, &w, &h);
	} else if (len >= 30 && memcmp(buf, "RIFF", 4) == 0 &&
	           memcmp(buf + 8, "WEBP", 4) == 0) {
		const guchar *c = buf + 12;
		if (memcmp(c, "VP8 ", 4) == 0 && c[11] == 0x9d && c[12] == 0x01 &&
		    c[13] == 0x2a) {
			w = le16(c + 14) & 0x3fff;
			h = le16(c + 16) & 0x3fff;
			ok = w > 0 && h > 0;
		} else if (memcmp(c, "VP8L", 4) == 0 && c[8] == 0x2f) {
			const guchar *b = c + 9;
			w = 1 + (b[0] | ((b[1] & 0x3f) << 8));
			h = 1 + ((b[1] >> 6) | (b[2] << 2) | ((b[3] & 0x0f) << 10));
			ok = TRUE;
		} else if (memcmp(c, "VP8X", 4) == 0) {
			w = 1 + le24(c + 12);
			h = 1 + le24(c + 15);
			ok = TRUE;
		}
	}

	if (ok) {
		if (width)
			*width = w;
		if (height)
			*height = h;
	}
	return ok;
}

JabberSfsFile *
jabber_sfs_file_from_path(const char *path, const char *name,
                          const char *media_type)
{
	JabberSfsFile *f;
	GChecksum *sha;
	FILE *fp;
	guchar buf[16384];
	guchar *head;
	const gsize head_size = 65536;
	gsize head_len = 0, n;
	goffset total = 0;
	guint8 digest[32];
	gsize digest_len = sizeof(digest);
	char *b64;

	if (path == NULL || (fp = g_fopen(path, "rb")) == NULL)
		return NULL;

	head = g_malloc(head_size);
	sha = g_checksum_new(G_CHECKSUM_SHA256);
	while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
		g_checksum_update(sha, buf, n);
		if (head_len < head_size) {
			gsize take = MIN(n, head_size - head_len);
			memcpy(head + head_len, buf, take);
			head_len += take;
		}
		total += n;
	}
	if (ferror(fp)) {
		fclose(fp);
		g_checksum_free(sha);
		g_free(head);
		return NULL;
	}
	fclose(fp);

	g_checksum_get_digest(sha, digest, &digest_len);
	g_checksum_free(sha);
	b64 = g_base64_encode(digest, digest_len);

	f = g_new0(JabberSfsFile, 1);
	f->size = total;
	f->hash = g_strdup_printf("sha-256:%s", b64);
	g_free(b64);
	f->name = g_strdup(name ? name : NULL);
	if (f->name == NULL)
		f->name = g_path_get_basename(path);
	f->media_type = g_strdup(media_type);
	if (jabber_sfs_image_size(head, head_len, &f->width, &f->height) ||
	    (media_type && g_str_has_prefix(media_type, "image/")))
		f->disposition = g_strdup("inline");
	g_free(head);
	return f;
}
