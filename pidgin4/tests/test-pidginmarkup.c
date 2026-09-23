/*
 * Unit tests for pidginmarkup.c: purple HTML and XEP-0393 parsing, Pango
 * markup output, and compose buffer <-> HTML / XEP-0393 round trips.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "pidgin-internal.h"

#include <gtk/gtk.h>

#include "imgstore.h"
#include "signals.h"
#include "smiley.h"

#include "pidginmarkup.h"
#include "pidginsmileytheme.h"

#include "test-support.h"

/**************************************************************************
 * Helpers
 **************************************************************************/

static PidginMarkupResult *
parse(const char *html, PidginMarkupFlags flags)
{
	PidginMarkupOptions opts = { flags, NULL, NULL, NULL };
	return pidgin_markup_parse_html(html, &opts);
}

static PidginMarkupResult *
style(const char *text)
{
	PidginMarkupOptions opts = { 0, NULL, NULL, NULL };
	return pidgin_markup_parse_styling(text, &opts);
}

/* TRUE if an attribute of @type covers exactly [s, e) (merged runs). */
static gboolean
has_attr(PidginMarkupResult *r, PangoAttrType type, guint s, guint e)
{
	GSList *attrs = pango_attr_list_get_attributes(r->attrs), *l;
	gboolean found = FALSE;

	for (l = attrs; l; l = l->next) {
		PangoAttribute *a = l->data;
		if (a->klass->type == type && a->start_index == s && a->end_index == e)
			found = TRUE;
	}
	g_slist_free_full(attrs, (GDestroyNotify)pango_attribute_destroy);
	return found;
}

static guint
count_attr(PidginMarkupResult *r, PangoAttrType type)
{
	GSList *attrs = pango_attr_list_get_attributes(r->attrs), *l;
	guint n = 0;

	for (l = attrs; l; l = l->next)
		if (((PangoAttribute *)l->data)->klass->type == type)
			n++;
	g_slist_free_full(attrs, (GDestroyNotify)pango_attribute_destroy);
	return n;
}

static PangoAttribute *
find_attr(PidginMarkupResult *r, PangoAttrType type)
{
	GSList *attrs = pango_attr_list_get_attributes(r->attrs), *l;
	PangoAttribute *found = NULL;

	for (l = attrs; l; l = l->next) {
		PangoAttribute *a = l->data;
		if (found == NULL && a->klass->type == type)
			found = pango_attribute_copy(a);
	}
	g_slist_free_full(attrs, (GDestroyNotify)pango_attribute_destroy);
	return found;
}

static PidginMarkupObject *
find_object(PidginMarkupResult *r, PidginMarkupObjectType type, guint nth)
{
	guint i;

	for (i = 0; i < r->objects->len; i++) {
		PidginMarkupObject *o = g_ptr_array_index(r->objects, i);
		if (o->type == type && nth-- == 0)
			return o;
	}
	return NULL;
}

static void
assert_color(PidginMarkupResult *r, PangoAttrType type, int red, int green, int blue)
{
	PangoAttribute *a = find_attr(r, type);
	PangoColor *c;

	g_assert_nonnull(a);
	c = &((PangoAttrColor *)a)->color;
	g_assert_cmpint(c->red >> 8, ==, red);
	g_assert_cmpint(c->green >> 8, ==, green);
	g_assert_cmpint(c->blue >> 8, ==, blue);
	pango_attribute_destroy(a);
}

static void
assert_valid_pango(PidginMarkupResult *r)
{
	char *markup = pidgin_markup_result_to_pango_markup(r, 0);
	char *no_links;
	GError *error = NULL;

	/* <a> is GtkLabel's extension of Pango markup */
	no_links = g_regex_replace(g_regex_new("</?a[^>]*>", 0, 0, NULL), markup, -1, 0,
	                           "", 0, NULL);
	if (!pango_parse_markup(no_links, -1, 0, NULL, NULL, NULL, &error))
		g_error("invalid pango markup '%s': %s", markup, error->message);
	g_free(no_links);
	g_free(markup);
}

/**************************************************************************
 * HTML parsing
 **************************************************************************/

static void
test_html_nested(void)
{
	PidginMarkupResult *r = parse("<b>bold <i>both</i></b> plain", 0);

	g_assert_cmpstr(r->text, ==, "bold both plain");
	g_assert_true(has_attr(r, PANGO_ATTR_WEIGHT, 0, 9));
	g_assert_true(has_attr(r, PANGO_ATTR_STYLE, 5, 9));
	assert_valid_pango(r);
	pidgin_markup_result_unref(r);

	/* misnested tags close per kind, as GtkIMHtml did */
	r = parse("<B><I>x</B>y</I>z", 0);
	g_assert_cmpstr(r->text, ==, "xyz");
	g_assert_true(has_attr(r, PANGO_ATTR_WEIGHT, 0, 1));
	g_assert_true(has_attr(r, PANGO_ATTR_STYLE, 0, 2));
	pidgin_markup_result_unref(r);

	r = parse("<u>u</u><s>s</s><strike>t</strike><em>e</em><strong>b</strong>"
	          "<sub>1</sub><sup>2</sup>", 0);
	g_assert_cmpstr(r->text, ==, "usteb12");
	g_assert_true(has_attr(r, PANGO_ATTR_UNDERLINE, 0, 1));
	g_assert_true(has_attr(r, PANGO_ATTR_STRIKETHROUGH, 1, 3));
	g_assert_true(has_attr(r, PANGO_ATTR_STYLE, 3, 4));
	g_assert_true(has_attr(r, PANGO_ATTR_WEIGHT, 4, 5));
	g_assert_true(has_attr(r, PANGO_ATTR_BASELINE_SHIFT, 5, 6));
	g_assert_true(has_attr(r, PANGO_ATTR_BASELINE_SHIFT, 6, 7));
	assert_valid_pango(r);
	pidgin_markup_result_unref(r);
}

