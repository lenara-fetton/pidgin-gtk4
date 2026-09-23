/**
 * @file pidginmarkup.h Purple HTML <-> Pango/GtkTextBuffer, XEP-0393
 * @ingroup pidgin
 */

/* pidgin
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

/*
 * PidginMarkup replaces GtkIMHtml's parser and gtk_imhtml_get_markup().
 *
 * Forward direction: the HTML subset libpurple and the prpls produce (see
 * pidgin/gtkimhtml.c for the reference behaviour) is parsed into a
 * PidginMarkupResult: the displayed text, a PangoAttrList with byte
 * indices into it, and a list of objects (links, images, smileys, rules,
 * spoilers, quotes, code blocks), each covering a byte range of the text.
 *
 * Every object's range holds a readable fallback (a smiley's shortcut, an
 * image's alt text, a line of box-drawing characters for <hr>), so the
 * text alone is always a faithful plain-text rendering (copy, search,
 * notifications). Renderers that can show graphics (PidginRichLabel's text
 * view mode) replace those ranges.
 *
 * The same result type comes out of the XEP-0393 Message Styling parser
 * for plain-text bodies.
 *
 * Reverse direction: the compose entry's GtkTextBuffer, formatted with the
 * tags created by pidgin_markup_buffer_ensure_tags() and friends, is
 * serialized to purple HTML (what gtk_imhtml_get_markup() produced) or to
 * XEP-0393 text.
 */
#ifndef _PIDGINMARKUP_H_
#define _PIDGINMARKUP_H_

#include <gtk/gtk.h>

#include "connection.h"

G_BEGIN_DECLS

/**************************************************************************
 * Parse options
 **************************************************************************/

typedef enum
{
	/** Drop colours (FONT color/back, CSS color/background). */
	PIDGIN_MARKUP_NO_COLOURS     = 1 << 0,
	/** Drop font faces. */
	PIDGIN_MARKUP_NO_FONTS       = 1 << 1,
	/** Drop font sizes. */
	PIDGIN_MARKUP_NO_SIZES       = 1 << 2,
	/** Drop B/I/U/S and the CSS equivalents. */
	PIDGIN_MARKUP_NO_FORMATTING  = 1 << 3,
	/** Don't replace smiley shortcuts. */
	PIDGIN_MARKUP_NO_SMILEYS     = 1 << 4,
	/** Don't turn bare URLs into links. */
	PIDGIN_MARKUP_NO_LINKIFY     = 1 << 5,
	/** Show HTML comments (dimmed) instead of dropping them. */
	PIDGIN_MARKUP_SHOW_COMMENTS  = 1 << 6,
	/**
	 * FONT size="N" is an absolute point size, not the 1..7 HTML scale
	 * (the prpl's OPT_PROTO_USE_POINTSIZE). FONT absz="N" is always one.
	 */
	PIDGIN_MARKUP_USE_POINTSIZE  = 1 << 7,
	/**
	 * Whole-buffer formatting only (PURPLE_CONNECTION_FORMATTING_WBFO):
	 * closing formatting tags are ignored, as GtkIMHtml did, so formatting
	 * runs to the end of the text.
	 */
	PIDGIN_MARKUP_WBFO           = 1 << 8,
	/** Drop images (IMG id/src become their alt text). */
	PIDGIN_MARKUP_NO_IMAGES      = 1 << 9,
	/** A literal newline is a line break (default: it's whitespace). */
	PIDGIN_MARKUP_KEEP_NEWLINES  = 1 << 10,
	/**
	 * If the body is plain text (entities and BR only, as XMPP plain
	 * bodies arrive), parse it as XEP-0393 Message Styling instead.
	 */
	PIDGIN_MARKUP_STYLING        = 1 << 11,

	/** What "don't show incoming formatting" means. */
	PIDGIN_MARKUP_NO_INCOMING_FORMATTING = PIDGIN_MARKUP_NO_COLOURS |
		PIDGIN_MARKUP_NO_FONTS | PIDGIN_MARKUP_NO_SIZES |
		PIDGIN_MARKUP_NO_FORMATTING
} PidginMarkupFlags;

