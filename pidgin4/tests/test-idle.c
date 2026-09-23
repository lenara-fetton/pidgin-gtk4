/*
 * pidgin4: tests for the idle time arithmetic (pidginidle-wayland.c).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <gtk/gtk.h>

#include "pidginidle-wayland.h"

#define S G_USEC_PER_SEC

static void
test_arithmetic(void)
{
	gint64 now = G_GINT64_CONSTANT(1800000000) * S;

	/* Not idle. */
	g_assert_cmpint(pidgin_idle_time_since(now, 0, 60), ==, 0);

	/* "idled" just now: the last input was 60 s ago. */
	g_assert_cmpint(pidgin_idle_time_since(now, now, 60), ==, 60);

	/* now - (idled_at - timeout) */
	g_assert_cmpint(pidgin_idle_time_since(now, now - 240 * S, 60), ==, 300);
	g_assert_cmpint(pidgin_idle_time_since(now, now - 1 * S, 5), ==, 6);
	/* Fractions of a second are dropped. */
	g_assert_cmpint(pidgin_idle_time_since(now, now - S / 2, 60), ==, 60);

	/* A clock that went backwards never gives a negative idle time. */
	g_assert_cmpint(pidgin_idle_time_since(now, now + 600 * S, 60), ==, 0);
}

static void
test_no_display(void)
{
	/* No GdkDisplay (no gtk_init): nothing to bind, and no idle time. */
	g_assert_false(pidgin_idle_wayland_start(60));
	g_assert_false(pidgin_idle_wayland_is_running());
	g_assert_cmpuint(pidgin_idle_wayland_get_version(), ==, 0);
	g_assert_cmpint(pidgin_idle_wayland_get_time_idle(), ==, 0);
	pidgin_idle_wayland_stop();
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/idle/arithmetic", test_arithmetic);
	g_test_add_func("/idle/no-display", test_no_display);

	return g_test_run();
}
