/*
 * pidgin
 *
 * Pidgin is the legal property of its developers, whose names are too numerous
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
#include "pidgin-internal.h"
#include "pidgin.h"

#include <math.h>

#include "account.h"
#include "connection.h"
#include "debug.h"
#include "prpl.h"
#include "util.h"

#include "gtkutils.h"
#include "pidginmarkup.h"
#include "pidginsmileytheme.h"

/* GtkIMHtml's font scale for HTML sizes 1..7 (gtkimhtml.c POINT_SIZE). */
static const double size_scales[] = { .85, .95, 1, 1.2, 1.44, 1.728, 2.0736 };
/* The classic HTML size -> point size table (USE_POINTSIZE protocols). */
static const int size_points[] = { 8, 10, 12, 14, 18, 24, 36 };

#define MIN_POINTS 4
#define MAX_POINTS 96
#define HR_TEXT "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80" \
                "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
#define DIM_ALPHA 32768   /* directives and comments: 50% */

double
pidgin_markup_size_scale(int size)
{
	return size_scales[CLAMP(size, 1, 7) - 1];
}

int
pidgin_markup_size_to_points(int size)
{
	return size_points[CLAMP(size, 1, 7) - 1];
}

int
pidgin_markup_points_to_size(int points)
{
	int i, best = 3, best_diff = G_MAXINT;

	for (i = 0; i < 7; i++) {
		int diff = ABS(size_points[i] - points);
		if (diff < best_diff) {
			best_diff = diff;
			best = i + 1;
		}
	}
	return best;
}

/**************************************************************************
 * Results
 **************************************************************************/

static void
object_free(PidginMarkupObject *obj)
{
	g_free(obj->uri);
	g_free(obj->alt);
	g_free(obj->sml);
	g_free(obj);
}

static PidginMarkupResult *
result_new(void)
{
	PidginMarkupResult *result = g_new0(PidginMarkupResult, 1);

	result->ref_count = 1;
	result->attrs = pango_attr_list_new();
	result->objects = g_ptr_array_new_with_free_func((GDestroyNotify)object_free);
	return result;
}

PidginMarkupResult *
pidgin_markup_result_ref(PidginMarkupResult *result)
{
	g_return_val_if_fail(result != NULL, NULL);
	g_atomic_int_inc(&result->ref_count);
	return result;
}

void
pidgin_markup_result_unref(PidginMarkupResult *result)
{
	if (result == NULL || !g_atomic_int_dec_and_test(&result->ref_count))
		return;
	g_free(result->text);
	pango_attr_list_unref(result->attrs);
	g_ptr_array_unref(result->objects);
	g_free(result);
}

G_DEFINE_BOXED_TYPE(PidginMarkupResult, pidgin_markup_result,
                    pidgin_markup_result_ref, pidgin_markup_result_unref)

gboolean
pidgin_markup_result_has_object(const PidginMarkupResult *result,
                                PidginMarkupObjectType type)
{
	guint i;

	for (i = 0; i < result->objects->len; i++) {
		PidginMarkupObject *obj = g_ptr_array_index(result->objects, i);
		if (obj->type == type)
			return TRUE;
	}
	return FALSE;
}

gboolean
pidgin_markup_result_has_graphics(const PidginMarkupResult *result)
{
	return pidgin_markup_result_has_object(result, PIDGIN_MARKUP_OBJECT_IMAGE) ||
	       pidgin_markup_result_has_object(result, PIDGIN_MARKUP_OBJECT_REMOTE_IMAGE) ||
	       pidgin_markup_result_has_object(result, PIDGIN_MARKUP_OBJECT_SMILEY) ||
	       pidgin_markup_result_has_object(result, PIDGIN_MARKUP_OBJECT_HR);
}

static PidginMarkupObject *
add_object(PidginMarkupResult *result, PidginMarkupObjectType type,
           guint start, guint end)
{
	PidginMarkupObject *obj = g_new0(PidginMarkupObject, 1);

	obj->type = type;
	obj->start = start;
	obj->end = end;
	g_ptr_array_add(result->objects, obj);
	return obj;
}

static gint
object_cmp(gconstpointer a, gconstpointer b)
{
	const PidginMarkupObject *oa = *(PidginMarkupObject * const *)a;
	const PidginMarkupObject *ob = *(PidginMarkupObject * const *)b;

	if (oa->start != ob->start)
		return oa->start < ob->start ? -1 : 1;
	/* outer (longer) first */
	if (oa->end != ob->end)
		return oa->end > ob->end ? -1 : 1;
	return 0;
}

/**************************************************************************
 * Colours
 **************************************************************************/

gboolean
pidgin_markup_parse_color(const char *str, GdkRGBA *rgba)
{
	char *s, *hex;
	gboolean ok;
	size_t len;

	if (str == NULL)
		return FALSE;

	s = g_strstrip(g_strdup(str));
	len = strlen(s);
	if (len == 0) {
		g_free(s);
		return FALSE;
	}

	/* purple HTML often has bare hex ("ff0000") */
	if (s[0] != '#' && (len == 6 || len == 3)) {
		size_t i;
		for (i = 0; i < len && g_ascii_isxdigit(s[i]); i++)
			;
		if (i == len) {
			hex = g_strconcat("#", s, NULL);
			g_free(s);
			s = hex;
		}
	}

	ok = gdk_rgba_parse(rgba, s);
	g_free(s);
	return ok;
}

static char *
color_to_hex(const GdkRGBA *rgba)
{
	return g_strdup_printf("#%02x%02x%02x",
	                       (int)(CLAMP(rgba->red, 0., 1.) * 255 + .5),
	                       (int)(CLAMP(rgba->green, 0., 1.) * 255 + .5),
	                       (int)(CLAMP(rgba->blue, 0., 1.) * 255 + .5));
}

static guint16
color16(double c)
{
	return (guint16)(CLAMP(c, 0., 1.) * 65535 + .5);
}

static gboolean
is_black(const GdkRGBA *c)
{
	return c->red < .01 && c->green < .01 && c->blue < .01 && c->alpha > .5;
}

/**************************************************************************
 * Link schemes
 **************************************************************************/

typedef struct
{
	char *scheme;
	PidginMarkupLinkActivateFunc activate;
	PidginMarkupLinkMenuFunc menu;
	gpointer data;
} LinkScheme;

static GList *schemes = NULL;

static void
scheme_free(LinkScheme *s)
{
	g_free(s->scheme);
	g_free(s);
}

gboolean
pidgin_markup_register_scheme(const char *scheme,
                              PidginMarkupLinkActivateFunc activate,
                              PidginMarkupLinkMenuFunc context_menu,
                              gpointer data)
{
	GList *l;
	LinkScheme *s;

	g_return_val_if_fail(scheme != NULL && *scheme != '\0', FALSE);

	for (l = schemes; l; l = l->next) {
		s = l->data;
		if (!g_ascii_strcasecmp(s->scheme, scheme)) {
			if (activate == NULL) {
				schemes = g_list_delete_link(schemes, l);
				scheme_free(s);
			} else {
				s->activate = activate;
				s->menu = context_menu;
				s->data = data;
			}
			return TRUE;
		}
	}
	if (activate == NULL)
		return FALSE;

	s = g_new0(LinkScheme, 1);
	s->scheme = g_strdup(scheme);
	s->activate = activate;
	s->menu = context_menu;
	s->data = data;
	schemes = g_list_prepend(schemes, s);
	return TRUE;
}

static LinkScheme *
find_scheme(const char *uri)
{
	GList *l;
	LinkScheme *best = NULL;
	size_t best_len = 0;

	if (uri == NULL)
		return NULL;
	for (l = schemes; l; l = l->next) {
		LinkScheme *s = l->data;
		size_t len = strlen(s->scheme);
		if (len > best_len && !g_ascii_strncasecmp(uri, s->scheme, len)) {
			best = s;
			best_len = len;
		}
	}
	return best;
}

gboolean
pidgin_markup_uri_is_known(const char *uri)
{
	return find_scheme(uri) != NULL;
}

gboolean
pidgin_markup_activate_uri(GtkWidget *widget, const char *uri)
{
	LinkScheme *s = find_scheme(uri);
	GtkWindow *parent = NULL;

	if (s != NULL && s->activate(widget, uri, s->data))
		return TRUE;

	if (widget != NULL && GTK_IS_WINDOW(gtk_widget_get_root(widget)))
		parent = GTK_WINDOW(gtk_widget_get_root(widget));

	/* Unknown schemes: only what looks like a URI with a scheme goes to
	 * the desktop (never a bare path). */
	if (uri != NULL && g_uri_peek_scheme(uri) != NULL) {
		pidgin_open_uri(parent, uri);
		return TRUE;
	}
	return FALSE;
}

static void
copy_to_clipboard(GtkWidget *widget, const char *text)
{
	GdkClipboard *clipboard;

	if (widget != NULL)
		clipboard = gtk_widget_get_clipboard(widget);
	else
		clipboard = gdk_display_get_clipboard(gdk_display_get_default());
	gdk_clipboard_set_text(clipboard, text);
}

typedef struct
{
	GtkWidget *widget;      /* weak */
	char *uri;
} LinkAction;

static void
link_action_free(gpointer data, GClosure *closure)
{
	LinkAction *la = data;

	g_clear_weak_pointer(&la->widget);
	g_free(la->uri);
	g_free(la);
}

static void
link_open_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	LinkAction *la = data;

	pidgin_markup_activate_uri(la->widget, la->uri);
}

static void
link_copy_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	LinkAction *la = data;
	const char *text = la->uri;

	if (!g_ascii_strncasecmp(text, "mailto:", 7))
		text += 7;
	copy_to_clipboard(la->widget, text);
}

static void
add_link_action(GActionMap *actions, const char *name, GCallback cb,
                GtkWidget *widget, const char *uri)
{
	GSimpleAction *action = g_simple_action_new(name, NULL);
	LinkAction *la = g_new0(LinkAction, 1);

	g_set_weak_pointer(&la->widget, widget);
	la->uri = g_strdup(uri);
	g_signal_connect_data(action, "activate", cb, la, link_action_free, 0);
	g_action_map_add_action(actions, G_ACTION(action));
	g_object_unref(action);
}

static gboolean
default_link_menu(GtkWidget *widget, const char *uri, GMenu *menu,
                  GActionMap *actions, gpointer data)
{
	gboolean mail = !g_ascii_strncasecmp(uri, "mailto:", 7);

	add_link_action(actions, "open", G_CALLBACK(link_open_cb), widget, uri);
	add_link_action(actions, "copy", G_CALLBACK(link_copy_cb), widget, uri);
	g_menu_append(menu, mail ? _("_Send Email") : _("_Open Link"), "link.open");
	g_menu_append(menu, mail ? _("_Copy Email Address") : _("_Copy Link Location"),
	              "link.copy");
	return TRUE;
}

gboolean
pidgin_markup_populate_link_menu(GtkWidget *widget, const char *uri,
                                 GMenu *menu, GActionMap *actions)
{
	LinkScheme *s;

	g_return_val_if_fail(uri != NULL, FALSE);

	s = find_scheme(uri);
	if (s != NULL && s->menu != NULL)
		return s->menu(widget, uri, menu, actions, s->data);
	return default_link_menu(widget, uri, menu, actions, NULL);
}

static gboolean
default_link_activate(GtkWidget *widget, const char *uri, gpointer data)
{
	GtkWindow *parent = NULL;

	if (widget != NULL && GTK_IS_WINDOW(gtk_widget_get_root(widget)))
		parent = GTK_WINDOW(gtk_widget_get_root(widget));
	pidgin_open_uri(parent, uri);
	return TRUE;
}

