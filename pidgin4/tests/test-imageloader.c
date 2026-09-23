/*
 * pidgin4: PidginImageLoader tests.
 *
 * Headless and offline: a SoupServer on 127.0.0.1 serves generated images
 * over plain http, which the loader accepts only in its test mode.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "pidgin-internal.h"

#include <utime.h>

#include <libsoup/soup.h>

#include "pidginimageloader.h"

#include "test-support.h"

/* The smallest GIF: 1x1, two-colour palette. */
static const guint8 tiny_gif[] = {
	0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x01, 0x00, 0x01, 0x00, 0x80, 0x00,
	0x00, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x01, 0x00,
	0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
	0x00, 0x02, 0x02, 0x44, 0x01, 0x00, 0x3b
};

#define BIG_SIZE 10000
#define SMALL_CAP 4096

typedef struct {
	SoupServer *server;
	char *base;          /* http://127.0.0.1:<port> */
	GBytes *png;         /* 7x5 */
	GHashTable *hits;    /* path -> GUINT_TO_POINTER(count) */
	gboolean fail_all;   /* answer everything with 500 */
	const char *cache_dir;
} Fixture;

/**************************************************************************
 * Server
 **************************************************************************/

static GBytes *
make_png(int width, int height)
{
	GdkTexture *texture;
	GBytes *pixels, *png;
	guint8 *data = g_malloc(width * height * 4);
	int i;

	for (i = 0; i < width * height * 4; i++)
		data[i] = (guint8)(i * 37);
	pixels = g_bytes_new_take(data, width * height * 4);
	texture = gdk_memory_texture_new(width, height, GDK_MEMORY_R8G8B8A8,
	                                 pixels, width * 4);
	png = gdk_texture_save_to_png_bytes(texture);
	g_object_unref(texture);
	g_bytes_unref(pixels);

	return png;
}

static guint
hits(Fixture *f, const char *path)
{
	return GPOINTER_TO_UINT(g_hash_table_lookup(f->hits, path));
}

static void
server_cb(SoupServer *server, SoupServerMessage *msg, const char *path,
          GHashTable *query, gpointer data)
{
	Fixture *f = data;
	SoupMessageHeaders *headers = soup_server_message_get_response_headers(msg);

	g_hash_table_insert(f->hits, g_strdup(path),
	                    GUINT_TO_POINTER(hits(f, path) + 1));

	if (f->fail_all) {
		soup_server_message_set_status(msg, 500, NULL);
		return;
	}

	if (g_str_has_prefix(path, "/png")) {
		soup_server_message_set_status(msg, 200, NULL);
		soup_server_message_set_response(msg, "image/png", SOUP_MEMORY_COPY,
		                                 g_bytes_get_data(f->png, NULL),
		                                 g_bytes_get_size(f->png));
	} else if (g_str_equal(path, "/gif")) {
		soup_server_message_set_status(msg, 200, NULL);
		soup_server_message_set_response(msg, "image/gif", SOUP_MEMORY_STATIC,
		                                 (const char *)tiny_gif,
		                                 sizeof(tiny_gif));
	} else if (g_str_equal(path, "/garbage")) {
		soup_server_message_set_status(msg, 200, NULL);
		soup_server_message_set_response(msg, "image/png", SOUP_MEMORY_STATIC,
		                                 "this is not an image", 20);
	} else if (g_str_equal(path, "/big")) {
		char *big = g_malloc0(BIG_SIZE);

		soup_server_message_set_status(msg, 200, NULL);
		soup_server_message_set_response(msg, "image/png", SOUP_MEMORY_TAKE,
		                                 big, BIG_SIZE);
	} else if (g_str_equal(path, "/big-chunked")) {
		SoupMessageBody *body = soup_server_message_get_response_body(msg);
		int i;

		soup_server_message_set_status(msg, 200, NULL);
		soup_message_headers_set_encoding(headers, SOUP_ENCODING_CHUNKED);
		soup_message_headers_set_content_type(headers, "image/png", NULL);
		for (i = 0; i < 10; i++)
			soup_message_body_append_take(body, g_malloc0(BIG_SIZE / 10),
			                              BIG_SIZE / 10);
		soup_message_body_complete(body);
	} else if (g_str_equal(path, "/share/clip")) {
		/* no extension: only the Content-Type says what it is */
		soup_server_message_set_status(msg, 200, NULL);
		soup_server_message_set_response(msg, "video/mp4", SOUP_MEMORY_STATIC,
		                                 "not really a video", 18);
	} else if (g_str_equal(path, "/redirect-ok")) {
		soup_server_message_set_redirect(msg, 302, "/png-redirected");
	} else if (g_str_equal(path, "/redirect-evil")) {
		soup_server_message_set_redirect(msg, 302,
		                                 "https://evil.example.com/x.png");
	} else {
		soup_server_message_set_status(msg, 404, NULL);
	}
}

