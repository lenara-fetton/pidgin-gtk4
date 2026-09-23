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
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "debug.h"
#include "idle.h"
#include "prefs.h"
#include "util.h"

#include "gtkidle.h"
#include "pidginidle-wayland.h"

/*
 * System idle time on Wayland (Pidgin 2 used XScreenSaver):
 *
 *   /pidgin4/idle/method = "system" (default):
 *     1. ext-idle-notify-v1 (Sway, wlroots and KDE compositors), see
 *        pidginidle-wayland.c;
 *     2. else org.gnome.Mutter.IdleMonitor.GetIdletime on the session bus
 *        (GNOME: Mutter does not offer ext-idle-notify to clients), polled
 *        every 10 s;
 *     3. else no UI ops: libpurple's own "purple" idle (the time since the
 *        last message sent) drives auto-away.
 *   "purple": no UI ops, as 3.
 *   "none": the system is never idle (no auto-away from inactivity).
 *
 * libpurple only uses these ops when the shared /purple/away/idle_reporting
 * is "system" (or "none" with auto-away on); its polling (idle.c) is
 * unchanged. PIDGIN4_IDLE_TIMEOUT=<seconds> shortens the ext-idle-notify
 * threshold for testing (default 60).
 */

#define IDLE4_PREFS PIDGIN4_PREFS_ROOT "/idle"
#define MUTTER_NAME "org.gnome.Mutter.IdleMonitor"
#define MUTTER_PATH "/org/gnome/Mutter/IdleMonitor/Core"
#define MUTTER_POLL_SECONDS 10

typedef enum
{
	IDLE_SOURCE_NONE,     /* no ops: libpurple's "purple" idle */
	IDLE_SOURCE_WAYLAND,
	IDLE_SOURCE_MUTTER,
	IDLE_SOURCE_NEVER     /* "none": never idle */
} IdleSource;

static IdleSource source = IDLE_SOURCE_NONE;
static GDBusConnection *session_bus = NULL;
static GCancellable *mutter_cancellable = NULL;
static guint mutter_poll_id = 0;
static guint64 mutter_idle_ms = 0;
static int idle_handle;

static time_t
pidgin_get_time_idle(void)
{
	switch (source) {
	case IDLE_SOURCE_WAYLAND:
		return pidgin_idle_wayland_get_time_idle();
	case IDLE_SOURCE_MUTTER:
		return (time_t)(mutter_idle_ms / 1000);
	default:
		return 0;
	}
}

static PurpleIdleUiOps ui_ops =
{
	pidgin_get_time_idle,
	NULL,
	NULL,
	NULL,
	NULL
};

/**************************************************************************
 * Wayland
 **************************************************************************/

static void
wayland_changed_cb(gboolean idle)
{
	if (idle) {
		purple_debug_info("idle", "ext-idle-notify: idled (idle %ld s)\n",
		                  (long)pidgin_idle_wayland_get_time_idle());
	} else {
		purple_debug_info("idle", "ext-idle-notify: resumed after %ld s idle\n",
		                  (long)pidgin_idle_wayland_get_time_idle());
		/* Let libpurple notice at once (it polls every second only
		 * while it thinks we are idle). */
		if (purple_strequal(purple_prefs_get_string("/purple/away/idle_reporting"),
		                    "system"))
			purple_idle_touch();
	}
}

/**************************************************************************
 * GNOME: org.gnome.Mutter.IdleMonitor
 **************************************************************************/

static void
mutter_reply_cb(GObject *object, GAsyncResult *result, gpointer data)
{
	GVariant *reply;
	GError *error = NULL;

	reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result, &error);
	if (reply == NULL) {
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			purple_debug_warning("idle", "Mutter GetIdletime failed: %s\n",
			                     error->message);
		g_error_free(error);
		return;
	}
	g_variant_get(reply, "(t)", &mutter_idle_ms);
	g_variant_unref(reply);
}

static gboolean
mutter_poll_cb(gpointer data)
{
	g_dbus_connection_call(session_bus, MUTTER_NAME, MUTTER_PATH, MUTTER_NAME,
		"GetIdletime", NULL, G_VARIANT_TYPE("(t)"), G_DBUS_CALL_FLAGS_NONE,
		5000, mutter_cancellable, mutter_reply_cb, NULL);
	return G_SOURCE_CONTINUE;
}