static void
test_html_colors(void)
{
	PidginMarkupResult *r;

	r = parse("<font color=\"#ff0000\" back=\"#00ff00\">x</font>", 0);
	assert_color(r, PANGO_ATTR_FOREGROUND, 255, 0, 0);
	assert_color(r, PANGO_ATTR_BACKGROUND, 0, 255, 0);
	assert_valid_pango(r);
	pidgin_markup_result_unref(r);

	r = parse("<span style=\"color: #0000ff; background-color: rgb(255, 255, 0)\">x</span>", 0);
	assert_color(r, PANGO_ATTR_FOREGROUND, 0, 0, 255);
	assert_color(r, PANGO_ATTR_BACKGROUND, 255, 255, 0);
	pidgin_markup_result_unref(r);

	r = parse("<span style='background: #cccccc; color: #cccccc;'> </span>x", 0);
	assert_color(r, PANGO_ATTR_FOREGROUND, 0xcc, 0xcc, 0xcc);
	g_assert_null(find_object(r, PIDGIN_MARKUP_OBJECT_SPOILER, 0));
	pidgin_markup_result_unref(r);

	/* bare hex, names */
	r = parse("<font color=\"ff8000\">x</font><font color=red>y</font>", 0);
	assert_color(r, PANGO_ATTR_FOREGROUND, 255, 128, 0);
	pidgin_markup_result_unref(r);

	/* no colours */
	r = parse("<font color=\"#ff0000\">x</font>", PIDGIN_MARKUP_NO_INCOMING_FORMATTING);
	g_assert_cmpuint(count_attr(r, PANGO_ATTR_FOREGROUND), ==, 0);
	pidgin_markup_result_unref(r);
}

static void
test_html_sizes(void)
{
	PidginMarkupResult *r;
	PangoAttribute *a;

	r = parse("<font size=\"5\">big</font>", 0);
	a = find_attr(r, PANGO_ATTR_SCALE);
	g_assert_nonnull(a);
	g_assert_cmpfloat_with_epsilon(((PangoAttrFloat *)a)->value, 1.44, 0.0001);
	pango_attribute_destroy(a);
	pidgin_markup_result_unref(r);

	r = parse("<font size=\"+2\">big</font><font size=\"-1\">small</font>", 0);
	g_assert_cmpuint(count_attr(r, PANGO_ATTR_SCALE), ==, 2);
	pidgin_markup_result_unref(r);

	/* USE_POINTSIZE: size is points */
	r = parse("<font size=\"14\">pt</font>", PIDGIN_MARKUP_USE_POINTSIZE);
	a = find_attr(r, PANGO_ATTR_SIZE);
	g_assert_nonnull(a);
	g_assert_cmpint(((PangoAttrInt *)a)->value, ==, 14 * PANGO_SCALE);
	pango_attribute_destroy(a);
	pidgin_markup_result_unref(r);

	/* without it, 14 is clamped to 7 */
	r = parse("<font size=\"14\">pt</font>", 0);
	g_assert_cmpuint(count_attr(r, PANGO_ATTR_SIZE), ==, 0);
	a = find_attr(r, PANGO_ATTR_SCALE);
	g_assert_cmpfloat_with_epsilon(((PangoAttrFloat *)a)->value, 2.0736, 0.0001);
	pango_attribute_destroy(a);
	pidgin_markup_result_unref(r);

	/* absz is always points; CSS pt/px/keywords */
	r = parse("<font absz=\"18\">a</font><span style=\"font-size: 9pt\">b</span>"
	          "<span style=\"font-size: large\">c</span>", 0);
	g_assert_true(has_attr(r, PANGO_ATTR_SIZE, 0, 1));
	g_assert_true(has_attr(r, PANGO_ATTR_SIZE, 1, 2));
	g_assert_true(has_attr(r, PANGO_ATTR_SCALE, 2, 3));
	assert_valid_pango(r);
	pidgin_markup_result_unref(r);

	g_assert_cmpint(pidgin_markup_size_to_points(4), ==, 14);
	g_assert_cmpint(pidgin_markup_points_to_size(13), ==, 3);
	g_assert_cmpint(pidgin_markup_points_to_size(24), ==, 6);
}

static void
test_html_text(void)
{
	PidginMarkupResult *r;

	r = parse("&lt;b&gt; &amp; &quot;q&quot; &#x263a; &#9731; &nbsp;.", 0);
	/* purple_markup_unescape_entity() makes &nbsp; a plain space */
	g_assert_cmpstr(r->text, ==, "<b> & \"q\" \xe2\x98\xba \xe2\x98\x83  .");
	pidgin_markup_result_unref(r);

	/* unknown tags and lone '<' stay literal */
	r = parse("I <3 you <grin> a<b", 0);
	g_assert_cmpstr(r->text, ==, "I <3 you <grin> a<b");
	pidgin_markup_result_unref(r);

	/* BR, newlines as whitespace, P */
	r = parse("a<br>b\nc<BR/>\nd<p>e</p>", 0);
	g_assert_cmpstr(r->text, ==, "a\nb c\nd\ne");
	pidgin_markup_result_unref(r);

	r = parse("a\nb", PIDGIN_MARKUP_KEEP_NEWLINES);
	g_assert_cmpstr(r->text, ==, "a\nb");
	pidgin_markup_result_unref(r);

	/* comments, title, head */
	r = parse("a<!-- secret -->b<title>t</title>c<html><body>d</body></html>", 0);
	g_assert_cmpstr(r->text, ==, "abcd");
	pidgin_markup_result_unref(r);

	r = parse("a<!-- shown -->b", PIDGIN_MARKUP_SHOW_COMMENTS);
	g_assert_cmpstr(r->text, ==, "a shown b");
	g_assert_cmpuint(count_attr(r, PANGO_ATTR_FOREGROUND_ALPHA), ==, 1);
	pidgin_markup_result_unref(r);

	/* PRE keeps newlines, CODE is monospace */
	r = parse("<pre>a\nb</pre><code>c</code>", 0);
	g_assert_cmpstr(r->text, ==, "a\nbc");
	g_assert_true(has_attr(r, PANGO_ATTR_FAMILY, 0, 4));
	pidgin_markup_result_unref(r);

	/* HR */
	r = parse("above<hr>below", 0);
	g_assert_nonnull(find_object(r, PIDGIN_MARKUP_OBJECT_HR, 0));
	g_assert_true(g_str_has_prefix(r->text, "above\n"));
	g_assert_true(g_str_has_suffix(r->text, "\nbelow"));
	g_assert_true(pidgin_markup_result_has_graphics(r));
	pidgin_markup_result_unref(r);

	/* invalid UTF-8 never gets through */
	r = parse("a\xff" "b", 0);
	g_assert_true(g_utf8_validate(r->text, -1, NULL));
	pidgin_markup_result_unref(r);

	g_assert_true(pidgin_markup_is_plain("a &amp; b<br>c<br/>"));
	g_assert_true(pidgin_markup_is_plain("x <3"));
	g_assert_false(pidgin_markup_is_plain("<b>x</b>"));
	{
		char *plain = pidgin_markup_plain_from_html("a &lt;3<br>b");
		g_assert_cmpstr(plain, ==, "a <3\nb");
		g_free(plain);
		plain = pidgin_markup_html_to_plain("<b>x</b><img src='https://h/i.png' alt='pic'>");
		g_assert_cmpstr(plain, ==, "xpic");
		g_free(plain);
	}
}