static void
fixture_setup(Fixture *f, gconstpointer data)
{
	GError *error = NULL;
	GSList *uris;
	const char *profile;

	profile = pidgin_test_profile_setup();
	f->cache_dir = profile;

	f->png = make_png(7, 5);
	f->hits = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	f->server = soup_server_new(NULL, NULL);
	soup_server_add_handler(f->server, NULL, server_cb, f, NULL);
	soup_server_listen_local(f->server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY,
	                         &error);
	g_assert_no_error(error);

	uris = soup_server_get_uris(f->server);
	g_assert_nonnull(uris);
	f->base = g_strdup_printf("http://127.0.0.1:%d",
	                          g_uri_get_port(uris->data));
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
}

static void
fixture_teardown(Fixture *f, gconstpointer data)
{
	soup_server_disconnect(f->server);
	g_object_unref(f->server);
	g_hash_table_destroy(f->hits);
	g_bytes_unref(f->png);
	g_free(f->base);

	/* Let finished sources and threads settle. */
	while (g_main_context_iteration(NULL, FALSE))
		;

	pidgin_test_profile_cleanup();
}

/**************************************************************************
 * Helpers
 **************************************************************************/

static PidginImageLoader *
test_loader(Fixture *f, const char *subdir)
{
	char *dir = subdir ? g_build_filename(f->cache_dir, subdir, NULL) : NULL;
	PidginImageLoader *loader = pidgin_image_loader_new(dir);

	pidgin_image_loader_set_allow_http_for_tests(loader, TRUE);
	pidgin_image_loader_allow_host(loader, "127.0.0.1");
	g_free(dir);

	return loader;
}

static char *
url(Fixture *f, const char *path)
{
	return g_strconcat(f->base, path, NULL);
}

typedef struct {
	gboolean done;
	GdkTexture *texture;
	GError *error;
} LoadResult;

static void
load_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	LoadResult *r = data;

	r->texture = pidgin_image_loader_load_finish(PIDGIN_IMAGE_LOADER(source),
	                                             result, &r->error);
	g_assert_true((r->texture == NULL) != (r->error == NULL));
	r->done = TRUE;
}

static void
wait_for(LoadResult *r)
{
	while (!r->done)
		g_main_context_iteration(NULL, TRUE);
}

static void
load_result_clear(LoadResult *r)
{
	g_clear_object(&r->texture);
	g_clear_error(&r->error);
	r->done = FALSE;
}

/* Loads @path (relative to the server, or a full URI) and waits. */
static void
load(PidginImageLoader *loader, Fixture *f, const char *path, LoadResult *r)
{
	char *uri = strstr(path, "://") ? g_strdup(path) : url(f, path);

	load_result_clear(r);
	pidgin_image_loader_load_async(loader, uri, NULL, load_cb, r);
	wait_for(r);
	g_free(uri);
}

/**************************************************************************
 * Tests
 **************************************************************************/

