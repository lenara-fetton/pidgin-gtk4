/*
 * pidgin4: XMPP Console (M7 port of pidgin/plugins/xmppconsole.c).
 *
 * Purple - XMPP debugging tool
 *
 * Copyright (C) 2002-2003, Sean Egan
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
 *
 */

/*
 * The traffic of one XMPP connection ("jabber-receiving-xmlnode" and
 * "jabber-sending-text" of prpl-jabber), pretty-printed as Pango markup in
 * a read-only GtkTextView: received stanzas on a red tint, sent ones on a
 * green one. Below it a monospace GtkTextView to type raw XML into (Enter
 * sends it with the prpl's send_raw, Shift+Enter is a newline; it is
 * tinted red while it doesn't parse), and <iq/>, <presence/> and
 * <message/> buttons whose popovers fill in a stanza skeleton. A drop-down
 * picks the connection when more than one XMPP account is signed in.
 */
#include "pidgin4-plugin.h"

#include "xmlnode.h"

typedef struct {
	PurpleConnection *gc;
	GtkWidget *window;
	GtkWidget *hbox;
	GtkWidget *dropdown;
	GtkStringList *names;
	GtkWidget *view;
	GtkTextBuffer *buffer;
	GtkWidget *entry;
	GtkWidget *sw;
	int count;
	GList *accounts;
} XmppConsole;

static XmppConsole *console = NULL;
static void *xmpp_console_handle = NULL;
static GtkCssProvider *console_css = NULL;

#define BRACKET_COLOR "#940f8c"
#define TAG_COLOR "#8b1dab"
#define ATTR_NAME_COLOR "#a02961"
#define ATTR_VALUE_COLOR "#324aa4"
#define XMLNS_COLOR "#2cb12f"

/* Pango markup of @node, indented by @depth tabs. */
static char *
xmlnode_to_pretty_str(xmlnode *node, int *len, int depth)
{
	GString *text = g_string_new("");
	xmlnode *c;
	char *node_name, *esc, *esc2, *tab = NULL;
	gboolean need_end = FALSE, pretty = TRUE;

	g_return_val_if_fail(node != NULL, NULL);

	if (pretty && depth) {
		tab = g_strnfill(depth, '\t');
		text = g_string_append(text, tab);
	}

	node_name = g_markup_escape_text(node->name, -1);
	g_string_append_printf(text,
	                       "<span foreground='" BRACKET_COLOR "'>&lt;</span>"
	                       "<span foreground='" TAG_COLOR "'><b>%s</b></span>",
	                       node_name);

	if (node->xmlns) {
		if ((!node->parent ||
		     !node->parent->xmlns ||
		     !purple_strequal(node->xmlns, node->parent->xmlns)) &&
		    !purple_strequal(node->xmlns, "jabber:client"))
		{
			char *xmlns = g_markup_escape_text(node->xmlns, -1);
			g_string_append_printf(text,
			                       " <span foreground='" ATTR_NAME_COLOR "'><b>xmlns</b></span>="
			                       "'<span foreground='" XMLNS_COLOR "'><b>%s</b></span>'",
			                       xmlns);
			g_free(xmlns);
		}
	}
	for (c = node->child; c; c = c->next)
	{
		if (c->type == XMLNODE_TYPE_ATTRIB) {
			esc = g_markup_escape_text(c->name, -1);
			esc2 = g_markup_escape_text(c->data, -1);
			g_string_append_printf(text,
			                       " <span foreground='" ATTR_NAME_COLOR "'><b>%s</b></span>="
			                       "'<span foreground='" ATTR_VALUE_COLOR "'>%s</span>'",
			                       esc, esc2);
			g_free(esc);
			g_free(esc2);
		} else if (c->type == XMLNODE_TYPE_TAG || c->type == XMLNODE_TYPE_DATA) {
			if (c->type == XMLNODE_TYPE_DATA)
				pretty = FALSE;
			need_end = TRUE;
		}
	}

	if (need_end) {
		g_string_append_printf(text,
		                       "<span foreground='" BRACKET_COLOR "'>&gt;</span>%s",
		                       pretty ? "\n" : "");

		for (c = node->child; c; c = c->next)
		{
			if (c->type == XMLNODE_TYPE_TAG) {
				int esc_len;
				esc = xmlnode_to_pretty_str(c, &esc_len, depth+1);
				text = g_string_append_len(text, esc, esc_len);
				g_free(esc);
			} else if (c->type == XMLNODE_TYPE_DATA && c->data_sz > 0) {
				esc = g_markup_escape_text(c->data, c->data_sz);
				text = g_string_append(text, esc);
				g_free(esc);
			}
		}

		if(tab && pretty)
			text = g_string_append(text, tab);
		g_string_append_printf(text,
		                       "<span foreground='" BRACKET_COLOR "'>&lt;</span>/"
		                       "<span foreground='" TAG_COLOR "'><b>%s</b></span>"
		                       "<span foreground='" BRACKET_COLOR "'>&gt;</span>\n",
		                       node_name);
	} else {
		g_string_append_printf(text,
		                       "/<span foreground='" BRACKET_COLOR "'>&gt;</span>\n");
	}

	g_free(node_name);

	g_free(tab);

	if(len)
		*len = text->len;

	return g_string_free(text, FALSE);
}

