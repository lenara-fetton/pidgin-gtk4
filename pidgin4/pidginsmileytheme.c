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

#include "debug.h"
#include "imgstore.h"
#include "prefs.h"
#include "smiley.h"
#include "util.h"

#include "pidginanimation.h"
#include "pidginsmileytheme.h"

struct _PidginSmiley
{
	char *shortcut;
	char *file;
	gboolean hidden;
	GdkPaintable *paintable;
};

typedef struct
{
	char *sml;
	GSList *smileys;        /* PidginSmiley*, theme order */
	/* first byte of shortcut -> GSList of PidginSmiley, longest first */
	GHashTable *by_first;
} SmileyList;

typedef struct
{
	char *path;             /* the "theme" file */
	char *name;
	char *desc;
	char *author;
	char *icon;
	GList *lists;           /* SmileyList*, theme order; NULL until loaded */
} SmileyTheme;

static GList *themes = NULL;            /* SmileyTheme*, probed */
static SmileyTheme *current = NULL;     /* loaded; NULL = smileys off */
static guint theme_pref_cb = 0;

/**************************************************************************
 * Smileys
 **************************************************************************/

const char *
pidgin_smiley_get_shortcut(const PidginSmiley *smiley)
{
	return smiley->shortcut;
}

const char *
pidgin_smiley_get_file(const PidginSmiley *smiley)
{
	return smiley->file;
}

gboolean
pidgin_smiley_is_hidden(const PidginSmiley *smiley)
{
	return smiley->hidden;
}

GdkPaintable *
pidgin_smiley_get_paintable(PidginSmiley *smiley)
{
	g_return_val_if_fail(smiley != NULL, NULL);

	if (smiley->paintable == NULL) {
		/* Shortcuts often share an image; share the paintable too, so
		 * one animation drives all of them. */
		if (current != NULL) {
			GList *l;
			GSList *s;

			for (l = current->lists; l; l = l->next) {
				SmileyList *list = l->data;
				for (s = list->smileys; s; s = s->next) {
					PidginSmiley *other = s->data;
					if (other != smiley && other->paintable != NULL &&
					    purple_strequal(other->file, smiley->file)) {
						smiley->paintable = g_object_ref(other->paintable);
						return smiley->paintable;
					}
				}
			}
		}
		smiley->paintable = pidgin_paintable_new_from_file(smiley->file);
		if (smiley->paintable == NULL)
			purple_debug_warning("smileys", "Could not load %s\n", smiley->file);
	}

	return smiley->paintable;
}

static void
smiley_free(PidginSmiley *smiley)
{
	g_free(smiley->shortcut);
	g_free(smiley->file);
	g_clear_object(&smiley->paintable);
	g_free(smiley);
}

static gint
longest_first(gconstpointer a, gconstpointer b)
{
	const PidginSmiley *sa = a, *sb = b;

	return (int)strlen(sb->shortcut) - (int)strlen(sa->shortcut);
}

static void
smiley_list_free(SmileyList *list)
{
	g_free(list->sml);
	if (list->by_first)
		g_hash_table_destroy(list->by_first);
	g_slist_free_full(list->smileys, (GDestroyNotify)smiley_free);
	g_free(list);
}

static void
smiley_list_index(SmileyList *list)
{
	GSList *l;

	list->by_first = g_hash_table_new_full(g_direct_hash, g_direct_equal,
	                                       NULL, (GDestroyNotify)g_slist_free);
	for (l = list->smileys; l; l = l->next) {
		PidginSmiley *smiley = l->data;
		gpointer key = GUINT_TO_POINTER((guchar)smiley->shortcut[0]);
		GSList *bucket = g_hash_table_lookup(list->by_first, key);

		g_hash_table_steal(list->by_first, key);
		bucket = g_slist_insert_sorted(bucket, smiley, longest_first);
		g_hash_table_insert(list->by_first, key, bucket);
	}
}

/**************************************************************************
 * Themes
 **************************************************************************/

static void
theme_unload(SmileyTheme *theme)
{
	g_list_free_full(theme->lists, (GDestroyNotify)smiley_list_free);
	theme->lists = NULL;
}

