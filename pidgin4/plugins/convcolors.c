/*
 * pidgin4: Conversation Colors (M7 port of pidgin/plugins/convcolors.c).
 *
 * Copyright (C) 2006 Sadrul Habib Chowdhury <sadrul@users.sourceforge.net>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301
 * USA.
 */

/*
 * Pidgin 2 wrapped each displayed message in <FONT COLOR><B><I><U>. Here
 * the message view's rows carry their type as CSS classes (msg-send,
 * msg-recv, msg-nick, msg-system, msg-error), and the plugin installs a
 * CSS provider that colours and styles the bodies of those rows in the
 * views it applies to: views of IMs get the class "convcolors-ims", views
 * of chats "convcolors-chats", following the prefs. "Ignore incoming
 * format" still strips the HTML in displaying-im-msg/-chat-msg, as before.
 * The prefs are Pidgin 2's (/plugins/gtk/gtk-plugin_pack-convcolors/...).
 */
#include "pidgin4-plugin.h"

#define PLUGIN_ID           "gtk-plugin_pack-convcolors"
#define PLUGIN_NAME         N_("Conversation Colors")
#define PLUGIN_STATIC_NAME  ConversationColors
#define PLUGIN_SUMMARY      N_("Customize colors in the conversation window")
#define PLUGIN_DESCRIPTION  N_("Customize colors in the conversation window")
#define PLUGIN_AUTHOR       "Sadrul H Chowdhury <sadrul@users.sourceforge.net>"

#define PREF_PREFIX   "/plugins/gtk/" PLUGIN_ID
#define PREF_IGNORE   PREF_PREFIX "/ignore_incoming"
#define PREF_CHATS    PREF_PREFIX "/chats"
#define PREF_IMS      PREF_PREFIX "/ims"

#define PREF_SEND     PREF_PREFIX "/send"
#define PREF_SEND_C   PREF_SEND "/color"
#define PREF_SEND_F   PREF_SEND "/format"
#define PREF_SEND_E   PREF_SEND "/enabled"

#define PREF_RECV     PREF_PREFIX "/recv"
#define PREF_RECV_C   PREF_RECV "/color"
#define PREF_RECV_F   PREF_RECV "/format"
#define PREF_RECV_E   PREF_RECV "/enabled"

#define PREF_SYSTEM   PREF_PREFIX "/system"
#define PREF_SYSTEM_C PREF_SYSTEM "/color"
#define PREF_SYSTEM_F PREF_SYSTEM "/format"
#define PREF_SYSTEM_E PREF_SYSTEM "/enabled"

#define PREF_ERROR    PREF_PREFIX "/error"
#define PREF_ERROR_C  PREF_ERROR "/color"
#define PREF_ERROR_F  PREF_ERROR "/format"
#define PREF_ERROR_E  PREF_ERROR "/enabled"

#define PREF_NICK     PREF_PREFIX "/nick"
#define PREF_NICK_C   PREF_NICK "/color"
#define PREF_NICK_F   PREF_NICK "/format"
#define PREF_NICK_E   PREF_NICK "/enabled"

#define CLASS_IMS   "convcolors-ims"
#define CLASS_CHATS "convcolors-chats"

enum
{
	FONT_BOLD      = 1 << 0,
	FONT_ITALIC    = 1 << 1,
	FONT_UNDERLINE = 1 << 2
};

/* In Pidgin 2's order of precedence: the first matching flag wins. */
static struct
{
	PurpleMessageFlags flag;
	char *prefix;
	const char *text;
	const char *row_class;
} formats[] =
{
	{PURPLE_MESSAGE_ERROR, PREF_ERROR, N_("Error Messages"), "msg-error"},
	{PURPLE_MESSAGE_NICK, PREF_NICK, N_("Highlighted Messages"), "msg-nick"},
	{PURPLE_MESSAGE_SYSTEM, PREF_SYSTEM, N_("System Messages"), "msg-system"},
	{PURPLE_MESSAGE_SEND, PREF_SEND, N_("Sent Messages"), "msg-send"},
	{PURPLE_MESSAGE_RECV, PREF_RECV, N_("Received Messages"), "msg-recv"},
	{0, NULL, NULL, NULL}
};

