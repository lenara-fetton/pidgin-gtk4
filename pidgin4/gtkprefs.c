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

/*
 * The preferences window (M5), a port of pidgin/gtkprefs.c: a
 * GtkStackSidebar with one page per topic. Every widget is bound to its
 * pref with PidginPrefBinding (pidginprefbinding.h), so the window has no
 * Apply/OK: changes take effect at once, and changes made elsewhere (the
 * buddy list menus, plugins) show up in an open window.
 *
 * Profile contract: pidgin4 uses the shared /pidgin/... and /purple/...
 * keys only where their meaning is Pidgin 2's, registering them with
 * Pidgin 2's types and defaults (so a profile Pidgin 2 has used gains
 * nothing). Where pidgin4 means something different (the sound method,
 * the browser, conversation placement, the send button) it uses its own
 * /pidgin4/... keys. See pidgin_prefs_init().
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "account.h"
#include "debug.h"
#include "log.h"
#include "network.h"
#include "prefs.h"
#include "savedstatuses.h"
#include "sound.h"
#include "util.h"

#include "gtkconv.h"
#include "gtkprefs.h"
#include "gtksound.h"
#include "gtkthemes.h"
#include "gtkutils.h"
#include "pidginbackfill.h"
#include "pidgincomposeentry.h"
#include "pidginimageencode.h"
#include "pidginmarkup.h"
#include "pidginmessageindex.h"
#include "pidginprefbinding.h"
#include "pidginrichlabel.h"
#include "pidginselftest.h"
#include "pidginsmileytheme.h"

#define CONV_PREFS   PIDGIN_PREFS_ROOT "/conversations"
#define SOUND_PREFS  PIDGIN_PREFS_ROOT "/sound"
#define SOUND4_PREFS PIDGIN4_PREFS_ROOT "/sound"
#define BROWSER4_PREFS PIDGIN4_PREFS_ROOT "/browser"
#define CONV4_PREFS  PIDGIN4_PREFS_ROOT "/conversations"

static GtkWidget *prefs_window = NULL;
static GtkWidget *prefs_stack = NULL;

/* Every pref a page binds, for the selftest's "opening changes nothing". */
static GPtrArray *bound_prefs = NULL;

/**************************************************************************
 * Registration
 **************************************************************************/

/* Shared keys the pages bind that nothing else in pidgin4 registers yet
 * (the conversation window, M4b, and sounds, M6, register theirs too:
 * registering a key twice is harmless). Types and defaults are Pidgin
 * 2's (pidgin/gtkconv.c, gtksound.c, gtkprefs.c, gtkthemes.c). */
static void
prefs_register_shared(void)
{
	static const char *const sound_events[][2] = {
		/* option, default enabled (Pidgin 2's) */
		{ "login", "1" }, { "logout", "1" }, { "im_recv", "1" },
		{ "first_im_recv", "0" }, { "send_im", "1" }, { "join_chat", "0" },
		{ "left_chat", "0" }, { "send_chat_msg", "0" },
		{ "chat_msg_recv", "0" }, { "nick_said", "0" },
		{ "pounce_default", "1" }, { "sent_attention", "1" },
		{ "got_attention", "1" },
	};
	gsize i;

	purple_prefs_add_none(PIDGIN_PREFS_ROOT);

	purple_prefs_add_none(CONV_PREFS);
	purple_prefs_add_bool(CONV_PREFS "/use_smooth_scrolling", TRUE);
	purple_prefs_add_bool(CONV_PREFS "/close_on_tabs", TRUE);
	purple_prefs_add_bool(CONV_PREFS "/send_bold", FALSE);
	purple_prefs_add_bool(CONV_PREFS "/send_italic", FALSE);
	purple_prefs_add_bool(CONV_PREFS "/send_underline", FALSE);
	purple_prefs_add_bool(CONV_PREFS "/spellcheck", TRUE);
	purple_prefs_add_bool(CONV_PREFS "/show_incoming_formatting", TRUE);
	purple_prefs_add_bool(CONV_PREFS "/resize_custom_smileys", TRUE);
	purple_prefs_add_int(CONV_PREFS "/custom_smileys_size", 96);
	purple_prefs_add_int(CONV_PREFS "/minimum_entry_lines", 2);
	purple_prefs_add_bool(CONV_PREFS "/show_timestamps", TRUE);
	purple_prefs_add_bool(CONV_PREFS "/show_formatting_toolbar", TRUE);
	purple_prefs_add_string(CONV_PREFS "/bgcolor", "");
	purple_prefs_add_string(CONV_PREFS "/fgcolor", "");
	purple_prefs_add_string(CONV_PREFS "/font_face", "");
	purple_prefs_add_int(CONV_PREFS "/font_size", 3);
	purple_prefs_add_bool(CONV_PREFS "/tabs", TRUE);
	purple_prefs_add_int(CONV_PREFS "/tab_side", GTK_POS_TOP);
	purple_prefs_add_int(CONV_PREFS "/scrollback_lines", 4000);
	purple_prefs_add_bool(CONV_PREFS "/use_theme_font", TRUE);
	purple_prefs_add_string(CONV_PREFS "/custom_font", "");
	purple_prefs_add_none(CONV_PREFS "/im");
	purple_prefs_add_bool(CONV_PREFS "/im/animate_buddy_icons", TRUE);
	purple_prefs_add_bool(CONV_PREFS "/im/show_buddy_icons", TRUE);
	purple_prefs_add_string(CONV_PREFS "/im/hide_new", "never");
	purple_prefs_add_bool(CONV_PREFS "/im/close_immediately", TRUE);

	purple_prefs_add_none(SOUND_PREFS);
	purple_prefs_add_none(SOUND_PREFS "/enabled");
	purple_prefs_add_none(SOUND_PREFS "/file");
	for (i = 0; i < G_N_ELEMENTS(sound_events); i++) {
		char *pref = g_strdup_printf(SOUND_PREFS "/enabled/%s", sound_events[i][0]);

		purple_prefs_add_bool(pref, sound_events[i][1][0] == '1');
		g_free(pref);
		pref = g_strdup_printf(SOUND_PREFS "/file/%s", sound_events[i][0]);
		purple_prefs_add_path(pref, "");
		g_free(pref);
	}
	purple_prefs_add_bool(SOUND_PREFS "/conv_focus", TRUE);
	purple_prefs_add_bool(SOUND_PREFS "/mute", FALSE);
	purple_prefs_add_int(SOUND_PREFS "/volume", 50);

	purple_prefs_add_none(PIDGIN_PREFS_ROOT "/smileys");
	purple_prefs_add_string(PIDGIN_PREFS_ROOT "/smileys/theme", "Default");
}

/* pidgin4's own keys for the prefs pages. */
static void
prefs_register_pidgin4(void)
{
	/* Sounds: GSound or a command, not Pidgin 2's GStreamer methods. The
	 * per-event enabled/file keys, conv_focus, mute, volume and
	 * /purple/sound/while_status stay shared. */
	purple_prefs_add_none(SOUND4_PREFS);
	purple_prefs_add_string(SOUND4_PREFS "/method", "automatic");
	purple_prefs_add_string(SOUND4_PREFS "/command", "");

	/* URLs open with GtkUriLauncher (the desktop's default, through the
	 * portal) unless a custom command is set. */
	purple_prefs_add_none(BROWSER4_PREFS);
	purple_prefs_add_string(BROWSER4_PREFS "/method", "system");
	purple_prefs_add_string(BROWSER4_PREFS "/command", "");

	/* Conversation window options with no Pidgin 2 equivalent (the send
	 * button was a plugin there) or a different meaning (placement). */
	purple_prefs_add_none(CONV4_PREFS);
	purple_prefs_add_string(CONV4_PREFS "/placement", "last");
	purple_prefs_add_bool(CONV4_PREFS "/show_send_button", FALSE);

	/* Pasted images: "auto" is JPEG for an opaque image whose JPEG is
	 * smaller than its PNG, else PNG (pidginimageencode.c). */
	purple_prefs_add_none(PIDGIN4_PREFS_ROOT "/images");
	purple_prefs_add_string(PIDGIN4_PREFS_ROOT "/images/paste_format", "auto");
	purple_prefs_add_int(PIDGIN4_PREFS_ROOT "/images/paste_jpeg_quality",
	                     PIDGIN_IMAGE_ENCODE_DEFAULT_QUALITY);
}

/*
 * Profile contract: pidgin4 never adds, renames or removes keys under
 * /pidgin that have a pidgin4-specific meaning (the GTK 2 UI owns them;
 * they are still loaded from and saved to prefs.xml untouched). Shared
 * keys are registered with Pidgin 2's type and default; everything
 * pidgin4-specific lives under /pidgin4. Pidgin 2 keeps unknown prefs
 * through a load/save cycle, so that subtree survives it.
 */
void
pidgin_prefs_init(void)
{
	purple_prefs_add_none(PIDGIN4_PREFS_ROOT);

	/* M5: the preferences window's keys. */
	prefs_register_shared();
	prefs_register_pidgin4();

	/* Plugins: pidgin4's own list. /pidgin/plugins/loaded holds GTK 2
	 * plugin paths that must never be loaded into this process. */
	purple_prefs_add_none(PIDGIN4_PREFS_ROOT "/plugins");
	purple_prefs_add_path_list(PIDGIN4_PREFS_ROOT "/plugins/loaded", NULL);

	/* The debug window (gtkdebug.c adds the rest). */
	purple_prefs_add_none(PIDGIN4_PREFS_ROOT "/debug");

	/* Account manager window. */
	purple_prefs_add_none(PIDGIN4_PREFS_ROOT "/accounts");
	purple_prefs_add_none(PIDGIN4_PREFS_ROOT "/accounts/dialog");
	purple_prefs_add_int(PIDGIN4_PREFS_ROOT "/accounts/dialog/width", 560);
	purple_prefs_add_int(PIDGIN4_PREFS_ROOT "/accounts/dialog/height", 380);

	/* Per-profile marker: which pidgin4 last used this profile. Written on
	 * every start; handy when reading a profile that both UIs share. */
	purple_prefs_add_string(PIDGIN4_PREFS_ROOT "/last_version", "");
	purple_prefs_set_string(PIDGIN4_PREFS_ROOT "/last_version", VERSION);

	/* M4b: /pidgin/conversations (Pidgin 2's) and /pidgin4/conversations. */
	pidgin_conversations_prefs_init();
}

