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

#include "conversation.h"
#include "debug.h"
#include "prefs.h"
#include "signals.h"
#include "util.h"
#include "value.h"

#include "pidginmessageview.h"
#include "pidginrichlabel.h"

#define ACTION_KEY "pidgin-message-action"
#define DEFAULT_SCROLLBACK 4000
#define BOTTOM_SLACK 24.0

struct _PidginMessageView
{
	GtkWidget parent;

	GtkWidget *search_bar;
	GtkWidget *search_entry;
	GtkWidget *scrolled;
	GtkWidget *list;

	GListStore *store;
	GtkCustomFilter *filter;
	GtkFilterListModel *filtered;
	char *search;

	PurpleConversation *conv;
	gboolean is_chat;
	PidginNickColorScheme nick_scheme;
	char *self_id;
	guint scrollback;

	PidginMessage *marker;
	GHashTable *rows;               /* bound PidginMessageRow* */

	gboolean stick;                 /* keep the bottom in view */
	guint stick_frames;
	guint tick_id;

	/* M4b: reply/react/edit/retract are offered (the conversation's prpl
	 * implements the M8 IPC), and retract on others' messages
	 * (moderation). */
	gboolean message_actions;
	gboolean can_moderate;
};

enum {
	SIG_REACTION_TOGGLED,
	SIG_REPLY_REQUESTED,
	SIG_EDIT_REQUESTED,
	SIG_RETRACT_REQUESTED,
	SIG_POPULATE_MENU,
	SIG_TOP_REACHED,
	N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(PidginMessageView, pidgin_message_view, GTK_TYPE_WIDGET)

/**************************************************************************
 * Conversation UI signals
 **************************************************************************/

static int conv_handle;
static gboolean conv_signals_registered = FALSE;

void *
pidgin_message_view_get_conv_handle(void)
{
	return &conv_handle;
}

void
pidgin_message_view_signals_init(void)
{
	void *handle = &conv_handle;

	if (conv_signals_registered)
		return;
	conv_signals_registered = TRUE;

	/* As pidgin/gtkconv.c registers them. */
	purple_signal_register(handle, "conversation-timestamp",
#if SIZEOF_TIME_T == 4
	                     purple_marshal_POINTER__POINTER_INT_BOOLEAN,
#else
	                     purple_marshal_POINTER__POINTER_INT64_BOOLEAN,
#endif
	                     purple_value_new(PURPLE_TYPE_STRING), 3,
	                     purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_CONVERSATION),
#if SIZEOF_TIME_T == 4
	                     purple_value_new(PURPLE_TYPE_INT),
#else
	                     purple_value_new(PURPLE_TYPE_INT64),
#endif
	                     purple_value_new(PURPLE_TYPE_BOOLEAN));

	purple_signal_register(handle, "displaying-im-msg",
	                     purple_marshal_BOOLEAN__POINTER_POINTER_POINTER_POINTER_POINTER,
	                     purple_value_new(PURPLE_TYPE_BOOLEAN), 5,
	                     purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
	                     purple_value_new(PURPLE_TYPE_STRING),
	                     purple_value_new_outgoing(PURPLE_TYPE_STRING),
	                     purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_CONVERSATION),
	                     purple_value_new(PURPLE_TYPE_INT));

	purple_signal_register(handle, "displayed-im-msg",
	                     purple_marshal_VOID__POINTER_POINTER_POINTER_POINTER_UINT,
	                     NULL, 5,
	                     purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
	                     purple_value_new(PURPLE_TYPE_STRING),
	                     purple_value_new(PURPLE_TYPE_STRING),
	                     purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_CONVERSATION),
	                     purple_value_new(PURPLE_TYPE_INT));

	purple_signal_register(handle, "displaying-chat-msg",
	                     purple_marshal_BOOLEAN__POINTER_POINTER_POINTER_POINTER_POINTER,
	                     purple_value_new(PURPLE_TYPE_BOOLEAN), 5,
	                     purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
	                     purple_value_new(PURPLE_TYPE_STRING),
	                     purple_value_new_outgoing(PURPLE_TYPE_STRING),
	                     purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_CONVERSATION),
	                     purple_value_new(PURPLE_TYPE_INT));

	purple_signal_register(handle, "displayed-chat-msg",
	                     purple_marshal_VOID__POINTER_POINTER_POINTER_POINTER_UINT,
	                     NULL, 5,
	                     purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
	                     purple_value_new(PURPLE_TYPE_STRING),
	                     purple_value_new(PURPLE_TYPE_STRING),
	                     purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_CONVERSATION),
	                     purple_value_new(PURPLE_TYPE_INT));
}

void
pidgin_message_view_signals_uninit(void)
{
	if (!conv_signals_registered)
		return;
	purple_signals_disconnect_by_handle(&conv_handle);
	purple_signals_unregister_by_instance(&conv_handle);
	conv_signals_registered = FALSE;
}

char *
pidgin_message_view_format_timestamp(PurpleConversation *conv, time_t when,
                                     gboolean show_date)
{
	char *mdate = NULL;
	struct tm *tm;

	if (conv != NULL && conv_signals_registered)
		mdate = purple_signal_emit_return_1(&conv_handle, "conversation-timestamp",
		                                    conv, when, show_date);
	if (mdate != NULL)
		return mdate;

	tm = localtime(&when);
	return g_strdup_printf("(%s)", show_date ? purple_date_format_long(tm) :
	                                           purple_time_format(tm));
}

gboolean
pidgin_message_view_emit_displaying(PurpleConversation *conv, const char *who,
                                    char **message, PurpleMessageFlags flags)
{
	gboolean im;

	g_return_val_if_fail(conv != NULL, FALSE);
	g_return_val_if_fail(message != NULL, FALSE);
	if (!conv_signals_registered)
		return FALSE;

	im = purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_IM;
	return GPOINTER_TO_INT(purple_signal_emit_return_1(&conv_handle,
		im ? "displaying-im-msg" : "displaying-chat-msg",
		purple_conversation_get_account(conv), who, message, conv, flags));
}

void
pidgin_message_view_emit_displayed(PurpleConversation *conv, const char *who,
                                   const char *message, PurpleMessageFlags flags)
{
	gboolean im;

	g_return_if_fail(conv != NULL);
	if (!conv_signals_registered)
		return;

	im = purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_IM;
	purple_signal_emit(&conv_handle, im ? "displayed-im-msg" : "displayed-chat-msg",
	                   purple_conversation_get_account(conv), who, message, conv, flags);
}

/**************************************************************************
 * Shared prefs (read only: owned by Pidgin 2's /pidgin tree)
 **************************************************************************/

static gboolean
pref_bool(const char *name, gboolean def)
{
	return purple_prefs_exists(name) ? purple_prefs_get_bool(name) : def;
}

static int
pref_int(const char *name, int def)
{
	return purple_prefs_exists(name) ? purple_prefs_get_int(name) : def;
}

/**************************************************************************
 * Rows
 **************************************************************************/

#define PIDGIN_TYPE_MESSAGE_ROW (pidgin_message_row_get_type())
G_DECLARE_FINAL_TYPE(PidginMessageRow, pidgin_message_row, PIDGIN, MESSAGE_ROW, GtkWidget)

struct _PidginMessageRow
{
	GtkWidget parent;

	PidginMessageView *view;        /* weak */
	PidginMessage *msg;
	guint position;
	gulong notify_id;
	gulong reactions_id;

	GtkWidget *reply_box;
	GtkWidget *reply_label;
	GtkWidget *main_box;
	GtkWidget *time_label;
	GtkWidget *name_label;
	GtkWidget *body;
	GtkWidget *edited_button;
	GtkWidget *edited_label;
	GtkWidget *receipt_label;
	GtkWidget *reactions_box;
	GtkWidget *attachment_box;
	PidginAttachment *shown_attachment;
	GtkWidget *marker;

