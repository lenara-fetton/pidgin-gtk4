# pidgin4: the GTK 4 user interface for libpurple 2.14.
#
# A standalone Meson project. libpurple comes from a private prefix through
# its purple.pc, e.g.
#   PKG_CONFIG_PATH=$HOME/.local/pidgin4/lib/pkgconfig \
#     meson setup build --prefix=$HOME/.local/pidgin4 --libdir=lib
# scripts/build-pidgin4.sh wraps this.

project('pidgin4', 'c',
	version : '2.14.14',
	license : 'GPL-2.0-or-later',
	meson_version : '>= 1.0',
	default_options : [
		'c_std=gnu11',
		'warning_level=2',
		'buildtype=debugoptimized',
	])

app_id = 'com.minowick.Pidgin4'

cc = meson.get_compiler('c')
gnome = import('gnome')
fs = import('fs')

prefix = get_option('prefix')
if prefix == '/usr' or prefix.startswith('/usr/')
	error('pidgin4 must not be installed into /usr; use a private prefix ' +
	      'such as $HOME/.local/pidgin4 (see scripts/build-pidgin4.sh)')
endif

bindir = prefix / get_option('bindir')
libdir = prefix / get_option('libdir')
datadir = prefix / get_option('datadir')
sysconfdir = prefix / get_option('sysconfdir')

glib_dep = dependency('glib-2.0', version : '>= 2.80')
gio_dep = dependency('gio-2.0')
gio_unix_dep = dependency('gio-unix-2.0')
gobject_dep = dependency('gobject-2.0')
gmodule_dep = dependency('gmodule-export-2.0')
gtk_dep = dependency('gtk4', version : '>= 4.14')
purple_dep = dependency('purple', version : ['>= 2.14.0', '< 3.0'])

# Everything libpurple-related (translations, its plugin dir, the rpath)
# comes from the purple.pc prefix, which may differ from our own prefix.
purple_prefix = purple_dep.get_variable(pkgconfig : 'prefix')
purple_libdir = purple_dep.get_variable(pkgconfig : 'libdir')
purple_plugindir = purple_dep.get_variable(pkgconfig : 'plugindir')
purple_datadir = purple_dep.get_variable(pkgconfig : 'datadir')

if purple_prefix == '/usr'
	warning('building against the system libpurple in /usr; the plan builds ' +
	        'against the private prefix (PKG_CONFIG_PATH=<prefix>/lib/pkgconfig)')
endif

conf = configuration_data()
conf.set_quoted('PACKAGE', 'pidgin')          # the shared gettext domain
conf.set_quoted('GETTEXT_PACKAGE', 'pidgin')
conf.set_quoted('VERSION', meson.project_version())
conf.set_quoted('DISPLAY_VERSION', meson.project_version())
conf.set_quoted('PIDGIN4_APP_ID', app_id)
conf.set_quoted('PIDGIN4_RESOURCE_PATH', '/com/minowick/Pidgin4')
conf.set_quoted('LOCALEDIR', purple_datadir / 'locale')
conf.set_quoted('DATADIR', datadir)
conf.set_quoted('LIBDIR', libdir / 'pidgin4')   # pidgin4 UI plugins
conf.set_quoted('PURPLE_LIBDIR', purple_plugindir)
conf.set_quoted('PURPLE_DATADIR', purple_datadir)
conf.set_quoted('SYSCONFDIR', sysconfdir)
conf.set_quoted('BINDIR', bindir)
conf.set('ENABLE_NLS', 1)
conf.set('HAVE_CONFIG_H', 1)
conf.set('SIZEOF_TIME_T', cc.sizeof('time_t', prefix : '#include <time.h>'))
configure_file(output : 'config.h', configuration : conf)

# package_revision.h: git describe of this tree at build time.
revision_h = vcs_tag(
	command : ['git', '-C', meson.project_source_root(), 'describe',
	           '--always', '--dirty', '--tags'],
	fallback : 'unknown',
	input : 'package_revision.h.in',
	output : 'package_revision.h',
	replace_string : '@REVISION@')