/**************************************************************************
 * Page helpers
 **************************************************************************/

static void
note_pref(const char *pref)
{
	if (bound_prefs == NULL)
		bound_prefs = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(bound_prefs, g_strdup(pref));
}

static GtkWidget *
page_new(void)
{
	GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_CAT_SPACE);

	gtk_widget_set_margin_top(vbox, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_bottom(vbox, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_start(vbox, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_end(vbox, PIDGIN_HIG_BORDER);
	return vbox;
}

static void
add_page(const char *name, const char *title, GtkWidget *page)
{
	GtkWidget *sw = pidgin_make_scrollable(page, GTK_POLICY_NEVER,
	                                       GTK_POLICY_AUTOMATIC, -1, -1);

	gtk_stack_add_titled(GTK_STACK(prefs_stack), sw, name, title);
}

static GtkWidget *
checkbox(GtkWidget *vbox, const char *label, const char *pref)
{
	GtkWidget *check = pidgin_pref_checkbox_new(label, pref);

	note_pref(pref);
	gtk_box_append(GTK_BOX(vbox), check);
	return check;
}

static GtkWidget *
spin(GtkWidget *vbox, const char *label, const char *pref, int min, int max,
     GtkSizeGroup *sg)
{
	GtkWidget *button = pidgin_pref_spin_new(pref, min, max);

	note_pref(pref);
	return pidgin_add_widget_to_vbox(GTK_BOX(vbox), label, sg, button, FALSE, NULL);
}

static GtkWidget *
entry(GtkWidget *vbox, const char *label, const char *pref, GtkSizeGroup *sg,
      gboolean masked)
{
	GtkWidget *e = masked ? gtk_password_entry_new() : gtk_entry_new();

	if (masked)
		gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(e), TRUE);
	pidgin_pref_bind_string(e, pref);
	note_pref(pref);
	return pidgin_add_widget_to_vbox(GTK_BOX(vbox), label, sg, e, TRUE, NULL);
}

/* A drop-down of NULL-terminated (label, value) string pairs. */
static GtkWidget *
dropdown_string(GtkWidget *vbox, const char *label, const char *pref,
                GtkSizeGroup *sg, ...)
{
	GPtrArray *labels = g_ptr_array_new(), *values = g_ptr_array_new();
	GtkWidget *dd;
	const char *l;
	va_list args;

	va_start(args, sg);
	while ((l = va_arg(args, const char *)) != NULL) {
		g_ptr_array_add(labels, (gpointer)l);
		g_ptr_array_add(values, va_arg(args, char *));
	}
	va_end(args);

	dd = pidgin_pref_dropdown_string_new(pref, (const char *const *)labels->pdata,
	                                     (const char *const *)values->pdata,
	                                     labels->len);
	g_ptr_array_free(labels, TRUE);
	g_ptr_array_free(values, TRUE);
	note_pref(pref);
	pidgin_add_widget_to_vbox(GTK_BOX(vbox), label, sg, dd, FALSE, NULL);
	return dd;
}

/* A drop-down of (label, int value) pairs, terminated by a NULL label. */
static GtkWidget *
dropdown_int(GtkWidget *vbox, const char *label, const char *pref,
             GtkSizeGroup *sg, ...)
{
	GPtrArray *labels = g_ptr_array_new();
	GArray *values = g_array_new(FALSE, FALSE, sizeof(int));
	GtkWidget *dd;
	const char *l;
	va_list args;

	va_start(args, sg);
	while ((l = va_arg(args, const char *)) != NULL) {
		int v = va_arg(args, int);

		g_ptr_array_add(labels, (gpointer)l);
		g_array_append_val(values, v);
	}
	va_end(args);

	dd = pidgin_pref_dropdown_int_new(pref, (const char *const *)labels->pdata,
	                                  (const int *)values->data, labels->len);
	g_ptr_array_free(labels, TRUE);
	g_array_free(values, TRUE);
	note_pref(pref);
	pidgin_add_widget_to_vbox(GTK_BOX(vbox), label, sg, dd, FALSE, NULL);
	return dd;
}

static GtkWidget *
note_label(GtkWidget *vbox, const char *text)
{
	GtkWidget *label = gtk_label_new(text);

	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_widget_add_css_class(label, "dim-label");
	gtk_box_append(GTK_BOX(vbox), label);
	return label;
}

/**************************************************************************
 * Interface
 **************************************************************************/

static GtkWidget *
interface_page(void)
{
	GtkWidget *ret = page_new();
	GtkWidget *vbox, *tabs_box;
	GtkSizeGroup *sg = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);

	vbox = pidgin_make_frame(ret, _("Buddy List"));
	checkbox(vbox, _("Closing the buddy list keeps Pidgin _running"),
	         PIDGIN4_PREFS_ROOT "/blist/close_hides");
	note_label(vbox, _("Quit with Buddies \342\206\222 Quit or Ctrl+Q. Until "
		"the system tray icon exists, a hidden buddy list comes back when "
		"Pidgin is started again."));

	vbox = pidgin_make_frame(ret, _("Conversation Window"));
	dropdown_string(vbox, _("_Hide new IM conversations:"),
		CONV_PREFS "/im/hide_new", sg,
		_("Never"), "never",
		_("When away"), "away",
		_("Always"), "always",
		NULL);

	vbox = pidgin_make_frame(ret, _("Tabs"));
	checkbox(vbox, _("Show IMs and chats in _tabbed windows"), CONV_PREFS "/tabs");
	tabs_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BOX_SPACE);
	gtk_widget_set_margin_start(tabs_box, PIDGIN_HIG_BORDER);
	gtk_box_append(GTK_BOX(vbox), tabs_box);
	pidgin_pref_bind_sensitive(tabs_box, CONV_PREFS "/tabs", FALSE);
	checkbox(tabs_box, _("Show close b_utton on tabs"), CONV_PREFS "/close_on_tabs");
	/* Pidgin 2's vertical tab variants (GTK_POS_* | 8) don't exist in
	 * GTK 4; a profile using one shows no selection and is left alone. */
	dropdown_int(tabs_box, _("_Placement:"), CONV_PREFS "/tab_side", sg,
		_("Top"), GTK_POS_TOP,
		_("Bottom"), GTK_POS_BOTTOM,
		_("Left"), GTK_POS_LEFT,
		_("Right"), GTK_POS_RIGHT,
		NULL);
	dropdown_string(tabs_box, _("N_ew conversations:"),
		CONV4_PREFS "/placement", sg,
		_("Last created window"), "last",
		_("New window"), "new",
		_("By group"), "group",
		_("By account"), "account",
		NULL);

	g_object_unref(sg);
	return ret;
}

/**************************************************************************
 * Conversations
 **************************************************************************/

/* The conversation font preview follows the pref through gtkthemes.c's
 * conversation font CSS (class .pidgin-conversation-font). */
static void
font_button_changed_cb(GtkFontDialogButton *button, GParamSpec *pspec,
                       gpointer data)
{
	PangoFontDescription *desc = gtk_font_dialog_button_get_font_desc(button);
	const char *pref = data;
	char *str;

	if (desc == NULL)
		return;

	if (gtk_font_dialog_button_get_level(button) == GTK_FONT_LEVEL_FAMILY)
		str = g_strdup(pango_font_description_get_family(desc));
	else
		str = pango_font_description_to_string(desc);
	if (!purple_strequal(purple_prefs_get_string(pref), str))
		purple_prefs_set_string(pref, str ? str : "");
	g_free(str);
}

/* The pref changed elsewhere (or was reset): show it. An empty value
 * leaves the button as it is (GtkFontDialogButton needs some font). */
static void
font_pref_changed_cb(const char *name, PurplePrefType type, gconstpointer value,
                     gpointer data)
{
	GtkFontDialogButton *button = data;
	PangoFontDescription *cur = gtk_font_dialog_button_get_font_desc(button);
	PangoFontDescription *desc;

	if (value == NULL || *(const char *)value == '\0')
		return;
	desc = pango_font_description_from_string(value);
	if (cur == NULL || !pango_font_description_equal(cur, desc))
		gtk_font_dialog_button_set_font_desc(button, desc);
	pango_font_description_free(desc);
}

static GtkWidget *
font_button_new(const char *pref, GtkFontLevel level)
{
	GtkFontDialog *dialog = gtk_font_dialog_new();
	GtkWidget *button = gtk_font_dialog_button_new(dialog);
	const char *value = purple_prefs_get_string(pref);

	gtk_font_dialog_button_set_level(GTK_FONT_DIALOG_BUTTON(button), level);
	gtk_font_dialog_button_set_use_font(GTK_FONT_DIALOG_BUTTON(button), TRUE);
	if (value != NULL && *value != '\0') {
		PangoFontDescription *desc = pango_font_description_from_string(value);

		gtk_font_dialog_button_set_font_desc(GTK_FONT_DIALOG_BUTTON(button), desc);
		pango_font_description_free(desc);
	}
	note_pref(pref);
	/* Connected after the initial value: opening the page writes nothing. */
	g_signal_connect(button, "notify::font-desc",
	                 G_CALLBACK(font_button_changed_cb), (gpointer)pref);
	purple_prefs_connect_callback(button, pref, font_pref_changed_cb, button);
	g_signal_connect_swapped(button, "destroy",
	                         G_CALLBACK(purple_prefs_disconnect_by_handle), button);
	return button;
}

static void
font_default_cb(GtkWidget *button, gpointer pref)
{
	purple_prefs_set_string(pref, "");
}

