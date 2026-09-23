/*
 * pidgin4 unit test support: a private X display for widget tests.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef PIDGIN4_TEST_DISPLAY_H
#define PIDGIN4_TEST_DISPLAY_H

#include <glib.h>

/**
 * Initializes GTK on $PIDGIN4_TEST_DISPLAY, or on a private Xvfb started
 * with -displayfd (stopped at exit); never on the session's display.
 * Returns FALSE if that is impossible: exit(77) to mark the test skipped.
 */
gboolean pidgin_test_init_gtk(void);

/** Dispatches pending main loop events. */
void pidgin_test_iterate(void);

#endif
