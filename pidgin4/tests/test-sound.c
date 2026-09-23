/*
 * pidgin4: tests for the sound prefs mapping and the custom command
 * (gtksound.c). Nothing is played.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "eventloop.h"
#include "prefs.h"
#include "util.h"

#include "gtkeventloop.h"
#include "gtksound.h"
#include "test-support.h"

static void
test_command(void)
{
	char *cmd;
	char **argv = NULL;

	/* %s is replaced by the quoted name. */
	cmd = pidgin_sound_build_command("aplay -q %s", "/tmp/a b.wav");
	g_assert_cmpstr(cmd, ==, "aplay -q '/tmp/a b.wav'");
	g_assert_true(g_shell_parse_argv(cmd, NULL, &argv, NULL));
	g_assert_cmpstr(argv[2], ==, "/tmp/a b.wav");
	g_strfreev(argv);
	g_free(cmd);

	/* No %s: appended. */
	cmd = pidgin_sound_build_command("paplay", "/x/receive.wav");
	g_assert_cmpstr(cmd, ==, "paplay '/x/receive.wav'");
	g_free(cmd);

	/* Quotes in the name stay one argument. */
	cmd = pidgin_sound_build_command("play %s --volume 0.5", "/tmp/it's.wav");
	g_assert_true(g_shell_parse_argv(cmd, NULL, &argv, NULL));
	g_assert_cmpuint(g_strv_length(argv), ==, 4);
	g_assert_cmpstr(argv[1], ==, "/tmp/it's.wav");
	g_strfreev(argv);
	g_free(cmd);

	/* Every %s. */
	cmd = pidgin_sound_build_command("sh -c 'x' %s %s", "f");
	g_assert_cmpstr(cmd, ==, "sh -c 'x' 'f' 'f'");
	g_free(cmd);

	g_assert_null(pidgin_sound_build_command("", "f"));
	g_assert_null(pidgin_sound_build_command(NULL, "f"));
}

static void
test_volume(void)
{
	char *db;

	g_assert_null(pidgin_sound_volume_to_db(0));
	g_assert_null(pidgin_sound_volume_to_db(-5));

	db = pidgin_sound_volume_to_db(50);
	g_assert_cmpstr(db, ==, "0.00");
	g_free(db);
	db = pidgin_sound_volume_to_db(100);
	g_assert_cmpstr(db, ==, "6.02");
	g_free(db);
	db = pidgin_sound_volume_to_db(250); /* clamped to 100 */
	g_assert_cmpstr(db, ==, "6.02");
	g_free(db);
	db = pidgin_sound_volume_to_db(25);
	g_assert_cmpstr(db, ==, "-6.02");
	g_free(db);
}

static void
test_events(void)
{
	/* Pidgin 2's option names, in PurpleSoundEventID order. */
	static const char *const options[] = {
		"login", "logout", "im_recv", "first_im_recv", "send_im",
		"join_chat", "left_chat", "send_chat_msg", "chat_msg_recv",
		"pounce_default", "nick_said", "got_attention",
	};
	int i;

	G_STATIC_ASSERT(G_N_ELEMENTS(options) == PURPLE_NUM_SOUNDS);
	for (i = 0; i < PURPLE_NUM_SOUNDS; i++) {
		g_assert_cmpstr(pidgin_sound_get_event_option(i), ==, options[i]);
		g_assert_nonnull(pidgin_sound_get_event_id(i));
	}
	g_assert_cmpstr(pidgin_sound_get_event_id(PURPLE_SOUND_RECEIVE), ==,
	                "message-new-instant");
	g_assert_cmpstr(pidgin_sound_get_event_id(PURPLE_SOUND_SEND), ==,
	                "message-sent-instant");
	g_assert_null(pidgin_sound_get_event_label(PURPLE_SOUND_POUNCE_DEFAULT));
	g_assert_nonnull(pidgin_sound_get_event_label(PURPLE_SOUND_RECEIVE));
	g_assert_null(pidgin_sound_get_event_option(PURPLE_NUM_SOUNDS));
}

