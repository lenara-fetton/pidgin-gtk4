/**
 * @file oob.h out-of-band transfer functions
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
#ifndef PURPLE_JABBER_OOB_H_
#define PURPLE_JABBER_OOB_H_

#include "jabber.h"

void jabber_oob_parse(JabberStream *js, const char *from, JabberIqType type,
                      const char *id, xmlnode *querynode);

/**
 * Folds a message's <x xmlns='jabber:x:oob'/> (XEP-0066) into its HTML body.
 *
 * When the body already is the URL (or contains it), which is how XEP-0363
 * uploads and OMEMO aesgcm:// media are sent, the body is left unchanged, so
 * the UI gets the bare URL and can linkify or preview it. Otherwise a link to
 * the URL (with <desc> as its text, if given) is appended.
 *
 * @param body  The message body so far (escaped plain text or XHTML).
 * @param x     The jabber:x:oob element.
 */
void jabber_oob_x_append_to_body(GString *body, xmlnode *x);

#endif /* PURPLE_JABBER_OOB_H_ */