	GSimpleActionGroup *actions;
	GMenu *menu;
	GMenu *plugin_section;
	GMenu *link_section;
	GSimpleActionGroup *link_actions;
	GtkWidget *popover;
	GtkWidget *emoji;
	double last_x, last_y;
	gboolean click_on_body;

	GStrv extra_classes;            /* M7: classes applied from the message */
};

G_DEFINE_FINAL_TYPE(PidginMessageRow, pidgin_message_row, GTK_TYPE_WIDGET)

static void row_update(PidginMessageRow *row);

static gboolean
message_is_action(PidginMessage *msg)
{
	return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(msg), ACTION_KEY));
}

static gboolean
message_is_own(PidginMessage *msg)
{
	return (pidgin_message_get_flags(msg) & PURPLE_MESSAGE_SEND) != 0;
}

/* ---- actions ---- */

static void
copy_text(GtkWidget *widget, const char *text)
{
	gdk_clipboard_set_text(gtk_widget_get_clipboard(widget), text ? text : "");
}

static void
act_copy(GSimpleAction *a, GVariant *p, gpointer data)
{
	PidginMessageRow *row = data;

	if (row->msg)
		copy_text(GTK_WIDGET(row), pidgin_message_get_plain_text(row->msg));
}

static void
act_copy_html(GSimpleAction *a, GVariant *p, gpointer data)
{
	PidginMessageRow *row = data;

	if (row->msg)
		copy_text(GTK_WIDGET(row), pidgin_message_get_html(row->msg));
}

static void
save_image_cb(GObject *source, GAsyncResult *res, gpointer data)
{
	GdkTexture *texture = data;
	GError *error = NULL;
	GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), res, &error);

	if (file != NULL) {
		char *path = g_file_get_path(file);
		if (path == NULL || !gdk_texture_save_to_png(texture, path))
			purple_debug_error("messageview", "Could not save the image to %s\n",
			                   path ? path : "?");
		g_free(path);
		g_object_unref(file);
	} else {
		g_clear_error(&error);
	}
	g_object_unref(texture);
}

static void
act_save_image(GSimpleAction *a, GVariant *p, gpointer data)
{
	PidginMessageRow *row = data;
	GtkFileDialog *dialog;
	GdkTexture *texture;
	graphene_point_t pt, out;
	GtkRoot *root;

	graphene_point_init(&pt, row->last_x, row->last_y);
	texture = NULL;
	if (row->click_on_body &&
	    gtk_widget_compute_point(GTK_WIDGET(row), row->body, &pt, &out))
		texture = pidgin_rich_label_get_image(PIDGIN_RICH_LABEL(row->body), out.x, out.y, NULL);
	if (texture == NULL)
		texture = pidgin_rich_label_get_image(PIDGIN_RICH_LABEL(row->body), -1, -1, NULL);
	if (texture == NULL)
		return;

	dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, _("Save Image"));
	gtk_file_dialog_set_initial_name(dialog, "image.png");
	root = gtk_widget_get_root(GTK_WIDGET(row));
	gtk_file_dialog_save(dialog, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL, NULL,
	                     save_image_cb, texture);
	g_object_unref(dialog);
}

static void
act_reply(GSimpleAction *a, GVariant *p, gpointer data)
{
	PidginMessageRow *row = data;

	if (row->msg && row->view)
		g_signal_emit(row->view, signals[SIG_REPLY_REQUESTED], 0, row->msg);
}

static void
act_edit(GSimpleAction *a, GVariant *p, gpointer data)
{
	PidginMessageRow *row = data;

	if (row->msg && row->view)
		g_signal_emit(row->view, signals[SIG_EDIT_REQUESTED], 0, row->msg);
}

static void
act_retract(GSimpleAction *a, GVariant *p, gpointer data)
{
	PidginMessageRow *row = data;

	if (row->msg && row->view)
		g_signal_emit(row->view, signals[SIG_RETRACT_REQUESTED], 0, row->msg);
}

static void
emoji_picked_cb(GtkEmojiChooser *chooser, const char *emoji, PidginMessageRow *row)
{
	PidginMessageView *view = row->view;
	gboolean add;

	if (row->msg == NULL || view == NULL)
		return;
	add = !pidgin_message_has_reaction(row->msg, emoji, view->self_id ? view->self_id : "");
	g_signal_emit(view, signals[SIG_REACTION_TOGGLED], 0, row->msg, emoji, add);
}

static void
act_react(GSimpleAction *a, GVariant *p, gpointer data)
{
	PidginMessageRow *row = data;
	GdkRectangle rect = { (int)row->last_x, (int)row->last_y, 1, 1 };

	if (row->emoji == NULL) {
		row->emoji = gtk_emoji_chooser_new();
		gtk_widget_set_parent(row->emoji, GTK_WIDGET(row));
		g_signal_connect(row->emoji, "emoji-picked", G_CALLBACK(emoji_picked_cb), row);
	}
	gtk_popover_set_pointing_to(GTK_POPOVER(row->emoji), &rect);
	gtk_popover_popup(GTK_POPOVER(row->emoji));
}

static const GActionEntry row_actions[] = {
	{ "copy", act_copy, NULL, NULL, NULL, { 0 } },
	{ "copy-html", act_copy_html, NULL, NULL, NULL, { 0 } },
	{ "save-image", act_save_image, NULL, NULL, NULL, { 0 } },
	{ "reply", act_reply, NULL, NULL, NULL, { 0 } },
	{ "react", act_react, NULL, NULL, NULL, { 0 } },
	{ "edit", act_edit, NULL, NULL, NULL, { 0 } },
	{ "retract", act_retract, NULL, NULL, NULL, { 0 } },
};

static void
set_action_enabled(PidginMessageRow *row, const char *name, gboolean enabled)
{
	GAction *action = g_action_map_lookup_action(G_ACTION_MAP(row->actions), name);

	g_simple_action_set_enabled(G_SIMPLE_ACTION(action), enabled);
}

static GMenu *
build_menu(PidginMessageRow *row)
{
	GMenu *menu = g_menu_new();
	GMenu *section;

	row->link_section = g_menu_new();
	g_menu_append_section(menu, NULL, G_MENU_MODEL(row->link_section));

	section = g_menu_new();
	g_menu_append(section, _("_Reply"), "msg.reply");
	g_menu_append(section, _("Re_act…"), "msg.react");
	g_menu_append(section, _("_Edit"), "msg.edit");
	g_menu_append(section, _("_Delete for Everyone"), "msg.retract");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);

	section = g_menu_new();
	g_menu_append(section, _("Copy _Message"), "msg.copy");
	g_menu_append(section, _("Copy as _HTML"), "msg.copy-html");
	g_menu_append(section, _("_Save Image…"), "msg.save-image");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);

	row->plugin_section = g_menu_new();
	g_menu_append_section(menu, NULL, G_MENU_MODEL(row->plugin_section));

	return menu;
}

/* ---- context menu ---- */

static void
update_link_section(PidginMessageRow *row, double x, double y)
{
	const char *uri = NULL;
	graphene_point_t pt, out;

	g_menu_remove_all(row->link_section);
	g_clear_object(&row->link_actions);
	gtk_widget_insert_action_group(GTK_WIDGET(row), "link", NULL);

	graphene_point_init(&pt, x, y);
	if (gtk_widget_compute_point(GTK_WIDGET(row), row->body, &pt, &out))
		uri = pidgin_rich_label_get_link_at(PIDGIN_RICH_LABEL(row->body), out.x, out.y);
	if (uri == NULL)
		return;

	row->link_actions = g_simple_action_group_new();
	pidgin_markup_populate_link_menu(row->body, uri, row->link_section,
	                                 G_ACTION_MAP(row->link_actions));
	gtk_widget_insert_action_group(GTK_WIDGET(row), "link",
	                               G_ACTION_GROUP(row->link_actions));
}

/* Capture phase: remember where the click was and prepare the dynamic
 * menu parts before the body's own context menu opens. */
