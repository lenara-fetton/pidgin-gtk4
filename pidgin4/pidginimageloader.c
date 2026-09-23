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
#include "pidgin-internal.h"

#include <utime.h>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <libsoup/soup.h>

#include "plugin.h"
#include "util.h"

#include "pidginimageloader.h"

#define MEMORY_CACHE_ENTRIES 48
#define READ_CHUNK_SIZE (64 * 1024)
#define MAX_REDIRECTS 5
#define TRIM_EVERY_N_STORES 32
#define CACHE_SIZE_UNKNOWN G_MAXUINT64
#define CACHE_NAME_LENGTH 64   /* SHA-256 hex */

#define OMEMO_PLUGIN_ID "core-omemo"
#define OMEMO_DECRYPT_COMMAND "omemo-decrypt-url"

#define URI_PARSE_FLAGS (G_URI_FLAGS_ENCODED_PATH | G_URI_FLAGS_ENCODED_QUERY | \
                         G_URI_FLAGS_ENCODED_FRAGMENT)

struct _PidginImageLoader {
	GObject parent;

	char *cache_dir;
	GHashTable *allowed_hosts;  /* lowercase host -> itself */
	GHashTable *allowed_uris;   /* exact URI -> itself */
	gboolean allow_http;
	gsize max_image_size;
	guint64 cache_limit;

	GMainContext *context;
	SoupSession *session;       /* created on first use */

	GHashTable *fetches;        /* uri -> Fetch, in progress */

	GQueue memory_lru;          /* MemoryEntry, most recent first */
	GHashTable *memory_cache;   /* uri -> GList link in memory_lru */

	guint64 cache_size;         /* estimate, or CACHE_SIZE_UNKNOWN */
	guint stores_since_trim;
	gboolean trimming;
	gboolean trim_again;
};

G_DEFINE_TYPE(PidginImageLoader, pidgin_image_loader, G_TYPE_OBJECT)

G_DEFINE_QUARK(pidgin-image-loader-error-quark, pidgin_image_loader_error)

static const char *const builtin_hosts[] = {
	/* Discord: custom emoji, attachments */
	"cdn.discordapp.com",
	"media.discordapp.net",
	/* Steam: images shared in chat */
	"images.steamusercontent.com",
	"steamusercontent-a.akamaihd.net",
	/* Steam: emoticons (community.cloudflare redirects to community) */
	"steamcommunity-a.akamaihd.net",
	"community.cloudflare.steamstatic.com",
	"community.steamstatic.com",
	/* Steam: game images and icons, avatars */
	"cdn.cloudflare.steamstatic.com",
	"shared.akamai.steamstatic.com",
	"avatars.steamstatic.com",
	"media.steampowered.com",
};

static PidginImageLoader *default_loader = NULL;

/* One network/disk load of a URI, shared by every request for it. */
typedef struct {
	int ref;
	PidginImageLoader *loader;
	char *uri;           /* as requested; the coalescing key */
	char *fetch_uri;     /* what goes over the wire */
	gboolean encrypted;  /* aesgcm:// */
	char *cache_path;    /* NULL: no disk cache for this one */
	GList *waiters;      /* Waiter */
	GCancellable *cancellable;
	gboolean done;
	SoupMessage *msg;
	GInputStream *stream;
	GByteArray *body;
	guint redirects;
} Fetch;

typedef struct {
	GTask *task;
	GCancellable *cancellable;
	gulong cancelled_id;
} Waiter;

typedef struct {
	Fetch *fetch;
	GTask *task;
} WaiterCancel;

typedef struct {
	char *uri;
	GdkTexture *texture;
} MemoryEntry;

typedef struct {
	GBytes *data;
	char *cache_path;
	gsize stored;        /* set by the thread: bytes written to the cache */
} DecodeJob;

typedef struct {
	char *path;
	gint64 mtime;        /* nanoseconds */
	goffset size;
} CacheFile;

typedef struct {
	char *dir;
	guint64 limit;
} TrimJob;

static void fetch_start_network(Fetch *fetch);

/**************************************************************************
 * URIs and the allowlist
 **************************************************************************/

/* Parses @uri and returns it if it may be fetched. *encrypted says whether
 * it is an aesgcm:// URI. */
static GUri *
parse_allowed(PidginImageLoader *loader, const char *uri, gboolean *encrypted)
{
	GUri *guri;
	const char *scheme;
	const char *host;
	char *lower;
	gboolean allowed;
	gboolean aesgcm = FALSE;

	if (uri == NULL)
		return NULL;

	guri = g_uri_parse(uri, URI_PARSE_FLAGS, NULL);
	if (guri == NULL)
		return NULL;

	scheme = g_uri_get_scheme(guri);
	if (g_ascii_strcasecmp(scheme, "aesgcm") == 0) {
		aesgcm = TRUE;
	} else if (g_ascii_strcasecmp(scheme, "https") != 0 &&
	           !(loader->allow_http && g_ascii_strcasecmp(scheme, "http") == 0)) {
		g_uri_unref(guri);
		return NULL;
	}

	/* "https://cdn.discordapp.com@evil.com/" and friends: no userinfo. */
	host = g_uri_get_host(guri);
	if (g_uri_get_userinfo(guri) != NULL || host == NULL || *host == '\0') {
		g_uri_unref(guri);
		return NULL;
	}

	lower = g_ascii_strdown(host, -1);
	allowed = g_hash_table_contains(loader->allowed_hosts, lower) ||
	          g_hash_table_contains(loader->allowed_uris, uri);
	g_free(lower);

	if (!allowed) {
		g_uri_unref(guri);
		return NULL;
	}

	if (encrypted != NULL)
		*encrypted = aesgcm;
	return guri;
}

/* The URI that is actually requested: the same one without the fragment,
 * and for aesgcm:// with the https scheme (http in test mode). */
