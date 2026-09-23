/**
 * @file sasl2.h XEP-0388 SASL2, XEP-0386 Bind2, XEP-0484 FAST
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
#ifndef PURPLE_JABBER_SASL2_H_
#define PURPLE_JABBER_SASL2_H_

#include "jabber.h"

/* Keys in the UI-backed per-account store (kvstore.h). */
#define JABBER_KV_SASL2_USER_AGENT_ID "sasl2/user-agent-id"
#define JABBER_KV_FAST_TOKEN          "fast/token"
#define JABBER_KV_FAST_MECHANISM      "fast/mechanism"
#define JABBER_KV_FAST_EXPIRY         "fast/expiry"

/** The Bind2 tag; the server makes the resource "<tag>.<random>". */
#define JABBER_BIND2_TAG "pidgin4"

/**
 * Starts SASL2 authentication if @a features offers it with Bind2 and the
 * stream is encrypted.  Returns FALSE (having done nothing) if legacy SASL
 * should be used instead.
 */
gboolean jabber_sasl2_start(JabberStream *js, xmlnode *features);

/** Handles <challenge/>, <success/>, <failure/>, <continue/> in NS_SASL2. */
void jabber_sasl2_process_packet(JabberStream *js, xmlnode *packet);

/*
 * The pieces below are only exposed for tests.
 */

/** HMAC-SHA-256(token, label || cb): the HT-SHA-256-* hashed token
 *  (draft-schmaus-kitten-sasl-ht).  Returns 32 bytes. */
guchar *jabber_ht_sha256(const char *token, const char *label,
                         const guchar *cb, gsize cb_len);

/** The channel binding type an HT-SHA-256-* mechanism needs: "" for
 *  -NONE, "tls-server-end-point" for -ENDP, "tls-exporter" for -EXPR,
 *  NULL for anything we don't implement. */
const char *jabber_ht_mech_cb_type(const char *mech);

/** Picks the HT mechanism to request a FAST token for, from the server's
 *  list: -EXPR if tls-exporter is available, else -ENDP if
 *  tls-server-end-point is, else -NONE.  NULL if none fits. */
const char *jabber_fast_pick_request_mech(GSList *server_mechs,
                                          gboolean have_exporter,
                                          gboolean have_endpoint);

/** Whether a stored token can be used now: it and its mechanism are set,
 *  the server offers the mechanism, the needed channel binding is
 *  available and the expiry (ISO 8601, may be NULL) is in the future. */
gboolean jabber_fast_token_usable(GSList *server_mechs, const char *mech,
                                  const char *token, const char *expiry,
                                  gint64 now, gboolean have_exporter,
                                  gboolean have_endpoint);

#endif /* PURPLE_JABBER_SASL2_H_ */
