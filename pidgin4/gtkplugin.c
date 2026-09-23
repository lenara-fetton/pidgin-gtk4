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
 * The plugins dialog and pidgin4's plugin list (the port of
 * pidgin/gtkplugin.c).
 *
 * Profile contract, rule 3: pidgin4's list is /pidgin4/plugins/loaded;
 * /pidgin/plugins/loaded (GTK 2 plugin paths) is never read or written.
 * A plugin whose file links another GUI toolkit (see
 * pidgin_plugin_file_is_foreign_toolkit() in gtkpluginpref.c) is never
 * loaded, never probed by pidgin4 itself, and shown greyed in the dialog.
 *
 * Remaining risk: libpurple's own purple_plugins_probe() at startup
 * dlopens every .so in the search paths (<profile>/pidgin4/plugins,
 * <profile>/plugins, <prefix>/lib/pidgin4, <purple prefix>/lib/purple-2)
 * before pidgin4 can look at them. A GTK 2 plugin copied into one of those
 * directories is therefore mapped into the process (its constructors run),
 * though pidgin4 never calls its load function. Those directories are
 * pidgin4's (or shared prpls only), so that is user error; the debug log
 * warns about it.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "debug.h"
#include "notify.h"
#include "plugin.h"
#include "prefs.h"
#include "request.h"

#include "gtkplugin.h"
#include "gtkpluginpref.h"
#include "gtkutils.h"
#include "pidginrichlabel.h"
#include "pidginselftest.h"

/**************************************************************************
 * Config frames and the plugin list
 **************************************************************************/

static gboolean
plugin_exists(PurplePlugin *plugin)
{
	return plugin != NULL && g_list_find(purple_plugins_get_all(), plugin) != NULL;
}

static gboolean
plugin_has_config(PurplePlugin *plug)
{
	if (plug == NULL || plug->info == NULL || !purple_plugin_is_loaded(plug))
		return FALSE;
	return (PIDGIN_IS_PIDGIN_PLUGIN(plug) &&
	        PIDGIN_PLUGIN_UI_INFO(plug)->get_config_frame != NULL) ||
	       (plug->info->prefs_info != NULL &&
	        plug->info->prefs_info->get_plugin_pref_frame != NULL);
}

GtkWidget *
pidgin_plugin_get_config_frame(PurplePlugin *plugin)
{
	GtkWidget *config = NULL;

	g_return_val_if_fail(plugin != NULL, NULL);
	g_return_val_if_fail(plugin->info != NULL, NULL);

	if (PIDGIN_IS_PIDGIN_PLUGIN(plugin) &&
	    PIDGIN_PLUGIN_UI_INFO(plugin)->get_config_frame != NULL) {
		config = PIDGIN_PLUGIN_UI_INFO(plugin)->get_config_frame(plugin);

		if (plugin->info->prefs_info != NULL &&
		    plugin->info->prefs_info->get_plugin_pref_frame != NULL)
			purple_debug_warning("gtkplugin",
				"Plugin %s contains both, ui_info and prefs_info "
				"preferences; prefs_info will be ignored.\n",
				plugin->info->name);
	}

	if (config == NULL && plugin->info->prefs_info != NULL &&
	    plugin->info->prefs_info->get_plugin_pref_frame != NULL) {
		PurplePluginPrefFrame *frame;

		frame = plugin->info->prefs_info->get_plugin_pref_frame(plugin);
		if (frame == NULL)
			return NULL;
		config = pidgin_plugin_pref_frame_to_widget(frame);
		if (config == NULL) {
			purple_plugin_pref_frame_destroy(frame);
			return NULL;
		}
		/* The frame owns the PurplePluginPrefs the widgets were built
		 * from: keep it as long as the widget. */
		g_object_set_data_full(G_OBJECT(config), "pidgin-plugin-pref-frame",
			frame, (GDestroyNotify)purple_plugin_pref_frame_destroy);
	}

	return config;
}

void
pidgin_plugins_save(void)
{
	purple_plugins_save_loaded(PIDGIN4_PREFS_ROOT "/plugins/loaded");
}

/* Refuses (TRUE) a plugin file that links another toolkit. */
static gboolean
refuse_foreign(const char *path, const char *why)
{
	char *lib = NULL;

	if (path == NULL || !pidgin_plugin_file_is_foreign_toolkit(path, &lib))
		return FALSE;
	purple_debug_warning("plugins",
		"Not %s %s: it links %s (another GUI toolkit), which would crash "
		"pidgin4\n", why, path, lib);
	g_free(lib);
	return TRUE;
}

/* libpurple's (static) purple_plugin_get_basename(): the file name
 * without the directory and, for native modules, the extension. */
static char *
plugin_basename(const char *filename)
{
	const char *basename = strrchr(filename, G_DIR_SEPARATOR);
	const char *last_period;

	basename = basename != NULL ? basename + 1 : filename;
	last_period = strrchr(basename, '.');
	if (last_period != NULL &&
	    (purple_strequal(last_period, ".dll") ||
	     purple_strequal(last_period, ".sl") ||
	     purple_strequal(last_period, ".so")))
		return g_strndup(basename, last_period - basename);
	return g_strdup(basename);
}