static void
color_button_changed_cb(GtkColorDialogButton *button, GParamSpec *pspec,
                        gpointer data)
{
	const GdkRGBA *rgba = gtk_color_dialog_button_get_rgba(button);
	const char *pref = data;
	char *str;

	str = g_strdup_printf("#%02x%02x%02x",
	                      (int)(rgba->red * 255 + 0.5),
	                      (int)(rgba->green * 255 + 0.5),
	                      (int)(rgba->blue * 255 + 0.5));
	if (!purple_strequal(purple_prefs_get_string(pref), str))
		purple_prefs_set_string(pref, str);
	g_free(str);
}

static void
color_pref_changed_cb(const char *name, PurplePrefType type,
                      gconstpointer value, gpointer data)
{
	GtkWidget *button = data;
	GdkRGBA rgba;

	gtk_widget_set_sensitive(button, value != NULL && *(const char *)value);
	if (value != NULL && *(const char *)value &&
	    pidgin_markup_parse_color(value, &rgba)) {
		const GdkRGBA *cur = gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(button));

		if (!gdk_rgba_equal(cur, &rgba))
			gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(button), &rgba);
	}
}

static void
color_check_toggled_cb(GtkCheckButton *check, gpointer data)
{
	GtkWidget *button = data;
	const char *pref = g_object_get_data(G_OBJECT(button), "pref");

	if (gtk_check_button_get_active(check)) {
		if (*purple_prefs_get_string(pref) == '\0')
			color_button_changed_cb(GTK_COLOR_DIALOG_BUTTON(button), NULL,
			                        (gpointer)pref);
	} else {
		purple_prefs_set_string(pref, "");
	}
}

/* "[x] Text color [button]": the pref is "" (no colour) or "#rrggbb". */
static void
color_row(GtkWidget *vbox, const char *label, const char *pref)
{
	GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	GtkWidget *check = gtk_check_button_new_with_mnemonic(label);
	GtkWidget *button = gtk_color_dialog_button_new(gtk_color_dialog_new());
	const char *value = purple_prefs_get_string(pref);
	GdkRGBA rgba = { 0, 0, 0, 1 };

	note_pref(pref);
	g_object_set_data(G_OBJECT(button), "pref", (gpointer)pref);
	if (value != NULL && *value != '\0' && pidgin_markup_parse_color(value, &rgba))
		gtk_check_button_set_active(GTK_CHECK_BUTTON(check), TRUE);
	gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(button), &rgba);
	gtk_widget_set_sensitive(button, value != NULL && *value != '\0');

	g_signal_connect(check, "toggled", G_CALLBACK(color_check_toggled_cb), button);
	g_signal_connect(button, "notify::rgba", G_CALLBACK(color_button_changed_cb),
	                 (gpointer)pref);
	purple_prefs_connect_callback(button, pref, color_pref_changed_cb, button);
	g_signal_connect_swapped(button, "destroy",
	                         G_CALLBACK(purple_prefs_disconnect_by_handle), button);

	gtk_box_append(GTK_BOX(hbox), check);
	gtk_box_append(GTK_BOX(hbox), button);
	gtk_box_append(GTK_BOX(vbox), hbox);
}

/* The default formatting preview: the HTML a new message starts with. */
static void
update_format_preview(GtkWidget *preview)
{
	GString *html = g_string_new(NULL);
	const char *face = purple_prefs_get_string(CONV_PREFS "/font_face");
	const char *fg = purple_prefs_get_string(CONV_PREFS "/fgcolor");
	const char *bg = purple_prefs_get_string(CONV_PREFS "/bgcolor");
	int size = purple_prefs_get_int(CONV_PREFS "/font_size");
	char *text;

	g_string_append(html, "<font");
	if (face && *face) {
		char *esc = g_markup_escape_text(face, -1);
		g_string_append_printf(html, " face=\"%s\"", esc);
		g_free(esc);
	}
	if (size >= 1 && size <= 7 && size != 3)
		g_string_append_printf(html, " size=\"%d\"", size);
	if (fg && *fg)
		g_string_append_printf(html, " color=\"%s\"", fg);
	if (bg && *bg)
		g_string_append_printf(html, " back=\"%s\"", bg);
	g_string_append_c(html, '>');
	if (purple_prefs_get_bool(CONV_PREFS "/send_bold"))
		g_string_append(html, "<b>");
	if (purple_prefs_get_bool(CONV_PREFS "/send_italic"))
		g_string_append(html, "<i>");
	if (purple_prefs_get_bool(CONV_PREFS "/send_underline"))
		g_string_append(html, "<u>");
	text = g_markup_escape_text(_("This is how your outgoing message text will "
		"appear when you use protocols that support formatting."), -1);
	g_string_append(html, text);
	g_free(text);
	if (purple_prefs_get_bool(CONV_PREFS "/send_underline"))
		g_string_append(html, "</u>");
	if (purple_prefs_get_bool(CONV_PREFS "/send_italic"))
		g_string_append(html, "</i>");
	if (purple_prefs_get_bool(CONV_PREFS "/send_bold"))
		g_string_append(html, "</b>");
	g_string_append(html, "</font>");

	{
		PidginMarkupOptions options = { 0 };

		options.flags = PIDGIN_MARKUP_NO_SMILEYS | PIDGIN_MARKUP_NO_LINKIFY;
		pidgin_rich_label_set_html(PIDGIN_RICH_LABEL(preview), html->str, &options);
	}
	g_string_free(html, TRUE);
}

static void
format_pref_changed_cb(const char *name, PurplePrefType type,
                       gconstpointer value, gpointer data)
{
	update_format_preview(data);
}

static GtkWidget *
conv_page(void)
{
	static const char *const format_prefs[] = {
		CONV_PREFS "/send_bold", CONV_PREFS "/send_italic",
		CONV_PREFS "/send_underline", CONV_PREFS "/font_face",
		CONV_PREFS "/font_size", CONV_PREFS "/fgcolor", CONV_PREFS "/bgcolor",
	};
	GtkWidget *ret = page_new();
	GtkWidget *vbox, *hbox, *button, *check, *preview, *entry_box, *compose;
	GtkSizeGroup *sg = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
	gsize i;

	vbox = pidgin_make_frame(ret, _("Conversations"));
	checkbox(vbox, _("Show _formatting on incoming messages"),
	         CONV_PREFS "/show_incoming_formatting");
	checkbox(vbox, _("Close IMs immediately when the tab is closed"),
	         CONV_PREFS "/im/close_immediately");
	checkbox(vbox, _("Show _detailed information"),
	         CONV_PREFS "/im/show_buddy_icons");
	check = checkbox(vbox, _("Enable buddy ic_on animation"),
	                 CONV_PREFS "/im/animate_buddy_icons");
	pidgin_pref_bind_sensitive(check, CONV_PREFS "/im/show_buddy_icons", FALSE);
	checkbox(vbox, _("_Notify buddies that you are typing to them"),
	         "/purple/conversations/im/send_typing");
	checkbox(vbox, _("Highlight _misspelled words"), CONV_PREFS "/spellcheck");
	checkbox(vbox, _("Show the formatting _toolbar"),
	         CONV_PREFS "/show_formatting_toolbar");
	checkbox(vbox, _("Show _timestamps"), CONV_PREFS "/show_timestamps");
	checkbox(vbox, _("Show a _Send button"), CONV4_PREFS "/show_send_button");
	checkbox(vbox, _("Use smooth-scrolling"), CONV_PREFS "/use_smooth_scrolling");

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	gtk_box_append(GTK_BOX(vbox), hbox);
	checkbox(hbox, _("Resize incoming custom smileys"),
	         CONV_PREFS "/resize_custom_smileys");
	button = spin(hbox, _("Maximum size:"), CONV_PREFS "/custom_smileys_size",
	              16, 512, NULL);
	pidgin_pref_bind_sensitive(button, CONV_PREFS "/resize_custom_smileys", FALSE);

	spin(vbox, _("Minimum input area height in lines:"),
	     CONV_PREFS "/minimum_entry_lines", 1, 8, sg);
	/* Pasted images (and dropped image data) */
	dropdown_string(vbox, _("Send _pasted images as:"),
	                PIDGIN4_PREFS_ROOT "/images/paste_format", sg,
	                _("Automatic (JPEG for photos)"), "auto", _("PNG"), "png",
	                _("JPEG"), "jpeg", NULL);
	button = spin(vbox, _("_JPEG quality:"), PIDGIN4_PREFS_ROOT "/images/paste_jpeg_quality",
	              50, 100, sg);
	pidgin_pref_bind_insensitive_string(button, PIDGIN4_PREFS_ROOT "/images/paste_format",
	                                    "png");
	spin(vbox, _("_Scrollback (messages kept in a window):"),
	     CONV_PREFS "/scrollback_lines", 100, 100000, sg);

	/* Font */
	vbox = pidgin_make_frame(ret, _("Font"));
	checkbox(vbox, _("Use the _system font"), CONV_PREFS "/use_theme_font");
	button = font_button_new(CONV_PREFS "/custom_font", GTK_FONT_LEVEL_FONT);
	hbox = pidgin_add_widget_to_vbox(GTK_BOX(vbox), _("Conversation _font:"),
	                                 NULL, button, FALSE, NULL);
	pidgin_pref_bind_sensitive(hbox, CONV_PREFS "/use_theme_font", TRUE);

	/* A compose entry in the conversation font (class
	 * .pidgin-conversation-font, see gtkthemes.c). */
	entry_box = pidgin_create_compose_entry(PURPLE_CONNECTION_HTML, FALSE,
	                                        &compose, NULL);
	gtk_widget_add_css_class(compose, "pidgin-conversation-font");
	pidgin_compose_entry_set_markup(PIDGIN_COMPOSE_ENTRY(compose),
		_("This is how your outgoing message text will appear when you use "
		  "protocols that support formatting."));
	gtk_widget_set_size_request(entry_box, -1, 60);
	gtk_widget_set_vexpand(entry_box, FALSE);
	gtk_box_append(GTK_BOX(vbox), entry_box);

	/* Default formatting */
	vbox = pidgin_make_frame(ret, _("Default Formatting"));
	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BORDER);
	gtk_box_append(GTK_BOX(vbox), hbox);
	checkbox(hbox, _("_Bold"), CONV_PREFS "/send_bold");
	checkbox(hbox, _("_Italic"), CONV_PREFS "/send_italic");
	checkbox(hbox, _("_Underline"), CONV_PREFS "/send_underline");

	button = font_button_new(CONV_PREFS "/font_face", GTK_FONT_LEVEL_FAMILY);
	hbox = pidgin_add_widget_to_vbox(GTK_BOX(vbox), _("Font _face:"), sg,
	                                 button, FALSE, NULL);
	button = gtk_button_new_with_mnemonic(_("_Default"));
	gtk_widget_set_tooltip_text(button, _("Use the default font face"));
	g_signal_connect(button, "clicked", G_CALLBACK(font_default_cb),
	                 (gpointer)(CONV_PREFS "/font_face"));
	gtk_box_append(GTK_BOX(hbox), button);
	dropdown_int(vbox, _("Font si_ze:"), CONV_PREFS "/font_size", sg,
		_("Smallest"), 1, _("Smaller"), 2, _("Normal"), 3, _("Larger"), 4,
		_("Large"), 5, _("Very large"), 6, _("Largest"), 7, NULL);
	color_row(vbox, _("_Text color"), CONV_PREFS "/fgcolor");
	color_row(vbox, _("Bac_kground color"), CONV_PREFS "/bgcolor");

	preview = pidgin_rich_label_new();
	gtk_widget_set_margin_top(preview, PIDGIN_HIG_BOX_SPACE);
	gtk_box_append(GTK_BOX(vbox), preview);
	update_format_preview(preview);
	for (i = 0; i < G_N_ELEMENTS(format_prefs); i++)
		purple_prefs_connect_callback(preview, format_prefs[i],
		                              format_pref_changed_cb, preview);
	g_signal_connect_swapped(preview, "destroy",
	                         G_CALLBACK(purple_prefs_disconnect_by_handle), preview);

	g_object_unref(sg);
	return ret;
}