static PurplePlugin *my_plugin = NULL;
static GtkCssProvider *provider = NULL;

static const char *
pref_string(const char *prefix, const char *leaf)
{
	char tmp[128];

	g_snprintf(tmp, sizeof(tmp), "%s/%s", prefix, leaf);
	return purple_prefs_get_string(tmp);
}

static int
pref_int(const char *prefix, const char *leaf)
{
	char tmp[128];

	g_snprintf(tmp, sizeof(tmp), "%s/%s", prefix, leaf);
	return purple_prefs_get_int(tmp);
}

static gboolean
pref_bool(const char *prefix, const char *leaf)
{
	char tmp[128];

	g_snprintf(tmp, sizeof(tmp), "%s/%s", prefix, leaf);
	return purple_prefs_get_bool(tmp);
}

/* The first format (in precedence order) of these flags, or -1. */
static int
format_for(PurpleMessageFlags flags)
{
	int i;

	for (i = 0; formats[i].prefix; i++)
		if (flags & formats[i].flag)
			return i;
	return -1;
}

static gboolean
applies_to(PurpleConversation *conv)
{
	PurpleConversationType type = purple_conversation_get_type(conv);

	return (type == PURPLE_CONV_TYPE_IM && purple_prefs_get_bool(PREF_IMS)) ||
	       (type == PURPLE_CONV_TYPE_CHAT && purple_prefs_get_bool(PREF_CHATS));
}

/**************************************************************************
 * The CSS
 **************************************************************************/

