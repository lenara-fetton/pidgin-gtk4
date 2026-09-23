/*
 * pidgin4: Iconify on Away (M7 port of pidgin/plugins/iconaway.c).
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
 * When an account's status stops being available, the buddy list is
 * hidden and every conversation window minimized (gtk_window_minimize()).
 * Sway has no minimized state and ignores the request (the windows stay
 * where they are); GNOME minimizes them. Despite the name it has nothing
 * to do with buddy icons, and it has no prefs.
 */
#include "pidgin4-plugin.h"

#define ICONAWAY_PLUGIN_ID "gtk-iconaway"

static void
iconify_windows(PurpleAccount *account, PurpleStatus *old, PurpleStatus *newstatus)
{
	PurplePresence *presence;
	GList *windows;

	presence = purple_status_get_presence(newstatus);

	if (purple_presence_is_available(presence))
		return;

	purple_blist_set_visible(FALSE);

	for (windows = pidgin_conv_windows_get_list(); windows != NULL; windows = windows->next) {
		PidginWindow *win = windows->data;

		gtk_window_minimize(GTK_WINDOW(pidgin_conv_window_get_window(win)));
	}
}

static gboolean
plugin_load(PurplePlugin *plugin)
{
	purple_signal_connect(purple_accounts_get_handle(), "account-status-changed",
	                      plugin, PURPLE_CALLBACK(iconify_windows), NULL);

	return TRUE;
}

static PurplePluginInfo info =
{
	PURPLE_PLUGIN_MAGIC,
	PURPLE_MAJOR_VERSION,
	PURPLE_MINOR_VERSION,
	PURPLE_PLUGIN_STANDARD,                           /**< type           */
	PIDGIN_PLUGIN_TYPE,                               /**< ui_requirement */
	0,                                                /**< flags          */
	NULL,                                             /**< dependencies   */
	PURPLE_PRIORITY_DEFAULT,                          /**< priority       */

	ICONAWAY_PLUGIN_ID,                               /**< id             */
	N_("Iconify on Away"),                            /**< name           */
	DISPLAY_VERSION,                                  /**< version        */
	                                                  /**  summary        */
	N_("Iconifies the buddy list and your conversations when you go away."),
	                                                  /**  description    */
	N_("Iconifies the buddy list and your conversations when you go away."),
	"Eric Warmenhoven <eric@warmenhoven.org>",        /**< author         */
	PURPLE_WEBSITE,                                   /**< homepage       */

	plugin_load,                                      /**< load           */
	NULL,                                             /**< unload         */
	NULL,                                             /**< destroy        */

	NULL,                                             /**< ui_info        */
	NULL,                                             /**< extra_info     */
	NULL,
	NULL,

	/* padding */
	NULL,
	NULL,
	NULL,
	NULL
};

static void
init_plugin(PurplePlugin *plugin)
{
}

PURPLE_INIT_PLUGIN(iconaway, init_plugin, info)