/**************************************************************************
 * Smiley themes
 **************************************************************************/

static void
smiley_theme_selected_cb(GtkSingleSelection *sel, GParamSpec *pspec, gpointer data)
{
	PidginItem *item = gtk_single_selection_get_selected_item(sel);
	GtkWidget *preview = data;
	GtkWidget *child;
	GSList *smileys, *l;
	int n = 0;

	if (item == NULL || g_object_get_data(G_OBJECT(sel), "updating"))
		return;
	if (!purple_strequal(purple_prefs_get_string(PIDGIN_PREFS_ROOT "/smileys/theme"),
	                     pidgin_item_get_id(item)))
		purple_prefs_set_string(PIDGIN_PREFS_ROOT "/smileys/theme",
		                        pidgin_item_get_id(item));

	/* A few smileys of the (now current) theme. */
	while ((child = gtk_widget_get_first_child(preview)) != NULL)
		gtk_box_remove(GTK_BOX(preview), child);
	smileys = pidgin_smiley_theme_get_smileys(NULL);
	for (l = smileys; l != NULL && n < 16; l = l->next) {
		PidginSmiley *smiley = l->data;
		GdkPaintable *paintable;
		GtkWidget *image;

		if (pidgin_smiley_is_hidden(smiley) ||
		    (paintable = pidgin_smiley_get_paintable(smiley)) == NULL)
			continue;
		image = gtk_image_new_from_paintable(paintable);
		gtk_image_set_pixel_size(GTK_IMAGE(image), 24);
		gtk_widget_set_tooltip_text(image, pidgin_smiley_get_shortcut(smiley));
		gtk_box_append(GTK_BOX(preview), image);
		n++;
	}
	if (n == 0)
		gtk_box_append(GTK_BOX(preview), gtk_label_new(_("(no smileys)")));
}

static void
smiley_row_setup_cb(GtkSignalListItemFactory *f, GtkListItem *li, gpointer data)
{
	GtkWidget *label = gtk_label_new(NULL);

	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_widget_set_margin_start(label, PIDGIN_HIG_BOX_SPACE);
	gtk_widget_set_margin_top(label, 3);
	gtk_widget_set_margin_bottom(label, 3);
	gtk_list_item_set_child(li, label);
}

static void
smiley_row_bind_cb(GtkSignalListItemFactory *f, GtkListItem *li, gpointer data)
{
	PidginItem *item = gtk_list_item_get_item(li);

	gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(li)),
	                   pidgin_item_get_label(item));
}

static void
select_theme_row(GtkSingleSelection *sel, const char *name)
{
	GListModel *model = gtk_single_selection_get_model(sel);
	guint i, n = g_list_model_get_n_items(model);

	for (i = 0; i < n; i++) {
		PidginItem *item = g_list_model_get_item(model, i);
		gboolean match = purple_strequal(pidgin_item_get_id(item), name);

		g_object_unref(item);
		if (match) {
			gtk_single_selection_set_selected(sel, i);
			return;
		}
	}
}

static void
smiley_theme_pref_cb(const char *name, PurplePrefType type, gconstpointer value,
                     gpointer data)
{
	select_theme_row(data, value);
}

static GtkWidget *
smiley_page(void)
{
	GtkWidget *ret = page_new();
	GtkWidget *vbox, *view, *preview;
	GListStore *store = g_list_store_new(PIDGIN_TYPE_ITEM);
	GtkSingleSelection *sel;
	GtkListItemFactory *factory;
	GList *names, *l;
	PidginItem *item;

	vbox = pidgin_make_frame(ret, _("Smiley Themes"));
	note_pref(PIDGIN_PREFS_ROOT "/smileys/theme");

	names = pidgin_smiley_themes_get_names();
	for (l = names; l != NULL; l = l->next) {
		if (purple_strequal(l->data, "none"))
			continue;
		item = pidgin_item_new(l->data, l->data, NULL);
		g_list_store_append(store, item);
		g_object_unref(item);
	}
	g_list_free(names);
	item = pidgin_item_new(_("None (no smileys)"), "none", NULL);
	g_list_store_append(store, item);
	g_object_unref(item);

	sel = gtk_single_selection_new(G_LIST_MODEL(store));
	gtk_single_selection_set_autoselect(sel, FALSE);
	gtk_single_selection_set_can_unselect(sel, TRUE);
	factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(smiley_row_setup_cb), NULL);
	g_signal_connect(factory, "bind", G_CALLBACK(smiley_row_bind_cb), NULL);
	view = gtk_list_view_new(GTK_SELECTION_MODEL(sel), factory);
	gtk_box_append(GTK_BOX(vbox), pidgin_make_scrollable(view, GTK_POLICY_NEVER,
	                                                     GTK_POLICY_AUTOMATIC, -1, 200));

	preview = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	gtk_widget_set_margin_top(preview, PIDGIN_HIG_BOX_SPACE);
	gtk_box_append(GTK_BOX(vbox), preview);

	/* Select the current theme before listening: no write on open. */
	g_object_set_data(G_OBJECT(sel), "updating", GINT_TO_POINTER(1));
	gtk_single_selection_set_selected(sel, GTK_INVALID_LIST_POSITION);
	select_theme_row(sel, purple_prefs_get_string(PIDGIN_PREFS_ROOT "/smileys/theme"));
	g_object_set_data(G_OBJECT(sel), "updating", NULL);
	g_signal_connect(sel, "notify::selected-item",
	                 G_CALLBACK(smiley_theme_selected_cb), preview);
	smiley_theme_selected_cb(sel, NULL, preview);

	purple_prefs_connect_callback(view, PIDGIN_PREFS_ROOT "/smileys/theme",
	                              smiley_theme_pref_cb, sel);
	g_signal_connect_swapped(view, "destroy",
	                         G_CALLBACK(purple_prefs_disconnect_by_handle), view);

	note_label(vbox, _("Custom smileys are managed in Tools \342\206\222 "
		"Custom Smileys."));
	return ret;
}

/**************************************************************************
 * Themes (CSS)
 **************************************************************************/

static const char user_css_template[] =
	"/* pidgin4 user style sheet.\n"
	" *\n"
	" * Loaded after pidgin4's own style at user priority, and reloaded as\n"
	" * soon as this file is saved. Some classes to style:\n"
	" *   .pidgin-blist-online, .pidgin-blist-away, .pidgin-blist-idle,\n"
	" *   .pidgin-blist-offline, .pidgin-blist-group, .pidgin-blist-contact,\n"
	" *   .pidgin-blist-chat         buddy list rows\n"
	" *   .pidgin-compose-entry      the message entry\n"
	" *   .pidgin-conversation-font  text in the conversation font\n"
	" *   pidgin-rich-label          formatted text (notify, user info)\n"
	" * Use GTK_DEBUG=interactive to find more.\n"
	" */\n\n"
	"/* .pidgin-blist-away { color: #808080; } */\n";

static void
css_launch_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	GError *error = NULL;
	gboolean ok;

	if (GTK_IS_FILE_LAUNCHER(source) && data != NULL)
		ok = gtk_file_launcher_open_containing_folder_finish(
			GTK_FILE_LAUNCHER(source), result, &error);
	else
		ok = gtk_file_launcher_launch_finish(GTK_FILE_LAUNCHER(source),
		                                     result, &error);
	if (!ok) {
		if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
			purple_debug_warning("prefs", "Could not open the style sheet: %s\n",
			                     error->message);
		g_error_free(error);
	}
}