/* Appends Pango @markup to the log, with the paragraph tag @tag (may be NULL). */
static void
append_markup(const char *markup, const char *tag)
{
	GtkTextIter end;
	GtkTextMark *start_mark;
	GtkTextIter start;
	GtkAdjustment *adj;
	gboolean at_end;

	if (console == NULL)
		return;

	adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(console->view));
	at_end = gtk_adjustment_get_value(adj) + gtk_adjustment_get_page_size(adj) >=
	         gtk_adjustment_get_upper(adj) - 1;

	gtk_text_buffer_get_end_iter(console->buffer, &end);
	start_mark = gtk_text_buffer_create_mark(console->buffer, NULL, &end, TRUE);
	gtk_text_buffer_insert_markup(console->buffer, &end, markup, -1);
	if (tag != NULL) {
		gtk_text_buffer_get_iter_at_mark(console->buffer, &start, start_mark);
		gtk_text_buffer_get_end_iter(console->buffer, &end);
		gtk_text_buffer_apply_tag_by_name(console->buffer, tag, &start, &end);
	}
	gtk_text_buffer_delete_mark(console->buffer, start_mark);

	if (at_end) {
		gtk_text_buffer_get_end_iter(console->buffer, &end);
		gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(console->view), &end, 0, FALSE, 0, 0);
	}
}

static void
xmlnode_received_cb(PurpleConnection *gc, xmlnode **packet, gpointer null)
{
	char *str;

	if (!console || console->gc != gc)
		return;
	str = xmlnode_to_pretty_str(*packet, NULL, 0);
	append_markup(str, "received");
	g_free(str);
}

static void
xmlnode_sent_cb(PurpleConnection *gc, char **packet, gpointer null)
{
	char *str;
	xmlnode *node;

	if (!console || console->gc != gc)
		return;
	node = xmlnode_from_str(*packet, -1);

	if (!node)
		return;

	str = xmlnode_to_pretty_str(node, NULL, 0);
	append_markup(str, "sent");
	g_free(str);
	xmlnode_free(node);
}

/**************************************************************************
 * The raw XML entry
 **************************************************************************/

static char *
entry_get_text(void)
{
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(console->entry));
	GtkTextIter start, end;

	gtk_text_buffer_get_bounds(buffer, &start, &end);
	return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

static void
message_send(void)
{
	PurplePluginProtocolInfo *prpl_info = NULL;
	PurpleConnection *gc;
	char *text;

	gc = console->gc;

	if (gc)
		prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(gc->prpl);

	text = entry_get_text();

	if (prpl_info && prpl_info->send_raw != NULL)
		prpl_info->send_raw(gc, text, strlen(text));

	g_free(text);
	gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(console->entry)), "", -1);
}

static gboolean
entry_key_cb(GtkEventControllerKey *controller, guint keyval, guint keycode,
             GdkModifierType state, gpointer data)
{
	if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) &&
	    !(state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK))) {
		message_send();
		return TRUE;
	}
	return FALSE;
}