static void
test_html_links(void)
{
	PidginMarkupResult *r;
	PidginMarkupObject *o;

	r = parse("go to <a href=\"http://pidgin.im/?a=1&amp;b=2\">the site</a>.", 0);
	g_assert_cmpstr(r->text, ==, "go to the site.");
	o = find_object(r, PIDGIN_MARKUP_OBJECT_LINK, 0);
	g_assert_nonnull(o);
	g_assert_cmpstr(o->uri, ==, "http://pidgin.im/?a=1&b=2");
	g_assert_cmpuint(o->start, ==, 6);
	g_assert_cmpuint(o->end, ==, 14);
	assert_valid_pango(r);
	pidgin_markup_result_unref(r);

	/* bare links, trailing punctuation, www., entities in the URL */
	r = parse("see https://example.com/a_b?x=1&amp;y=2. and www.pidgin.im, ok", 0);
	o = find_object(r, PIDGIN_MARKUP_OBJECT_LINK, 0);
	g_assert_nonnull(o);
	g_assert_cmpstr(o->uri, ==, "https://example.com/a_b?x=1&y=2");
	o = find_object(r, PIDGIN_MARKUP_OBJECT_LINK, 1);
	g_assert_nonnull(o);
	g_assert_cmpstr(o->uri, ==, "http://www.pidgin.im");
	pidgin_markup_result_unref(r);

	/* not in the middle of a word; not when disabled */
	r = parse("xhttp://a.b", 0);
	g_assert_null(find_object(r, PIDGIN_MARKUP_OBJECT_LINK, 0));
	pidgin_markup_result_unref(r);
	r = parse("http://a.b", PIDGIN_MARKUP_NO_LINKIFY);
	g_assert_null(find_object(r, PIDGIN_MARKUP_OBJECT_LINK, 0));
	pidgin_markup_result_unref(r);

	/* a link in pango markup */
	r = parse("<a href='http://x/'>x</a> <b>y</b>", 0);
	{
		char *m = pidgin_markup_result_to_pango_markup(r, 0);
		g_assert_nonnull(strstr(m, "<a href=\"http://x/\">x</a>"));
		g_free(m);
	}
	pidgin_markup_result_unref(r);
}

static void
test_html_images(void)
{
	PidginMarkupResult *r;
	PidginMarkupObject *o;

	r = parse("a<img id=\"3\">b<IMG ID=\"4\" alt=\"pic\" width=\"20\">", 0);
	o = find_object(r, PIDGIN_MARKUP_OBJECT_IMAGE, 0);
	g_assert_nonnull(o);
	g_assert_cmpint(o->id, ==, 3);
	o = find_object(r, PIDGIN_MARKUP_OBJECT_IMAGE, 1);
	g_assert_cmpint(o->id, ==, 4);
	g_assert_cmpint(o->width, ==, 20);
	g_assert_cmpstr(r->text + o->start, ==, "pic");
	pidgin_markup_result_unref(r);

	r = parse("<img src=\"https://cdn.discordapp.com/emojis/1.png\" alt=\":cat:\"/>", 0);
	o = find_object(r, PIDGIN_MARKUP_OBJECT_REMOTE_IMAGE, 0);
	g_assert_nonnull(o);
	g_assert_cmpstr(o->uri, ==, "https://cdn.discordapp.com/emojis/1.png");
	g_assert_cmpstr(r->text, ==, ":cat:");
	o = find_object(r, PIDGIN_MARKUP_OBJECT_LINK, 0);
	g_assert_nonnull(o);
	g_assert_cmpstr(o->uri, ==, "https://cdn.discordapp.com/emojis/1.png");
	pidgin_markup_result_unref(r);

	r = parse("<img id=\"3\"><img src=\"https://h/x.png\">", PIDGIN_MARKUP_NO_IMAGES);
	g_assert_null(find_object(r, PIDGIN_MARKUP_OBJECT_IMAGE, 0));
	g_assert_null(find_object(r, PIDGIN_MARKUP_OBJECT_REMOTE_IMAGE, 0));
	g_assert_cmpstr(r->text, ==, "https://h/x.png");
	pidgin_markup_result_unref(r);
}