static void
update_css(void)
{
	GString *css = g_string_new(NULL);
	int i;

	/* Lowest precedence first, so that for a row with several classes
	 * (msg-recv msg-nick) the rule of the stronger one comes last. */
	for (i = G_N_ELEMENTS(formats) - 2; i >= 0; i--) {
		const char *color;
		GdkRGBA rgba;
		int f;

		if (!pref_bool(formats[i].prefix, "enabled"))
			continue;
		color = pref_string(formats[i].prefix, "color");
		f = pref_int(formats[i].prefix, "format");

		g_string_append_printf(css,
			"." CLASS_IMS " pidgin-message-row.%s .body,\n"
			"." CLASS_CHATS " pidgin-message-row.%s .body {\n",
			formats[i].row_class, formats[i].row_class);
		if (color != NULL && *color && gdk_rgba_parse(&rgba, color)) {
			char *c = gdk_rgba_to_string(&rgba);

			g_string_append_printf(css, "\tcolor: %s;\n", c);
			g_free(c);
		}
		g_string_append_printf(css, "\tfont-weight: %s;\n", (f & FONT_BOLD) ? "bold" : "normal");
		g_string_append_printf(css, "\tfont-style: %s;\n", (f & FONT_ITALIC) ? "italic" : "normal");
		g_string_append_printf(css, "\ttext-decoration: %s;\n",
		                       (f & FONT_UNDERLINE) ? "underline" : "none");
		g_string_append(css, "}\n");
	}

	if (provider == NULL) {
		provider = gtk_css_provider_new();
		gtk_style_context_add_provider_for_display(gdk_display_get_default(),
			GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
	}
	gtk_css_provider_load_from_string(provider, css->str);
	g_string_free(css, TRUE);
}

static void
apply_to_view(PurpleConversation *conv, gboolean enable)
{
	PidginMessageView *view = pidgin4_plugin_conv_view(conv);
	GtkWidget *w;
	gboolean im;

	if (view == NULL)
		return;
	w = GTK_WIDGET(view);
	im = purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_IM;
	gtk_widget_remove_css_class(w, CLASS_IMS);
	gtk_widget_remove_css_class(w, CLASS_CHATS);
	if (enable && applies_to(conv))
		gtk_widget_add_css_class(w, im ? CLASS_IMS : CLASS_CHATS);
}

static void
apply_all(gboolean enable)
{
	GList *l;

	for (l = purple_get_conversations(); l != NULL; l = l->next)
		apply_to_view(l->data, enable);
}

static void
conv_displayed_cb(PidginConversation *gtkconv, gpointer data)
{
	apply_to_view(pidgin_conv_get_conversation(gtkconv), TRUE);
}

static void
prefs_changed_cb(const char *name, PurplePrefType type, gconstpointer val, gpointer data)
{
	update_css();
	apply_all(TRUE);
}

/**************************************************************************
 * Ignoring incoming formatting
 **************************************************************************/

static gboolean
displaying_msg(PurpleAccount *account, const char *who, char **displaying,
               PurpleConversation *conv, PurpleMessageFlags flags)
{
	int i = format_for(flags);
	char *t;

	if (i < 0 || !pref_bool(formats[i].prefix, "enabled") || !applies_to(conv))
		return FALSE;

	if (purple_prefs_get_bool(PREF_IGNORE)) {
		/* This seems to be necessary, especially for received messages. */
		t = *displaying;
		*displaying = purple_strreplace(t, "\n", "<br>");
		g_free(t);

		t = *displaying;
		*displaying = purple_markup_strip_html(t);
		g_free(t);

		t = *displaying;
		*displaying = g_markup_escape_text(t, -1);
		g_free(t);

		/* Restore the links */
		t = *displaying;
		*displaying = purple_markup_linkify(t);
		g_free(t);
	}

	return FALSE;
}

/**************************************************************************
 * Plugin
 **************************************************************************/

static gboolean
plugin_load(PurplePlugin *plugin)
{
	my_plugin = plugin;

	purple_signal_connect(pidgin_conversations_get_handle(), "displaying-im-msg", plugin,
	                      PURPLE_CALLBACK(displaying_msg), NULL);
	purple_signal_connect(pidgin_conversations_get_handle(), "displaying-chat-msg", plugin,
	                      PURPLE_CALLBACK(displaying_msg), NULL);
	purple_signal_connect(pidgin_conversations_get_handle(), "conversation-displayed", plugin,
	                      PURPLE_CALLBACK(conv_displayed_cb), NULL);
	purple_prefs_connect_callback(plugin, PREF_PREFIX, prefs_changed_cb, NULL);

	update_css();
	apply_all(TRUE);
	return TRUE;
}

static gboolean
plugin_unload(PurplePlugin *plugin)
{
	apply_all(FALSE);
	if (provider != NULL) {
		gtk_style_context_remove_provider_for_display(gdk_display_get_default(),
			GTK_STYLE_PROVIDER(provider));
		g_clear_object(&provider);
	}
	my_plugin = NULL;
	return TRUE;
}

/**************************************************************************
 * The configuration frame (GTK 4)
 **************************************************************************/

static void
toggle_enabled(GtkCheckButton *button, gpointer data)
{
	char tmp[128];

	g_snprintf(tmp, sizeof(tmp), "%s/enabled", (const char *)data);
	purple_prefs_set_bool(tmp, gtk_check_button_get_active(button));
}

static void
toggle_format(GtkCheckButton *button, gpointer data)
{
	const char *prefix = data;
	int bit = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "convcolors-bit"));
	char tmp[128];
	int f;

	g_snprintf(tmp, sizeof(tmp), "%s/format", prefix);
	f = purple_prefs_get_int(tmp);
	if (gtk_check_button_get_active(button))
		f |= bit;
	else
		f &= ~bit;
	purple_prefs_set_int(tmp, f);
}

