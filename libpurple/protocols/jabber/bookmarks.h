/**
 * @file bookmarks.h XEP-0402 PEP Native Bookmarks (with XEP-0048 fallback)
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
#ifndef PURPLE_JABBER_BOOKMARKS_H_
#define PURPLE_JABBER_BOOKMARKS_H_

#include "plugin.h"

#include "jabber.h"
#include "xmlnode.h"

typedef struct {
	char *jid;       /**< room bare JID */
	char *name;
	char *nick;
	char *password;
	gboolean autojoin;
} JabberBookmark;

void jabber_bookmark_free(JabberBookmark *bookmark);

/*
 * Pure (de)serialization, unit-tested.
 */

/** XEP-0402: an <item id='room@server'><conference/></item>. */
JabberBookmark *jabber_bookmark_parse_item(xmlnode *item);

/** XEP-0048: all <conference/>s of a <storage xmlns='storage:bookmarks'/>. */
GList *jabber_bookmarks_parse_storage(xmlnode *storage);

/** XEP-0402: <conference xmlns='urn:xmpp:bookmarks:1'/> for an item. */
xmlnode *jabber_bookmark_to_conference(const JabberBookmark *bookmark);

/** XEP-0048: <storage xmlns='storage:bookmarks'/> holding @a bookmarks. */
xmlnode *jabber_bookmarks_to_storage(GList *bookmarks);

/*
 * Stream integration.
 */

/** Fetches the bookmarks (after connecting) and joins autojoin rooms. */
void jabber_bookmarks_fetch(JabberStream *js);

/** The account advertised urn:xmpp:bookmarks:1#compat. */
void jabber_bookmarks_set_compat(JabberStream *js, gboolean compat);

void jabber_bookmarks_close(JabberStream *js);

/** PEP notification handlers (registered from jabber_pep_init). */
void jabber_bookmarks_pep_init(void);

/** Registers the bookmark-add and bookmark-remove IPC commands. */
void jabber_bookmarks_init(PurplePlugin *plugin);

#endif /* PURPLE_JABBER_BOOKMARKS_H_ */
