/*
 * pidgin4
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
 * The About window (Pidgin 2's About and Build Information dialogs and
 * the credits of pidgin/gtkdialogs.c): pages About, Build Information
 * and Credits in a GtkStack.
 */
#include "pidgin-internal.h"
#include "pidgin.h"
#include "package_revision.h"

#include <gtksourceview/gtksource.h>
#include <libsoup/soup.h>
#include <libspelling.h>
#include <sqlite3.h>

#include "core.h"
#include "plugin.h"
#include "sslconn.h"
#include "util.h"

#include "gtkutils.h"
#include "pidginabout.h"
#include "pidginrichlabel.h"
#include "pidginselftest.h"

typedef struct {
	const char *name;
	const char *role;       /* or the language, for translators */
} Credit;

/* From pidgin/gtkdialogs.c (Pidgin 2.14.14), names only. */
static const Credit developers[] = {
	{ "Gary 'grim' Kramlich", N_("lead developer") },
	{ "Richard 'rlaager' Laager", NULL },
	{ "Eion Robb", NULL },
	{ "Elliott 'QuLogic' Sales de Andrade", NULL },
	{ NULL, NULL }
};

static const Credit patch_writers[] = {
	{ "Markus 'ivanhoe' Fischer", NULL },
	{ NULL, NULL }
};

static const Credit retired_developers[] = {
	{ "Daniel 'datallah' Atallah", NULL },
	{ "Paul 'darkrain42' Aurich", NULL },
	{ "John 'rekkanoryo' Bailey", NULL },
	{ "Ethan 'Paco-Paco' Blanton", NULL },
	{ "Herman Bloggs", N_("win32 port") },
	{ "Hylke Bons", N_("artist") },
	{ "Thomas Butter", NULL },
	{ "Sadrul Habib Chowdhury", NULL },
	{ "Mark 'KingAnt' Doliner", NULL },
	{ "Jim Duchek", N_("maintainer") },
	{ "Sean Egan", NULL },
	{ "Rob Flynn", N_("maintainer") },
	{ "Adam Fritzler", N_("libfaim maintainer") },
	{ "Christian 'ChipX86' Hammond", N_("webmaster") },
	{ "Casey Harkins", NULL },
	{ "Ivan Komarov", NULL },
	{ "Syd Logan", N_("hacker and designated driver [lazy bum]") },
	{ "Marcus 'malu' Lundblad", NULL },
	{ "Sulabh 'sulabh_m' Mahajan", NULL },
	{ "Richard 'wabz' Nelson", NULL },
	{ "Christopher 'siege' O'Brien", NULL },
	{ "Bartosz Oler", NULL },
	{ "Etan 'deryni' Reisner", NULL },
	{ "Tim 'marv' Ringenbach", NULL },
	{ "Michael 'Maiku' Ruprecht", N_("voice and video") },
	{ "Luke 'LSchiere' Schierer", N_("support") },
	{ "Megan 'Cae' Schneider", N_("support/QA") },
	{ "Evan Schoenberg", NULL },
	{ "Jim Seymour", N_("XMPP") },
	{ "Mark Spencer", N_("original author") },
	{ "Kevin 'SimGuy' Stange", N_("webmaster") },
	{ "Will 'resiak' Thompson", NULL },
	{ "Stu 'nosnilmot' Tomlinson", NULL },
	{ "Jorge 'Masca' Villaseñor", NULL },
	{ "Nathan 'faceprint' Walp", NULL },
	{ "Eric Warmenhoven", N_("lead developer") },
	{ "Tomasz Wasilczyk", NULL },
	{ NULL, NULL }
};

static const Credit retired_patch_writers[] = {
	{ "Jakub 'haakon' Adam", NULL },
	{ "Felipe 'shx' Contreras", NULL },
	{ "Decklin Foster", NULL },
	{ "Krzysztof Klinikowski", NULL },
	{ "Peter 'Bleeter' Lawler", NULL },
	{ "Robert 'Robot101' McQueen", NULL },
	{ "Benjamin Miller", NULL },
	{ "Dennis 'EvilDennisR' Ristuccia", N_("Senior Contributor/QA") },
	{ "Peter 'Fmoo' Ruibal", NULL },
	{ "Gabriel 'Nix' Schulhof", NULL },
	{ NULL, NULL }
};