static void
row_capture_pressed_cb(GtkGestureClick *gesture, int n, double x, double y,
                       PidginMessageRow *row)
{
	GtkWidget *picked = gtk_widget_pick(GTK_WIDGET(row), x, y, GTK_PICK_DEFAULT);

	row->last_x = x;
	row->last_y = y;
	row->click_on_body = picked != NULL &&
	                     (picked == row->body || gtk_widget_is_ancestor(picked, row->body));
	update_link_section(row, x, y);
}

/* Bubble phase: a right click that the body didn't handle (on the
 * timestamp, the name or the margins) opens the row menu. */
static void
row_pressed_cb(GtkGestureClick *gesture, int n, double x, double y, PidginMessageRow *row)
{
	GdkRectangle rect = { (int)x, (int)y, 1, 1 };

	if (row->msg == NULL || pidgin_message_get_kind(row->msg) != PIDGIN_MESSAGE_KIND_NORMAL)
		return;
	if (row->click_on_body)
		return;

	if (row->popover == NULL) {
		row->popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(row->menu));
		gtk_widget_set_parent(row->popover, GTK_WIDGET(row));
		gtk_popover_set_has_arrow(GTK_POPOVER(row->popover), FALSE);
		gtk_widget_set_halign(row->popover, GTK_ALIGN_START);
	}
	gtk_popover_set_pointing_to(GTK_POPOVER(row->popover), &rect);
	gtk_popover_popup(GTK_POPOVER(row->popover));
	gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
reply_clicked_cb(GtkGestureClick *gesture, int n, double x, double y,
                 PidginMessageRow *row)
{
	PidginMessage *target;

	if (row->msg == NULL || row->view == NULL)
		return;
	target = pidgin_message_view_find_by_id(row->view, pidgin_message_get_reply_to(row->msg));
	if (target != NULL)
		pidgin_message_view_scroll_to_message(row->view, target);
}

/* ---- reactions ---- */

static void
reaction_toggled_cb(GtkToggleButton *button, PidginMessageRow *row)
{
	const char *emoji = g_object_get_data(G_OBJECT(button), "emoji");
	PidginMessageView *view = row->view;
	gboolean add;

	if (row->msg == NULL || view == NULL)
		return;
	add = !pidgin_message_has_reaction(row->msg, emoji, view->self_id ? view->self_id : "");

	/* The view shows the model; the caller updates it once sent. */
	g_signal_handlers_block_by_func(button, reaction_toggled_cb, row);
	gtk_toggle_button_set_active(button, !add);
	g_signal_handlers_unblock_by_func(button, reaction_toggled_cb, row);

	g_signal_emit(view, signals[SIG_REACTION_TOGGLED], 0, row->msg, emoji, add);
}

static void
update_reactions(PidginMessageRow *row)
{
	GtkWidget *child;
	GHashTable *reactions = pidgin_message_get_reactions(row->msg);
	GList *emojis, *l;
	const char *self = row->view && row->view->self_id ? row->view->self_id : "";

	while ((child = gtk_widget_get_first_child(row->reactions_box)) != NULL)
		gtk_box_remove(GTK_BOX(row->reactions_box), child);

	emojis = pidgin_message_get_reaction_emojis(row->msg);
	for (l = emojis; l; l = l->next) {
		GList *senders = g_hash_table_lookup(reactions, l->data), *s;
		GString *tip = g_string_new(NULL);
		char *text = g_strdup_printf("%s %u", (const char *)l->data, g_list_length(senders));
		GtkWidget *button = gtk_toggle_button_new_with_label(text);

		for (s = senders; s; s = s->next)
			g_string_append_printf(tip, "%s%s", tip->len ? ", " : "", (const char *)s->data);
		gtk_widget_set_tooltip_text(button, tip->str);
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button),
			g_list_find_custom(senders, self, (GCompareFunc)g_strcmp0) != NULL);
		g_object_set_data_full(G_OBJECT(button), "emoji", g_strdup(l->data), g_free);
		g_signal_connect(button, "toggled", G_CALLBACK(reaction_toggled_cb), row);
		gtk_box_append(GTK_BOX(row->reactions_box), button);
		g_string_free(tip, TRUE);
		g_free(text);
	}
	gtk_widget_set_visible(row->reactions_box, emojis != NULL &&
	                       !pidgin_message_get_retracted(row->msg));
	g_list_free(emojis);
}

/* ---- name, timestamp ---- */

static void
get_background(GtkWidget *widget, GdkRGBA *bg)
{
	GdkRGBA fg;

	/* GTK 4 has no background query; infer it from the text colour. */
	gtk_widget_get_color(widget, &fg);
	if (0.3 * fg.red + 0.59 * fg.green + 0.11 * fg.blue > 0.5)
		*bg = (GdkRGBA){ 0.14, 0.14, 0.14, 1 };
	else
		*bg = (GdkRGBA){ 1, 1, 1, 1 };
}

static void
update_name(PidginMessageRow *row)
{
	PidginMessage *msg = row->msg;
	PurpleMessageFlags flags = pidgin_message_get_flags(msg);
	GtkWidget *name = row->name_label;
	const char *alias = pidgin_message_get_alias(msg);
	static const char *classes[] = { "send", "receive", "highlight", "action",
		"whisper", "error", "system", "auto-resp", NULL };
	char *text = NULL;
	int i;

	for (i = 0; classes[i]; i++)
		gtk_widget_remove_css_class(name, classes[i]);
	gtk_widget_remove_css_class(GTK_WIDGET(row), "error");
	gtk_widget_remove_css_class(GTK_WIDGET(row), "system");

	if (flags & (PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_ERROR | PURPLE_MESSAGE_RAW) ||
	    alias == NULL || *alias == '\0') {
		gtk_widget_set_visible(name, FALSE);
		if (flags & PURPLE_MESSAGE_ERROR)
			gtk_widget_add_css_class(GTK_WIDGET(row), "error");
		else if (flags & PURPLE_MESSAGE_SYSTEM)
			gtk_widget_add_css_class(GTK_WIDGET(row), "system");
		return;
	}

	gtk_widget_set_visible(name, TRUE);
	if (message_is_action(msg)) {
		text = g_strdup_printf("***%s", alias);
		gtk_widget_add_css_class(name, "action");
	} else if (flags & PURPLE_MESSAGE_AUTO_RESP) {
		text = g_strdup_printf(_("%s <AUTO-REPLY>:"), alias);
		gtk_widget_add_css_class(name, "auto-resp");
	} else {
		text = g_strdup_printf("%s:", alias);
	}

	if (flags & PURPLE_MESSAGE_WHISPER)
		gtk_widget_add_css_class(name, "whisper");
	else if (flags & PURPLE_MESSAGE_SEND)
		gtk_widget_add_css_class(name, "send");
	else if (flags & PURPLE_MESSAGE_RECV)
		gtk_widget_add_css_class(name, "receive");
	if (flags & PURPLE_MESSAGE_NICK)
		gtk_widget_add_css_class(name, "highlight");

	gtk_label_set_text(GTK_LABEL(name), text);

	/* Chat nick colours, like Pidgin 2 (not for us, highlights, actions) */
	if (row->view && row->view->is_chat && (flags & PURPLE_MESSAGE_RECV) &&
	    !(flags & PURPLE_MESSAGE_NICK) && !message_is_action(msg)) {
		GdkRGBA bg, color;
		PangoAttrList *attrs = pango_attr_list_new();
		const char *key = pidgin_message_get_sender(msg);

		get_background(name, &bg);
		pidgin_nick_color_get(row->view->nick_scheme, key ? key : alias, &bg, &color);
		pango_attr_list_insert(attrs, pango_attr_foreground_new(color.red * 65535,
		                       color.green * 65535, color.blue * 65535));
		gtk_label_set_attributes(GTK_LABEL(name), attrs);
		pango_attr_list_unref(attrs);
	} else {
		gtk_label_set_attributes(GTK_LABEL(name), NULL);
	}
	g_free(text);
}