void
pidgin_markup_init(void)
{
	static const char *builtin[] = {
		"http://", "https://", "ftp://", "mailto:", "xmpp:", "irc://",
		"ircs://", NULL
	};
	int i;

	for (i = 0; builtin[i]; i++) {
		if (find_scheme(builtin[i]) == NULL)
			pidgin_markup_register_scheme(builtin[i], default_link_activate,
			                              default_link_menu, NULL);
	}
}

void
pidgin_markup_uninit(void)
{
	g_list_free_full(schemes, (GDestroyNotify)scheme_free);
	schemes = NULL;
}

/*
 * Length of a URI scheme prefix at @c that starts a bare link, as
 * gtk_imhtml_is_protocol() did: the registered schemes and "www.".
 */
static gsize
link_prefix_len(const char *c)
{
	LinkScheme *s;

	if (!g_ascii_strncasecmp(c, "www.", 4))
		return 4;
	s = find_scheme(c);
	return s ? strlen(s->scheme) : 0;
}

/**************************************************************************
 * The HTML parser
 **************************************************************************/

typedef struct
{
	gboolean bold, italic, underline, strike, sub, sup, mono, dim;
	const char *face;       /* interned */
	gboolean fore_set, back_set;
	GdkRGBA fore, back;
	double scale;           /* 1.0 = none */
	int points;             /* absolute size, 0 = none */
} RunStyle;

typedef struct
{
	const char *face;       /* interned */
	gboolean fore_set, back_set;
	GdkRGBA fore, back;
	int size;               /* 1..7 */
	double extra_scale;     /* CSS %/em */
	int points;
	const char *sml;        /* interned */
	/* SPAN style properties */
	gboolean bold, italic, underline, strike;
	gboolean spoiler;
	guint spoiler_start;
} FontFrame;

typedef struct
{
	const PidginMarkupOptions *opts;
	PidginMarkupFlags flags;
	PidginMarkupResult *result;
	GString *text;

	int bold, italic, underline, strike, sub, sup, mono, pre, title, dim;
	GSList *fonts;          /* FontFrame* */
	GSList *quotes;         /* PidginMarkupObject* open blockquotes */

	PidginMarkupObject *link;
	gboolean after_br;

	RunStyle run;
	guint run_start;
} Parser;

static void
style_current(Parser *p, RunStyle *st)
{
	FontFrame *f = p->fonts ? p->fonts->data : NULL;

	memset(st, 0, sizeof(*st));
	st->bold = p->bold > 0 || (f && f->bold);
	st->italic = p->italic > 0 || (f && f->italic);
	st->underline = p->underline > 0 || (f && f->underline);
	st->strike = p->strike > 0 || (f && f->strike);
	st->sub = p->sub > 0;
	st->sup = p->sup > 0 && !st->sub;
	st->mono = p->mono > 0;
	st->dim = p->dim > 0;
	st->scale = 1.0;

	if (f != NULL) {
		st->face = f->face;
		st->fore_set = f->fore_set;
		st->fore = f->fore;
		st->back_set = f->back_set;
		st->back = f->back;
		if (f->points > 0)
			st->points = f->points;
		else
			st->scale = pidgin_markup_size_scale(f->size) * f->extra_scale;
	}
	if (st->mono)
		st->face = g_intern_static_string("monospace");
}

static gboolean
style_equal(const RunStyle *a, const RunStyle *b)
{
	return a->bold == b->bold && a->italic == b->italic &&
	       a->underline == b->underline && a->strike == b->strike &&
	       a->sub == b->sub && a->sup == b->sup && a->mono == b->mono &&
	       a->dim == b->dim && a->face == b->face &&
	       a->fore_set == b->fore_set && a->back_set == b->back_set &&
	       (!a->fore_set || gdk_rgba_equal(&a->fore, &b->fore)) &&
	       (!a->back_set || gdk_rgba_equal(&a->back, &b->back)) &&
	       a->scale == b->scale && a->points == b->points;
}

static void
attr_add(PangoAttrList *list, PangoAttribute *attr, guint start, guint end)
{
	attr->start_index = start;
	attr->end_index = end;
	pango_attr_list_change(list, attr);
}

static void
emit_style(PangoAttrList *attrs, const RunStyle *st, guint start, guint end)
{
	if (start >= end)
		return;

	if (st->bold)
		attr_add(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD), start, end);
	if (st->italic)
		attr_add(attrs, pango_attr_style_new(PANGO_STYLE_ITALIC), start, end);
	if (st->underline)
		attr_add(attrs, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE), start, end);
	if (st->strike)
		attr_add(attrs, pango_attr_strikethrough_new(TRUE), start, end);
	if (st->sub || st->sup) {
		attr_add(attrs, pango_attr_baseline_shift_new(st->sub ?
		         PANGO_BASELINE_SHIFT_SUBSCRIPT : PANGO_BASELINE_SHIFT_SUPERSCRIPT),
		         start, end);
		attr_add(attrs, pango_attr_font_scale_new(st->sub ?
		         PANGO_FONT_SCALE_SUBSCRIPT : PANGO_FONT_SCALE_SUPERSCRIPT),
		         start, end);
	}
	if (st->face)
		attr_add(attrs, pango_attr_family_new(st->face), start, end);
	if (st->fore_set)
		attr_add(attrs, pango_attr_foreground_new(color16(st->fore.red),
		         color16(st->fore.green), color16(st->fore.blue)), start, end);
	if (st->back_set)
		attr_add(attrs, pango_attr_background_new(color16(st->back.red),
		         color16(st->back.green), color16(st->back.blue)), start, end);
	if (st->dim)
		attr_add(attrs, pango_attr_foreground_alpha_new(DIM_ALPHA), start, end);
	if (st->points > 0)
		attr_add(attrs, pango_attr_size_new(st->points * PANGO_SCALE), start, end);
	else if (st->scale != 1.0)
		attr_add(attrs, pango_attr_scale_new(st->scale), start, end);
}

static void
flush_run(Parser *p)
{
	emit_style(p->result->attrs, &p->run, p->run_start, p->text->len);
	p->run_start = p->text->len;
}

/* Called before any text is appended: starts a new run if the style
 * changed. */
static void
sync_style(Parser *p)
{
	RunStyle st;

	style_current(p, &st);
	if (!style_equal(&st, &p->run)) {
		flush_run(p);
		p->run = st;
	}
}

static void
append_text(Parser *p, const char *s, gssize len)
{
	if (len < 0)
		len = strlen(s);
	if (len == 0)
		return;
	if (p->title > 0)
		return;         /* <title> content is not shown */
	sync_style(p);
	g_string_append_len(p->text, s, len);
	p->after_br = FALSE;
}

static void
ensure_newline(Parser *p)
{
	if (p->text->len > 0 && p->text->str[p->text->len - 1] != '\n') {
		append_text(p, "\n", 1);
		p->after_br = TRUE;
	}
}

/* ---- tags ---- */

typedef struct
{
	char name[16];          /* lower case */
	gboolean closing;
	gboolean self_closing;
	const char *attrs;      /* after the name, up to end */
	const char *attrs_end;
	const char *end;        /* past the '>' */
} Tag;

static const char *known_tags[] = {
	"a", "b", "bold", "strong", "i", "italic", "em", "u", "underline",
	"s", "strike", "del", "sub", "sup", "pre", "code", "tt", "title", "br",
	"hr", "p", "font", "span", "img", "html", "head", "body", "cite",
	"binary", "h1", "h2", "h3", "h4", "h5", "h6", "div", "blockquote",
	"ul", "ol", "li", "q", "small", "big", "meta", "style", NULL
};

static gboolean
is_known_tag(const char *name)
{
	int i;

	for (i = 0; known_tags[i]; i++)
		if (!strcmp(known_tags[i], name))
			return TRUE;
	return FALSE;
}

/*
 * Recognises a tag at @c (pointing at '<'), like gtk_imhtml_is_tag(): a
 * known name right after '<' or '</', then whitespace, '/' or '>', and a
 * closing '>' (quotes respected). Anything else is literal text.
 */
static gboolean
parse_tag(const char *c, Tag *tag)
{
	const char *n = c + 1, *e;
	size_t len = 0;
	char quote = 0;

	memset(tag, 0, sizeof(*tag));
	if (*n == '/') {
		tag->closing = TRUE;
		n++;
	}
	while (g_ascii_isalnum(n[len]) && len < sizeof(tag->name) - 1) {
		tag->name[len] = g_ascii_tolower(n[len]);
		len++;
	}
	tag->name[len] = '\0';
	if (len == 0 || !is_known_tag(tag->name))
		return FALSE;
	if (n[len] != '>' && n[len] != '/' && !g_ascii_isspace(n[len]))
		return FALSE;

	tag->attrs = n + len;
	for (e = tag->attrs; *e; e++) {
		if (quote) {
			if (*e == quote)
				quote = 0;
		} else if (*e == '"' || *e == '\'') {
			/* only quotes that start an attribute value */
			if (e > tag->attrs && e[-1] == '=')
				quote = *e;
		} else if (*e == '>') {
			break;
		} else if (*e == '<') {
			return FALSE;
		}
	}
	if (*e != '>')
		return FALSE;

	tag->attrs_end = e;
	tag->end = e + 1;
	if (e > tag->attrs && e[-1] == '/')
		tag->self_closing = TRUE;
	return TRUE;
}

/* Returns the unescaped value of attribute @name (case-insensitive). */
static char *
tag_get_attr(const Tag *tag, const char *name)
{
	const char *c = tag->attrs, *end = tag->attrs_end;
	size_t nlen = strlen(name);

	while (c < end) {
		const char *an, *vs, *ve;
		size_t alen;

		while (c < end && (g_ascii_isspace(*c) || *c == '/'))
			c++;
		an = c;
		while (c < end && *c != '=' && !g_ascii_isspace(*c) && *c != '/')
			c++;
		alen = c - an;
		while (c < end && g_ascii_isspace(*c))
			c++;
		if (c >= end || *c != '=') {
			if (alen == 0)
				c++;
			continue;
		}
		c++;
		while (c < end && g_ascii_isspace(*c))
			c++;
		if (c < end && (*c == '"' || *c == '\'')) {
			char q = *c++;
			vs = c;
			while (c < end && *c != q)
				c++;
			ve = c;
			if (c < end)
				c++;
		} else {
			vs = c;
			while (c < end && !g_ascii_isspace(*c))
				c++;
			ve = c;
			if (ve > vs && ve == end && ve[-1] == '/')
				ve--;
		}
		if (alen == nlen && !g_ascii_strncasecmp(an, name, nlen)) {
			char *raw = g_strndup(vs, ve - vs);
			char *value = purple_unescape_html(raw);
			g_free(raw);
			return value;
		}
	}
	return NULL;
}

static FontFrame *
push_font(Parser *p)
{
	FontFrame *f = g_new0(FontFrame, 1);
	FontFrame *old = p->fonts ? p->fonts->data : NULL;

	if (old != NULL) {
		*f = *old;
		f->spoiler = FALSE;
	} else {
		f->size = 3;
		f->extra_scale = 1.0;
	}
	p->fonts = g_slist_prepend(p->fonts, f);
	return f;
}

static void
pop_font(Parser *p)
{
	FontFrame *f;

	if (p->fonts == NULL)
		return;
	f = p->fonts->data;
	if (f->spoiler && p->text->len > f->spoiler_start)
		add_object(p->result, PIDGIN_MARKUP_OBJECT_SPOILER, f->spoiler_start,
		           p->text->len);
	p->fonts = g_slist_delete_link(p->fonts, p->fonts);
	g_free(f);
}

static void
font_set_size(Parser *p, FontFrame *f, const char *size)
{
	int n;

	if (size == NULL || (p->flags & PIDGIN_MARKUP_NO_SIZES))
		return;

	if (p->flags & PIDGIN_MARKUP_USE_POINTSIZE) {
		n = atoi(size);
		if (n > 0) {
			f->points = CLAMP(n, MIN_POINTS, MAX_POINTS);
			f->extra_scale = 1.0;
		}
		return;
	}

	if (*size == '+') {
		n = atoi(size + 1) + 3;
	} else if (*size == '-') {
		n = MAX(0, 3 - atoi(size + 1));
	} else if (g_ascii_isdigit(*size)) {
		n = atoi(size);
	} else {
		return;
	}
	f->size = CLAMP(n, 1, 7);
	f->points = 0;
	f->extra_scale = 1.0;
}