static const Credit translators[] = {
	{ "Samuel Murray", N_("Afrikaans") },
	{ "Friedel Wolff", N_("Afrikaans") },
	{ "Khaled Hosny", N_("Arabic") },
	{ "Amitakhya Phukan", N_("Assamese") },
	{ "Llumex03", N_("Asturian") },
	{ "Ihar Hrachyshka", N_("Belarusian Latin") },
	{ "Vladimira Girginova", N_("Bulgarian") },
	{ "Vladimir (Kaladan) Petkov", N_("Bulgarian") },
	{ "Jamil Ahmed", N_("Bengali") },
	{ "Israt Jahan", N_("Bengali") },
	{ "Samia Nimatullah", N_("Bengali") },
	{ "Runa Bhattacharjee", N_("Bengali-India") },
	{ "Gwenn Meynier", N_("Breton") },
	{ "Chandrakant Dhutadmal", N_("Bodo") },
	{ "Lejla Hadzialic", N_("Bosnian") },
	{ "Josep Puigdemont", N_("Catalan") },
	{ "Toni Hermoso", N_("Valencian-Catalan") },
	{ "Josep Puigdemont", N_("Valencian-Catalan") },
	{ "David Vachulka", N_("Czech") },
	{ "Nicky Thomassen", N_("Danish") },
	{ "Björn Voigt", N_("German") },
	{ "Norbu", N_("Dzongkha") },
	{ "Jurmey Rabgay", N_("Dzongkha") },
	{ "Wangmo Sherpa", N_("Dzongkha") },
	{ "Katsaloulis Panayotis", N_("Greek") },
	{ "Panos Bouklis", N_("Greek") },
	{ "Michael Findlay", N_("Australian English") },
	{ "Phil Hannent", N_("British English") },
	{ "Adam Weinberger", N_("Canadian English") },
	{ "Stéphane Fillod", N_("Esperanto") },
	{ "Javier Fernández-Sanguino Peña", N_("Spanish") },
	{ "KNTRO", N_("Argentine Spanish") },
	{ "Ivar Smolin", N_("Estonian") },
	{ "Mikel Pascual Aldabaldetreku", N_("Basque") },
	{ "Elnaz Sarbar", N_("Persian") },
	{ "Roozbeh Pournader", N_("Persian") },
	{ "Meelad Zakaria", N_("Persian") },
	{ "Timo Jyrinki", N_("Finnish") },
	{ "Aaron Kearns", N_("Irish") },
	{ "Kevin Scannell", N_("Irish") },
	{ "Mar Castro", N_("Galician") },
	{ "Frco. Javier Rial", N_("Galician") },
	{ "Ankit Patel", N_("Gujarati") },
	{ "Shalom Craimer", N_("Hebrew") },
	{ "Sangeeta Kumari", N_("Hindi") },
	{ "Rajesh Ranjan", N_("Hindi") },
	{ "Sabina Drempetić", N_("Croatian") },
	{ "Kelemen Gábor", N_("Hungarian") },
	{ "Rai S. Regawa", N_("Indonesian") },
	{ "Claudio Satriano", N_("Italian") },
	{ "Takayuki Kusano", N_("Japanese") },
	{ "Baurzhan Muftakhidinov", N_("Kazakh") },
	{ "Khoem Sokhem", N_("Khmer") },
	{ "Sushizang", N_("Korean") },
	{ "Chandrakant Dhutadmal", N_("Kashmiri") },
	{ "Amed Ç. Jiyan", N_("Kurdish") },
	{ "Erdal Ronahi", N_("Kurdish") },
	{ "Rizoyê Xerzî", N_("Kurdish") },
	{ "Haval A. Ahmed", N_("Kurdish (Sorani)") },
	{ "Algimantas Margevičius", N_("Lithuanian") },
	{ "Rudolfs Mazurs", N_("Latvian") },
	{ "Ingmārs Dīriņš", N_("Latvian") },
	{ "Sangeeta Kumari", N_("Maithili") },
	{ "Rajesh Ranjan", N_("Maithili") },
	{ "David Preece", N_("Meadow Mari") },
	{ "Arangel Angov ", N_("Macedonian") },
	{ "Ivana Kirkovska", N_("Macedonian") },
	{ "Jovan Naumovski", N_("Macedonian") },
	{ "abuyop", N_("Malay") },
	{ "Ani Peter", N_("Malayalam") },
	{ "gooyo", N_("Mongolian") },
	{ "Sandeep Shedmake", N_("Marathi") },
	{ "Thura Hlaing", N_("Burmese") },
	{ "Allan Nordhøy", N_("Bokmål Norwegian") },
	{ "Saroj Dhakal", N_("Nepali") },
	{ "Gideon van Melle", N_("Dutch, Flemish") },
	{ "Yngve Spjeld Landro", N_("Norwegian Nynorsk") },
	{ "Cédric Valmary", N_("Occitan") },
	{ "Manoj Kumar Giri", N_("Oriya") },
	{ "Amanpreet Singh Alam", N_("Punjabi") },
	{ "Piotr Drąg", N_("Polish") },
	{ "Paulo Ribeiro", N_("Portuguese") },
	{ "Renato Silva", N_("Portuguese-Brazil") },
	{ "Kashif Masood", N_("Pashto") },
	{ "Mișu Moldovan", N_("Romanian") },
	{ "Andrei Popescu", N_("Romanian") },
	{ "Антон Самохвалов", N_("Russian") },
	{ "Chandrakant Dhutadmal", N_("Sindhi") },
	{ "Jozef Káčer", N_("Slovak") },
	{ "loptosko", N_("Slovak") },
	{ "Martin Srebotnjak", N_("Slovenian") },
	{ "Besnik Bleta", N_("Albanian") },
	{ "Miloš Popović", N_("Serbian") },
	{ "Miloš Popović", N_("Serbian Latin") },
	{ "Yajith Ajantha Dayarathna", N_("Sinhala") },
	{ "Danishka Navin", N_("Sinhala") },
	{ "Josef Andersson", N_("Swedish") },
	{ "Paul Msegeya", N_("Swahili") },
	{ "I. Felix", N_("Tamil") },
	{ "Viveka Nathan K", N_("Tamil") },
	{ "Krishnababu Krottapalli", N_("Telugu") },
	{ "Isriya Paireepairit", N_("Thai") },
	{ "ILDAR Valeev", N_("Tatar") },
	{ "Oleksandr Kovalenko", N_("Ukranian") },
	{ "RKVS Raman", N_("Urdu") },
	{ "Nguyễn Vũ Hưng", N_("Vietnamese") },
	{ "Aron Xu", N_("Simplified Chinese") },
	{ "Abel Cheung", N_("Hong Kong Chinese") },
	{ "Ambrose C. Li", N_("Hong Kong Chinese") },
	{ "Paladin R. Liu", N_("Hong Kong Chinese") },
	{ "Ambrose C. Li", N_("Traditional Chinese") },
	{ "Paladin R. Liu", N_("Traditional Chinese") },
	{ NULL, NULL }
};