static gboolean
show_date_for(PidginMessageRow *row)
{
	time_t when = pidgin_message_get_time(row->msg);
	GListModel *model;
	PidginMessage *prev = NULL;
	gboolean show;

	/* As Pidgin 2: the date on the first message of a day, and on
	 * anything older than 20 minutes. */
	if (time(NULL) > when + 20 * 60)
		return TRUE;
	if (row->view == NULL || row->position == 0)
		return FALSE;

	model = G_LIST_MODEL(row->view->filtered);
	prev = g_list_model_get_item(model, row->position - 1);
	if (prev == NULL)
		return FALSE;
	{
		time_t pt = pidgin_message_get_time(prev);
		struct tm a = *localtime(&pt), b = *localtime(&when);
		show = a.tm_yday != b.tm_yday || a.tm_year != b.tm_year;
	}
	g_object_unref(prev);
	return show;
}

static void
update_timestamp(PidginMessageRow *row)
{
	char *ts;

	if (!pref_bool(PIDGIN_PREFS_ROOT "/conversations/show_timestamps", TRUE)) {
		gtk_widget_set_visible(row->time_label, FALSE);
		return;
	}
	ts = pidgin_message_view_format_timestamp(row->view ? row->view->conv : NULL,
	                                          pidgin_message_get_time(row->msg),
	                                          show_date_for(row));
	gtk_label_set_text(GTK_LABEL(row->time_label), ts);
	gtk_widget_set_visible(row->time_label, ts != NULL && *ts != '\0');
	g_free(ts);
}

/* ---- the rest ---- */

static void
update_reply(PidginMessageRow *row)
{
	const char *reply_to = pidgin_message_get_reply_to(row->msg);
	const char *sender = pidgin_message_get_reply_to_sender(row->msg);
	const char *preview = pidgin_message_get_reply_preview(row->msg);
	PidginMessage *target = NULL;
	char *snippet, *text;

	if (reply_to == NULL || pidgin_message_get_retracted(row->msg)) {
		gtk_widget_set_visible(row->reply_box, FALSE);
		return;
	}
	if (row->view != NULL)
		target = pidgin_message_view_find_by_id(row->view, reply_to);
	if (target != NULL) {
		preview = pidgin_message_get_plain_text(target);
		if (sender == NULL)
			sender = pidgin_message_get_alias(target);
	}
	if (preview == NULL)
		preview = _("(message not loaded)");

	snippet = g_utf8_substring(preview, 0, 80);
	g_strdelimit(snippet, "\n", ' ');
	text = g_strdup_printf("\xe2\x86\xa9 %s%s%s%s", sender ? sender : "", sender ? ": " : "",
	                       snippet, g_utf8_strlen(preview, -1) > 80 ? "\xe2\x80\xa6" : "");
	gtk_label_set_text(GTK_LABEL(row->reply_label), text);
	gtk_widget_set_visible(row->reply_box, TRUE);
	g_free(text);
	g_free(snippet);
}

static void
update_edited(PidginMessageRow *row)
{
	GPtrArray *history = pidgin_message_get_history(row->msg);
	GString *orig;
	guint i;

	if (!pidgin_message_get_edited(row->msg) || pidgin_message_get_retracted(row->msg)) {
		gtk_widget_set_visible(row->edited_button, FALSE);
		return;
	}
	orig = g_string_new(NULL);
	for (i = 0; history && i < history->len; i++) {
		char *plain = pidgin_markup_html_to_plain(g_ptr_array_index(history, i));
		if (orig->len)
			g_string_append(orig, "\n\xe2\x80\x94\n");
		g_string_append(orig, plain);
		g_free(plain);
	}
	gtk_label_set_text(GTK_LABEL(row->edited_label), orig->str);
	gtk_widget_set_visible(row->edited_button, TRUE);
	g_string_free(orig, TRUE);
}

static void
update_receipt(PidginMessageRow *row)
{
	PidginReceiptState state = pidgin_message_get_receipt(row->msg);
	GtkWidget *l = row->receipt_label;

	gtk_widget_remove_css_class(l, "sent");
	gtk_widget_remove_css_class(l, "delivered");
	gtk_widget_remove_css_class(l, "displayed");
	if (!message_is_own(row->msg) || state == PIDGIN_RECEIPT_NONE ||
	    pidgin_message_get_retracted(row->msg)) {
		gtk_widget_set_visible(l, FALSE);
		return;
	}
	switch (state) {
		case PIDGIN_RECEIPT_SENT:
			gtk_label_set_text(GTK_LABEL(l), "\xe2\x9c\x93");
			gtk_widget_set_tooltip_text(l, _("Sent"));
			gtk_widget_add_css_class(l, "sent");
			break;
		case PIDGIN_RECEIPT_DELIVERED:
			gtk_label_set_text(GTK_LABEL(l), "\xe2\x9c\x93\xe2\x9c\x93");
			gtk_widget_set_tooltip_text(l, _("Delivered"));
			gtk_widget_add_css_class(l, "delivered");
			break;
		default:
			gtk_label_set_text(GTK_LABEL(l), "\xe2\x9c\x93\xe2\x9c\x93");
			gtk_widget_set_tooltip_text(l, _("Read"));
			gtk_widget_add_css_class(l, "displayed");
			break;
	}
	gtk_widget_set_visible(l, TRUE);
}

static void
update_body(PidginMessageRow *row)
{
	PidginRichLabel *body = PIDGIN_RICH_LABEL(row->body);

	if (pidgin_message_get_retracted(row->msg)) {
		gtk_widget_add_css_class(GTK_WIDGET(row), "retracted");
		pidgin_rich_label_set_text(body, _("This message was deleted."));
		pidgin_rich_label_set_highlight(body, NULL);
		return;
	}
	gtk_widget_remove_css_class(GTK_WIDGET(row), "retracted");
	pidgin_rich_label_set_result(body, pidgin_message_get_markup(row->msg));
	pidgin_rich_label_set_highlight(body, row->view ? row->view->search : NULL);
}

static void
update_menu(PidginMessageRow *row)
{
	gboolean own = message_is_own(row->msg);
	gboolean retracted = pidgin_message_get_retracted(row->msg);
	gboolean image = pidgin_markup_result_has_object(pidgin_message_get_markup(row->msg),
	                                                 PIDGIN_MARKUP_OBJECT_IMAGE) ||
	                 pidgin_markup_result_has_object(pidgin_message_get_markup(row->msg),
	                                                 PIDGIN_MARKUP_OBJECT_REMOTE_IMAGE);

	gboolean meta = row->view == NULL || row->view->message_actions;
	gboolean has_id = pidgin_message_get_stanza_id(row->msg) != NULL ||
	                  pidgin_message_get_origin_id(row->msg) != NULL ||
	                  pidgin_message_get_server_id(row->msg) != NULL;
	gboolean moderate = row->view != NULL && row->view->can_moderate &&
	                    pidgin_message_get_server_id(row->msg) != NULL;

	/* Without the M8 actions (or ids to address the message by) only
	 * the demo offers them, for its own tests. */
	if (row->view != NULL && row->view->message_actions && !has_id)
		meta = FALSE;
	set_action_enabled(row, "edit", meta && own && !retracted);
	set_action_enabled(row, "retract", meta && (own || moderate) && !retracted);
	set_action_enabled(row, "react", meta && !retracted);
	set_action_enabled(row, "reply", meta && !retracted);
	set_action_enabled(row, "save-image", image && !retracted);

	g_menu_remove_all(row->plugin_section);
	if (row->view != NULL)
		g_signal_emit(row->view, signals[SIG_POPULATE_MENU], 0, row->msg,
		              row->plugin_section);
}

/*
 * M7: row-level classes for styling plugins (convcolors): the message
 * type as msg-send, msg-recv, msg-nick, msg-system, msg-error,
 * msg-whisper, msg-auto-resp, msg-delayed, plus the message's own extra
 * classes (pidgin_message_add_css_class(), e.g. "history").
 */