static char *
make_fetch_uri(PidginImageLoader *loader, GUri *guri, gboolean encrypted)
{
	const char *scheme = g_uri_get_scheme(guri);
	GUri *built;
	char *str;

	if (encrypted)
		scheme = loader->allow_http ? "http" : "https";

	built = g_uri_build(g_uri_get_flags(guri), scheme, NULL,
	                    g_uri_get_host(guri), g_uri_get_port(guri),
	                    g_uri_get_path(guri), g_uri_get_query(guri), NULL);
	str = g_uri_to_string(built);
	g_uri_unref(built);

	return str;
}

/* 12- or 16-byte IV followed by a 32-byte key, as hex. */
static gboolean
aesgcm_fragment_valid(const char *fragment)
{
	gsize len, i;

	if (fragment == NULL)
		return FALSE;

	len = strlen(fragment);
	if (len != (12 + 32) * 2 && len != (16 + 32) * 2)
		return FALSE;

	for (i = 0; i < len; i++) {
		if (!g_ascii_isxdigit(fragment[i]))
			return FALSE;
	}

	return TRUE;
}

static char *
cache_path_for(PidginImageLoader *loader, const char *uri, gboolean encrypted)
{
	char *digest;
	char *path;

	if (loader->cache_dir == NULL || encrypted)
		return NULL;

	digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, uri, -1);
	path = g_build_filename(loader->cache_dir, digest, NULL);
	g_free(digest);

	return path;
}

/**************************************************************************
 * Decoding (any thread)
 **************************************************************************/

static GdkTexture *
texture_from_pixbuf(GdkPixbuf *pixbuf)
{
	GdkTexture *texture;
	GBytes *pixels;

	if (gdk_pixbuf_get_bits_per_sample(pixbuf) != 8 ||
	    gdk_pixbuf_get_colorspace(pixbuf) != GDK_COLORSPACE_RGB)
		return NULL;

	pixels = gdk_pixbuf_read_pixel_bytes(pixbuf);
	texture = gdk_memory_texture_new(gdk_pixbuf_get_width(pixbuf),
	                                 gdk_pixbuf_get_height(pixbuf),
	                                 gdk_pixbuf_get_has_alpha(pixbuf) ?
	                                 GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
	                                 pixels,
	                                 gdk_pixbuf_get_rowstride(pixbuf));
	g_bytes_unref(pixels);

	return texture;
}

static GdkTexture *
decode_bytes(GBytes *bytes, GError **error)
{
	GdkTexture *texture;
	GdkPixbufLoader *pixbuf_loader;
	GdkPixbuf *pixbuf;
	gboolean ok;

	if (g_bytes_get_size(bytes) == 0) {
		g_set_error_literal(error, PIDGIN_IMAGE_LOADER_ERROR,
		                    PIDGIN_IMAGE_LOADER_ERROR_DECODE,
		                    _("The image is empty"));
		return NULL;
	}

	/* PNG, JPEG and TIFF. */
	texture = gdk_texture_new_from_bytes(bytes, NULL);
	if (texture != NULL)
		return texture;

	/* GIF, WebP and whatever else gdk-pixbuf has loaders for; for
	 * animations this is the first frame. */
	pixbuf_loader = gdk_pixbuf_loader_new();
	ok = gdk_pixbuf_loader_write_bytes(pixbuf_loader, bytes, NULL);
	/* Always close, or the loader complains when finalized. */
	ok = gdk_pixbuf_loader_close(pixbuf_loader, NULL) && ok;

	pixbuf = ok ? gdk_pixbuf_loader_get_pixbuf(pixbuf_loader) : NULL;
	if (pixbuf != NULL)
		texture = texture_from_pixbuf(pixbuf);
	g_object_unref(pixbuf_loader);

	if (texture == NULL) {
		g_set_error_literal(error, PIDGIN_IMAGE_LOADER_ERROR,
		                    PIDGIN_IMAGE_LOADER_ERROR_DECODE,
		                    _("The image could not be decoded"));
	}

	return texture;
}

/* Reads and decodes a cache file, refreshing its mtime; a file that no
 * longer decodes is removed. */
static GdkTexture *
read_cache_file(const char *path)
{
	GdkTexture *texture;
	GBytes *bytes;
	char *contents;
	gsize len;

	if (!g_file_get_contents(path, &contents, &len, NULL))
		return NULL;

	bytes = g_bytes_new_take(contents, len);
	texture = decode_bytes(bytes, NULL);
	g_bytes_unref(bytes);

	if (texture != NULL)
		g_utime(path, NULL);
	else
		g_unlink(path);

	return texture;
}

/**************************************************************************
 * Disk cache trimming (any thread)
 **************************************************************************/

static gboolean
is_cache_name(const char *name)
{
	int i;

	for (i = 0; i < CACHE_NAME_LENGTH; i++) {
		if (!g_ascii_isxdigit(name[i]) || g_ascii_isupper(name[i]))
			return FALSE;
	}

	return name[CACHE_NAME_LENGTH] == '\0';
}

static int
cache_file_compare(gconstpointer a, gconstpointer b)
{
	const CacheFile *fa = a, *fb = b;

	if (fa->mtime != fb->mtime)
		return fa->mtime < fb->mtime ? -1 : 1;
	return strcmp(fa->path, fb->path);
}

/* Deletes the oldest cache files until at most @limit bytes remain;
 * returns the remaining total. Only files named like cache entries are
 * counted or touched. */
