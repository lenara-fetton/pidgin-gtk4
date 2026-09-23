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
#ifndef _PIDGINSELFTEST_H_
#define _PIDGINSELFTEST_H_

#include "pidgin.h"

/*
 * PIDGIN4_WINDOWS_SELFTEST=1 (M5): opens every M5 window, walks every
 * preferences page, opens each editor, then quits with status 0, or 1 if
 * any step failed. Module selftests are synchronous: they open their
 * windows, spin the main loop with pidgin_selftest_iterate()/_wait(),
 * record problems with pidgin_selftest_fail(), and close what they
 * opened before returning.
 */

/** Starts the selftest from an idle callback (gtkmain.c). */
void pidgin_windows_selftest_start(void);

/** Spins the default main context for about @ms milliseconds. */
void pidgin_selftest_iterate(guint ms);

/** Spins the main context until @cond(@data) is TRUE or @timeout_ms
 * passes. Returns the last value of @cond. */
gboolean pidgin_selftest_wait(gboolean (*cond)(gpointer data), gpointer data,
                              guint timeout_ms);

/** Logs a selftest step ("selftest: <module>: ..."). */
void pidgin_selftest_log(const char *module, const char *format, ...)
	G_GNUC_PRINTF(2, 3);

/** Records a failure (the selftest will exit 1) and logs it. */
void pidgin_selftest_fail(const char *module, const char *format, ...)
	G_GNUC_PRINTF(2, 3);

/** TRUE while the windows selftest runs (modules can skip slow work). */
gboolean pidgin_selftest_is_running(void);

#endif /* _PIDGINSELFTEST_H_ */
