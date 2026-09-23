/*
 * pidgin4: helpers shared by the UI plugins (M7), linked into each one.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "pidgin4-plugin.h"

static void
config_close_cb(GtkButton *button, GtkWidget *window)
{
	gtk_window_destroy(GTK_WINDOW(window));
}

GtkWidget *
pidgin4_plugin_show_config(PurplePlugin *plugin)
{
	GtkWidget *frame = NULL;
	GtkWidget *window, *content;

	g_return_val_if_fail(plugin != NULL && plugin->info != NULL, NULL);

	if (pidgin_plugin_get_config_frame != NULL) {
		/* M5: knows both kinds of frames */
		frame = pidgin_plugin_get_config_frame(plugin);
	} else if (plugin->info->ui_info != NULL &&
	           purple_strequal(plugin->info->ui_requirement, PIDGIN_PLUGIN_TYPE) &&
	           PIDGIN_PLUGIN_UI_INFO(plugin)->get_config_frame != NULL) {
		frame = PIDGIN_PLUGIN_UI_INFO(plugin)->get_config_frame(plugin);
	} else if (plugin->info->prefs_info != NULL &&
	           plugin->info->prefs_info->get_plugin_pref_frame != NULL &&
	           pidgin_plugin_pref_frame_to_widget != NULL) {
		PurplePluginPrefFrame *pf = plugin->info->prefs_info->get_plugin_pref_frame(plugin);

		frame = pidgin_plugin_pref_frame_to_widget(pf);
		purple_plugin_pref_frame_destroy(pf);
	}

	if (frame == NULL) {
		purple_debug_info("plugins", "%s has no configuration frame here\n",
		                  purple_plugin_get_id(plugin));
		return NULL;
	}

	window = pidgin_dialog_new(_(purple_plugin_get_name(plugin)),
	                           NULL, "plugin_config", TRUE);
	content = pidgin_dialog_get_content_area(window);
	gtk_widget_set_vexpand(frame, TRUE);
	gtk_box_append(GTK_BOX(content), frame);
	pidgin_dialog_add_button(window, _("_Close"), G_CALLBACK(config_close_cb), window);
	pidgin_window_set_secondary(GTK_WINDOW(window));
	gtk_window_present(GTK_WINDOW(window));
	return window;
}