static void
test_html_spoiler(void)
{
	PidginMarkupResult *r;
	PidginMarkupObject *o;
	char *m;

	r = parse("a <span style=\"color: black; background-color: black\">secret</span> b", 0);
	o = find_object(r, PIDGIN_MARKUP_OBJECT_SPOILER, 0);
	g_assert_nonnull(o);
	g_assert_cmpuint(o->start, ==, 2);
	g_assert_cmpuint(o->end, ==, 8);
	m = pidgin_markup_result_to_pango_markup(r, 0);
	g_assert_nonnull(strstr(m, "pidgin-spoiler:0"));
	g_free(m);
	m = pidgin_markup_result_to_pango_markup(r, 1);
	g_assert_null(strstr(m, "pidgin-spoiler"));
	g_free(m);
	assert_valid_pango(r);
	pidgin_markup_result_unref(r);

	r = parse("<span style=\"foreground: black; background: black\">s</span>", 0);
	g_assert_nonnull(find_object(r, PIDGIN_MARKUP_OBJECT_SPOILER, 0));
	pidgin_markup_result_unref(r);
}

static void
test_html_wbfo(void)
{
	PidginMarkupResult *r = parse("<b>a</b>b<font color=\"#ff0000\">c</font>d", PIDGIN_MARKUP_WBFO);

	g_assert_cmpstr(r->text, ==, "abcd");
	g_assert_true(has_attr(r, PANGO_ATTR_WEIGHT, 0, 4));
	g_assert_true(has_attr(r, PANGO_ATTR_FOREGROUND, 2, 4));
	pidgin_markup_result_unref(r);
}

static void
test_html_smileys(void)
{
	PidginMarkupResult *r;
	PidginMarkupObject *o;
	char *theme = pidgin_test_data_path("smileys/test/theme");

	g_assert_true(pidgin_smiley_theme_load_file(theme));
	g_free(theme);

	r = parse("hi :) and &gt;:o <b>:-)</b> <a href='http://x'>:)</a>", 0);
	o = find_object(r, PIDGIN_MARKUP_OBJECT_SMILEY, 0);
	g_assert_nonnull(o);
	g_assert_cmpstr(o->alt, ==, ":)");
	o = find_object(r, PIDGIN_MARKUP_OBJECT_SMILEY, 1);
	g_assert_nonnull(o);
	g_assert_cmpstr(o->alt, ==, ">:o");
	o = find_object(r, PIDGIN_MARKUP_OBJECT_SMILEY, 2);
	g_assert_nonnull(o);
	g_assert_cmpstr(o->alt, ==, ":-)");
	/* not inside links */
	g_assert_null(find_object(r, PIDGIN_MARKUP_OBJECT_SMILEY, 3));
	g_assert_cmpstr(r->text, ==, "hi :) and >:o :-) :)");
	pidgin_markup_result_unref(r);

	/* category from FONT sml, falling back to [default] */
	r = parse("<font sml=\"XMPP\">=)</font> =)", 0);
	o = find_object(r, PIDGIN_MARKUP_OBJECT_SMILEY, 0);
	g_assert_nonnull(o);
	g_assert_cmpstr(o->sml, ==, "XMPP");
	g_assert_null(find_object(r, PIDGIN_MARKUP_OBJECT_SMILEY, 1));
	pidgin_markup_result_unref(r);

	r = parse(":)", PIDGIN_MARKUP_NO_SMILEYS);
	g_assert_null(find_object(r, PIDGIN_MARKUP_OBJECT_SMILEY, 0));
	pidgin_markup_result_unref(r);

	pidgin_smiley_theme_set_current("none");
}

/**************************************************************************
 * XEP-0393
 **************************************************************************/

static void
assert_unstyled(const char *text)
{
	PidginMarkupResult *r = style(text);

	if (count_attr(r, PANGO_ATTR_WEIGHT) != 0 || count_attr(r, PANGO_ATTR_STYLE) != 0 ||
	    count_attr(r, PANGO_ATTR_STRIKETHROUGH) != 0 || count_attr(r, PANGO_ATTR_FAMILY) != 0)
		g_error("'%s' should not be styled", text);
	g_assert_cmpstr(r->text, ==, text);
	pidgin_markup_result_unref(r);
}

static void
test_styling_spans(void)
{
	PidginMarkupResult *r;

	assert_unstyled("plain span");

	r = style("*strong span*");
	g_assert_true(has_attr(r, PANGO_ATTR_WEIGHT, 0, 13));
	/* directives dimmed but kept */
	g_assert_true(has_attr(r, PANGO_ATTR_FOREGROUND_ALPHA, 0, 1));
	g_assert_true(has_attr(r, PANGO_ATTR_FOREGROUND_ALPHA, 12, 13));
	assert_valid_pango(r);
	pidgin_markup_result_unref(r);

	r = style("plain _emphasis_ plain");
	g_assert_true(has_attr(r, PANGO_ATTR_STYLE, 6, 16));
	pidgin_markup_result_unref(r);

	r = style("`pre` plain *strong*");
	g_assert_true(has_attr(r, PANGO_ATTR_FAMILY, 0, 5));
	g_assert_true(has_attr(r, PANGO_ATTR_WEIGHT, 12, 20));
	pidgin_markup_result_unref(r);

	r = style("*strong*plain*");
	g_assert_true(has_attr(r, PANGO_ATTR_WEIGHT, 0, 8));
	g_assert_cmpuint(count_attr(r, PANGO_ATTR_WEIGHT), ==, 1);
	pidgin_markup_result_unref(r);

	r = style("* plain *strong*");
	g_assert_true(has_attr(r, PANGO_ATTR_WEIGHT, 8, 16));
	g_assert_cmpuint(count_attr(r, PANGO_ATTR_WEIGHT), ==, 1);
	pidgin_markup_result_unref(r);

	assert_unstyled("not strong*");
	assert_unstyled("*not strong");
	assert_unstyled("*not \n strong*");
	assert_unstyled("*not *strong");
	assert_unstyled("**");
	assert_unstyled("***");
	assert_unstyled("****");
	assert_unstyled("snake_case_name");

	r = style("Two spans, both *alike in dignity*");
	g_assert_true(has_attr(r, PANGO_ATTR_WEIGHT, 16, 34));
	pidgin_markup_result_unref(r);

	r = style("The full title is _Twelfth Night, or What You Will_ but\n_most_ people shorten it.");
	g_assert_true(has_attr(r, PANGO_ATTR_STYLE, 18, 51));
	g_assert_true(has_attr(r, PANGO_ATTR_STYLE, 56, 62));
	pidgin_markup_result_unref(r);

	r = style("Everyone ~dis~likes cake.");
	g_assert_true(has_attr(r, PANGO_ATTR_STRIKETHROUGH, 9, 14));
	pidgin_markup_result_unref(r);

	r = style("This is `*monospace*`");
	g_assert_true(has_attr(r, PANGO_ATTR_FAMILY, 8, 21));
	g_assert_cmpuint(count_attr(r, PANGO_ATTR_WEIGHT), ==, 0);
	pidgin_markup_result_unref(r);

	r = style("This is *`monospace and bold`*");
	g_assert_true(has_attr(r, PANGO_ATTR_WEIGHT, 8, 30));
	g_assert_true(has_attr(r, PANGO_ATTR_FAMILY, 9, 29));
	pidgin_markup_result_unref(r);

	r = style("Wow, I can write in `monospace`!");
	g_assert_true(has_attr(r, PANGO_ATTR_FAMILY, 20, 31));
	pidgin_markup_result_unref(r);

	/* spans may not escape blocks */
	assert_unstyled("There are three blocks in this body, one per line,\n"
	                "but there is no *formatting\nas spans* may not escape blocks.");

	/* a URL's underscores are not emphasis */
	r = style("see http://a.org/x_y_z ok");
	g_assert_cmpuint(count_attr(r, PANGO_ATTR_STYLE), ==, 0);
	g_assert_nonnull(find_object(r, PIDGIN_MARKUP_OBJECT_LINK, 0));
	g_assert_cmpstr(find_object(r, PIDGIN_MARKUP_OBJECT_LINK, 0)->uri, ==,
	                "http://a.org/x_y_z");
	pidgin_markup_result_unref(r);
}

