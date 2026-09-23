/*
 * purple - Jabber Protocol Plugin
 *
 * XEP-0393 Message Styling: converts outgoing libpurple markup into a
 * plain body with styling markers (M8, doc/PIDGIN-UPGRADE.md).
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

#include "util.h"

#include "styling.h"

#include <string.h>

enum {
	STYLE_STRONG = 0,
	STYLE_EMPH,
	STYLE_STRIKE,
	STYLE_CODE,
	STYLE_PRE,
	STYLE_COUNT
};

static const char style_markers[] = { '*', '_', '~', '`' };

typedef struct {
	char *name;    /* lower-case element name */
	guint mask;    /* styles this element opened */
} OpenElement;

typedef struct {
	GString *out;
	int counts[STYLE_COUNT];
	guint pending;         /* opened, no visible text since */
	gboolean need_newline; /* after a closing ``` fence */
	GArray *stack;         /* OpenElement */
} Styler;

/*
 * The marked-up text still goes through purple_markup_strip_html(), which
 * folds literal newlines like HTML does; line breaks are written as <br>.
 */
#define BR "<br>"

/* Length of a <br>, <br/> or <br /> ending @str at @len, or 0. */
static gsize
trailing_br(const char *str, gsize len)
{
	static const char *const forms[] = { "<br>", "<br/>", "<br />", NULL };
	int i;

	for (i = 0; forms[i]; i++) {
		gsize n = strlen(forms[i]);
		if (len >= n && g_ascii_strncasecmp(str + len - n, forms[i], n) == 0)
			return n;
	}
	return 0;
}

static gboolean
at_line_start(const Styler *st)
{
	return st->out->len == 0 || trailing_br(st->out->str, st->out->len) > 0;
}

static gboolean
in_code(const Styler *st)
{
	return st->counts[STYLE_CODE] > 0 || st->counts[STYLE_PRE] > 0;
}

static void
flush_pending(Styler *st)
{
	int k;

	if (st->need_newline) {
		g_string_append(st->out, BR);
		st->need_newline = FALSE;
	}

	for (k = 0; k < STYLE_PRE; k++) {
		if (st->pending & (1u << k)) {
			g_string_append_c(st->out, style_markers[k]);
		}
	}
	st->pending = 0;
}

static void
close_marker(Styler *st, int k)
{
	gsize pos;

	if (st->pending & (1u << k)) {
		/* Nothing visible in between: drop both markers. */
		st->pending &= ~(1u << k);
		return;
	}

	/* 0393: the closing marker must follow a non-whitespace character
	 * (and stay on the line it opened on). */
	pos = st->out->len;
	for (;;) {
		gsize br;

		if (pos > 0 && g_ascii_isspace(st->out->str[pos - 1]))
			pos--;
		else if ((br = trailing_br(st->out->str, pos)) > 0)
			pos -= br;
		else
			break;
	}
	g_string_insert_c(st->out, pos, style_markers[k]);
}

static void
open_style(Styler *st, int k)
{
	st->counts[k]++;
	if (st->counts[k] != 1)
		return;

	if (k == STYLE_PRE) {
		st->pending = 0;
		st->need_newline = FALSE;
		if (!at_line_start(st))
			g_string_append(st->out, BR);
		g_string_append(st->out, "```" BR);
	} else {
		st->pending |= 1u << k;
	}
}

static void
close_style(Styler *st, int k)
{
	if (st->counts[k] <= 0)
		return;
	st->counts[k]--;
	if (st->counts[k] != 0)
		return;

	if (k == STYLE_PRE) {
		if (!at_line_start(st))
			g_string_append(st->out, BR);
		g_string_append(st->out, "```");
		st->need_newline = TRUE;
	} else {
		close_marker(st, k);
	}
}

/* Styles a <span style=''> asks for. */
static guint
span_styles(const char *tag, gsize len)
{
	char *lower = g_ascii_strdown(tag, len);
	guint mask = 0;
	char *style = strstr(lower, "style=");

	if (style) {
		if (strstr(style, "font-weight: bold") || strstr(style, "font-weight:bold") ||
		    strstr(style, "font-weight: 700") || strstr(style, "font-weight:700"))
			mask |= 1u << STYLE_STRONG;
		if (strstr(style, "font-style: italic") || strstr(style, "font-style:italic"))
			mask |= 1u << STYLE_EMPH;
		if (strstr(style, "line-through"))
			mask |= 1u << STYLE_STRIKE;
	}

	g_free(lower);
	return mask;
}