static const Credit past_translators[] = {
	{ "Daniel Yacob", N_("Amharic") },
	{ "Mohamed Magdy", N_("Arabic") },
	{ "Hristo Todorov", N_("Bulgarian") },
	{ "Indranil Das Gupta", N_("Bengali") },
	{ "Tisa Nafisa", N_("Bengali") },
	{ "JM Pérez Cáncer", N_("Catalan") },
	{ "Robert Millan", N_("Catalan") },
	{ "Honza Král", N_("Czech") },
	{ "Miloslav Trmac", N_("Czech") },
	{ "Peter Bach", N_("Danish") },
	{ "Morten Brix Pedersen", N_("Danish") },
	{ "Daniel Seifert, Karsten Weiss", N_("German") },
	{ "Jochen Kemnade", N_("German") },
	{ "Peter Lawler", N_("Australian English") },
	{ "Luke Ross", N_("British English") },
	{ "JM Pérez Cáncer", N_("Spanish") },
	{ "Nicolás Lichtmaier", N_("Spanish") },
	{ "Amaya Rodrigo", N_("Spanish") },
	{ "Alejandro G Villar", N_("Spanish") },
	{ "Iñaki Larrañaga Murgoitio", N_("Basque") },
	{ "Hizkuntza Politikarako Sailburuordetza", N_("Basque") },
	{ "Arto Alakulju", N_("Finnish") },
	{ "Tero Kuusela", N_("Finnish") },
	{ "Sébastien François", N_("French") },
	{ "Loïc Jeannin", N_("French") },
	{ "Stéphane Pontier", N_("French") },
	{ "Stéphane Wirtel", N_("French") },
	{ "Éric Boumaour", N_("French") },
	{ "Ignacio Casal Quinteiro", N_("Galician") },
	{ "Pavel Bibergal", N_("Hebrew") },
	{ "Ravishankar Shrivastava", N_("Hindi") },
	{ "Zoltan Sutto", N_("Hungarian") },
	{ "David Avsharyan", N_("Armenian") },
	{ "Salvatore di Maggio", N_("Italian") },
	{ "Takashi Aihana", N_("Japanese") },
	{ "Ryosuke Kutsuna", N_("Japanese") },
	{ "Junichi Uekawa", N_("Japanese") },
	{ "Taku Yasui", N_("Japanese") },
	{ "Temuri Doghonadze", N_("Georgian") },
	{ "Sang-hyun S, A Ho-seok Lee", N_("Korean") },
	{ "Kyeong-uk Son", N_("Korean") },
	{ "Anousak Souphavah", N_("Lao") },
	{ "Laurynas Biveinis", N_("Lithuanian") },
	{ "Gediminas Čičinskas", N_("Lithuanian") },
	{ "Andrius Štikonas", N_("Lithuanian") },
	{ "Tomislav Markovski", N_("Macedonian") },
	{ "Muhammad Najmi bin Ahmad Zabidi", N_("Malay") },
	{ "Hans Fredrik Nordhaug", N_("Bokmål Norwegian") },
	{ "Hallvard Glad", N_("Bokmål Norwegian") },
	{ "Petter Johan Olsen", N_("Bokmål Norwegian") },
	{ "Espen Stefansen", N_("Bokmål Norwegian") },
	{ "Shyam Krishna Bal", N_("Nepali") },
	{ "Vincent van Adrighem", N_("Dutch, Flemish") },
	{ "Yannig Marchegay", N_("Occitan") },
	{ "Krzysztof Foltman", N_("Polish") },
	{ "Paweł Godlewski", N_("Polish") },
	{ "Piotr Makowski", N_("Polish") },
	{ "Emil Nowak", N_("Polish") },
	{ "Przemysław Sułek", N_("Polish") },
	{ "Duarte Henriques", N_("Portuguese") },
	{ "Maurício de Lemos Rodrigues Collares Neto", N_("Portuguese-Brazil") },
	{ "Rodrigo Luiz Marques Flores", N_("Portuguese-Brazil") },
	{ "Dmitry Beloglazov", N_("Russian") },
	{ "Alexandre Prokoudine", N_("Russian") },
	{ "Sergey Volozhanin", N_("Russian") },
	{ "Daniel Režný", N_("Slovak") },
	{ "Richard Golier", N_("Slovak") },
	{ "helix84", N_("Slovak") },
	{ "Matjaz Horvat", N_("Slovenian") },
	{ "Danilo Šegan", N_("Serbian") },
	{ "Aleksandar Urosevic", N_("Serbian") },
	{ "Peter Hjalmarsson", N_("Swedish") },
	{ "Tore Lundqvist", N_("Swedish") },
	{ "Christian Rose", N_("Swedish") },
	{ "Mr. Subbaramaih", N_("Telugu") },
	{ "Serdar Soytetir", N_("Turkish") },
	{ "Ahmet Alp Balkan", N_("Turkish") },
	{ "Hashao, Rocky S. Lee", N_("Simplified Chinese") },
	{ "Funda Wang", N_("Simplified Chinese") },
	{ "Hashao, Rocky S. Lee", N_("Traditional Chinese") },
	{ NULL, NULL }
};