static void
color_set_cb(GtkColorDialogButton *button, GParamSpec *pspec, gpointer data)
{
	const GdkRGBA *rgba = gtk_color_dialog_button_get_rgba(button);
	char tmp[128], colorstr[8];

	g_snprintf(colorstr, sizeof(colorstr), "#%02X%02X%02X",
	           (int)(rgba->red * 255 + 0.5), (int)(rgba->green * 255 + 0.5),
	           (int)(rgba->blue * 255 + 0.5));
	g_snprintf(tmp, sizeof(tmp), "%s/color", (const char *)data);
	if (!purple_strequal(purple_prefs_get_string(tmp), colorstr))
		purple_prefs_set_string(tmp, colorstr);
}

static void
sensitive_cb(GtkCheckButton *enabled, GtkWidget *widget)
{
	gtk_widget_set_sensitive(widget, gtk_check_button_get_active(enabled));
}

static GtkWidget *
check_for(const char *label, gboolean active)
{
	GtkWidget *button = gtk_check_button_new_with_mnemonic(label);

	gtk_check_button_set_active(GTK_CHECK_BUTTON(button), active);
	return button;
}

static void
pref_check_cb(GtkCheckButton *button, gpointer data)
{
	purple_prefs_set_bool(data, gtk_check_button_get_active(button));
}

