/**
 * @file stream_management.h XEP-0198
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

#ifndef PURPLE_JABBER_STREAM_MANAGEMENT_H
#define PURPLE_JABBER_STREAM_MANAGEMENT_H

void jabber_sm_init(void);
void jabber_sm_uninit(void);

/* Post-bind <enable resume='true'/>. */
void jabber_sm_enable(JabberStream *js);
void jabber_sm_process_packet(JabberStream *js, xmlnode *packet);

void jabber_sm_ack_send(JabberStream *js);
void jabber_sm_ack_read(JabberStream *js, xmlnode *packet);

void jabber_sm_outbound(JabberStream *js, xmlnode *packet);
void jabber_sm_inbound(JabberStream *js, xmlnode *packet);

/* Session resumption.  The state survives the JabberStream (see
 * stream_management.c). */

/* TRUE if a dropped session of this account can be resumed. */
gboolean jabber_sm_can_resume(JabberStream *js);
/* Sends <resume/> (instead of resource binding) if possible. */
gboolean jabber_sm_resume_start(JabberStream *js);
/* The <resume/> element, for SASL2 inline use; sets SM_RESUMING. */
xmlnode *jabber_sm_resume_node(JabberStream *js);
/* <resumed/>: re-sends unacked stanzas, finishes the session setup. */
void jabber_sm_resumed(JabberStream *js, xmlnode *packet);
/* <failed/> in answer to <resume/>: forget the old session.  The caller
 * binds a new one.  packet may be NULL. */
void jabber_sm_resume_failed(JabberStream *js, xmlnode *packet);

/* The Bind2 inline <enable/>; call jabber_sm_inline_enable_sent() once
 * it went out. */
xmlnode *jabber_sm_inline_enable_node(JabberStream *js);
void jabber_sm_inline_enable_sent(JabberStream *js);

/* A new (not resumed) session is bound and SM enable was requested if
 * available: re-send the old session's unacked messages. */
void jabber_sm_session_started(JabberStream *js);

/* From jabber_close(): saves or discards the resumption state.  Returns
 * TRUE if the stream must be left unclosed (no </stream:stream>) so the
 * server keeps the session for resumption. */
gboolean jabber_sm_stream_closing(JabberStream *js);

#endif /* PURPLE_JABBER_STREAM_MANAGEMENT_H */