typedef struct {
	GtkWidget *window;
	GtkWidget *stack;
	GtkWidget *build_label;
} AboutWindow;

static AboutWindow *about = NULL;

/**************************************************************************
 * Content
 **************************************************************************/

static void
append_row(GString *str, const char *label, const char *value)
{
	char *v = g_markup_escape_text(value ? value : "", -1);

	g_string_append_printf(str, "<b>%s</b> %s<br>", label, v);
	g_free(v);
}

static char *
version_string(guint major, guint minor, guint micro)
{
	return g_strdup_printf("%u.%u.%u", major, minor, micro);
}

static char *
about_html(void)
{
	GString *str = g_string_new(NULL);
	char *tmp;

	g_string_append_printf(str, "<b>libpurple</b> %s<br>", purple_core_get_version());
	tmp = version_string(gtk_get_major_version(), gtk_get_minor_version(),
	                     gtk_get_micro_version());
	append_row(str, "GTK", tmp);
	g_free(tmp);
	tmp = version_string(glib_major_version, glib_minor_version, glib_micro_version);
	append_row(str, "GLib", tmp);
	g_free(tmp);
	tmp = version_string(gtk_source_get_major_version(), gtk_source_get_minor_version(),
	                     gtk_source_get_micro_version());
	append_row(str, "GtkSourceView", tmp);
	g_free(tmp);
	append_row(str, "libspelling", SPELLING_VERSION_S);
	g_string_append(str, "<br>");

	g_string_append_printf(str, _("%s is a messaging client based on libpurple which is capable of "
		  "connecting to multiple messaging services at once.  %s is written "
		  "in C using GTK+.  %s is released, and may be modified and "
		  "redistributed,  under the terms of the GPL version 2 (or later).  "
		  "A copy of the GPL is distributed with %s.  %s is copyrighted by "
		  "its contributors, a list of whom is also distributed with %s.  "
		  "There is no warranty for %s.<BR><BR>"), PIDGIN_NAME, PIDGIN_NAME,
		PIDGIN_NAME, PIDGIN_NAME, PIDGIN_NAME, PIDGIN_NAME, PIDGIN_NAME);
	g_string_append(str, "This is a personal GTK 4 build of it (Pidgin 4, "
		PIDGIN4_APP_ID ").<br><br>");
	g_string_append_printf(str, "<a href=\"%s\">%s</a>", PURPLE_WEBSITE, PURPLE_WEBSITE);

	return g_string_free(str, FALSE);
}