static void
css_set_size(Parser *p, FontFrame *f, const char *size)
{
	static const struct { const char *name; int size; } names[] = {
		{ "xx-small", 1 }, { "x-small", 2 }, { "smaller", 2 }, { "small", 2 },
		{ "medium", 3 }, { "large", 4 }, { "larger", 4 }, { "x-large", 5 },
		{ "xx-large", 6 }, { "xxx-large", 7 }, { NULL, 0 }
	};
	char *end = NULL;
	double v;
	int i;

	if (p->flags & PIDGIN_MARKUP_NO_SIZES)
		return;

	for (i = 0; names[i].name; i++) {
		if (!g_ascii_strcasecmp(size, names[i].name)) {
			f->size = names[i].size;
			f->points = 0;
			f->extra_scale = 1.0;
			return;
		}
	}

	v = g_ascii_strtod(size, &end);
	if (end == size || v <= 0)
		return;
	while (*end == ' ')
		end++;
	if (!g_ascii_strcasecmp(end, "pt")) {
		f->points = CLAMP((int)(v + .5), MIN_POINTS, MAX_POINTS);
	} else if (!g_ascii_strcasecmp(end, "px")) {
		f->points = CLAMP((int)(v * .75 + .5), MIN_POINTS, MAX_POINTS);
	} else if (!strcmp(end, "%")) {
		f->points = 0;
		f->extra_scale = CLAMP(v / 100., .5, 4.);
	} else if (!g_ascii_strcasecmp(end, "em")) {
		f->points = 0;
		f->extra_scale = CLAMP(v, .5, 4.);
	}
}

static char *
css_prop(const char *style, const char *name)
{
	char *v = purple_markup_get_css_property(style, name);

	if (v != NULL) {
		g_strstrip(v);
		if (*v == '\0') {
			g_free(v);
			v = NULL;
		}
	}
	return v;
}

static void
handle_span(Parser *p, const Tag *tag)
{
	FontFrame *f = push_font(p);
	char *style = tag_get_attr(tag, "style");
	char *color, *background, *family, *size, *textdec, *weight, *fstyle;
	GdkRGBA fg, bg;
	gboolean fg_ok, bg_ok;

	if (style == NULL)
		return;

	color = css_prop(style, "color");
	if (color == NULL)
		color = css_prop(style, "foreground");
	background = css_prop(style, "background-color");
	if (background == NULL)
		background = css_prop(style, "background");
	family = css_prop(style, "font-family");
	size = css_prop(style, "font-size");
	textdec = css_prop(style, "text-decoration");
	weight = css_prop(style, "font-weight");
	fstyle = css_prop(style, "font-style");

	fg_ok = pidgin_markup_parse_color(color, &fg);
	bg_ok = pidgin_markup_parse_color(background, &bg);

	/* Discord spoilers: black on black. */
	if (fg_ok && bg_ok && is_black(&fg) && is_black(&bg)) {
		f->spoiler = TRUE;
		f->spoiler_start = p->text->len;
	} else if (!(p->flags & PIDGIN_MARKUP_NO_COLOURS)) {
		if (fg_ok) {
			f->fore = fg;
			f->fore_set = TRUE;
		}
		if (bg_ok) {
			f->back = bg;
			f->back_set = TRUE;
		}
	}

	if (family && !(p->flags & PIDGIN_MARKUP_NO_FONTS)) {
		char *first = g_strstrip(g_strdelimit(family, "\"'", ' '));
		char *comma = strchr(first, ',');
		if (comma)
			*comma = '\0';
		g_strstrip(first);
		if (*first)
			f->face = g_intern_string(first);
	}

	if (size)
		css_set_size(p, f, size);

	if (!(p->flags & PIDGIN_MARKUP_NO_FORMATTING)) {
		if (textdec) {
			if (strstr(textdec, "underline"))
				f->underline = TRUE;
			if (strstr(textdec, "line-through"))
				f->strike = TRUE;
			if (!g_ascii_strcasecmp(textdec, "none"))
				f->underline = f->strike = FALSE;
		}
		if (weight) {
			if (!g_ascii_strcasecmp(weight, "bold") ||
			    !g_ascii_strcasecmp(weight, "bolder") || atoi(weight) >= 600)
				f->bold = TRUE;
			else if (!g_ascii_strcasecmp(weight, "normal") ||
			         !g_ascii_strcasecmp(weight, "lighter") ||
			         (atoi(weight) > 0 && atoi(weight) < 600))
				f->bold = FALSE;
		}
		if (fstyle) {
			if (!g_ascii_strcasecmp(fstyle, "italic") ||
			    !g_ascii_strcasecmp(fstyle, "oblique"))
				f->italic = TRUE;
			else if (!g_ascii_strcasecmp(fstyle, "normal"))
				f->italic = FALSE;
		}
	}

	g_free(color);
	g_free(background);
	g_free(family);
	g_free(size);
	g_free(textdec);
	g_free(weight);
	g_free(fstyle);
	g_free(style);
}

static void
handle_font(Parser *p, const Tag *tag)
{
	FontFrame *f = push_font(p);
	char *color = tag_get_attr(tag, "color");
	char *back = tag_get_attr(tag, "back");
	char *face = tag_get_attr(tag, "face");
	char *size = tag_get_attr(tag, "size");
	char *absz = tag_get_attr(tag, "absz");
	char *sml = tag_get_attr(tag, "sml");
	GdkRGBA rgba;

	if (!(p->flags & PIDGIN_MARKUP_NO_COLOURS)) {
		if (pidgin_markup_parse_color(color, &rgba)) {
			f->fore = rgba;
			f->fore_set = TRUE;
		}
		if (pidgin_markup_parse_color(back, &rgba)) {
			f->back = rgba;
			f->back_set = TRUE;
		}
	}
	if (face && *face && !(p->flags & PIDGIN_MARKUP_NO_FONTS))
		f->face = g_intern_string(face);
	if (sml && *sml)
		f->sml = g_intern_string(sml);

	if (absz && atoi(absz) > 0 && !(p->flags & PIDGIN_MARKUP_NO_SIZES)) {
		f->points = CLAMP(atoi(absz), MIN_POINTS, MAX_POINTS);
		f->extra_scale = 1.0;
	} else {
		font_set_size(p, f, size);
	}

	g_free(color);
	g_free(back);
	g_free(face);
	g_free(size);
	g_free(absz);
	g_free(sml);
}

static void
link_close(Parser *p)
{
	if (p->link == NULL)
		return;
	p->link->end = p->text->len;
	if (p->link->end <= p->link->start) {
		g_ptr_array_remove(p->result->objects, p->link);
	}
	p->link = NULL;
}

static void
link_open(Parser *p, const char *href)
{
	link_close(p);
	p->link = add_object(p->result, PIDGIN_MARKUP_OBJECT_LINK,
	                     p->text->len, p->text->len);
	p->link->uri = g_strdup(href);
}

static void
handle_img(Parser *p, const Tag *tag)
{
	char *id = tag_get_attr(tag, "id");
	char *src = tag_get_attr(tag, "src");
	char *alt = tag_get_attr(tag, "alt");
	char *width = tag_get_attr(tag, "width");
	char *height = tag_get_attr(tag, "height");
	PidginMarkupObject *obj;
	guint start;

	if (id != NULL && atoi(id) > 0) {
		if (!(p->flags & PIDGIN_MARKUP_NO_IMAGES)) {
			const char *fallback = (alt && *alt) ? alt : _("[Image]");
			start = p->text->len;
			append_text(p, fallback, -1);
			obj = add_object(p->result, PIDGIN_MARKUP_OBJECT_IMAGE, start,
			                 p->text->len);
			obj->id = atoi(id);
			obj->alt = g_strdup(alt);
			obj->width = width ? atoi(width) : 0;
			obj->height = height ? atoi(height) : 0;
		}
	} else if (src != NULL && *src) {
		/* The alt text (or the URL) as a link; renderers that may load
		 * the image replace it. */
		const char *fallback = (alt && *alt) ? alt : src;
		gboolean own_link = (p->link == NULL);

		if (own_link)
			link_open(p, src);
		start = p->text->len;
		append_text(p, fallback, -1);
		if (own_link)
			link_close(p);
		if (!(p->flags & PIDGIN_MARKUP_NO_IMAGES)) {
			obj = add_object(p->result, PIDGIN_MARKUP_OBJECT_REMOTE_IMAGE,
			                 start, p->text->len);
			obj->uri = g_strdup(src);
			obj->alt = g_strdup(alt);
			obj->width = width ? atoi(width) : 0;
			obj->height = height ? atoi(height) : 0;
		}
	}

	g_free(id);
	g_free(src);
	g_free(alt);
	g_free(width);
	g_free(height);
}

static void
counter(int *c, gboolean open, gboolean wbfo)
{
	if (open)
		(*c)++;
	else if (*c > 0 && !wbfo)
		(*c)--;
}

static void
handle_tag(Parser *p, const Tag *tag)
{
	const char *n = tag->name;
	gboolean open = !tag->closing;
	gboolean wbfo = (p->flags & PIDGIN_MARKUP_WBFO) != 0;
	gboolean nofmt = (p->flags & PIDGIN_MARKUP_NO_FORMATTING) != 0;

	if (!strcmp(n, "b") || !strcmp(n, "bold") || !strcmp(n, "strong")) {
		if (!nofmt)
			counter(&p->bold, open, wbfo);
	} else if (!strcmp(n, "i") || !strcmp(n, "italic") || !strcmp(n, "em") ||
	           !strcmp(n, "cite")) {
		if (!nofmt)
			counter(&p->italic, open, wbfo);
	} else if (!strcmp(n, "u") || !strcmp(n, "underline")) {
		if (!nofmt)
			counter(&p->underline, open, wbfo);
	} else if (!strcmp(n, "s") || !strcmp(n, "strike") || !strcmp(n, "del")) {
		if (!nofmt)
			counter(&p->strike, open, wbfo);
	} else if (!strcmp(n, "sub")) {
		counter(&p->sub, open, FALSE);
	} else if (!strcmp(n, "sup")) {
		counter(&p->sup, open, FALSE);
	} else if (!strcmp(n, "code") || !strcmp(n, "tt")) {
		counter(&p->mono, open, FALSE);
	} else if (!strcmp(n, "pre")) {
		counter(&p->mono, open, FALSE);
		counter(&p->pre, open, FALSE);
	} else if (!strcmp(n, "title")) {
		counter(&p->title, open, FALSE);
	} else if (!strcmp(n, "br")) {
		if (open) {
			append_text(p, "\n", 1);
			p->after_br = TRUE;
		}
	} else if (!strcmp(n, "p") || !strcmp(n, "div") || !strcmp(n, "ul") ||
	           !strcmp(n, "ol")) {
		if (open)
			ensure_newline(p);
	} else if (!strcmp(n, "li")) {
		if (open) {
			ensure_newline(p);
			append_text(p, "\xe2\x80\xa2 ", -1);
		}
	} else if (n[0] == 'h' && g_ascii_isdigit(n[1])) {
		ensure_newline(p);
		counter(&p->bold, open, FALSE);
	} else if (!strcmp(n, "blockquote")) {
		ensure_newline(p);
		if (open) {
			PidginMarkupObject *q = add_object(p->result,
				PIDGIN_MARKUP_OBJECT_QUOTE, p->text->len, p->text->len);
			q->id = g_slist_length(p->quotes) + 1;
			p->quotes = g_slist_prepend(p->quotes, q);
		} else if (p->quotes) {
			PidginMarkupObject *q = p->quotes->data;
			q->end = p->text->len;
			p->quotes = g_slist_delete_link(p->quotes, p->quotes);
		}
	} else if (!strcmp(n, "hr")) {
		guint start;
		ensure_newline(p);
		start = p->text->len;
		append_text(p, HR_TEXT, -1);
		add_object(p->result, PIDGIN_MARKUP_OBJECT_HR, start, p->text->len);
		append_text(p, "\n", 1);
		p->after_br = TRUE;
	} else if (!strcmp(n, "font")) {
		if (open)
			handle_font(p, tag);
		else if (!wbfo)
			pop_font(p);
	} else if (!strcmp(n, "span") || !strcmp(n, "q") || !strcmp(n, "small") ||
	           !strcmp(n, "big")) {
		if (open) {
			handle_span(p, tag);
			if (!strcmp(n, "small") || !strcmp(n, "big")) {
				FontFrame *f = p->fonts->data;
				if (!(p->flags & PIDGIN_MARKUP_NO_SIZES) && f->points == 0)
					f->size = CLAMP(f->size + (n[0] == 's' ? -1 : 1), 1, 7);
			}
		} else if (!wbfo) {
			pop_font(p);
		}
	} else if (!strcmp(n, "a")) {
		if (open) {
			char *href = tag_get_attr(tag, "href");
			if (href && *href)
				link_open(p, href);
			g_free(href);
		} else {
			link_close(p);
		}
	} else if (!strcmp(n, "img")) {
		if (open)
			handle_img(p, tag);
	}
	/* html, head, body, binary, meta, style: ignored */

	if (tag->self_closing && open && strcmp(n, "br") && strcmp(n, "img") &&
	    strcmp(n, "hr")) {
		/* <b/> and friends: open and close at once */
		Tag closing = *tag;
		closing.closing = TRUE;
		closing.self_closing = FALSE;
		handle_tag(p, &closing);
	}
}