/* The styles an opening element maps to; 0 if it is passed through. */
static gboolean
element_styles(const char *name, const char *tag, gsize len, guint *mask)
{
	if (purple_strequal(name, "b") || purple_strequal(name, "strong"))
		*mask = 1u << STYLE_STRONG;
	else if (purple_strequal(name, "i") || purple_strequal(name, "em"))
		*mask = 1u << STYLE_EMPH;
	else if (purple_strequal(name, "s") || purple_strequal(name, "strike") ||
	         purple_strequal(name, "del"))
		*mask = 1u << STYLE_STRIKE;
	else if (purple_strequal(name, "code") || purple_strequal(name, "tt"))
		*mask = 1u << STYLE_CODE;
	else if (purple_strequal(name, "pre"))
		*mask = 1u << STYLE_PRE;
	else if (purple_strequal(name, "span"))
		*mask = span_styles(tag, len);
	else
		return FALSE;

	return TRUE;
}

static void
handle_open(Styler *st, const char *name, const char *tag, gsize len)
{
	OpenElement el;
	guint mask = 0;
	int k;

	if (!element_styles(name, tag, len, &mask)) {
		g_string_append_len(st->out, tag, len);
		return;
	}

	/* Inside code, styling elements are ignored (0393 doesn't nest). */
	if (in_code(st) && !(mask & (1u << STYLE_PRE)) && !(mask & (1u << STYLE_CODE)))
		mask = 0;

	el.name = g_strdup(name);
	el.mask = 0;
	for (k = 0; k < STYLE_COUNT; k++) {
		if (mask & (1u << k)) {
			/* Code inside pre stays literal. */
			if (k == STYLE_CODE && st->counts[STYLE_PRE] > 0)
				continue;
			open_style(st, k);
			el.mask |= 1u << k;
		}
	}
	g_array_append_val(st->stack, el);
}

static void
handle_close(Styler *st, const char *name, const char *tag, gsize len)
{
	gint i;
	guint dummy;

	if (!element_styles(name, tag, len, &dummy)) {
		g_string_append_len(st->out, tag, len);
		return;
	}

	for (i = (gint)st->stack->len - 1; i >= 0; i--) {
		OpenElement *el = &g_array_index(st->stack, OpenElement, i);

		if (purple_strequal(el->name, name)) {
			int k;

			for (k = STYLE_COUNT - 1; k >= 0; k--)
				if (el->mask & (1u << k))
					close_style(st, k);
			g_free(el->name);
			g_array_remove_index(st->stack, i);
			return;
		}
	}
}

char *
jabber_styling_html_to_text(const char *html)
{
	Styler st;
	const char *p;
	char *marked, *plain;
	int k;

	if (html == NULL)
		return NULL;

	memset(&st, 0, sizeof(st));
	st.out = g_string_sized_new(strlen(html) + 8);
	st.stack = g_array_new(FALSE, FALSE, sizeof(OpenElement));

	for (p = html; *p; ) {
		if (*p == '<') {
			const char *end = strchr(p, '>');
			const char *n;
			gboolean closing;
			char *name;
			gsize len;

			if (end == NULL) {
				/* Not a tag: strip_html treats it as text. */
				flush_pending(&st);
				g_string_append(st.out, p);
				break;
			}
			len = end - p + 1;
			n = p + 1;
			closing = (*n == '/');
			if (closing)
				n++;
			{
				const char *e = n;
				while (*e && (g_ascii_isalnum(*e)))
					e++;
				name = g_ascii_strdown(n, e - n);
			}

			if (*name == '\0' || end[-1] == '/') {
				/* <br/>, comments, self-closed styling elements */
				guint unused = 0;

				if (purple_strequal(name, "br") && st.need_newline)
					st.need_newline = FALSE;
				/* <b/> etc. style nothing; the rest is passed through */
				if (*name == '\0' || !element_styles(name, p, len, &unused))
					g_string_append_len(st.out, p, len);
			} else if (closing) {
				handle_close(&st, name, p, len);
			} else {
				if (purple_strequal(name, "br") && st.need_newline)
					st.need_newline = FALSE;
				handle_open(&st, name, p, len);
			}

			g_free(name);
			p = end + 1;
		} else {
			const char *next = g_utf8_next_char(p);

			if (!g_ascii_isspace(*p))
				flush_pending(&st);
			g_string_append_len(st.out, p, next - p);
			p = next;
		}
	}

	/* Unclosed elements: close them at the end. */
	while (st.stack->len) {
		OpenElement *el = &g_array_index(st.stack, OpenElement,
		                                 st.stack->len - 1);
		for (k = STYLE_COUNT - 1; k >= 0; k--)
			if (el->mask & (1u << k))
				close_style(&st, k);
		g_free(el->name);
		g_array_remove_index(st.stack, st.stack->len - 1);
	}
	g_array_free(st.stack, TRUE);

	marked = g_string_free(st.out, FALSE);
	plain = purple_markup_strip_html(marked);
	g_free(marked);

	return plain;
}