static guint64
trim_directory(const char *dir_path, guint64 limit)
{
	GDir *dir;
	GArray *files;
	const char *name;
	guint64 total = 0;
	guint i;

	dir = g_dir_open(dir_path, 0, NULL);
	if (dir == NULL)
		return 0;

	files = g_array_new(FALSE, FALSE, sizeof(CacheFile));
	while ((name = g_dir_read_name(dir)) != NULL) {
		CacheFile file;
		struct stat st;

		if (!is_cache_name(name))
			continue;

		file.path = g_build_filename(dir_path, name, NULL);
		if (lstat(file.path, &st) != 0 || !S_ISREG(st.st_mode)) {
			g_free(file.path);
			continue;
		}

		file.mtime = (gint64)st.st_mtim.tv_sec * G_GINT64_CONSTANT(1000000000) +
		             st.st_mtim.tv_nsec;
		file.size = st.st_size;
		total += st.st_size;
		g_array_append_val(files, file);
	}
	g_dir_close(dir);

	g_array_sort(files, cache_file_compare);

	for (i = 0; i < files->len; i++) {
		CacheFile *file = &g_array_index(files, CacheFile, i);

		if (total > limit && g_unlink(file->path) == 0)
			total -= file->size;
		g_free(file->path);
	}
	g_array_free(files, TRUE);

	return total;
}

static void
trim_job_free(TrimJob *job)
{
	g_free(job->dir);
	g_free(job);
}

static void
trim_thread(GTask *task, gpointer source, gpointer task_data,
            GCancellable *cancellable)
{
	TrimJob *job = task_data;

	g_task_return_int(task, (gssize)trim_directory(job->dir, job->limit));
}

static void schedule_trim(PidginImageLoader *loader);

static void
trim_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	PidginImageLoader *loader = PIDGIN_IMAGE_LOADER(source);
	gssize total = g_task_propagate_int(G_TASK(result), NULL);

	loader->trimming = FALSE;
	if (total >= 0)
		loader->cache_size = (guint64)total;

	if (loader->trim_again) {
		loader->trim_again = FALSE;
		schedule_trim(loader);
	}
}

static void
schedule_trim(PidginImageLoader *loader)
{
	GTask *task;
	TrimJob *job;

	if (loader->cache_dir == NULL)
		return;

	if (loader->trimming) {
		loader->trim_again = TRUE;
		return;
	}

	loader->trimming = TRUE;
	loader->stores_since_trim = 0;

	job = g_new0(TrimJob, 1);
	job->dir = g_strdup(loader->cache_dir);
	job->limit = loader->cache_limit;

	task = g_task_new(loader, NULL, trim_cb, NULL);
	g_task_set_source_tag(task, schedule_trim);
	g_task_set_task_data(task, job, (GDestroyNotify)trim_job_free);
	g_task_run_in_thread(task, trim_thread);
	g_object_unref(task);
}

/* Throttled: a trim needs a directory scan, so it runs when the estimate
 * says the cache is over its limit, or every few stores. */
static void
cache_stored(PidginImageLoader *loader, gsize size)
{
	if (loader->cache_size != CACHE_SIZE_UNKNOWN)
		loader->cache_size += size;
	loader->stores_since_trim++;

	if (loader->cache_size == CACHE_SIZE_UNKNOWN ||
	    loader->cache_size > loader->cache_limit ||
	    loader->stores_since_trim >= TRIM_EVERY_N_STORES)
		schedule_trim(loader);
}

/**************************************************************************
 * Memory cache
 **************************************************************************/

static void
memory_entry_free(MemoryEntry *entry)
{
	g_free(entry->uri);
	g_object_unref(entry->texture);
	g_free(entry);
}

static GdkTexture *
memory_lookup(PidginImageLoader *loader, const char *uri)
{
	GList *link = g_hash_table_lookup(loader->memory_cache, uri);
	MemoryEntry *entry;

	if (link == NULL)
		return NULL;

	g_queue_unlink(&loader->memory_lru, link);
	g_queue_push_head_link(&loader->memory_lru, link);
	entry = link->data;

	return g_object_ref(entry->texture);
}

static void
memory_store(PidginImageLoader *loader, const char *uri, GdkTexture *texture)
{
	GList *link = g_hash_table_lookup(loader->memory_cache, uri);
	MemoryEntry *entry;

	if (link != NULL) {
		entry = link->data;
		g_set_object(&entry->texture, texture);
		g_queue_unlink(&loader->memory_lru, link);
		g_queue_push_head_link(&loader->memory_lru, link);
		return;
	}

	entry = g_new0(MemoryEntry, 1);
	entry->uri = g_strdup(uri);
	entry->texture = g_object_ref(texture);
	g_queue_push_head(&loader->memory_lru, entry);
	g_hash_table_insert(loader->memory_cache, entry->uri,
	                    loader->memory_lru.head);

	while (loader->memory_lru.length > MEMORY_CACHE_ENTRIES) {
		entry = g_queue_pop_tail(&loader->memory_lru);
		g_hash_table_remove(loader->memory_cache, entry->uri);
		memory_entry_free(entry);
	}
}

/**************************************************************************
 * OMEMO decryption through plugin IPC (main thread)
 **************************************************************************/

static PurplePlugin *
omemo_plugin(void)
{
	PurplePlugin *plugin = purple_plugins_find_with_id(OMEMO_PLUGIN_ID);
	int num_params = 0;

	if (plugin == NULL || !purple_plugin_is_loaded(plugin))
		return NULL;

	if (!purple_plugin_ipc_get_params(plugin, OMEMO_DECRYPT_COMMAND, NULL,
	                                  &num_params, NULL) ||
	    num_params != 2)
		return NULL;

	return plugin;
}

static gboolean
omemo_decrypt(const char *uri, GByteArray *data)
{
	PurplePlugin *plugin = omemo_plugin();
	gboolean ok = FALSE;
	gpointer result;

	if (plugin == NULL)
		return FALSE;

	result = purple_plugin_ipc_call(plugin, OMEMO_DECRYPT_COMMAND, &ok,
	                                uri, data);

	return ok && GPOINTER_TO_INT(result);
}