static GtkWidget *
get_config_frame(PurplePlugin *plugin)
{
	GtkWidget *ret, *frame, *hbox, *button, *enabled;
	static const struct { int bit; const char *label; } styles[] = {
		{ FONT_BOLD, N_("Bold") },
		{ FONT_ITALIC, N_("Italic") },
		{ FONT_UNDERLINE, N_("Underline") },
	};
	int i;
	guint j;

	ret = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_CAT_SPACE);
	gtk_widget_set_margin_top(ret, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_bottom(ret, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_start(ret, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_end(ret, PIDGIN_HIG_BORDER);

	for (i = 0; formats[i].prefix; i++) {
		gboolean e = pref_bool(formats[i].prefix, "enabled");
		int f = pref_int(formats[i].prefix, "format");
		GtkColorDialog *dialog;
		GdkRGBA rgba;
		char title[128];

		frame = pidgin_make_frame(ret, _(formats[i].text));
		hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
		gtk_box_append(GTK_BOX(frame), hbox);

		enabled = check_for(_("Enabled"), e);
		g_signal_connect(enabled, "toggled", G_CALLBACK(toggle_enabled), formats[i].prefix);
		gtk_box_append(GTK_BOX(hbox), enabled);

		dialog = gtk_color_dialog_new();
		g_snprintf(title, sizeof(title), _("Select Color for %s"), _(formats[i].text));
		gtk_color_dialog_set_title(dialog, title);
		gtk_color_dialog_set_with_alpha(dialog, FALSE);
		button = gtk_color_dialog_button_new(dialog);
		if (gdk_rgba_parse(&rgba, pref_string(formats[i].prefix, "color")))
			gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(button), &rgba);
		g_signal_connect(button, "notify::rgba", G_CALLBACK(color_set_cb), formats[i].prefix);
		gtk_widget_set_sensitive(button, e);
		g_signal_connect(enabled, "toggled", G_CALLBACK(sensitive_cb), button);
		gtk_box_append(GTK_BOX(hbox), button);

		for (j = 0; j < G_N_ELEMENTS(styles); j++) {
			button = check_for(_(styles[j].label), (f & styles[j].bit) != 0);
			g_object_set_data(G_OBJECT(button), "convcolors-bit",
			                  GINT_TO_POINTER(styles[j].bit));
			g_signal_connect(button, "toggled", G_CALLBACK(toggle_format), formats[i].prefix);
			gtk_widget_set_sensitive(button, e);
			g_signal_connect(enabled, "toggled", G_CALLBACK(sensitive_cb), button);
			gtk_box_append(GTK_BOX(hbox), button);
		}
	}

	frame = pidgin_make_frame(ret, _("General"));
	button = check_for(_("Ignore incoming format"), purple_prefs_get_bool(PREF_IGNORE));
	g_signal_connect(button, "toggled", G_CALLBACK(pref_check_cb), PREF_IGNORE);
	gtk_box_append(GTK_BOX(frame), button);
	button = check_for(_("Apply in Chats"), purple_prefs_get_bool(PREF_CHATS));
	g_signal_connect(button, "toggled", G_CALLBACK(pref_check_cb), PREF_CHATS);
	gtk_box_append(GTK_BOX(frame), button);
	button = check_for(_("Apply in IMs"), purple_prefs_get_bool(PREF_IMS));
	g_signal_connect(button, "toggled", G_CALLBACK(pref_check_cb), PREF_IMS);
	gtk_box_append(GTK_BOX(frame), button);

	return ret;
}

static PidginPluginUiInfo ui_info =
{
	get_config_frame,
	0,

	/* padding */
	NULL,
	NULL,
	NULL,
	NULL
};

static PurplePluginInfo info =
{
	PURPLE_PLUGIN_MAGIC,            /* Magic              */
	PURPLE_MAJOR_VERSION,           /* Purple Major Version */
	PURPLE_MINOR_VERSION,           /* Purple Minor Version */
	PURPLE_PLUGIN_STANDARD,         /* plugin type        */
	PIDGIN_PLUGIN_TYPE,             /* ui requirement     */
	0,                              /* flags              */
	NULL,                           /* dependencies       */
	PURPLE_PRIORITY_DEFAULT,        /* priority           */

	PLUGIN_ID,                      /* plugin id          */
	PLUGIN_NAME,                    /* name               */
	DISPLAY_VERSION,                /* version            */
	PLUGIN_SUMMARY,                 /* summary            */
	PLUGIN_DESCRIPTION,             /* description        */
	PLUGIN_AUTHOR,                  /* author             */
	PURPLE_WEBSITE,                 /* website            */

	plugin_load,                    /* load               */
	plugin_unload,                  /* unload             */
	NULL,                           /* destroy            */

	&ui_info,                       /* ui_info            */
	NULL,                           /* extra_info         */
	NULL,                           /* prefs_info         */
	NULL,                           /* actions            */

	/* padding */
	NULL,
	NULL,
	NULL,
	NULL
};

static void
init_plugin(PurplePlugin *plugin)
{
	purple_prefs_add_none("/plugins/gtk");
	purple_prefs_add_none(PREF_PREFIX);

	purple_prefs_add_bool(PREF_IGNORE, TRUE);
	purple_prefs_add_bool(PREF_CHATS, TRUE);
	purple_prefs_add_bool(PREF_IMS, TRUE);

	purple_prefs_add_none(PREF_SEND);
	purple_prefs_add_none(PREF_RECV);
	purple_prefs_add_none(PREF_SYSTEM);
	purple_prefs_add_none(PREF_ERROR);
	purple_prefs_add_none(PREF_NICK);

	purple_prefs_add_string(PREF_SEND_C, "#909090");
	purple_prefs_add_string(PREF_RECV_C, "#000000");
	purple_prefs_add_string(PREF_SYSTEM_C, "#50a050");
	purple_prefs_add_string(PREF_ERROR_C, "#ff0000");
	purple_prefs_add_string(PREF_NICK_C, "#0000dd");

	purple_prefs_add_int(PREF_SEND_F, 0);
	purple_prefs_add_int(PREF_RECV_F, 0);
	purple_prefs_add_int(PREF_SYSTEM_F, FONT_ITALIC);
	purple_prefs_add_int(PREF_ERROR_F, FONT_BOLD | FONT_UNDERLINE);
	purple_prefs_add_int(PREF_NICK_F, FONT_BOLD);

	purple_prefs_add_bool(PREF_SEND_E, TRUE);
	purple_prefs_add_bool(PREF_RECV_E, TRUE);
	purple_prefs_add_bool(PREF_SYSTEM_E, TRUE);
	purple_prefs_add_bool(PREF_ERROR_E, TRUE);
	purple_prefs_add_bool(PREF_NICK_E, TRUE);
}

PURPLE_INIT_PLUGIN(PLUGIN_STATIC_NAME, init_plugin, info)
