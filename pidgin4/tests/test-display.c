/*
 * pidgin4 unit test support: a private X display for widget tests.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "test-display.h"

static GPid xvfb_pid;

static void
stop_xvfb(void)
{
	if (xvfb_pid != 0) {
		kill(xvfb_pid, SIGTERM);
		waitpid(xvfb_pid, NULL, 0);
		g_spawn_close_pid(xvfb_pid);
		xvfb_pid = 0;
	}
}

/*
 * Widget tests never use the session's display. With PIDGIN4_TEST_DISPLAY
 * set (e.g. an Xvfb ":97") that one is used; otherwise a private Xvfb is
 * started with -displayfd. Then GTK is initialized on it (x11 backend).
 * Returns FALSE (the test should exit 77, "skipped") if neither works.
 */
gboolean
pidgin_test_init_gtk(void)
{
	const char *display = g_getenv("PIDGIN4_TEST_DISPLAY");
	char *xvfb = NULL;

	if (display == NULL || *display == '\0') {
		char *argv[] = { NULL, "-displayfd", NULL, "-nolisten", "tcp",
		                 "-screen", "0", "1024x768x24", NULL };
		int fds[2];
		char fdstr[16], buf[32];
		ssize_t n, got = 0;

		xvfb = g_find_program_in_path("Xvfb");
		if (xvfb == NULL || pipe(fds) != 0) {
			g_free(xvfb);
			return FALSE;
		}
		g_snprintf(fdstr, sizeof(fdstr), "%d", fds[1]);
		argv[0] = xvfb;
		argv[2] = fdstr;
		if (!g_spawn_async_with_fds(NULL, argv, NULL,
		                            G_SPAWN_LEAVE_DESCRIPTORS_OPEN |
		                            G_SPAWN_STDOUT_TO_DEV_NULL |
		                            G_SPAWN_STDERR_TO_DEV_NULL,
		                            NULL, NULL, &xvfb_pid, -1, -1, -1, NULL)) {
			close(fds[0]);
			close(fds[1]);
			g_free(xvfb);
			return FALSE;
		}
		close(fds[1]);
		g_free(xvfb);
		/* Xvfb writes "<num>\n" when it is ready. */
		while (got < (ssize_t)sizeof(buf) - 1 &&
		       (n = read(fds[0], buf + got, sizeof(buf) - 1 - got)) > 0) {
			got += n;
			if (memchr(buf, '\n', got) != NULL)
				break;
		}
		close(fds[0]);
		if (got <= 0) {
			stop_xvfb();
			return FALSE;
		}
		buf[got] = '\0';
		g_strchomp(buf);
		display = g_strdup_printf(":%s", buf);
		atexit(stop_xvfb);
	}

	g_setenv("DISPLAY", display, TRUE);
	g_setenv("GDK_BACKEND", "x11", TRUE);
	g_unsetenv("WAYLAND_DISPLAY");
	g_setenv("GTK_A11Y", "none", TRUE);

	return gtk_init_check();
}

/* Runs pending main loop work. */
void
pidgin_test_iterate(void)
{
	int i;

	for (i = 0; i < 50 && g_main_context_pending(NULL); i++)
		g_main_context_iteration(NULL, FALSE);
}