static void
update_row_classes(PidginMessageRow *row)
{
	static const struct {
		PurpleMessageFlags flag;
		const char *name;
	} types[] = {
		{ PURPLE_MESSAGE_SEND, "msg-send" },
		{ PURPLE_MESSAGE_RECV, "msg-recv" },
		{ PURPLE_MESSAGE_NICK, "msg-nick" },
		{ PURPLE_MESSAGE_SYSTEM, "msg-system" },
		{ PURPLE_MESSAGE_ERROR, "msg-error" },
		{ PURPLE_MESSAGE_WHISPER, "msg-whisper" },
		{ PURPLE_MESSAGE_AUTO_RESP, "msg-auto-resp" },
		{ PURPLE_MESSAGE_DELAYED, "msg-delayed" },
	};
	PurpleMessageFlags flags = row->msg ? pidgin_message_get_flags(row->msg) : 0;
	const char * const *extra = row->msg ? pidgin_message_get_css_classes(row->msg) : NULL;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(types); i++) {
		if (flags & types[i].flag)
			gtk_widget_add_css_class(GTK_WIDGET(row), types[i].name);
		else
			gtk_widget_remove_css_class(GTK_WIDGET(row), types[i].name);
	}

	for (i = 0; row->extra_classes && row->extra_classes[i]; i++)
		gtk_widget_remove_css_class(GTK_WIDGET(row), row->extra_classes[i]);
	g_clear_pointer(&row->extra_classes, g_strfreev);
	if (extra != NULL) {
		row->extra_classes = g_strdupv((char **)extra);
		for (i = 0; extra[i]; i++)
			gtk_widget_add_css_class(GTK_WIDGET(row), extra[i]);
	}
}

/* The message's attachment under its text (rebuilt only when it changes). */
static void
update_attachment(PidginMessageRow *row)
{
	PidginAttachment *att = pidgin_message_get_attachment(row->msg);
	GtkWidget *child;

	if (att == row->shown_attachment)
		return;
	while ((child = gtk_widget_get_first_child(row->attachment_box)) != NULL)
		gtk_box_remove(GTK_BOX(row->attachment_box), child);
	g_set_object(&row->shown_attachment, att);
	if (att != NULL)
		gtk_box_append(GTK_BOX(row->attachment_box), pidgin_attachment_widget_new(att));
	gtk_widget_set_visible(row->attachment_box, att != NULL);
}

static void
row_update(PidginMessageRow *row)
{
	gboolean marker;

	if (row->msg == NULL)
		return;

	marker = pidgin_message_get_kind(row->msg) == PIDGIN_MESSAGE_KIND_MARKER;
	update_row_classes(row);
	gtk_widget_set_visible(row->marker, marker);
	gtk_widget_set_visible(row->main_box, !marker);
	if (marker) {
		gtk_widget_set_visible(row->reply_box, FALSE);
		gtk_widget_set_visible(row->reactions_box, FALSE);
		gtk_widget_set_visible(row->attachment_box, FALSE);
		return;
	}

	update_timestamp(row);
	update_name(row);
	update_body(row);
	update_edited(row);
	update_receipt(row);
	update_reply(row);
	update_reactions(row);
	update_attachment(row);
	update_menu(row);
}

static void
msg_notify_cb(GObject *obj, GParamSpec *pspec, PidginMessageRow *row)
{
	row_update(row);
}

static void
msg_reactions_cb(PidginMessage *msg, PidginMessageRow *row)
{
	update_reactions(row);
}

static void
row_unbind(PidginMessageRow *row)
{
	if (row->msg == NULL)
		return;
	g_clear_signal_handler(&row->notify_id, row->msg);
	g_clear_signal_handler(&row->reactions_id, row->msg);
	g_clear_object(&row->msg);
}

static void
row_bind(PidginMessageRow *row, PidginMessage *msg, guint position)
{
	row_unbind(row);
	row->msg = g_object_ref(msg);
	row->position = position;
	row->notify_id = g_signal_connect(msg, "notify", G_CALLBACK(msg_notify_cb), row);
	row->reactions_id = g_signal_connect(msg, "reactions-changed",
	                                     G_CALLBACK(msg_reactions_cb), row);
	row_update(row);
}

static void
pidgin_message_row_dispose(GObject *obj)
{
	PidginMessageRow *row = PIDGIN_MESSAGE_ROW(obj);
	GtkWidget *child;

	row_unbind(row);
	g_clear_pointer(&row->popover, gtk_widget_unparent);
	g_clear_pointer(&row->emoji, gtk_widget_unparent);
	while ((child = gtk_widget_get_first_child(GTK_WIDGET(row))) != NULL)
		gtk_widget_unparent(child);
	g_clear_object(&row->actions);
	g_clear_object(&row->link_actions);
	g_clear_object(&row->menu);
	g_clear_object(&row->plugin_section);
	g_clear_object(&row->link_section);
	g_clear_pointer(&row->extra_classes, g_strfreev);
	g_clear_object(&row->shown_attachment);
	g_clear_weak_pointer(&row->view);

	G_OBJECT_CLASS(pidgin_message_row_parent_class)->dispose(obj);
}

static void
pidgin_message_row_class_init(PidginMessageRowClass *klass)
{
	G_OBJECT_CLASS(klass)->dispose = pidgin_message_row_dispose;
	gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(klass), GTK_TYPE_BOX_LAYOUT);
	gtk_widget_class_set_css_name(GTK_WIDGET_CLASS(klass), "pidgin-message-row");
}