void
pidgin_plugins_load_saved(const char *key)
{
	GList *f, *files, *keep = NULL;
	gboolean dropped = FALSE;
	GList *l;

	g_return_if_fail(key != NULL);

	/* libpurple's own probe already dlopen'ed what is in the search
	 * paths; tell about foreign files there (see the top of this file). */
	for (l = purple_plugins_get_all(); l != NULL; l = l->next) {
		PurplePlugin *plug = l->data;

		if (plug->path != NULL &&
		    pidgin_plugin_file_is_foreign_toolkit(plug->path, NULL))
			purple_debug_warning("plugins",
				"%s in a pidgin4 plugin directory links another GUI "
				"toolkit; remove it (it will never be loaded)\n",
				plug->path);
	}

	/* purple_plugins_load_saved() (libpurple/plugin.c), plus the foreign
	 * toolkit check before anything is probed or loaded. */
	files = purple_prefs_get_path_list(key);
	for (f = files; f != NULL; f = f->next) {
		char *filename = f->data;
		char *basename;
		PurplePlugin *plugin;

		if (filename == NULL)
			continue;

		/*
		 * We don't know if the filename uses Windows or Unix path
		 * separators (because people might be sharing a prefs.xml
		 * file across systems), so we find the last occurrence
		 * of either.
		 */
		basename = strrchr(filename, '/');
		if (basename == NULL || basename < strrchr(filename, '\\'))
			basename = strrchr(filename, '\\');
		if (basename != NULL)
			basename++;

		/* Strip the extension */
		if (basename != NULL)
			basename = plugin_basename(basename);

		plugin = purple_plugins_find_with_filename(filename);
		if (plugin == NULL && basename != NULL)
			plugin = purple_plugins_find_with_basename(basename);
		if (plugin == NULL) {
			if (refuse_foreign(filename, "probing")) {
				dropped = TRUE;
				g_free(basename);
				g_free(filename);
				continue;
			}
			plugin = purple_plugin_probe(filename);
		}

		if (plugin != NULL && refuse_foreign(plugin->path, "loading")) {
			dropped = TRUE;
			g_free(basename);
			g_free(filename);
			continue;
		}

		if (plugin != NULL) {
			purple_debug_info("plugins", "Loading saved plugin %s\n",
			                  plugin->path);
			purple_plugin_load(plugin);
		} else {
			purple_debug_error("plugins", "Unable to find saved plugin %s\n",
			                   filename);
		}
		keep = g_list_append(keep, filename);
		g_free(basename);
	}
	g_list_free(files);

	if (dropped)
		purple_prefs_set_path_list(key, keep);
	g_list_free_full(keep, g_free);
}

/* Probes new files in the search paths (Pidgin 2 re-probed when the
 * dialog opened), except those that link another toolkit. */
static void
probe_new_plugins(void)
{
	GList *l;

	if (!g_module_supported())
		return;

	for (l = purple_plugins_get_search_paths(); l != NULL; l = l->next) {
		GDir *dir = g_dir_open(l->data, 0, NULL);
		const char *file;

		if (dir == NULL)
			continue;
		while ((file = g_dir_read_name(dir)) != NULL) {
			char *path;

			if (!g_str_has_suffix(file, "." G_MODULE_SUFFIX))
				continue;
			path = g_build_filename(l->data, file, NULL);
			if (purple_plugins_find_with_filename(path) == NULL &&
			    !refuse_foreign(path, "probing"))
				purple_plugin_probe(path);
			g_free(path);
		}
		g_dir_close(dir);
	}
}

/**************************************************************************
 * List rows
 **************************************************************************/

#define PIDGIN_TYPE_PLUGIN_ROW (pidgin_plugin_row_get_type())
G_DECLARE_FINAL_TYPE(PidginPluginRow, pidgin_plugin_row, PIDGIN, PLUGIN_ROW, GObject)

struct _PidginPluginRow {
	GObject parent;
	PurplePlugin *plugin;   /* checked with plugin_exists() before use */
	char *foreign_lib;      /* the other toolkit's library, or NULL */
};

G_DEFINE_TYPE(PidginPluginRow, pidgin_plugin_row, G_TYPE_OBJECT)

enum { ROW_CHANGED, ROW_N_SIGNALS };
static guint row_signals[ROW_N_SIGNALS];

static void
pidgin_plugin_row_finalize(GObject *obj)
{
	g_free(PIDGIN_PLUGIN_ROW(obj)->foreign_lib);
	G_OBJECT_CLASS(pidgin_plugin_row_parent_class)->finalize(obj);
}

static void
pidgin_plugin_row_class_init(PidginPluginRowClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = pidgin_plugin_row_finalize;
	row_signals[ROW_CHANGED] = g_signal_new("changed",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 0);
}

static void
pidgin_plugin_row_init(PidginPluginRow *row)
{
}

static PidginPluginRow *
pidgin_plugin_row_new(PurplePlugin *plugin)
{
	PidginPluginRow *row = g_object_new(PIDGIN_TYPE_PLUGIN_ROW, NULL);

	row->plugin = plugin;
	if (plugin->path != NULL)
		pidgin_plugin_file_is_foreign_toolkit(plugin->path, &row->foreign_lib);
	return row;
}

/**************************************************************************
 * The dialog
 **************************************************************************/

