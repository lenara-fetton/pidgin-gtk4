/*
 * pidgin
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
 *
 */

/*
 * pidgin4 startup: a GtkApplication replaces main()/gtk_main() and the
 * bundled getopt, g_unix_signal_add() replaces the socketpair signal hack.
 * The libpurple core-init order is the one from pidgin/gtkmain.c.
 */
#include "pidgin-internal.h"
#include "pidgin.h"
#include "package_revision.h"

#include <glib-unix.h>

#include "account.h"
#include "conversation.h"
#include "core.h"
#include "debug.h"
#include "eventloop.h"
#include "network.h"
#include "notify.h"
#include "plugin.h"
#include "pounce.h"
#include "prefs.h"
#include "prpl.h"
#include "request.h"
#include "savedstatuses.h"
#include "sound.h"
#include "status.h"
#include "util.h"

#include "gtkaccount.h"
#include "gtkblist.h"
#include "gtkconn.h"
#include "gtkdebug.h"
#include "gtkdialogs.h"
#include "gtkdocklet.h"
#include "gtkeventloop.h"
#include "gtkidle.h"
#include "gtknotify.h"
#include "gtkprefs.h"
#include "gtkrequest.h"
#include "gtksound.h"
#include "gtkutils.h"
#include "pidginsingleui.h"
#include "stubs.h"

/* Command line options (parsed by GApplication in the local instance). */
static struct {
	char *config_dir;
	gboolean debug;
	gboolean force_online;
	gboolean login;
	char *login_arg;
	gboolean multiple;
	gboolean nologin;
	gboolean version;
} opts;

static GtkApplication *application = NULL;
static gboolean core_running = FALSE;
static gboolean app_held = FALSE;
static int exit_status = 0;
static char *pidgin4_dir = NULL;
static GHashTable *ui_info = NULL;
static guint signal_sources[3];

GtkApplication *
pidgin_application_get(void)
{
	return application;
}

const char *
pidgin_user_dir(void)
{
	if (pidgin4_dir == NULL) {
		pidgin4_dir = g_build_filename(purple_user_dir(),
		                               PIDGIN4_PROFILE_SUBDIR, NULL);
		if (g_mkdir_with_parents(pidgin4_dir, S_IRWXU) != 0)
			purple_debug_error("main", "Could not create %s: %s\n",
			                   pidgin4_dir, g_strerror(errno));
	}
	return pidgin4_dir;
}

/**************************************************************************
 * libpurple core UI ops
 **************************************************************************/

static void
dologin_named(const char *name)
{
	PurpleAccount *account;
	char **names;
	int i;

	if (name != NULL) { /* list of names given */
		names = g_strsplit(name, ",", 64);
		for (i = 0; names[i] != NULL; i++) {
			account = purple_accounts_find(names[i], NULL);
			if (account != NULL) { /* found a user */
				purple_account_set_enabled(account, PIDGIN_UI, TRUE);
			}
		}
		g_strfreev(names);
	} else { /* no name given, use the first account */
		GList *accounts;

		accounts = purple_accounts_get_all();
		if (accounts != NULL)
		{
			account = (PurpleAccount *)accounts->data;
			purple_account_set_enabled(account, PIDGIN_UI, TRUE);
		}
	}
}

static void
debug_init(void)
{
	purple_debug_set_ui_ops(pidgin_debug_get_ui_ops());
	pidgin_debug_init();
}

static void
pidgin_ui_init(void)
{
	/*
	 * Set the UI operation structures.
	 *
	 * Not set in M2, so libpurple runs without them (every call site
	 * checks for NULL ops/functions):
	 *   conversations (TODO(M4)): no conversation windows; messages are
	 *     still logged, and received IMs/chats are not shown;
	 *   xfers, privacy, roomlist (TODO(M5)): transfers are neither offered
	 *     nor shown, privacy and room list windows do not exist;
	 *   sound, idle (TODO(M6)): see gtksound.c and gtkidle.c;
	 *   whiteboard, media: dropped (no voice/video, no Doodle).
	 * The blist ops (gtkblist.c) leave save_node/remove_node/
	 * save_account NULL: libpurple then uses its own savers, which is
	 * how blist.xml is written.
	 */
	purple_accounts_set_ui_ops(pidgin_accounts_get_ui_ops());
	purple_blist_set_ui_ops(pidgin_blist_get_ui_ops());
	purple_notify_set_ui_ops(pidgin_notify_get_ui_ops());
	purple_request_set_ui_ops(pidgin_request_get_ui_ops());
	purple_sound_set_ui_ops(pidgin_sound_get_ui_ops());
	purple_connections_set_ui_ops(pidgin_connections_get_ui_ops());
	purple_idle_set_ui_ops(pidgin_idle_get_ui_ops());

	pidgin_account_init();
	pidgin_blist_init();
	pidgin_docklet_init();   /* M6: after the blist (its prefs and signals) */
	pidgin_connection_init();
	pidgin_pounces_init();
	pidgin_utils_init();
	pidgin_notify_init();
}