static void
append_plugins(GString *str, GList *plugins, gboolean only_loaded)
{
	GList *l;
	gboolean first = TRUE;

	for (l = plugins; l != NULL; l = l->next) {
		PurplePlugin *plug = l->data;
		char *line;

		if (only_loaded && !purple_plugin_is_loaded(plug))
			continue;
		line = g_markup_printf_escaped("%s%s %s (%s)", first ? "" : ", ",
			purple_plugin_get_name(plug) ? purple_plugin_get_name(plug) : "?",
			purple_plugin_get_version(plug) ? purple_plugin_get_version(plug) : "",
			purple_plugin_get_id(plug));
		g_string_append(str, line);
		g_free(line);
		first = FALSE;
	}
	if (first)
		g_string_append(str, "none");
}

char *
pidgin_about_get_build_info_html(void)
{
	GString *str = g_string_new(NULL);
	PurplePlugin *omemo;
	GList *l, *ssl = NULL;
	GdkDisplay *display;
	char *tmp, *pidgin4_plugins;

	/* Not translated: for bug reports, like Pidgin 2's. */
	g_string_append_printf(str, "<b>%s %s</b> (libpurple %s)<br>%s<br><br>",
		"Pidgin 4", DISPLAY_VERSION, purple_core_get_version(), REVISION);

	g_string_append(str, "<b>Compiled with</b><br>");
	g_string_append_printf(str, "GTK %d.%d.%d, GLib %d.%d.%d, GtkSourceView %d.%d.%d, "
		"libspelling %s, libsoup %d.%d.%d, SQLite %s<br><br>",
		GTK_MAJOR_VERSION, GTK_MINOR_VERSION, GTK_MICRO_VERSION,
		GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION,
		GTK_SOURCE_MAJOR_VERSION, GTK_SOURCE_MINOR_VERSION, GTK_SOURCE_MICRO_VERSION,
		SPELLING_VERSION_S, SOUP_MAJOR_VERSION, SOUP_MINOR_VERSION, SOUP_MICRO_VERSION,
		SQLITE_VERSION);

	g_string_append(str, "<b>Running with</b><br>");
	g_string_append_printf(str, "GTK %u.%u.%u, GLib %u.%u.%u, GtkSourceView %u.%u.%u, "
		"libsoup %u.%u.%u, SQLite %s",
		gtk_get_major_version(), gtk_get_minor_version(), gtk_get_micro_version(),
		glib_major_version, glib_minor_version, glib_micro_version,
		gtk_source_get_major_version(), gtk_source_get_minor_version(),
		gtk_source_get_micro_version(),
		soup_get_major_version(), soup_get_minor_version(), soup_get_micro_version(),
		sqlite3_libversion());
	display = gdk_display_get_default();
	if (display != NULL) {
		tmp = g_markup_escape_text(G_OBJECT_TYPE_NAME(display), -1);
		g_string_append_printf(str, "; display: %s", tmp);
		g_free(tmp);
	}
	g_string_append(str, "<br><br>");

	g_string_append(str, "<b>Paths</b><br>");
	append_row(str, "libpurple plugins:", PURPLE_LIBDIR);
	append_row(str, "pidgin4 plugins (prefix):", LIBDIR);
	pidgin4_plugins = g_build_filename(purple_user_dir(), PIDGIN4_PROFILE_SUBDIR,
	                                   "plugins", NULL);
	append_row(str, "pidgin4 plugins (profile):", pidgin4_plugins);
	g_free(pidgin4_plugins);
	append_row(str, "Data:", DATADIR);
	append_row(str, "Locale:", LOCALEDIR);
	append_row(str, "Profile:", purple_user_dir());
	g_string_append(str, "<br>");

	g_string_append(str, "<b>Features</b><br>");
	for (l = purple_plugins_get_loaded(); l != NULL; l = l->next) {
		PurplePlugin *plug = l->data;

		if (g_str_has_prefix(purple_plugin_get_id(plug), "ssl-"))
			ssl = g_list_append(ssl, plug);
	}
	g_string_append_printf(str, "<b>SSL:</b> %s (",
		purple_ssl_is_supported() ? "yes" : "no");
	append_plugins(str, ssl, TRUE);
	g_string_append(str, ")<br>");
	g_list_free(ssl);

	g_string_append(str, "<b>Protocols:</b> ");
	append_plugins(str, purple_plugins_get_protocols(), FALSE);
	g_string_append(str, "<br>");

	omemo = purple_plugins_find_with_id("core-omemo");
	g_string_append_printf(str, "<b>OMEMO plugin:</b> %s<br>",
		omemo == NULL ? "not found" :
		purple_plugin_is_loaded(omemo) ? "loaded" : "available, not loaded");
	g_string_append(str, "<b>D-Bus:</b> disabled<br>");
	g_string_append(str, "<b>Voice and video:</b> disabled<br>");
	g_string_append(str, "<b>UI:</b> " PIDGIN_UI ", " PIDGIN4_APP_ID "<br>");

	return g_string_free(str, FALSE);
}