static void
test_allowlist(void)
{
	PidginImageLoader *loader = pidgin_image_loader_new(NULL);
	static const char *const allowed[] = {
		"https://cdn.discordapp.com/emojis/1234.png",
		"https://CDN.DiscordApp.com/emojis/1234.png?size=48",
		"https://media.discordapp.net/attachments/1/2/a.gif",
		"https://cdn.discordapp.com:443/x.png",
		"aesgcm://cdn.discordapp.com/x.png#00112233",
		"https://images.steamusercontent.com/ugc/123/ABC/",
		"https://steamusercontent-a.akamaihd.net/ugc/1/2/",
		"https://steamcommunity-a.akamaihd.net/economy/emoticon/steamhappy",
		"https://community.cloudflare.steamstatic.com/economy/emoticon/steamhappy",
		"https://community.steamstatic.com/economy/emoticon/steamhappy",
		"https://cdn.cloudflare.steamstatic.com/steamcommunity/public/images/apps/440/x.jpg",
		"https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/440/capsule_184x69.jpg?t=1",
		"https://avatars.steamstatic.com/fef49e7fa7e1997310d705b2a6158ff8dc1cdfeb_full.jpg",
		"https://media.steampowered.com/steamcommunity/public/images/apps/440/x.jpg",
	};
	static const char *const refused[] = {
		"http://cdn.discordapp.com/emojis/1234.png",
		"https://cdn.discordapp.com.evil.com/x.png",
		"https://evil.com/cdn.discordapp.com",
		"https://evil.com/?u=https://cdn.discordapp.com/x.png",
		"evil.com/cdn.discordapp.com",
		"https://cdn.discordapp.com@evil.com/x.png",
		"https://user:pass@cdn.discordapp.com/x.png",
		"https://evil.com#@cdn.discordapp.com/x.png",
		"https://xcdn.discordapp.com/x.png",
		"https://discordapp.com/x.png",
		"https://cdn.discordapp.com./x.png",
		"ftp://cdn.discordapp.com/x.png",
		"file:///etc/passwd",
		"data:image/png;base64,AAAA",
		"https:///x.png",
		"",
		"upload.example.org/x.png",
		"https://upload.example.org/x.png",
		"http://images.steamusercontent.com/ugc/1/2/",
		"https://steamusercontent.com/ugc/1/2/",
		"https://store.steampowered.com/api/appdetails?appids=440",
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(allowed); i++) {
		if (!pidgin_image_loader_is_allowed(loader, allowed[i]))
			g_error("should be allowed: %s", allowed[i]);
	}
	for (i = 0; i < G_N_ELEMENTS(refused); i++) {
		if (pidgin_image_loader_is_allowed(loader, refused[i]))
			g_error("should be refused: %s", refused[i]);
	}
	g_assert_false(pidgin_image_loader_is_allowed(loader, NULL));

	/* Runtime additions, e.g. an XEP-0363 upload host. */
	pidgin_image_loader_allow_host(loader, "Upload.Example.ORG");
	g_assert_true(pidgin_image_loader_is_allowed(loader,
		"https://upload.example.org/x.png"));
	g_assert_true(pidgin_image_loader_is_allowed(loader,
		"https://UPLOAD.example.org/x.png"));
	g_assert_false(pidgin_image_loader_is_allowed(loader,
		"https://a.upload.example.org/x.png"));
	g_assert_false(pidgin_image_loader_is_allowed(loader,
		"http://upload.example.org/x.png"));

	g_object_unref(loader);
}

/* One URI allowed whatever its host (an XMPP file share); the rest of the
 * host stays refused. */
