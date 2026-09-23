/*
 * pidgin4: system idle time from the Wayland ext-idle-notify-v1 protocol.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef _PIDGINIDLE_WAYLAND_H_
#define _PIDGINIDLE_WAYLAND_H_

#include <time.h>

#include <glib.h>

/*
 * The compositor (Sway, and any wlroots or KDE compositor) sends "idled"
 * after @timeout seconds without user input and "resumed" on the next
 * input. The idle time is then now - (idled_at - timeout), and 0 while the
 * user is active. The protocol only tells when the threshold is crossed,
 * so idle times below the timeout read as 0; pidgin4 uses 60 s, far below
 * any auto-away setting (whole minutes).
 *
 * Version 2 of ext_idle_notifier_v1 is used when offered
 * (get_input_idle_notification: input only, idle inhibitors such as a
 * playing video do not keep the user "active"), else version 1.
 *
 * Event dispatch: the registry, the notifier and the notification live on
 * a private wl_event_queue (the registry roundtrip runs on it, so GTK's
 * events are not dispatched reentrantly). GDK reads the display fd for
 * every queue but dispatches only its own (GTK 4.22: proxies on the
 * default queue got no events until shutdown), so a small GSource
 * dispatches ours in the main loop; it never reads the fd itself.
 */

/**
 * Binds ext_idle_notifier_v1 on the default GdkDisplay and asks for a
 * notification after @timeout seconds. FALSE if the display is not
 * Wayland or the compositor lacks the protocol (e.g. Mutter).
 */
gboolean pidgin_idle_wayland_start(guint timeout);

/**
 * @callback runs in the main loop with TRUE on "idled" and FALSE on
 * "resumed" (before the idle time is reset, so it can still be read).
 */
void pidgin_idle_wayland_set_callback(void (*callback)(gboolean idle));

/** Destroys the notification and the notifier. */
void pidgin_idle_wayland_stop(void);

/** TRUE between a successful start and stop. */
gboolean pidgin_idle_wayland_is_running(void);

/** The protocol version bound (1 or 2), 0 when not running. */
guint pidgin_idle_wayland_get_version(void);

/** Seconds of idle time now (0 while the user is active). */
time_t pidgin_idle_wayland_get_time_idle(void);

/**
 * The arithmetic: with the notification's @timeout (seconds) and the time
 * "idled" was received, @idled_at (µs of g_get_real_time(), 0: not idle),
 * the idle time at @now (µs) in seconds.
 */
time_t pidgin_idle_time_since(gint64 now, gint64 idled_at, guint timeout);

#endif /* _PIDGINIDLE_WAYLAND_H_ */