typedef struct {
	GtkWidget *window;
	GtkWidget *columnview;
	GtkWidget *details;
	GtkWidget *config_button;
	GListStore *store;
	GtkSingleSelection *selection;
} PluginDialog;

static PluginDialog *plugin_dialog = NULL;
/* PurplePlugin * -> its Configure window */
static GHashTable *config_windows = NULL;

static void update_details(void);

static const char *
plugin_display_name(PurplePlugin *plug)
{
	const char *name = purple_plugin_get_name(plug);

	return (name != NULL && *name != '\0') ? name : plug->path;
}

static gboolean
plugin_is_listed(PurplePlugin *plug)
{
	if (plug->info == NULL)
		return FALSE;
	/* Like Pidgin 2: no prpls, loaders or invisible plugins; and no
	 * plugins for another UI. */
	if (plug->info->type != PURPLE_PLUGIN_STANDARD ||
	    (plug->info->flags & PURPLE_PLUGIN_FLAG_INVISIBLE))
		return FALSE;
	if (plug->info->ui_requirement != NULL &&
	    !purple_strequal(plug->info->ui_requirement, PIDGIN_UI))
		return FALSE;
	return TRUE;
}

static int
row_compare(gconstpointer a, gconstpointer b, gpointer data)
{
	PidginPluginRow *ra = (PidginPluginRow *)a, *rb = (PidginPluginRow *)b;
	char *ka = g_utf8_casefold(plugin_display_name(ra->plugin), -1);
	char *kb = g_utf8_casefold(plugin_display_name(rb->plugin), -1);
	int ret = g_utf8_collate(ka, kb);

	g_free(ka);
	g_free(kb);
	return ret;
}

static void
populate_store(void)
{
	GList *l;

	g_list_store_remove_all(plugin_dialog->store);
	for (l = purple_plugins_get_all(); l != NULL; l = l->next) {
		PurplePlugin *plug = l->data;
		PidginPluginRow *row;

		if (!plugin_is_listed(plug))
			continue;
		row = pidgin_plugin_row_new(plug);
		g_list_store_insert_sorted(plugin_dialog->store, row, row_compare, NULL);
		g_object_unref(row);
	}
}

static PidginPluginRow *
find_row(PurplePlugin *plug, guint *pos)
{
	guint i, n;

	if (plugin_dialog == NULL)
		return NULL;
	n = g_list_model_get_n_items(G_LIST_MODEL(plugin_dialog->store));
	for (i = 0; i < n; i++) {
		PidginPluginRow *row = g_list_model_get_item(G_LIST_MODEL(plugin_dialog->store), i);

		if (row->plugin == plug) {
			if (pos != NULL)
				*pos = i;
			return row;     /* a new reference */
		}
		g_object_unref(row);
	}
	return NULL;
}

static void
plugin_changed(PurplePlugin *plug)
{
	PidginPluginRow *row = find_row(plug, NULL);

	if (row != NULL) {
		g_signal_emit(row, row_signals[ROW_CHANGED], 0);
		g_object_unref(row);
	}
	update_details();
}

static PurplePlugin *
selected_plugin(void)
{
	PidginPluginRow *row;

	if (plugin_dialog == NULL)
		return NULL;
	row = gtk_single_selection_get_selected_item(plugin_dialog->selection);
	if (row == NULL || !plugin_exists(row->plugin))
		return NULL;
	return row->plugin;
}

/* Configure windows */

static void
config_window_destroy_cb(GtkWidget *window, gpointer data)
{
	if (config_windows != NULL)
		g_hash_table_remove(config_windows, data);
}

static void
config_close_cb(GtkWidget *button, gpointer window)
{
	gtk_window_destroy(GTK_WINDOW(window));
}

static GtkWidget *
plugin_config_show(PurplePlugin *plug)
{
	GtkWidget *window, *config, *sw;

	if (!plugin_exists(plug) || !plugin_has_config(plug))
		return NULL;

	if (config_windows == NULL)
		config_windows = g_hash_table_new(NULL, NULL);
	if ((window = g_hash_table_lookup(config_windows, plug)) != NULL) {
		gtk_window_present(GTK_WINDOW(window));
		return window;
	}

	config = pidgin_plugin_get_config_frame(plug);
	if (config == NULL)
		return NULL;

	window = pidgin_dialog_new(plugin_display_name(plug),
		plugin_dialog != NULL ? GTK_WINDOW(plugin_dialog->window) : NULL,
		"plugin_config", TRUE);
	sw = pidgin_make_scrollable(config, GTK_POLICY_NEVER,
	                            GTK_POLICY_AUTOMATIC, -1, -1);
	gtk_window_set_default_size(GTK_WINDOW(window), 460, -1);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(sw), TRUE);
	gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(sw), 520);
	gtk_widget_set_vexpand(sw, TRUE);
	gtk_box_append(GTK_BOX(pidgin_dialog_get_content_area(window)), sw);
	pidgin_dialog_add_button(window, _("_Close"), G_CALLBACK(config_close_cb), window);

	g_hash_table_insert(config_windows, plug, window);
	g_signal_connect(window, "destroy", G_CALLBACK(config_window_destroy_cb), plug);
	pidgin_window_set_secondary(GTK_WINDOW(window));
	gtk_window_present(GTK_WINDOW(window));
	return window;
}

