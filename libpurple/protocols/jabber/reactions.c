/*
 * purple - Jabber Protocol Plugin
 *
 * XEP-0444 Message Reactions (M8, doc/PIDGIN-UPGRADE.md).
 *
 * A <reactions/> element carries the sender's complete current set of
 * reactions to one message.  The UI wants add/remove events, so the prpl
 * remembers each sender's last set per target for the session and emits
 * the difference.  The UI keeps the persistent state.
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
#include "internal.h"

#include "debug.h"
#include "signals.h"
#include "util.h"

#include "jabber.h"
#include "message.h"
#include "reactions.h"

#include <string.h>

#define REACTIONS_MAX 32
#define REACTION_MAX_BYTES 64
#define REACTIONS_CACHE_MAX 4096

/* "<account>\x1f<conv>\x1f<sender identity>\x1f<target>" -> char** set */
static GHashTable *reaction_sets = NULL;

/**************************************************************************
 * Sets
 **************************************************************************/

static gboolean
set_contains(char **set, const char *emoji)
{
	for (; set && *set; set++)
		if (purple_strequal(*set, emoji))
			return TRUE;
	return FALSE;
}

static void
set_add(GPtrArray *set, const char *emoji)
{
	char *clean;
	guint i;

	if (emoji == NULL || set->len >= REACTIONS_MAX)
		return;

	clean = g_strstrip(g_strdup(emoji));
	if (*clean == '\0' || strlen(clean) > REACTION_MAX_BYTES ||
	    !g_utf8_validate(clean, -1, NULL)) {
		g_free(clean);
		return;
	}
	for (i = 0; i < set->len; i++) {
		if (purple_strequal(g_ptr_array_index(set, i), clean)) {
			g_free(clean);
			return;
		}
	}
	g_ptr_array_add(set, clean);
}

static char **
set_finish(GPtrArray *set)
{
	g_ptr_array_add(set, NULL);
	return (char **)g_ptr_array_free(set, FALSE);
}

char **
jabber_reactions_parse(xmlnode *reactions)
{
	GPtrArray *set = g_ptr_array_new();
	xmlnode *reaction;

	for (reaction = reactions ? xmlnode_get_child(reactions, "reaction") : NULL;
	     reaction; reaction = xmlnode_get_next_twin(reaction)) {
		char *data = xmlnode_get_data(reaction);
		set_add(set, data);
		g_free(data);
	}

	return set_finish(set);
}

char **
jabber_reactions_split(const char *list)
{
	GPtrArray *set = g_ptr_array_new();
	char **parts, **p;

	parts = g_strsplit_set(list ? list : "", " \t\r\n", -1);
	for (p = parts; *p; p++)
		set_add(set, *p);
	g_strfreev(parts);

	return set_finish(set);
}

void
jabber_reactions_diff(char **old_set, char **new_set,
                      GPtrArray *added, GPtrArray *removed)
{
	char **p;

	for (p = new_set; p && *p; p++)
		if (!set_contains(old_set, *p))
			g_ptr_array_add(added, *p);
	for (p = old_set; p && *p; p++)
		if (!set_contains(new_set, *p))
			g_ptr_array_add(removed, *p);
}

void
jabber_reactions_reset(void)
{
	if (reaction_sets) {
		g_hash_table_destroy(reaction_sets);
		reaction_sets = NULL;
	}
}

static char *
cache_key(JabberStream *js, const JabberMessageTarget *t, const char *target_id)
{
	char *bare = jabber_get_bare_jid(t->conv_name);
	char *conv = g_utf8_strdown(bare ? bare : t->conv_name, -1);
	char *key = g_strdup_printf("%s\x1f%s\x1f%s\x1f%s",
			purple_account_get_username(purple_connection_get_account(js->gc)),
			conv, t->identity ? t->identity : "", target_id);

	g_free(conv);
	g_free(bare);
	return key;
}

/**************************************************************************
 * Applying a set
 **************************************************************************/

static char *
join_escaped(GPtrArray *emoji)
{
	GString *str = g_string_new(NULL);
	guint i;

	for (i = 0; i < emoji->len; i++) {
		char *escaped = g_markup_escape_text(g_ptr_array_index(emoji, i), -1);
		if (i)
			g_string_append_c(str, ' ');
		g_string_append(str, escaped);
		g_free(escaped);
	}

	return g_string_free(str, FALSE);
}

