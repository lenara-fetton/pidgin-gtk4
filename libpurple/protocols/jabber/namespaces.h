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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 *
 */

#ifndef PURPLE_JABBER_NAMESPACES_H_
#define PURPLE_JABBER_NAMESPACES_H_

#define NS_XMPP_BIND "urn:ietf:params:xml:ns:xmpp-bind"
#define NS_XMPP_CLIENT "jabber:client"
#define NS_XMPP_SERVER "jabber:server"
#define NS_XMPP_SASL "urn:ietf:params:xml:ns:xmpp-sasl"
#define NS_XMPP_SESSION "urn:ietf:params:xml:ns:xmpp-session"
#define NS_XMPP_STANZAS "urn:ietf:params:xml:ns:xmpp-stanzas"
#define NS_XMPP_STREAMS "http://etherx.jabber.org/streams"
#define NS_XMPP_TLS "urn:ietf:params:xml:ns:xmpp-tls"

/* XEP-0012 Last Activity (and XEP-0256 Last Activity in Presence) */
#define NS_LAST_ACTIVITY "jabber:iq:last"

/* XEP-0030 Service Discovery */
#define NS_DISCO_INFO "http://jabber.org/protocol/disco#info"
#define NS_DISCO_ITEMS "http://jabber.org/protocol/disco#items"

/* XEP-0047 IBB (In-band bytestreams) */
#define NS_IBB "http://jabber.org/protocol/ibb"

/* XEP-0065 SOCKS5 Bytestreams */
#define NS_BYTESTREAMS "http://jabber.org/protocol/bytestreams"

/* XEP-0066 Out of Band Data (OOB) */
#define NS_OOB_IQ_DATA "jabber:iq:oob"
#define NS_OOB_X_DATA  "jabber:x:oob"

/* XEP-0071 XHTML-IM (rich-text messages) */
#define NS_XHTML_IM "http://jabber.org/protocol/xhtml-im"
#define NS_XHTML "http://www.w3.org/1999/xhtml"

/* XEP-0084 v0.12 User Avatar */
#define NS_AVATAR_0_12_DATA     "http://www.xmpp.org/extensions/xep-0084.html#ns-data"
#define NS_AVATAR_0_12_METADATA "http://www.xmpp.org/extensions/xep-0084.html#ns-metadata"

/* XEP-0084 v1.1 User Avatar */
#define NS_AVATAR_1_1_DATA      "urn:xmpp:avatar:data"
#define NS_AVATAR_1_1_METADATA  "urn:xmpp:avatar:metadata"

/* XEP-0096 SI File Transfer */
#define NS_SI_FILE_TRANSFER 	"http://jabber.org/protocol/si/profile/file-transfer"

/* XEP-0124 Bidirectional-streams Over Synchronous HTTP (BOSH) */
#define NS_BOSH "http://jabber.org/protocol/httpbind"

/* XEP-0191 Simple Communications Blocking */
#define NS_SIMPLE_BLOCKING "urn:xmpp:blocking"

/* XEP-0198 Stream Management */
#define NS_STREAM_MANAGEMENT "urn:xmpp:sm:3"

/* XEP-0199 Ping */
#define NS_PING "urn:xmpp:ping"

/* XEP-0202 Entity Time */
#define NS_ENTITY_TIME "urn:xmpp:time"

/* XEP-0203 Delayed Delivery (and legacy delayed delivery) */
#define NS_DELAYED_DELIVERY "urn:xmpp:delay"
#define NS_DELAYED_DELIVERY_LEGACY "jabber:x:delay"

/*
 * M8 message and MUC core (carbons.c, mam.c, bookmarks.c, message.c, chat.c)
 */
/* XEP-0004 Data Forms, XEP-0045 MUC, XEP-0060 PubSub */
#define NS_XDATA "jabber:x:data"
#define NS_MUC "http://jabber.org/protocol/muc"
#define NS_MUC_USER "http://jabber.org/protocol/muc#user"
#define NS_PUBSUB "http://jabber.org/protocol/pubsub"
#define NS_PUBSUB_EVENT "http://jabber.org/protocol/pubsub#event"
#define NS_PUBSUB_PUBLISH_OPTIONS "http://jabber.org/protocol/pubsub#publish-options"
/* XEP-0048 Bookmarks (legacy, stored in PEP) */
#define NS_BOOKMARKS_LEGACY "storage:bookmarks"
/* XEP-0059 Result Set Management */
#define NS_RSM "http://jabber.org/protocol/rsm"
/* XEP-0184 Message Delivery Receipts */
#define NS_RECEIPTS "urn:xmpp:receipts"
/* XEP-0280 Message Carbons */
#define NS_CARBONS "urn:xmpp:carbons:2"
/* XEP-0297 Stanza Forwarding */
#define NS_FORWARD "urn:xmpp:forward:0"
/* XEP-0308 Last Message Correction */
#define NS_MESSAGE_CORRECT "urn:xmpp:message-correct:0"
/* XEP-0313 Message Archive Management */
#define NS_MAM "urn:xmpp:mam:2"
/* XEP-0333 Chat Markers */
#define NS_CHAT_MARKERS "urn:xmpp:chat-markers:0"
/* XEP-0334 Message Processing Hints */
#define NS_HINTS "urn:xmpp:hints"
/* XEP-0359 Unique and Stable Stanza IDs */
#define NS_SID "urn:xmpp:sid:0"
/* XEP-0402 PEP Native Bookmarks */
#define NS_BOOKMARKS2 "urn:xmpp:bookmarks:1"
#define NS_BOOKMARKS2_COMPAT "urn:xmpp:bookmarks:1#compat"
/* XEP-0421 Anonymous unique occupant identifiers for MUCs */
#define NS_OCCUPANT_ID "urn:xmpp:occupant-id:0"
/* XEP-0424 Message Retraction */
#define NS_RETRACT "urn:xmpp:message-retract:1"
/* XEP-0428 Fallback Indication */
#define NS_FALLBACK "urn:xmpp:fallback:0"
/* XEP-0444 Message Reactions */
#define NS_REACTIONS "urn:xmpp:reactions:0"
/* XEP-0461 Message Replies */
#define NS_REPLY "urn:xmpp:reply:0"