static void
entry_changed_cb(GtkTextBuffer *buffer, void *data)
{
	char *xmlstr, *str;
	xmlnode *node;
	int lines = MIN(gtk_text_buffer_get_line_count(buffer), 6);

	/* grow with the text, up to 6 lines */
	gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(console->sw), lines * 18 + 8);

	str = entry_get_text();
	if (!str)
		return;
	xmlstr = g_strdup_printf("<xml>%s</xml>", str);
	node = xmlnode_from_str(xmlstr, -1);
	if (node || *str == '\0')
		gtk_widget_remove_css_class(console->entry, "xmpp-console-invalid");
	else
		gtk_widget_add_css_class(console->entry, "xmpp-console-invalid");
	g_free(str);
	g_free(xmlstr);
	if (node)
		xmlnode_free(node);
}

/**************************************************************************
 * The <iq/>, <presence/> and <message/> popovers
 **************************************************************************/

typedef enum {
	STANZA_IQ,
	STANZA_PRESENCE,
	STANZA_MESSAGE
} StanzaKind;

typedef struct {
	const char *label;
	const char *const *options;   /* NULL: an entry */
} StanzaField;

static const char *const iq_types[] = { "get", "set", "result", "error", NULL };
static const char *const presence_types[] = { "default", "unavailable", "subscribe",
	"unsubscribe", "subscribed", "unsubscribed", "probe", "error", NULL };
static const char *const presence_shows[] = { "default", "away", "dnd", "xa", "chat", NULL };
static const char *const message_types[] = { "chat", "headline", "groupchat", "normal",
	"error", NULL };

static const StanzaField iq_fields[] = {
	{ "To:", NULL }, { "Type:", iq_types }, { NULL, NULL }
};
static const StanzaField presence_fields[] = {
	{ "To:", NULL }, { "Type:", presence_types }, { "Show:", presence_shows },
	{ "Status:", NULL }, { "Priority:", NULL }, { NULL, NULL }
};
static const StanzaField message_fields[] = {
	{ "To:", NULL }, { "Type:", message_types }, { "Body:", NULL },
	{ "Subject:", NULL }, { "Thread:", NULL }, { NULL, NULL }
};

static const char *
field_value(GtkWidget **widgets, int i)
{
	if (GTK_IS_DROP_DOWN(widgets[i])) {
		GtkStringObject *obj = gtk_drop_down_get_selected_item(GTK_DROP_DOWN(widgets[i]));

		return obj ? gtk_string_object_get_string(obj) : "";
	}
	return gtk_editable_get_text(GTK_EDITABLE(widgets[i]));
}

static void
append_attr(GString *s, const char *name, const char *value)
{
	char *esc;

	if (value == NULL || *value == '\0')
		return;
	esc = g_markup_escape_text(value, -1);
	g_string_append_printf(s, " %s='%s'", name, esc);
	g_free(esc);
}

static void
append_child(GString *s, const char *name, const char *value)
{
	char *esc;

	if (value == NULL || *value == '\0')
		return;
	esc = g_markup_escape_text(value, -1);
	g_string_append_printf(s, "<%s>%s</%s>", name, esc, name);
	g_free(esc);
}

static void
stanza_insert_cb(GtkButton *button, GtkWidget *popover)
{
	StanzaKind kind = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(popover), "stanza-kind"));
	GtkWidget **w = g_object_get_data(G_OBJECT(popover), "stanza-widgets");
	GString *s = g_string_new(NULL);
	const char *close;
	char *id = g_strdup_printf("console%x", g_random_int());
	GtkTextBuffer *buffer;
	GtkTextIter iter;
	char *stanza;
	gsize offset;

	switch (kind) {
		case STANZA_IQ:
			g_string_append(s, "<iq");
			append_attr(s, "to", field_value(w, 0));
			append_attr(s, "id", id);
			append_attr(s, "type", field_value(w, 1));
			g_string_append(s, ">");
			close = "</iq>";
			break;
		case STANZA_PRESENCE:
			g_string_append(s, "<presence");
			append_attr(s, "to", field_value(w, 0));
			append_attr(s, "id", id);
			if (!purple_strequal(field_value(w, 1), "default"))
				append_attr(s, "type", field_value(w, 1));
			g_string_append(s, ">");
			if (!purple_strequal(field_value(w, 2), "default"))
				append_child(s, "show", field_value(w, 2));
			append_child(s, "status", field_value(w, 3));
			if (!purple_strequal(field_value(w, 4), "0"))
				append_child(s, "priority", field_value(w, 4));
			close = "</presence>";
			break;
		case STANZA_MESSAGE:
		default:
			g_string_append(s, "<message");
			append_attr(s, "to", field_value(w, 0));
			append_attr(s, "id", id);
			append_attr(s, "type", field_value(w, 1));
			g_string_append(s, ">");
			append_child(s, "body", field_value(w, 2));
			append_child(s, "subject", field_value(w, 3));
			append_child(s, "thread", field_value(w, 4));
			close = "</message>";
			break;
	}
	/* the cursor goes before the closing tag */
	offset = g_utf8_strlen(s->str, -1);
	g_string_append(s, close);
	stanza = g_string_free(s, FALSE);

	buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(console->entry));
	gtk_text_buffer_set_text(buffer, stanza, -1);
	gtk_text_buffer_get_iter_at_offset(buffer, &iter, offset);
	gtk_text_buffer_place_cursor(buffer, &iter);
	g_free(stanza);
	g_free(id);

	gtk_popover_popdown(GTK_POPOVER(popover));
	gtk_widget_grab_focus(console->entry);
}