/* ---- text: smileys and bare links ---- */

/* Decodes up to @max bytes of text at @c (stopping at '<') for smiley
 * matching; @raw_len[i] is the raw length consumed for i+1 decoded
 * bytes. Returns the decoded string. */
static char *
decode_ahead(const char *c, gsize max, gsize *raw_len)
{
	GString *out = g_string_new(NULL);
	const char *start = c;

	while (*c && *c != '<' && out->len < max) {
		const char *amp;
		int tlen;

		if (*c == '&' && (amp = purple_markup_unescape_entity(c, &tlen)) != NULL) {
			gsize i, alen = strlen(amp);
			for (i = 0; i < alen && out->len < max; i++) {
				g_string_append_c(out, amp[i]);
				raw_len[out->len - 1] = (c - start) + (i == alen - 1 ? tlen : 0);
			}
			c += tlen;
		} else {
			g_string_append_c(out, *c);
			c++;
			raw_len[out->len - 1] = c - start;
		}
	}
	return g_string_free(out, FALSE);
}

#define SMILEY_LOOKAHEAD 48

/* Tries to match a smiley at @c; returns the raw length consumed. */
static gsize
try_smiley(Parser *p, const char *c)
{
	FontFrame *f = p->fonts ? p->fonts->data : NULL;
	const char *sml = (f && f->sml) ? f->sml : p->opts->protocol_sml;
	gsize raw_len[SMILEY_LOOKAHEAD];
	char *decoded;
	gsize len = 0, raw;
	gboolean custom = FALSE;
	PidginMarkupObject *obj;
	guint start;

	if ((p->flags & PIDGIN_MARKUP_NO_SMILEYS) || p->link != NULL ||
	    p->mono > 0 || p->title > 0)
		return 0;

	decoded = decode_ahead(c, SMILEY_LOOKAHEAD, raw_len);
	if (*decoded == '\0') {
		g_free(decoded);
		return 0;
	}

	if (p->opts->smiley_match != NULL) {
		len = p->opts->smiley_match(sml, decoded, p->opts->smiley_match_data);
		custom = len > 0;
	}
	if (len == 0)
		len = pidgin_smiley_theme_match(sml, decoded, NULL);
	if (len == 0 || len > strlen(decoded)) {
		g_free(decoded);
		return 0;
	}

	raw = raw_len[len - 1];
	start = p->text->len;
	append_text(p, decoded, len);
	obj = add_object(p->result, PIDGIN_MARKUP_OBJECT_SMILEY, start, p->text->len);
	obj->alt = g_strndup(decoded, len);
	obj->sml = g_strdup(sml);
	obj->id = custom ? 1 : 0;
	g_free(decoded);

	return raw;
}

static gboolean
is_url_trailing_punct(char ch, const char *url)
{
	if (strchr(".,;:!?\"'", ch))
		return TRUE;
	if (ch == ')' && strchr(url, '(') == NULL)
		return TRUE;
	return FALSE;
}

/* A bare link at a word start, like gtk_imhtml_is_protocol(). Returns the
 * raw length consumed, 0 if none. */
static gsize
try_linkify(Parser *p, const char *c)
{
	gsize plen;
	const char *e;
	GString *url;
	char *href;
	gsize raw;
	guint start;

	if ((p->flags & PIDGIN_MARKUP_NO_LINKIFY) || p->link != NULL || p->title > 0)
		return 0;
	if (p->text->len > 0 && !g_ascii_isspace(p->text->str[p->text->len - 1]) &&
	    !strchr("(<[\"'", p->text->str[p->text->len - 1]))
		return 0;

	plen = link_prefix_len(c);
	if (plen == 0 || c[plen] == '\0' || g_ascii_isspace(c[plen]) || c[plen] == '<')
		return 0;

	url = g_string_new(NULL);
	e = c;
	while (*e && !g_ascii_isspace(*e) && *e != '<') {
		const char *amp;
		int tlen;
		if (*e == '&' && (amp = purple_markup_unescape_entity(e, &tlen)) != NULL) {
			g_string_append(url, amp);
			e += tlen;
		} else {
			g_string_append_c(url, *e);
			e++;
		}
	}
	/* Trailing punctuation belongs to the sentence (raw == decoded for
	 * those characters). */
	while (url->len > plen && is_url_trailing_punct(url->str[url->len - 1], url->str) &&
	       e > c && e[-1] == url->str[url->len - 1]) {
		g_string_truncate(url, url->len - 1);
		e--;
	}
	if (url->len <= plen) {
		g_string_free(url, TRUE);
		return 0;
	}

	raw = e - c;
	href = !g_ascii_strncasecmp(url->str, "www.", 4) ?
		g_strconcat("http://", url->str, NULL) : g_strdup(url->str);

	start = p->text->len;
	link_open(p, href);
	append_text(p, url->str, url->len);
	link_close(p);
	(void)start;

	g_free(href);
	g_string_free(url, TRUE);
	return raw;
}

static void
parser_finish(Parser *p)
{
	link_close(p);
	while (p->fonts)
		pop_font(p);
	while (p->quotes) {
		PidginMarkupObject *q = p->quotes->data;
		q->end = p->text->len;
		p->quotes = g_slist_delete_link(p->quotes, p->quotes);
	}
	flush_run(p);

	p->result->text = g_string_free(p->text, FALSE);
	p->text = NULL;
	g_ptr_array_sort(p->result->objects, object_cmp);
}

static const PidginMarkupOptions default_options = { 0, NULL, NULL, NULL };

PidginMarkupResult *
pidgin_markup_parse_html(const char *html, const PidginMarkupOptions *options)
{
	Parser p;
	const char *c;

	if (options == NULL)
		options = &default_options;
	if (html == NULL)
		html = "";

	if ((options->flags & PIDGIN_MARKUP_STYLING) && pidgin_markup_is_plain(html)) {
		char *plain = pidgin_markup_plain_from_html(html);
		PidginMarkupResult *result = pidgin_markup_parse_styling(plain, options);
		g_free(plain);
		return result;
	}

	memset(&p, 0, sizeof(p));
	p.opts = options;
	p.flags = options->flags;
	p.result = result_new();
	p.text = g_string_sized_new(strlen(html));
	p.run.scale = 1.0;

	c = html;
	while (*c) {
		gsize consumed;

		if (*c == '<') {
			Tag tag;

			if (!strncmp(c, "<!--", 4)) {
				const char *end = strstr(c + 4, "-->");
				const char *stop = end ? end : c + strlen(c);
				if (p.flags & PIDGIN_MARKUP_SHOW_COMMENTS) {
					char *raw = g_strndup(c + 4, stop - (c + 4));
					char *text = purple_unescape_html(raw);
					p.dim++;
					append_text(&p, text, -1);
					p.dim--;
					g_free(text);
					g_free(raw);
				}
				c = end ? end + 3 : stop;
				continue;
			}
			if (parse_tag(c, &tag)) {
				handle_tag(&p, &tag);
				c = tag.end;
				continue;
			}
		}

		if (*c == '\r' || *c == '\n') {
			if (*c == '\r' && c[1] == '\n')
				c++;
			if (p.pre > 0 || (p.flags & PIDGIN_MARKUP_KEEP_NEWLINES)) {
				append_text(&p, "\n", 1);
			} else if (!p.after_br && p.text->len > 0) {
				append_text(&p, " ", 1);
			}
			c++;
			continue;
		}

		if ((consumed = try_smiley(&p, c)) > 0) {
			c += consumed;
			continue;
		}

		if (g_ascii_isalpha(*c) && (consumed = try_linkify(&p, c)) > 0) {
			c += consumed;
			continue;
		}

		if (*c == '&') {
			const char *amp;
			int tlen;
			if ((amp = purple_markup_unescape_entity(c, &tlen)) != NULL) {
				append_text(&p, amp, -1);
				c += tlen;
				continue;
			}
		}

		/* A whole UTF-8 character (invalid bytes are replaced). */
		{
			const char *next = g_utf8_find_next_char(c, NULL);
			gsize clen = next ? (gsize)(next - c) : strlen(c);
			if (g_utf8_validate(c, clen, NULL))
				append_text(&p, c, clen);
			else
				append_text(&p, "\xef\xbf\xbd", 3);
			c += clen;
		}
	}

	parser_finish(&p);
	return p.result;
}

/**************************************************************************
 * Plain text helpers
 **************************************************************************/

gboolean
pidgin_markup_is_plain(const char *html)
{
	const char *c;

	if (html == NULL)
		return TRUE;
	for (c = html; (c = strchr(c, '<')) != NULL; c++) {
		Tag tag;
		if (!parse_tag(c, &tag))
			continue;          /* a literal '<' */
		if (strcmp(tag.name, "br") || tag.closing)
			return FALSE;
	}
	return TRUE;
}

char *
pidgin_markup_plain_from_html(const char *html)
{
	GString *out;
	const char *c;

	if (html == NULL)
		return g_strdup("");

	out = g_string_sized_new(strlen(html));
	for (c = html; *c; ) {
		Tag tag;
		const char *amp;
		int tlen;

		if (*c == '<' && parse_tag(c, &tag)) {
			if (!strcmp(tag.name, "br") && !tag.closing)
				g_string_append_c(out, '\n');
			c = tag.end;
		} else if (*c == '&' && (amp = purple_markup_unescape_entity(c, &tlen)) != NULL) {
			g_string_append(out, amp);
			c += tlen;
		} else {
			g_string_append_c(out, *c);
			c++;
		}
	}
	return g_string_free(out, FALSE);
}

char *
pidgin_markup_html_to_plain(const char *html)
{
	PidginMarkupOptions opts = { PIDGIN_MARKUP_NO_SMILEYS | PIDGIN_MARKUP_NO_LINKIFY,
	                             NULL, NULL, NULL };
	PidginMarkupResult *result = pidgin_markup_parse_html(html, &opts);
	char *text = g_strdup(result->text);

	pidgin_markup_result_unref(result);
	return text;
}