static void
css_open(GtkWidget *button, gboolean folder)
{
	const char *path = pidgin_themes_get_user_css_path();
	GtkFileLauncher *launcher;
	GFile *file;

	if (!folder && !g_file_test(path, G_FILE_TEST_EXISTS)) {
		GError *error = NULL;

		if (!g_file_set_contents(path, user_css_template, -1, &error)) {
			purple_debug_error("prefs", "Could not create %s: %s\n",
			                   path, error->message);
			g_error_free(error);
			return;
		}
		g_chmod(path, S_IRUSR | S_IWUSR);
	}

	file = g_file_new_for_path(path);
	launcher = gtk_file_launcher_new(file);
	if (folder)
		gtk_file_launcher_open_containing_folder(launcher,
			GTK_WINDOW(gtk_widget_get_root(button)), NULL, css_launch_cb,
			GINT_TO_POINTER(1));
	else
		gtk_file_launcher_launch(launcher,
			GTK_WINDOW(gtk_widget_get_root(button)), NULL, css_launch_cb, NULL);
	g_object_unref(launcher);
	g_object_unref(file);
}

static void
css_open_cb(GtkWidget *button, gpointer data)
{
	css_open(button, FALSE);
}

static void
css_folder_cb(GtkWidget *button, gpointer data)
{
	css_open(button, TRUE);
}

static void
css_update_status(GtkWidget *label)
{
	const char *path = pidgin_themes_get_user_css_path();
	const char *error = pidgin_themes_get_user_css_error();
	char *text;

	if (!g_file_test(path, G_FILE_TEST_EXISTS))
		text = g_strdup(_("The file does not exist yet: Open creates it with "
		                  "some examples."));
	else if (error != NULL)
		text = g_strdup_printf(_("Errors in the style sheet:\n%s"), error);
	else
		text = g_strdup(_("The style sheet is loaded."));
	gtk_label_set_text(GTK_LABEL(label), text);
	g_free(text);
}

static void
css_reload_cb(GtkWidget *button, gpointer label)
{
	pidgin_themes_reload_user_css();
	css_update_status(label);
}

static GtkWidget *
theme_page(void)
{
	GtkWidget *ret = page_new();
	GtkWidget *vbox, *label, *hbox, *button, *status;
	char *markup, *esc;

	vbox = pidgin_make_frame(ret, _("Style Sheet"));
	note_label(vbox, _("Pidgin uses the GTK theme of your desktop. Colours "
		"and fonts of its own widgets (buddy list rows, the message entry, "
		"formatted text) can be changed with CSS in this file, which is "
		"loaded after the built-in style and reloaded whenever it changes:"));

	esc = g_markup_escape_text(pidgin_themes_get_user_css_path(), -1);
	markup = g_strdup_printf("<tt>%s</tt>", esc);
	label = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(label), markup);
	gtk_label_set_selectable(GTK_LABEL(label), TRUE);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_CHAR);
	gtk_box_append(GTK_BOX(vbox), label);
	g_free(markup);
	g_free(esc);

	status = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(status), 0.0);
	gtk_label_set_wrap(GTK_LABEL(status), TRUE);
	gtk_label_set_selectable(GTK_LABEL(status), TRUE);
	css_update_status(status);

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	button = gtk_button_new_with_mnemonic(_("_Open"));
	g_signal_connect(button, "clicked", G_CALLBACK(css_open_cb), NULL);
	gtk_box_append(GTK_BOX(hbox), button);
	button = gtk_button_new_with_mnemonic(_("Open _Folder"));
	g_signal_connect(button, "clicked", G_CALLBACK(css_folder_cb), NULL);
	gtk_box_append(GTK_BOX(hbox), button);
	button = gtk_button_new_with_mnemonic(_("_Reload"));
	g_signal_connect(button, "clicked", G_CALLBACK(css_reload_cb), status);
	gtk_box_append(GTK_BOX(hbox), button);
	gtk_box_append(GTK_BOX(vbox), hbox);
	gtk_box_append(GTK_BOX(vbox), status);

	note_label(vbox, _("Pidgin 2's buddy list, conversation and sound themes "
		"are not supported; the conversation font and default formatting "
		"are on the Conversations page."));
	return ret;
}

/**************************************************************************
 * Sounds
 **************************************************************************/

static const char *
sound_file_pref(const char *option, char *buf, gsize len)
{
	g_snprintf(buf, len, SOUND_PREFS "/file/%s", option);
	return buf;
}

static void
sound_file_label_update(GtkWidget *label, const char *file)
{
	char *base = (file && *file) ? g_path_get_basename(file) : NULL;

	gtk_label_set_text(GTK_LABEL(label), base ? base : _("(default)"));
	gtk_widget_set_tooltip_text(label, (file && *file) ? file : NULL);
	g_free(base);
}

static void
sound_file_pref_cb(const char *name, PurplePrefType type, gconstpointer value,
                   gpointer data)
{
	sound_file_label_update(data, value);
}

static void
sound_choose_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	char *pref = data;
	GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);

	if (file != NULL) {
		char *path = g_file_get_path(file);

		if (path != NULL)
			purple_prefs_set_path(pref, path);
		g_free(path);
		g_object_unref(file);
	}
	g_free(pref);
}

static void
sound_browse_cb(GtkWidget *button, gpointer data)
{
	const char *option = data;
	char buf[256];
	GtkFileDialog *dialog = gtk_file_dialog_new();
	GtkFileFilter *filter = gtk_file_filter_new();
	GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
	const char *cur = purple_prefs_get_path(sound_file_pref(option, buf, sizeof(buf)));

	gtk_file_dialog_set_title(dialog, _("Sound Selection"));
	gtk_file_filter_set_name(filter, _("Sounds"));
	gtk_file_filter_add_mime_type(filter, "audio/*");
	g_list_store_append(filters, filter);
	gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
	if (cur != NULL && *cur != '\0') {
		GFile *file = g_file_new_for_path(cur);
		gtk_file_dialog_set_initial_file(dialog, file);
		g_object_unref(file);
	}
	gtk_file_dialog_open(dialog, GTK_WINDOW(gtk_widget_get_root(button)), NULL,
	                     sound_choose_cb, g_strdup(buf));
	g_object_unref(filters);
	g_object_unref(filter);
	g_object_unref(dialog);
}

static void
sound_reset_cb(GtkWidget *button, gpointer data)
{
	char buf[256];

	purple_prefs_set_path(sound_file_pref(data, buf, sizeof(buf)), "");
}

/*
 * Plays an event's sound as Pidgin 2's test_sound() did: with the event,
 * the status condition and mute temporarily overridden. The sound itself
 * comes from the sound UI ops (M6, GSound); without them this is silent.
 */
static void
sound_preview_cb(GtkWidget *button, gpointer data)
{
	PurpleSoundEventID event = GPOINTER_TO_INT(data);
	char *enabled = g_strdup_printf(SOUND_PREFS "/enabled/%s",
	                                pidgin_sound_get_event_option(event));
	gboolean was_enabled = purple_prefs_get_bool(enabled);
	gboolean was_muted = purple_prefs_get_bool(SOUND_PREFS "/mute");
	int was_status = purple_prefs_get_int("/purple/sound/while_status");

	purple_prefs_set_bool(enabled, TRUE);
	purple_prefs_set_bool(SOUND_PREFS "/mute", FALSE);
	purple_prefs_set_int("/purple/sound/while_status", 3);
	purple_sound_play_event(event, NULL);
	purple_prefs_set_int("/purple/sound/while_status", was_status);
	purple_prefs_set_bool(SOUND_PREFS "/mute", was_muted);
	purple_prefs_set_bool(enabled, was_enabled);
	g_free(enabled);
}

static void
volume_changed_cb(GtkRange *range, gpointer data)
{
	int value = (int)(gtk_range_get_value(range) + 0.5);

	if (purple_prefs_get_int(SOUND_PREFS "/volume") != value)
		purple_prefs_set_int(SOUND_PREFS "/volume", value);
}

static void
volume_pref_cb(const char *name, PurplePrefType type, gconstpointer value,
               gpointer data)
{
	if ((int)(gtk_range_get_value(data) + 0.5) != GPOINTER_TO_INT(value))
		gtk_range_set_value(data, GPOINTER_TO_INT(value));
}

