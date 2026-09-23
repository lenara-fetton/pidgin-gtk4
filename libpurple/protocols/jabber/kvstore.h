/**
 * @file kvstore.h Per-account key/value storage provided by the UI
 *
 * purple
 *
 * Purple is the legal property of its developers, whose names are too numerous
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
#ifndef PURPLE_JABBER_KVSTORE_H_
#define PURPLE_JABBER_KVSTORE_H_

#include "account.h"
#include "plugin.h"

/*
 * Small per-account string store that the prpl uses for state which must
 * NOT go into accounts.xml (see the profile compatibility contract in
 * doc/PIDGIN-UPGRADE.md): the XEP-0313 MAM archive ids, the XEP-0484 FAST
 * token, and similar.
 *
 * The prpl doesn't store anything itself.  It asks the UI through two
 * signals on the jabber plugin handle:
 *
 *   "jabber-kv-load"  (PurpleAccount *account, const char *key)
 *                     -> const char *value, or NULL if unknown
 *   "jabber-kv-store" (PurpleAccount *account, const char *key,
 *                      const char *value)   value == NULL deletes
 *
 * pidgin4 answers them from <profile>/pidgin4/messages.db.  A UI that does
 * not connect to them (the GTK 2 UI, finch) simply gets no persistence:
 * jabber_kv_load() returns NULL and callers must treat that as "no state",
 * e.g. do a bounded MAM catch-up and a full SASL login.
 *
 * Keys are short ASCII identifiers, namespaced by feature, e.g.
 * "mam/last-id", "mam/last-id/<muc-jid>", "fast/token", "fast/mechanism".
 */

/**
 * Returns the stored value for @a key on @a account, or NULL.  The caller
 * owns the returned string.
 */
gchar *jabber_kv_load(PurpleAccount *account, const char *key);

/**
 * Stores @a value under @a key for @a account.  A NULL @a value removes
 * the key.  No-op when no UI answers the signal.
 */
void jabber_kv_store(PurpleAccount *account, const char *key,
                     const char *value);

/** Registers the two signals on the plugin handle. Called from jabber_plugin_init(). */
void jabber_kv_init(PurplePlugin *plugin);

#endif /* PURPLE_JABBER_KVSTORE_H_ */