static void
append_credits(GString *str, const char *title, const Credit *list, gboolean translators)
{
	int i;

	g_string_append_printf(str, "<font size=\"4\"><b>%s</b></font><br>", title);
	for (i = 0; list[i].name != NULL; i++) {
		char *name = g_markup_escape_text(list[i].name, -1);

		if (translators) {
			char *lang = g_markup_escape_text(_(list[i].role), -1);
			g_string_append_printf(str, "%s: %s<br>", lang, name);
			g_free(lang);
		} else if (list[i].role != NULL) {
			char *role = g_markup_escape_text(_(list[i].role), -1);
			g_string_append_printf(str, "%s (%s)<br>", name, role);
			g_free(role);
		} else {
			g_string_append_printf(str, "%s<br>", name);
		}
		g_free(name);
	}
	g_string_append(str, "<br>");
}

static char *
credits_html(void)
{
	GString *str = g_string_new(NULL);

	append_credits(str, _("Current Developers"), developers, FALSE);
	append_credits(str, _("Crazy Patch Writers"), patch_writers, FALSE);
	append_credits(str, _("Retired Developers"), retired_developers, FALSE);
	append_credits(str, _("Retired Crazy Patch Writers"), retired_patch_writers, FALSE);
	append_credits(str, _("Current Translators"), translators, TRUE);
	append_credits(str, _("Past Translators"), past_translators, TRUE);
	return g_string_free(str, FALSE);
}

