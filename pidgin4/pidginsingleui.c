/*
 * pidgin4: the "one UI per profile" startup check.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "pidgin-internal.h"

#include <limits.h>

#include "pidginsingleui.h"

static gboolean
is_pidgin2_name(const char *path)
{
	char *base;
	gboolean ret;

	if (path == NULL || *path == '\0')
		return FALSE;

	base = g_path_get_basename(path);
	ret = g_str_equal(base, "pidgin");
	g_free(base);
	return ret;
}

char *
pidgin_single_ui_parse_cmdline(const char *cmdline, gsize len,
                               gboolean *is_pidgin)
{
	GPtrArray *argv = g_ptr_array_new();
	const char *p = cmdline, *end = cmdline + len;
	char *dir = NULL;
	guint i;

	while (p < end) {
		gsize n = strnlen(p, end - p);

		g_ptr_array_add(argv, (gpointer)p);
		p += n + 1;
	}

	if (is_pidgin != NULL)
		*is_pidgin = argv->len > 0 && is_pidgin2_name(argv->pdata[0]);

	for (i = 1; i < argv->len; i++) {
		const char *arg = argv->pdata[i];
		const char *next = (i + 1 < argv->len) ? argv->pdata[i + 1] : NULL;

		if (g_str_equal(arg, "--"))
			break;

		if (g_str_has_prefix(arg, "--")) {
			if (g_str_has_prefix(arg, "--config=")) {
				g_free(dir);
				dir = g_strdup(arg + strlen("--config="));
			} else if (g_str_equal(arg, "--config") && next != NULL) {
				g_free(dir);
				dir = g_strdup(next);
				i++;
			} else if (g_str_equal(arg, "--session") || g_str_equal(arg, "--display")) {
				/* options with a separate required argument */
				i++;
			}
			continue;
		}

		if (arg[0] == '-' && arg[1] != '\0') {
			/* A cluster of short options, e.g. "-nmc" DIR or "-c/dir".
			 * Pidgin 2's optstring is "c:dfhmnl::s:v". */
			const char *c;

			for (c = arg + 1; *c; c++) {
				if (*c == 'c' || *c == 's') {
					const char *val = NULL;

					if (c[1] != '\0') {
						val = c + 1;
					} else if (next != NULL) {
						val = next;
						i++;
					}
					if (*c == 'c' && val != NULL) {
						g_free(dir);
						dir = g_strdup(val);
					}
					break;
				}
				if (*c == 'l')   /* optional argument, attached only */
					break;
			}
		}
	}

	g_ptr_array_free(argv, TRUE);
	return dir;
}

/* Returns the value of @name in a NUL-separated environment block. */
static char *
environ_get(const char *env, gsize len, const char *name)
{
	const char *p = env, *end = env + len;
	gsize nlen = strlen(name);

	while (p < end) {
		gsize n = strnlen(p, end - p);

		if (n > nlen && strncmp(p, name, nlen) == 0 && p[nlen] == '=')
			return g_strndup(p + nlen + 1, n - nlen - 1);
		p += n + 1;
	}
	return NULL;
}

static char *
canonical_path(const char *path)
{
	char buf[PATH_MAX];

	if (realpath(path, buf) != NULL)
		return g_strdup(buf);
	return g_canonicalize_filename(path, NULL);
}

/* The profile directory Pidgin 2 process @pid uses, canonicalized. */
static char *
process_profile_dir(const char *pid, const char *dir_arg)
{
	char *path, *dir, *ret;

	if (dir_arg == NULL) {
		char *env = NULL, *home = NULL;
		gsize len = 0;

		path = g_build_filename("/proc", pid, "environ", NULL);
		if (g_file_get_contents(path, &env, &len, NULL))
			home = environ_get(env, len, "HOME");
		g_free(path);
		g_free(env);

		dir = g_build_filename(home ? home : g_get_home_dir(), ".purple", NULL);
		g_free(home);
	} else if (g_path_is_absolute(dir_arg)) {
		dir = g_strdup(dir_arg);
	} else {
		char *cwd;

		path = g_build_filename("/proc", pid, "cwd", NULL);
		cwd = g_file_read_link(path, NULL);
		g_free(path);
		if (cwd == NULL)
			return NULL;
		dir = g_build_filename(cwd, dir_arg, NULL);
		g_free(cwd);
	}

	ret = canonical_path(dir);
	g_free(dir);
	return ret;
}

char *
pidgin_single_ui_find_conflict(const char *profile_dir)
{
	GDir *proc;
	const char *name;
	char *target, *found = NULL;
	char self[32];

	g_return_val_if_fail(profile_dir != NULL, NULL);

	proc = g_dir_open("/proc", 0, NULL);
	if (proc == NULL)
		return NULL;

	target = canonical_path(profile_dir);
	g_snprintf(self, sizeof(self), "%d", (int)getpid());

	while (found == NULL && (name = g_dir_read_name(proc)) != NULL) {
		char *path, *cmdline = NULL, *exe, *dir_arg, *dir;
		gboolean is_pidgin = FALSE;
		gsize len = 0;

		if (!g_ascii_isdigit(name[0]) || g_str_equal(name, self))
			continue;

		path = g_build_filename("/proc", name, "cmdline", NULL);
		if (!g_file_get_contents(path, &cmdline, &len, NULL) || len == 0) {
			g_free(path);
			g_free(cmdline);
			continue;
		}
		g_free(path);

		dir_arg = pidgin_single_ui_parse_cmdline(cmdline, len, &is_pidgin);
		if (!is_pidgin) {
			path = g_build_filename("/proc", name, "exe", NULL);
			exe = g_file_read_link(path, NULL);
			g_free(path);
			is_pidgin = is_pidgin2_name(exe);
			g_free(exe);
		}

		if (is_pidgin) {
			dir = process_profile_dir(name, dir_arg);
			if (dir != NULL && g_str_equal(dir, target)) {
				guint i;

				/* cmdline for the message: NULs to spaces */
				for (i = 0; i + 1 < len; i++)
					if (cmdline[i] == '\0')
						cmdline[i] = ' ';
				found = g_strdup_printf("pid %s: %s", name, cmdline);
			}
			g_free(dir);
		}

		g_free(dir_arg);
		g_free(cmdline);
	}

	g_dir_close(proc);
	g_free(target);
	return found;
}
