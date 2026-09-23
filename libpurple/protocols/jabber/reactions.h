/**
 * @file reactions.h XEP-0444 Message Reactions
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
#ifndef PURPLE_JABBER_REACTIONS_H_
#define PURPLE_JABBER_REACTIONS_H_

#include "message.h"

/**
 * Handles an incoming <reactions/>.  The message is always consumed: its
 * body (if any) is the XEP-0428 fallback.  Emits one message-reaction
 * per emoji added or removed relative to the sender's previous set for
 * the target (known for this session); if no handler renders them, a
 * text line ("X reacted 👍 to a message") is written instead.
 */
gboolean jabber_reactions_handle(JabberMessage *jm, const JabberMessageTarget *t);

/**
 * Parses the <reaction/> children of @a reactions: trimmed, non-empty,
 * unique, at most 32 of at most 64 bytes each.  NULL-terminated.
 */
char **jabber_reactions_parse(xmlnode *reactions);

/**
 * Splits a space-separated emoji list the way jabber_reactions_parse()
 * cleans up a stanza.
 */
char **jabber_reactions_split(const char *list);

/**
 * The emoji in @a new_set but not in @a old_set (@a added) and the other
 * way around (@a removed); either set may be NULL.  The arrays get
 * borrowed pointers.
 */
void jabber_reactions_diff(char **old_set, char **new_set,
		GPtrArray *added, GPtrArray *removed);

/** Forgets the per-session reaction sets (tests, unload). */
void jabber_reactions_reset(void);

/** Registers the send-reaction IPC command. */
void jabber_reactions_init(PurplePlugin *plugin);

#endif /* PURPLE_JABBER_REACTIONS_H_ */