static void
pidgin_message_row_init(PidginMessageRow *row)
{
	GtkLayoutManager *layout = gtk_widget_get_layout_manager(GTK_WIDGET(row));
	GtkWidget *popover;
	GtkGesture *gesture;

	gtk_orientable_set_orientation(GTK_ORIENTABLE(layout), GTK_ORIENTATION_VERTICAL);
	gtk_box_layout_set_spacing(GTK_BOX_LAYOUT(layout), 2);
	gtk_widget_add_css_class(GTK_WIDGET(row), "pidgin-message-row");

	/* reply quote */
	row->reply_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(row->reply_box, "reply-quote");
	row->reply_label = gtk_label_new(NULL);
	gtk_label_set_ellipsize(GTK_LABEL(row->reply_label), PANGO_ELLIPSIZE_END);
	gtk_label_set_xalign(GTK_LABEL(row->reply_label), 0.0);
	gtk_widget_set_hexpand(row->reply_label, TRUE);
	gtk_box_append(GTK_BOX(row->reply_box), row->reply_label);
	gesture = gtk_gesture_click_new();
	g_signal_connect(gesture, "released", G_CALLBACK(reply_clicked_cb), row);
	gtk_widget_add_controller(row->reply_box, GTK_EVENT_CONTROLLER(gesture));
	gtk_widget_set_cursor_from_name(row->reply_box, "pointer");
	gtk_widget_set_parent(row->reply_box, GTK_WIDGET(row));

	/* time, name, body, decorations */
	row->main_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	row->time_label = gtk_label_new(NULL);
	gtk_widget_add_css_class(row->time_label, "timestamp");
	gtk_widget_set_valign(row->time_label, GTK_ALIGN_START);
	gtk_box_append(GTK_BOX(row->main_box), row->time_label);
	row->name_label = gtk_label_new(NULL);
	gtk_widget_add_css_class(row->name_label, "name");
	gtk_widget_set_valign(row->name_label, GTK_ALIGN_START);
	gtk_label_set_selectable(GTK_LABEL(row->name_label), FALSE);
	gtk_box_append(GTK_BOX(row->main_box), row->name_label);
	row->body = pidgin_rich_label_new();
	gtk_widget_add_css_class(row->body, "body");
	gtk_widget_set_hexpand(row->body, TRUE);
	gtk_box_append(GTK_BOX(row->main_box), row->body);

	row->edited_label = gtk_label_new(NULL);
	gtk_label_set_wrap(GTK_LABEL(row->edited_label), TRUE);
	gtk_label_set_selectable(GTK_LABEL(row->edited_label), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(row->edited_label), 60);
	popover = gtk_popover_new();
	gtk_popover_set_child(GTK_POPOVER(popover), row->edited_label);
	row->edited_button = gtk_menu_button_new();
	/* a child, not a label: labelled menu buttons always show an arrow */
	gtk_menu_button_set_child(GTK_MENU_BUTTON(row->edited_button), gtk_label_new(_("(edited)")));
	gtk_menu_button_set_always_show_arrow(GTK_MENU_BUTTON(row->edited_button), FALSE);
	gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(row->edited_button), FALSE);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(row->edited_button), popover);
	gtk_widget_set_tooltip_text(row->edited_button, _("Show the original message"));
	gtk_widget_add_css_class(row->edited_button, "edited");
	gtk_widget_set_valign(row->edited_button, GTK_ALIGN_START);
	gtk_box_append(GTK_BOX(row->main_box), row->edited_button);

	row->receipt_label = gtk_label_new(NULL);
	gtk_widget_add_css_class(row->receipt_label, "receipt");
	gtk_widget_set_valign(row->receipt_label, GTK_ALIGN_START);
	gtk_box_append(GTK_BOX(row->main_box), row->receipt_label);
	gtk_widget_set_parent(row->main_box, GTK_WIDGET(row));

	/* an attachment (a received image, ...) */
	row->attachment_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(row->attachment_box, "attachment");
	gtk_widget_set_visible(row->attachment_box, FALSE);
	gtk_widget_set_parent(row->attachment_box, GTK_WIDGET(row));

	/* reactions */
	row->reactions_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_widget_add_css_class(row->reactions_box, "reactions");
	gtk_widget_set_parent(row->reactions_box, GTK_WIDGET(row));

	/* marker */
	row->marker = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_widget_add_css_class(row->marker, "pidgin-marker-line");
	gtk_widget_set_parent(row->marker, GTK_WIDGET(row));

	/* actions and menus */
	row->actions = g_simple_action_group_new();
	g_action_map_add_action_entries(G_ACTION_MAP(row->actions), row_actions,
	                                G_N_ELEMENTS(row_actions), row);
	gtk_widget_insert_action_group(GTK_WIDGET(row), "msg", G_ACTION_GROUP(row->actions));
	row->menu = build_menu(row);
	pidgin_rich_label_set_extra_menu(PIDGIN_RICH_LABEL(row->body), G_MENU_MODEL(row->menu));

	gesture = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), GDK_BUTTON_SECONDARY);
	gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(gesture), GTK_PHASE_CAPTURE);
	g_signal_connect(gesture, "pressed", G_CALLBACK(row_capture_pressed_cb), row);
	gtk_widget_add_controller(GTK_WIDGET(row), GTK_EVENT_CONTROLLER(gesture));

	gesture = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), GDK_BUTTON_SECONDARY);
	g_signal_connect(gesture, "pressed", G_CALLBACK(row_pressed_cb), row);
	gtk_widget_add_controller(GTK_WIDGET(row), GTK_EVENT_CONTROLLER(gesture));
}

static GtkWidget *
message_row_new(PidginMessageView *view)
{
	PidginMessageRow *row = g_object_new(PIDGIN_TYPE_MESSAGE_ROW, NULL);

	g_set_weak_pointer(&row->view, view);
	return GTK_WIDGET(row);
}

/**************************************************************************
 * List factory
 **************************************************************************/

static void
factory_setup_cb(GtkSignalListItemFactory *factory, GtkListItem *item,
                 PidginMessageView *view)
{
	gtk_list_item_set_activatable(item, FALSE);
	gtk_list_item_set_selectable(item, FALSE);
	gtk_list_item_set_child(item, message_row_new(view));
}

static void
factory_bind_cb(GtkSignalListItemFactory *factory, GtkListItem *item,
                PidginMessageView *view)
{
	PidginMessageRow *row = PIDGIN_MESSAGE_ROW(gtk_list_item_get_child(item));

	row_bind(row, gtk_list_item_get_item(item), gtk_list_item_get_position(item));
	g_hash_table_add(view->rows, row);
}

static void
factory_unbind_cb(GtkSignalListItemFactory *factory, GtkListItem *item,
                  PidginMessageView *view)
{
	PidginMessageRow *row = PIDGIN_MESSAGE_ROW(gtk_list_item_get_child(item));

	g_hash_table_remove(view->rows, row);
	row_unbind(row);
}

/**************************************************************************
 * Scrolling
 **************************************************************************/

static GtkAdjustment *
vadj(PidginMessageView *view)
{
	return gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(view->scrolled));
}

gboolean
pidgin_message_view_is_at_bottom(PidginMessageView *view)
{
	GtkAdjustment *adj;

	g_return_val_if_fail(PIDGIN_IS_MESSAGE_VIEW(view), TRUE);

	if (view->tick_id != 0)
		return TRUE;            /* still scrolling down */
	adj = vadj(view);
	return gtk_adjustment_get_value(adj) + gtk_adjustment_get_page_size(adj) >=
	       gtk_adjustment_get_upper(adj) - BOTTOM_SLACK;
}

/* Row heights are estimated until rows are laid out, so going to the
 * bottom takes a few frames. */