static GtkWidget *
sound_page(void)
{
	GtkWidget *ret = page_new();
	GtkWidget *vbox, *grid, *hbox, *label, *w, *scale;
	GtkSizeGroup *sg = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
	int row = 0;
	PurpleSoundEventID ev;

	vbox = pidgin_make_frame(ret, _("Sound Options"));
	dropdown_string(vbox, _("_Method:"), SOUND4_PREFS "/method", sg,
		_("Automatic"), "automatic",
		_("Command"), "custom",
		_("No sounds"), "none",
		NULL);
	hbox = entry(vbox, _("Sound c_ommand:\n(%s for filename)"),
	             SOUND4_PREFS "/command", sg, FALSE);
	pidgin_pref_bind_sensitive_string(hbox, SOUND4_PREFS "/method", "custom");

	checkbox(vbox, _("M_ute sounds"), SOUND_PREFS "/mute");
	checkbox(vbox, _("Sounds when conversation has _focus"),
	         SOUND_PREFS "/conv_focus");
	dropdown_int(vbox, _("_Enable sounds:"), "/purple/sound/while_status", sg,
		_("Only when available"), 1,
		_("Only when not available"), 2,
		_("Always"), 3,
		NULL);

	scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 5);
	gtk_scale_set_draw_value(GTK_SCALE(scale), TRUE);
	gtk_range_set_value(GTK_RANGE(scale), purple_prefs_get_int(SOUND_PREFS "/volume"));
	g_signal_connect(scale, "value-changed", G_CALLBACK(volume_changed_cb), NULL);
	purple_prefs_connect_callback(scale, SOUND_PREFS "/volume", volume_pref_cb, scale);
	g_signal_connect_swapped(scale, "destroy",
	                         G_CALLBACK(purple_prefs_disconnect_by_handle), scale);
	note_pref(SOUND_PREFS "/volume");
	hbox = pidgin_add_widget_to_vbox(GTK_BOX(vbox), _("V_olume:"), sg, scale,
	                                 TRUE, NULL);
	pidgin_pref_bind_sensitive_string(hbox, SOUND4_PREFS "/method", "automatic");
	note_label(vbox, _("Sounds are played with GSound (the desktop's event "
		"sounds) or with the command; the sound backend comes with desktop "
		"integration, until then nothing is played."));

	vbox = pidgin_make_frame(ret, _("Sound Events"));
	grid = gtk_grid_new();
	gtk_grid_set_column_spacing(GTK_GRID(grid), PIDGIN_HIG_BOX_SPACE);
	gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
	gtk_box_append(GTK_BOX(vbox), grid);

	for (ev = 0; ev < PURPLE_NUM_SOUNDS; ev++) {
		const char *option = pidgin_sound_get_event_option(ev);
		const char *text = pidgin_sound_get_event_label(ev);
		char enabled[256], file[256];

		if (option == NULL || text == NULL)
			continue;
		g_snprintf(enabled, sizeof(enabled), SOUND_PREFS "/enabled/%s", option);
		sound_file_pref(option, file, sizeof(file));
		note_pref(enabled);
		note_pref(file);

		w = pidgin_pref_checkbox_new(text, enabled);
		gtk_grid_attach(GTK_GRID(grid), w, 0, row, 1, 1);

		label = gtk_label_new(NULL);
		gtk_label_set_xalign(GTK_LABEL(label), 0.0);
		gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
		gtk_label_set_width_chars(GTK_LABEL(label), 16);
		gtk_widget_set_hexpand(label, TRUE);
		sound_file_label_update(label, purple_prefs_get_path(file));
		purple_prefs_connect_callback(label, file, sound_file_pref_cb, label);
		g_signal_connect_swapped(label, "destroy",
		                         G_CALLBACK(purple_prefs_disconnect_by_handle), label);
		gtk_grid_attach(GTK_GRID(grid), label, 1, row, 1, 1);

		w = gtk_button_new_with_mnemonic(_("_Browse..."));
		g_signal_connect(w, "clicked", G_CALLBACK(sound_browse_cb), (gpointer)option);
		gtk_grid_attach(GTK_GRID(grid), w, 2, row, 1, 1);
		w = gtk_button_new_with_mnemonic(_("Pre_view"));
		g_signal_connect(w, "clicked", G_CALLBACK(sound_preview_cb), GINT_TO_POINTER(ev));
		gtk_grid_attach(GTK_GRID(grid), w, 3, row, 1, 1);
		w = gtk_button_new_with_mnemonic(_("_Reset"));
		g_signal_connect(w, "clicked", G_CALLBACK(sound_reset_cb), (gpointer)option);
		gtk_grid_attach(GTK_GRID(grid), w, 4, row, 1, 1);
		row++;
	}
	pidgin_pref_bind_sensitive(grid, SOUND_PREFS "/mute", TRUE);

	g_object_unref(sg);
	return ret;
}

/**************************************************************************
 * Network (with the global proxy)
 **************************************************************************/

static void
auto_ip_label_update(GtkWidget *check)
{
	const char *ip = purple_network_get_my_ip(-1);
	char *text = g_strdup_printf(_("Use _automatically detected IP address: %s"),
	                             (ip && *ip) ? ip : _("Disabled"));

	gtk_check_button_set_label(GTK_CHECK_BUTTON(check), text);
	g_free(text);
}

/* Host, port and credentials apply to an explicit proxy only. */
static void
proxy_box_update(GtkWidget *box)
{
	const char *type = purple_prefs_get_string("/purple/proxy/type");

	gtk_widget_set_sensitive(box, !purple_strequal(type, "none") &&
	                              !purple_strequal(type, "envvar"));
}

static void
proxy_type_cb(const char *name, PurplePrefType type, gconstpointer value,
              gpointer data)
{
	proxy_box_update(data);
}

static GtkWidget *
network_page(void)
{
	GtkWidget *ret = page_new();
	GtkWidget *vbox, *check, *hbox, *box;
	GtkSizeGroup *sg = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);

	vbox = pidgin_make_frame(ret, _("IP Address"));
	entry(vbox, _("ST_UN server:"), "/purple/network/stun_server", sg, FALSE);
	check = checkbox(vbox, "", "/purple/network/auto_ip");
	gtk_check_button_set_use_underline(GTK_CHECK_BUTTON(check), TRUE);
	auto_ip_label_update(check);
	hbox = entry(vbox, _("Public _IP:"), "/purple/network/public_ip", sg, FALSE);
	pidgin_pref_bind_sensitive(hbox, "/purple/network/auto_ip", TRUE);

	vbox = pidgin_make_frame(ret, _("Ports"));
	checkbox(vbox, _("_Enable automatic router port forwarding"),
	         "/purple/network/map_ports");
	checkbox(vbox, _("_Manually specify range of ports to listen on:"),
	         "/purple/network/ports_range_use");
	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BOX_SPACE);
	gtk_widget_set_margin_start(box, PIDGIN_HIG_BORDER);
	gtk_box_append(GTK_BOX(vbox), box);
	spin(box, _("_Start:"), "/purple/network/ports_range_start", 0, 65535, sg);
	spin(box, _("_End:"), "/purple/network/ports_range_end", 0, 65535, sg);
	pidgin_pref_bind_sensitive(box, "/purple/network/ports_range_use", FALSE);

	vbox = pidgin_make_frame(ret, _("Relay Server (TURN)"));
	entry(vbox, _("_TURN server:"), "/purple/network/turn_server", sg, FALSE);
	spin(vbox, _("_UDP Port:"), "/purple/network/turn_port", 0, 65535, sg);
	spin(vbox, _("T_CP Port:"), "/purple/network/turn_port_tcp", 0, 65535, sg);
	entry(vbox, _("Use_rname:"), "/purple/network/turn_username", sg, FALSE);
	entry(vbox, _("Pass_word:"), "/purple/network/turn_password", sg, TRUE);

	vbox = pidgin_make_frame(ret, _("Proxy Server"));
	/* "envvar" asks GIO's GProxyResolver: GNOME's proxy settings, the
	 * environment, PAC/WPAD, or the proxy portal (libpurple, M1). */
	dropdown_string(vbox, _("Proxy t_ype:"), "/purple/proxy/type", sg,
		_("No proxy"), "none",
		_("Use System Proxy Settings"), "envvar",
		_("HTTP"), "http",
		_("SOCKS 4"), "socks4",
		_("SOCKS 5"), "socks5",
		_("Tor/Privacy (SOCKS5)"), "tor",
		NULL);
	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BOX_SPACE);
	gtk_box_append(GTK_BOX(vbox), box);
	entry(box, _("_Host:"), "/purple/proxy/host", sg, FALSE);
	spin(box, _("P_ort:"), "/purple/proxy/port", 0, 65535, sg);
	entry(box, _("User_name:"), "/purple/proxy/username", sg, FALSE);
	entry(box, _("Pa_ssword:"), "/purple/proxy/password", sg, TRUE);
	check = checkbox(box, _("Use remote _DNS with SOCKS4 proxies"),
	                 "/purple/proxy/socks4_remotedns");
	pidgin_pref_bind_sensitive_string(check, "/purple/proxy/type", "socks4");
	proxy_box_update(box);
	purple_prefs_connect_callback(box, "/purple/proxy/type", proxy_type_cb, box);
	g_signal_connect_swapped(box, "destroy",
	                         G_CALLBACK(purple_prefs_disconnect_by_handle), box);

	g_object_unref(sg);
	return ret;
}

/**************************************************************************
 * Browser
 **************************************************************************/

static GtkWidget *
browser_page(void)
{
	GtkWidget *ret = page_new();
	GtkWidget *vbox, *hbox;
	GtkSizeGroup *sg = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);

	vbox = pidgin_make_frame(ret, _("Browser Selection"));
	dropdown_string(vbox, _("_Browser:"), BROWSER4_PREFS "/method", sg,
		_("Desktop Default"), "system",
		_("Manual"), "custom",
		NULL);
	hbox = entry(vbox, _("_Manual:\n(%s for URL)"), BROWSER4_PREFS "/command",
	             sg, FALSE);
	pidgin_pref_bind_sensitive_string(hbox, BROWSER4_PREFS "/method", "custom");
	note_label(vbox, _("The desktop default opens links through the "
		"desktop (xdg-desktop-portal where available). A manual command "
		"is used for web links only; other links (mailto:, xmpp:, ...) "
		"always go to the desktop."));

	g_object_unref(sg);
	return ret;
}

/**************************************************************************
 * Logging
 **************************************************************************/

static GtkWidget *
logging_page(void)
{
	GtkWidget *ret = page_new();
	GtkWidget *vbox, *dd;
	GListStore *store = g_list_store_new(PIDGIN_TYPE_ITEM);
	GList *options, *l;

	vbox = pidgin_make_frame(ret, _("Logging"));

	/* purple_log_logger_get_options(): label, id, label, id, ... */
	options = purple_log_logger_get_options();
	for (l = options; l != NULL && l->next != NULL; l = l->next->next) {
		PidginItem *item = pidgin_item_new(l->data, l->next->data, NULL);

		g_list_store_append(store, item);
		g_object_unref(item);
	}
	g_list_free(options);
	dd = pidgin_item_dropdown_new(G_LIST_MODEL(store));
	pidgin_pref_bind_dropdown_string(dd, "/purple/logging/format");
	note_pref("/purple/logging/format");
	pidgin_add_widget_to_vbox(GTK_BOX(vbox), _("Log _format:"), NULL, dd, FALSE, NULL);

	checkbox(vbox, _("Log all _instant messages"), "/purple/logging/log_ims");
	checkbox(vbox, _("Log all c_hats"), "/purple/logging/log_chats");
	checkbox(vbox, _("Log all _status changes to system log"),
	         "/purple/logging/log_system");
	return ret;
}

/**************************************************************************
 * Status / Idle
 **************************************************************************/

/* A drop-down of the saved (non-transient) statuses bound to an int pref
 * that holds a creation time, as Pidgin 2's status menus. */