static void
test_allow_uri(void)
{
	PidginImageLoader *loader = pidgin_image_loader_new(NULL);
	const char *share = "https://files.other.example/abc/photo.jpg";
	const char *enc = "aesgcm://files.other.example/abc/p.jpg#00112233";

	g_assert_false(pidgin_image_loader_is_allowed(loader, share));
	pidgin_image_loader_allow_uri(loader, share);
	pidgin_image_loader_allow_uri(loader, enc);
	g_assert_true(pidgin_image_loader_is_allowed(loader, share));
	g_assert_true(pidgin_image_loader_is_allowed(loader, enc));
	g_assert_false(pidgin_image_loader_is_allowed(loader,
		"https://files.other.example/abc/other.jpg"));
	g_assert_false(pidgin_image_loader_is_allowed(loader,
		"https://files.other.example/abc/photo.jpg?x"));
	/* the scheme rules still hold */
	pidgin_image_loader_allow_uri(loader, "http://files.other.example/x.png");
	pidgin_image_loader_allow_uri(loader, "file:///etc/passwd");
	g_assert_false(pidgin_image_loader_is_allowed(loader, "http://files.other.example/x.png"));
	g_assert_false(pidgin_image_loader_is_allowed(loader, "file:///etc/passwd"));
	g_object_unref(loader);
}

typedef struct {
	gboolean done;
	char *type;
	goffset size;
	GError *error;
} ProbeResult;

static void
probe_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	ProbeResult *r = data;

	r->type = pidgin_image_loader_probe_finish(PIDGIN_IMAGE_LOADER(source), result,
	                                           &r->size, &r->error);
	r->done = TRUE;
}

static void
probe(PidginImageLoader *loader, const char *uri, ProbeResult *r)
{
	g_clear_pointer(&r->type, g_free);
	g_clear_error(&r->error);
	r->done = FALSE;
	pidgin_image_loader_probe_async(loader, uri, NULL, probe_cb, r);
	while (!r->done)
		g_main_context_iteration(NULL, TRUE);
}

/* HEAD: the Content-Type and length, any host, no body fetched */
static void
test_probe(Fixture *f, gconstpointer data)
{
	PidginImageLoader *loader = pidgin_image_loader_new(NULL);
	ProbeResult r = { 0 };
	char *uri;

	/* not in test mode: http is not probed */
	uri = url(f, "/png");
	probe(loader, uri, &r);
	g_assert_error(r.error, PIDGIN_IMAGE_LOADER_ERROR, PIDGIN_IMAGE_LOADER_ERROR_NOT_ALLOWED);
	g_assert_cmpuint(g_hash_table_size(f->hits), ==, 0);
	g_free(uri);

	pidgin_image_loader_set_allow_http_for_tests(loader, TRUE);
	uri = url(f, "/png");      /* 127.0.0.1 is not an allowed host */
	probe(loader, uri, &r);
	g_assert_no_error(r.error);
	g_assert_cmpstr(r.type, ==, "image/png");
	g_assert_cmpint(r.size, ==, g_bytes_get_size(f->png));
	g_free(uri);

	uri = url(f, "/share/clip");
	probe(loader, uri, &r);
	g_assert_no_error(r.error);
	g_assert_cmpstr(r.type, ==, "video/mp4");
	g_free(uri);

	uri = url(f, "/missing");
	probe(loader, uri, &r);
	g_assert_error(r.error, PIDGIN_IMAGE_LOADER_ERROR, PIDGIN_IMAGE_LOADER_ERROR_HTTP);
	g_free(uri);

	probe(loader, "ftp://127.0.0.1/x", &r);
	g_assert_error(r.error, PIDGIN_IMAGE_LOADER_ERROR, PIDGIN_IMAGE_LOADER_ERROR_NOT_ALLOWED);

	g_clear_pointer(&r.type, g_free);
	g_clear_error(&r.error);
	g_object_unref(loader);
}

