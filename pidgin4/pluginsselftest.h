/*
 * pidgin4: the UI plugins selftest (M7, see pluginsselftest.c).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef _PIDGIN_PLUGINS_SELFTEST_H_
#define _PIDGIN_PLUGINS_SELFTEST_H_

/**
 * No-op unless PIDGIN4_PLUGINS_SELFTEST is set: loads every ported UI
 * plugin, checks what each does on selftest conversations, unloads them
 * and quits (exit status 0 on success). See pidgin4/TESTING.md.
 */
void pidgin_ported_plugins_selftest(void);

#endif /* _PIDGIN_PLUGINS_SELFTEST_H_ */
