/*
 * purple - Jabber Protocol Plugin
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
 *
 */
#include "internal.h"

#include "debug.h"
#include "signals.h"
#include "value.h"

#include "jabber.h"
#include "kvstore.h"

static PurplePlugin *kv_plugin = NULL;

gchar *
jabber_kv_load(PurpleAccount *account, const char *key)
{
	const char *value;

	g_return_val_if_fail(account != NULL, NULL);
	g_return_val_if_fail(key != NULL, NULL);
	g_return_val_if_fail(kv_plugin != NULL, NULL);

	value = purple_signal_emit_return_1(kv_plugin, "jabber-kv-load",
	                                    account, key);
	if (value == NULL) {
		purple_debug_misc("jabber", "kv: no value for %s (no UI store?)\n",
		                  key);
		return NULL;
	}

	return g_strdup(value);
}

void
jabber_kv_store(PurpleAccount *account, const char *key, const char *value)
{
	g_return_if_fail(account != NULL);
	g_return_if_fail(key != NULL);
	g_return_if_fail(kv_plugin != NULL);

	purple_signal_emit(kv_plugin, "jabber-kv-store", account, key, value);
}

void
jabber_kv_init(PurplePlugin *plugin)
{
	kv_plugin = plugin;

	/* Returns the stored value, or NULL. The UI owns the returned string;
	 * it must stay valid until the next call (we strdup it immediately). */
	purple_signal_register(plugin, "jabber-kv-load",
			purple_marshal_POINTER__POINTER_POINTER,
			purple_value_new(PURPLE_TYPE_STRING), 2,
			purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
			purple_value_new(PURPLE_TYPE_STRING)); /* key */

	purple_signal_register(plugin, "jabber-kv-store",
			purple_marshal_VOID__POINTER_POINTER_POINTER,
			NULL, 3,
			purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
			purple_value_new(PURPLE_TYPE_STRING),  /* key */
			purple_value_new(PURPLE_TYPE_STRING)); /* value, NULL deletes */
}