static void
pidgin_quit(void)
{
	/* Uninit */
	pidgin_utils_uninit();
	pidgin_notify_uninit();
	pidgin_connection_uninit();
	pidgin_docklet_uninit();
	pidgin_blist_uninit();
	pidgin_account_uninit();
	pidgin_debug_uninit();

	if (NULL != ui_info) {
		g_hash_table_destroy(ui_info);
		ui_info = NULL;
	}
}

static GHashTable *
pidgin_ui_get_info(void)
{
	if (NULL == ui_info) {
		ui_info = g_hash_table_new(g_str_hash, g_str_equal);

		g_hash_table_insert(ui_info, "name", (char*)PIDGIN_NAME);
		g_hash_table_insert(ui_info, "version", VERSION);
		g_hash_table_insert(ui_info, "website", PURPLE_WEBSITE);
		g_hash_table_insert(ui_info, "dev_website", PURPLE_DEVEL_WEBSITE);
		g_hash_table_insert(ui_info, "client_type", "pc");

		/* Same client keys as Pidgin 2 (gtkmain.c) for the prpls that
		 * still look for them. */
		g_hash_table_insert(ui_info, "prpl-aim-clientkey", "do1UCeb5gNqxB1S1");
		g_hash_table_insert(ui_info, "prpl-icq-clientkey", "ma1cSASNCKFtrdv9");
		g_hash_table_insert(ui_info, "prpl-aim-distid", GINT_TO_POINTER(1715));
		g_hash_table_insert(ui_info, "prpl-icq-distid", GINT_TO_POINTER(1550));
	}

	return ui_info;
}

static PurpleCoreUiOps core_ops =
{
	pidgin_prefs_init,
	debug_init,
	pidgin_ui_init,
	pidgin_quit,
	pidgin_ui_get_info,
	NULL,
	NULL,
	NULL
};

/**************************************************************************
 * Quitting
 **************************************************************************/

void
pidgin_application_quit(void)
{
	if (core_running) {
		core_running = FALSE;
		purple_debug_info("main", "Quitting: saving and shutting down libpurple\n");
		/* Disconnects, saves every .xml file, unloads plugins, and calls
		 * pidgin_quit() through the core UI ops. */
		purple_core_quit();
	}

	if (application != NULL) {
		if (app_held) {
			app_held = FALSE;
			g_application_release(G_APPLICATION(application));
		}
		g_application_quit(G_APPLICATION(application));
	}
}

static gboolean
quit_signal_cb(gpointer data)
{
	int sig = GPOINTER_TO_INT(data);

	if (core_running)
		purple_debug_warning("main", "Caught signal %d, quitting\n", sig);
	pidgin_application_quit();

	return G_SOURCE_CONTINUE;
}

static void
install_signal_handlers(void)
{
	/* libpurple writes to sockets; a closed peer must not kill us. */
	signal(SIGPIPE, SIG_IGN);

	signal_sources[0] = g_unix_signal_add(SIGINT, quit_signal_cb, GINT_TO_POINTER(SIGINT));
	signal_sources[1] = g_unix_signal_add(SIGTERM, quit_signal_cb, GINT_TO_POINTER(SIGTERM));
	signal_sources[2] = g_unix_signal_add(SIGHUP, quit_signal_cb, GINT_TO_POINTER(SIGHUP));
}

static void
remove_signal_handlers(void)
{
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(signal_sources); i++) {
		if (signal_sources[i] != 0) {
			g_source_remove(signal_sources[i]);
			signal_sources[i] = 0;
		}
	}
}

/**************************************************************************
 * Application actions
 **************************************************************************/

static void
quit_action_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	pidgin_application_quit();
}