/**************************************************************************
 * Fetches
 **************************************************************************/

static Fetch *
fetch_ref(Fetch *fetch)
{
	fetch->ref++;
	return fetch;
}

static void
fetch_unref(Fetch *fetch)
{
	if (--fetch->ref > 0)
		return;

	g_warn_if_fail(fetch->waiters == NULL);

	g_clear_object(&fetch->msg);
	g_clear_object(&fetch->stream);
	if (fetch->body != NULL)
		g_byte_array_unref(fetch->body);
	g_object_unref(fetch->cancellable);
	g_object_unref(fetch->loader);
	g_free(fetch->uri);
	g_free(fetch->fetch_uri);
	g_free(fetch->cache_path);
	g_free(fetch);
}

static void
waiter_finish(Waiter *waiter, GdkTexture *texture, const GError *error)
{
	if (waiter->cancellable != NULL) {
		g_cancellable_disconnect(waiter->cancellable, waiter->cancelled_id);
		g_object_unref(waiter->cancellable);
	}

	if (texture != NULL) {
		g_task_return_pointer(waiter->task, g_object_ref(texture),
		                      g_object_unref);
	} else {
		g_task_return_error(waiter->task, g_error_copy(error));
	}

	g_object_unref(waiter->task);
	g_free(waiter);
}

static void
fetch_complete(Fetch *fetch, GdkTexture *texture, const GError *error)
{
	PidginImageLoader *loader = fetch->loader;
	GList *waiters, *l;

	if (fetch->done)
		return;
	fetch->done = TRUE;

	fetch_ref(fetch);

	if (g_hash_table_lookup(loader->fetches, fetch->uri) == fetch)
		g_hash_table_remove(loader->fetches, fetch->uri);

	if (texture != NULL)
		memory_store(loader, fetch->uri, texture);

	waiters = fetch->waiters;
	fetch->waiters = NULL;
	for (l = waiters; l != NULL; l = l->next)
		waiter_finish(l->data, texture, error);
	g_list_free(waiters);

	g_clear_object(&fetch->stream);
	g_clear_object(&fetch->msg);

	fetch_unref(fetch);
}

static void
fetch_fail(Fetch *fetch, int code, const char *message)
{
	GError *error = g_error_new_literal(PIDGIN_IMAGE_LOADER_ERROR, code,
	                                    message);

	fetch_complete(fetch, NULL, error);
	g_error_free(error);
}

static void
waiter_cancel_free(gpointer data)
{
	WaiterCancel *cancel = data;

	fetch_unref(cancel->fetch);
	g_object_unref(cancel->task);
	g_free(cancel);
}

/* On the loader's main context, outside the "cancelled" handler (which may
 * not disconnect itself). */
static gboolean
waiter_cancelled_idle(gpointer data)
{
	WaiterCancel *cancel = data;
	Fetch *fetch = cancel->fetch;
	Waiter *waiter = NULL;
	GError *error;
	GList *l;

	for (l = fetch->waiters; l != NULL; l = l->next) {
		if (((Waiter *)l->data)->task == cancel->task) {
			waiter = l->data;
			break;
		}
	}

	if (waiter == NULL)
		return G_SOURCE_REMOVE;   /* already finished */

	fetch->waiters = g_list_delete_link(fetch->waiters, l);
	error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED,
	                            _("Operation was cancelled"));
	waiter_finish(waiter, NULL, error);
	g_error_free(error);

	/* Nobody wants it any more: stop the fetch, and let new requests for
	 * the URI start afresh rather than join the dying one. */
	if (fetch->waiters == NULL && !fetch->done) {
		PidginImageLoader *loader = fetch->loader;

		if (g_hash_table_lookup(loader->fetches, fetch->uri) == fetch)
			g_hash_table_remove(loader->fetches, fetch->uri);
		g_cancellable_cancel(fetch->cancellable);
	}

	return G_SOURCE_REMOVE;
}

static void
waiter_cancelled_cb(GCancellable *cancellable, gpointer data)
{
	WaiterCancel *cancel = data;
	WaiterCancel *copy = g_new0(WaiterCancel, 1);
	GSource *source;

	copy->fetch = fetch_ref(cancel->fetch);
	copy->task = g_object_ref(cancel->task);

	source = g_idle_source_new();
	g_source_set_priority(source, G_PRIORITY_DEFAULT);
	g_source_set_callback(source, waiter_cancelled_idle, copy,
	                      waiter_cancel_free);
	g_source_attach(source, cancel->fetch->loader->context);
	g_source_unref(source);
}

static void
fetch_add_waiter(Fetch *fetch, GTask *task)
{
	Waiter *waiter = g_new0(Waiter, 1);
	GCancellable *cancellable = g_task_get_cancellable(task);

	waiter->task = g_object_ref(task);
	fetch->waiters = g_list_append(fetch->waiters, waiter);

	if (cancellable != NULL) {
		WaiterCancel *cancel = g_new0(WaiterCancel, 1);

		cancel->fetch = fetch_ref(fetch);
		cancel->task = g_object_ref(task);
		waiter->cancellable = g_object_ref(cancellable);
		waiter->cancelled_id = g_cancellable_connect(cancellable,
		                                             G_CALLBACK(waiter_cancelled_cb),
		                                             cancel, waiter_cancel_free);
	}
}

/* Decoding (and storing into the disk cache) in a worker thread. */

static void
decode_job_free(DecodeJob *job)
{
	g_bytes_unref(job->data);
	g_free(job->cache_path);
	g_free(job);
}