static GtkWidget *
savedstatus_dropdown(const char *pref)
{
	GListStore *store = g_list_store_new(PIDGIN_TYPE_ITEM);
	PurpleSavedStatus *current = purple_savedstatus_find_by_creation_time(
		purple_prefs_get_int(pref));
	GList *l;
	GtkWidget *dd;

	for (l = purple_savedstatuses_get_all(); l != NULL; l = l->next) {
		PurpleSavedStatus *status = l->data;
		char *id;
		PidginItem *item;

		if (purple_savedstatus_is_transient(status) && status != current)
			continue;
		id = g_strdup_printf("%d", (int)purple_savedstatus_get_creation_time(status));
		item = pidgin_item_new(purple_savedstatus_get_title(status), id, NULL);
		g_list_store_append(store, item);
		g_object_unref(item);
		g_free(id);
	}
	dd = pidgin_item_dropdown_new(G_LIST_MODEL(store));
	pidgin_pref_bind_dropdown_int(dd, pref);
	note_pref(pref);
	return dd;
}

static GtkWidget *
away_page(void)
{
	GtkWidget *ret = page_new();
	GtkWidget *vbox, *hbox, *dd;
	GtkSizeGroup *sg = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);

	vbox = pidgin_make_frame(ret, _("Idle"));
	/* "system" is the desktop's idle time: Wayland ext-idle-notify-v1 or
	 * GNOME's IdleMonitor (M6); until then libpurple falls back to the
	 * last-activity timer. */
	dropdown_string(vbox, _("_Report idle time:"), "/purple/away/idle_reporting", sg,
		_("Never"), "none",
		_("From last sent message"), "purple",
		_("Based on keyboard or mouse use"), "system",
		NULL);
	hbox = spin(vbox, _("_Minutes before becoming idle:"),
	            "/purple/away/mins_before_away", 1, 24 * 60, sg);
	pidgin_pref_bind_insensitive_string(hbox, "/purple/away/idle_reporting", "none");

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	gtk_box_append(GTK_BOX(vbox), hbox);
	checkbox(hbox, _("Change to this status when _idle:"),
	         "/purple/away/away_when_idle");
	dd = savedstatus_dropdown("/purple/savedstatus/idleaway");
	gtk_box_append(GTK_BOX(hbox), dd);
	pidgin_pref_bind_sensitive(dd, "/purple/away/away_when_idle", FALSE);

	vbox = pidgin_make_frame(ret, _("Away"));
	dropdown_string(vbox, _("_Auto-reply:"), "/purple/away/auto_reply", sg,
		_("Never"), "never",
		_("When away"), "away",
		_("When both away and idle"), "awayidle",
		NULL);

	vbox = pidgin_make_frame(ret, _("Status at Startup"));
	checkbox(vbox, _("Use status from last _exit at startup"),
	         "/purple/savedstatus/startup_current_status");
	dd = savedstatus_dropdown("/purple/savedstatus/startup");
	hbox = pidgin_add_widget_to_vbox(GTK_BOX(vbox), _("Status to a_pply at startup:"),
	                                 sg, dd, FALSE, NULL);
	pidgin_pref_bind_sensitive(hbox, "/purple/savedstatus/startup_current_status", TRUE);

	g_object_unref(sg);
	return ret;
}

/**************************************************************************
 * Message index
 **************************************************************************/

typedef struct {
	GtkWidget *info;
	GtkWidget *progress;
	GtkWidget *start;
	GtkWidget *pause;
	GtkWidget *resume;
	gboolean paused;
} IndexPage;

static void
index_page_update_info(IndexPage *page)
{
	PidginMessageIndex *idx = pidgin_message_index_get_default();
	PidginBackfill *bf = pidgin_backfill_get_default();
	GStatBuf st;
	char *size = NULL, *text;
	gboolean running = bf != NULL && pidgin_backfill_is_running(bf);

	if (idx == NULL) {
		gtk_label_set_text(GTK_LABEL(page->info),
		                   _("The message index could not be opened."));
		gtk_widget_set_sensitive(page->start, FALSE);
		gtk_widget_set_sensitive(page->pause, FALSE);
		gtk_widget_set_sensitive(page->resume, FALSE);
		return;
	}

	if (g_stat(pidgin_message_index_get_path(idx), &st) == 0)
		size = purple_str_size_to_units(st.st_size);
	text = g_strdup_printf(_("%s\n%s, %" G_GINT64_FORMAT " messages"),
	                       pidgin_message_index_get_path(idx),
	                       size ? size : _("empty"),
	                       pidgin_message_index_count(idx));
	gtk_label_set_text(GTK_LABEL(page->info), text);
	g_free(text);
	g_free(size);

	gtk_widget_set_sensitive(page->start, bf != NULL && !running &&
		purple_prefs_get_bool(PIDGIN4_PREFS_ROOT "/index/backfill"));
	gtk_widget_set_sensitive(page->pause, running && !page->paused);
	gtk_widget_set_sensitive(page->resume, running && page->paused);
	if (!running)
		gtk_progress_bar_set_text(GTK_PROGRESS_BAR(page->progress),
		                          _("The log index is not running."));
}

static void
index_progress_cb(PidginBackfill *bf, guint files_done, guint files_total,
                  guint64 bytes_done, guint64 bytes_total, GtkWidget *progress)
{
	IndexPage *page = g_object_get_data(G_OBJECT(progress), "index-page");
	char *text;

	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(page->progress),
		bytes_total > 0 ? (double)bytes_done / bytes_total : 0.0);
	text = g_strdup_printf(_("%u of %u log files"), files_done, files_total);
	gtk_progress_bar_set_text(GTK_PROGRESS_BAR(page->progress), text);
	g_free(text);
}

static void
index_finished_cb(PidginBackfill *bf, gboolean completed, GtkWidget *progress)
{
	IndexPage *page = g_object_get_data(G_OBJECT(progress), "index-page");

	page->paused = FALSE;
	index_page_update_info(page);
	if (completed) {
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(page->progress), 1.0);
		gtk_progress_bar_set_text(GTK_PROGRESS_BAR(page->progress),
		                          _("All logs are indexed."));
	}
}

static void
index_start_cb(GtkWidget *button, IndexPage *page)
{
	PidginBackfill *bf = pidgin_backfill_get_default();

	if (bf != NULL)
		pidgin_backfill_start(bf);
	page->paused = FALSE;
	index_page_update_info(page);
}

static void
index_pause_cb(GtkWidget *button, IndexPage *page)
{
	PidginBackfill *bf = pidgin_backfill_get_default();

	if (bf != NULL)
		pidgin_backfill_pause(bf);
	page->paused = TRUE;
	index_page_update_info(page);
}

static void
index_resume_cb(GtkWidget *button, IndexPage *page)
{
	PidginBackfill *bf = pidgin_backfill_get_default();

	if (bf != NULL)
		pidgin_backfill_resume(bf);
	page->paused = FALSE;
	index_page_update_info(page);
}

/* Rebuild: forget every log file's rows and cursor (rows with XMPP ids
 * survive, unlinked; see pidgin_message_index_forget_file), then index
 * again. The walk runs in a thread; the index serializes writers. */
static void
forget_dir(PidginMessageIndex *idx, const char *root, const char *rel)
{
	char *path = rel ? g_build_filename(root, rel, NULL) : g_strdup(root);
	GDir *dir = g_dir_open(path, 0, NULL);
	const char *name;

	if (dir != NULL) {
		while ((name = g_dir_read_name(dir)) != NULL) {
			char *child_rel = rel ? g_build_filename(rel, name, NULL) : g_strdup(name);
			char *child = g_build_filename(root, child_rel, NULL);

			if (g_file_test(child, G_FILE_TEST_IS_DIR) &&
			    !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
				forget_dir(idx, root, child_rel);
			else if (g_str_has_suffix(name, ".html") || g_str_has_suffix(name, ".txt"))
				pidgin_message_index_forget_file(idx, child_rel);
			g_free(child);
			g_free(child_rel);
		}
		g_dir_close(dir);
	}
	g_free(path);
}

static void
rebuild_thread(GTask *task, gpointer source, gpointer data, GCancellable *c)
{
	char *logs = g_build_filename(purple_user_dir(), "logs", NULL);

	forget_dir(data, logs, NULL);
	g_free(logs);
	g_task_return_boolean(task, TRUE);
}

static void
rebuild_done_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	PidginBackfill *bf = pidgin_backfill_get_default();

	if (bf != NULL && purple_prefs_get_bool(PIDGIN4_PREFS_ROOT "/index/backfill"))
		pidgin_backfill_start(bf);
	purple_debug_info("prefs", "Message index: forgot all log files; reindexing\n");
}

static void
rebuild_confirm_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	PidginMessageIndex *idx = pidgin_message_index_get_default();
	PidginBackfill *bf = pidgin_backfill_get_default();
	GTask *task;

	if (gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, NULL) != 1 ||
	    idx == NULL)
		return;

	if (bf != NULL)
		pidgin_backfill_cancel(bf);
	task = g_task_new(NULL, NULL, rebuild_done_cb, NULL);
	g_task_set_task_data(task, g_object_ref(idx), g_object_unref);
	g_task_run_in_thread(task, rebuild_thread);
	g_object_unref(task);
}

static void
index_rebuild_cb(GtkWidget *button, IndexPage *page)
{
	GtkAlertDialog *dialog = gtk_alert_dialog_new("%s",
		_("Rebuild the message index from the logs?"));

	gtk_alert_dialog_set_detail(dialog, _("Every log file is indexed again. "
		"Message ids and reactions of messages received in this program "
		"are kept. This can take a long time for large logs."));
	gtk_alert_dialog_set_buttons(dialog,
		(const char *[]){ _("Cancel"), _("Rebuild"), NULL });
	gtk_alert_dialog_set_cancel_button(dialog, 0);
	gtk_alert_dialog_set_default_button(dialog, 0);
	gtk_alert_dialog_choose(dialog, GTK_WINDOW(gtk_widget_get_root(button)), NULL,
	                        rebuild_confirm_cb, page);
	g_object_unref(dialog);
}