static void
plugin_config_close(PurplePlugin *plug)
{
	GtkWidget *window;

	if (config_windows != NULL &&
	    (window = g_hash_table_lookup(config_windows, plug)) != NULL)
		gtk_window_destroy(GTK_WINDOW(window));
}

/* Loading and unloading */

static void
plugin_unload_now(PurplePlugin *plug)
{
	if (!plugin_exists(plug))
		return;

	if (!purple_plugin_unload(plug)) {
		const char *primary = _("Could not unload plugin");
		const char *reload = _("The plugin could not be unloaded now, but will be disabled at the next startup.");

		if (plug->error == NULL) {
			purple_notify_warning(NULL, NULL, primary, reload);
		} else {
			char *tmp = g_strdup_printf("%s\n\n%s", reload, plug->error);
			purple_notify_warning(NULL, NULL, primary, tmp);
			g_free(tmp);
		}
		purple_plugin_disable(plug);
	}

	plugin_changed(plug);
	pidgin_plugins_save();
}

static void
unload_confirm_cb(gpointer data, int action)
{
	PurplePlugin *plug = purple_plugins_find_with_id(data);

	if (plug != NULL)
		plugin_unload_now(plug);
	g_free(data);
}

static void
unload_cancel_cb(gpointer data, int action)
{
	PurplePlugin *plug = purple_plugins_find_with_id(data);

	g_free(data);
	/* Put the check box back. */
	if (plug != NULL)
		plugin_changed(plug);
}

/* What the Enabled check box does. */
static void
plugin_set_enabled(PurplePlugin *plug, gboolean enable)
{
	PidginPluginRow *row;

	if (!plugin_exists(plug) || enable == purple_plugin_is_loaded(plug))
		goto out;

	if (enable) {
		row = find_row(plug, NULL);
		if ((row != NULL && row->foreign_lib != NULL) ||
		    refuse_foreign(plug->path, "loading")) {
			g_clear_object(&row);
			goto out;
		}
		g_clear_object(&row);
		if (purple_plugin_is_unloadable(plug))
			goto out;

		if (!purple_plugin_load(plug)) {
			char *msg = g_strdup_printf("%s\n\n%s", plugin_display_name(plug),
				plug->error ? plug->error : _("Unknown error"));
			purple_notify_error(NULL, NULL, _("Could not load plugin"), msg);
			g_free(msg);
		}
		pidgin_plugins_save();
		goto out;
	}

	plugin_config_close(plug);

	if (plug->dependent_plugins != NULL) {
		GString *tmp = g_string_new(_("The following plugins will be unloaded."));
		GList *l;

		for (l = plug->dependent_plugins; l != NULL; l = l->next) {
			PurplePlugin *dep = purple_plugins_find_with_id(l->data);

			if (dep != NULL)
				g_string_append_printf(tmp, "\n\t%s\n", purple_plugin_get_name(dep));
		}
		purple_request_action(plugin_dialog, NULL,
			_("Multiple plugins will be unloaded."), tmp->str, 0,
			NULL, NULL, NULL,
			g_strdup(purple_plugin_get_id(plug)), 2,
			_("Unload Plugins"), G_CALLBACK(unload_confirm_cb),
			_("Cancel"), G_CALLBACK(unload_cancel_cb));
		g_string_free(tmp, TRUE);
	} else {
		plugin_unload_now(plug);
	}

out:
	if (plugin_exists(plug))
		plugin_changed(plug);
}

/* Cells */

typedef void (*CellUpdateFunc)(GtkWidget *child, PidginPluginRow *row);

static void
cell_row_changed_cb(PidginPluginRow *row, GtkListItem *li)
{
	CellUpdateFunc update = g_object_get_data(G_OBJECT(li), "pidgin-update");
	GtkWidget *child = gtk_list_item_get_child(li);

	if (update != NULL && child != NULL && plugin_exists(row->plugin))
		update(child, row);
}

static void
cell_bind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	PidginPluginRow *row = gtk_list_item_get_item(li);
	gulong id;

	g_object_set_data(G_OBJECT(li), "pidgin-update", data);
	cell_row_changed_cb(row, li);
	id = g_signal_connect(row, "changed", G_CALLBACK(cell_row_changed_cb), li);
	g_object_set_data(G_OBJECT(li), "pidgin-handler", GSIZE_TO_POINTER(id));
}

static void
cell_unbind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	PidginPluginRow *row = gtk_list_item_get_item(li);
	gulong id = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(li), "pidgin-handler"));

	if (row != NULL && id != 0)
		g_signal_handler_disconnect(row, id);
	g_object_set_data(G_OBJECT(li), "pidgin-handler", NULL);
}

static void
add_column(GtkWidget *columnview, const char *title, GCallback setup,
           CellUpdateFunc update, gboolean expand)
{
	GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
	GtkColumnViewColumn *column;

	g_signal_connect(factory, "setup", setup, NULL);
	g_signal_connect(factory, "bind", G_CALLBACK(cell_bind_cb), update);
	g_signal_connect(factory, "unbind", G_CALLBACK(cell_unbind_cb), NULL);

	column = gtk_column_view_column_new(title, factory);
	gtk_column_view_column_set_expand(column, expand);
	gtk_column_view_column_set_resizable(column, TRUE);
	gtk_column_view_append_column(GTK_COLUMN_VIEW(columnview), column);
	g_object_unref(column);
}