/**************************************************************************
 * XEP-0393 Message Styling
 **************************************************************************/

typedef struct
{
	const char *text;
	PidginMarkupResult *result;
	guchar *opaque;         /* 1 = inside a link: no directives */
	const PidginMarkupOptions *opts;
} Styler;

typedef struct
{
	guint s, e;             /* content [s, e) of a line in the text */
} Line;

static gboolean
is_ws(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
	       c == '\v';
}

static gboolean
is_directive(char c)
{
	return c == '*' || c == '_' || c == '~' || c == '`';
}

static void
dim(Styler *st, guint s, guint e)
{
	attr_add(st->result->attrs, pango_attr_foreground_alpha_new(DIM_ALPHA), s, e);
}

static void
style_span(Styler *st, char d, guint s, guint e)
{
	PangoAttribute *attr = NULL;

	switch (d) {
		case '*': attr = pango_attr_weight_new(PANGO_WEIGHT_BOLD); break;
		case '_': attr = pango_attr_style_new(PANGO_STYLE_ITALIC); break;
		case '~': attr = pango_attr_strikethrough_new(TRUE); break;
		case '`': attr = pango_attr_family_new("monospace"); break;
	}
	attr_add(st->result->attrs, attr, s, e);
	dim(st, s, s + 1);
	dim(st, e - 1, e);
}

/*
 * Spans within [s, e) of one line (XEP-0393 section 6.2): an opening
 * directive at the block start, after whitespace, or right after a
 * different opening directive, not followed by whitespace; the first
 * matching closing directive that is not preceded by whitespace, with
 * some text in between that isn't just the directive character; lazily,
 * left to right. Preformatted spans are not parsed further.
 */
static void
parse_spans(Styler *st, guint s, guint e, char after_opening)
{
	const char *t = st->text;
	guint i = s;

	while (i < e) {
		char d = t[i];
		gboolean can_open;
		guint q;

		if (!is_directive(d) || st->opaque[i]) {
			i++;
			continue;
		}

		can_open = (i == s && (s == 0 || after_opening != 0 || is_ws(t[i - 1]) ||
		                       t[i - 1] == '>')) ||
		           (i > s && is_ws(t[i - 1]));
		if (i == s && after_opening != 0 && after_opening == d)
			can_open = FALSE;
		if (i == s && s > 0 && after_opening == 0 && !is_ws(t[i - 1]) &&
		    t[i - 1] != '>')
			can_open = FALSE;
		if (!can_open || i + 1 >= e || is_ws(t[i + 1])) {
			i++;
			continue;
		}

		for (q = i + 2; q < e; q++) {
			guint k;
			gboolean only_d = TRUE;

			if (t[q] != d || st->opaque[q] || is_ws(t[q - 1]))
				continue;
			for (k = i + 1; k < q; k++) {
				if (t[k] != d) {
					only_d = FALSE;
					break;
				}
			}
			if (!only_d)
				break;
		}
		if (q >= e) {
			i++;
			continue;
		}

		style_span(st, d, i, q + 1);
		if (d != '`')
			parse_spans(st, i + 1, q, d);
		i = q + 1;
	}
}

static gboolean
line_starts_with(Styler *st, const Line *l, const char *prefix)
{
	size_t n = strlen(prefix);

	return l->e - l->s >= n && !strncmp(st->text + l->s, prefix, n);
}

static gboolean
line_is_fence_end(Styler *st, const Line *l)
{
	guint e = l->e;

	while (e > l->s && (st->text[e - 1] == '\r' || st->text[e - 1] == ' '))
		e--;
	return e - l->s == 3 && !strncmp(st->text + l->s, "```", 3);
}

static void
parse_blocks(Styler *st, Line *lines, guint n, int depth)
{
	guint i = 0;

	while (i < n) {
		Line *l = &lines[i];

		if (l->e > l->s && st->text[l->s] == '>') {
			guint j, k;
			Line *child;
			PidginMarkupObject *q;

			for (j = i; j < n && lines[j].e > lines[j].s &&
			     st->text[lines[j].s] == '>'; j++)
				;
			child = g_new(Line, j - i);
			for (k = i; k < j; k++) {
				Line *c = &child[k - i];
				dim(st, lines[k].s, lines[k].s + 1);
				c->s = lines[k].s + 1;
				c->e = lines[k].e;
				/* trim the first leading whitespace character */
				if (c->s < c->e && (st->text[c->s] == ' ' || st->text[c->s] == '\t'))
					c->s++;
			}
			q = add_object(st->result, PIDGIN_MARKUP_OBJECT_QUOTE, lines[i].s,
			               lines[j - 1].e);
			q->id = depth + 1;
			parse_blocks(st, child, j - i, depth + 1);
			g_free(child);
			i = j;
		} else if (line_starts_with(st, l, "```")) {
			guint k;
			guint end;

			for (k = i + 1; k < n && !line_is_fence_end(st, &lines[k]); k++)
				;
			end = (k < n) ? lines[k].e : lines[n - 1].e;
			add_object(st->result, PIDGIN_MARKUP_OBJECT_CODE_BLOCK, l->s, end);
			attr_add(st->result->attrs, pango_attr_family_new("monospace"),
			         l->s, end);
			dim(st, l->s, l->e);
			if (k < n)
				dim(st, lines[k].s, lines[k].e);
			i = (k < n) ? k + 1 : n;
		} else {
			parse_spans(st, l->s, l->e, 0);
			i++;
		}
	}
}

/* Bare links in plain text; their characters are opaque to the span
 * parser (a URL's underscores are not emphasis). */
static void
styling_linkify(Styler *st, guint len)
{
	guint i = 0;
	const char *t = st->text;

	while (i < len) {
		gsize plen;
		guint e;
		PidginMarkupObject *obj;

		if ((i > 0 && !is_ws(t[i - 1]) && !strchr("(<[\"'", t[i - 1])) ||
		    !g_ascii_isalpha(t[i]) || (plen = link_prefix_len(t + i)) == 0 ||
		    i + plen >= len || is_ws(t[i + plen])) {
			i++;
			continue;
		}
		for (e = i; e < len && !is_ws(t[e]); e++)
			;
		while (e > i + plen) {
			char ch = t[e - 1];
			if (strchr(".,;:!?\"'*_~`", ch) ||
			    (ch == ')' && memchr(t + i, '(', e - i) == NULL))
				e--;
			else
				break;
		}
		if (e <= i + plen) {
			i++;
			continue;
		}
		obj = add_object(st->result, PIDGIN_MARKUP_OBJECT_LINK, i, e);
		obj->uri = g_strndup(t + i, e - i);
		if (!g_ascii_strncasecmp(obj->uri, "www.", 4)) {
			char *full = g_strconcat("http://", obj->uri, NULL);
			g_free(obj->uri);
			obj->uri = full;
		}
		memset(st->opaque + i, 1, e - i);
		i = e;
	}
}

static void
styling_smileys(Styler *st, guint len)
{
	guint i = 0;
	const char *sml = st->opts->protocol_sml;

	if (st->opts->flags & PIDGIN_MARKUP_NO_SMILEYS)
		return;

	while (i < len) {
		gsize m = 0;
		gboolean custom = FALSE;

		if (!st->opaque[i]) {
			if (st->opts->smiley_match) {
				m = st->opts->smiley_match(sml, st->text + i,
				                           st->opts->smiley_match_data);
				custom = m > 0;
			}
			if (m == 0)
				m = pidgin_smiley_theme_match(sml, st->text + i, NULL);
		}
		if (m > 0 && i + m <= len) {
			PidginMarkupObject *obj = add_object(st->result,
				PIDGIN_MARKUP_OBJECT_SMILEY, i, i + m);
			obj->alt = g_strndup(st->text + i, m);
			obj->sml = g_strdup(sml);
			obj->id = custom ? 1 : 0;
			i += m;
		} else {
			i++;
		}
	}
}

PidginMarkupResult *
pidgin_markup_parse_styling(const char *text, const PidginMarkupOptions *options)
{
	Styler st;
	GArray *lines;
	guint len, i, start;

	if (options == NULL)
		options = &default_options;
	if (text == NULL)
		text = "";

	memset(&st, 0, sizeof(st));
	st.result = result_new();
	/* Invalid UTF-8 never reaches the widgets. */
	st.result->text = g_utf8_make_valid(text, -1);
	st.text = st.result->text;
	st.opts = options;
	len = strlen(st.text);
	st.opaque = g_malloc0(len + 1);

	if (!(options->flags & PIDGIN_MARKUP_NO_LINKIFY))
		styling_linkify(&st, len);

	lines = g_array_new(FALSE, FALSE, sizeof(Line));
	for (start = 0, i = 0; i <= len; i++) {
		if (i == len || st.text[i] == '\n') {
			Line l = { start, i };
			g_array_append_val(lines, l);
			start = i + 1;
		}
	}
	parse_blocks(&st, (Line *)lines->data, lines->len, 0);
	g_array_free(lines, TRUE);

	/* Smileys only outside code (their ranges don't change the text). */
	{
		guint k;
		for (k = 0; k < st.result->objects->len; k++) {
			PidginMarkupObject *o = g_ptr_array_index(st.result->objects, k);
			if (o->type == PIDGIN_MARKUP_OBJECT_CODE_BLOCK)
				memset(st.opaque + o->start, 1, o->end - o->start);
		}
	}
	styling_smileys(&st, len);

	g_free(st.opaque);
	g_ptr_array_sort(st.result->objects, object_cmp);
	return st.result;
}

/**************************************************************************
 * Pango markup
 **************************************************************************/

static void
append_span_open(GString *out, GSList *attrs)
{
	GSList *l;
	gboolean any = FALSE;

	for (l = attrs; l; l = l->next) {
		PangoAttribute *a = l->data;

		if (!any) {
			g_string_append(out, "<span");
			any = TRUE;
		}
		switch (a->klass->type) {
			case PANGO_ATTR_WEIGHT:
				g_string_append_printf(out, " weight=\"%d\"",
				                       ((PangoAttrInt *)a)->value);
				break;
			case PANGO_ATTR_STYLE:
				g_string_append(out, " style=\"italic\"");
				break;
			case PANGO_ATTR_UNDERLINE:
				g_string_append(out, " underline=\"single\"");
				break;
			case PANGO_ATTR_STRIKETHROUGH:
				g_string_append(out, " strikethrough=\"true\"");
				break;
			case PANGO_ATTR_FAMILY: {
				char *f = g_markup_escape_text(((PangoAttrString *)a)->value, -1);
				g_string_append_printf(out, " font_family=\"%s\"", f);
				g_free(f);
				break;
			}
			case PANGO_ATTR_FOREGROUND:
			case PANGO_ATTR_BACKGROUND: {
				PangoColor *c = &((PangoAttrColor *)a)->color;
				g_string_append_printf(out, " %s=\"#%02x%02x%02x\"",
				                       a->klass->type == PANGO_ATTR_FOREGROUND ?
				                       "foreground" : "background",
				                       c->red >> 8, c->green >> 8, c->blue >> 8);
				break;
			}
			case PANGO_ATTR_FOREGROUND_ALPHA:
				g_string_append_printf(out, " fgalpha=\"%d\"",
				                       ((PangoAttrInt *)a)->value);
				break;
			case PANGO_ATTR_SIZE:
				g_string_append_printf(out, " size=\"%d\"",
				                       ((PangoAttrInt *)a)->value);
				break;
			case PANGO_ATTR_SCALE: {
				char buf[G_ASCII_DTOSTR_BUF_SIZE];
				g_ascii_formatd(buf, sizeof(buf), "%.1f",
				                ((PangoAttrFloat *)a)->value * 100.);
				g_string_append_printf(out, " size=\"%s%%\"", buf);
				break;
			}
			case PANGO_ATTR_BASELINE_SHIFT:
				g_string_append_printf(out, " baseline_shift=\"%s\"",
				                       ((PangoAttrInt *)a)->value ==
				                       PANGO_BASELINE_SHIFT_SUBSCRIPT ?
				                       "subscript" : "superscript");
				break;
			case PANGO_ATTR_FONT_SCALE:
				g_string_append_printf(out, " font_scale=\"%s\"",
				                       ((PangoAttrInt *)a)->value ==
				                       PANGO_FONT_SCALE_SUBSCRIPT ?
				                       "subscript" : "superscript");
				break;
			default:
				break;
		}
	}
	if (any)
		g_string_append_c(out, '>');
}