static void
index_backfill_pref_cb(const char *name, PurplePrefType type,
                       gconstpointer value, gpointer data)
{
	index_page_update_info(data);
}

static GtkWidget *
index_page(void)
{
	GtkWidget *ret = page_new();
	GtkWidget *vbox, *hbox, *button;
	IndexPage *page = g_new0(IndexPage, 1);
	PidginBackfill *bf = pidgin_backfill_get_default();

	g_object_set_data_full(G_OBJECT(ret), "index-page", page, g_free);

	vbox = pidgin_make_frame(ret, _("Message Index"));
	note_label(vbox, _("The message index is a database next to the logs "
		"(the logs themselves are not changed). It is used for searching "
		"logs, and for message ids, reactions and history from the server."));

	page->info = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(page->info), 0.0);
	gtk_label_set_selectable(GTK_LABEL(page->info), TRUE);
	gtk_label_set_wrap(GTK_LABEL(page->info), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(page->info), PANGO_WRAP_WORD_CHAR);
	gtk_box_append(GTK_BOX(vbox), page->info);

	vbox = pidgin_make_frame(ret, _("Indexing Logs"));
	checkbox(vbox, _("_Index existing logs in the background"),
	         PIDGIN4_PREFS_ROOT "/index/backfill");
	page->progress = gtk_progress_bar_new();
	gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(page->progress), TRUE);
	gtk_box_append(GTK_BOX(vbox), page->progress);

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	gtk_box_append(GTK_BOX(vbox), hbox);
	page->start = gtk_button_new_with_mnemonic(_("_Index Now"));
	g_signal_connect(page->start, "clicked", G_CALLBACK(index_start_cb), page);
	gtk_box_append(GTK_BOX(hbox), page->start);
	page->pause = gtk_button_new_with_mnemonic(_("_Pause"));
	g_signal_connect(page->pause, "clicked", G_CALLBACK(index_pause_cb), page);
	gtk_box_append(GTK_BOX(hbox), page->pause);
	page->resume = gtk_button_new_with_mnemonic(_("_Resume"));
	g_signal_connect(page->resume, "clicked", G_CALLBACK(index_resume_cb), page);
	gtk_box_append(GTK_BOX(hbox), page->resume);
	button = gtk_button_new_with_mnemonic(_("Re_build..."));
	g_signal_connect(button, "clicked", G_CALLBACK(index_rebuild_cb), page);
	gtk_box_append(GTK_BOX(hbox), button);

	g_object_set_data(G_OBJECT(page->progress), "index-page", page);
	if (bf != NULL) {
		g_signal_connect_object(bf, "progress", G_CALLBACK(index_progress_cb),
		                        page->progress, 0);
		g_signal_connect_object(bf, "finished", G_CALLBACK(index_finished_cb),
		                        page->progress, 0);
	}
	purple_prefs_connect_callback(ret, PIDGIN4_PREFS_ROOT "/index/backfill",
	                              index_backfill_pref_cb, page);
	g_signal_connect_swapped(ret, "destroy",
	                         G_CALLBACK(purple_prefs_disconnect_by_handle), ret);

	index_page_update_info(page);
	return ret;
}

/**************************************************************************
 * The window
 **************************************************************************/

static void
prefs_window_destroy_cb(GtkWidget *window, gpointer data)
{
	prefs_window = NULL;
	prefs_stack = NULL;
}

void
pidgin_prefs_show(void)
{
	GtkWidget *hbox, *sidebar;

	if (prefs_window != NULL) {
		gtk_window_present(GTK_WINDOW(prefs_window));
		return;
	}

	if (bound_prefs != NULL)
		g_ptr_array_set_size(bound_prefs, 0);

	prefs_window = gtk_window_new();
	gtk_window_set_application(GTK_WINDOW(prefs_window), pidgin_application_get());
	gtk_window_set_title(GTK_WINDOW(prefs_window), _("Preferences"));
	gtk_window_set_default_size(GTK_WINDOW(prefs_window), 760, 560);
	gtk_widget_set_name(prefs_window, "preferences");
	g_signal_connect(prefs_window, "destroy", G_CALLBACK(prefs_window_destroy_cb), NULL);

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	prefs_stack = gtk_stack_new();
	gtk_widget_set_hexpand(prefs_stack, TRUE);
	sidebar = gtk_stack_sidebar_new();
	gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(sidebar), GTK_STACK(prefs_stack));
	gtk_box_append(GTK_BOX(hbox), sidebar);
	gtk_box_append(GTK_BOX(hbox), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
	gtk_box_append(GTK_BOX(hbox), prefs_stack);
	gtk_window_set_child(GTK_WINDOW(prefs_window), hbox);

	add_page("interface", _("Interface"), interface_page());
	add_page("conversations", _("Conversations"), conv_page());
	add_page("smileys", _("Smiley Themes"), smiley_page());
	add_page("themes", _("Themes"), theme_page());
	add_page("sounds", _("Sounds"), sound_page());
	add_page("network", _("Network"), network_page());
	add_page("browser", _("Browser"), browser_page());
	add_page("logging", _("Logging"), logging_page());
	add_page("status", _("Status / Idle"), away_page());
	add_page("index", _("Message Index"), index_page());

	gtk_window_present(GTK_WINDOW(prefs_window));
}

void
pidgin_prefs_hide(void)
{
	if (prefs_window != NULL)
		gtk_window_destroy(GTK_WINDOW(prefs_window));
}

/**************************************************************************
 * Selftest
 **************************************************************************/

static char *
pref_value_string(const char *name)
{
	GList *list, *l;
	GString *str;

	if (!purple_prefs_exists(name))
		return g_strdup("<missing>");

	switch (purple_prefs_get_type(name)) {
	case PURPLE_PREF_BOOLEAN:
		return g_strdup(purple_prefs_get_bool(name) ? "true" : "false");
	case PURPLE_PREF_INT:
		return g_strdup_printf("%d", purple_prefs_get_int(name));
	case PURPLE_PREF_STRING:
		return g_strdup(purple_prefs_get_string(name));
	case PURPLE_PREF_PATH:
		return g_strdup(purple_prefs_get_path(name));
	case PURPLE_PREF_STRING_LIST:
	case PURPLE_PREF_PATH_LIST:
		list = purple_prefs_get_type(name) == PURPLE_PREF_PATH_LIST ?
			purple_prefs_get_path_list(name) : purple_prefs_get_string_list(name);
		str = g_string_new(NULL);
		for (l = list; l != NULL; l = l->next)
			g_string_append_printf(str, "%s;", (char *)l->data);
		g_list_free_full(list, g_free);
		return g_string_free(str, FALSE);
	default:
		return g_strdup("");
	}
}

void
pidgin_prefs_selftest(void)
{
	static const char *const pages[] = {
		"interface", "conversations", "smileys", "themes", "sounds",
		"network", "browser", "logging", "status", "index",
	};
	GHashTable *before = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	GPtrArray *names;
	gsize i;
	guint j, changed = 0;
	gboolean toggled;

	pidgin_prefs_hide();
	pidgin_prefs_show();
	pidgin_selftest_iterate(100);
	if (prefs_window == NULL || bound_prefs == NULL || bound_prefs->len == 0) {
		pidgin_selftest_fail("prefs", "the window did not open");
		g_hash_table_destroy(before);
		return;
	}
	/* Snapshot after opening too: opening must not have written. The
	 * list of bound prefs is known only once the pages exist. */
	names = g_ptr_array_new_with_free_func(g_free);
	for (j = 0; j < bound_prefs->len; j++)
		g_ptr_array_add(names, g_strdup(g_ptr_array_index(bound_prefs, j)));
	pidgin_prefs_hide();
	pidgin_selftest_iterate(50);
	for (j = 0; j < names->len; j++) {
		const char *name = g_ptr_array_index(names, j);
		g_hash_table_replace(before, g_strdup(name), pref_value_string(name));
	}

	pidgin_prefs_show();
	for (i = 0; i < G_N_ELEMENTS(pages); i++) {
		GtkWidget *child;

		gtk_stack_set_visible_child_name(GTK_STACK(prefs_stack), pages[i]);
		pidgin_selftest_iterate(60);
		child = gtk_stack_get_visible_child(GTK_STACK(prefs_stack));
		if (child == NULL || !purple_strequal(
		        gtk_stack_get_visible_child_name(GTK_STACK(prefs_stack)), pages[i]))
			pidgin_selftest_fail("prefs", "page %s missing", pages[i]);
		else {
			char *shot = g_strdup_printf("prefs-%s", pages[i]);

			pidgin_selftest_log("prefs", "page %s", pages[i]);
			pidgin_selftest_screenshot(prefs_window, shot);
			g_free(shot);
		}
	}

	/* A binding round trip on a harmless shared pref: the value written
	 * through the pref shows in the window's widget and back. */
	toggled = purple_prefs_get_bool(CONV_PREFS "/use_smooth_scrolling");
	purple_prefs_set_bool(CONV_PREFS "/use_smooth_scrolling", !toggled);
	pidgin_selftest_iterate(20);
	purple_prefs_set_bool(CONV_PREFS "/use_smooth_scrolling", toggled);

	pidgin_prefs_hide();
	pidgin_selftest_iterate(50);
	if (prefs_window != NULL)
		pidgin_selftest_fail("prefs", "the window did not close");

	for (j = 0; j < names->len; j++) {
		const char *name = g_ptr_array_index(names, j);
		char *now = pref_value_string(name);

		if (!purple_strequal(now, g_hash_table_lookup(before, name))) {
			pidgin_selftest_fail("prefs", "opening the window changed %s "
				"(%s -> %s)", name, (char *)g_hash_table_lookup(before, name), now);
			changed++;
		}
		g_free(now);
	}
	pidgin_selftest_log("prefs", "%u bound prefs, %u changed", names->len, changed);
	g_ptr_array_free(names, TRUE);
	g_hash_table_destroy(before);
}