static void
enabled_toggled_cb(GtkCheckButton *check, gpointer data)
{
	PurplePlugin *plug = g_object_get_data(G_OBJECT(check), "pidgin-plugin");

	plugin_set_enabled(plug, gtk_check_button_get_active(check));
}

static void
enabled_setup_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	GtkWidget *check = gtk_check_button_new();

	gtk_widget_set_halign(check, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(check, GTK_ALIGN_CENTER);
	gtk_accessible_update_property(GTK_ACCESSIBLE(check),
		GTK_ACCESSIBLE_PROPERTY_LABEL, _("Enabled"), -1);
	g_signal_connect(check, "toggled", G_CALLBACK(enabled_toggled_cb), NULL);
	gtk_list_item_set_child(li, check);
}

static void
enabled_update(GtkWidget *child, PidginPluginRow *row)
{
	PurplePlugin *plug = row->plugin;
	char *tip = NULL;

	g_signal_handlers_block_by_func(child, enabled_toggled_cb, NULL);
	g_object_set_data(G_OBJECT(child), "pidgin-plugin", plug);
	gtk_check_button_set_active(GTK_CHECK_BUTTON(child), purple_plugin_is_loaded(plug));
	g_signal_handlers_unblock_by_func(child, enabled_toggled_cb, NULL);

	if (row->foreign_lib != NULL)
		tip = g_strdup_printf(_("This plugin is built for another toolkit (%s) "
			"and cannot be loaded."), row->foreign_lib);
	gtk_widget_set_sensitive(child, row->foreign_lib == NULL &&
		(purple_plugin_is_loaded(plug) || !purple_plugin_is_unloadable(plug)));
	gtk_widget_set_tooltip_text(child, tip);
	g_free(tip);
}

static void
name_setup_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *name = gtk_label_new(NULL), *summary = gtk_label_new(NULL);

	gtk_label_set_xalign(GTK_LABEL(name), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
	gtk_label_set_xalign(GTK_LABEL(summary), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(summary), PANGO_ELLIPSIZE_END);
	gtk_widget_add_css_class(summary, "dim-label");
	gtk_box_append(GTK_BOX(box), name);
	gtk_box_append(GTK_BOX(box), summary);
	gtk_list_item_set_child(li, box);
}

static void
name_update(GtkWidget *child, PidginPluginRow *row)
{
	PurplePlugin *plug = row->plugin;
	GtkWidget *name = gtk_widget_get_first_child(child);
	GtkWidget *summary = gtk_widget_get_next_sibling(name);
	char *markup;

	markup = g_markup_printf_escaped("<b>%s</b>", plugin_display_name(plug));
	gtk_label_set_markup(GTK_LABEL(name), markup);
	g_free(markup);

	if (row->foreign_lib != NULL)
		markup = g_markup_printf_escaped(
			"<span foreground=\"#c01c28\">%s</span>",
			_("Built for another toolkit; cannot be loaded"));
	else if (plug->error != NULL)
		markup = g_markup_printf_escaped(
			"<span foreground=\"#c01c28\">%s</span>", plug->error);
	else
		markup = g_markup_escape_text(
			purple_plugin_get_summary(plug) ? purple_plugin_get_summary(plug) : "", -1);
	gtk_label_set_markup(GTK_LABEL(summary), markup);
	g_free(markup);

	gtk_widget_set_sensitive(child, row->foreign_lib == NULL);
	gtk_widget_set_tooltip_text(child, purple_plugin_get_summary(plug));
}

static void
label_setup_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	GtkWidget *label = gtk_label_new(NULL);

	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_list_item_set_child(li, label);
}

static void
version_update(GtkWidget *child, PidginPluginRow *row)
{
	const char *version = purple_plugin_get_version(row->plugin);

	gtk_label_set_text(GTK_LABEL(child), version ? version : "");
	gtk_widget_set_sensitive(child, row->foreign_lib == NULL);
}

/* Details */

static void
append_escaped_lines(GString *str, const char *text)
{
	char *escaped = g_markup_escape_text(text, -1);
	char **lines = g_strsplit(escaped, "\n", -1);
	char *joined = g_strjoinv("<br>", lines);

	g_string_append(str, joined);
	g_free(joined);
	g_strfreev(lines);
	g_free(escaped);
}

