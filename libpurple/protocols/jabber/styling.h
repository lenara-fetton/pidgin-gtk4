/**
 * @file styling.h XEP-0393 Message Styling (outgoing)
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
#ifndef PURPLE_JABBER_STYLING_H_
#define PURPLE_JABBER_STYLING_H_

#include <glib.h>

/**
 * Converts libpurple markup (what UIs pass to send_im/send_chat) into the
 * plain-text body sent on the wire, with XEP-0393 markers for the
 * formatting that has one:
 *
 *   <b>, <strong>, font-weight:bold          -> *strong*
 *   <i>, <em>, font-style:italic             -> _emphasis_
 *   <s>, <strike>, <del>, line-through       -> ~strike~
 *   <code>, <tt>                             -> `code`
 *   <pre>                                    -> ``` fenced block ```
 *
 * Everything else is stripped with purple_markup_strip_html() semantics
 * (entities decoded, <br> and <p> become newlines, links become
 * "text (url)").  Markers are kept next to non-whitespace, as 0393
 * requires, and nested duplicates are merged.  A UI that already writes
 * 0393 markers itself only has to escape its plain text.
 */
char *jabber_styling_html_to_text(const char *html);

#endif /* PURPLE_JABBER_STYLING_H_ */