static int
uint_cmp(gconstpointer a, gconstpointer b)
{
	guint x = *(const guint *)a, y = *(const guint *)b;
	return x < y ? -1 : x > y;
}

char *
pidgin_markup_result_to_pango_markup(const PidginMarkupResult *result,
                                     guint32 revealed)
{
	GString *out;
	GArray *bounds;
	GSList *all, *l;
	guint len = strlen(result->text), i, k;
	int spoiler_index = 0;
	/* the link (or hidden spoiler) currently open */
	const PidginMarkupObject *open = NULL;
	char *open_uri = NULL;

	out = g_string_sized_new(len * 2);
	bounds = g_array_new(FALSE, FALSE, sizeof(guint));
	all = pango_attr_list_get_attributes(result->attrs);

	k = 0;
	g_array_append_val(bounds, k);
	g_array_append_val(bounds, len);
	for (l = all; l; l = l->next) {
		PangoAttribute *a = l->data;
		guint s = MIN(a->start_index, len), e = MIN(a->end_index, len);
		g_array_append_val(bounds, s);
		g_array_append_val(bounds, e);
	}
	for (i = 0; i < result->objects->len; i++) {
		PidginMarkupObject *o = g_ptr_array_index(result->objects, i);
		if (o->type == PIDGIN_MARKUP_OBJECT_LINK ||
		    o->type == PIDGIN_MARKUP_OBJECT_SPOILER) {
			g_array_append_val(bounds, o->start);
			g_array_append_val(bounds, o->end);
		}
	}
	g_array_sort(bounds, uint_cmp);

	for (i = 0; i + 1 < bounds->len; i++) {
		guint s = g_array_index(bounds, guint, i);
		guint e = g_array_index(bounds, guint, i + 1);
		GSList *covering = NULL;
		const PidginMarkupObject *want = NULL;
		gboolean hidden = FALSE;
		char *text;

		if (s >= e)
			continue;

		/* which link/spoiler covers [s, e)? spoilers win */
		spoiler_index = 0;
		for (k = 0; k < result->objects->len; k++) {
			PidginMarkupObject *o = g_ptr_array_index(result->objects, k);
			if (o->type == PIDGIN_MARKUP_OBJECT_SPOILER) {
				if (o->start <= s && e <= o->end &&
				    spoiler_index < 32 && !(revealed & (1u << spoiler_index))) {
					want = o;
					hidden = TRUE;
					break;
				}
				spoiler_index++;
			} else if (o->type == PIDGIN_MARKUP_OBJECT_LINK && want == NULL &&
			           o->start <= s && e <= o->end) {
				want = o;
			}
		}

		if (want != open) {
			if (open != NULL)
				g_string_append(out, "</a>");
			g_clear_pointer(&open_uri, g_free);
			open = want;
			if (open != NULL) {
				char *uri;
				if (hidden)
					open_uri = g_strdup_printf("pidgin-spoiler:%d", spoiler_index);
				else
					open_uri = g_strdup(open->uri);
				uri = g_markup_escape_text(open_uri, -1);
				g_string_append_printf(out, "<a href=\"%s\">", uri);
				g_free(uri);
			}
		}

		for (l = all; l; l = l->next) {
			PangoAttribute *a = l->data;
			if (a->start_index <= s && e <= a->end_index)
				covering = g_slist_prepend(covering, a);
		}
		covering = g_slist_reverse(covering);
		if (hidden) {
			g_string_append(out, "<span foreground=\"#000000\" background=\"#000000\">");
		}
		append_span_open(out, covering);
		text = g_markup_escape_text(result->text + s, e - s);
		g_string_append(out, text);
		g_free(text);
		if (covering)
			g_string_append(out, "</span>");
		if (hidden)
			g_string_append(out, "</span>");
		g_slist_free(covering);
	}
	if (open != NULL)
		g_string_append(out, "</a>");

	g_free(open_uri);
	g_slist_free_full(all, (GDestroyNotify)pango_attribute_destroy);
	g_array_free(bounds, TRUE);
	return g_string_free(out, FALSE);
}

/**************************************************************************
 * Capabilities
 **************************************************************************/

PidginFormatCaps
pidgin_format_caps_from_features(PurpleConnectionFlags features)
{
	PidginFormatCaps caps;

	if (features & PURPLE_CONNECTION_HTML) {
		caps = PIDGIN_FORMAT_HTML_ALL;
		if (features & PURPLE_CONNECTION_NO_BGCOLOR)
			caps &= ~PIDGIN_FORMAT_BACKCOLOR;
		if (features & PURPLE_CONNECTION_NO_FONTSIZE)
			caps &= ~PIDGIN_FORMAT_SIZE;
		if (features & PURPLE_CONNECTION_NO_URLDESC)
			caps &= ~PIDGIN_FORMAT_LINKDESC;
		if (features & PURPLE_CONNECTION_FORMATTING_WBFO)
			caps |= PIDGIN_FORMAT_WBFO;
	} else {
		caps = PIDGIN_FORMAT_SMILEY | PIDGIN_FORMAT_IMAGE;
	}

	if (features & PURPLE_CONNECTION_NO_IMAGES)
		caps &= ~PIDGIN_FORMAT_IMAGE;
	if (features & PURPLE_CONNECTION_ALLOW_CUSTOM_SMILEY)
		caps |= PIDGIN_FORMAT_CUSTOM_SMILEY;

	return caps;
}

PidginFormatCaps
pidgin_format_caps_for_account(PurpleAccount *account)
{
	PurpleConnection *gc;
	PurpleConnectionFlags features = 0;
	PurplePlugin *prpl;

	g_return_val_if_fail(account != NULL, 0);

	gc = purple_account_get_connection(account);
	if (gc != NULL)
		features = gc->flags;

	if (purple_strequal(purple_account_get_protocol_id(account), "prpl-jabber")) {
		PidginFormatCaps caps = PIDGIN_FORMAT_STYLING_ALL;
		if (features & PURPLE_CONNECTION_ALLOW_CUSTOM_SMILEY)
			caps |= PIDGIN_FORMAT_CUSTOM_SMILEY;
		return caps;
	}

	/* As Pidgin 2's gtkconv.c: no inline images for a prpl without
	 * OPT_PROTO_IM_IMAGE (Steam, Discord, IRC) even if it doesn't set
	 * PURPLE_CONNECTION_NO_IMAGES; it would drop the <img> anyway. */
	prpl = purple_find_prpl(purple_account_get_protocol_id(account));
	if (prpl != NULL && !(PURPLE_PLUGIN_PROTOCOL_INFO(prpl)->options & OPT_PROTO_IM_IMAGE))
		features |= PURPLE_CONNECTION_NO_IMAGES;

	return pidgin_format_caps_from_features(features);
}

/**************************************************************************
 * Compose buffer tags
 **************************************************************************/

void
pidgin_markup_buffer_ensure_tags(GtkTextBuffer *buffer)
{
	GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);

	if (gtk_text_tag_table_lookup(table, "bold") != NULL)
		return;
	gtk_text_buffer_create_tag(buffer, "bold", "weight", PANGO_WEIGHT_BOLD, NULL);
	gtk_text_buffer_create_tag(buffer, "italic", "style", PANGO_STYLE_ITALIC, NULL);
	gtk_text_buffer_create_tag(buffer, "underline", "underline", PANGO_UNDERLINE_SINGLE, NULL);
	gtk_text_buffer_create_tag(buffer, "strike", "strikethrough", TRUE, NULL);
	gtk_text_buffer_create_tag(buffer, "code", "family", "monospace", NULL);
}

static GtkTextTag *
get_named_tag(GtkTextBuffer *buffer, const char *name, const char *prop,
              gpointer value_ptr, double scale)
{
	GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
	GtkTextTag *tag = gtk_text_tag_table_lookup(table, name);

	if (tag != NULL)
		return tag;
	if (prop == NULL)
		tag = gtk_text_buffer_create_tag(buffer, name, "scale", scale, NULL);
	else
		tag = gtk_text_buffer_create_tag(buffer, name, prop, value_ptr, NULL);
	return tag;
}

GtkTextTag *
pidgin_markup_buffer_get_size_tag(GtkTextBuffer *buffer, int size)
{
	char name[16];

	size = CLAMP(size, 1, 7);
	g_snprintf(name, sizeof(name), "size:%d", size);
	return get_named_tag(buffer, name, NULL, NULL, pidgin_markup_size_scale(size));
}

GtkTextTag *
pidgin_markup_buffer_get_face_tag(GtkTextBuffer *buffer, const char *face)
{
	char *name = g_strconcat("face:", face, NULL);
	GtkTextTag *tag = get_named_tag(buffer, name, "family", (gpointer)face, 0);

	g_free(name);
	return tag;
}

static GtkTextTag *
color_tag(GtkTextBuffer *buffer, const char *prefix, const char *prop,
          const GdkRGBA *color)
{
	char *hex = color_to_hex(color);
	char *name = g_strconcat(prefix, hex, NULL);
	GdkRGBA opaque = *color;
	GtkTextTag *tag;

	opaque.alpha = 1.0;
	tag = get_named_tag(buffer, name, prop, &opaque, 0);
	g_free(name);
	g_free(hex);
	return tag;
}

GtkTextTag *
pidgin_markup_buffer_get_fore_tag(GtkTextBuffer *buffer, const GdkRGBA *color)
{
	return color_tag(buffer, "fore:", "foreground-rgba", color);
}

GtkTextTag *
pidgin_markup_buffer_get_back_tag(GtkTextBuffer *buffer, const GdkRGBA *color)
{
	return color_tag(buffer, "back:", "background-rgba", color);
}

GtkTextTag *
pidgin_markup_buffer_create_link_tag(GtkTextBuffer *buffer, const char *url)
{
	GtkTextTag *tag = gtk_text_buffer_create_tag(buffer, NULL,
		"underline", PANGO_UNDERLINE_SINGLE, NULL);

	g_object_set_data_full(G_OBJECT(tag), PIDGIN_MARKUP_LINK_KEY, g_strdup(url), g_free);
	return tag;
}

static gboolean
tag_in_family(GtkTextTag *tag, const char *family)
{
	char *name = NULL;
	gboolean ret;

	if (!strcmp(family, "link"))
		return g_object_get_data(G_OBJECT(tag), PIDGIN_MARKUP_LINK_KEY) != NULL;

	g_object_get(tag, "name", &name, NULL);
	ret = name != NULL && g_str_has_prefix(name, family);
	g_free(name);
	return ret;
}

typedef struct
{
	const char *family;
	GSList *tags;
} FamilyData;

static void
collect_family(GtkTextTag *tag, gpointer data)
{
	FamilyData *fd = data;

	if (tag_in_family(tag, fd->family))
		fd->tags = g_slist_prepend(fd->tags, tag);
}

void
pidgin_markup_buffer_remove_family(GtkTextBuffer *buffer, const char *family,
                                   const GtkTextIter *start, const GtkTextIter *end)
{
	FamilyData fd = { family, NULL };
	GSList *l;

	gtk_text_tag_table_foreach(gtk_text_buffer_get_tag_table(buffer),
	                           collect_family, &fd);
	for (l = fd.tags; l; l = l->next)
		gtk_text_buffer_remove_tag(buffer, l->data, start, end);
	g_slist_free(fd.tags);
}