static void
update_details(void)
{
	PurplePlugin *plug;
	PidginPluginRow *row;
	GString *str;
	char *tmp;
	const char *s;
	guint pos;

	if (plugin_dialog == NULL)
		return;

	plug = selected_plugin();
	gtk_widget_set_sensitive(plugin_dialog->config_button, plugin_has_config(plug));
	if (plug == NULL) {
		pidgin_rich_label_set_text(PIDGIN_RICH_LABEL(plugin_dialog->details), "");
		return;
	}

	str = g_string_new(NULL);
	tmp = g_markup_escape_text(plugin_display_name(plug), -1);
	g_string_append_printf(str, "<font size=\"4\"><b>%s</b></font> ", tmp);
	g_free(tmp);
	if ((s = purple_plugin_get_version(plug)) != NULL) {
		tmp = g_markup_escape_text(s, -1);
		g_string_append_printf(str, "<font size=\"2\">%s</font>", tmp);
		g_free(tmp);
	}
	g_string_append(str, "<br><br>");

	if ((s = purple_plugin_get_description(plug)) != NULL && *s != '\0') {
		append_escaped_lines(str, s);
		g_string_append(str, "<br><br>");
	}
	if ((s = purple_plugin_get_author(plug)) != NULL && *s != '\0') {
		g_string_append_printf(str, "%s ", _("<b>Written by:</b>"));
		append_escaped_lines(str, s);
		g_string_append(str, "<br>");
	}
	if ((s = purple_plugin_get_homepage(plug)) != NULL && *s != '\0') {
		tmp = g_markup_escape_text(s, -1);
		g_string_append_printf(str, "%s <a href=\"%s\">%s</a><br>",
			_("<b>Web site:</b>"), tmp, tmp);
		g_free(tmp);
	}
	if (plug->path != NULL) {
		tmp = g_markup_escape_text(plug->path, -1);
		g_string_append_printf(str, "%s %s<br>", _("<b>Filename:</b>"), tmp);
		g_free(tmp);
	}

	row = find_row(plug, &pos);
	if (row != NULL && row->foreign_lib != NULL) {
		tmp = g_markup_printf_escaped(_("This plugin is built for another toolkit (%s) "
			"and cannot be loaded."), row->foreign_lib);
		g_string_append_printf(str, "<br><font color=\"#c01c28\"><b>%s</b></font>", tmp);
		g_free(tmp);
	} else if (plug->error != NULL) {
		tmp = g_markup_escape_text(plug->error, -1);
		g_string_append_printf(str, "<br><font color=\"#c01c28\"><b>%s</b></font>",
			tmp);
		g_free(tmp);
	}
	g_clear_object(&row);

	pidgin_rich_label_set_html(PIDGIN_RICH_LABEL(plugin_dialog->details), str->str, NULL);
	g_string_free(str, TRUE);
}

static void
selection_changed_cb(GtkSelectionModel *model, guint position, guint n_items,
                     gpointer data)
{
	update_details();
}

static void
configure_clicked_cb(GtkWidget *button, gpointer data)
{
	plugin_config_show(selected_plugin());
}

static void
row_activate_cb(GtkColumnView *view, guint position, gpointer data)
{
	PidginPluginRow *row = g_list_model_get_item(G_LIST_MODEL(plugin_dialog->selection), position);

	if (row != NULL) {
		if (plugin_exists(row->plugin) && plugin_has_config(row->plugin))
			plugin_config_show(row->plugin);
		g_object_unref(row);
	}
}

static void
close_clicked_cb(GtkWidget *button, gpointer data)
{
	gtk_window_destroy(GTK_WINDOW(plugin_dialog->window));
}

static void
plugin_dialog_destroy_cb(GtkWidget *window, gpointer data)
{
	if (plugin_dialog == NULL)
		return;
	purple_request_close_with_handle(plugin_dialog);
	g_clear_object(&plugin_dialog->selection);
	g_clear_object(&plugin_dialog->store);
	g_free(plugin_dialog);
	plugin_dialog = NULL;
}

void
pidgin_plugin_dialog_show(void)
{
	PluginDialog *dialog;
	GtkWidget *win, *content, *sw, *paned, *details_sw;

	if (plugin_dialog != NULL) {
		gtk_window_present(GTK_WINDOW(plugin_dialog->window));
		return;
	}

	probe_new_plugins();

	plugin_dialog = dialog = g_new0(PluginDialog, 1);
	dialog->window = win = pidgin_dialog_new(_("Plugins"), NULL,
	                                         "plugins", TRUE);
	gtk_window_set_default_size(GTK_WINDOW(win), 600, 560);
	g_signal_connect(win, "destroy", G_CALLBACK(plugin_dialog_destroy_cb), NULL);
	content = pidgin_dialog_get_content_area(win);

	dialog->store = g_list_store_new(PIDGIN_TYPE_PLUGIN_ROW);
	dialog->selection = gtk_single_selection_new(
		G_LIST_MODEL(g_object_ref(dialog->store)));
	gtk_single_selection_set_autoselect(dialog->selection, FALSE);
	gtk_single_selection_set_can_unselect(dialog->selection, TRUE);
	g_signal_connect(dialog->selection, "selection-changed",
	                 G_CALLBACK(selection_changed_cb), NULL);

	dialog->columnview = gtk_column_view_new(
		GTK_SELECTION_MODEL(g_object_ref(dialog->selection)));
	gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(dialog->columnview), FALSE);
	g_signal_connect(dialog->columnview, "activate", G_CALLBACK(row_activate_cb), NULL);
	add_column(dialog->columnview, _("Enabled"), G_CALLBACK(enabled_setup_cb),
	           enabled_update, FALSE);
	add_column(dialog->columnview, _("Name"), G_CALLBACK(name_setup_cb),
	           name_update, TRUE);
	add_column(dialog->columnview, _("Version"), G_CALLBACK(label_setup_cb),
	           version_update, FALSE);

	paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
	gtk_widget_set_vexpand(paned, TRUE);
	sw = pidgin_make_scrollable(dialog->columnview, GTK_POLICY_NEVER,
	                            GTK_POLICY_AUTOMATIC, -1, 250);
	gtk_paned_set_start_child(GTK_PANED(paned), sw);
	gtk_paned_set_resize_start_child(GTK_PANED(paned), TRUE);
	gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);

	dialog->details = pidgin_rich_label_new();
	pidgin_rich_label_set_force_text_view(PIDGIN_RICH_LABEL(dialog->details), TRUE);
	gtk_widget_set_margin_top(dialog->details, PIDGIN_HIG_BOX_SPACE);
	details_sw = pidgin_make_scrollable(dialog->details, GTK_POLICY_NEVER,
	                                    GTK_POLICY_AUTOMATIC, -1, 120);
	gtk_paned_set_end_child(GTK_PANED(paned), details_sw);
	gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
	gtk_box_append(GTK_BOX(content), paned);

	dialog->config_button = pidgin_dialog_add_button(win, _("C_onfigure Plugin"),
		G_CALLBACK(configure_clicked_cb), NULL);
	pidgin_dialog_add_button(win, _("_Close"), G_CALLBACK(close_clicked_cb), NULL);

	populate_store();
	update_details();

	pidgin_window_set_secondary(GTK_WINDOW(win));
	gtk_window_present(GTK_WINDOW(win));
}

