/**
 * @file mam.h XEP-0313 Message Archive Management
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
#ifndef PURPLE_JABBER_MAM_H_
#define PURPLE_JABBER_MAM_H_

#include "account.h"
#include "plugin.h"

#include "jabber.h"
#include "chat.h"
#include "xmlnode.h"

/** Catch-up without a stored archive id looks back this far, bounded. */
#define JABBER_MAM_CATCHUP_SECONDS   (24 * 60 * 60)
#define JABBER_MAM_PAGE_SIZE         50
#define JABBER_MAM_CATCHUP_MAX_PAGES 20
#define JABBER_MAM_OLDER_MAX         100
/** Size of the per-account "seen this session" id set. */
#define JABBER_MAM_SEEN_MAX          4096

typedef enum {
	JABBER_MAM_QUERY_CATCHUP,  /**< forward from the last id (or a time) */
	JABBER_MAM_QUERY_OLDER,    /**< one page before an id (scroll-back) */
	JABBER_MAM_QUERY_POSITION  /**< learn the newest id, deliver nothing */
} JabberMamQueryKind;

typedef enum {
	JABBER_MAM_PAGE_DONE,
	JABBER_MAM_PAGE_NEXT
} JabberMamPageAction;

typedef struct {
	JabberStream *js;
	JabberMamQueryKind kind;
	char *queryid;
	char *archive;    /**< bare JID of the archive (own account or room) */
	gboolean muc;     /**< a room archive */
	char *with;       /**< filter for the own archive, or NULL */
	char *conv_name;  /**< for mam-query-done; NULL for the account catch-up */
	char *kv_key;     /**< where the last id is stored, or NULL */
	char *after;      /**< RSM cursor for CATCHUP */
	char *before;     /**< RSM cursor for OLDER ("" = the newest page) */
	time_t start;     /**< CATCHUP by time when no id is known (0 = none) */
	guint max;        /**< page size */
	guint pages;      /**< pages completed */
	guint max_pages;
	guint page_count; /**< results received for the current page */
	guint delivered;  /**< results received in total */
	char *first;      /**< oldest id seen in this query */
	char *last;       /**< newest id seen in this query */
	gboolean complete;
	gboolean retried; /**< CATCHUP restarted after an unknown id */
} JabberMamQuery;

/*
 * Pure pieces (unit-tested).
 */

/** New query state.  @a js may be NULL in tests. */
JabberMamQuery *jabber_mam_query_new(JabberStream *js, JabberMamQueryKind kind,
                                     const char *archive, gboolean muc);
void jabber_mam_query_free(JabberMamQuery *query);

/** Builds the <query xmlns='urn:xmpp:mam:2'/> for the next page. */
xmlnode *jabber_mam_query_build(JabberMamQuery *query);

/**
 * Updates the paging state from a <fin/> (complete flag and RSM first and
 * last) and says whether to fetch another page.
 */
JabberMamPageAction jabber_mam_query_page_done(JabberMamQuery *query,
		gboolean complete, const char *rsm_first, const char *rsm_last);

/**
 * If @a packet carries a MAM <result/>, returns the forwarded <message/>
 * (or NULL if malformed) and sets the out parameters (all optional; the
 * strings are owned by @a packet).  @a is_result says whether it was a
 * result at all.
 */
xmlnode *jabber_mam_unwrap(xmlnode *packet, gboolean *is_result,
                           const char **queryid, const char **id,
                           gboolean *has_stamp, time_t *stamp);

/**
 * The per-account session id set.  Returns TRUE if @a key was already
 * there; adds it when @a add.  Bounded (oldest entries are dropped).
 */
gboolean jabber_mam_seen(PurpleAccount *account, const char *key, gboolean add);

/*
 * Stream integration.
 */

/** Routes a <message/> carrying a <result/>.  TRUE if it was one. */
gboolean jabber_mam_handle_result(JabberStream *js, xmlnode *packet);

/** Starts the account catch-up (js->mam_supported must be set). */
void jabber_mam_catchup(JabberStream *js);

/** Starts the room catch-up after (re)joining @a chat. */
void jabber_mam_muc_catchup(JabberChat *chat);

/** A live message with archive id @a id from archive @a by was shown. */
void jabber_mam_note_live_id(JabberStream *js, const char *by, const char *id);

/** The stored (or session) last archive id for @a room_jid, or NULL. */
char *jabber_mam_get_last_id(PurpleAccount *account, const char *room_jid);

void jabber_mam_close(JabberStream *js);

/** Registers the mam-fetch-older IPC command and mam-query-done signal. */
void jabber_mam_init(PurplePlugin *plugin);
void jabber_mam_uninit(void);

#endif /* PURPLE_JABBER_MAM_H_ */