static void
accounts_action_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	pidgin_accounts_window_show();
}

static void
debug_action_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	pidgin_debug_window_show();
}

static void
about_action_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	pidgin_dialogs_about();
}

static const GActionEntry app_actions[] = {
	{ .name = "quit", .activate = quit_action_cb },
	{ .name = "accounts", .activate = accounts_action_cb },
	{ .name = "debug", .activate = debug_action_cb },
	{ .name = "about", .activate = about_action_cb },
};

/**************************************************************************
 * Startup
 **************************************************************************/

static void
conflict_dialog_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, NULL);

	pidgin_application_quit();
}

/* Profile contract, rule 8: refuse to run beside a Pidgin 2 that uses the
 * same profile. Returns FALSE (and shows a dialog) if one does. */
static gboolean
check_single_ui(void)
{
	GtkAlertDialog *dialog;
	char *conflict, *detail;

	conflict = pidgin_single_ui_find_conflict(purple_user_dir());
	if (conflict == NULL)
		return TRUE;

	g_printerr("pidgin4: Pidgin 2 is using the profile %s (%s); exiting.\n",
	           purple_user_dir(), conflict);

	/* However we leave (the dialog or a signal), it is a failure. */
	exit_status = 1;
	g_application_hold(G_APPLICATION(application));
	app_held = TRUE;

	detail = g_strdup_printf(_("Pidgin 2 is running with the same settings "
		"directory (%s):\n%s\n\nOnly one of them can use a settings "
		"directory at a time. Quit Pidgin 2 first, or start this one with "
		"a different directory (-c DIR)."), purple_user_dir(), conflict);
	dialog = gtk_alert_dialog_new("%s", _("Pidgin 2 is already running"));
	gtk_alert_dialog_set_detail(dialog, detail);
	gtk_alert_dialog_set_modal(dialog, TRUE);
	gtk_alert_dialog_choose(dialog, NULL, NULL, conflict_dialog_cb, NULL);
	g_object_unref(dialog);

	g_free(detail);
	g_free(conflict);
	return FALSE;
}

