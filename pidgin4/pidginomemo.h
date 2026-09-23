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
#ifndef _PIDGINOMEMO_H_
#define _PIDGINOMEMO_H_

#include "pidgin.h"
#include "account.h"

/*
 * OMEMO fingerprints and trust (M5), over the core-omemo plugin's IPC
 * (omemo-list-devices, omemo-set-trust, omemo-own-fingerprint; see
 * doc/PIDGIN-UPGRADE.md, M8 "Landed (OMEMO)"). Without the plugin loaded
 * everything is greyed out.
 */

/** TRUE if the core-omemo plugin is loaded. */
gboolean pidgin_omemo_is_available(void);

/**
 * Shows the fingerprints window: our own fingerprint and every known
 * device with a trust drop-down. @account may be NULL (the first XMPP
 * account); @jid, if not NULL, selects that contact (for the
 * conversation window, M4b).
 */
void pidgin_omemo_show_fingerprints(PurpleAccount *account, const char *jid);

void pidgin_omemo_init(void);
void pidgin_omemo_uninit(void);

/** PIDGIN4_WINDOWS_SELFTEST: opens the window (plugin loaded or not). */
void pidgin_omemo_selftest(void);

/** TEST ONLY: use @plugin (loaded, with the IPC commands) as the OMEMO
 * plugin; NULL: the real one again. */
void pidgin_omemo_set_plugin_for_tests(PurplePlugin *plugin);

#endif /* _PIDGINOMEMO_H_ */
