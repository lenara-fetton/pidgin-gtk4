/*
 * pidgin4 unit test support.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "util.h"

#include "gtkblist.h"
#include "gtkconvwin.h"
#include "test-support.h"

static char *profile_dir;

/* The components call this (gtkutils.c); tests have no GtkApplication
 * unless they created one themselves. */
GtkApplication *
pidgin_application_get(void)
{
	GApplication *app = g_application_get_default();

	return GTK_IS_APPLICATION(app) ? GTK_APPLICATION(app) : NULL;
}

/* pidgin_window_set_secondary() (gtkutils.c) looks for the buddy list and
 * the conversation windows; tests have neither. */
GtkWidget *
pidgin_blist_get_window(void)
{
	return NULL;
}

GList *
pidgin_conv_windows_get_list(void)
{
	return NULL;
}

GtkWidget *
pidgin_conv_window_get_window(PidginWindow *win)
{
	return NULL;
}

void
pidgin_test_rm_rf(const char *path)
{
	GDir *dir;
	const char *name;

	if (g_file_test(path, G_FILE_TEST_IS_SYMLINK) ||
	    !g_file_test(path, G_FILE_TEST_IS_DIR)) {
		g_unlink(path);
		return;
	}

	dir = g_dir_open(path, 0, NULL);
	if (dir != NULL) {
		while ((name = g_dir_read_name(dir)) != NULL) {
			char *child = g_build_filename(path, name, NULL);
			pidgin_test_rm_rf(child);
			g_free(child);
		}
		g_dir_close(dir);
	}
	g_rmdir(path);
}

const char *
pidgin_test_profile_setup(void)
{
	GError *error = NULL;

	pidgin_test_profile_cleanup();
	profile_dir = g_dir_make_tmp("pidgin4-test-XXXXXX", &error);
	g_assert_no_error(error);
	purple_util_set_user_dir(profile_dir);

	return profile_dir;
}

void
pidgin_test_profile_cleanup(void)
{
	if (profile_dir != NULL) {
		if (g_getenv("PIDGIN4_TEST_KEEP") == NULL)
			pidgin_test_rm_rf(profile_dir);
		g_clear_pointer(&profile_dir, g_free);
	}
}

char *
pidgin_test_data_path(const char *name)
{
	const char *base = g_getenv("PIDGIN4_TEST_DATA");

	if (base == NULL)
		base = "tests/data";
	return g_build_filename(base, name, NULL);
}