static gboolean
mutter_available(void)
{
	GVariant *reply;
	gboolean owned = FALSE;

	if (session_bus == NULL)
		session_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
	if (session_bus == NULL)
		return FALSE;

	reply = g_dbus_connection_call_sync(session_bus, "org.freedesktop.DBus",
		"/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
		g_variant_new("(s)", MUTTER_NAME), G_VARIANT_TYPE("(b)"),
		G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
	if (reply != NULL) {
		g_variant_get(reply, "(b)", &owned);
		g_variant_unref(reply);
	}
	return owned;
}

static void
mutter_start(void)
{
	mutter_idle_ms = 0;
	mutter_cancellable = g_cancellable_new();
	mutter_poll_cb(NULL);
	mutter_poll_id = g_timeout_add_seconds(MUTTER_POLL_SECONDS, mutter_poll_cb, NULL);
}

/**************************************************************************
 * Setup
 **************************************************************************/

static void
stop_sources(void)
{
	pidgin_idle_wayland_stop();
	if (mutter_poll_id != 0) {
		g_source_remove(mutter_poll_id);
		mutter_poll_id = 0;
	}
	if (mutter_cancellable != NULL) {
		g_cancellable_cancel(mutter_cancellable);
		g_clear_object(&mutter_cancellable);
	}
	source = IDLE_SOURCE_NONE;
}

static guint
wayland_timeout(void)
{
	const char *env = g_getenv("PIDGIN4_IDLE_TIMEOUT");
	guint64 value;

	if (env != NULL && g_ascii_string_to_unsigned(env, 10, 1, 3600, &value, NULL))
		return (guint)value;
	return 60;
}

static PurpleIdleUiOps *
choose_source(void)
{
	const char *method = purple_prefs_get_string(IDLE4_PREFS "/method");

	stop_sources();

	if (purple_strequal(method, "none")) {
		source = IDLE_SOURCE_NEVER;
		purple_debug_info("idle", "idle method: none (never idle)\n");
		return &ui_ops;
	}

	if (purple_strequal(method, "purple")) {
		purple_debug_info("idle", "idle method: purple (libpurple's own)\n");
		return NULL;
	}

	pidgin_idle_wayland_set_callback(wayland_changed_cb);
	if (pidgin_idle_wayland_start(wayland_timeout())) {
		source = IDLE_SOURCE_WAYLAND;
		purple_debug_info("idle", "idle method: system, ext-idle-notify-v1 "
		                  "version %u (threshold %u s)\n",
		                  pidgin_idle_wayland_get_version(), wayland_timeout());
		return &ui_ops;
	}

	if (mutter_available()) {
		source = IDLE_SOURCE_MUTTER;
		mutter_start();
		purple_debug_info("idle", "idle method: system, %s (polled every %d s)\n",
		                  MUTTER_NAME, MUTTER_POLL_SECONDS);
		return &ui_ops;
	}

	purple_debug_info("idle", "idle method: system, but neither "
	                  "ext-idle-notify-v1 nor %s is available; using "
	                  "libpurple's own idle tracking\n", MUTTER_NAME);
	return NULL;
}

static void
method_changed_cb(const char *name, PurplePrefType type, gconstpointer value,
                  gpointer data)
{
	purple_idle_set_ui_ops(choose_source());
}

PurpleIdleUiOps *
pidgin_idle_get_ui_ops(void)
{
	static gboolean initialized = FALSE;

	if (!initialized) {
		initialized = TRUE;
		/* Read by the M5 preferences window. */
		purple_prefs_add_none(IDLE4_PREFS);
		purple_prefs_add_string(IDLE4_PREFS "/method", "system");
		purple_prefs_connect_callback(&idle_handle, IDLE4_PREFS "/method",
		                              method_changed_cb, NULL);
	}

	return choose_source();
}

void
pidgin_idle_uninit(void)
{
	purple_prefs_disconnect_by_handle(&idle_handle);
	stop_sources();
	g_clear_object(&session_bus);
}