/**************************************************************************
 * HTML -> compose buffer
 **************************************************************************/

static GtkTextTag *
tag_for_attr(GtkTextBuffer *buffer, PangoAttribute *a, PidginFormatCaps caps)
{
	switch (a->klass->type) {
		case PANGO_ATTR_WEIGHT:
			if ((caps & PIDGIN_FORMAT_BOLD) && ((PangoAttrInt *)a)->value >= PANGO_WEIGHT_BOLD)
				return gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buffer), "bold");
			break;
		case PANGO_ATTR_STYLE:
			if (caps & PIDGIN_FORMAT_ITALIC)
				return gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buffer), "italic");
			break;
		case PANGO_ATTR_UNDERLINE:
			if (caps & PIDGIN_FORMAT_UNDERLINE)
				return gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buffer), "underline");
			break;
		case PANGO_ATTR_STRIKETHROUGH:
			if (caps & PIDGIN_FORMAT_STRIKE)
				return gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buffer), "strike");
			break;
		case PANGO_ATTR_FAMILY: {
			const char *f = ((PangoAttrString *)a)->value;
			if (!strcmp(f, "monospace") && (caps & PIDGIN_FORMAT_CODE))
				return gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buffer), "code");
			if (caps & PIDGIN_FORMAT_FACE)
				return pidgin_markup_buffer_get_face_tag(buffer, f);
			break;
		}
		case PANGO_ATTR_FOREGROUND:
		case PANGO_ATTR_BACKGROUND: {
			PangoColor *c = &((PangoAttrColor *)a)->color;
			GdkRGBA rgba = { c->red / 65535., c->green / 65535., c->blue / 65535., 1. };
			if (a->klass->type == PANGO_ATTR_FOREGROUND && (caps & PIDGIN_FORMAT_FORECOLOR))
				return pidgin_markup_buffer_get_fore_tag(buffer, &rgba);
			if (a->klass->type == PANGO_ATTR_BACKGROUND && (caps & PIDGIN_FORMAT_BACKCOLOR))
				return pidgin_markup_buffer_get_back_tag(buffer, &rgba);
			break;
		}
		case PANGO_ATTR_SCALE:
			if (caps & PIDGIN_FORMAT_SIZE) {
				double v = ((PangoAttrFloat *)a)->value;
				int i, best = 3;
				double diff = 100;
				for (i = 1; i <= 7; i++) {
					if (fabs(pidgin_markup_size_scale(i) - v) < diff) {
						diff = fabs(pidgin_markup_size_scale(i) - v);
						best = i;
					}
				}
				if (best != 3)
					return pidgin_markup_buffer_get_size_tag(buffer, best);
			}
			break;
		case PANGO_ATTR_SIZE:
			if (caps & PIDGIN_FORMAT_SIZE) {
				int size = pidgin_markup_points_to_size(((PangoAttrInt *)a)->value / PANGO_SCALE);
				if (size != 3)
					return pidgin_markup_buffer_get_size_tag(buffer, size);
			}
			break;
		default:
			break;
	}
	return NULL;
}

void
pidgin_markup_buffer_insert_html(GtkTextBuffer *buffer, GtkTextIter *iter,
                                 const char *html, PidginFormatCaps caps,
                                 PidginMarkupFlags flags,
                                 PidginMarkupInsertObjectFunc insert_object,
                                 gpointer data)
{
	PidginMarkupOptions opts = { 0, NULL, NULL, NULL };
	PidginMarkupResult *result;
	GtkTextMark *start_mark;
	GSList *attrs, *l;
	GtkTextIter start, s, e;
	int base;
	guint i;

	g_return_if_fail(GTK_IS_TEXT_BUFFER(buffer));
	g_return_if_fail(iter != NULL);

	pidgin_markup_buffer_ensure_tags(buffer);

	opts.flags = flags | PIDGIN_MARKUP_NO_LINKIFY | PIDGIN_MARKUP_KEEP_NEWLINES;
	if (!(caps & (PIDGIN_FORMAT_SMILEY | PIDGIN_FORMAT_CUSTOM_SMILEY)) ||
	    insert_object == NULL)
		opts.flags |= PIDGIN_MARKUP_NO_SMILEYS;
	if (caps & PIDGIN_FORMAT_CUSTOM_SMILEY) {
		opts.smiley_match = pidgin_custom_smiley_match_func;
	}
	if (caps & PIDGIN_FORMAT_STYLING)
		opts.flags |= PIDGIN_MARKUP_STYLING;
	result = pidgin_markup_parse_html(html, &opts);

	start_mark = gtk_text_buffer_create_mark(buffer, NULL, iter, TRUE);
	base = gtk_text_iter_get_offset(iter);
	gtk_text_buffer_insert(buffer, iter, result->text, -1);
	gtk_text_buffer_get_iter_at_mark(buffer, &start, start_mark);

#define OFFSET_OF(byte) (base + (int)g_utf8_pointer_to_offset(result->text, \
                         result->text + MIN((guint)(byte), strlen(result->text))))

	attrs = pango_attr_list_get_attributes(result->attrs);
	for (l = attrs; l; l = l->next) {
		PangoAttribute *a = l->data;
		GtkTextTag *tag = tag_for_attr(buffer, a, caps);
		if (tag == NULL)
			continue;
		gtk_text_buffer_get_iter_at_offset(buffer, &s, OFFSET_OF(a->start_index));
		gtk_text_buffer_get_iter_at_offset(buffer, &e, OFFSET_OF(a->end_index));
		gtk_text_buffer_apply_tag(buffer, tag, &s, &e);
	}
	g_slist_free_full(attrs, (GDestroyNotify)pango_attribute_destroy);

	/* Links, then objects from last to first so offsets stay valid. */
	if (caps & PIDGIN_FORMAT_LINK) {
		for (i = 0; i < result->objects->len; i++) {
			PidginMarkupObject *o = g_ptr_array_index(result->objects, i);
			if (o->type != PIDGIN_MARKUP_OBJECT_LINK)
				continue;
			gtk_text_buffer_get_iter_at_offset(buffer, &s, OFFSET_OF(o->start));
			gtk_text_buffer_get_iter_at_offset(buffer, &e, OFFSET_OF(o->end));
			gtk_text_buffer_apply_tag(buffer,
				pidgin_markup_buffer_create_link_tag(buffer, o->uri), &s, &e);
		}
	}
	for (i = result->objects->len; i-- > 0; ) {
		PidginMarkupObject *o = g_ptr_array_index(result->objects, i);
		if (insert_object == NULL)
			break;
		if (o->type != PIDGIN_MARKUP_OBJECT_IMAGE &&
		    o->type != PIDGIN_MARKUP_OBJECT_SMILEY)
			continue;
		if (o->type == PIDGIN_MARKUP_OBJECT_IMAGE && !(caps & PIDGIN_FORMAT_IMAGE))
			continue;
		gtk_text_buffer_get_iter_at_offset(buffer, &s, OFFSET_OF(o->start));
		gtk_text_buffer_get_iter_at_offset(buffer, &e, OFFSET_OF(o->end));
		gtk_text_buffer_delete(buffer, &s, &e);
		insert_object(buffer, &s, o, data);
	}
#undef OFFSET_OF

	/* @iter ends up after the inserted text, as gtk_text_buffer_insert() */
	gtk_text_buffer_get_iter_at_mark(buffer, &start, start_mark);
	gtk_text_buffer_delete_mark(buffer, start_mark);
	(void)start;

	pidgin_markup_result_unref(result);
}

/**************************************************************************
 * Compose buffer -> HTML
 **************************************************************************/

/* Serializable tags in the order they are opened at the same position. */
enum {
	ORD_FACE, ORD_SIZE, ORD_FORE, ORD_BACK, ORD_LINK, ORD_BOLD, ORD_ITALIC,
	ORD_UNDERLINE, ORD_STRIKE, ORD_NONE
};

static int
tag_order(GtkTextTag *tag, const char **name_out)
{
	static char *name = NULL;

	g_free(name);
	name = NULL;
	g_object_get(tag, "name", &name, NULL);
	*name_out = name;

	if (name == NULL)
		return g_object_get_data(G_OBJECT(tag), PIDGIN_MARKUP_LINK_KEY) ?
			ORD_LINK : ORD_NONE;
	if (!strcmp(name, "bold")) return ORD_BOLD;
	if (!strcmp(name, "italic")) return ORD_ITALIC;
	if (!strcmp(name, "underline")) return ORD_UNDERLINE;
	if (!strcmp(name, "strike")) return ORD_STRIKE;
	if (g_str_has_prefix(name, "face:")) return ORD_FACE;
	if (g_str_has_prefix(name, "size:")) return ORD_SIZE;
	if (g_str_has_prefix(name, "fore:")) return ORD_FORE;
	if (g_str_has_prefix(name, "back:")) return ORD_BACK;
	return ORD_NONE;
}

typedef struct
{
	GtkTextTag *tag;
	int order;
	char *start;
	const char *end;
} OpenTag;

static OpenTag *
open_tag_new(GtkTextTag *tag, PidginMarkupFlags flags)
{
	const char *name;
	int order = tag_order(tag, &name);
	OpenTag *ot;
	char *esc;

	if (order == ORD_NONE)
		return NULL;

	ot = g_new0(OpenTag, 1);
	ot->tag = tag;
	ot->order = order;
	switch (order) {
		case ORD_BOLD: ot->start = g_strdup("<b>"); ot->end = "</b>"; break;
		case ORD_ITALIC: ot->start = g_strdup("<i>"); ot->end = "</i>"; break;
		case ORD_UNDERLINE: ot->start = g_strdup("<u>"); ot->end = "</u>"; break;
		case ORD_STRIKE: ot->start = g_strdup("<s>"); ot->end = "</s>"; break;
		case ORD_FACE:
			esc = g_markup_escape_text(name + 5, -1);
			ot->start = g_strdup_printf("<font face=\"%s\">", esc);
			ot->end = "</font>";
			g_free(esc);
			break;
		case ORD_SIZE: {
			int size = atoi(name + 5);
			if (flags & PIDGIN_MARKUP_USE_POINTSIZE)
				size = pidgin_markup_size_to_points(size);
			ot->start = g_strdup_printf("<font size=\"%d\">", size);
			ot->end = "</font>";
			break;
		}
		case ORD_FORE:
			ot->start = g_strdup_printf("<font color=\"%s\">", name + 5);
			ot->end = "</font>";
			break;
		case ORD_BACK:
			ot->start = g_strdup_printf("<font back=\"%s\">", name + 5);
			ot->end = "</font>";
			break;
		case ORD_LINK:
			esc = g_markup_escape_text(g_object_get_data(G_OBJECT(tag),
			                           PIDGIN_MARKUP_LINK_KEY), -1);
			ot->start = g_strdup_printf("<a href=\"%s\">", esc);
			ot->end = "</a>";
			g_free(esc);
			break;
	}
	return ot;
}

static void
open_tag_free(OpenTag *ot)
{
	g_free(ot->start);
	g_free(ot);
}

static gint
open_tag_cmp(gconstpointer a, gconstpointer b)
{
	return ((const OpenTag *)a)->order - ((const OpenTag *)b)->order;
}

static void
append_escaped_char(GString *str, gunichar c)
{
	switch (c) {
		case '<': g_string_append(str, "&lt;"); break;
		case '>': g_string_append(str, "&gt;"); break;
		case '&': g_string_append(str, "&amp;"); break;
		case '"': g_string_append(str, "&quot;"); break;
		case '\n': g_string_append(str, "<br>"); break;
		default: g_string_append_unichar(str, c); break;
	}
}