static void
decode_thread(GTask *task, gpointer source, gpointer task_data,
              GCancellable *cancellable)
{
	DecodeJob *job = task_data;
	GdkTexture *texture;
	GError *error = NULL;

	texture = decode_bytes(job->data, &error);
	if (texture == NULL) {
		g_task_return_error(task, error);
		return;
	}

	if (job->cache_path != NULL) {
		char *dir = g_path_get_dirname(job->cache_path);
		gsize len;
		const char *contents = g_bytes_get_data(job->data, &len);

		g_mkdir_with_parents(dir, 0700);
		if (g_file_set_contents_full(job->cache_path, contents, len,
		                             G_FILE_SET_CONTENTS_CONSISTENT, 0600,
		                             &error)) {
			job->stored = len;
		} else {
			g_debug("image cache: %s", error->message);
			g_clear_error(&error);
		}
		g_free(dir);
	}

	g_task_return_pointer(task, texture, g_object_unref);
}

static void
decode_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	Fetch *fetch = data;
	DecodeJob *job = g_task_get_task_data(G_TASK(result));
	GdkTexture *texture;
	GError *error = NULL;

	texture = g_task_propagate_pointer(G_TASK(result), &error);

	if (job->stored > 0)
		cache_stored(fetch->loader, job->stored);

	fetch_complete(fetch, texture, error);

	g_clear_object(&texture);
	g_clear_error(&error);
	fetch_unref(fetch);
}

static void
fetch_decode(Fetch *fetch, GBytes *data)
{
	DecodeJob *job = g_new0(DecodeJob, 1);
	GTask *task;

	job->data = data;
	job->cache_path = g_strdup(fetch->cache_path);

	task = g_task_new(NULL, fetch->cancellable, decode_cb, fetch_ref(fetch));
	g_task_set_source_tag(task, fetch_decode);
	g_task_set_task_data(task, job, (GDestroyNotify)decode_job_free);
	g_task_run_in_thread(task, decode_thread);
	g_object_unref(task);
}

static void
fetch_body_done(Fetch *fetch)
{
	GByteArray *body = fetch->body;

	fetch->body = NULL;
	g_clear_object(&fetch->stream);

	if (fetch->encrypted) {
		/* The plugin may have been unloaded meanwhile. */
		if (!omemo_decrypt(fetch->uri, body)) {
			g_byte_array_unref(body);
			fetch_fail(fetch, PIDGIN_IMAGE_LOADER_ERROR_DECRYPT,
			           _("The encrypted image could not be decrypted"));
			return;
		}
	}

	fetch_decode(fetch, g_byte_array_free_to_bytes(body));
}

static void fetch_read_next(Fetch *fetch);

static void
fetch_read_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	Fetch *fetch = data;
	GBytes *bytes;
	GError *error = NULL;
	gsize size;

	bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result,
	                                         &error);
	if (bytes == NULL) {
		fetch_complete(fetch, NULL, error);
		g_error_free(error);
		fetch_unref(fetch);
		return;
	}

	size = g_bytes_get_size(bytes);
	if (size == 0) {
		g_bytes_unref(bytes);
		fetch_body_done(fetch);
	} else if (fetch->body->len + size > fetch->loader->max_image_size) {
		g_bytes_unref(bytes);
		fetch_fail(fetch, PIDGIN_IMAGE_LOADER_ERROR_TOO_LARGE,
		           _("The image is too large"));
	} else {
		g_byte_array_append(fetch->body, g_bytes_get_data(bytes, NULL), size);
		g_bytes_unref(bytes);
		fetch_read_next(fetch);
	}

	fetch_unref(fetch);
}

static void
fetch_read_next(Fetch *fetch)
{
	g_input_stream_read_bytes_async(fetch->stream, READ_CHUNK_SIZE,
	                                G_PRIORITY_DEFAULT, fetch->cancellable,
	                                fetch_read_cb, fetch_ref(fetch));
}

/* Follows a redirect by hand, so that it can only lead to allowed URIs. */
static void
fetch_redirect(Fetch *fetch)
{
	PidginImageLoader *loader = fetch->loader;
	const char *location;
	GUri *next;
	gboolean encrypted = FALSE;

	location = soup_message_headers_get_one(
		soup_message_get_response_headers(fetch->msg), "Location");
	if (location == NULL || fetch->redirects >= MAX_REDIRECTS) {
		fetch_fail(fetch, PIDGIN_IMAGE_LOADER_ERROR_HTTP,
		           _("Too many redirects"));
		return;
	}

	next = g_uri_parse_relative(soup_message_get_uri(fetch->msg), location,
	                            URI_PARSE_FLAGS, NULL);
	if (next != NULL) {
		char *str = g_uri_to_string(next);

		g_uri_unref(next);
		next = parse_allowed(loader, str, &encrypted);
		g_free(str);
	}

	if (next == NULL || encrypted) {
		if (next != NULL)
			g_uri_unref(next);
		fetch_fail(fetch, PIDGIN_IMAGE_LOADER_ERROR_NOT_ALLOWED,
		           _("The image redirects to an address that is not allowed"));
		return;
	}

	fetch->redirects++;
	g_free(fetch->fetch_uri);
	fetch->fetch_uri = make_fetch_uri(loader, next, FALSE);
	g_uri_unref(next);

	g_clear_object(&fetch->msg);
	fetch_start_network(fetch);
}