/**************************************************************************
 * The window
 **************************************************************************/

static GtkWidget *
rich_page(const char *html, GtkWidget **label_out)
{
	GtkWidget *label = pidgin_rich_label_new();
	GtkWidget *sw;

	pidgin_rich_label_set_force_text_view(PIDGIN_RICH_LABEL(label), TRUE);
	pidgin_rich_label_set_html(PIDGIN_RICH_LABEL(label), html, NULL);
	sw = pidgin_make_scrollable(label, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC, -1, -1);
	gtk_widget_set_vexpand(sw, TRUE);
	if (label_out != NULL)
		*label_out = label;
	return sw;
}

static void
copy_cb(GtkWidget *button, gpointer data)
{
	GtkWidget *label = data;

	gdk_clipboard_set_text(gtk_widget_get_clipboard(label),
		pidgin_rich_label_get_text(PIDGIN_RICH_LABEL(label)));
}

static void
close_cb(GtkWidget *button, gpointer data)
{
	gtk_window_destroy(GTK_WINDOW(about->window));
}

static void
about_destroy_cb(GtkWidget *window, gpointer data)
{
	g_clear_pointer(&about, g_free);
}

void
pidgin_about_show(void)
{
	GtkWidget *win, *content, *switcher, *page, *image, *label, *box, *button;
	char *html, *title, *markup;

	if (about != NULL) {
		gtk_window_present(GTK_WINDOW(about->window));
		return;
	}

	about = g_new0(AboutWindow, 1);
	title = g_strdup_printf(_("About %s"), "Pidgin 4");
	about->window = win = pidgin_dialog_new(title, NULL,
	                                        "about", TRUE);
	g_free(title);
	gtk_window_set_default_size(GTK_WINDOW(win), 560, 560);
	g_signal_connect(win, "destroy", G_CALLBACK(about_destroy_cb), NULL);
	content = pidgin_dialog_get_content_area(win);

	about->stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(about->stack),
	                              GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_widget_set_vexpand(about->stack, TRUE);
	switcher = gtk_stack_switcher_new();
	gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(about->stack));
	gtk_widget_set_halign(switcher, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(content), switcher);
	gtk_box_append(GTK_BOX(content), about->stack);

	/* About */
	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BOX_SPACE);
	image = gtk_image_new_from_icon_name(PIDGIN4_APP_ID);
	gtk_image_set_pixel_size(GTK_IMAGE(image), 96);
	gtk_box_append(GTK_BOX(box), image);
	label = gtk_label_new(NULL);
	markup = g_markup_printf_escaped("<span size=\"x-large\" weight=\"bold\">Pidgin 4</span>\n%s (%s)",
	                                 DISPLAY_VERSION, REVISION);
	gtk_label_set_markup(GTK_LABEL(label), markup);
	gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
	gtk_label_set_selectable(GTK_LABEL(label), TRUE);
	g_free(markup);
	gtk_box_append(GTK_BOX(box), label);
	html = about_html();
	page = rich_page(html, NULL);
	g_free(html);
	gtk_box_append(GTK_BOX(box), page);
	gtk_stack_add_titled(GTK_STACK(about->stack), box, "about", _("About"));

	/* Build Information */
	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BOX_SPACE);
	html = pidgin_about_get_build_info_html();
	page = rich_page(html, &about->build_label);
	g_free(html);
	gtk_box_append(GTK_BOX(box), page);
	button = gtk_button_new_with_mnemonic(_("_Copy"));
	gtk_widget_set_halign(button, GTK_ALIGN_END);
	gtk_widget_set_tooltip_text(button, _("Copy the build information to the clipboard"));
	g_signal_connect(button, "clicked", G_CALLBACK(copy_cb), about->build_label);
	g_object_set_data(G_OBJECT(about->stack), "pidgin-copy-button", button);
	gtk_box_append(GTK_BOX(box), button);
	gtk_stack_add_titled(GTK_STACK(about->stack), box, "build", _("Build Information"));

	/* Credits */
	html = credits_html();
	page = rich_page(html, NULL);
	g_free(html);
	gtk_stack_add_titled(GTK_STACK(about->stack), page, "credits", _("Credits"));

	pidgin_dialog_add_button(win, _("_Close"), G_CALLBACK(close_cb), NULL);

	pidgin_window_set_secondary(GTK_WINDOW(win));
	gtk_window_present(GTK_WINDOW(win));
}