static gboolean
stick_tick_cb(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
	PidginMessageView *view = PIDGIN_MESSAGE_VIEW(widget);
	GtkAdjustment *adj = vadj(view);
	guint n = g_list_model_get_n_items(G_LIST_MODEL(view->filtered));

	if (n > 0 && view->stick_frames == 3)
		gtk_list_view_scroll_to(GTK_LIST_VIEW(view->list), n - 1, GTK_LIST_SCROLL_NONE, NULL);
	gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) -
	                         gtk_adjustment_get_page_size(adj));
	if (--view->stick_frames == 0) {
		view->tick_id = 0;
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

void
pidgin_message_view_scroll_to_bottom(PidginMessageView *view)
{
	g_return_if_fail(PIDGIN_IS_MESSAGE_VIEW(view));

	view->stick_frames = 3;
	if (view->tick_id == 0)
		view->tick_id = gtk_widget_add_tick_callback(GTK_WIDGET(view), stick_tick_cb,
		                                             NULL, NULL);
}

void
pidgin_message_view_scroll_to_message(PidginMessageView *view, PidginMessage *msg)
{
	guint n, i;

	g_return_if_fail(PIDGIN_IS_MESSAGE_VIEW(view));

	n = g_list_model_get_n_items(G_LIST_MODEL(view->filtered));
	for (i = 0; i < n; i++) {
		PidginMessage *m = g_list_model_get_item(G_LIST_MODEL(view->filtered), i);
		g_object_unref(m);
		if (m == msg) {
			if (view->tick_id) {
				gtk_widget_remove_tick_callback(GTK_WIDGET(view), view->tick_id);
				view->tick_id = 0;
			}
			gtk_list_view_scroll_to(GTK_LIST_VIEW(view->list), i,
			                        GTK_LIST_SCROLL_NONE, NULL);
			return;
		}
	}
}

/**************************************************************************
 * Messages
 **************************************************************************/

static void
trim_scrollback(PidginMessageView *view)
{
	guint n = g_list_model_get_n_items(G_LIST_MODEL(view->store));

	if (view->scrollback == 0 || n <= view->scrollback)
		return;
	g_list_store_splice(view->store, 0, n - view->scrollback, NULL, 0);
}

/* "/me does" -> an action, as pidgin_conv_write_conv() did. */
static void
detect_action(PidginMessage *msg)
{
	PurpleMessageFlags flags = pidgin_message_get_flags(msg);
	char *html;

	if (!(flags & (PURPLE_MESSAGE_SEND | PURPLE_MESSAGE_RECV)) ||
	    pidgin_message_get_kind(msg) != PIDGIN_MESSAGE_KIND_NORMAL ||
	    message_is_action(msg))
		return;

	html = g_strdup(pidgin_message_get_html(msg));
	if (purple_message_meify(html, -1)) {
		g_object_set_data(G_OBJECT(msg), ACTION_KEY, GINT_TO_POINTER(1));
		pidgin_message_set_html(msg, html);
	}
	g_free(html);
}

void
pidgin_message_view_append(PidginMessageView *view, PidginMessage *msg)
{
	gboolean stick;

	g_return_if_fail(PIDGIN_IS_MESSAGE_VIEW(view));
	g_return_if_fail(PIDGIN_IS_MESSAGE(msg));

	stick = pidgin_message_view_is_at_bottom(view);
	detect_action(msg);
	g_list_store_append(view->store, msg);
	trim_scrollback(view);
	if (stick)
		pidgin_message_view_scroll_to_bottom(view);
}

void
pidgin_message_view_prepend_many(PidginMessageView *view, GPtrArray *messages)
{
	guint i;

	g_return_if_fail(PIDGIN_IS_MESSAGE_VIEW(view));
	g_return_if_fail(messages != NULL);

	for (i = 0; i < messages->len; i++)
		detect_action(g_ptr_array_index(messages, i));
	/* GtkListView keeps its anchor item in place when items are
	 * inserted before it, so what is on screen stays put. */
	g_list_store_splice(view->store, 0, 0, messages->pdata, messages->len);
}

void
pidgin_message_view_prepend(PidginMessageView *view, PidginMessage *msg)
{
	GPtrArray *one = g_ptr_array_new();

	g_ptr_array_add(one, msg);
	pidgin_message_view_prepend_many(view, one);
	g_ptr_array_free(one, TRUE);
}

void
pidgin_message_view_clear(PidginMessageView *view)
{
	g_return_if_fail(PIDGIN_IS_MESSAGE_VIEW(view));
	g_list_store_remove_all(view->store);
	g_clear_object(&view->marker);
}

PidginMessage *
pidgin_message_view_find_by_id(PidginMessageView *view, const char *id)
{
	guint n;

	g_return_val_if_fail(PIDGIN_IS_MESSAGE_VIEW(view), NULL);

	if (id == NULL || *id == '\0')
		return NULL;
	n = g_list_model_get_n_items(G_LIST_MODEL(view->store));
	while (n-- > 0) {
		PidginMessage *msg = g_list_model_get_item(G_LIST_MODEL(view->store), n);
		g_object_unref(msg);    /* the store keeps it */
		if (pidgin_message_has_id(msg, id))
			return msg;
	}
	return NULL;
}

PidginMessage *
pidgin_message_view_get_last_sent(PidginMessageView *view)
{
	guint n;

	g_return_val_if_fail(PIDGIN_IS_MESSAGE_VIEW(view), NULL);

	n = g_list_model_get_n_items(G_LIST_MODEL(view->store));
	while (n-- > 0) {
		PidginMessage *msg = g_list_model_get_item(G_LIST_MODEL(view->store), n);
		g_object_unref(msg);
		if (pidgin_message_get_kind(msg) == PIDGIN_MESSAGE_KIND_NORMAL &&
		    message_is_own(msg) && !pidgin_message_get_retracted(msg))
			return msg;
	}
	return NULL;
}

void
pidgin_message_view_remove_marker(PidginMessageView *view)
{
	guint pos;

	g_return_if_fail(PIDGIN_IS_MESSAGE_VIEW(view));

	if (view->marker == NULL)
		return;
	if (g_list_store_find(view->store, view->marker, &pos))
		g_list_store_remove(view->store, pos);
	g_clear_object(&view->marker);
}

void
pidgin_message_view_set_marker(PidginMessageView *view)
{
	gboolean stick;

	g_return_if_fail(PIDGIN_IS_MESSAGE_VIEW(view));

	stick = pidgin_message_view_is_at_bottom(view);
	pidgin_message_view_remove_marker(view);
	if (g_list_model_get_n_items(G_LIST_MODEL(view->store)) == 0)
		return;
	view->marker = pidgin_message_new_marker();
	g_list_store_append(view->store, view->marker);
	if (stick)
		pidgin_message_view_scroll_to_bottom(view);
}

PidginMessage *
pidgin_message_view_get_marker(PidginMessageView *view)
{
	g_return_val_if_fail(PIDGIN_IS_MESSAGE_VIEW(view), NULL);
	return view->marker;
}

/**************************************************************************
 * Find
 **************************************************************************/

static gboolean
filter_func(gpointer item, gpointer data)
{
	PidginMessageView *view = data;
	PidginMessage *msg = item;
	char *hay, *needle;
	gboolean match;

	if (view->search == NULL)
		return TRUE;
	if (pidgin_message_get_kind(msg) != PIDGIN_MESSAGE_KIND_NORMAL)
		return FALSE;

	needle = g_utf8_casefold(view->search, -1);
	hay = g_utf8_casefold(pidgin_message_get_plain_text(msg), -1);
	match = strstr(hay, needle) != NULL;
	g_free(hay);
	if (!match && pidgin_message_get_alias(msg) != NULL) {
		hay = g_utf8_casefold(pidgin_message_get_alias(msg), -1);
		match = strstr(hay, needle) != NULL;
		g_free(hay);
	}
	g_free(needle);
	return match;
}

void
pidgin_message_view_set_search_text(PidginMessageView *view, const char *text)
{
	GHashTableIter iter;
	gpointer row;

	g_return_if_fail(PIDGIN_IS_MESSAGE_VIEW(view));

	if (text != NULL && *text == '\0')
		text = NULL;
	if (g_strcmp0(view->search, text) == 0)
		return;
	g_free(view->search);
	view->search = g_strdup(text);

	gtk_filter_changed(GTK_FILTER(view->filter), GTK_FILTER_CHANGE_DIFFERENT);
	g_hash_table_iter_init(&iter, view->rows);
	while (g_hash_table_iter_next(&iter, &row, NULL)) {
		PidginMessageRow *r = row;
		if (r->msg && !pidgin_message_get_retracted(r->msg))
			pidgin_rich_label_set_highlight(PIDGIN_RICH_LABEL(r->body), view->search);
	}
	if (text == NULL)
		pidgin_message_view_scroll_to_bottom(view);

	if (text != NULL && g_strcmp0(gtk_editable_get_text(GTK_EDITABLE(view->search_entry)), text))
		gtk_editable_set_text(GTK_EDITABLE(view->search_entry), text);
}

static void
search_changed_cb(GtkSearchEntry *entry, PidginMessageView *view)
{
	pidgin_message_view_set_search_text(view, gtk_editable_get_text(GTK_EDITABLE(entry)));
}

static void
search_mode_cb(GObject *bar, GParamSpec *pspec, PidginMessageView *view)
{
	if (!gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(bar)))
		pidgin_message_view_set_search_text(view, NULL);
}

GtkSearchBar *
pidgin_message_view_get_search_bar(PidginMessageView *view)
{
	g_return_val_if_fail(PIDGIN_IS_MESSAGE_VIEW(view), NULL);
	return GTK_SEARCH_BAR(view->search_bar);
}

void
pidgin_message_view_set_search_mode(PidginMessageView *view, gboolean on)
{
	g_return_if_fail(PIDGIN_IS_MESSAGE_VIEW(view));
	gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(view->search_bar), on);
	if (on)
		gtk_widget_grab_focus(view->search_entry);
}

guint
pidgin_message_view_get_n_visible(PidginMessageView *view)
{
	g_return_val_if_fail(PIDGIN_IS_MESSAGE_VIEW(view), 0);
	return g_list_model_get_n_items(G_LIST_MODEL(view->filtered));
}

/**************************************************************************
 * Properties
 **************************************************************************/

void
pidgin_message_view_set_conversation(PidginMessageView *view, PurpleConversation *conv)
{
	g_return_if_fail(PIDGIN_IS_MESSAGE_VIEW(view));

	view->conv = conv;
	if (conv != NULL)
		view->is_chat = purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_CHAT;
}