static void
load_css(void)
{
	GtkCssProvider *provider = gtk_css_provider_new();

	gtk_css_provider_load_from_resource(provider,
		PIDGIN4_RESOURCE_PATH "/style.css");
	gtk_style_context_add_provider_for_display(gdk_display_get_default(),
		GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(provider);
}

static void
add_plugin_search_paths(void)
{
	char *path;

	/*
	 * Profile contract, rule 3: only these directories, never the system
	 * /usr/lib64/pidgin or /usr/lib64/purple-2. Earlier entries win.
	 *   <profile>/pidgin4/plugins  pidgin4 UI plugins (created here)
	 *   <profile>/plugins          prpls shared with Pidgin 2 (Discord,
	 *                              Steam); not created, nothing added
	 *   <prefix>/lib/pidgin4       pidgin4 UI plugins from the prefix
	 *   <purple prefix>/lib/purple-2  libpurple adds this itself (LIBDIR
	 *                              of the installed libpurple)
	 */
	path = g_build_filename(pidgin_user_dir(), "plugins", NULL);
	if (g_mkdir_with_parents(path, S_IRWXU) != 0)
		purple_debug_error("main", "Could not create %s\n", path);
	purple_plugins_add_search_path(path);
	g_free(path);

	path = g_build_filename(purple_user_dir(), "plugins", NULL);
	purple_plugins_add_search_path(path);
	g_free(path);

	purple_plugins_add_search_path(LIBDIR);
}

static void
startup_cb(GApplication *app, gpointer data)
{
	GList *accounts;

	install_signal_handlers();

	g_action_map_add_action_entries(G_ACTION_MAP(app), app_actions,
	                                G_N_ELEMENTS(app_actions), app);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.quit",
		(const char *[]){ "<Control>q", NULL });

	gtk_window_set_default_icon_name(PIDGIN4_APP_ID);
	load_css();

	/* set a user-specified config directory */
	if (opts.config_dir != NULL)
		purple_util_set_user_dir(opts.config_dir);

	/* Unlike Pidgin 2 there is no ~/.gaim migration (purple_core_migrate):
	 * nobody has a pre-2007 profile any more. */

	if (!opts.multiple && !check_single_ui())
		return;

	purple_debug_set_enabled(opts.debug);

	purple_core_set_ui_ops(&core_ops);
	purple_eventloop_set_ui_ops(pidgin_eventloop_get_ui_ops());

	add_plugin_search_paths();

	if (!purple_core_init(PIDGIN_UI)) {
		g_printerr("Initialization of the libpurple core failed.\n");
		exit_status = 1;
		g_application_quit(app);
		return;
	}
	core_running = TRUE;

	/* Keep running with no window open: accounts stay connected until
	 * app.quit (Ctrl+Q) or a signal. TODO(M6): the tray icon. */
	g_application_hold(app);
	app_held = TRUE;

	/* TODO: Move blist loading into purple_blist_init() */
	purple_set_blist(purple_blist_new());
	purple_blist_load();

	/* load plugins we had when we quit (pidgin4's own list) */
	purple_plugins_load_saved(PIDGIN4_PREFS_ROOT "/plugins/loaded");

	/* TODO: Move pounces loading into purple_pounces_init() */
	purple_pounces_load();

	/* This needs to be before purple_blist_show() so the
	 * statusbox gets the forced online status. */
	if (opts.force_online)
		purple_network_force_online();

	/* The buddy list window (gtkblist.c) is the main window. */
	purple_blist_show();

	/* As Pidgin 2: with no enabled account, open the accounts window. */
	if ((accounts = purple_accounts_get_all_active()) == NULL ||
	    g_getenv("PIDGIN4_ACCOUNT_SELFTEST") != NULL)
		pidgin_accounts_window_show();
	g_list_free(accounts);

	if (purple_prefs_get_bool(PIDGIN4_PREFS_ROOT "/debug/enabled"))
		pidgin_debug_window_show();
	else if (opts.debug)
		pidgin_debug_window_show_for_session();

	/* Developer aids for headless testing: open one request of every
	 * kind (gtkrequest.c). PIDGIN4_ACCOUNT_SELFTEST is handled when the
	 * accounts window opens (gtkaccount.c). */
	if (g_getenv("PIDGIN4_REQUEST_SELFTEST") != NULL)
		pidgin_request_selftest();

	/* M6 developer aids (no-ops unless their variable is set). */
	pidgin_docklet_selftest();

	if (opts.login) {
		/* disable all accounts */
		for (accounts = purple_accounts_get_all(); accounts != NULL; accounts = accounts->next) {
			PurpleAccount *account = accounts->data;
			purple_account_set_enabled(account, PIDGIN_UI, FALSE);
		}
		/* honor the startup status preference */
		if (!purple_prefs_get_bool("/purple/savedstatus/startup_current_status"))
			purple_savedstatus_activate(purple_savedstatus_get_startup());
		/* now enable the requested ones */
		dologin_named(opts.login_arg);
	} else if (opts.nologin) {
		/* Set all accounts to "offline" */
		PurpleSavedStatus *saved_status;

		/* If we've used this type+message before, lookup the transient status */
		saved_status = purple_savedstatus_find_transient_by_type_and_message(
							PURPLE_STATUS_OFFLINE, NULL);

		/* If this type+message is unique then create a new transient saved status */
		if (saved_status == NULL)
			saved_status = purple_savedstatus_new(NULL, PURPLE_STATUS_OFFLINE);

		/* Set the status for each account */
		purple_savedstatus_activate(saved_status);
	} else {
		/* Everything is good to go--sign on already */
		if (!purple_prefs_get_bool("/purple/savedstatus/startup_current_status"))
			purple_savedstatus_activate(purple_savedstatus_get_startup());
		purple_accounts_restore_current_statuses();
	}
}

static void
activate_cb(GApplication *app, gpointer data)
{
	if (!core_running)
		return;

	/* A second launch raises the buddy list. */
	purple_blist_set_visible(TRUE);
}

static void
shutdown_cb(GApplication *app, gpointer data)
{
	/* Covers any path out of the main loop that skipped
	 * pidgin_application_quit(). */
	if (core_running) {
		core_running = FALSE;
		purple_core_quit();
	}
	remove_signal_handlers();
}

/**************************************************************************
 * Command line
 **************************************************************************/

static gboolean
login_option_cb(const char *option, const char *value, gpointer data,
                GError **error)
{
	opts.login = TRUE;
	g_free(opts.login_arg);
	opts.login_arg = g_strdup(value);
	return TRUE;
}

