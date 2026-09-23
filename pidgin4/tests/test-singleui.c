/*
 * Unit tests for pidginsingleui.c: finding a Pidgin 2 that uses the same
 * profile directory from its /proc cmdline.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "pidgin-internal.h"

#include "pidginsingleui.h"

/* Builds a NUL-separated cmdline from a NULL-terminated argv. */
static char *
cmdline(gsize *len, const char *first, ...)
{
	GString *str = g_string_new(NULL);
	const char *arg;
	va_list ap;

	va_start(ap, first);
	for (arg = first; arg != NULL; arg = va_arg(ap, const char *))
		g_string_append_len(str, arg, strlen(arg) + 1);
	va_end(ap);

	*len = str->len;
	return g_string_free(str, FALSE);
}

static void
check(const char *expected_dir, gboolean expected_pidgin, char *cl, gsize len)
{
	gboolean is_pidgin = !expected_pidgin;
	char *dir = pidgin_single_ui_parse_cmdline(cl, len, &is_pidgin);

	g_assert_cmpstr(dir, ==, expected_dir);
	g_assert_cmpint(is_pidgin, ==, expected_pidgin);
	g_free(dir);
	g_free(cl);
}

static void
test_parse(void)
{
	gsize len;
	char *cl;

	cl = cmdline(&len, "/usr/bin/pidgin", NULL);
	check(NULL, TRUE, cl, len);

	cl = cmdline(&len, "pidgin", "-c", "/home/u/.purple-gtk4", NULL);
	check("/home/u/.purple-gtk4", TRUE, cl, len);

	cl = cmdline(&len, "pidgin", "-c/tmp/p", "-n", NULL);
	check("/tmp/p", TRUE, cl, len);

	cl = cmdline(&len, "pidgin", "--config=rel/dir", NULL);
	check("rel/dir", TRUE, cl, len);

	cl = cmdline(&len, "pidgin", "--config", "/x", "-d", NULL);
	check("/x", TRUE, cl, len);

	/* clustered short options ending in c take the next argument */
	cl = cmdline(&len, "/usr/bin/pidgin", "-nmc", "/y", NULL);
	check("/y", TRUE, cl, len);

	/* -l has an optional attached argument: "-lc" is a login name */
	cl = cmdline(&len, "pidgin", "-lc", NULL);
	check(NULL, TRUE, cl, len);

	/* --session takes an argument that must not be mistaken for -c */
	cl = cmdline(&len, "pidgin", "--session", "-c", "-n", NULL);
	check(NULL, TRUE, cl, len);

	/* the last -c wins, like getopt */
	cl = cmdline(&len, "pidgin", "-c", "/a", "-c", "/b", NULL);
	check("/b", TRUE, cl, len);

	/* after -- nothing is an option */
	cl = cmdline(&len, "pidgin", "--", "-c", "/z", NULL);
	check(NULL, TRUE, cl, len);

	/* not Pidgin 2 */
	cl = cmdline(&len, "/home/u/.local/pidgin4/bin/pidgin4", "-c", "/q", NULL);
	check("/q", FALSE, cl, len);
	cl = cmdline(&len, "vim", "pidgin", NULL);
	check(NULL, FALSE, cl, len);

	/* a trailing -c without a value */
	cl = cmdline(&len, "pidgin", "-c", NULL);
	check(NULL, TRUE, cl, len);
}

static void
test_no_conflict(void)
{
	/* No Pidgin 2 uses a fresh temporary directory. */
	char *dir = g_dir_make_tmp("pidgin4-singleui-XXXXXX", NULL);
	char *found;

	g_assert_nonnull(dir);
	found = pidgin_single_ui_find_conflict(dir);
	g_assert_null(found);
	g_rmdir(dir);
	g_free(dir);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/singleui/parse-cmdline", test_parse);
	g_test_add_func("/singleui/no-conflict", test_no_conflict);

	return g_test_run();
}
