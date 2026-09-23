/**
 * @file pidginsmileytheme.h Smiley themes and custom smileys
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
 * The minimal port of pidgin/gtkthemes.c: Pidgin 2 smiley themes (the
 * "theme" files in <prefix>/share/pixmaps/pidgin/emotes/<name>/ and
 * <profile>/smileys/<name>/), the current one chosen by the shared pref
 * /pidgin/smileys/theme ("none" disables smileys), plus libpurple's
 * custom smileys (purple_smileys_*). The theme manager UI is M5.
 */
#ifndef _PIDGINSMILEYTHEME_H_
#define _PIDGINSMILEYTHEME_H_

#include <gtk/gtk.h>

#include "smiley.h"

G_BEGIN_DECLS

typedef struct _PidginSmiley PidginSmiley;

/** The text that is replaced, e.g. ":-)". */
const char *pidgin_smiley_get_shortcut(const PidginSmiley *smiley);

/** The image file. */
const char *pidgin_smiley_get_file(const PidginSmiley *smiley);

/** Hidden smileys ("! " in the theme) are recognised but not in the picker. */
gboolean pidgin_smiley_is_hidden(const PidginSmiley *smiley);

/**
 * The smiley's image (a PidginAnimation for animated ones), loaded on
 * first use and shared by everything showing it. Transfer none.
 */
GdkPaintable *pidgin_smiley_get_paintable(PidginSmiley *smiley);

/** Loads the theme list and the current theme. */
void pidgin_smiley_themes_init(void);
void pidgin_smiley_themes_uninit(void);

/** Names of the installed themes (transfer container). */
GList *pidgin_smiley_themes_get_names(void);

/** The current theme's name, or NULL if smileys are off. */
const char *pidgin_smiley_theme_get_current(void);

/**
 * Loads theme @name (or the first one if not found) as the current
 * theme. "none" turns smileys off. Does not write the pref.
 */
void pidgin_smiley_theme_set_current(const char *name);

/** TRUE when the current theme is "none" or there is none. */
gboolean pidgin_smiley_themes_disabled(void);

/**
 * Loads the theme in @path (a "theme" file) as current; for tests and for
 * callers with their own theme directory.
 */
gboolean pidgin_smiley_theme_load_file(const char *path);

/**
 * The smileys of category @sml (a theme [section], e.g. "XMPP", matched
 * case-insensitively), falling back to [default]. Transfer none.
 */
GSList *pidgin_smiley_theme_get_smileys(const char *sml);

/**
 * Returns the length of the longest smiley shortcut of category @sml at
 * the start of @text, or 0; @smiley (may be NULL) gets the smiley.
 */
gsize pidgin_smiley_theme_match(const char *sml, const char *text,
                                PidginSmiley **smiley);

/** The smiley of category @sml with @shortcut, or NULL. */
PidginSmiley *pidgin_smiley_theme_lookup(const char *sml, const char *shortcut);

/**
 * Custom smileys (purple_smileys_*): the longest shortcut at the start of
 * @text, or 0. @smiley (may be NULL) gets it.
 */
gsize pidgin_custom_smiley_match(const char *text, PurpleSmiley **smiley);

/** A paintable for a custom smiley (cached per smiley). Transfer none. */
GdkPaintable *pidgin_custom_smiley_get_paintable(PurpleSmiley *smiley);

/**
 * A PidginMarkupSmileyMatchFunc (see pidginmarkup.h) over the custom
 * smileys; @data is unused.
 */
gsize pidgin_custom_smiley_match_func(const char *sml, const char *text,
                                      gpointer data);

G_END_DECLS

#endif /* _PIDGINSMILEYTHEME_H_ */
