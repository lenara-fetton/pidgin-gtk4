# pidgin4 UI plugins (M7). Each is a shared module installed into
# <prefix>/lib/pidgin4 (LIBDIR), which pidgin4 adds to libpurple's plugin
# search path; the loaded ones are kept in /pidgin4/plugins/loaded.
#
# The modules link GTK 4, GLib and libpurple only. Every pidgin_* symbol
# (and those of the libraries pidgin4 itself links) is resolved from the
# executable, which is built with export_dynamic. The other dependencies
# below are used for their headers only.

plugin_link_deps = [glib_dep, gobject_dep, gio_dep, gtk_dep, purple_dep]
plugin_header_deps = [
	gtksourceview_dep.partial_dependency(compile_args : true, includes : true),
	spelling_dep.partial_dependency(compile_args : true, includes : true),
]

plugin_inc = include_directories('.', '..')

# pidgin4_plugin_show_config() and friends, linked into every plugin.
plugin_support = static_library('pidgin4-plugin-support',
	'pidgin4-plugin.c',
	c_args : ['-DPIDGIN4_PLUGIN'],
	include_directories : plugin_inc,
	dependencies : plugin_link_deps + plugin_header_deps,
	pic : true,
	install : false)

# name : [sources, extra link dependencies]
ported_plugins = {
	'history' : [files('history.c'), []],
	'markerline' : [files('markerline.c'), []],
	'timestamp_format' : [files('timestamp_format.c'), []],
}

pidgin4_plugin_dir = libdir / 'pidgin4'
plugin_targets = []

foreach name, spec : ported_plugins
	plugin_targets += shared_module(name,
		spec[0],
		name_prefix : '',
		c_args : ['-DPIDGIN4_PLUGIN'],
		include_directories : plugin_inc,
		link_with : plugin_support,
		dependencies : plugin_link_deps + plugin_header_deps + spec[1],
		install : true,
		install_dir : pidgin4_plugin_dir)
endforeach

# Make sure the directory exists even if every plugin is removed from the
# list above (pidgin4 adds it to the search path either way).
install_emptydir(pidgin4_plugin_dir)