static void
test_styling_blocks(void)
{
	PidginMarkupResult *r;
	PidginMarkupObject *o;

	r = style("> That that is, is.\n\nSaid the old hermit of Prague.");
	o = find_object(r, PIDGIN_MARKUP_OBJECT_QUOTE, 0);
	g_assert_nonnull(o);
	g_assert_cmpint(o->id, ==, 1);
	g_assert_cmpuint(o->start, ==, 0);
	g_assert_cmpuint(o->end, ==, 19);
	g_assert_null(find_object(r, PIDGIN_MARKUP_OBJECT_QUOTE, 1));
	pidgin_markup_result_unref(r);

	r = style(">> That that is, is.\n> Said the old hermit of Prague.\n\nWho?");
	o = find_object(r, PIDGIN_MARKUP_OBJECT_QUOTE, 0);
	g_assert_cmpint(o->id, ==, 1);
	g_assert_cmpuint(o->end, ==, 53);
	o = find_object(r, PIDGIN_MARKUP_OBJECT_QUOTE, 1);
	g_assert_nonnull(o);
	g_assert_cmpint(o->id, ==, 2);
	g_assert_cmpuint(o->start, ==, 1);
	g_assert_cmpuint(o->end, ==, 20);
	pidgin_markup_result_unref(r);

	/* spans inside quotes */
	r = style("> *bold*");
	g_assert_true(has_attr(r, PANGO_ATTR_WEIGHT, 2, 8));
	pidgin_markup_result_unref(r);

	r = style("```ignored\n(println \"Hello, world!\")\n```\n\n"
	          "This should show up as monospace, preformatted text \xe2\xa4\xb4");
	o = find_object(r, PIDGIN_MARKUP_OBJECT_CODE_BLOCK, 0);
	g_assert_nonnull(o);
	g_assert_cmpuint(o->start, ==, 0);
	g_assert_cmpuint(o->end, ==, 40);
	g_assert_true(has_attr(r, PANGO_ATTR_FAMILY, 0, 40));
	pidgin_markup_result_unref(r);

	/* no styling inside the block */
	r = style("```\n*not bold*\n```");
	g_assert_cmpuint(count_attr(r, PANGO_ATTR_WEIGHT), ==, 0);
	pidgin_markup_result_unref(r);

	/* an unterminated fence ends with its parent block */
	r = style("> ```\n> (println \"Hello, world!\")\n\n"
	          "The entire blockquote is a preformatted text block, but this line\nis plaintext!");
	o = find_object(r, PIDGIN_MARKUP_OBJECT_CODE_BLOCK, 0);
	g_assert_nonnull(o);
	g_assert_cmpuint(o->start, ==, 2);
	g_assert_cmpuint(o->end, ==, 33);
	pidgin_markup_result_unref(r);

	/* plain XMPP bodies through the HTML entry point */
	{
		PidginMarkupOptions opts = { PIDGIN_MARKUP_STYLING, NULL, NULL, NULL };
		r = pidgin_markup_parse_html("&gt; quoted<br>*bold* &amp; more", &opts);
		g_assert_cmpstr(r->text, ==, "> quoted\n*bold* & more");
		g_assert_nonnull(find_object(r, PIDGIN_MARKUP_OBJECT_QUOTE, 0));
		g_assert_true(has_attr(r, PANGO_ATTR_WEIGHT, 9, 15));
		pidgin_markup_result_unref(r);

		/* real HTML stays HTML */
		r = pidgin_markup_parse_html("<b>*x*</b>", &opts);
		g_assert_true(has_attr(r, PANGO_ATTR_WEIGHT, 0, 3));
		g_assert_cmpuint(count_attr(r, PANGO_ATTR_FOREGROUND_ALPHA), ==, 0);
		pidgin_markup_result_unref(r);
	}
}

/**************************************************************************
 * Buffer round trips
 **************************************************************************/

