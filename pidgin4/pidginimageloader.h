/*
 * pidgin4
 *
 * Pidgin is the legal property of its developers, whose names are too numerous
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

/*
 * PidginImageLoader: asynchronous loader for remote images shown inline in
 * messages (<img src="https://..."> from Discord custom emoji and
 * attachments, XEP-0363 HTTP upload links).
 *
 * - Only allowlisted hosts are fetched: cdn.discordapp.com and
 *   media.discordapp.net are built in, others are added at runtime with
 *   pidgin_image_loader_allow_host() (e.g. an account's XEP-0363 upload
 *   host). Hosts match exactly and case-insensitively; only the https and
 *   aesgcm schemes are accepted, URIs with userinfo are refused, and
 *   redirects are followed only to allowed URIs. Anything else fails with
 *   PIDGIN_IMAGE_LOADER_ERROR_NOT_ALLOWED without touching the network.
 * - One shared SoupSession; concurrent requests for the same URI share one
 *   fetch. Bodies above the size cap fail with ..._ERROR_TOO_LARGE.
 * - Images are decoded off the main thread (GdkTexture's own loaders, then
 *   GdkPixbuf for GIF and the rest; animations yield their first frame).
 * - Disk cache: one file per URI, named by the SHA-256 hex digest of the
 *   URI, mode 0600, written atomically. Hits refresh the file's mtime and
 *   the cache is trimmed oldest-mtime first down to the size limit. A small
 *   in-memory LRU of decoded textures sits in front of it.
 * - Transport errors from libsoup/GIO (G_IO_ERROR, G_RESOLVER_ERROR,
 *   G_TLS_ERROR, ...) are passed through unchanged.
 *
 * The loader is a main-thread object: call it and receive its callbacks on
 * the main context it was created in.
 *
 * Encrypted media (aesgcm://)
 * ---------------------------
 * "aesgcm://host/path#<iv><key>" (XEP-0454; the fragment is a 12- or
 * 16-byte IV followed by a 32-byte AES-256-GCM key, all hex) is fetched as
 * "https://host/path" (the host must be allowed like any https host) and
 * decrypted by the OMEMO plugin through libpurple plugin IPC:
 *
 *   plugin id: "core-omemo" (must be loaded)
 *   command:   "omemo-decrypt-url"
 *   signature: gboolean (const char *aesgcm_url, GByteArray *data)
 *   marshal:   purple_marshal_BOOLEAN__POINTER_POINTER
 *   return:    PurpleValue boolean; params: two PURPLE_TYPE_POINTER
 *
 * The plugin decrypts @data (ciphertext followed by the 16-byte GCM tag)
 * in place, resizing it to the plaintext length, and returns TRUE on
 * success; on failure it returns FALSE (and may leave @data in any state).
 * It is called on the main thread, synchronously. When the plugin or the
 * command is missing, or decryption fails, the load fails with
 * PIDGIN_IMAGE_LOADER_ERROR_DECRYPT. Decrypted images are never written to
 * disk; they are cached only in memory.
 */
#ifndef PIDGIN_IMAGE_LOADER_H
#define PIDGIN_IMAGE_LOADER_H

#include <gio/gio.h>
#include <gdk/gdk.h>

G_BEGIN_DECLS

#define PIDGIN_TYPE_IMAGE_LOADER (pidgin_image_loader_get_type())
G_DECLARE_FINAL_TYPE(PidginImageLoader, pidgin_image_loader, PIDGIN,
                     IMAGE_LOADER, GObject)

#define PIDGIN_IMAGE_LOADER_ERROR (pidgin_image_loader_error_quark())

typedef enum {
	PIDGIN_IMAGE_LOADER_ERROR_NOT_ALLOWED,
	PIDGIN_IMAGE_LOADER_ERROR_TOO_LARGE,
	PIDGIN_IMAGE_LOADER_ERROR_HTTP,
	PIDGIN_IMAGE_LOADER_ERROR_DECODE,
	PIDGIN_IMAGE_LOADER_ERROR_DECRYPT
} PidginImageLoaderError;