static void
test_prefs(void)
{
	char *file, *expected;

	pidgin_test_profile_setup();
	/* Setting a pref schedules a save through the event loop. */
	purple_eventloop_set_ui_ops(pidgin_eventloop_get_ui_ops());
	purple_prefs_init();
	purple_prefs_add_none(PIDGIN4_PREFS_ROOT);
	pidgin_sound_register_prefs();
	pidgin_sound_register_prefs(); /* idempotent */

	/* The shared keys have Pidgin 2's types and defaults. */
	g_assert_cmpint(purple_prefs_get_type("/pidgin/sound/enabled/im_recv"), ==,
	                PURPLE_PREF_BOOLEAN);
	g_assert_true(purple_prefs_get_bool("/pidgin/sound/enabled/im_recv"));
	g_assert_false(purple_prefs_get_bool("/pidgin/sound/enabled/first_im_recv"));
	g_assert_false(purple_prefs_get_bool("/pidgin/sound/enabled/chat_msg_recv"));
	g_assert_true(purple_prefs_get_bool("/pidgin/sound/enabled/pounce_default"));
	g_assert_true(purple_prefs_get_bool("/pidgin/sound/enabled/got_attention"));
	g_assert_true(purple_prefs_get_bool("/pidgin/sound/enabled/sent_attention"));
	g_assert_cmpint(purple_prefs_get_type("/pidgin/sound/file/im_recv"), ==,
	                PURPLE_PREF_PATH);
	g_assert_true(purple_prefs_get_bool("/pidgin/sound/conv_focus"));
	g_assert_false(purple_prefs_get_bool("/pidgin/sound/mute"));
	/* Pidgin 2's method/command/volume are not pidgin4's. */
	g_assert_false(purple_prefs_exists("/pidgin/sound/method"));
	g_assert_false(purple_prefs_exists("/pidgin/sound/volume"));
	g_assert_cmpstr(purple_prefs_get_string("/pidgin4/sound/method"), ==, "gsound");
	g_assert_cmpstr(purple_prefs_get_string("/pidgin4/sound/command"), ==, "");
	g_assert_cmpint(purple_prefs_get_int("/pidgin4/sound/volume"), ==, 50);

	/* Default files: libpurple's share/sounds/purple. */
	file = pidgin_sound_get_event_file(PURPLE_SOUND_RECEIVE);
	expected = g_build_filename(PURPLE_DATADIR, "sounds", "purple",
	                            "receive.wav", NULL);
	g_assert_cmpstr(file, ==, expected);
	g_free(file);
	g_free(expected);
	file = pidgin_sound_get_event_file(PURPLE_SOUND_CHAT_NICK);
	g_assert_true(g_str_has_suffix(file, "/sounds/purple/alert.wav"));
	g_free(file);
	file = pidgin_sound_get_event_file(PURPLE_SOUND_CHAT_JOIN);
	g_assert_true(g_str_has_suffix(file, "/sounds/purple/login.wav"));
	g_free(file);

	/* A custom file wins. */
	purple_prefs_set_path("/pidgin/sound/file/im_recv", "/home/u/ding.oga");
	file = pidgin_sound_get_event_file(PURPLE_SOUND_RECEIVE);
	g_assert_cmpstr(file, ==, "/home/u/ding.oga");
	g_free(file);
	g_assert_true(pidgin_sound_is_customized());
	purple_prefs_set_path("/pidgin/sound/file/im_recv", "");
	g_assert_false(pidgin_sound_is_customized());

	g_assert_null(pidgin_sound_get_event_file(PURPLE_NUM_SOUNDS));

	pidgin_test_profile_cleanup();
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/sound/command", test_command);
	g_test_add_func("/sound/volume", test_volume);
	g_test_add_func("/sound/events", test_events);
	g_test_add_func("/sound/prefs", test_prefs);

	return g_test_run();
}
