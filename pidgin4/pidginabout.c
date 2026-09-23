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

/* PLACEHOLDER (M5 scaffolding): the M2 GtkAboutDialog, moved here from
 * gtkdialogs.c; replaced by the About window with build info and credits. */
#include "pidgin-internal.h"
#include "pidgin.h"
#include "package_revision.h"

#include "core.h"

#include "gtkutils.h"
#include "pidginabout.h"

static GtkWidget *about_dialog = NULL;

void
pidgin_about_show(void)
{
	char *version, *comments;

	if (about_dialog != NULL) {
		gtk_window_present(GTK_WINDOW(about_dialog));
		return;
	}

	version = g_strdup_printf("%s (%s), libpurple %s", VERSION, REVISION,
	                          purple_core_get_version());
	comments = g_strdup_printf(_("%s is a messaging client based on libpurple "
		"which is capable of connecting to multiple messaging services at "
		"once. This is a personal GTK 4 build of it."), PIDGIN_NAME);

	about_dialog = gtk_about_dialog_new();
	g_object_add_weak_pointer(G_OBJECT(about_dialog), (gpointer *)&about_dialog);
	gtk_window_set_application(GTK_WINDOW(about_dialog), pidgin_application_get());
	gtk_window_set_transient_for(GTK_WINDOW(about_dialog), pidgin_get_active_window());
	g_object_set(about_dialog,
		"program-name", "Pidgin 4",
		"version", version,
		"comments", comments,
		"website", PURPLE_WEBSITE,
		"logo-icon-name", PIDGIN4_APP_ID,
		"license-type", GTK_LICENSE_GPL_2_0,
		"copyright", "Pidgin is the legal property of its developers.",
		NULL);
	g_free(version);
	g_free(comments);

	gtk_window_present(GTK_WINDOW(about_dialog));
}

char *
pidgin_about_get_build_info_html(void)
{
	return g_strdup("");
}

void
pidgin_about_selftest(void)
{
}