/** Default cap on a single image body: 16 MiB. */
#define PIDGIN_IMAGE_LOADER_DEFAULT_MAX_IMAGE_SIZE (16 * 1024 * 1024)
/** Default disk cache size limit: 256 MiB. */
#define PIDGIN_IMAGE_LOADER_DEFAULT_CACHE_LIMIT (G_GUINT64_CONSTANT(256) * 1024 * 1024)

GQuark pidgin_image_loader_error_quark(void);

/**
 * Creates a loader. @cache_dir is created (mode 0700) if needed; NULL
 * disables the disk cache (the in-memory cache stays).
 */
PidginImageLoader *pidgin_image_loader_new(const char *cache_dir);

/**
 * The application's shared loader (transfer none), caching in
 * <purple_user_dir()>/pidgin4/image-cache/.
 */
PidginImageLoader *pidgin_image_loader_get_default(void);

/** Allows fetching from @host (exact match, case-insensitive). */
void pidgin_image_loader_allow_host(PidginImageLoader *loader,
                                    const char *host);

/**
 * Allows fetching exactly @uri (https or aesgcm, compared as a string),
 * whatever its host: an attachment someone shared (an XMPP file share).
 * A redirect from it must still lead to an allowed host.
 */
void pidgin_image_loader_allow_uri(PidginImageLoader *loader, const char *uri);

/**
 * A HEAD request for @uri (https only, http in test mode; any host: it
 * fetches no body), to learn what a link is before allowing it. Uses the
 * loader's session.
 */
void pidgin_image_loader_probe_async(PidginImageLoader *loader, const char *uri,
                                     GCancellable *cancellable,
                                     GAsyncReadyCallback callback, gpointer data);

/** The Content-Type (lowercase, no parameters; "" if none) and, in @size,
 * the Content-Length or -1. NULL with @error set on failure. */
char *pidgin_image_loader_probe_finish(PidginImageLoader *loader, GAsyncResult *result,
                                       goffset *size, GError **error);

/** Whether @uri may be fetched: https/aesgcm on an allowed host, or an
 * allowed URI. */
gboolean pidgin_image_loader_is_allowed(PidginImageLoader *loader,
                                        const char *uri);

void pidgin_image_loader_set_max_image_size(PidginImageLoader *loader,
                                            gsize bytes);
void pidgin_image_loader_set_cache_limit(PidginImageLoader *loader,
                                         guint64 bytes);

/**
 * Loads @uri: memory cache, then disk cache, then the network. The
 * callback runs on the calling thread's main context.
 */
void pidgin_image_loader_load_async(PidginImageLoader *loader,
                                    const char *uri,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer data);

/** Returns the texture (transfer full), or NULL with @error set. */
GdkTexture *pidgin_image_loader_load_finish(PidginImageLoader *loader,
                                            GAsyncResult *result,
                                            GError **error);

/**
 * Synchronous cache lookup (memory, then disk; never the network), for a
 * quick re-render of already-seen images. Returns a new reference or NULL
 * on a miss. A disk hit reads and decodes the file on the calling thread.
 */
GdkTexture *pidgin_image_loader_lookup_cached(PidginImageLoader *loader,
                                              const char *uri);

/**
 * Deletes cache files, oldest mtime first, until the cache is within its
 * limit. Synchronous (directory scan on the calling thread); the loader
 * also trims on its own, in a worker thread, as it stores images.
 */
void pidgin_image_loader_trim_cache(PidginImageLoader *loader);

/** The disk cache file for @uri, or NULL without a disk cache. For tests. */
char *pidgin_image_loader_cache_path(PidginImageLoader *loader,
                                     const char *uri);

/**
 * TEST ONLY: also accept plain http:// URIs (and fetch aesgcm:// over
 * http), so the unit tests can use a local SoupServer. Never call this in
 * the application.
 */
void pidgin_image_loader_set_allow_http_for_tests(PidginImageLoader *loader,
                                                  gboolean allow);

G_END_DECLS

#endif /* PIDGIN_IMAGE_LOADER_H */