/**
 * Looks for a smiley shortcut at the start of @text (NUL-terminated,
 * already unescaped). Returns the length in bytes of the longest match, or
 * 0. @sml is the smiley category in effect (FONT sml=, else the protocol's
 * name), may be NULL.
 */
typedef gsize (*PidginMarkupSmileyMatchFunc)(const char *sml, const char *text,
                                            gpointer data);

typedef struct
{
	PidginMarkupFlags flags;
	/** Default smiley category, usually the prpl's info->name. */
	const char *protocol_sml;
	/**
	 * Custom smiley matcher, tried before the smiley theme (e.g. the
	 * conversation's custom smileys). May be NULL.
	 */
	PidginMarkupSmileyMatchFunc smiley_match;
	gpointer smiley_match_data;
} PidginMarkupOptions;

/**************************************************************************
 * Parse result
 **************************************************************************/

typedef enum
{
	/** An imgstore image, IMG id=. */
	PIDGIN_MARKUP_OBJECT_IMAGE,
	/** A remote image, IMG src=; the range holds the alt text (linked). */
	PIDGIN_MARKUP_OBJECT_REMOTE_IMAGE,
	/** A smiley; the range holds the shortcut. */
	PIDGIN_MARKUP_OBJECT_SMILEY,
	/** A horizontal rule, on a line of its own. */
	PIDGIN_MARKUP_OBJECT_HR,
	/** A link; uri is the target. */
	PIDGIN_MARKUP_OBJECT_LINK,
	/** Discord spoiler text (black on black); click to reveal. */
	PIDGIN_MARKUP_OBJECT_SPOILER,
	/** A XEP-0393 quotation (or BLOCKQUOTE); depth >= 1. */
	PIDGIN_MARKUP_OBJECT_QUOTE,
	/** A XEP-0393 preformatted block or PRE element. */
	PIDGIN_MARKUP_OBJECT_CODE_BLOCK
} PidginMarkupObjectType;

typedef struct
{
	PidginMarkupObjectType type;
	/** Byte range [start, end) in the result text. */
	guint start;
	guint end;
	/**
	 * IMAGE: the imgstore id. QUOTE: the nesting depth. SMILEY: 1 if the
	 * options' smiley_match found it (a custom smiley), 0 for the theme.
	 */
	int id;
	/** LINK: the href. REMOTE_IMAGE: the src. */
	char *uri;
	/** REMOTE_IMAGE/IMAGE: alt text. SMILEY: the shortcut. */
	char *alt;
	/** SMILEY: the smiley category. */
	char *sml;
	/** IMAGE/REMOTE_IMAGE: requested width/height, or 0. */
	int width;
	int height;
} PidginMarkupObject;

#define PIDGIN_TYPE_MARKUP_RESULT (pidgin_markup_result_get_type())

typedef struct
{
	/*< private >*/
	int ref_count;

	/*< public >*/
	/** The displayed text (UTF-8). */
	char *text;
	/** Pango attributes with byte indices into text. */
	PangoAttrList *attrs;
	/** PidginMarkupObject*, ordered by start. */
	GPtrArray *objects;
} PidginMarkupResult;

GType pidgin_markup_result_get_type(void);
PidginMarkupResult *pidgin_markup_result_ref(PidginMarkupResult *result);
void pidgin_markup_result_unref(PidginMarkupResult *result);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(PidginMarkupResult, pidgin_markup_result_unref)

/** TRUE if any object needs graphics (image, remote image, smiley, rule). */
gboolean pidgin_markup_result_has_graphics(const PidginMarkupResult *result);

/** TRUE if any object is of @type. */
gboolean pidgin_markup_result_has_object(const PidginMarkupResult *result,
                                         PidginMarkupObjectType type);

/**
 * Converts the result to Pango markup for a GtkLabel: the attributes as
 * <span>s and links as <a href>. Spoilers whose index (0-based among the
 * spoilers) is not in @revealed (a bitmask, spoilers >= 32 are always
 * revealed) are hidden and become links to "pidgin-spoiler:<index>" so a
 * GtkLabel "activate-link" handler can reveal them.
 */
char *pidgin_markup_result_to_pango_markup(const PidginMarkupResult *result,
                                           guint32 revealed);

/**************************************************************************
 * Parsing
 **************************************************************************/