/* Takes @new_set. */
static void
reactions_apply(JabberStream *js, const JabberMessageTarget *t,
                const char *target_id, char **new_set, time_t when,
                gboolean delayed)
{
	PurpleAccount *account = purple_connection_get_account(js->gc);
	GPtrArray *added = g_ptr_array_new(), *removed = g_ptr_array_new();
	char *key = cache_key(js, t, target_id);
	char **old_set;
	gboolean handled = TRUE;
	guint i;

	if (reaction_sets == NULL)
		reaction_sets = g_hash_table_new_full(g_str_hash, g_str_equal,
		                                      g_free, (GDestroyNotify)g_strfreev);

	old_set = g_hash_table_lookup(reaction_sets, key);
	jabber_reactions_diff(old_set, new_set, added, removed);

	if (added->len == 0 && removed->len == 0) {
		g_ptr_array_free(added, TRUE);
		g_ptr_array_free(removed, TRUE);
		g_strfreev(new_set);
		g_free(key);
		return;
	}

	if (jabber_ui_supports_message_meta()) {
		for (i = 0; i < removed->len; i++)
			if (!GPOINTER_TO_INT(purple_signal_emit_return_1(
					purple_conversations_get_handle(), "message-reaction",
					account, t->conv_name, target_id,
					g_ptr_array_index(removed, i), t->sender,
					GINT_TO_POINTER(FALSE))))
				handled = FALSE;
		for (i = 0; i < added->len; i++)
			if (!GPOINTER_TO_INT(purple_signal_emit_return_1(
					purple_conversations_get_handle(), "message-reaction",
					account, t->conv_name, target_id,
					g_ptr_array_index(added, i), t->sender,
					GINT_TO_POINTER(TRUE))))
				handled = FALSE;
	} else {
		handled = FALSE;
	}

	if (!handled) {
		char *who = jabber_message_display_name(js, t);
		char *list, *text;

		if (added->len) {
			list = join_escaped(added);
			text = g_strdup_printf(_("%s reacted %s to a message"), who, list);
		} else {
			list = join_escaped(removed);
			text = g_strdup_printf(_("%s removed the reaction %s"), who, list);
		}
		jabber_message_write_event(js, t, text, when, delayed);
		g_free(text);
		g_free(list);
		g_free(who);
	}

	/* removed/added point into the sets: update the cache last */
	g_ptr_array_free(added, TRUE);
	g_ptr_array_free(removed, TRUE);

	if (g_hash_table_size(reaction_sets) >= REACTIONS_CACHE_MAX)
		g_hash_table_remove_all(reaction_sets);
	if (new_set[0])
		g_hash_table_replace(reaction_sets, key, new_set);
	else {
		g_hash_table_remove(reaction_sets, key);
		g_free(key);
		g_strfreev(new_set);
	}
}

gboolean
jabber_reactions_handle(JabberMessage *jm, const JabberMessageTarget *t)
{
	const char *target_id = xmlnode_get_attrib(jm->reactions, "id");

	if (target_id == NULL || *target_id == '\0' || t->sender == NULL) {
		purple_debug_info("jabber", "Ignoring reactions without a target "
		                  "or sender\n");
		return TRUE;
	}

	reactions_apply(jm->js, t, target_id, jabber_reactions_parse(jm->reactions),
	                jm->sent, jm->delayed);
	return TRUE;
}

/**************************************************************************
 * IPC: send-reaction
 **************************************************************************/

static gboolean
jabber_reactions_ipc_send(PurpleAccount *account, const char *conv_name,
                          const char *target_id, const char *emoji_list)
{
	JabberIpcTarget t;
	xmlnode *message, *reactions, *child;
	char **set, **p;
	gboolean ok = FALSE;

	if (target_id == NULL || *target_id == '\0')
		return FALSE;

	if (!jabber_ipc_target_init(&t, account, conv_name))
		goto out;

	set = jabber_reactions_split(emoji_list);

	message = jabber_message_stanza_new(t.js, t.to, t.groupchat);
	reactions = xmlnode_new_child(message, "reactions");
	xmlnode_set_namespace(reactions, NS_REACTIONS);
	xmlnode_set_attrib(reactions, "id", target_id);
	for (p = set; *p; p++) {
		child = xmlnode_new_child(reactions, "reaction");
		xmlnode_insert_data(child, *p, -1);
	}

	if (set[0]) {
		/* XEP-0428: the whole body is the fallback for plain clients. */
		char *body = g_strjoinv(" ", set);

		child = xmlnode_new_child(message, "body");
		xmlnode_insert_data(child, body, -1);
		g_free(body);
		child = xmlnode_new_child(message, "fallback");
		xmlnode_set_namespace(child, NS_FALLBACK);
		xmlnode_set_attrib(child, "for", NS_REACTIONS);
	}
	child = xmlnode_new_child(message, "store");
	xmlnode_set_namespace(child, NS_HINTS);

	jabber_send(t.js, message);
	xmlnode_free(message);

	if (!t.groupchat) {
		/* Report our own reactions like ones from another device; in
		 * rooms the reflected message does that. */
		JabberMessageTarget own = { NULL, NULL, NULL, NULL, TRUE };

		own.conv_name = (char *)conv_name;
		own.sender = jabber_message_own_jid(t.js, FALSE);
		own.identity = jabber_message_own_jid(t.js, TRUE);
		reactions_apply(t.js, &own, target_id, set, time(NULL), FALSE);
		g_free(own.sender);
		g_free(own.identity);
	} else {
		g_strfreev(set);
	}
	ok = TRUE;

out:
	jabber_ipc_target_clear(&t);
	return ok;
}

void
jabber_reactions_init(PurplePlugin *plugin)
{
	/* gboolean (account, conv name, target id, space-separated emoji) */
	purple_plugin_ipc_register(plugin, "send-reaction",
			PURPLE_CALLBACK(jabber_reactions_ipc_send),
			purple_marshal_BOOLEAN__POINTER_POINTER_POINTER_POINTER,
			purple_value_new(PURPLE_TYPE_BOOLEAN), 4,
			purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING));
}