static GtkWidget *
stanza_button(const char *label, StanzaKind kind, const StanzaField *fields)
{
	GtkWidget *button = gtk_menu_button_new();
	GtkWidget *popover = gtk_popover_new();
	GtkWidget *grid = gtk_grid_new();
	GtkWidget *insert;
	GtkWidget **widgets;
	int n, i;

	for (n = 0; fields[n].label != NULL; n++)
		;
	widgets = g_new0(GtkWidget *, n);

	gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
	for (i = 0; i < n; i++) {
		GtkWidget *l = gtk_label_new(fields[i].label);

		gtk_label_set_xalign(GTK_LABEL(l), 0);
		gtk_grid_attach(GTK_GRID(grid), l, 0, i, 1, 1);
		if (fields[i].options != NULL)
			widgets[i] = gtk_drop_down_new_from_strings(fields[i].options);
		else {
			widgets[i] = gtk_entry_new();
			gtk_entry_set_activates_default(GTK_ENTRY(widgets[i]), TRUE);
			gtk_widget_set_hexpand(widgets[i], TRUE);
			if (g_str_equal(fields[i].label, "Priority:"))
				gtk_editable_set_text(GTK_EDITABLE(widgets[i]), "0");
		}
		gtk_grid_attach(GTK_GRID(grid), widgets[i], 1, i, 1, 1);
	}
	insert = gtk_button_new_with_mnemonic(_("_Insert"));
	gtk_widget_set_halign(insert, GTK_ALIGN_END);
	gtk_grid_attach(GTK_GRID(grid), insert, 0, n, 2, 1);
	g_signal_connect(insert, "clicked", G_CALLBACK(stanza_insert_cb), popover);

	g_object_set_data(G_OBJECT(popover), "stanza-kind", GINT_TO_POINTER(kind));
	g_object_set_data_full(G_OBJECT(popover), "stanza-widgets", widgets, g_free);
	gtk_popover_set_child(GTK_POPOVER(popover), grid);
	gtk_popover_set_default_widget(GTK_POPOVER(popover), insert);

	gtk_menu_button_set_label(GTK_MENU_BUTTON(button), label);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), popover);
	return button;
}

/**************************************************************************
 * Connections
 **************************************************************************/

static void
update_hbox(void)
{
	gtk_widget_set_visible(console->hbox, console->count >= 2);
}

static void
signing_on_cb(PurpleConnection *gc)
{
	if (!console)
		return;
	if (!purple_strequal(purple_account_get_protocol_id(purple_connection_get_account(gc)),
	                     "prpl-jabber"))
		return;

	gtk_string_list_append(console->names, purple_account_get_username(gc->account));
	console->accounts = g_list_append(console->accounts, gc);
	console->count++;

	if (console->count == 1)
		console->gc = gc;
	update_hbox();
}

static void
signed_off_cb(PurpleConnection *gc)
{
	int i;

	if (!console)
		return;

	i = g_list_index(console->accounts, gc);
	if (i < 0)
		return;

	console->accounts = g_list_remove(console->accounts, gc);
	console->count--;
	if (gc == console->gc) {
		console->gc = NULL;
		append_markup(_("<span foreground='#777777'>Logged out.</span>\n"), NULL);
	}
	gtk_string_list_remove(console->names, i);
	update_hbox();
}