/**************************************************************************
 * Selftest
 **************************************************************************/

#define MODULE "about"

void
pidgin_about_selftest(void)
{
	static const char *pages[] = { "about", "build", "credits" };
	const char *text;
	gsize i;

	pidgin_about_show();
	pidgin_selftest_iterate(100);
	if (about == NULL) {
		pidgin_selftest_fail(MODULE, "the window did not open");
		return;
	}
	for (i = 0; i < G_N_ELEMENTS(pages); i++) {
		GtkWidget *child;

		gtk_stack_set_visible_child_name(GTK_STACK(about->stack), pages[i]);
		pidgin_selftest_iterate(100);
		child = gtk_stack_get_visible_child(GTK_STACK(about->stack));
		if (child == NULL ||
		    !purple_strequal(gtk_stack_get_visible_child_name(GTK_STACK(about->stack)), pages[i]))
			pidgin_selftest_fail(MODULE, "page %s not shown", pages[i]);
	}

	text = pidgin_rich_label_get_text(PIDGIN_RICH_LABEL(about->build_label));
	if (text == NULL || strstr(text, "libpurple") == NULL || strstr(text, "Protocols") == NULL)
		pidgin_selftest_fail(MODULE, "the build information is incomplete");
	g_signal_emit_by_name(g_object_get_data(G_OBJECT(about->stack), "pidgin-copy-button"),
	                      "clicked");
	pidgin_selftest_iterate(50);
	if (gdk_clipboard_get_content(gtk_widget_get_clipboard(about->window)) == NULL)
		pidgin_selftest_fail(MODULE, "Copy did not set the clipboard");

	gtk_window_destroy(GTK_WINDOW(about->window));
	pidgin_selftest_iterate(50);
	if (about != NULL)
		pidgin_selftest_fail(MODULE, "the window did not close");
}