static const GOptionEntry option_entries[] = {
	{ "config", 'c', 0, G_OPTION_ARG_FILENAME, &opts.config_dir,
	  N_("use DIR for config files"), N_("DIR") },
	{ "debug", 'd', 0, G_OPTION_ARG_NONE, &opts.debug,
	  N_("print debugging messages to stdout and show the debug window"), NULL },
	{ "force-online", 'f', 0, G_OPTION_ARG_NONE, &opts.force_online,
	  N_("force online, regardless of network status"), NULL },
	{ "login", 'l', G_OPTION_FLAG_OPTIONAL_ARG, G_OPTION_ARG_CALLBACK,
	  (gpointer)login_option_cb,
	  N_("enable specified account(s) (optional argument NAME specifies "
	     "account(s) to use, separated by commas. Without this only the "
	     "first account will be enabled)"), N_("NAME") },
	{ "multiple", 'm', 0, G_OPTION_ARG_NONE, &opts.multiple,
	  N_("allow multiple instances, and skip the check for a Pidgin 2 "
	     "using the same config directory"), NULL },
	{ "allow-multiple", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE,
	  &opts.multiple, NULL, NULL },
	{ "nologin", 'n', 0, G_OPTION_ARG_NONE, &opts.nologin,
	  N_("don't automatically login"), NULL },
	{ "version", 'v', 0, G_OPTION_ARG_NONE, &opts.version,
	  N_("display the current version and exit"), NULL },
	{ NULL }
};

static int
handle_local_options_cb(GApplication *app, GVariantDict *options, gpointer data)
{
	if (opts.version) {
		printf("%s %s (%s) (libpurple %s)\n", "Pidgin 4", DISPLAY_VERSION,
		       REVISION, purple_core_get_version());
		return 0;
	}

	if (opts.multiple) {
		/* Also no GApplication uniqueness: this process becomes its own
		 * primary instance. */
		g_application_set_flags(app,
			g_application_get_flags(app) | G_APPLICATION_NON_UNIQUE);
	}

	return -1;
}

/*
 * With G_DEBUG=fatal-criticals (the test setup), criticals from GTK, GLib
 * and pidgin4 itself (G_LOG_DOMAIN "pidgin4") abort, but those logged by
 * libpurple and its plugins (no log domain) do not: libpurple 2.14 emits
 * some on ordinary profiles, e.g. purple_buddy_new() for buddies of an
 * account whose prpl is gone (blist.c: purple_presence_set_status_active
 * with no "offline" status). They are still printed/logged.
 */
static gboolean
fatal_log_filter(const char *log_domain, GLogLevelFlags log_level,
                 const char *message, gpointer data)
{
	if (log_domain == NULL && !(log_level & G_LOG_LEVEL_ERROR)) {
		g_printerr("pidgin4: not fatal (libpurple): %s\n", message);
		return FALSE;
	}
	return TRUE;
}

int
main(int argc, char *argv[])
{
	int status;

	g_test_log_set_fatal_handler(fatal_log_filter, NULL);

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	bind_textdomain_codeset(PACKAGE, "UTF-8");
	textdomain(PACKAGE);

	g_set_application_name("Pidgin 4");

	application = gtk_application_new(PIDGIN4_APP_ID, G_APPLICATION_DEFAULT_FLAGS);
	g_application_set_resource_base_path(G_APPLICATION(application),
	                                     PIDGIN4_RESOURCE_PATH);
	g_application_set_option_context_summary(G_APPLICATION(application),
		_("A second instance only raises the running one's window, "
		  "unless -m is given."));
	g_application_set_option_context_parameter_string(G_APPLICATION(application), "");
	g_application_add_main_option_entries(G_APPLICATION(application), option_entries);

	g_signal_connect(application, "handle-local-options",
	                 G_CALLBACK(handle_local_options_cb), NULL);
	g_signal_connect(application, "startup", G_CALLBACK(startup_cb), NULL);
	g_signal_connect(application, "activate", G_CALLBACK(activate_cb), NULL);
	g_signal_connect(application, "shutdown", G_CALLBACK(shutdown_cb), NULL);

	status = g_application_run(G_APPLICATION(application), argc, argv);
	if (status == 0)
		status = exit_status;

	g_clear_object(&application);
	g_free(opts.config_dir);
	g_free(opts.login_arg);
	g_free(pidgin4_dir);

	return status;
}