static void
fetch_send_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	Fetch *fetch = data;
	GInputStream *stream;
	SoupMessageHeaders *headers;
	GError *error = NULL;
	guint status;

	stream = soup_session_send_finish(SOUP_SESSION(source), result, &error);
	if (stream == NULL) {
		fetch_complete(fetch, NULL, error);
		g_error_free(error);
		fetch_unref(fetch);
		return;
	}

	status = soup_message_get_status(fetch->msg);
	headers = soup_message_get_response_headers(fetch->msg);

	if (SOUP_STATUS_IS_REDIRECTION(status) && status != SOUP_STATUS_NOT_MODIFIED) {
		g_object_unref(stream);
		fetch_redirect(fetch);
	} else if (!SOUP_STATUS_IS_SUCCESSFUL(status)) {
		char *message = g_strdup_printf(_("The server returned HTTP %u %s"),
		                                status,
		                                soup_message_get_reason_phrase(fetch->msg));

		g_object_unref(stream);
		fetch_fail(fetch, PIDGIN_IMAGE_LOADER_ERROR_HTTP, message);
		g_free(message);
	} else if (soup_message_headers_get_encoding(headers) == SOUP_ENCODING_CONTENT_LENGTH &&
	           soup_message_headers_get_content_length(headers) >
	           (goffset)fetch->loader->max_image_size) {
		g_object_unref(stream);
		fetch_fail(fetch, PIDGIN_IMAGE_LOADER_ERROR_TOO_LARGE,
		           _("The image is too large"));
	} else {
		fetch->stream = stream;
		fetch->body = g_byte_array_new();
		fetch_read_next(fetch);
	}

	fetch_unref(fetch);
}

static SoupSession *
loader_session(PidginImageLoader *loader)
{
	if (loader->session == NULL) {
		loader->session = soup_session_new_with_options(
			"user-agent", "pidgin4/" VERSION,
			"timeout", 30,
			"idle-timeout", 60,
			"max-conns", 16,
			"max-conns-per-host", 4,
			NULL);
	}

	return loader->session;
}

static void
fetch_start_network(Fetch *fetch)
{
	fetch->msg = soup_message_new(SOUP_METHOD_GET, fetch->fetch_uri);
	if (fetch->msg == NULL) {
		fetch_fail(fetch, PIDGIN_IMAGE_LOADER_ERROR_NOT_ALLOWED,
		           _("The image address is not valid"));
		return;
	}

	soup_message_add_flags(fetch->msg, SOUP_MESSAGE_NO_REDIRECT);
	soup_session_send_async(loader_session(fetch->loader), fetch->msg,
	                        G_PRIORITY_DEFAULT, fetch->cancellable,
	                        fetch_send_cb, fetch_ref(fetch));
}

/* Disk cache lookup in a worker thread; a miss goes to the network. */

static void
disk_lookup_thread(GTask *task, gpointer source, gpointer task_data,
                   GCancellable *cancellable)
{
	g_task_return_pointer(task, read_cache_file(task_data), g_object_unref);
}

static void
disk_lookup_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	Fetch *fetch = data;
	GdkTexture *texture;
	GError *error = NULL;

	texture = g_task_propagate_pointer(G_TASK(result), &error);
	if (error != NULL) {
		fetch_complete(fetch, NULL, error);
		g_error_free(error);
	} else if (texture != NULL) {
		fetch_complete(fetch, texture, NULL);
		g_object_unref(texture);
	} else if (!fetch->done) {
		fetch_start_network(fetch);
	}

	fetch_unref(fetch);
}

static void
fetch_start(Fetch *fetch)
{
	GTask *task;

	if (fetch->cache_path == NULL) {
		fetch_start_network(fetch);
		return;
	}

	task = g_task_new(NULL, fetch->cancellable, disk_lookup_cb,
	                  fetch_ref(fetch));
	g_task_set_source_tag(task, fetch_start);
	g_task_set_task_data(task, g_strdup(fetch->cache_path), g_free);
	g_task_run_in_thread(task, disk_lookup_thread);
	g_object_unref(task);
}

/**************************************************************************
 * GObject
 **************************************************************************/

static void
pidgin_image_loader_finalize(GObject *object)
{
	PidginImageLoader *loader = PIDGIN_IMAGE_LOADER(object);

	if (loader->session != NULL)
		soup_session_abort(loader->session);
	g_clear_object(&loader->session);

	g_hash_table_destroy(loader->fetches);
	g_hash_table_destroy(loader->memory_cache);
	g_queue_clear_full(&loader->memory_lru, (GDestroyNotify)memory_entry_free);
	g_hash_table_destroy(loader->allowed_hosts);
	g_hash_table_destroy(loader->allowed_uris);
	g_main_context_unref(loader->context);
	g_free(loader->cache_dir);

	if (default_loader == loader)
		default_loader = NULL;

	G_OBJECT_CLASS(pidgin_image_loader_parent_class)->finalize(object);
}

static void
pidgin_image_loader_class_init(PidginImageLoaderClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize = pidgin_image_loader_finalize;
}

static void
pidgin_image_loader_init(PidginImageLoader *loader)
{
	gsize i;

	loader->allowed_hosts = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                              g_free, NULL);
	for (i = 0; i < G_N_ELEMENTS(builtin_hosts); i++)
		g_hash_table_add(loader->allowed_hosts, g_strdup(builtin_hosts[i]));
	loader->allowed_uris = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	loader->max_image_size = PIDGIN_IMAGE_LOADER_DEFAULT_MAX_IMAGE_SIZE;
	loader->cache_limit = PIDGIN_IMAGE_LOADER_DEFAULT_CACHE_LIMIT;
	loader->cache_size = CACHE_SIZE_UNKNOWN;
	loader->context = g_main_context_ref_thread_default();

	/* The key is the fetch's own uri; the table holds a reference. */
	loader->fetches = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
	                                        (GDestroyNotify)fetch_unref);
	loader->memory_cache = g_hash_table_new(g_str_hash, g_str_equal);
	g_queue_init(&loader->memory_lru);
}

/**************************************************************************
 * API
 **************************************************************************/

PidginImageLoader *
pidgin_image_loader_new(const char *cache_dir)
{
	PidginImageLoader *loader = g_object_new(PIDGIN_TYPE_IMAGE_LOADER, NULL);

	if (cache_dir != NULL) {
		loader->cache_dir = g_strdup(cache_dir);
		if (g_mkdir_with_parents(cache_dir, 0700) != 0) {
			g_debug("image cache: cannot create %s: %s", cache_dir,
			        g_strerror(errno));
		}
	}

	return loader;
}