PurpleConversation *
pidgin_message_view_get_conversation(PidginMessageView *view)
{
	g_return_val_if_fail(PIDGIN_IS_MESSAGE_VIEW(view), NULL);
	return view->conv;
}

void
pidgin_message_view_set_is_chat(PidginMessageView *view, gboolean is_chat)
{
	g_return_if_fail(PIDGIN_IS_MESSAGE_VIEW(view));
	view->is_chat = is_chat;
}

void
pidgin_message_view_set_nick_color_scheme(PidginMessageView *view,
                                          PidginNickColorScheme scheme)
{
	g_return_if_fail(PIDGIN_IS_MESSAGE_VIEW(view));
	view->nick_scheme = scheme;
}

void
pidgin_message_view_set_self_id(PidginMessageView *view, const char *self_id)
{
	g_return_if_fail(PIDGIN_IS_MESSAGE_VIEW(view));
	g_free(view->self_id);
	view->self_id = g_strdup(self_id);
}

GListModel *
pidgin_message_view_get_model(PidginMessageView *view)
{
	g_return_val_if_fail(PIDGIN_IS_MESSAGE_VIEW(view), NULL);
	return G_LIST_MODEL(view->store);
}

void
pidgin_message_view_set_message_actions(PidginMessageView *view, gboolean enabled,
                                        gboolean can_moderate)
{
	GHashTableIter iter;
	gpointer row;

	g_return_if_fail(PIDGIN_IS_MESSAGE_VIEW(view));
	view->message_actions = enabled;
	view->can_moderate = can_moderate;
	g_hash_table_iter_init(&iter, view->rows);
	while (g_hash_table_iter_next(&iter, &row, NULL))
		if (PIDGIN_MESSAGE_ROW(row)->msg != NULL)
			update_menu(row);
}

void
pidgin_message_view_refresh(PidginMessageView *view)
{
	GHashTableIter iter;
	gpointer row;

	g_return_if_fail(PIDGIN_IS_MESSAGE_VIEW(view));
	g_hash_table_iter_init(&iter, view->rows);
	while (g_hash_table_iter_next(&iter, &row, NULL))
		if (PIDGIN_MESSAGE_ROW(row)->msg != NULL)
			row_update(row);
}

static void
edge_reached_cb(GtkScrolledWindow *sw, GtkPositionType pos, PidginMessageView *view)
{
	if (pos == GTK_POS_TOP)
		g_signal_emit(view, signals[SIG_TOP_REACHED], 0);
}

void
pidgin_message_view_set_scrollback(PidginMessageView *view, guint max)
{
	g_return_if_fail(PIDGIN_IS_MESSAGE_VIEW(view));
	view->scrollback = max;
	trim_scrollback(view);
}

/**************************************************************************
 * GObject
 **************************************************************************/

static void
pidgin_message_view_dispose(GObject *obj)
{
	PidginMessageView *view = PIDGIN_MESSAGE_VIEW(obj);
	GtkWidget *child;

	if (view->tick_id) {
		gtk_widget_remove_tick_callback(GTK_WIDGET(view), view->tick_id);
		view->tick_id = 0;
	}
	while ((child = gtk_widget_get_first_child(GTK_WIDGET(view))) != NULL)
		gtk_widget_unparent(child);
	g_clear_object(&view->filtered);
	g_clear_object(&view->store);
	g_clear_object(&view->marker);
	g_clear_pointer(&view->rows, g_hash_table_destroy);

	G_OBJECT_CLASS(pidgin_message_view_parent_class)->dispose(obj);
}

static void
pidgin_message_view_finalize(GObject *obj)
{
	PidginMessageView *view = PIDGIN_MESSAGE_VIEW(obj);

	g_free(view->search);
	g_free(view->self_id);
	G_OBJECT_CLASS(pidgin_message_view_parent_class)->finalize(obj);
}

static void
pidgin_message_view_class_init(PidginMessageViewClass *klass)
{
	GObjectClass *obj_class = G_OBJECT_CLASS(klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

	obj_class->dispose = pidgin_message_view_dispose;
	obj_class->finalize = pidgin_message_view_finalize;
	gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BOX_LAYOUT);
	gtk_widget_class_set_css_name(widget_class, "pidgin-message-view");

	signals[SIG_REACTION_TOGGLED] = g_signal_new("reaction-toggled",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 3, PIDGIN_TYPE_MESSAGE, G_TYPE_STRING, G_TYPE_BOOLEAN);
	signals[SIG_REPLY_REQUESTED] = g_signal_new("reply-requested",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, PIDGIN_TYPE_MESSAGE);
	signals[SIG_EDIT_REQUESTED] = g_signal_new("edit-requested",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, PIDGIN_TYPE_MESSAGE);
	signals[SIG_RETRACT_REQUESTED] = g_signal_new("retract-requested",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, PIDGIN_TYPE_MESSAGE);
	signals[SIG_POPULATE_MENU] = g_signal_new("populate-menu",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 2, PIDGIN_TYPE_MESSAGE, G_TYPE_MENU);
	signals[SIG_TOP_REACHED] = g_signal_new("top-reached",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 0);
}

static void
pidgin_message_view_init(PidginMessageView *view)
{
	GtkLayoutManager *layout = gtk_widget_get_layout_manager(GTK_WIDGET(view));
	GtkListItemFactory *factory;
	GtkNoSelection *selection;

	gtk_orientable_set_orientation(GTK_ORIENTABLE(layout), GTK_ORIENTATION_VERTICAL);

	view->rows = g_hash_table_new(g_direct_hash, g_direct_equal);
	view->scrollback = pref_int(PIDGIN_PREFS_ROOT "/conversations/scrollback_lines",
	                            DEFAULT_SCROLLBACK);

	/* find */
	view->search_entry = gtk_search_entry_new();
	gtk_widget_set_hexpand(view->search_entry, TRUE);
	g_signal_connect(view->search_entry, "search-changed", G_CALLBACK(search_changed_cb), view);
	view->search_bar = gtk_search_bar_new();
	gtk_search_bar_set_child(GTK_SEARCH_BAR(view->search_bar), view->search_entry);
	gtk_search_bar_connect_entry(GTK_SEARCH_BAR(view->search_bar),
	                             GTK_EDITABLE(view->search_entry));
	gtk_search_bar_set_show_close_button(GTK_SEARCH_BAR(view->search_bar), TRUE);
	g_signal_connect(view->search_bar, "notify::search-mode-enabled",
	                 G_CALLBACK(search_mode_cb), view);
	gtk_widget_set_parent(view->search_bar, GTK_WIDGET(view));

	/* the list */
	view->store = g_list_store_new(PIDGIN_TYPE_MESSAGE);
	view->filter = gtk_custom_filter_new(filter_func, view, NULL);
	view->filtered = gtk_filter_list_model_new(G_LIST_MODEL(g_object_ref(view->store)),
	                                           GTK_FILTER(view->filter));
	selection = gtk_no_selection_new(G_LIST_MODEL(g_object_ref(view->filtered)));

	factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(factory_setup_cb), view);
	g_signal_connect(factory, "bind", G_CALLBACK(factory_bind_cb), view);
	g_signal_connect(factory, "unbind", G_CALLBACK(factory_unbind_cb), view);

	view->list = gtk_list_view_new(GTK_SELECTION_MODEL(selection), factory);
	gtk_widget_add_css_class(view->list, "pidgin-message-list");

	view->scrolled = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(view->scrolled),
	                               GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(view->scrolled), view->list);
	gtk_widget_set_vexpand(view->scrolled, TRUE);
	gtk_widget_set_hexpand(view->scrolled, TRUE);
	gtk_widget_set_parent(view->scrolled, GTK_WIDGET(view));
	g_signal_connect(view->scrolled, "edge-reached", G_CALLBACK(edge_reached_cb), view);
	view->message_actions = TRUE;
}

GtkWidget *
pidgin_message_view_new(void)
{
	return g_object_new(PIDGIN_TYPE_MESSAGE_VIEW, NULL);
}