/**
 * Parses purple HTML. @options may be NULL (defaults: everything shown,
 * newlines are whitespace, smileys from the current theme).
 */
PidginMarkupResult *pidgin_markup_parse_html(const char *html,
                                             const PidginMarkupOptions *options);

/**
 * Parses plain text as XEP-0393 Message Styling. Directive characters stay
 * in the text (dimmed, styled like their span), as the XEP recommends.
 * Of @options only NO_LINKIFY and NO_SMILEYS/the smiley matchers are used.
 */
PidginMarkupResult *pidgin_markup_parse_styling(const char *text,
                                                const PidginMarkupOptions *options);

/**
 * TRUE if @html has no markup but entities and BR tags (so it is a plain
 * body a prpl escaped), i.e. PIDGIN_MARKUP_STYLING applies to it.
 */
gboolean pidgin_markup_is_plain(const char *html);

/**
 * Converts plain-escaped HTML (see pidgin_markup_is_plain()) back to text:
 * entities decoded, BR to newlines.
 */
char *pidgin_markup_plain_from_html(const char *html);

/**
 * Converts HTML to plain text the way the result text renders it (images
 * as their alt text, smileys as text, rules as a line). Handy for FTS
 * bodies and notifications.
 */
char *pidgin_markup_html_to_plain(const char *html);

/**
 * Parses a colour as purple HTML uses them (#rgb, #rrggbb, names, CSS
 * rgb()). Returns FALSE if it isn't one.
 */
gboolean pidgin_markup_parse_color(const char *str, GdkRGBA *rgba);

/** The GtkIMHtml font scale factor for HTML size 1..7. */
double pidgin_markup_size_scale(int size);

/**
 * Converts between HTML sizes 1..7 and point sizes, for USE_POINTSIZE
 * protocols: 8, 10, 12, 14, 18, 24, 36 pt (the classic HTML table).
 */
int pidgin_markup_size_to_points(int size);
int pidgin_markup_points_to_size(int points);

/**************************************************************************
 * Formatting capabilities (what the compose entry offers)
 **************************************************************************/

typedef enum
{
	PIDGIN_FORMAT_BOLD          = 1 << 0,
	PIDGIN_FORMAT_ITALIC        = 1 << 1,
	PIDGIN_FORMAT_UNDERLINE     = 1 << 2,
	PIDGIN_FORMAT_STRIKE        = 1 << 3,
	/** Inline code; only in XEP-0393 mode. */
	PIDGIN_FORMAT_CODE          = 1 << 4,
	/** Larger/smaller font sizes. */
	PIDGIN_FORMAT_SIZE          = 1 << 5,
	PIDGIN_FORMAT_FACE          = 1 << 6,
	PIDGIN_FORMAT_FORECOLOR     = 1 << 7,
	PIDGIN_FORMAT_BACKCOLOR     = 1 << 8,
	PIDGIN_FORMAT_LINK          = 1 << 9,
	/** Links with a description different from the URL. */
	PIDGIN_FORMAT_LINKDESC      = 1 << 10,
	PIDGIN_FORMAT_IMAGE         = 1 << 11,
	PIDGIN_FORMAT_SMILEY        = 1 << 12,
	PIDGIN_FORMAT_CUSTOM_SMILEY = 1 << 13,
	/**
	 * Not a button: the entry serializes to XEP-0393 text instead of
	 * HTML (XMPP, see M8 in doc/PIDGIN-UPGRADE.md).
	 */
	PIDGIN_FORMAT_STYLING       = 1 << 14,
	/** Not a button: whole-buffer formatting only (WBFO). */
	PIDGIN_FORMAT_WBFO          = 1 << 15,

	PIDGIN_FORMAT_HTML_ALL = PIDGIN_FORMAT_BOLD | PIDGIN_FORMAT_ITALIC |
		PIDGIN_FORMAT_UNDERLINE | PIDGIN_FORMAT_STRIKE | PIDGIN_FORMAT_SIZE |
		PIDGIN_FORMAT_FACE | PIDGIN_FORMAT_FORECOLOR |
		PIDGIN_FORMAT_BACKCOLOR | PIDGIN_FORMAT_LINK |
		PIDGIN_FORMAT_LINKDESC | PIDGIN_FORMAT_IMAGE | PIDGIN_FORMAT_SMILEY,
	/** What an XMPP conversation offers (XEP-0393). */
	PIDGIN_FORMAT_STYLING_ALL = PIDGIN_FORMAT_BOLD | PIDGIN_FORMAT_ITALIC |
		PIDGIN_FORMAT_STRIKE | PIDGIN_FORMAT_CODE | PIDGIN_FORMAT_SMILEY |
		PIDGIN_FORMAT_STYLING
} PidginFormatCaps;