PidginImageLoader *
pidgin_image_loader_get_default(void)
{
	if (default_loader == NULL) {
		char *dir = g_build_filename(purple_user_dir(), "pidgin4",
		                             "image-cache", NULL);

		default_loader = pidgin_image_loader_new(dir);
		g_free(dir);
	}

	return default_loader;
}

void
pidgin_image_loader_allow_host(PidginImageLoader *loader, const char *host)
{
	g_return_if_fail(PIDGIN_IS_IMAGE_LOADER(loader));
	g_return_if_fail(host != NULL && *host != '\0');

	g_hash_table_add(loader->allowed_hosts, g_ascii_strdown(host, -1));
}

void
pidgin_image_loader_allow_uri(PidginImageLoader *loader, const char *uri)
{
	g_return_if_fail(PIDGIN_IS_IMAGE_LOADER(loader));
	g_return_if_fail(uri != NULL && *uri != '\0');

	g_hash_table_add(loader->allowed_uris, g_strdup(uri));
}

/**************************************************************************
 * Probing (HEAD)
 **************************************************************************/

typedef struct {
	SoupMessage *msg;
	char *content_type;
	goffset size;
} Probe;

static void
probe_free(Probe *probe)
{
	g_clear_object(&probe->msg);
	g_free(probe->content_type);
	g_free(probe);
}

static void
probe_send_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	GTask *task = data;
	Probe *probe = g_task_get_task_data(task);
	GError *error = NULL;
	GInputStream *stream = soup_session_send_finish(SOUP_SESSION(source), result, &error);
	SoupMessageHeaders *headers;
	guint status;

	if (stream == NULL) {
		g_task_return_error(task, error);
		g_object_unref(task);
		return;
	}
	g_input_stream_close(stream, NULL, NULL);
	g_object_unref(stream);

	status = soup_message_get_status(probe->msg);
	if (!SOUP_STATUS_IS_SUCCESSFUL(status)) {
		g_task_return_new_error(task, PIDGIN_IMAGE_LOADER_ERROR,
		                        PIDGIN_IMAGE_LOADER_ERROR_HTTP,
		                        _("HTTP error %u"), status);
		g_object_unref(task);
		return;
	}
	headers = soup_message_get_response_headers(probe->msg);
	probe->content_type = g_ascii_strdown(
		soup_message_headers_get_content_type(headers, NULL) ?
		soup_message_headers_get_content_type(headers, NULL) : "", -1);
	probe->size = soup_message_headers_get_encoding(headers) == SOUP_ENCODING_CONTENT_LENGTH ?
		soup_message_headers_get_content_length(headers) : -1;
	g_task_return_boolean(task, TRUE);
	g_object_unref(task);
}

void
pidgin_image_loader_probe_async(PidginImageLoader *loader, const char *uri,
                                GCancellable *cancellable,
                                GAsyncReadyCallback callback, gpointer data)
{
	GTask *task;
	GUri *guri;
	Probe *probe;
	const char *scheme;

	g_return_if_fail(PIDGIN_IS_IMAGE_LOADER(loader));

	task = g_task_new(loader, cancellable, callback, data);
	g_task_set_source_tag(task, pidgin_image_loader_probe_async);

	guri = uri ? g_uri_parse(uri, URI_PARSE_FLAGS, NULL) : NULL;
	scheme = guri ? g_uri_get_scheme(guri) : NULL;
	if (guri == NULL || g_uri_get_userinfo(guri) != NULL || g_uri_get_host(guri) == NULL ||
	    !(g_ascii_strcasecmp(scheme, "https") == 0 ||
	      (loader->allow_http && g_ascii_strcasecmp(scheme, "http") == 0))) {
		g_task_return_new_error(task, PIDGIN_IMAGE_LOADER_ERROR,
		                        PIDGIN_IMAGE_LOADER_ERROR_NOT_ALLOWED,
		                        _("Only https addresses are probed"));
		if (guri != NULL)
			g_uri_unref(guri);
		g_object_unref(task);
		return;
	}
	g_uri_unref(guri);

	probe = g_new0(Probe, 1);
	probe->size = -1;
	probe->msg = soup_message_new(SOUP_METHOD_HEAD, uri);
	g_task_set_task_data(task, probe, (GDestroyNotify)probe_free);
	if (probe->msg == NULL) {
		g_task_return_new_error(task, PIDGIN_IMAGE_LOADER_ERROR,
		                        PIDGIN_IMAGE_LOADER_ERROR_NOT_ALLOWED,
		                        _("The address is not valid"));
		g_object_unref(task);
		return;
	}
	soup_session_send_async(loader_session(loader), probe->msg, G_PRIORITY_DEFAULT,
	                        cancellable, probe_send_cb, task);
}

char *
pidgin_image_loader_probe_finish(PidginImageLoader *loader, GAsyncResult *result,
                                 goffset *size, GError **error)
{
	Probe *probe;

	g_return_val_if_fail(PIDGIN_IS_IMAGE_LOADER(loader), NULL);
	g_return_val_if_fail(g_task_is_valid(result, loader), NULL);

	if (!g_task_propagate_boolean(G_TASK(result), error))
		return NULL;
	probe = g_task_get_task_data(G_TASK(result));
	if (size != NULL)
		*size = probe->size;
	return g_strdup(probe->content_type);
}

gboolean
pidgin_image_loader_is_allowed(PidginImageLoader *loader, const char *uri)
{
	GUri *guri;

	g_return_val_if_fail(PIDGIN_IS_IMAGE_LOADER(loader), FALSE);

	guri = parse_allowed(loader, uri, NULL);
	if (guri == NULL)
		return FALSE;

	g_uri_unref(guri);
	return TRUE;
}