/**************************************************************************
 * Init
 **************************************************************************/

static void
plugin_load_cb(PurplePlugin *plugin, gpointer data)
{
	plugin_changed(plugin);
}

static void
plugin_unload_cb(PurplePlugin *plugin, gpointer data)
{
	plugin_config_close(plugin);
	plugin_changed(plugin);
}

static int handle;

void
pidgin_plugins_init(void)
{
	purple_signal_connect(purple_plugins_get_handle(), "plugin-load", &handle,
	                      PURPLE_CALLBACK(plugin_load_cb), NULL);
	purple_signal_connect(purple_plugins_get_handle(), "plugin-unload", &handle,
	                      PURPLE_CALLBACK(plugin_unload_cb), NULL);
}

void
pidgin_plugins_uninit(void)
{
	purple_signals_disconnect_by_handle(&handle);
	if (config_windows != NULL) {
		GList *windows = g_hash_table_get_values(config_windows), *l;

		for (l = windows; l != NULL; l = l->next)
			gtk_window_destroy(GTK_WINDOW(l->data));
		g_list_free(windows);
		g_clear_pointer(&config_windows, g_hash_table_destroy);
	}
	if (plugin_dialog != NULL)
		gtk_window_destroy(GTK_WINDOW(plugin_dialog->window));
}

/**************************************************************************
 * Selftest
 **************************************************************************/

#define MODULE "plugins"

static char *
path_list_string(const char *pref)
{
	GList *l, *list;
	GString *str;

	if (!purple_prefs_exists(pref))
		return g_strdup("(missing)");
	list = purple_prefs_get_path_list(pref);
	str = g_string_new(NULL);
	for (l = list; l != NULL; l = l->next)
		g_string_append_printf(str, "%s\n", (char *)l->data);
	g_list_free_full(list, g_free);
	return g_string_free(str, FALSE);
}

/* What purple_plugins_save_loaded() would write, sorted. */
static char *
loaded_list_string(void)
{
	GList *l, *paths = NULL;
	GString *str = g_string_new(NULL);

	for (l = purple_plugins_get_loaded(); l != NULL; l = l->next) {
		PurplePlugin *plug = l->data;

		if (plug->info->type != PURPLE_PLUGIN_PROTOCOL &&
		    plug->info->type != PURPLE_PLUGIN_LOADER && plug->path != NULL)
			paths = g_list_insert_sorted(paths, plug->path, (GCompareFunc)strcmp);
	}
	for (l = paths; l != NULL; l = l->next)
		g_string_append_printf(str, "%s\n", (char *)l->data);
	g_list_free(paths);
	return g_string_free(str, FALSE);
}

static char *
sorted_path_list_string(const char *pref)
{
	GList *l, *list = purple_prefs_get_path_list(pref);
	GString *str = g_string_new(NULL);

	list = g_list_sort(list, (GCompareFunc)strcmp);
	for (l = list; l != NULL; l = l->next)
		g_string_append_printf(str, "%s\n", (char *)l->data);
	g_list_free_full(list, g_free);
	return g_string_free(str, FALSE);
}

static gboolean
path_list_contains(const char *pref, const char *path)
{
	GList *list = purple_prefs_get_path_list(pref), *l;
	gboolean found = FALSE;

	for (l = list; l != NULL; l = l->next)
		if (purple_strequal(l->data, path))
			found = TRUE;
	g_list_free_full(list, g_free);
	return found;
}

static void
selftest_config(PurplePlugin *plug)
{
	GtkWidget *window = plugin_config_show(plug);

	if (window == NULL) {
		pidgin_selftest_fail(MODULE, "no config window for %s", purple_plugin_get_id(plug));
		return;
	}
	pidgin_selftest_iterate(150);
	pidgin_selftest_log(MODULE, "configure window of %s ok", purple_plugin_get_id(plug));
	gtk_window_destroy(GTK_WINDOW(window));
	pidgin_selftest_iterate(50);
	if (config_windows != NULL && g_hash_table_lookup(config_windows, plug) != NULL)
		pidgin_selftest_fail(MODULE, "config window of %s not forgotten",
		                     purple_plugin_get_id(plug));
}