resources = gnome.compile_resources('pidgin4-resources',
	'resources/com.minowick.Pidgin4.gresource.xml',
	source_dir : 'resources',
	c_name : 'pidgin4')

add_project_arguments(
	'-DHAVE_CONFIG_H',
	'-DPURPLE_DISABLE_DEPRECATED',
	'-DGDK_VERSION_MIN_REQUIRED=GDK_VERSION_4_14',
	'-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_80',
	'-DG_LOG_DOMAIN="pidgin4"',
	cc.get_supported_arguments([
		'-Wno-unused-parameter',
		'-Wno-missing-field-initializers',
		'-Wno-cast-function-type',
		'-Wno-sign-compare',
		'-Werror=implicit-function-declaration',
		'-Werror=incompatible-pointer-types',
		'-Werror=int-conversion',
		'-Werror=return-type',
	]),
	language : 'c')

pidgin4_sources = files(
	'gtkaccount.c',
	'gtkconn.c',
	'gtkdebug.c',
	'gtkdialogs.c',
	'gtkeventloop.c',
	'gtkidle.c',
	'gtkmain.c',
	'gtknotify.c',
	'gtkprefs.c',
	'gtkrequest.c',
	'gtksound.c',
	'gtkutils.c',
	'pidginmenu.c',
	'pidginsingleui.c',
	'stubs.c',
)

pidgin4_deps = [glib_dep, gio_dep, gio_unix_dep, gobject_dep, gmodule_dep,
                gtk_dep, purple_dep]

executable('pidgin4',
	pidgin4_sources, resources, revision_h,
	dependencies : pidgin4_deps,
	# UI plugins resolve pidgin_* symbols from the executable.
	export_dynamic : true,
	build_rpath : purple_libdir,
	install_rpath : purple_libdir,
	install : true)

# Desktop entry and icons in <prefix>/share. scripts/build-pidgin4.sh
# --desktop-integration also links them into ~/.local/share.
desktop_conf = configuration_data()
desktop_conf.set('BINDIR', bindir)
configure_file(
	input : 'data/' + app_id + '.desktop.in',
	output : app_id + '.desktop',
	configuration : desktop_conf,
	install : true,
	install_dir : datadir / 'applications')

foreach size : ['16x16', '22x22', '24x24', '32x32', '48x48']
	install_data('resources/icons' / size / 'apps' / app_id + '.png',
		install_dir : datadir / 'icons' / 'hicolor' / size / 'apps')
endforeach
install_data('resources/icons/scalable/apps' / app_id + '.svg',
	install_dir : datadir / 'icons' / 'hicolor' / 'scalable' / 'apps')

# Tests
test_menu = executable('test-pidginmenu',
	'tests/test-pidginmenu.c', 'pidginmenu.c',
	dependencies : [glib_dep, gio_dep, gobject_dep, purple_dep],
	build_rpath : purple_libdir,
	install : false)
test('pidginmenu', test_menu)

test_singleui = executable('test-singleui',
	'tests/test-singleui.c', 'pidginsingleui.c',
	dependencies : [glib_dep, purple_dep],
	install : false)
test('singleui', test_singleui)

# M3: the buddy list model (links libpurple and GTK; needs no display).
test_blistmodel = executable('test-blistmodel',
	'tests/test-blistmodel.c', 'pidginblistmodel.c', 'gtkeventloop.c',
	dependencies : [glib_dep, gio_dep, gobject_dep, gtk_dep, purple_dep],
	build_rpath : purple_libdir,
	install : false)
test('blistmodel', test_blistmodel)

desktop_validate = find_program('desktop-file-validate', required : false)
if desktop_validate.found()
	test('desktop-file', desktop_validate,
		args : [meson.current_build_dir() / app_id + '.desktop'])
endif

summary({
	'prefix' : prefix,
	'libpurple prefix' : purple_prefix,
	'libpurple plugin dir' : purple_plugindir,
	'pidgin4 plugin dir' : libdir / 'pidgin4',
	'locale dir' : purple_datadir / 'locale',
})
