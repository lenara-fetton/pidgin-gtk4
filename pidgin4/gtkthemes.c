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
 * The user's CSS, <profile>/pidgin4/gtk4.css. It replaces Pidgin 2's gtkrc
 * files and blist themes: the built-in style.css (gtkmain.c) defines the
 * classes (.pidgin-blist-online, ...), and gtk4.css can override anything
 * at GTK_STYLE_PROVIDER_PRIORITY_USER. The file is watched with a
 * GFileMonitor (on its directory, so creating it later works too) and
 * reloaded when it changes. Parse errors are logged and kept for the
 * Themes page of the preferences.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "debug.h"
#include "util.h"

#include "gtkthemes.h"

static GtkCssProvider *user_provider = NULL;
static GFileMonitor *monitor = NULL;
static char *user_css_path = NULL;
static GString *user_css_error = NULL;
static guint reload_source = 0;

const char *
pidgin_themes_get_user_css_path(void)
{
	if (user_css_path == NULL)
		user_css_path = g_build_filename(pidgin_user_dir(), "gtk4.css", NULL);
	return user_css_path;
}

const char *
pidgin_themes_get_user_css_error(void)
{
	return (user_css_error != NULL && user_css_error->len > 0) ?
		user_css_error->str : NULL;
}

static void
parsing_error_cb(GtkCssProvider *provider, GtkCssSection *section,
                 const GError *error, gpointer data)
{
	char *where = gtk_css_section_to_string(section);

	purple_debug_warning("themes", "%s: %s: %s\n",
	                     pidgin_themes_get_user_css_path(), where, error->message);
	if (user_css_error->len > 0)
		g_string_append_c(user_css_error, '\n');
	g_string_append_printf(user_css_error, "%s: %s", where, error->message);
	g_free(where);
}

void
pidgin_themes_reload_user_css(void)
{
	const char *path = pidgin_themes_get_user_css_path();
	GdkDisplay *display = gdk_display_get_default();

	if (display == NULL)
		return;

	if (user_css_error == NULL)
		user_css_error = g_string_new(NULL);
	g_string_truncate(user_css_error, 0);

	if (user_provider == NULL) {
		user_provider = gtk_css_provider_new();
		g_signal_connect(user_provider, "parsing-error",
		                 G_CALLBACK(parsing_error_cb), NULL);
		gtk_style_context_add_provider_for_display(display,
			GTK_STYLE_PROVIDER(user_provider),
			GTK_STYLE_PROVIDER_PRIORITY_USER);
	}

	if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
		gtk_css_provider_load_from_path(user_provider, path);
		purple_debug_info("themes", "Loaded %s\n", path);
	} else {
		/* Removed or never created: nothing from it applies. */
		gtk_css_provider_load_from_string(user_provider, "");
	}
}

static gboolean
reload_timeout_cb(gpointer data)
{
	reload_source = 0;
	pidgin_themes_reload_user_css();
	return G_SOURCE_REMOVE;
}

static void
monitor_changed_cb(GFileMonitor *mon, GFile *file, GFile *other,
                   GFileMonitorEvent event, gpointer data)
{
	char *name = g_file_get_basename(file);
	char *other_name = other ? g_file_get_basename(other) : NULL;

	/* Editors save through renames; react to anything touching gtk4.css
	 * and coalesce the burst. */
	if (purple_strequal(name, "gtk4.css") ||
	    purple_strequal(other_name, "gtk4.css")) {
		if (reload_source != 0)
			g_source_remove(reload_source);
		reload_source = g_timeout_add(200, reload_timeout_cb, NULL);
	}
	g_free(name);
	g_free(other_name);
}

void
pidgin_themes_init(void)
{
	GFile *dir;
	GError *error = NULL;

	pidgin_themes_reload_user_css();

	dir = g_file_new_for_path(pidgin_user_dir());
	monitor = g_file_monitor_directory(dir, G_FILE_MONITOR_WATCH_MOVES,
	                                   NULL, &error);
	if (monitor != NULL)
		g_signal_connect(monitor, "changed", G_CALLBACK(monitor_changed_cb), NULL);
	else {
		purple_debug_warning("themes", "Cannot watch %s: %s\n",
		                     pidgin_user_dir(), error->message);
		g_error_free(error);
	}
	g_object_unref(dir);
}

void
pidgin_themes_uninit(void)
{
	if (reload_source != 0) {
		g_source_remove(reload_source);
		reload_source = 0;
	}
	if (monitor != NULL) {
		g_file_monitor_cancel(monitor);
		g_clear_object(&monitor);
	}
	if (user_provider != NULL) {
		GdkDisplay *display = gdk_display_get_default();

		if (display != NULL)
			gtk_style_context_remove_provider_for_display(display,
				GTK_STYLE_PROVIDER(user_provider));
		g_clear_object(&user_provider);
	}
	if (user_css_error != NULL) {
		g_string_free(user_css_error, TRUE);
		user_css_error = NULL;
	}
	g_clear_pointer(&user_css_path, g_free);
}
