/*
 * pidgin4: common includes for the UI plugins in pidgin4/plugins (M7).
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
 * A pidgin4 UI plugin is a PurplePlugin of type PURPLE_PLUGIN_STANDARD
 * with the ui_requirement PIDGIN_PLUGIN_TYPE ("gtk-gaim", as in Pidgin 2).
 * It is a shared module built with -DPIDGIN4_PLUGIN, linked against GTK 4,
 * GLib and libpurple only; every pidgin_* symbol comes from the pidgin4
 * executable (export_dynamic). It installs into <prefix>/lib/pidgin4 (or
 * the user's <profile>/pidgin4/plugins), never into <profile>/plugins,
 * and pidgin4 keeps its list in /pidgin4/plugins/loaded (profile contract,
 * rules 3 and 4). See pidgin4/plugins/README.md.
 */
#ifndef _PIDGIN4_PLUGIN_H_
#define _PIDGIN4_PLUGIN_H_

#ifndef PURPLE_PLUGINS
#	define PURPLE_PLUGINS
#endif

#include "pidgin-internal.h"
#include "pidgin.h"

#include <gtk/gtk.h>

#include "account.h"
#include "blist.h"
#include "connection.h"
#include "conversation.h"
#include "debug.h"
#include "log.h"
#include "notify.h"
#include "plugin.h"
#include "pluginpref.h"
#include "prefs.h"
#include "prpl.h"
#include "request.h"
#include "signals.h"
#include "status.h"
#include "util.h"
#include "version.h"

#include "gtkblist.h"
#include "gtkconv.h"
#include "gtkconvwin.h"
#include "gtkutils.h"
#include "pidgincomposeentry.h"
#include "pidginmessage.h"
#include "pidginmessageview.h"

/*
 * The plugins dialog and the pref frame widgets are M5's (gtkplugin.h,
 * gtkpluginpref.h). Until they are in the tree, PidginPluginUiInfo is
 * defined here with Pidgin 2's layout.
 */
#if defined(__has_include)
#	if __has_include("gtkplugin.h")
#		include "gtkplugin.h"
#		define PIDGIN4_HAVE_GTKPLUGIN_H 1
#	endif
#	if __has_include("gtkpluginpref.h")
#		include "gtkpluginpref.h"
#	endif
#endif

#ifndef PIDGIN4_HAVE_GTKPLUGIN_H
typedef struct _PidginPluginUiInfo PidginPluginUiInfo;

/** A pidgin4 UI plugin's ui_info (Pidgin 2's layout). */
struct _PidginPluginUiInfo
{
	GtkWidget *(*get_config_frame)(PurplePlugin *plugin);

	int page_num;                                         /**< Reserved */

	/* padding */
	void (*_pidgin_reserved1)(void);
	void (*_pidgin_reserved2)(void);
	void (*_pidgin_reserved3)(void);
	void (*_pidgin_reserved4)(void);
};

#define PIDGIN_PLUGIN_UI_INFO(plugin) \
	((PidginPluginUiInfo *)(plugin)->info->ui_info)
#endif

/*
 * M5's API, declared weak: a plugin still loads into a pidgin4 that lacks
 * it (the symbol is then NULL). Check before calling.
 */
GtkWidget *pidgin_plugin_pref_frame_to_widget(PurplePluginPrefFrame *frame) __attribute__((weak));
GtkWidget *pidgin_plugin_get_config_frame(PurplePlugin *plugin) __attribute__((weak));
void pidgin_plugins_save(void) __attribute__((weak));

/**
 * The plugin's configuration in a small window of its own (e.g. from a
 * context menu): the ui_info frame, else the PurplePluginPrefFrame
 * through M5's pidgin_plugin_pref_frame_to_widget(). Returns the window,
 * or NULL if there is nothing to show.
 */
GtkWidget *pidgin4_plugin_show_config(PurplePlugin *plugin);

/** The PidginMessageView of a conversation, or NULL. */
static inline PidginMessageView *
pidgin4_plugin_conv_view(PurpleConversation *conv)
{
	PidginConversation *gtkconv;

	if (conv == NULL || !PIDGIN_IS_PIDGIN_CONVERSATION(conv))
		return NULL;
	gtkconv = PIDGIN_CONVERSATION(conv);
	if (gtkconv == NULL || gtkconv->imhtml == NULL)
		return NULL;
	return PIDGIN_MESSAGE_VIEW(pidgin_conv_get_message_view(gtkconv));
}

#endif /* _PIDGIN4_PLUGIN_H_ */
