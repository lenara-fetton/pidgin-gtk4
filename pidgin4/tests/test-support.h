/*
 * pidgin4 unit test support: what the components library needs from
 * gtkmain.c, plus a scratch profile directory.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef PIDGIN4_TEST_SUPPORT_H
#define PIDGIN4_TEST_SUPPORT_H

#include <glib.h>

/**
 * Creates a fresh temporary directory, makes it libpurple's user dir
 * (purple_util_set_user_dir()) and returns its path (owned by the
 * support code; removed with pidgin_test_profile_cleanup()).
 */
const char *pidgin_test_profile_setup(void);

/** Recursively removes the scratch profile. */
void pidgin_test_profile_cleanup(void);

/** Removes a directory tree (no symlink following). */
void pidgin_test_rm_rf(const char *path);

/** $PIDGIN4_TEST_DATA/@name (tests/data in the source tree). */
char *pidgin_test_data_path(const char *name);

#endif