static void
theme_free(SmileyTheme *theme)
{
	theme_unload(theme);
	g_free(theme->path);
	g_free(theme->name);
	g_free(theme->desc);
	g_free(theme->author);
	g_free(theme->icon);
	g_free(theme);
}

/*
 * Parses a theme file as pidgin/gtkthemes.c does: Name=, Description=,
 * Icon=, Author= headers, [section]s, and "file shortcut shortcut..."
 * lines (a leading "! " hides them from the picker, a backslash escapes
 * the next character). With @load FALSE only the headers are read.
 */
static SmileyTheme *
theme_parse(const char *path, gboolean load)
{
	FILE *f = g_fopen(path, "rb");
	SmileyTheme *theme;
	SmileyList *list = NULL;
	char *dirname;
	char buf[256];
	gsize line_nbr = 0;

	if (f == NULL)
		return NULL;

	theme = g_new0(SmileyTheme, 1);
	theme->path = g_strdup(path);
	dirname = g_path_get_dirname(path);

	while (fgets(buf, sizeof(buf), f) != NULL) {
		char *i;
		size_t len;

		line_nbr++;
		len = strlen(buf);
		while (len && (buf[len - 1] == '\r' || buf[len - 1] == '\n'))
			buf[--len] = '\0';
		if (len == 0 || buf[0] == '#')
			continue;

		if (!g_utf8_validate(buf, -1, NULL)) {
			purple_debug_error("smileys", "%s:%" G_GSIZE_FORMAT " is invalid UTF-8\n",
			                   path, line_nbr);
			continue;
		}

		i = buf;
		while (g_ascii_isspace(*i))
			i++;

		if (*i == '[' && strchr(i, ']')) {
			if (!load)
				continue;
			list = g_new0(SmileyList, 1);
			list->sml = g_strndup(i + 1, strchr(i, ']') - i - 1);
			theme->lists = g_list_append(theme->lists, list);
		} else if (!g_ascii_strncasecmp(i, "Name=", 5)) {
			g_free(theme->name);
			theme->name = g_strdup(i + 5);
		} else if (!g_ascii_strncasecmp(i, "Description=", 12)) {
			g_free(theme->desc);
			theme->desc = g_strdup(i + 12);
		} else if (!g_ascii_strncasecmp(i, "Icon=", 5)) {
			g_free(theme->icon);
			theme->icon = g_build_filename(dirname, i + 5, NULL);
		} else if (!g_ascii_strncasecmp(i, "Author=", 7)) {
			g_free(theme->author);
			theme->author = g_strdup(i + 7);
		} else if (load && list != NULL) {
			gboolean hidden = FALSE;
			char *file = NULL;

			if (i[0] == '!' && i[1] == ' ') {
				hidden = TRUE;
				i += 2;
			}
			while (*i) {
				GString *word = g_string_new(NULL);

				while (*i && !g_ascii_isspace(*i)) {
					const char *next;
					if (*i == '\\' && i[1] != '\0')
						i++;
					next = g_utf8_next_char(i);
					g_string_append_len(word, i, next - i);
					i = (char *)next;
				}
				if (word->len > 0) {
					if (file == NULL) {
						file = g_build_filename(dirname, word->str, NULL);
					} else {
						PidginSmiley *smiley = g_new0(PidginSmiley, 1);
						smiley->shortcut = g_strdup(word->str);
						smiley->file = g_strdup(file);
						smiley->hidden = hidden;
						list->smileys = g_slist_prepend(list->smileys, smiley);
					}
				}
				g_string_free(word, TRUE);
				while (g_ascii_isspace(*i))
					i++;
			}
			g_free(file);
		}
	}
	fclose(f);
	g_free(dirname);

	if (!theme->name || !theme->desc || !theme->author) {
		purple_debug_error("smileys", "Invalid smiley theme '%s'\n", path);
		theme_free(theme);
		return NULL;
	}

	if (load) {
		GList *l;
		for (l = theme->lists; l; l = l->next) {
			SmileyList *sl = l->data;
			sl->smileys = g_slist_reverse(sl->smileys);
			smiley_list_index(sl);
		}
	}

	return theme;
}