static void
test_not_allowed(Fixture *f, gconstpointer data)
{
	PidginImageLoader *loader = pidgin_image_loader_new(NULL);
	LoadResult r = { 0 };

	/* No test mode: plain http is refused without a request. */
	pidgin_image_loader_allow_host(loader, "127.0.0.1");
	load(loader, f, "/png", &r);
	g_assert_error(r.error, PIDGIN_IMAGE_LOADER_ERROR,
	               PIDGIN_IMAGE_LOADER_ERROR_NOT_ALLOWED);
	load(loader, f, "https://evil.example.com/x.png", &r);
	g_assert_error(r.error, PIDGIN_IMAGE_LOADER_ERROR,
	               PIDGIN_IMAGE_LOADER_ERROR_NOT_ALLOWED);
	g_assert_cmpuint(g_hash_table_size(f->hits), ==, 0);

	load_result_clear(&r);
	g_object_unref(loader);
}

static void
test_load_and_disk_cache(Fixture *f, gconstpointer data)
{
	PidginImageLoader *loader = test_loader(f, "cache");
	LoadResult r = { 0 };
	char *uri = url(f, "/png");
	char *path;
	GStatBuf st;
	GdkTexture *cached;

	g_assert_null(pidgin_image_loader_lookup_cached(loader, uri));

	load(loader, f, "/png", &r);
	g_assert_no_error(r.error);
	g_assert_cmpint(gdk_texture_get_width(r.texture), ==, 7);
	g_assert_cmpint(gdk_texture_get_height(r.texture), ==, 5);
	g_assert_cmpuint(hits(f, "/png"), ==, 1);

	/* Stored, private, named by the SHA-256 of the URI. */
	path = pidgin_image_loader_cache_path(loader, uri);
	g_assert_nonnull(path);
	g_assert_cmpuint(strlen(strrchr(path, '/') + 1), ==, 64);
	g_assert_cmpint(g_stat(path, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);
	g_assert_cmpint(st.st_size, ==, g_bytes_get_size(f->png));

	/* The memory cache answers the next request. */
	load(loader, f, "/png", &r);
	g_assert_no_error(r.error);
	g_assert_cmpuint(hits(f, "/png"), ==, 1);
	cached = pidgin_image_loader_lookup_cached(loader, uri);
	g_assert_nonnull(cached);
	g_object_unref(cached);
	g_object_unref(loader);

	/* A fresh loader finds it on disk, with the server failing. */
	f->fail_all = TRUE;
	loader = test_loader(f, "cache");
	load(loader, f, "/png", &r);
	g_assert_no_error(r.error);
	g_assert_cmpint(gdk_texture_get_width(r.texture), ==, 7);
	g_assert_cmpuint(hits(f, "/png"), ==, 1);
	g_object_unref(loader);

	/* ... and synchronously, too. */
	loader = test_loader(f, "cache");
	cached = pidgin_image_loader_lookup_cached(loader, uri);
	g_assert_nonnull(cached);
	g_assert_cmpint(gdk_texture_get_height(cached), ==, 5);
	g_object_unref(cached);

	/* Without the cache file, the failing server shows. */
	g_object_unref(loader);
	loader = test_loader(f, "cache");
	g_assert_cmpint(g_unlink(path), ==, 0);
	load(loader, f, "/png", &r);
	g_assert_error(r.error, PIDGIN_IMAGE_LOADER_ERROR,
	               PIDGIN_IMAGE_LOADER_ERROR_HTTP);
	g_assert_cmpuint(hits(f, "/png"), ==, 2);

	load_result_clear(&r);
	g_free(path);
	g_free(uri);
	g_object_unref(loader);
}

static void
test_no_disk_cache(Fixture *f, gconstpointer data)
{
	PidginImageLoader *loader = test_loader(f, NULL);
	LoadResult r = { 0 };
	char *uri = url(f, "/png");

	g_assert_null(pidgin_image_loader_cache_path(loader, uri));
	load(loader, f, "/png", &r);
	g_assert_no_error(r.error);

	load_result_clear(&r);
	g_free(uri);
	g_object_unref(loader);
}

static void
test_gif(Fixture *f, gconstpointer data)
{
	PidginImageLoader *loader = test_loader(f, NULL);
	LoadResult r = { 0 };

	load(loader, f, "/gif", &r);
	g_assert_no_error(r.error);
	g_assert_cmpint(gdk_texture_get_width(r.texture), ==, 1);
	g_assert_cmpint(gdk_texture_get_height(r.texture), ==, 1);

	load_result_clear(&r);
	g_object_unref(loader);
}

static void
test_coalescing(Fixture *f, gconstpointer data)
{
	PidginImageLoader *loader = test_loader(f, "cache");
	LoadResult r[5] = { { 0 } };
	char *uri = url(f, "/png");
	int i;

	for (i = 0; i < 5; i++)
		pidgin_image_loader_load_async(loader, uri, NULL, load_cb, &r[i]);
	for (i = 0; i < 5; i++) {
		wait_for(&r[i]);
		g_assert_no_error(r[i].error);
		g_assert_true(r[i].texture == r[0].texture);
	}
	g_assert_cmpuint(hits(f, "/png"), ==, 1);

	for (i = 0; i < 5; i++)
		load_result_clear(&r[i]);
	g_free(uri);
	g_object_unref(loader);
}

static void
test_cancel(Fixture *f, gconstpointer data)
{
	PidginImageLoader *loader = test_loader(f, NULL);
	GCancellable *cancellable = g_cancellable_new();
	LoadResult a = { 0 }, b = { 0 }, c = { 0 };
	char *uri = url(f, "/png");

	/* One of two waiters cancels: the other still gets the image. */
	pidgin_image_loader_load_async(loader, uri, cancellable, load_cb, &a);
	pidgin_image_loader_load_async(loader, uri, NULL, load_cb, &b);
	g_cancellable_cancel(cancellable);
	wait_for(&a);
	wait_for(&b);
	g_assert_error(a.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	g_assert_no_error(b.error);
	g_assert_nonnull(b.texture);
	g_object_unref(cancellable);

	/* The only waiter cancels. */
	cancellable = g_cancellable_new();
	g_free(uri);
	uri = url(f, "/png-other");
	pidgin_image_loader_load_async(loader, uri, cancellable, load_cb, &c);
	g_cancellable_cancel(cancellable);
	wait_for(&c);
	g_assert_error(c.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);

	/* Already cancelled. */
	load_result_clear(&c);
	pidgin_image_loader_load_async(loader, uri, cancellable, load_cb, &c);
	wait_for(&c);
	g_assert_error(c.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);

	/* And a fresh request after the abandoned fetch works. */
	load(loader, f, "/png-other", &c);
	g_assert_no_error(c.error);

	load_result_clear(&a);
	load_result_clear(&b);
	load_result_clear(&c);
	g_object_unref(cancellable);
	g_free(uri);
	g_object_unref(loader);
}

static void
test_errors(Fixture *f, gconstpointer data)
{
	PidginImageLoader *loader = test_loader(f, "cache");
	LoadResult r = { 0 };
	char *uri, *path;

	pidgin_image_loader_set_max_image_size(loader, SMALL_CAP);

	load(loader, f, "/big", &r);
	g_assert_error(r.error, PIDGIN_IMAGE_LOADER_ERROR,
	               PIDGIN_IMAGE_LOADER_ERROR_TOO_LARGE);
	load(loader, f, "/big-chunked", &r);
	g_assert_error(r.error, PIDGIN_IMAGE_LOADER_ERROR,
	               PIDGIN_IMAGE_LOADER_ERROR_TOO_LARGE);
	load(loader, f, "/missing", &r);
	g_assert_error(r.error, PIDGIN_IMAGE_LOADER_ERROR,
	               PIDGIN_IMAGE_LOADER_ERROR_HTTP);

	load(loader, f, "/garbage", &r);
	g_assert_error(r.error, PIDGIN_IMAGE_LOADER_ERROR,
	               PIDGIN_IMAGE_LOADER_ERROR_DECODE);
	/* Undecodable data is not cached. */
	uri = url(f, "/garbage");
	path = pidgin_image_loader_cache_path(loader, uri);
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
	g_free(path);
	g_free(uri);

	/* Redirects are followed only to allowed URIs. */
	load(loader, f, "/redirect-ok", &r);
	g_assert_no_error(r.error);
	g_assert_cmpuint(hits(f, "/png-redirected"), ==, 1);
	load(loader, f, "/redirect-evil", &r);
	g_assert_error(r.error, PIDGIN_IMAGE_LOADER_ERROR,
	               PIDGIN_IMAGE_LOADER_ERROR_NOT_ALLOWED);

	/* Nothing is listening here. */
	soup_server_disconnect(f->server);
	load(loader, f, "/png", &r);
	g_assert_nonnull(r.error);
	g_assert_false(r.error->domain == PIDGIN_IMAGE_LOADER_ERROR);

	load_result_clear(&r);
	g_object_unref(loader);
}

static goffset
cache_total(const char *dir, int *count)
{
	GDir *d = g_dir_open(dir, 0, NULL);
	const char *name;
	goffset total = 0;

	*count = 0;
	g_assert_nonnull(d);
	while ((name = g_dir_read_name(d)) != NULL) {
		char *path = g_build_filename(dir, name, NULL);
		GStatBuf st;

		/* A background trim may delete files under us. */
		if (g_stat(path, &st) == 0) {
			total += st.st_size;
			(*count)++;
		}
		g_free(path);
	}
	g_dir_close(d);

	return total;
}

static void
test_eviction(Fixture *f, gconstpointer data)
{
	PidginImageLoader *loader = test_loader(f, "cache");
	char *dir = g_build_filename(f->cache_dir, "cache", NULL);
	char *paths[5];
	char *stray;
	LoadResult r = { 0 };
	gsize size = g_bytes_get_size(f->png);
	time_t now = time(NULL);
	int i, count;

	for (i = 0; i < 5; i++) {
		char *path = g_strdup_printf("/png?n=%d", i);
		char *uri = url(f, path);
		struct utimbuf times;

		load(loader, f, path, &r);
		g_assert_no_error(r.error);
		paths[i] = pidgin_image_loader_cache_path(loader, uri);
		g_assert_true(g_file_test(paths[i], G_FILE_TEST_IS_REGULAR));

		/* Oldest first: 0 is the least recently used. Out of order on
		 * purpose, so that name or creation order would not do. */
		times.actime = times.modtime = now - 1000 + ((i * 3) % 5) * 100;
		g_assert_cmpint(g_utime(paths[i], &times), ==, 0);

		g_free(uri);
		g_free(path);
	}
	g_assert_cmpuint(hits(f, "/png"), ==, 5);

	/* Files that are not cache entries are left alone. */
	stray = g_build_filename(dir, "README", NULL);
	g_assert_true(g_file_set_contents(stray, "x", 1, NULL));

	/* Room for two. */
	pidgin_image_loader_set_cache_limit(loader, size * 2 + size / 2);
	pidgin_image_loader_trim_cache(loader);

	/* mtimes: 0 -> -1000, 1 -> -700, 2 -> -900, 3 -> -600, 4 -> -800 */
	g_assert_false(g_file_test(paths[0], G_FILE_TEST_EXISTS));
	g_assert_false(g_file_test(paths[2], G_FILE_TEST_EXISTS));
	g_assert_false(g_file_test(paths[4], G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(paths[1], G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(paths[3], G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(stray, G_FILE_TEST_EXISTS));
	g_assert_cmpint(g_unlink(stray), ==, 0);
	g_assert_cmpint(cache_total(dir, &count), <=, size * 2 + size / 2);
	g_assert_cmpint(count, ==, 2);

	/* A disk hit refreshes the mtime, so it survives the next trim. */
	g_object_unref(loader);
	loader = test_loader(f, "cache");
	{
		char *uri = url(f, "/png?n=1");
		GdkTexture *t = pidgin_image_loader_lookup_cached(loader, uri);

		g_assert_nonnull(t);
		g_object_unref(t);
		g_free(uri);
	}
	pidgin_image_loader_set_cache_limit(loader, size + size / 2);
	pidgin_image_loader_trim_cache(loader);
	g_assert_true(g_file_test(paths[1], G_FILE_TEST_EXISTS));
	g_assert_false(g_file_test(paths[3], G_FILE_TEST_EXISTS));

	/* Automatic trimming as images are stored (in a worker thread). */
	pidgin_image_loader_set_cache_limit(loader, size * 3 + size / 2);
	for (i = 10; i < 20; i++) {
		char *path = g_strdup_printf("/png?n=%d", i);

		load(loader, f, path, &r);
		g_assert_no_error(r.error);
		g_free(path);
	}
	/* The last trim may still be running; wait for the total to settle. */
	for (i = 0; i < 200 && cache_total(dir, &count) > size * 3 + size / 2; i++) {
		g_main_context_iteration(NULL, FALSE);
		g_usleep(10000);
	}
	g_assert_cmpint(cache_total(dir, &count), <=, size * 3 + size / 2);

	for (i = 0; i < 5; i++)
		g_free(paths[i]);
	load_result_clear(&r);
	g_free(stray);
	g_free(dir);
	g_object_unref(loader);
}

static void
test_aesgcm_without_omemo(Fixture *f, gconstpointer data)
{
	PidginImageLoader *loader = test_loader(f, "cache");
	LoadResult r = { 0 };
	char *key = g_strnfill(88, 'a');
	char *uri = g_strdup_printf("aesgcm://127.0.0.1:%s/png#%s",
	                            strrchr(f->base, ':') + 1, key);

	g_assert_true(pidgin_image_loader_is_allowed(loader, uri));
	g_assert_null(pidgin_image_loader_cache_path(loader, uri));

	load(loader, f, uri, &r);
	g_assert_error(r.error, PIDGIN_IMAGE_LOADER_ERROR,
	               PIDGIN_IMAGE_LOADER_ERROR_DECRYPT);
	/* Refused before downloading anything. */
	g_assert_cmpuint(hits(f, "/png"), ==, 0);

	load_result_clear(&r);
	g_free(uri);
	g_free(key);
	g_object_unref(loader);
}

static void
test_default(void)
{
	const char *profile = pidgin_test_profile_setup();
	PidginImageLoader *loader = pidgin_image_loader_get_default();
	char *dir = g_build_filename(profile, "pidgin4", "image-cache", NULL);
	char *path;
	GStatBuf st;

	g_assert_true(loader == pidgin_image_loader_get_default());
	g_assert_cmpint(g_stat(dir, &st), ==, 0);
	g_assert_true(S_ISDIR(st.st_mode));
	g_assert_cmpint(st.st_mode & 0777, ==, 0700);

	path = pidgin_image_loader_cache_path(loader,
		"https://cdn.discordapp.com/emojis/1.png");
	g_assert_true(g_str_has_prefix(path, dir));

	g_free(path);
	g_free(dir);
	g_object_unref(loader);   /* test only: drops the singleton */
	pidgin_test_profile_cleanup();
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/imageloader/allowlist", test_allowlist);
	g_test_add_func("/imageloader/default", test_default);
	g_test_add_func("/imageloader/allow-uri", test_allow_uri);
#define ADD(name, func) \
	g_test_add("/imageloader/" name, Fixture, NULL, fixture_setup, func, \
	           fixture_teardown)
	ADD("not-allowed", test_not_allowed);
	ADD("probe", test_probe);
	ADD("load-and-disk-cache", test_load_and_disk_cache);
	ADD("no-disk-cache", test_no_disk_cache);
	ADD("gif", test_gif);
	ADD("coalescing", test_coalescing);
	ADD("cancel", test_cancel);
	ADD("errors", test_errors);
	ADD("eviction", test_eviction);
	ADD("aesgcm-without-omemo", test_aesgcm_without_omemo);
#undef ADD

	return g_test_run();
}
