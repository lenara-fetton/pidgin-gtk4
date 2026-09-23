/*
 * pidgin4
 *
 * Pidgin is the legal property of its developers, whose names are too numerous
 * to list here.  Please refer to the COPYRIGHT file distributed with this
 * source distribution.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 */

/*
 * PIDGIN4_WINDOWS_SELFTEST=1: the M5 windows selftest. It runs each
 * module's synchronous selftest in turn, from an idle callback once the
 * buddy list is up, then quits: exit status 0, or 1 if a step failed.
 * With G_DEBUG=fatal-criticals any GTK/GLib/pidgin4 critical aborts it.
 *
 * PIDGIN4_WINDOWS_SELFTEST may also name a comma-separated subset of the
 * modules below (e.g. "prefs,log") instead of "1".
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "debug.h"
#include "util.h"

#include "gtkcertmgr.h"
#include "gtkft.h"
#include "gtklog.h"
#include "gtkplugin.h"
#include "gtkpounce.h"
#include "gtkprefs.h"
#include "gtkprivacy.h"
#include "gtkroomlist.h"
#include "gtksavedstatuses.h"
#include "gtksmiley.h"
#include "pidginabout.h"
#include "pidginomemo.h"
#include "pidginselftest.h"

static gboolean running = FALSE;
static int failures = 0;

static const struct {
	const char *name;
	void (*run)(void);
} modules[] = {
	{ "prefs",    pidgin_prefs_selftest },
	{ "pounces",  pidgin_pounces_selftest },
	{ "status",   pidgin_status_selftest },
	{ "log",      pidgin_log_selftest },
	{ "privacy",  pidgin_privacy_selftest },
	{ "roomlist", pidgin_roomlist_selftest },
	{ "certmgr",  pidgin_certmgr_selftest },
	{ "xfers",    pidgin_xfers_selftest },
	{ "smileys",  pidgin_smileys_selftest },
	{ "plugins",  pidgin_plugins_selftest },
	{ "about",    pidgin_about_selftest },
	{ "omemo",    pidgin_omemo_selftest },
};

gboolean
pidgin_selftest_is_running(void)
{
	return running;
}

void
pidgin_selftest_log(const char *module, const char *format, ...)
{
	va_list args;
	char *msg;

	va_start(args, format);
	msg = g_strdup_vprintf(format, args);
	va_end(args);
	purple_debug_info("selftest", "%s: %s\n", module, msg);
	g_print("selftest: %s: %s\n", module, msg);
	g_free(msg);
}

void
pidgin_selftest_fail(const char *module, const char *format, ...)
{
	va_list args;
	char *msg;

	va_start(args, format);
	msg = g_strdup_vprintf(format, args);
	va_end(args);
	failures++;
	purple_debug_error("selftest", "%s: FAILED: %s\n", module, msg);
	g_printerr("selftest: %s: FAILED: %s\n", module, msg);
	g_free(msg);
}

static gboolean
timeout_cb(gpointer data)
{
	*(gboolean *)data = TRUE;
	return G_SOURCE_REMOVE;
}

void
pidgin_selftest_iterate(guint ms)
{
	gboolean done = FALSE;
	guint id = g_timeout_add(ms, timeout_cb, &done);

	while (!done)
		g_main_context_iteration(NULL, TRUE);
	(void)id;
}

gboolean
pidgin_selftest_wait(gboolean (*cond)(gpointer data), gpointer data,
                     guint timeout_ms)
{
	gboolean timed_out = FALSE;
	guint id = g_timeout_add(timeout_ms, timeout_cb, &timed_out);
	gboolean result;

	while (!(result = cond(data)) && !timed_out)
		g_main_context_iteration(NULL, TRUE);
	if (!timed_out)
		g_source_remove(id);
	return result;
}

static gboolean
module_selected(const char *spec, const char *name)
{
	char **names;
	gboolean found = FALSE;
	int i;

	if (spec == NULL || *spec == '\0' || purple_strequal(spec, "1"))
		return TRUE;
	names = g_strsplit(spec, ",", -1);
	for (i = 0; names[i] != NULL; i++)
		if (purple_strequal(g_strstrip(names[i]), name))
			found = TRUE;
	g_strfreev(names);
	return found;
}

static gboolean
run_cb(gpointer data)
{
	const char *spec = g_getenv("PIDGIN4_WINDOWS_SELFTEST");
	gsize i;
	gint64 start;

	running = TRUE;
	g_print("selftest: start\n");
	for (i = 0; i < G_N_ELEMENTS(modules); i++) {
		int before = failures;

		if (!module_selected(spec, modules[i].name))
			continue;
		start = g_get_monotonic_time();
		pidgin_selftest_log(modules[i].name, "begin");
		modules[i].run();
		/* Let closed windows finish going away. */
		pidgin_selftest_iterate(50);
		pidgin_selftest_log(modules[i].name, "%s (%.1f s)",
			failures == before ? "ok" : "FAILED",
			(g_get_monotonic_time() - start) / 1e6);
	}
	running = FALSE;

	g_print("selftest: %s, %d failure(s)\n",
	        failures == 0 ? "PASSED" : "FAILED", failures);
	if (failures != 0)
		pidgin_application_set_exit_status(1);
	pidgin_application_quit();

	return G_SOURCE_REMOVE;
}

void
pidgin_windows_selftest_start(void)
{
	/* After the startup code has finished (blist shown, plugins loaded). */
	g_timeout_add(500, run_cb, NULL);
}