/**
 * The capabilities gtk_imhtml_setup_entry() derived from connection flags:
 * HTML gets everything minus NO_BGCOLOR/NO_FONTSIZE/NO_URLDESC/NO_IMAGES,
 * non-HTML only smileys and images; ALLOW_CUSTOM_SMILEY and
 * FORMATTING_WBFO are carried over.
 */
PidginFormatCaps pidgin_format_caps_from_features(PurpleConnectionFlags features);

/**
 * The capabilities for a conversation on @account: from its connection
 * flags, except XMPP (prpl-jabber), which gets PIDGIN_FORMAT_STYLING_ALL.
 * Neither includes images sent by upload (see below): gtkconv.c adds
 * IMAGE for the conversations that take them.
 */
PidginFormatCaps pidgin_format_caps_for_account(PurpleAccount *account);

/**
 * Whether @account's prpl sends files by HTTP upload now: its IPC
 * "http-upload-available" (the jabber prpl, XEP-0363, once the server's
 * upload service is known) answers TRUE for the connected account.
 * Inserted images can then go as uploads (pidgin4 gtkconv.c).
 */
gboolean pidgin_format_account_uploads_images(PurpleAccount *account);

/** The prpl's IPC "http-upload-max-size" for @account: the largest file
 *  the upload service takes, 0 if unknown or unlimited. */
guint64 pidgin_format_account_upload_max_size(PurpleAccount *account);

/**************************************************************************
 * GtkTextBuffer formatting tags and serialization
 **************************************************************************/

/*
 * Tag names in a compose buffer (created on demand):
 *   "bold", "italic", "underline", "strike", "code"
 *   "size:N"        N = 1..7 (3, the default, is never applied)
 *   "face:FAMILY"
 *   "fore:#rrggbb", "back:#rrggbb"
 *   links are anonymous tags with the URL as object data
 *   "pidgin-link-url" (see pidgin_markup_buffer_create_link_tag())
 *
 * Inline objects are GtkTextChildAnchors or paintables whose HTML (e.g.
 * <IMG ID="5"> or a smiley shortcut) is attached as object data
 * PIDGIN_MARKUP_HTML_KEY to the anchor, or to the paintable.
 */
#define PIDGIN_MARKUP_HTML_KEY "pidgin-markup-html"
#define PIDGIN_MARKUP_LINK_KEY "pidgin-link-url"

/** Creates the fixed tags (bold, italic, underline, strike, code). */
void pidgin_markup_buffer_ensure_tags(GtkTextBuffer *buffer);

GtkTextTag *pidgin_markup_buffer_get_size_tag(GtkTextBuffer *buffer, int size);
GtkTextTag *pidgin_markup_buffer_get_face_tag(GtkTextBuffer *buffer, const char *face);
GtkTextTag *pidgin_markup_buffer_get_fore_tag(GtkTextBuffer *buffer, const GdkRGBA *color);
GtkTextTag *pidgin_markup_buffer_get_back_tag(GtkTextBuffer *buffer, const GdkRGBA *color);
GtkTextTag *pidgin_markup_buffer_create_link_tag(GtkTextBuffer *buffer, const char *url);

/**
 * Removes all tags of one family ("size:", "face:", "fore:", "back:",
 * "link") from [start, end).
 */
void pidgin_markup_buffer_remove_family(GtkTextBuffer *buffer, const char *family,
                                        const GtkTextIter *start,
                                        const GtkTextIter *end);

/**
 * Inserts @html at @iter as formatted compose-buffer text (the inverse of
 * pidgin_markup_buffer_to_html()). Formatting not in @caps is dropped;
 * images become anchors carrying their HTML (the caller gives them a
 * widget or paintable via @insert_object, may be NULL: then images are
 * inserted as their HTML text).
 */