static void
insert_object_cb(GtkTextBuffer *buffer, GtkTextIter *iter,
                 const PidginMarkupObject *obj, gpointer data)
{
	GtkTextChildAnchor *anchor;
	char *html;

	if (obj->type == PIDGIN_MARKUP_OBJECT_IMAGE)
		html = g_strdup_printf("<IMG ID=\"%d\">", obj->id);
	else
		html = g_markup_escape_text(obj->alt, -1);
	anchor = gtk_text_child_anchor_new();
	g_object_set_data_full(G_OBJECT(anchor), PIDGIN_MARKUP_HTML_KEY, html, g_free);
	gtk_text_buffer_insert_child_anchor(buffer, iter, anchor);
	g_object_unref(anchor);
}

static char *
round_trip(const char *html, PidginFormatCaps caps, PidginMarkupFlags flags)
{
	GtkTextBuffer *buffer = gtk_text_buffer_new(NULL);
	GtkTextIter start, end;
	char *out;

	gtk_text_buffer_get_end_iter(buffer, &end);
	pidgin_markup_buffer_insert_html(buffer, &end, html, caps, flags,
	                                 insert_object_cb, NULL);
	gtk_text_buffer_get_bounds(buffer, &start, &end);
	out = pidgin_markup_buffer_to_html(buffer, &start, &end, flags);
	g_object_unref(buffer);
	return out;
}

static void
assert_round_trip(const char *in, const char *expected, PidginFormatCaps caps,
                  PidginMarkupFlags flags)
{
	char *out = round_trip(in, caps, flags);

	if (g_strcmp0(out, expected) != 0)
		g_error("round trip of '%s':\n  got      '%s'\n  expected '%s'", in, out, expected);
	g_free(out);
}

#define ALL (PIDGIN_FORMAT_HTML_ALL | PIDGIN_FORMAT_CUSTOM_SMILEY)

static void
test_round_trip_html(void)
{
	assert_round_trip("plain", "plain", ALL, 0);
	assert_round_trip("<b>bold</b> <i>it</i>", "<b>bold</b> <i>it</i>", ALL, 0);
	assert_round_trip("<b>a<i>b</i></b>c", "<b>a<i>b</i></b>c", ALL, 0);
	assert_round_trip("<u>u</u><s>s</s>", "<u>u</u><s>s</s>", ALL, 0);
	assert_round_trip("<font color=\"#ff0000\">r</font>", "<font color=\"#ff0000\">r</font>", ALL, 0);
	assert_round_trip("<span style=\"color: #FF0000\">r</span>", "<font color=\"#ff0000\">r</font>", ALL, 0);
	assert_round_trip("<font back=\"#00ff00\">g</font>", "<font back=\"#00ff00\">g</font>", ALL, 0);
	assert_round_trip("<font face=\"Sans\">f</font>", "<font face=\"Sans\">f</font>", ALL, 0);
	assert_round_trip("<font face=\"Sans\" color=\"#0000ff\"><b>x</b></font>",
	                  "<font face=\"Sans\"><font color=\"#0000ff\"><b>x</b></font></font>", ALL, 0);
	assert_round_trip("<font size=\"5\">big</font>", "<font size=\"5\">big</font>", ALL, 0);
	assert_round_trip("<font size=\"3\">normal</font>", "normal", ALL, 0);
	assert_round_trip("<a href=\"http://x/?a=1&amp;b=2\">x</a>", "<a href=\"http://x/?a=1&amp;b=2\">x</a>", ALL, 0);
	assert_round_trip("a &lt; b &amp; &quot;c&quot; > d", "a &lt; b &amp; &quot;c&quot; &gt; d", ALL, 0);
	assert_round_trip("a<br>b<br/>c", "a<br>b<br>c", ALL, 0);
	assert_round_trip("x<IMG ID=\"5\">y", "x<IMG ID=\"5\">y", ALL, 0);
	/* capabilities drop what the protocol can't send */
	assert_round_trip("<font back=\"#00ff00\" color=\"#ff0000\">g</font><font size=\"5\">s</font>",
	                  "<font color=\"#ff0000\">g</font>s",
	                  ALL & ~(PIDGIN_FORMAT_BACKCOLOR | PIDGIN_FORMAT_SIZE), 0);
	assert_round_trip("x<IMG ID=\"5\">y", "x[Image]y", ALL & ~PIDGIN_FORMAT_IMAGE, 0);
}

static void
test_round_trip_pointsize(void)
{
	/* USE_POINTSIZE: points in, points out (via the nearest 1..7 size) */
	assert_round_trip("<font size=\"14\">x</font>", "<font size=\"14\">x</font>",
	                  ALL, PIDGIN_MARKUP_USE_POINTSIZE);
	assert_round_trip("<font size=\"36\">x</font>", "<font size=\"36\">x</font>",
	                  ALL, PIDGIN_MARKUP_USE_POINTSIZE);
	assert_round_trip("<font absz=\"18\">x</font>", "<font size=\"18\">x</font>",
	                  ALL, PIDGIN_MARKUP_USE_POINTSIZE);
	/* sizes without pointsize -> points with it */
	{
		GtkTextBuffer *buffer = gtk_text_buffer_new(NULL);
		GtkTextIter s, e;
		char *out;

		gtk_text_buffer_get_end_iter(buffer, &e);
		pidgin_markup_buffer_insert_html(buffer, &e, "<font size=\"4\">x</font>", ALL, 0, NULL, NULL);
		gtk_text_buffer_get_bounds(buffer, &s, &e);
		out = pidgin_markup_buffer_to_html(buffer, &s, &e, PIDGIN_MARKUP_USE_POINTSIZE);
		g_assert_cmpstr(out, ==, "<font size=\"14\">x</font>");
		g_free(out);
		out = pidgin_markup_buffer_to_html(buffer, &s, &e, 0);
		g_assert_cmpstr(out, ==, "<font size=\"4\">x</font>");
		g_free(out);
		g_object_unref(buffer);
	}
}