static void
dropdown_changed_cb(GtkDropDown *dropdown, GParamSpec *pspec, gpointer nul)
{
	guint pos;
	PurpleConnection *gc;

	if (!console)
		return;

	pos = gtk_drop_down_get_selected(dropdown);
	gc = g_list_nth_data(console->accounts, pos);
	if (gc == NULL || gc == console->gc)
		return;

	console->gc = gc;
	gtk_text_buffer_set_text(console->buffer, "", -1);
}

/**************************************************************************
 * The window
 **************************************************************************/

static void
console_destroy(GtkWidget *window, gpointer nul)
{
	if (console == NULL)
		return;
	g_list_free(console->accounts);
	g_clear_object(&console->names);
	g_free(console);
	console = NULL;
}

static void
create_console(PurplePluginAction *action)
{
	GtkWidget *vbox, *label, *toolbar;
	GtkEventController *keys;
	GtkTextBuffer *buffer;
	GList *connections;

	if (console) {
		gtk_window_present(GTK_WINDOW(console->window));
		return;
	}

	console = g_new0(XmppConsole, 1);

	console->window = gtk_window_new();
	gtk_window_set_title(GTK_WINDOW(console->window), _("XMPP Console"));
	gtk_window_set_default_size(GTK_WINDOW(console->window), 580, 400);
	if (pidgin_application_get() != NULL)
		gtk_window_set_application(GTK_WINDOW(console->window), pidgin_application_get());
	g_signal_connect(console->window, "destroy", G_CALLBACK(console_destroy), NULL);

	if (console_css == NULL) {
		console_css = gtk_css_provider_new();
		gtk_css_provider_load_from_string(console_css,
			"textview.xmpp-console-invalid, textview.xmpp-console-invalid text {"
			" background-color: alpha(#ff0000, 0.15); }");
		gtk_style_context_add_provider_for_display(gtk_widget_get_display(console->window),
			GTK_STYLE_PROVIDER(console_css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	}

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_margin_top(vbox, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_bottom(vbox, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_start(vbox, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_end(vbox, PIDGIN_HIG_BORDER);
	gtk_window_set_child(GTK_WINDOW(console->window), vbox);

	console->hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
	gtk_box_append(GTK_BOX(vbox), console->hbox);
	label = gtk_label_new(_("Account: "));
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_box_append(GTK_BOX(console->hbox), label);
	console->names = gtk_string_list_new(NULL);
	for (connections = purple_connections_get_all(); connections; connections = connections->next) {
		PurpleConnection *gc = connections->data;
		if (purple_strequal(purple_account_get_protocol_id(purple_connection_get_account(gc)), "prpl-jabber")) {
			console->count++;
			console->accounts = g_list_append(console->accounts, gc);
			gtk_string_list_append(console->names,
			                       purple_account_get_username(purple_connection_get_account(gc)));
			if (!console->gc)
				console->gc = gc;
		}
	}
	console->dropdown = gtk_drop_down_new(G_LIST_MODEL(g_object_ref(console->names)), NULL);
	gtk_widget_set_hexpand(console->dropdown, TRUE);
	gtk_box_append(GTK_BOX(console->hbox), console->dropdown);
	g_signal_connect(console->dropdown, "notify::selected", G_CALLBACK(dropdown_changed_cb), NULL);

	console->view = gtk_text_view_new();
	console->buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(console->view));
	gtk_text_view_set_editable(GTK_TEXT_VIEW(console->view), FALSE);
	gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(console->view), FALSE);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(console->view), TRUE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(console->view), GTK_WRAP_WORD_CHAR);
	gtk_text_buffer_create_tag(console->buffer, "received",
		"paragraph-background-rgba", &(GdkRGBA){ 1.0, 0.3, 0.3, 0.18 }, NULL);
	gtk_text_buffer_create_tag(console->buffer, "sent",
		"paragraph-background-rgba", &(GdkRGBA){ 0.45, 0.75, 0.25, 0.18 }, NULL);
	if (console->count == 0)
		append_markup(_("<span foreground='#777777'>Not connected to XMPP</span>\n"), NULL);
	gtk_widget_set_vexpand(console->view, TRUE);
	gtk_box_append(GTK_BOX(vbox),
		pidgin_make_scrollable(console->view, GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC, -1, -1));

	toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
	gtk_box_append(GTK_BOX(toolbar), stanza_button("<iq/>", STANZA_IQ, iq_fields));
	gtk_box_append(GTK_BOX(toolbar), stanza_button("<presence/>", STANZA_PRESENCE, presence_fields));
	gtk_box_append(GTK_BOX(toolbar), stanza_button("<message/>", STANZA_MESSAGE, message_fields));
	gtk_box_append(GTK_BOX(vbox), toolbar);

	console->entry = gtk_text_view_new();
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(console->entry), TRUE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(console->entry), GTK_WRAP_WORD_CHAR);
	keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(entry_key_cb), NULL);
	gtk_widget_add_controller(console->entry, keys);

	console->sw = pidgin_make_scrollable(console->entry, GTK_POLICY_AUTOMATIC,
	                                     GTK_POLICY_AUTOMATIC, -1, -1);
	gtk_box_append(GTK_BOX(vbox), console->sw);
	buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(console->entry));
	g_signal_connect(buffer, "changed", G_CALLBACK(entry_changed_cb), NULL);

	entry_changed_cb(buffer, NULL);

	update_hbox();
	pidgin_window_set_secondary(GTK_WINDOW(console->window));
	gtk_window_present(GTK_WINDOW(console->window));
	gtk_widget_grab_focus(console->entry);
}