static void
probe_dir(const char *base)
{
	GDir *dir = g_dir_open(base, 0, NULL);
	const char *name;

	if (dir == NULL)
		return;

	while ((name = g_dir_read_name(dir)) != NULL) {
		char *path = g_build_filename(base, name, "theme", NULL);
		SmileyTheme *theme = NULL;
		GList *l;

		for (l = themes; l; l = l->next) {
			if (purple_strequal(((SmileyTheme *)l->data)->path, path))
				break;
		}
		if (l == NULL && g_file_test(path, G_FILE_TEST_IS_REGULAR))
			theme = theme_parse(path, FALSE);
		if (theme != NULL)
			themes = g_list_append(themes, theme);
		g_free(path);
	}
	g_dir_close(dir);
}

static void
probe_themes(void)
{
	char *dirs[3];
	int i;

	/* The libpurple prefix installs Pidgin 2's emotes; our own prefix
	 * may add more; the profile's smileys/ holds user themes (read
	 * only: pidgin4 never creates it, see the profile contract). */
	dirs[0] = g_build_filename(PURPLE_DATADIR, "pixmaps", "pidgin", "emotes", NULL);
	dirs[1] = g_build_filename(DATADIR, "pixmaps", "pidgin", "emotes", NULL);
	dirs[2] = g_build_filename(purple_user_dir(), "smileys", NULL);

	for (i = 0; i < 3; i++) {
		if (i == 1 && purple_strequal(dirs[0], dirs[1]))
			continue;
		probe_dir(dirs[i]);
	}
	for (i = 0; i < 3; i++)
		g_free(dirs[i]);
}

static void
set_current(SmileyTheme *theme)
{
	SmileyTheme *loaded;

	if (current != NULL) {
		theme_unload(current);
		current = NULL;
	}
	if (theme == NULL)
		return;

	loaded = theme_parse(theme->path, TRUE);
	if (loaded == NULL)
		return;
	theme->lists = loaded->lists;
	loaded->lists = NULL;
	theme_free(loaded);
	current = theme;
}

gboolean
pidgin_smiley_theme_load_file(const char *path)
{
	SmileyTheme *theme = theme_parse(path, FALSE);

	if (theme == NULL)
		return FALSE;
	themes = g_list_append(themes, theme);
	set_current(theme);
	return current == theme;
}

void
pidgin_smiley_theme_set_current(const char *name)
{
	GList *l;

	if (name != NULL && !g_ascii_strcasecmp(name, "none")) {
		set_current(NULL);
		return;
	}

	for (l = themes; l; l = l->next) {
		SmileyTheme *theme = l->data;
		if (purple_strequal(theme->name, name)) {
			set_current(theme);
			return;
		}
	}
	/* As Pidgin 2: no (known) theme chosen means the first one. */
	set_current(themes ? themes->data : NULL);
}

const char *
pidgin_smiley_theme_get_current(void)
{
	return current ? current->name : NULL;
}

gboolean
pidgin_smiley_themes_disabled(void)
{
	return current == NULL || !g_ascii_strcasecmp(current->name, "none");
}

GList *
pidgin_smiley_themes_get_names(void)
{
	GList *names = NULL, *l;

	for (l = themes; l; l = l->next)
		names = g_list_append(names, ((SmileyTheme *)l->data)->name);
	return names;
}

static SmileyList *
find_list(const char *sml)
{
	GList *l;
	SmileyList *def = NULL;

	if (current == NULL || current->lists == NULL)
		return NULL;

	for (l = current->lists; l; l = l->next) {
		SmileyList *list = l->data;
		if (sml != NULL && !g_ascii_strcasecmp(list->sml, sml))
			return list;
		if (def == NULL && !g_ascii_strcasecmp(list->sml, "default"))
			def = list;
	}
	return def ? def : current->lists->data;
}

GSList *
pidgin_smiley_theme_get_smileys(const char *sml)
{
	SmileyList *list;

	if (pidgin_smiley_themes_disabled())
		return NULL;
	list = find_list(sml);
	return list ? list->smileys : NULL;
}