void
pidgin_image_loader_set_max_image_size(PidginImageLoader *loader, gsize bytes)
{
	g_return_if_fail(PIDGIN_IS_IMAGE_LOADER(loader));
	g_return_if_fail(bytes > 0);

	loader->max_image_size = bytes;
}

void
pidgin_image_loader_set_cache_limit(PidginImageLoader *loader, guint64 bytes)
{
	g_return_if_fail(PIDGIN_IS_IMAGE_LOADER(loader));

	loader->cache_limit = bytes;
}

void
pidgin_image_loader_set_allow_http_for_tests(PidginImageLoader *loader,
                                             gboolean allow)
{
	g_return_if_fail(PIDGIN_IS_IMAGE_LOADER(loader));

	loader->allow_http = allow;
}

void
pidgin_image_loader_load_async(PidginImageLoader *loader, const char *uri,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback, gpointer data)
{
	GTask *task;
	GUri *guri;
	GdkTexture *texture;
	Fetch *fetch;
	gboolean encrypted = FALSE;

	g_return_if_fail(PIDGIN_IS_IMAGE_LOADER(loader));

	task = g_task_new(loader, cancellable, callback, data);
	g_task_set_source_tag(task, pidgin_image_loader_load_async);

	guri = parse_allowed(loader, uri, &encrypted);
	if (guri == NULL) {
		g_task_return_new_error(task, PIDGIN_IMAGE_LOADER_ERROR,
		                        PIDGIN_IMAGE_LOADER_ERROR_NOT_ALLOWED,
		                        _("Images from this address are not loaded"));
		g_object_unref(task);
		return;
	}

	if (g_task_return_error_if_cancelled(task)) {
		g_uri_unref(guri);
		g_object_unref(task);
		return;
	}

	texture = memory_lookup(loader, uri);
	if (texture != NULL) {
		g_task_return_pointer(task, texture, g_object_unref);
		g_uri_unref(guri);
		g_object_unref(task);
		return;
	}

	/* No point downloading what cannot be decrypted. */
	if (encrypted && (!aesgcm_fragment_valid(g_uri_get_fragment(guri)) ||
	                  omemo_plugin() == NULL)) {
		g_task_return_new_error(task, PIDGIN_IMAGE_LOADER_ERROR,
		                        PIDGIN_IMAGE_LOADER_ERROR_DECRYPT,
		                        _("The encrypted image cannot be decrypted"));
		g_uri_unref(guri);
		g_object_unref(task);
		return;
	}

	fetch = g_hash_table_lookup(loader->fetches, uri);
	if (fetch != NULL) {
		fetch_add_waiter(fetch, task);
	} else {
		fetch = g_new0(Fetch, 1);
		fetch->ref = 1;
		fetch->loader = g_object_ref(loader);
		fetch->uri = g_strdup(uri);
		fetch->fetch_uri = make_fetch_uri(loader, guri, encrypted);
		fetch->encrypted = encrypted;
		fetch->cache_path = cache_path_for(loader, uri, encrypted);
		fetch->cancellable = g_cancellable_new();

		/* The table takes the initial reference. */
		g_hash_table_insert(loader->fetches, fetch->uri, fetch);
		fetch_add_waiter(fetch, task);
		fetch_start(fetch);
	}

	g_uri_unref(guri);
	g_object_unref(task);
}

GdkTexture *
pidgin_image_loader_load_finish(PidginImageLoader *loader,
                                GAsyncResult *result, GError **error)
{
	g_return_val_if_fail(PIDGIN_IS_IMAGE_LOADER(loader), NULL);
	g_return_val_if_fail(g_task_is_valid(result, loader), NULL);
	g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) ==
	                     pidgin_image_loader_load_async, NULL);

	return g_task_propagate_pointer(G_TASK(result), error);
}

GdkTexture *
pidgin_image_loader_lookup_cached(PidginImageLoader *loader, const char *uri)
{
	GdkTexture *texture;
	GUri *guri;
	gboolean encrypted = FALSE;
	char *path;

	g_return_val_if_fail(PIDGIN_IS_IMAGE_LOADER(loader), NULL);

	guri = parse_allowed(loader, uri, &encrypted);
	if (guri == NULL)
		return NULL;
	g_uri_unref(guri);

	texture = memory_lookup(loader, uri);
	if (texture != NULL)
		return texture;

	path = cache_path_for(loader, uri, encrypted);
	if (path == NULL)
		return NULL;

	texture = read_cache_file(path);
	g_free(path);

	if (texture != NULL)
		memory_store(loader, uri, texture);

	return texture;
}

void
pidgin_image_loader_trim_cache(PidginImageLoader *loader)
{
	g_return_if_fail(PIDGIN_IS_IMAGE_LOADER(loader));

	if (loader->cache_dir == NULL)
		return;

	loader->cache_size = trim_directory(loader->cache_dir, loader->cache_limit);
	loader->stores_since_trim = 0;
}

char *
pidgin_image_loader_cache_path(PidginImageLoader *loader, const char *uri)
{
	GUri *guri;
	gboolean encrypted = FALSE;

	g_return_val_if_fail(PIDGIN_IS_IMAGE_LOADER(loader), NULL);
	g_return_val_if_fail(uri != NULL, NULL);

	/* aesgcm:// images never go to disk. */
	guri = g_uri_parse(uri, URI_PARSE_FLAGS, NULL);
	if (guri != NULL) {
		encrypted = g_ascii_strcasecmp(g_uri_get_scheme(guri), "aesgcm") == 0;
		g_uri_unref(guri);
	}

	return cache_path_for(loader, uri, encrypted);
}
