/*
 * pidgin4: PidginAttachment: what a file or a URL is (image, audio,
 * video) by its MIME type or its extension. Headless.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "pidgin-internal.h"

#include "pidginattachment.h"

#include "test-support.h"

static void
test_by_extension(void)
{
	static const struct { const char *name; PidginAttachmentKind kind; } cases[] = {
		{ "photo.png", PIDGIN_ATTACHMENT_IMAGE },
		{ "/home/u/Downloads/IMG_0001.JPG", PIDGIN_ATTACHMENT_IMAGE },
		{ "a.jpeg", PIDGIN_ATTACHMENT_IMAGE },
		{ "anim.webp", PIDGIN_ATTACHMENT_IMAGE },
		{ "voice.ogg", PIDGIN_ATTACHMENT_AUDIO },
		{ "voice.opus", PIDGIN_ATTACHMENT_AUDIO },
		{ "song.MP3", PIDGIN_ATTACHMENT_AUDIO },
		{ "note.m4a", PIDGIN_ATTACHMENT_AUDIO },
		{ "clip.mp4", PIDGIN_ATTACHMENT_VIDEO },
		{ "clip.webm", PIDGIN_ATTACHMENT_VIDEO },
		{ "movie.MKV", PIDGIN_ATTACHMENT_VIDEO },
		{ "clip.mov", PIDGIN_ATTACHMENT_VIDEO },
		{ "notes.txt", PIDGIN_ATTACHMENT_NONE },
		{ "archive.tar.gz", PIDGIN_ATTACHMENT_NONE },
		{ "README", PIDGIN_ATTACHMENT_NONE },
		{ "dir.mp4/file", PIDGIN_ATTACHMENT_NONE },
		{ ".hidden", PIDGIN_ATTACHMENT_NONE },
		{ "trailing.", PIDGIN_ATTACHMENT_NONE },
		/* URLs: the path's extension; query and fragment ignored */
		{ "https://upload.example.org/abc/clip.mp4", PIDGIN_ATTACHMENT_VIDEO },
		{ "https://cdn.discordapp.com/attachments/1/2/voice-message.ogg?ex=1&is=2",
		  PIDGIN_ATTACHMENT_AUDIO },
		{ "aesgcm://upload.example.org/x/photo.jpg#0011223344", PIDGIN_ATTACHMENT_IMAGE },
		{ "https://example.org/watch?v=clip.mp4", PIDGIN_ATTACHMENT_NONE },
		{ "https://example.org/clip.mp4.html", PIDGIN_ATTACHMENT_NONE },
		{ "https://example.org/", PIDGIN_ATTACHMENT_NONE },
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
		if (pidgin_attachment_classify(cases[i].name, NULL) != cases[i].kind)
			g_error("%s: %d, expected %d", cases[i].name,
			        pidgin_attachment_classify(cases[i].name, NULL), cases[i].kind);
	g_assert_cmpint(pidgin_attachment_classify(NULL, NULL), ==, PIDGIN_ATTACHMENT_NONE);
}

static void
test_by_mime(void)
{
	/* the MIME type wins over the name */
	g_assert_cmpint(pidgin_attachment_classify("x", "video/mp4"), ==, PIDGIN_ATTACHMENT_VIDEO);
	g_assert_cmpint(pidgin_attachment_classify("x.png", "audio/ogg"), ==,
	                PIDGIN_ATTACHMENT_AUDIO);
	g_assert_cmpint(pidgin_attachment_classify(NULL, "IMAGE/PNG"), ==, PIDGIN_ATTACHMENT_IMAGE);
	g_assert_cmpint(pidgin_attachment_classify("clip.mp4", "text/html"), ==,
	                PIDGIN_ATTACHMENT_NONE);
	g_assert_cmpint(pidgin_attachment_classify(NULL, "application/pdf"), ==,
	                PIDGIN_ATTACHMENT_NONE);
	/* ... except what says nothing: then the name */
	g_assert_cmpint(pidgin_attachment_classify("clip.mp4", "application/octet-stream"), ==,
	                PIDGIN_ATTACHMENT_VIDEO);
	g_assert_cmpint(pidgin_attachment_classify("clip.mp4", ""), ==, PIDGIN_ATTACHMENT_VIDEO);
}

static void
test_objects(void)
{
	const char *profile = pidgin_test_profile_setup();
	char *path = g_build_filename(profile, "a clip.mp4", NULL);
	PidginAttachment *att;

	g_file_set_contents(path, "12345", 5, NULL);
	att = pidgin_attachment_new_for_file(path, PIDGIN_ATTACHMENT_VIDEO);
	g_assert_cmpstr(pidgin_attachment_get_name(att), ==, "a clip.mp4");
	g_assert_cmpstr(pidgin_attachment_get_path(att), ==, path);
	g_assert_true(g_str_has_prefix(pidgin_attachment_get_uri(att), "file:///"));
	g_assert_cmpint(pidgin_attachment_get_size(att), ==, 5);
	g_object_unref(att);

	att = pidgin_attachment_new_for_uri("https://h.example/d/voice.ogg?x=1",
	                                    PIDGIN_ATTACHMENT_AUDIO, -1);
	g_assert_cmpstr(pidgin_attachment_get_name(att), ==, "voice.ogg");
	g_assert_null(pidgin_attachment_get_path(att));
	g_assert_cmpint(pidgin_attachment_get_size(att), ==, -1);
	g_assert_cmpint(pidgin_attachment_get_kind(att), ==, PIDGIN_ATTACHMENT_AUDIO);
	g_object_unref(att);

	g_free(path);
	pidgin_test_profile_cleanup();
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/attachment/by-extension", test_by_extension);
	g_test_add_func("/attachment/by-mime", test_by_mime);
	g_test_add_func("/attachment/objects", test_objects);

	return g_test_run();
}