/**************************************************************************
 * Plugin
 **************************************************************************/

static gboolean
plugin_load(PurplePlugin *plugin)
{
	PurplePlugin *jabber;

	jabber = purple_find_prpl("prpl-jabber");
	if (!jabber)
		return FALSE;

	xmpp_console_handle = plugin;
	purple_signal_connect(jabber, "jabber-receiving-xmlnode", xmpp_console_handle,
	                      PURPLE_CALLBACK(xmlnode_received_cb), NULL);
	purple_signal_connect(jabber, "jabber-sending-text", xmpp_console_handle,
	                      PURPLE_CALLBACK(xmlnode_sent_cb), NULL);
	purple_signal_connect(purple_connections_get_handle(), "signing-on",
	                      plugin, PURPLE_CALLBACK(signing_on_cb), NULL);
	purple_signal_connect(purple_connections_get_handle(), "signed-off",
	                      plugin, PURPLE_CALLBACK(signed_off_cb), NULL);

	return TRUE;
}

static gboolean
plugin_unload(PurplePlugin *plugin)
{
	if (console)
		gtk_window_destroy(GTK_WINDOW(console->window));
	if (console_css != NULL) {
		gtk_style_context_remove_provider_for_display(gdk_display_get_default(),
			GTK_STYLE_PROVIDER(console_css));
		g_clear_object(&console_css);
	}
	return TRUE;
}

static GList *
actions(PurplePlugin *plugin, gpointer context)
{
	GList *l = NULL;
	PurplePluginAction *act = NULL;

	act = purple_plugin_action_new(_("XMPP Console"), create_console);
	l = g_list_append(l, act);

	return l;
}

static PurplePluginInfo info =
{
	PURPLE_PLUGIN_MAGIC,
	PURPLE_MAJOR_VERSION,
	PURPLE_MINOR_VERSION,
	PURPLE_PLUGIN_STANDARD,                       /**< type           */
	PIDGIN_PLUGIN_TYPE,                           /**< ui_requirement */
	0,                                            /**< flags          */
	NULL,                                         /**< dependencies   */
	PURPLE_PRIORITY_DEFAULT,                      /**< priority       */

	"gtk-xmpp",                                   /**< id             */
	N_("XMPP Console"),                           /**< name           */
	DISPLAY_VERSION,                              /**< version        */
	                                              /**  summary        */
	N_("Send and receive raw XMPP stanzas."),
	                                              /**  description    */
	N_("This plugin is useful for debugging XMPP servers or clients."),
	"Sean Egan <seanegan@gmail.com>",             /**< author         */
	PURPLE_WEBSITE,                               /**< homepage       */

	plugin_load,                                  /**< load           */
	plugin_unload,                                /**< unload         */
	NULL,                                         /**< destroy        */

	NULL,                                         /**< ui_info        */
	NULL,                                         /**< extra_info     */
	NULL,
	actions,

	/* padding */
	NULL,
	NULL,
	NULL,
	NULL
};

static void
init_plugin(PurplePlugin *plugin)
{
}

PURPLE_INIT_PLUGIN(xmppconsole, init_plugin, info)