void
pidgin_plugins_selftest(void)
{
	char *gtk2_before, *gtk2_after, *list_before, *list_after;
	GList *saved_list;
	PurplePlugin *psychic;
	PidginPluginRow *row;
	gboolean was_loaded;
	guint pos, n;
	GList *l;
	char *lib = NULL;

	if (pidgin_plugin_file_is_foreign_toolkit("/usr/lib64/pidgin/history.so", &lib)) {
		pidgin_selftest_log(MODULE, "history.so (GTK 2) links %s: refused", lib);
		g_free(lib);
	}

	gtk2_before = path_list_string(PIDGIN_PREFS_ROOT "/plugins/loaded");
	/* Toggling saves the list, which then holds every loaded plugin
	 * (the SSL plugins too); the original is put back at the end. */
	saved_list = purple_prefs_get_path_list(PIDGIN4_PREFS_ROOT "/plugins/loaded");
	list_before = loaded_list_string();

	pidgin_plugin_dialog_show();
	pidgin_selftest_iterate(200);
	if (plugin_dialog == NULL) {
		pidgin_selftest_fail(MODULE, "the dialog did not open");
		goto done;
	}
	n = g_list_model_get_n_items(G_LIST_MODEL(plugin_dialog->store));
	pidgin_selftest_log(MODULE, "%u plugins listed", n);

	/* Select each row once (the details area). */
	for (pos = 0; pos < n; pos++) {
		gtk_single_selection_set_selected(plugin_dialog->selection, pos);
		pidgin_selftest_iterate(10);
	}

	psychic = purple_plugins_find_with_id("core-psychic");
	if (psychic == NULL) {
		pidgin_selftest_fail(MODULE, "core-psychic was not probed");
		goto close;
	}
	row = find_row(psychic, &pos);
	if (row == NULL) {
		pidgin_selftest_fail(MODULE, "core-psychic is not listed");
		goto close;
	}
	g_object_unref(row);
	gtk_single_selection_set_selected(plugin_dialog->selection, pos);

	was_loaded = purple_plugin_is_loaded(psychic);
	if (was_loaded) {
		/* Off, then on again: ends as it was. */
		plugin_set_enabled(psychic, FALSE);
		pidgin_selftest_iterate(50);
		if (purple_plugin_is_loaded(psychic) ||
		    path_list_contains(PIDGIN4_PREFS_ROOT "/plugins/loaded", psychic->path))
			pidgin_selftest_fail(MODULE, "psychic did not unload");
	}

	plugin_set_enabled(psychic, TRUE);
	pidgin_selftest_iterate(50);
	if (!purple_plugin_is_loaded(psychic))
		pidgin_selftest_fail(MODULE, "psychic did not load: %s",
		                     psychic->error ? psychic->error : "?");
	if (!path_list_contains(PIDGIN4_PREFS_ROOT "/plugins/loaded", psychic->path))
		pidgin_selftest_fail(MODULE, "%s not in /pidgin4/plugins/loaded", psychic->path);
	if (!gtk_widget_get_sensitive(plugin_dialog->config_button))
		pidgin_selftest_fail(MODULE, "Configure Plugin is not enabled for psychic");
	selftest_config(psychic);

	/* Every other loaded plugin's configuration. */
	for (l = purple_plugins_get_loaded(); l != NULL; l = l->next) {
		PurplePlugin *plug = l->data;

		if (plug != psychic && plugin_has_config(plug))
			selftest_config(plug);
	}

	if (!was_loaded) {
		plugin_set_enabled(psychic, FALSE);
		pidgin_selftest_iterate(50);
		if (purple_plugin_is_loaded(psychic))
			pidgin_selftest_fail(MODULE, "psychic did not unload");
	}

	list_after = sorted_path_list_string(PIDGIN4_PREFS_ROOT "/plugins/loaded");
	if (!purple_strequal(list_before, list_after))
		pidgin_selftest_fail(MODULE, "/pidgin4/plugins/loaded changed:\n%s---\n%s",
		                     list_before, list_after);
	g_free(list_after);

close:
	gtk_window_destroy(GTK_WINDOW(plugin_dialog->window));
	pidgin_selftest_iterate(50);
	if (plugin_dialog != NULL)
		pidgin_selftest_fail(MODULE, "the dialog did not close");

done:
	gtk2_after = path_list_string(PIDGIN_PREFS_ROOT "/plugins/loaded");
	if (!purple_strequal(gtk2_before, gtk2_after))
		pidgin_selftest_fail(MODULE, "/pidgin/plugins/loaded changed");
	else
		pidgin_selftest_log(MODULE, "/pidgin/plugins/loaded unchanged");
	g_free(gtk2_before);
	g_free(gtk2_after);
	g_free(list_before);

	purple_prefs_set_path_list(PIDGIN4_PREFS_ROOT "/plugins/loaded", saved_list);
	g_list_free_full(saved_list, g_free);
}