gsize
pidgin_smiley_theme_match(const char *sml, const char *text, PidginSmiley **smiley)
{
	SmileyList *list;
	GSList *l;

	if (text == NULL || *text == '\0' || pidgin_smiley_themes_disabled())
		return 0;
	list = find_list(sml);
	if (list == NULL || list->by_first == NULL)
		return 0;

	for (l = g_hash_table_lookup(list->by_first, GUINT_TO_POINTER((guchar)*text));
	     l; l = l->next) {
		PidginSmiley *s = l->data;
		gsize len = strlen(s->shortcut);
		if (strncmp(text, s->shortcut, len) == 0) {
			if (smiley)
				*smiley = s;
			return len;
		}
	}
	return 0;
}

PidginSmiley *
pidgin_smiley_theme_lookup(const char *sml, const char *shortcut)
{
	GSList *l;

	for (l = pidgin_smiley_theme_get_smileys(sml); l; l = l->next) {
		PidginSmiley *s = l->data;
		if (purple_strequal(s->shortcut, shortcut))
			return s;
	}
	return NULL;
}

/**************************************************************************
 * Custom smileys
 **************************************************************************/

gsize
pidgin_custom_smiley_match(const char *text, PurpleSmiley **smiley)
{
	GList *all, *l;
	gsize best = 0;

	if (text == NULL || *text == '\0')
		return 0;

	all = purple_smileys_get_all();
	for (l = all; l; l = l->next) {
		const char *shortcut = purple_smiley_get_shortcut(l->data);
		gsize len = shortcut ? strlen(shortcut) : 0;

		if (len > best && strncmp(text, shortcut, len) == 0) {
			best = len;
			if (smiley)
				*smiley = l->data;
		}
	}
	g_list_free(all);

	return best;
}

gsize
pidgin_custom_smiley_match_func(const char *sml, const char *text, gpointer data)
{
	return pidgin_custom_smiley_match(text, NULL);
}

GdkPaintable *
pidgin_custom_smiley_get_paintable(PurpleSmiley *smiley)
{
	GdkPaintable *paintable;
	const char *checksum;
	gconstpointer data;
	size_t len = 0;

	g_return_val_if_fail(smiley != NULL, NULL);

	/* Cached per smiley and per image (the checksum changes when the
	 * user replaces the image in the smiley manager). */
	checksum = purple_smiley_get_checksum(smiley);
	paintable = g_object_get_data(G_OBJECT(smiley), "pidgin4-paintable");
	if (paintable != NULL &&
	    purple_strequal(g_object_get_data(G_OBJECT(smiley), "pidgin4-checksum"), checksum))
		return paintable;

	data = purple_smiley_get_data(smiley, &len);
	paintable = pidgin_paintable_new_from_data(data, len);
	g_object_set_data_full(G_OBJECT(smiley), "pidgin4-paintable", paintable,
	                       paintable ? g_object_unref : NULL);
	g_object_set_data_full(G_OBJECT(smiley), "pidgin4-checksum",
	                       g_strdup(checksum), g_free);

	return paintable;
}

/**************************************************************************
 * Init
 **************************************************************************/

static void
theme_pref_changed(const char *name, PurplePrefType type, gconstpointer value,
                   gpointer data)
{
	pidgin_smiley_theme_set_current(value);
}

void
pidgin_smiley_themes_init(void)
{
	const char *pref = PIDGIN_PREFS_ROOT "/smileys/theme";

	probe_themes();

	/* Shared with Pidgin 2 (same meaning): read, never registered or
	 * written here; the theme chooser is M5. */
	pidgin_smiley_theme_set_current(purple_prefs_exists(pref) ?
	                                purple_prefs_get_string(pref) : "Default");
	if (purple_prefs_exists(pref))
		theme_pref_cb = purple_prefs_connect_callback(&themes, pref,
		                                              theme_pref_changed, NULL);
}

void
pidgin_smiley_themes_uninit(void)
{
	if (theme_pref_cb != 0) {
		purple_prefs_disconnect_callback(theme_pref_cb);
		theme_pref_cb = 0;
	}
	current = NULL;
	g_list_free_full(themes, (GDestroyNotify)theme_free);
	themes = NULL;
}