/* XEP-0206 XMPP over BOSH */
#define NS_XMPP_BOSH "urn:xmpp:xbosh"

/* XEP-0224 Attention */
#define NS_ATTENTION "urn:xmpp:attention:0"

/* XEP-0231 BoB (Bits of Binary) */
#define NS_BOB "urn:xmpp:bob"

/* XEP-0237 Roster Versioning */
#define NS_ROSTER_VERSIONING "urn:xmpp:features:rosterver"

/* XEP-0264 File Transfer Thumbnails (Thumbs) */
#define NS_THUMBS "urn:xmpp:thumbs:0"

/* Google extensions */
#define NS_GOOGLE_CAMERA "http://www.google.com/xmpp/protocol/camera/v1"
#define NS_GOOGLE_VIDEO "http://www.google.com/xmpp/protocol/video/v1"
#define NS_GOOGLE_VOICE "http://www.google.com/xmpp/protocol/voice/v1"
#define NS_GOOGLE_JINGLE_INFO "google:jingleinfo"

#define NS_GOOGLE_MAIL_NOTIFY "google:mail:notify"
#define NS_GOOGLE_ROSTER "google:roster"

#define NS_GOOGLE_PROTOCOL_SESSION "http://www.google.com/xmpp/protocol/session"
#define NS_GOOGLE_SESSION "http://www.google.com/session"
#define NS_GOOGLE_SESSION_PHONE "http://www.google.com/session/phone"
#define NS_GOOGLE_SESSION_VIDEO "http://www.google.com/session/video"

/* M8: XEP-0363 HTTP File Upload (httpupload.c) */
#define NS_HTTP_UPLOAD "urn:xmpp:http:upload:0"
/* M8 authentication/connection: SASL2 (XEP-0388), Bind2 (XEP-0386),
 * FAST (XEP-0484), SASL channel-binding type capability (XEP-0440),
 * Client State Indication (XEP-0352), Message Carbons (XEP-0280, only
 * for the Bind2 inline enable). */
#define NS_SASL2 "urn:xmpp:sasl:2"
#define NS_BIND2 "urn:xmpp:bind:0"
#define NS_FAST "urn:xmpp:fast:0"
#define NS_SASL_CB "urn:xmpp:sasl-cb:0"
#define NS_CSI "urn:xmpp:csi:0"
#ifndef NS_CARBONS
#define NS_CARBONS "urn:xmpp:carbons:2"
#endif

/* M8 message semantics */
/* XEP-0393 Message Styling */
#define NS_STYLING "urn:xmpp:styling:0"
/* XEP-0424 v0.2/0.3 (retract:0 via XEP-0422 fastening) */
#define NS_RETRACT_LEGACY "urn:xmpp:message-retract:0"
#define NS_FASTEN "urn:xmpp:fasten:0"
/* XEP-0425 Message Moderation */
#define NS_MODERATE "urn:xmpp:message-moderate:1"
#define NS_MODERATE_LEGACY "urn:xmpp:message-moderate:0"
/* XEP-0490 Message Displayed Synchronization */
#define NS_MDS "urn:xmpp:mds:displayed:0"

/* M8 server features round 2 */
/* XEP-0186 Invisible Command */
#define NS_INVISIBLE "urn:xmpp:invisible:0"
/* XEP-0377 Spam Reporting */
#define NS_REPORTING "urn:xmpp:reporting:1"
#define JABBER_REPORTING_SPAM "urn:xmpp:reporting:spam"
#define JABBER_REPORTING_ABUSE "urn:xmpp:reporting:abuse"
/* XEP-0447 Stateless File Sharing, XEP-0446 File Metadata, XEP-0264
 * thumbnails (v1), XEP-0300 hashes, XEP-0103 URL data, XEP-0385 SIMS
 * (legacy, receive only) */
#define NS_SFS "urn:xmpp:sfs:0"
#define NS_FILE_METADATA "urn:xmpp:file:metadata:0"
#define NS_THUMBS_1 "urn:xmpp:thumbs:1"
#define NS_HASHES_2 "urn:xmpp:hashes:2"
#define NS_URL_DATA "http://jabber.org/protocol/url-data"
#define NS_REFERENCE "urn:xmpp:reference:0"
#define NS_SIMS "urn:xmpp:sims:1"
#define NS_JINGLE_FT_5 "urn:xmpp:jingle:apps:file-transfer:5"
/* XEP-0380 Explicit Message Encryption */
#define NS_EME "urn:xmpp:eme:0"
/* XEP-0319 Last User Interaction in Presence */
#define NS_IDLE "urn:xmpp:idle:1"

#endif /* PURPLE_JABBER_NAMESPACES_H_ */