static void
test_round_trip_wbfo(void)
{
	GtkTextBuffer *buffer;
	GtkTextIter s, e;
	char *out;

	/* WBFO: closing tags are ignored on the way in, and the formatting at
	 * the start wraps everything on the way out */
	assert_round_trip("<font color=\"#ff0000\"><b>x</b>y</font>",
	                  "<font color=\"#ff0000\"><b>xy</b></font>", ALL, PIDGIN_MARKUP_WBFO);

	buffer = gtk_text_buffer_new(NULL);
	pidgin_markup_buffer_ensure_tags(buffer);
	gtk_text_buffer_set_text(buffer, "hello\nworld", -1);
	gtk_text_buffer_get_bounds(buffer, &s, &e);
	gtk_text_buffer_apply_tag_by_name(buffer, "bold", &s, &e);
	gtk_text_buffer_apply_tag(buffer, pidgin_markup_buffer_get_size_tag(buffer, 5), &s, &e);
	out = pidgin_markup_buffer_to_html(buffer, &s, &e, PIDGIN_MARKUP_WBFO);
	g_assert_cmpstr(out, ==, "<font size=\"5\"><b>hello<br>world</b></font>");
	g_free(out);

	/* partial formatting is ignored under WBFO: the start decides */
	gtk_text_buffer_get_iter_at_offset(buffer, &s, 3);
	gtk_text_buffer_apply_tag_by_name(buffer, "italic", &s, &e);
	gtk_text_buffer_get_bounds(buffer, &s, &e);
	out = pidgin_markup_buffer_to_html(buffer, &s, &e, PIDGIN_MARKUP_WBFO);
	g_assert_cmpstr(out, ==, "<font size=\"5\"><b>hello<br>world</b></font>");
	g_free(out);
	g_object_unref(buffer);
}

static void
test_round_trip_overlap(void)
{
	GtkTextBuffer *buffer = gtk_text_buffer_new(NULL);
	GtkTextIter s, e;
	char *out;

	pidgin_markup_buffer_ensure_tags(buffer);
	gtk_text_buffer_set_text(buffer, "abcdef", -1);
	gtk_text_buffer_get_iter_at_offset(buffer, &s, 0);
	gtk_text_buffer_get_iter_at_offset(buffer, &e, 4);
	gtk_text_buffer_apply_tag_by_name(buffer, "bold", &s, &e);
	gtk_text_buffer_get_iter_at_offset(buffer, &s, 2);
	gtk_text_buffer_get_iter_at_offset(buffer, &e, 6);
	gtk_text_buffer_apply_tag_by_name(buffer, "italic", &s, &e);
	gtk_text_buffer_get_bounds(buffer, &s, &e);
	out = pidgin_markup_buffer_to_html(buffer, &s, &e, 0);
	g_assert_cmpstr(out, ==, "<b>ab<i>cd</i></b><i>ef</i>");
	g_free(out);

	/* a link inside formatting */
	gtk_text_buffer_get_iter_at_offset(buffer, &s, 1);
	gtk_text_buffer_get_iter_at_offset(buffer, &e, 3);
	gtk_text_buffer_apply_tag(buffer,
		pidgin_markup_buffer_create_link_tag(buffer, "http://x/"), &s, &e);
	gtk_text_buffer_get_bounds(buffer, &s, &e);
	out = pidgin_markup_buffer_to_html(buffer, &s, &e, 0);
	g_assert_cmpstr(out, ==,
		"<b>a<a href=\"http://x/\">b<i>c</i></a><i>d</i></b><i>ef</i>");
	g_free(out);

	/* a sub-range starts with the tags in effect */
	gtk_text_buffer_get_iter_at_offset(buffer, &s, 3);
	gtk_text_buffer_get_iter_at_offset(buffer, &e, 5);
	out = pidgin_markup_buffer_to_html(buffer, &s, &e, 0);
	g_assert_cmpstr(out, ==, "<b><i>d</i></b><i>e</i>");
	g_free(out);

	/* remove a family */
	gtk_text_buffer_get_bounds(buffer, &s, &e);
	pidgin_markup_buffer_remove_family(buffer, "link", &s, &e);
	out = pidgin_markup_buffer_to_html(buffer, &s, &e, 0);
	g_assert_cmpstr(out, ==, "<b>ab<i>cd</i></b><i>ef</i>");
	g_free(out);

	g_object_unref(buffer);
}

static char *
styling_of(const char *text, const char *tag, int s1, int e1,
           const char *tag2, int s2, int e2)
{
	GtkTextBuffer *buffer = gtk_text_buffer_new(NULL);
	GtkTextIter s, e;
	char *out;

	pidgin_markup_buffer_ensure_tags(buffer);
	gtk_text_buffer_set_text(buffer, text, -1);
	if (tag) {
		gtk_text_buffer_get_iter_at_offset(buffer, &s, s1);
		gtk_text_buffer_get_iter_at_offset(buffer, &e, e1);
		gtk_text_buffer_apply_tag_by_name(buffer, tag, &s, &e);
	}
	if (tag2) {
		gtk_text_buffer_get_iter_at_offset(buffer, &s, s2);
		gtk_text_buffer_get_iter_at_offset(buffer, &e, e2);
		gtk_text_buffer_apply_tag_by_name(buffer, tag2, &s, &e);
	}
	gtk_text_buffer_get_bounds(buffer, &s, &e);
	out = pidgin_markup_buffer_to_styling(buffer, &s, &e);
	g_object_unref(buffer);
	return out;
}

static void
assert_styling(char *out, const char *expected)
{
	g_assert_cmpstr(out, ==, expected);
	g_free(out);
}