static const char *
anchor_html(const GtkTextIter *iter)
{
	GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(iter);
	GdkPaintable *paintable;

	if (anchor != NULL)
		return g_object_get_data(G_OBJECT(anchor), PIDGIN_MARKUP_HTML_KEY);
	paintable = gtk_text_iter_get_paintable(iter);
	if (paintable != NULL)
		return g_object_get_data(G_OBJECT(paintable), PIDGIN_MARKUP_HTML_KEY);
	return NULL;
}

/* Tags that start at @iter (or are in effect at @start). */
static GList *
tags_starting(const GtkTextIter *iter, gboolean in_effect, PidginMarkupFlags flags)
{
	GSList *tags = gtk_text_iter_get_tags(iter), *l;
	GList *out = NULL;

	for (l = tags; l; l = l->next) {
		OpenTag *ot;
		if (!in_effect && !gtk_text_iter_starts_tag(iter, l->data))
			continue;
		if ((ot = open_tag_new(l->data, flags)) != NULL)
			out = g_list_insert_sorted(out, ot, open_tag_cmp);
	}
	g_slist_free(tags);
	return out;
}

static void
append_buffer_char(GString *str, const GtkTextIter *iter)
{
	gunichar c = gtk_text_iter_get_char(iter);

	if (c == 0xFFFC) {
		const char *html = anchor_html(iter);
		if (html != NULL)
			g_string_append(str, html);
	} else {
		append_escaped_char(str, c);
	}
}

char *
pidgin_markup_buffer_to_html(GtkTextBuffer *buffer, const GtkTextIter *start_in,
                             const GtkTextIter *end_in, PidginMarkupFlags flags)
{
	GString *str = g_string_new(NULL);
	GtkTextIter start = *start_in, end = *end_in, iter, next;
	GQueue *q = g_queue_new();
	GList *starting, *l;
	OpenTag *ot;

	gtk_text_iter_order(&start, &end);

	if (flags & PIDGIN_MARKUP_WBFO) {
		/* The buffer is formatted as a whole: the formatting at the
		 * start (or the insertion formatting of an empty entry) wraps
		 * the text. */
		starting = tags_starting(&start, TRUE, flags);
		for (l = starting; l; l = l->next) {
			ot = l->data;
			if (ot->order == ORD_LINK) {
				open_tag_free(ot);
				continue;
			}
			g_string_append(str, ot->start);
			g_queue_push_tail(q, ot);
		}
		g_list_free(starting);
		for (iter = start; !gtk_text_iter_equal(&iter, &end);
		     gtk_text_iter_forward_char(&iter))
			append_buffer_char(str, &iter);
		while ((ot = g_queue_pop_tail(q)) != NULL) {
			g_string_append(str, ot->end);
			open_tag_free(ot);
		}
		g_queue_free(q);
		return g_string_free(str, FALSE);
	}

	/* Tags already in progress at the start. */
	starting = tags_starting(&start, TRUE, flags);
	for (l = starting; l; l = l->next) {
		ot = l->data;
		g_string_append(str, ot->start);
		g_queue_push_tail(q, ot);
	}
	g_list_free(starting);

	iter = start;
	while (!gtk_text_iter_equal(&iter, &end)) {
		GSList *tags, *sl;

		if (!gtk_text_iter_equal(&iter, &start)) {
			starting = tags_starting(&iter, FALSE, flags);
			for (l = starting; l; l = l->next) {
				ot = l->data;
				g_string_append(str, ot->start);
				g_queue_push_tail(q, ot);
			}
			g_list_free(starting);
		}

		append_buffer_char(str, &iter);

		next = iter;
		gtk_text_iter_forward_char(&next);

		/* Close the tags that end after this character, innermost
		 * first; tags opened after them but still going are closed
		 * and reopened, as gtk_imhtml_get_markup_range() did. */
		tags = gtk_text_iter_get_tags(&iter);
		tags = g_slist_reverse(tags);
		for (sl = tags; sl; sl = sl->next) {
			GtkTextTag *tag = sl->data;
			GQueue *reopen;
			GList *ql;
			gboolean ends = gtk_text_iter_equal(&next, &end) ||
			                !gtk_text_iter_has_tag(&next, tag);

			if (!ends)
				continue;
			for (ql = q->head; ql; ql = ql->next)
				if (((OpenTag *)ql->data)->tag == tag)
					break;
			if (ql == NULL)
				continue;

			reopen = g_queue_new();
			while ((ot = g_queue_pop_tail(q)) != NULL && ot->tag != tag) {
				g_string_append(str, ot->end);
				if (!gtk_text_iter_equal(&next, &end) &&
				    gtk_text_iter_has_tag(&next, ot->tag))
					g_queue_push_head(reopen, ot);
				else
					open_tag_free(ot);
			}
			if (ot != NULL) {
				g_string_append(str, ot->end);
				open_tag_free(ot);
			}
			while ((ot = g_queue_pop_head(reopen)) != NULL) {
				g_string_append(str, ot->start);
				g_queue_push_tail(q, ot);
			}
			g_queue_free(reopen);
		}
		g_slist_free(tags);

		iter = next;
	}

	while ((ot = g_queue_pop_tail(q)) != NULL) {
		g_string_append(str, ot->end);
		open_tag_free(ot);
	}
	g_queue_free(q);

	return g_string_free(str, FALSE);
}

/**************************************************************************
 * Compose buffer -> XEP-0393
 **************************************************************************/

typedef struct
{
	char d;
	guint s, e;             /* character offsets within the line */
} SRange;

static const char styling_chars[] = { '*', '_', '~', '`' };
static const char *styling_tags[] = { "bold", "italic", "strike", "code" };

static gint
srange_cmp(gconstpointer a, gconstpointer b)
{
	const SRange *x = a, *y = b;
	int px, py;

	if (x->s != y->s)
		return x->s < y->s ? -1 : 1;
	if (x->e != y->e)
		return x->e > y->e ? -1 : 1;
	/* code innermost */
	px = strchr("*_~`", x->d) - "*_~`";
	py = strchr("*_~`", y->d) - "*_~`";
	return px - py;
}

static void
serialize_styling_line(GString *out, const gunichar *chars, const char *const *repl,
                       guint n, guchar *flags)
{
	GList *ranges = NULL, *l;
	GPtrArray *stack;
	guint i, k;
	GList **open_at, **close_at;

	/* ranges per directive, whitespace trimmed */
	for (k = 0; k < 4; k++) {
		i = 0;
		while (i < n) {
			guint s, e;
			if (!(flags[i] & (1 << k))) {
				i++;
				continue;
			}
			s = i;
			while (i < n && (flags[i] & (1 << k)))
				i++;
			e = i;
			while (s < e && g_unichar_isspace(chars[s]))
				s++;
			while (e > s && g_unichar_isspace(chars[e - 1]))
				e--;
			if (e > s) {
				SRange *r = g_new(SRange, 1);
				r->d = styling_chars[k];
				r->s = s;
				r->e = e;
				ranges = g_list_insert_sorted(ranges, r, srange_cmp);
			}
		}
	}

	/* Make them nest: a range that sticks out of its parent is split
	 * at the parent's end; nothing nests inside code. */
	stack = g_ptr_array_new();
	for (l = ranges; l; ) {
		SRange *r = l->data;
		SRange *top;

		while (stack->len > 0 &&
		       ((SRange *)g_ptr_array_index(stack, stack->len - 1))->e <= r->s)
			g_ptr_array_remove_index(stack, stack->len - 1);
		top = stack->len ? g_ptr_array_index(stack, stack->len - 1) : NULL;

		if (top != NULL && (top->d == '`' || top->d == r->d)) {
			GList *dead = l;
			if (r->e > top->e) {
				r->s = top->e;
				while (r->s < r->e && g_unichar_isspace(chars[r->s]))
					r->s++;
				if (r->s < r->e) {
					/* re-sort the rest later */
					l = l->next;
					ranges = g_list_remove_link(ranges, dead);
					ranges = g_list_insert_sorted(ranges, r, srange_cmp);
					g_list_free(dead);
					continue;
				}
			}
			l = l->next;
			ranges = g_list_delete_link(ranges, dead);
			g_free(r);
			continue;
		}
		if (top != NULL && r->e > top->e) {
			SRange *rest = g_new(SRange, 1);
			rest->d = r->d;
			rest->s = top->e;
			rest->e = r->e;
			r->e = top->e;
			while (r->e > r->s && g_unichar_isspace(chars[r->e - 1]))
				r->e--;
			while (rest->s < rest->e && g_unichar_isspace(chars[rest->s]))
				rest->s++;
			if (rest->s < rest->e)
				ranges = g_list_insert_sorted(ranges, rest, srange_cmp);
			else
				g_free(rest);
		}
		g_ptr_array_add(stack, r);
		l = l->next;
	}
	g_ptr_array_free(stack, TRUE);

	open_at = g_new0(GList *, n + 1);
	close_at = g_new0(GList *, n + 1);
	for (l = ranges; l; l = l->next) {
		SRange *r = l->data;
		if (r->e <= r->s)
			continue;
		open_at[r->s] = g_list_append(open_at[r->s], r);      /* outer first */
		close_at[r->e] = g_list_prepend(close_at[r->e], r);   /* inner first */
	}

	for (i = 0; i <= n; i++) {
		for (l = close_at[i]; l; l = l->next)
			g_string_append_c(out, ((SRange *)l->data)->d);
		if (i == n)
			break;
		for (l = open_at[i]; l; l = l->next)
			g_string_append_c(out, ((SRange *)l->data)->d);
		if (repl[i] != NULL)
			g_string_append(out, repl[i]);
		else
			g_string_append_unichar(out, chars[i]);
	}

	for (i = 0; i <= n; i++) {
		g_list_free(open_at[i]);
		g_list_free(close_at[i]);
	}
	g_free(open_at);
	g_free(close_at);
	g_list_free_full(ranges, g_free);
}

char *
pidgin_markup_buffer_to_styling(GtkTextBuffer *buffer, const GtkTextIter *start_in,
                                const GtkTextIter *end_in)
{
	GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
	GtkTextTag *tags[4];
	GtkTextIter start = *start_in, end = *end_in, iter;
	GString *out = g_string_new(NULL);
	GArray *chars = g_array_new(FALSE, FALSE, sizeof(gunichar));
	GArray *flags = g_array_new(FALSE, FALSE, sizeof(guchar));
	GPtrArray *repl = g_ptr_array_new();
	int k;

	gtk_text_iter_order(&start, &end);
	for (k = 0; k < 4; k++)
		tags[k] = gtk_text_tag_table_lookup(table, styling_tags[k]);

	iter = start;
	for (;;) {
		gboolean at_end = gtk_text_iter_equal(&iter, &end);
		gunichar c = at_end ? 0 : gtk_text_iter_get_char(&iter);

		if (at_end || c == '\n') {
			serialize_styling_line(out, (gunichar *)chars->data,
			                       (const char *const *)repl->pdata, chars->len,
			                       (guchar *)flags->data);
			g_array_set_size(chars, 0);
			g_array_set_size(flags, 0);
			g_ptr_array_set_size(repl, 0);
			if (at_end)
				break;
			g_string_append_c(out, '\n');
		} else {
			guchar f = 0;
			const char *r = NULL;

			for (k = 0; k < 4; k++)
				if (tags[k] && gtk_text_iter_has_tag(&iter, tags[k]))
					f |= 1 << k;
			if (c == 0xFFFC) {
				const char *html = anchor_html(&iter);
				r = html ? html : "";
			}
			g_array_append_val(chars, c);
			g_array_append_val(flags, f);
			g_ptr_array_add(repl, (gpointer)r);
		}
		gtk_text_iter_forward_char(&iter);
	}

	g_array_free(chars, TRUE);
	g_array_free(flags, TRUE);
	g_ptr_array_free(repl, TRUE);

	/* Anchor HTML (smiley shortcuts, <IMG ID>) is HTML; the rest is text.
	 * Smiley shortcuts are stored unescaped, so this is plain text. */
	return g_string_free(out, FALSE);
}
