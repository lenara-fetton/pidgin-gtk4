/*
 * pidgin4: the "one UI per profile" startup check.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef _PIDGIN_SINGLE_UI_H_
#define _PIDGIN_SINGLE_UI_H_

#include <glib.h>

/**
 * Looks for a running Pidgin 2 (a process whose argv[0] or executable is
 * named "pidgin") that uses the profile directory @profile_dir, by reading
 * /proc/<pid>/cmdline: "-c DIR", "-cDIR", "--config=DIR", "--config DIR",
 * or combined short options ending in c. Without one of those, Pidgin 2
 * uses $HOME/.purple (HOME from /proc/<pid>/environ). Relative
 * directories are resolved against /proc/<pid>/cwd, and all paths are
 * compared after realpath().
 *
 * Pidgin 2 cannot make the same check against pidgin4, so pidgin4 must
 * refuse to start (profile compatibility contract, rule 8).
 *
 * @return NULL if there is none, or a new string describing the process
 *         ("pid 1234: /usr/bin/pidgin -c /home/x/.purple").
 */
char *pidgin_single_ui_find_conflict(const char *profile_dir);

/**
 * Parses a NUL-separated /proc cmdline (@len bytes) and returns the
 * profile directory it names with -c/--config, or NULL for the default.
 * @is_pidgin is set to whether argv[0] is a Pidgin 2 binary. Exposed for
 * testing.
 */
char *pidgin_single_ui_parse_cmdline(const char *cmdline, gsize len,
                                     gboolean *is_pidgin);

#endif /* _PIDGIN_SINGLE_UI_H_ */