static void
test_styling_serialize(void)
{
	PidginMarkupResult *r;
	char *out;

	assert_styling(styling_of("plain", NULL, 0, 0, NULL, 0, 0), "plain");
	assert_styling(styling_of("bold text", "bold", 0, 4, NULL, 0, 0), "*bold* text");
	/* whitespace goes outside the directives */
	assert_styling(styling_of("bold text", "bold", 0, 5, NULL, 0, 0), "*bold* text");
	assert_styling(styling_of("a bold", "bold", 1, 6, NULL, 0, 0), "a *bold*");
	assert_styling(styling_of("x", "bold", 0, 1, "italic", 0, 1), "*_x_*");
	assert_styling(styling_of("rm -rf", "code", 0, 6, NULL, 0, 0), "`rm -rf`");
	assert_styling(styling_of("gone", "strike", 0, 4, NULL, 0, 0), "~gone~");
	/* nothing inside code */
	assert_styling(styling_of("a b", "code", 0, 3, "bold", 0, 1), "`a b`");
	/* lines are closed and reopened */
	assert_styling(styling_of("a\nb", "bold", 0, 3, NULL, 0, 0), "*a*\n*b*");
	/* overlaps are split so they nest */
	out = styling_of("one two three", "bold", 0, 7, "italic", 4, 13);
	g_assert_cmpstr(out, ==, "*one _two_* _three_");
	r = style(out);
	g_assert_true(has_attr(r, PANGO_ATTR_WEIGHT, 0, 11));
	g_assert_true(has_attr(r, PANGO_ATTR_STYLE, 5, 10));
	g_assert_true(has_attr(r, PANGO_ATTR_STYLE, 12, 19));
	pidgin_markup_result_unref(r);
	g_free(out);
	/* underline, sizes etc. are dropped */
	assert_styling(styling_of("u", "underline", 0, 1, NULL, 0, 0), "u");
}

static void
test_caps(void)
{
	PidginFormatCaps caps;

	caps = pidgin_format_caps_from_features(PURPLE_CONNECTION_HTML |
		PURPLE_CONNECTION_NO_BGCOLOR | PURPLE_CONNECTION_NO_FONTSIZE |
		PURPLE_CONNECTION_FORMATTING_WBFO);
	g_assert_true(caps & PIDGIN_FORMAT_BOLD);
	g_assert_false(caps & PIDGIN_FORMAT_BACKCOLOR);
	g_assert_false(caps & PIDGIN_FORMAT_SIZE);
	g_assert_true(caps & PIDGIN_FORMAT_WBFO);

	caps = pidgin_format_caps_from_features(PURPLE_CONNECTION_NO_IMAGES |
		PURPLE_CONNECTION_ALLOW_CUSTOM_SMILEY);
	g_assert_false(caps & PIDGIN_FORMAT_BOLD);
	g_assert_false(caps & PIDGIN_FORMAT_IMAGE);
	g_assert_true(caps & PIDGIN_FORMAT_SMILEY);
	g_assert_true(caps & PIDGIN_FORMAT_CUSTOM_SMILEY);
}

static gboolean scheme_hit;

static gboolean
test_scheme_cb(GtkWidget *w, const char *uri, gpointer data)
{
	scheme_hit = TRUE;
	return TRUE;
}

static void
test_schemes(void)
{
	GMenu *menu = g_menu_new();
	GSimpleActionGroup *group = g_simple_action_group_new();

	pidgin_markup_init();
	g_assert_true(pidgin_markup_uri_is_known("HTTPS://x"));
	g_assert_false(pidgin_markup_uri_is_known("open://x"));
	g_assert_true(pidgin_markup_register_scheme("open://", test_scheme_cb, NULL, NULL));
	g_assert_true(pidgin_markup_activate_uri(NULL, "open://log"));
	g_assert_true(scheme_hit);
	g_assert_true(pidgin_markup_populate_link_menu(NULL, "http://x", menu,
	                                               G_ACTION_MAP(group)));
	g_assert_cmpint(g_menu_model_get_n_items(G_MENU_MODEL(menu)), ==, 2);
	g_assert_true(g_action_group_has_action(G_ACTION_GROUP(group), "copy"));
	g_assert_true(pidgin_markup_register_scheme("open://", NULL, NULL, NULL));
	g_assert_false(pidgin_markup_uri_is_known("open://x"));
	/* bare links use the registry */
	{
		PidginMarkupResult *r = parse("irc://irc.libera.chat/#pidgin", 0);
		g_assert_nonnull(find_object(r, PIDGIN_MARKUP_OBJECT_LINK, 0));
		pidgin_markup_result_unref(r);
	}
	g_object_unref(menu);
	g_object_unref(group);
}

int
main(int argc, char *argv[])
{
	int ret;

	g_test_init(&argc, &argv, NULL);
	pidgin_test_profile_setup();
	purple_signals_init();
	purple_imgstore_init();
	purple_smileys_init();      /* the custom smiley matcher needs it */
	pidgin_markup_init();

	g_test_add_func("/markup/html/nested", test_html_nested);
	g_test_add_func("/markup/html/colors", test_html_colors);
	g_test_add_func("/markup/html/sizes", test_html_sizes);
	g_test_add_func("/markup/html/text", test_html_text);
	g_test_add_func("/markup/html/links", test_html_links);
	g_test_add_func("/markup/html/images", test_html_images);
	g_test_add_func("/markup/html/spoiler", test_html_spoiler);
	g_test_add_func("/markup/html/wbfo", test_html_wbfo);
	g_test_add_func("/markup/html/smileys", test_html_smileys);
	g_test_add_func("/markup/styling/spans", test_styling_spans);
	g_test_add_func("/markup/styling/blocks", test_styling_blocks);
	g_test_add_func("/markup/buffer/html", test_round_trip_html);
	g_test_add_func("/markup/buffer/pointsize", test_round_trip_pointsize);
	g_test_add_func("/markup/buffer/wbfo", test_round_trip_wbfo);
	g_test_add_func("/markup/buffer/overlap", test_round_trip_overlap);
	g_test_add_func("/markup/buffer/styling", test_styling_serialize);
	g_test_add_func("/markup/caps", test_caps);
	g_test_add_func("/markup/schemes", test_schemes);

	ret = g_test_run();
	pidgin_markup_uninit();
	pidgin_test_profile_cleanup();
	return ret;
}