typedef void (*PidginMarkupInsertObjectFunc)(GtkTextBuffer *buffer,
                                             GtkTextIter *iter,
                                             const PidginMarkupObject *object,
                                             gpointer data);

void pidgin_markup_buffer_insert_html(GtkTextBuffer *buffer, GtkTextIter *iter,
                                      const char *html, PidginFormatCaps caps,
                                      PidginMarkupFlags flags,
                                      PidginMarkupInsertObjectFunc insert_object,
                                      gpointer data);

/**
 * Serializes [start, end) of a compose buffer to purple HTML, as
 * gtk_imhtml_get_markup_range() did: <b>, <i>, <u>, <s>,
 * <font face/size/color/back>, <a href>, newlines as <br>, anchors as their
 * PIDGIN_MARKUP_HTML_KEY data, and &lt; &gt; &amp; &quot; escaped.
 *
 * PIDGIN_MARKUP_USE_POINTSIZE writes size="PT". PIDGIN_MARKUP_WBFO wraps
 * the whole text in the formatting that is in effect at @start (the
 * buffer is formatted as a whole) instead of following tag toggles.
 */
char *pidgin_markup_buffer_to_html(GtkTextBuffer *buffer,
                                   const GtkTextIter *start,
                                   const GtkTextIter *end,
                                   PidginMarkupFlags flags);

/**
 * Serializes [start, end) of a compose buffer to XEP-0393 text: bold as
 * *...*, italic _..._, strike ~...~, code `...`. Directives are moved
 * inside word boundaries (a span never starts or ends with whitespace) and
 * are closed and reopened at line ends. Other formatting is dropped.
 * Anchors become their HTML data unescaped (smiley shortcuts).
 */
char *pidgin_markup_buffer_to_styling(GtkTextBuffer *buffer,
                                      const GtkTextIter *start,
                                      const GtkTextIter *end);

/**************************************************************************
 * Link schemes (replaces gtk_imhtml_class_register_protocol)
 **************************************************************************/

/**
 * Activates @uri (a click). Return TRUE if handled. @widget is the widget
 * showing the link (for the parent window).
 */
typedef gboolean (*PidginMarkupLinkActivateFunc)(GtkWidget *widget,
                                                 const char *uri,
                                                 gpointer data);

/**
 * Adds context menu items for @uri to @menu. Items' actions must be
 * "link.NAME" with the actions added to @actions (the menu's "link"
 * action group). Return TRUE if items were added.
 */
typedef gboolean (*PidginMarkupLinkMenuFunc)(GtkWidget *widget,
                                             const char *uri, GMenu *menu,
                                             GActionMap *actions,
                                             gpointer data);

/**
 * Registers a URI scheme prefix, e.g. "http://", "mailto:", "open://".
 * A NULL @activate unregisters it. The longest matching prefix wins; the
 * match is case-insensitive. Returns FALSE if @scheme is invalid.
 */
gboolean pidgin_markup_register_scheme(const char *scheme,
                                       PidginMarkupLinkActivateFunc activate,
                                       PidginMarkupLinkMenuFunc context_menu,
                                       gpointer data);

/** TRUE if @uri starts with a registered scheme. */
gboolean pidgin_markup_uri_is_known(const char *uri);

/**
 * Handles a click on @uri: the registered handler, else (for http(s)
 * and friends) pidgin_open_uri(). Returns TRUE if something handled it.
 */
gboolean pidgin_markup_activate_uri(GtkWidget *widget, const char *uri);

/**
 * Adds the context menu items for @uri to @menu (see
 * PidginMarkupLinkMenuFunc). Falls back to "Open Link" and "Copy Link
 * Location". Returns TRUE if items were added.
 */
gboolean pidgin_markup_populate_link_menu(GtkWidget *widget, const char *uri,
                                          GMenu *menu, GActionMap *actions);

/** Registers the built-in schemes; idempotent. */
void pidgin_markup_init(void);
void pidgin_markup_uninit(void);

G_END_DECLS

#endif /* _PIDGINMARKUP_H_ */
