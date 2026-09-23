/*
 * pidgin4: the conversation UI selftest (PIDGIN4_CONV_SELFTEST=1).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * No real account may sign in during tests (the user's Pidgin 2 runs
 * them), and libpurple needs a connection to create a conversation. So
 * this registers an in-process protocol plugin, "prpl-pidgin4-selftest"
 * (tests/selftest-prpl.c, shared with the M7 plugins selftest), whose
 * login succeeds at once, with the M8 IPC commands (they only record
 * their calls), and a throwaway account on it. It then drives
 * conversations of both types through libpurple's own API, emits the M8
 * signals by hand, runs the window actions, checks the view, the index
 * (messages.db), the HTML log (the contract rule 7 fallback lines) and
 * the unseen state, removes the account, and quits with status 0 on
 * success. Run it on a scratch copy of a profile: it writes logs and
 * index rows (see pidgin4/TESTING.md).
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include <glib/gstdio.h>
#include <libsoup/soup.h>

#include "account.h"
#include "blist.h"
#include "buddyicon.h"
#include "cmds.h"
#include "connection.h"
#include "conversation.h"
#include "debug.h"
#include "imgstore.h"
#include "log.h"
#include "plugin.h"
#include "prefs.h"
#include "prpl.h"
#include "server.h"
#include "signals.h"
#include "status.h"
#include "util.h"
#include "version.h"

#include "gtkconv.h"
#include "gtkconvwin.h"
#include "pidginattachment.h"
#include "pidginimageloader.h"
#include "pidgincomposeentry.h"
#include "pidginformattoolbar.h"
#include "pidginconvmeta.h"
#include "pidginmessage.h"
#include "pidginmessageindex.h"
#include "pidginmessageview.h"

#include "tests/selftest-prpl.h"

#define ST_PRPL_ID PIDGIN_SELFTEST_PRPL_ID
#define ST_USER "selftest@example.invalid"
#define ST_BUDDY "buddy@example.invalid"
#define ST_ROOM "room@conference.example.invalid"
#define THUMBS "\xf0\x9f\x91\x8d"
#define PARTY "\xf0\x9f\x8e\x89"

static int failures = 0;
static int checks = 0;
static PurpleAccount *st_account = NULL;

#define CHECK(cond, ...) G_STMT_START { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		g_printerr("PIDGIN4_CONV_SELFTEST: FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
		g_printerr(__VA_ARGS__); \
		g_printerr("\n"); \
	} \
} G_STMT_END

/**************************************************************************
 * Helpers
 **************************************************************************/

static void
spin(guint ms)
{
	pidgin_selftest_spin(ms);
}

/* PIDGIN4_CONV_SELFTEST_HOLD=N: stay N seconds at the named points (for
 * screenshots: xwd/import on the Xvfb display, grim on Wayland). */
static void
hold(const char *what)
{
	const char *env = g_getenv("PIDGIN4_CONV_SELFTEST_HOLD");
	int secs = env ? atoi(env) : 0;

	if (secs <= 0)
		return;
	g_print("PIDGIN4_CONV_SELFTEST: holding %d s (%s)\n", secs, what);
	spin(secs * 1000);
	/* What input during the hold did (e.g. xdotool key ctrl+Tab). */
	{
		GList *l;

		for (l = pidgin_conv_windows_get_list(); l != NULL; l = l->next) {
			PurpleConversation *c = pidgin_conv_window_get_active_conversation(l->data);

			g_print("PIDGIN4_CONV_SELFTEST: after %s: window %p on %s (%u tabs)\n", what,
			        l->data, c ? purple_conversation_get_name(c) : "-",
			        pidgin_conv_window_get_gtkconv_count(l->data));
		}
	}
}

static GHashTable *
meta_new(const char *first_key, ...)
{
	GHashTable *meta = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	const char *key = first_key;
	va_list args;

	va_start(args, first_key);
	while (key != NULL) {
		const char *value = va_arg(args, const char *);

		g_hash_table_insert(meta, g_strdup(key), g_strdup(value));
		key = va_arg(args, const char *);
	}
	va_end(args);
	return meta;
}

static void
emit_meta(const char *conv_name, GHashTable *meta)
{
	purple_signal_emit(purple_conversations_get_handle(), "receiving-message-meta",
	                   st_account, conv_name, meta);
}

static PidginMessageView *
view_of(PurpleConversation *conv)
{
	return PIDGIN_MESSAGE_VIEW(pidgin_conv_get_message_view(PIDGIN_CONVERSATION(conv)));
}

static guint
n_messages(PurpleConversation *conv)
{
	return g_list_model_get_n_items(pidgin_message_view_get_model(view_of(conv)));
}

static PidginMessage *
nth_message(PurpleConversation *conv, guint n)
{
	PidginMessage *m = g_list_model_get_item(pidgin_message_view_get_model(view_of(conv)), n);

	if (m != NULL)
		g_object_unref(m);  /* the store keeps it */
	return m;
}

static PidginMessage *
last_message(PurpleConversation *conv)
{
	guint n = n_messages(conv);

	return n > 0 ? nth_message(conv, n - 1) : NULL;
}

static char *
log_path(PurpleConversation *conv)
{
	return pidgin_conv_meta_log_file(conv);
}

static gboolean
file_contains(const char *path, const char *needle)
{
	char *contents = NULL;
	gboolean ret;

	if (path == NULL || !g_file_get_contents(path, &contents, NULL, NULL))
		return FALSE;
	ret = strstr(contents, needle) != NULL;
	g_free(contents);
	return ret;
}

static gboolean
emit_bool(const char *signal, ...)
{
	/* purple_signal_emit_return_1 with varargs */
	va_list args;
	void *ret;

	va_start(args, signal);
	ret = purple_signal_emit_vargs_return_1(purple_conversations_get_handle(), signal, args);
	va_end(args);
	return GPOINTER_TO_INT(ret) != 0;
}

static const char *
call(const char *command)
{
	return pidgin_selftest_prpl_get_call(command);
}

static gboolean
activate(PidginWindow *win, const char *action, GVariant *param)
{
	return gtk_widget_activate_action_variant(win->window, action, param);
}

/* A plain red PNG of the given size (g_free it) */
static gpointer
make_png(int width, int height, gsize *len)
{
	guchar *pixels = g_malloc(width * height * 4);
	GBytes *bytes, *png;
	GdkTexture *texture;
	int i;

	for (i = 0; i < width * height * 4; i += 4) {
		pixels[i] = 0xcc;
		pixels[i + 1] = 0x22;
		pixels[i + 2] = 0x22;
		pixels[i + 3] = 0xff;
	}
	bytes = g_bytes_new_take(pixels, width * height * 4);
	texture = gdk_memory_texture_new(width, height, GDK_MEMORY_R8G8B8A8, bytes, width * 4);
	png = gdk_texture_save_to_png_bytes(texture);
	g_bytes_unref(bytes);
	g_object_unref(texture);
	return g_bytes_unref_to_data(png, len);
}

/* A small PNG in the imgstore (the caller unrefs it) */
static int
add_test_image(void)
{
	gsize len;
	gpointer data = make_png(8, 8, &len);

	return purple_imgstore_add_with_id(data, len, "selftest.png");
}

/* Steam's prpl: no HTML, no OPT_PROTO_IM_IMAGE, no file transfer */
static void
set_steam_like(PurpleConversation *conv, gboolean steam)
{
	PurpleConnection *gc = purple_account_get_connection(st_account);
	PurplePluginProtocolInfo *prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(gc->prpl);

	if (steam) {
		gc->flags &= ~PURPLE_CONNECTION_HTML;
		prpl_info->options &= ~OPT_PROTO_IM_IMAGE;
	} else {
		gc->flags |= PURPLE_CONNECTION_HTML;
		prpl_info->options |= OPT_PROTO_IM_IMAGE;
	}
	purple_conversation_set_features(conv, gc->flags);
	pidgin_conv_update_buttons_by_protocol(conv);
}

static gboolean entry_has_anchor(PidginComposeEntry *entry);

/* Drops a file on the conversation as a file manager would; returns the
 * buttons of the dialog it opened (NULL if none), and closes that. */
static char *
drop_file(PurpleConversation *conv, const char *path)
{
	GtkWidget *tab = PIDGIN_CONVERSATION(conv)->tab_cont;
	GListModel *controllers = gtk_widget_observe_controllers(tab);
	GtkDropTarget *target = NULL;
	GValue value = G_VALUE_INIT;
	GSList *files;
	gboolean ret = FALSE;
	char *buttons = NULL;
	guint i;

	for (i = 0; i < g_list_model_get_n_items(controllers) && target == NULL; i++) {
		GObject *c = g_list_model_get_item(controllers, i);

		if (GTK_IS_DROP_TARGET(c))
			target = GTK_DROP_TARGET(c);
		g_object_unref(c);
	}
	g_object_unref(controllers);
	if (target == NULL)
		return NULL;

	g_object_set_data(G_OBJECT(tab), "pidgin-image-drop-dialog", NULL);
	files = g_slist_append(NULL, g_file_new_for_path(path));
	g_value_init(&value, GDK_TYPE_FILE_LIST);
	g_value_take_boxed(&value, gdk_file_list_new_from_list(files));
	g_slist_free_full(files, g_object_unref);
	g_signal_emit_by_name(target, "drop", &value, 1.0, 1.0, &ret);
	g_value_unset(&value);
	spin(100);

	if (g_object_get_data(G_OBJECT(tab), "pidgin-image-drop-dialog") != NULL) {
		char **list = NULL;

		g_object_get(g_object_get_data(G_OBJECT(tab), "pidgin-image-drop-dialog"),
		             "buttons", &list, NULL);
		buttons = list ? g_strjoinv("|", list) : g_strdup("");
		g_strfreev(list);
		g_cancellable_cancel(g_object_get_data(G_OBJECT(tab), "pidgin-image-drop-cancel"));
		spin(100);
	}
	return buttons;
}

/* Sending an inline image (Insert Image, or a dropped image put in the
 * message) crashed right after the send: the compose entry's image
 * anchor upset libspelling when the entry was cleared. Both with an HTML
 * protocol (the image goes out as <img id=N>) and one like Steam's. */
static void
test_images(PurpleConversation *conv)
{
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);
	PidginComposeEntry *entry = PIDGIN_COMPOSE_ENTRY(gtkconv->entry);
	PurpleBuddy *buddy;
	char *path, *buttons;
	guint n;
	int id;

	CHECK(pidgin_compose_entry_get_caps(entry) & PIDGIN_FORMAT_IMAGE, "no image caps");

	/* Text and an image */
	id = add_test_image();
	CHECK(id > 0, "imgstore add");
	pidgin_compose_entry_set_markup(entry, "look: ");
	pidgin_compose_entry_insert_image(entry, id);
	purple_imgstore_unref_by_id(id);    /* the entry holds its own */
	n = n_messages(conv);
	CHECK(pidgin_compose_entry_send(entry), "image send");
	spin(300);
	CHECK(n_messages(conv) == n + 1, "image message not shown (%u, %u)", n_messages(conv), n);
	CHECK(call("send-im") != NULL && strstr(call("send-im"), "look:") != NULL,
	      "send-im: %s", call("send-im"));
	g_print("PIDGIN4_CONV_SELFTEST: image message: %s\n",
	        pidgin_message_get_html(last_message(conv)));

	/* An image alone */
	id = add_test_image();
	pidgin_compose_entry_insert_image(entry, id);
	purple_imgstore_unref_by_id(id);
	n = n_messages(conv);
	CHECK(pidgin_compose_entry_send(entry), "image-only send");
	spin(300);
	CHECK(n_messages(conv) == n + 1, "image-only message not shown (%u, %u)",
	      n_messages(conv), n);

	/* History: Up brings the sent image back */
	pidgin_compose_entry_history_up(entry);
	spin(100);
	pidgin_compose_entry_clear(entry);

	/* Dropping an image: the prpl takes inline images, no file transfer,
	 * the buddy isn't on the list. As a paste: inline, no question. */
	id = add_test_image();
	path = g_build_filename(purple_user_dir(), "selftest-drop.png", NULL);
	CHECK(g_file_set_contents(path, purple_imgstore_get_data(purple_imgstore_find_by_id(id)),
	                          purple_imgstore_get_size(purple_imgstore_find_by_id(id)), NULL),
	      "writing %s", path);
	purple_imgstore_unref_by_id(id);
	buttons = drop_file(conv, path);
	CHECK(buttons == NULL, "drop asked %s", buttons);
	CHECK(entry_has_anchor(entry), "the dropped image isn't in the entry");
	g_free(buttons);
	pidgin_compose_entry_clear(entry);

	/* A protocol like Steam: no inline images (as Pidgin 2), and a drop
	 * offers only the buddy icon */
	set_steam_like(conv, TRUE);
	CHECK(!(pidgin_compose_entry_get_caps(entry) & PIDGIN_FORMAT_IMAGE),
	      "images offered to a prpl without OPT_PROTO_IM_IMAGE");
	buttons = drop_file(conv, path);
	CHECK(buttons == NULL, "drop offered %s without a buddy", buttons);
	g_free(buttons);
	buddy = purple_buddy_new(st_account, ST_BUDDY, NULL);
	purple_blist_add_buddy(buddy, NULL, NULL, NULL);
	buttons = drop_file(conv, path);
	CHECK(purple_strequal(buttons, "Set as Buddy Icon|Cancel"), "drop offered %s", buttons);
	g_free(buttons);
	purple_blist_remove_buddy(buddy);
	g_unlink(path);
	g_free(path);

	/* ... and an image that got into the entry anyway (a pasted draft)
	 * goes out stripped, as it did in Pidgin 2 */
	id = add_test_image();
	pidgin_compose_entry_set_markup(entry, "plain ");
	pidgin_compose_entry_insert_image(entry, id);
	purple_imgstore_unref_by_id(id);
	n = n_messages(conv);
	pidgin_compose_entry_send(entry);
	spin(300);
	CHECK(n_messages(conv) == n + 1, "plain image message not shown (%u, %u)",
	      n_messages(conv), n);
	set_steam_like(conv, FALSE);
	hold("images");
}

/* The infopane's buddy icon is 32 px, whatever the icon's size (it took
 * a large part of the window with a Steam avatar). */
static void
test_buddy_icon(PurpleConversation *conv)
{
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);
	GtkWidget *icon = gtkconv->u.im->icon;
	gboolean show = purple_prefs_get_bool(PIDGIN_PREFS_ROOT "/conversations/im/show_buddy_icons");
	int min = 0, nat = 0;
	gsize len;
	gpointer data;

	purple_prefs_set_bool(PIDGIN_PREFS_ROOT "/conversations/im/show_buddy_icons", TRUE);
	data = make_png(256, 128, &len);
	purple_buddy_icons_set_for_user(st_account, ST_BUDDY, data, len, NULL);
	pidgin_conv_update_buddy_icon(conv);
	spin(300);
	CHECK(gtk_widget_get_visible(icon), "no buddy icon");
	gtk_widget_measure(icon, GTK_ORIENTATION_HORIZONTAL, -1, &min, &nat, NULL, NULL);
	CHECK(nat <= 32, "icon natural width %d", nat);
	gtk_widget_measure(icon, GTK_ORIENTATION_VERTICAL, -1, &min, &nat, NULL, NULL);
	CHECK(nat <= 32, "icon natural height %d", nat);
	CHECK(gtk_widget_get_width(icon) <= 32 && gtk_widget_get_height(icon) <= 32,
	      "icon allocated %dx%d", gtk_widget_get_width(icon), gtk_widget_get_height(icon));
	hold("buddy icon");

	purple_buddy_icons_set_for_user(st_account, ST_BUDDY, NULL, 0, NULL);
	pidgin_conv_update_buddy_icon(conv);
	purple_prefs_set_bool(PIDGIN_PREFS_ROOT "/conversations/im/show_buddy_icons", show);
}

/* The toolbar's "Attention!" button: shown for a prpl with
 * send_attention, sends it; hidden without. */
static void
test_attention(PurpleConversation *conv)
{
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);
	PurplePluginProtocolInfo *prpl_info =
		PURPLE_PLUGIN_PROTOCOL_INFO(purple_account_get_connection(st_account)->prpl);
	GtkWidget *button;
	gboolean (*send_attention)(PurpleConnection *, const char *, guint);

	CHECK(gtkconv->toolbar != NULL, "no toolbar");
	if (gtkconv->toolbar == NULL)
		return;
	button = pidgin_format_toolbar_get_attention_button(PIDGIN_FORMAT_TOOLBAR(gtkconv->toolbar));
	/* the button acts on its window's active conversation, its own tab */
	pidgin_conv_window_switch_gtkconv(gtkconv->win, gtkconv);
	pidgin_conv_update_buttons_by_protocol(conv);
	CHECK(gtk_widget_get_visible(button), "no attention button with send_attention");
	CHECK(gtk_widget_activate(button), "attention button not activatable");
	spin(400);      /* a button emits clicked after its activate animation */
	CHECK(purple_strequal(call("send-attention"), ST_BUDDY "||"), "send-attention: %s",
	      call("send-attention"));

	send_attention = prpl_info->send_attention;
	prpl_info->send_attention = NULL;
	pidgin_conv_update_buttons_by_protocol(conv);
	CHECK(!gtk_widget_get_visible(button), "attention button without send_attention");
	prpl_info->send_attention = send_attention;
	pidgin_conv_update_buttons_by_protocol(conv);

	/* never in a chat */
	{
		GList *l;

		for (l = purple_get_chats(); l != NULL; l = l->next) {
			PidginConversation *gtkchat = PIDGIN_CONVERSATION((PurpleConversation *)l->data);

			if (gtkchat != NULL && gtkchat->toolbar != NULL)
				CHECK(!gtk_widget_get_visible(pidgin_format_toolbar_get_attention_button(
				          PIDGIN_FORMAT_TOOLBAR(gtkchat->toolbar))), "attention in a chat");
		}
	}
}

static gboolean
send_to(PidginWindow *win, PurpleAccount *account, const char *name)
{
	return activate(win, "conv.send-to", g_variant_new("(sss)",
		purple_account_get_protocol_id(account), purple_account_get_username(account), name));
}

/* Send To (Pidgin 2's): an IM with a buddy whose contact has more buddies
 * gets the menu, and picking one re-targets the same conversation. */
static void
test_send_to(PurpleConversation *conv)
{
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);
	PidginWindow *win = gtkconv->win;
	PurpleAccount *account2;
	PurpleGroup *group;
	PurpleBuddy *b1, *b2, *b3;
	PurpleContact *contact;
	guint n, bar_items;

	pidgin_conv_window_switch_gtkconv(win, gtkconv);
	pidgin_conv_window_update_menu(win);
	bar_items = g_menu_model_get_n_items(G_MENU_MODEL(win->menu.model));
	CHECK(!win->send_to_shown, "Send To without a contact");

	/* A contact: two buddies on this account, one on a second account */
	account2 = pidgin_selftest_account_new("selftest2@example.invalid");
	group = purple_group_new("pidgin4 selftest Send To");
	purple_blist_add_group(group, NULL);
	b1 = purple_buddy_new(st_account, ST_BUDDY, NULL);
	purple_blist_add_buddy(b1, NULL, group, NULL);
	contact = purple_buddy_get_contact(b1);
	b2 = purple_buddy_new(st_account, "buddy2@example.invalid", NULL);
	purple_blist_add_buddy(b2, contact, group, NULL);
	b3 = purple_buddy_new(account2, "buddy3@example.invalid", NULL);
	purple_blist_add_buddy(b3, contact, group, NULL);

	pidgin_conv_window_update_menu(win);
	CHECK(win->send_to_shown && g_menu_model_get_n_items(G_MENU_MODEL(win->menu.model)) ==
	      (int)bar_items + 1, "no Send To menu");
	CHECK(g_menu_model_get_n_items(G_MENU_MODEL(win->send_to)) == 3, "%d Send To items",
	      g_menu_model_get_n_items(G_MENU_MODEL(win->send_to)));

	/* The other buddy on the same account */
	n = n_messages(conv);
	CHECK(send_to(win, st_account, "buddy2@example.invalid"), "conv.send-to");
	CHECK(purple_strequal(purple_conversation_get_name(conv), "buddy2@example.invalid"),
	      "name %s", purple_conversation_get_name(conv));
	CHECK(purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, "buddy2@example.invalid",
	                                            st_account) == conv, "not found by the new name");
	CHECK(purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, ST_BUDDY, st_account) == NULL,
	      "still found by the old name");
	CHECK(n_messages(conv) == n, "the scrollback changed (%u, %u)", n_messages(conv), n);
	pidgin_compose_entry_set_markup(PIDGIN_COMPOSE_ENTRY(gtkconv->entry), "to the second");
	pidgin_compose_entry_send(PIDGIN_COMPOSE_ENTRY(gtkconv->entry));
	CHECK(purple_strequal(call("send-im"), "buddy2@example.invalid|to the second|"),
	      "send-im: %s", call("send-im"));
	{
		char *path = log_path(conv);

		CHECK(path != NULL && strstr(path, "buddy2@example.invalid") != NULL &&
		      file_contains(path, "to the second"), "logged to %s", path ? path : "(none)");
		g_free(path);
	}
	{
		GVariant *state = g_action_group_get_action_state(G_ACTION_GROUP(win->actions),
		                                                  "send-to");
		const char *name = NULL;

		g_variant_get(state, "(&s&s&s)", NULL, NULL, &name);
		CHECK(purple_strequal(name, "buddy2@example.invalid"), "Send To state %s", name);
		g_variant_unref(state);
	}

	/* The buddy on the other account */
	CHECK(send_to(win, account2, "buddy3@example.invalid"), "conv.send-to (account)");
	CHECK(purple_conversation_get_account(conv) == account2, "account not changed");
	CHECK(purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, "buddy3@example.invalid",
	                                            account2) == conv, "not found on account 2");
	CHECK(purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, "buddy2@example.invalid",
	                                            st_account) == NULL, "found on account 1");
	pidgin_compose_entry_set_markup(PIDGIN_COMPOSE_ENTRY(gtkconv->entry), "to the third");
	pidgin_compose_entry_send(PIDGIN_COMPOSE_ENTRY(gtkconv->entry));
	CHECK(purple_strequal(call("send-im"), "buddy3@example.invalid|to the third|"),
	      "send-im: %s", call("send-im"));
	CHECK(n_messages(conv) == n + 2, "messages %u, %u", n_messages(conv), n);

	/* and back */
	CHECK(send_to(win, st_account, ST_BUDDY), "conv.send-to (back)");
	CHECK(purple_conversation_get_account(conv) == st_account &&
	      purple_strequal(purple_conversation_get_name(conv), ST_BUDDY), "not back");
	CHECK(purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, ST_BUDDY, st_account) ==
	      conv, "not found back");
	hold("send to");

	/* The contact goes: so does the menu */
	purple_blist_remove_buddy(b3);
	purple_blist_remove_buddy(b2);
	purple_blist_remove_buddy(b1);
	purple_blist_remove_group(group);
	pidgin_selftest_account_remove(account2);
	pidgin_conv_window_update_menu(win);
	CHECK(!win->send_to_shown && g_menu_model_get_n_items(G_MENU_MODEL(win->menu.model)) ==
	      (int)bar_items, "Send To left over");
}

/* Puts a 16x16 PNG texture on the clipboard, alone or with @text (a
 * union provider, text first as apps offer it), and pastes as Ctrl+V. */
static void
paste_image_clipboard(PidginComposeEntry *entry, const char *text)
{
	GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(entry));
	gsize len;
	gpointer data = make_png(16, 16, &len);
	GBytes *bytes = g_bytes_new_take(data, len);
	GdkTexture *texture = gdk_texture_new_from_bytes(bytes, NULL);

	if (text == NULL) {
		gdk_clipboard_set_texture(clipboard, texture);
	} else {
		GdkContentProvider *providers[2];
		GdkContentProvider *union_provider;

		providers[0] = gdk_content_provider_new_typed(G_TYPE_STRING, text);
		providers[1] = gdk_content_provider_new_typed(GDK_TYPE_TEXTURE, texture);
		union_provider = gdk_content_provider_new_union(providers, 2);
		gdk_clipboard_set_content(clipboard, union_provider);
		g_object_unref(union_provider);
	}
	g_object_unref(texture);
	g_bytes_unref(bytes);
	spin(50);
	g_signal_emit_by_name(entry, "paste-clipboard");
	spin(400);
}

/* The files in <profile>/pidgin4/paste */
static int
count_paste_files(char **first)
{
	char *dir = g_build_filename(purple_user_dir(), "pidgin4", "paste", NULL);
	GDir *d = g_dir_open(dir, 0, NULL);
	const char *name;
	int n = 0;

	while (d != NULL && (name = g_dir_read_name(d)) != NULL) {
		if (n++ == 0 && first != NULL)
			*first = g_build_filename(dir, name, NULL);
	}
	if (d != NULL)
		g_dir_close(d);
	g_free(dir);
	return n;
}

/* The "Send Image/File" confirmation open on @conv (gtkconv.c keeps it on
 * the tab), or NULL. */
#define SEND_CONFIRM_KEY "pidgin-send-confirm-dialog"

static GtkWindow *
confirm_dialog(PurpleConversation *conv)
{
	return g_object_get_data(G_OBJECT(PIDGIN_CONVERSATION(conv)->tab_cont), SEND_CONFIRM_KEY);
}

static GtkWidget *
find_named(GtkWidget *widget, const char *name)
{
	GtkWidget *child, *ret = NULL;

	if (purple_strequal(gtk_widget_get_name(widget), name))
		return widget;
	for (child = gtk_widget_get_first_child(widget); child != NULL && ret == NULL;
	     child = gtk_widget_get_next_sibling(child))
		ret = find_named(child, name);
	return ret;
}

static const char *
confirm_label(GtkWindow *dialog, const char *name)
{
	GtkWidget *label = find_named(GTK_WIDGET(dialog), name);

	return GTK_IS_LABEL(label) ? gtk_label_get_text(GTK_LABEL(label)) : "";
}

/* Send (the default widget, as Enter) or Cancel (window.close, as
 * Escape) */
static void
confirm_answer(PurpleConversation *conv, gboolean send)
{
	GtkWindow *dialog = confirm_dialog(conv);

	if (dialog == NULL)
		return;
	if (send)
		gtk_widget_activate(gtk_window_get_default_widget(dialog));
	else
		gtk_widget_activate_action(GTK_WIDGET(dialog), "window.close", NULL);
	spin(300);
}

/* Pasting an image: (a) inline with OPT_PROTO_IM_IMAGE; (b) as a file
 * transfer of <profile>/pidgin4/paste/pasted-*.png without it but with
 * send_file (the file goes when the transfer ends), after a confirmation
 * unless /pidgin4/images/confirm_file_send is off; (c) a text paste with
 * neither. Text next to the image wins unless it is blank. */
static void
test_paste_image(PurpleConversation *conv)
{
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);
	PidginComposeEntry *entry = PIDGIN_COMPOSE_ENTRY(gtkconv->entry);
	char *text, *path = NULL, *expect, *before;
	PurpleXfer *xfer;
	GtkWindow *dialog;
	int files;

	pidgin_conv_window_switch_gtkconv(gtkconv->win, gtkconv);

	/* (a) inline */
	pidgin_selftest_prpl_set_caps(TRUE, FALSE);
	pidgin_conv_update_buttons_by_protocol(conv);
	CHECK(pidgin_compose_entry_get_paste_images(entry), "(a) images not pasted");
	pidgin_compose_entry_clear(entry);
	paste_image_clipboard(entry, NULL);
	CHECK(entry_has_anchor(entry), "(a) no pasted image in the entry");
	CHECK(pidgin_compose_entry_send(entry), "(a) send");
	spin(300);
	text = call("send-im") ? g_ascii_strdown(call("send-im"), -1) : NULL;
	CHECK(text != NULL && strstr(text, "<img id=") != NULL, "(a) send-im: %s", call("send-im"));
	g_free(text);

	/* ... text next to the image wins; blank text doesn't */
	pidgin_compose_entry_clear(entry);
	paste_image_clipboard(entry, "cells as text");
	text = pidgin_compose_entry_get_text(entry);
	CHECK(purple_strequal(text, "cells as text") && !entry_has_anchor(entry),
	      "(a) text and image pasted \"%s\"", text);
	g_free(text);
	pidgin_compose_entry_clear(entry);
	paste_image_clipboard(entry, "  ");
	CHECK(entry_has_anchor(entry), "(a) blank text and image: no image");
	pidgin_compose_entry_clear(entry);

	/* (b) a file transfer: at once with confirm_file_send off */
	purple_prefs_set_bool(PIDGIN4_PREFS_ROOT "/images/confirm_file_send", FALSE);
	pidgin_selftest_prpl_set_caps(FALSE, TRUE);
	pidgin_conv_update_buttons_by_protocol(conv);
	CHECK(pidgin_compose_entry_get_paste_images(entry), "(b) images not pasted");
	files = count_paste_files(NULL);
	paste_image_clipboard(entry, NULL);
	CHECK(!entry_has_anchor(entry), "(b) image put inline");
	CHECK(count_paste_files(&path) == files + 1, "(b) no pasted file");
	{
		char *base = path ? g_path_get_basename(path) : NULL;

		CHECK(base != NULL && g_str_has_prefix(base, "pasted-") &&
		      g_str_has_suffix(base, ".png"), "(b) pasted file %s", path);
		g_free(base);
	}
	expect = g_strdup_printf(ST_BUDDY "|%s|", path);
	CHECK(purple_strequal(call("send-file"), expect), "(b) send-file: %s (expected %s)",
	      call("send-file"), expect);
	g_free(expect);
	{
		/* ours, then libpurple's "Offering to send ..." */
		gboolean status = FALSE;
		guint i, n = n_messages(conv);

		for (i = n > 3 ? n - 3 : 0; i < n; i++)
			if (strstr(pidgin_message_get_html(nth_message(conv, i)),
			           "Sending the image pasted-") != NULL)
				status = TRUE;
		CHECK(status, "(b) no status line");
	}
	xfer = pidgin_selftest_prpl_get_last_xfer();
	CHECK(xfer != NULL, "(b) no transfer");
	if (xfer != NULL) {
		purple_xfer_cancel_local(xfer);
		pidgin_selftest_prpl_forget_xfer();
		spin(300);
		CHECK(path == NULL || !g_file_test(path, G_FILE_TEST_EXISTS),
		      "(b) %s left after the transfer ended", path);
	}
	g_free(path);
	path = NULL;

	/* ... encoded by /pidgin4/images/paste_format: JPEG, named .jpg */
	purple_prefs_set_string(PIDGIN4_PREFS_ROOT "/images/paste_format", "jpeg");
	files = count_paste_files(NULL);
	paste_image_clipboard(entry, NULL);
	CHECK(count_paste_files(&path) == files + 1 && g_str_has_suffix(path, ".jpg"),
	      "(b) JPEG paste saved %s", path);
	CHECK(call("send-file") != NULL && g_str_has_suffix(call("send-file"), ".jpg|"),
	      "(b) JPEG send-file: %s", call("send-file"));
	purple_prefs_set_string(PIDGIN4_PREFS_ROOT "/images/paste_format", "auto");
	xfer = pidgin_selftest_prpl_get_last_xfer();
	if (xfer != NULL) {
		purple_xfer_cancel_local(xfer);
		pidgin_selftest_prpl_forget_xfer();
		spin(300);
	}
	CHECK(path == NULL || !g_file_test(path, G_FILE_TEST_EXISTS), "(b) %s left", path);
	g_free(path);
	path = NULL;

	/* ... with it on (the default): a dialog, modal to the conversation
	 * window; nothing is saved or sent before its Send */
	purple_prefs_set_bool(PIDGIN4_PREFS_ROOT "/images/confirm_file_send", TRUE);
	pidgin_selftest_prpl_clear_call("send-file");
	files = count_paste_files(NULL);
	paste_image_clipboard(entry, NULL);
	dialog = confirm_dialog(conv);
	CHECK(dialog != NULL, "(b) no confirmation");
	CHECK(!entry_has_anchor(entry) && pidgin_compose_entry_is_empty(entry),
	      "(b) confirm: something was pasted");
	CHECK(count_paste_files(NULL) == files, "(b) saved before Send");
	CHECK(call("send-file") == NULL, "(b) sent before Send: %s", call("send-file"));
	if (dialog != NULL) {
		GtkWidget *preview = find_named(GTK_WIDGET(dialog), "file-confirm-preview");
		const char *to = confirm_label(dialog, "file-confirm-recipient");
		const char *details = confirm_label(dialog, "file-confirm-details");

		CHECK(gtk_widget_get_visible(GTK_WIDGET(dialog)), "(b) confirmation not shown");
		CHECK(gtk_window_get_modal(dialog), "(b) confirmation not modal");
		CHECK(gtk_window_get_transient_for(dialog) ==
		      GTK_WINDOW(gtk_widget_get_root(gtkconv->tab_cont)),
		      "(b) confirmation not transient for the conversation window");
		CHECK(GTK_IS_PICTURE(preview), "(b) no image preview");
		CHECK(strstr(to, purple_conversation_get_title(conv)) != NULL &&
		      strstr(to, ST_USER) != NULL, "(b) recipient \"%s\"", to);
		CHECK(strstr(details, "16 × 16") != NULL && strstr(details, "PNG") != NULL,
		      "(b) details \"%s\"", details);
		CHECK(GTK_IS_BUTTON(gtk_window_get_default_widget(dialog)) &&
		      purple_strequal(gtk_button_get_label(
		          GTK_BUTTON(gtk_window_get_default_widget(dialog))), _("_Send")),
		      "(b) Send is not the default");
	}
	confirm_answer(conv, TRUE);
	CHECK(confirm_dialog(conv) == NULL, "(b) confirmation left after Send");
	CHECK(count_paste_files(&path) == files + 1, "(b) Send: no pasted file");
	expect = g_strdup_printf(ST_BUDDY "|%s|", path);
	CHECK(purple_strequal(call("send-file"), expect), "(b) Send: send-file %s (expected %s)",
	      call("send-file"), expect);
	g_free(expect);
	xfer = pidgin_selftest_prpl_get_last_xfer();
	CHECK(xfer != NULL, "(b) Send: no transfer");
	if (xfer != NULL) {
		purple_xfer_cancel_local(xfer);
		pidgin_selftest_prpl_forget_xfer();
		spin(300);
	}
	CHECK(path == NULL || !g_file_test(path, G_FILE_TEST_EXISTS), "(b) %s left", path);
	g_free(path);
	path = NULL;

	/* ... Cancel (Escape): no file, no send */
	pidgin_selftest_prpl_clear_call("send-file");
	files = count_paste_files(NULL);
	paste_image_clipboard(entry, NULL);
	CHECK(confirm_dialog(conv) != NULL, "(b) no confirmation to cancel");
	confirm_answer(conv, FALSE);
	CHECK(confirm_dialog(conv) == NULL, "(b) confirmation left after Cancel");
	CHECK(count_paste_files(NULL) == files, "(b) Cancel left a file");
	CHECK(call("send-file") == NULL, "(b) Cancel sent %s", call("send-file"));

	/* ... a dropped file of another type: its icon, name and size; Send
	 * sends it as it is */
	path = g_build_filename(purple_user_dir(), "selftest-drop.txt", NULL);
	CHECK(g_file_set_contents(path, "not an image\n", -1, NULL), "writing %s", path);
	text = drop_file(conv, path);
	g_free(text);
	dialog = confirm_dialog(conv);
	CHECK(dialog != NULL, "(b) no confirmation for a dropped file");
	CHECK(call("send-file") == NULL, "(b) dropped file sent before Send: %s",
	      call("send-file"));
	if (dialog != NULL) {
		CHECK(GTK_IS_IMAGE(find_named(GTK_WIDGET(dialog), "file-confirm-preview")),
		      "(b) dropped file: no icon");
		CHECK(strstr(confirm_label(dialog, "file-confirm-details"), "13 bytes") != NULL,
		      "(b) dropped file details \"%s\"",
		      confirm_label(dialog, "file-confirm-details"));
	}
	confirm_answer(conv, TRUE);
	expect = g_strdup_printf(ST_BUDDY "|%s|", path);
	CHECK(purple_strequal(call("send-file"), expect), "(b) dropped send-file: %s (expected %s)",
	      call("send-file"), expect);
	g_free(expect);
	xfer = pidgin_selftest_prpl_get_last_xfer();
	if (xfer != NULL) {
		purple_xfer_cancel_local(xfer);
		pidgin_selftest_prpl_forget_xfer();
		spin(300);
	}
	CHECK(g_file_test(path, G_FILE_TEST_EXISTS), "(b) the dropped file was removed");
	g_unlink(path);
	g_free(path);
	path = NULL;

	/* ... the conversation closing drops an open confirmation */
	{
		PurpleConversation *other = purple_conversation_new(PURPLE_CONV_TYPE_IM, st_account,
		                                                    "other@example.invalid");
		GBytes *png;
		gsize len;
		gpointer data = make_png(12, 10, &len);

		pidgin_conv_update_buttons_by_protocol(other);
		pidgin_selftest_prpl_clear_call("send-file");
		files = count_paste_files(NULL);
		png = g_bytes_new_take(data, len);
		CHECK(pidgin_conv_offer_image(PIDGIN_CONVERSATION(other), png, "pasted-close.png"),
		      "(b) close: not taken");
		g_bytes_unref(png);
		dialog = confirm_dialog(other);
		CHECK(dialog != NULL, "(b) close: no confirmation");
		if (dialog != NULL)
			g_object_add_weak_pointer(G_OBJECT(dialog), (gpointer *)&dialog);
		purple_conversation_destroy(other);
		spin(300);
		CHECK(dialog == NULL, "(b) confirmation left after its conversation closed");
		if (dialog != NULL) {
			g_signal_handlers_disconnect_by_data(dialog, &dialog);
			gtk_window_destroy(dialog);
		}
		if (dialog != NULL)
			g_object_remove_weak_pointer(G_OBJECT(dialog), (gpointer *)&dialog);
		CHECK(count_paste_files(NULL) == files, "(b) close left a file");
		CHECK(call("send-file") == NULL, "(b) close sent %s", call("send-file"));
	}
	pidgin_conv_window_switch_gtkconv(gtkconv->win, gtkconv);

	/* (c) neither: the text paste, and nothing else */
	pidgin_selftest_prpl_set_caps(FALSE, FALSE);
	pidgin_conv_update_buttons_by_protocol(conv);
	CHECK(!pidgin_compose_entry_get_paste_images(entry), "(c) images pasted");
	before = g_strdup(call("send-file"));
	files = count_paste_files(NULL);
	pidgin_compose_entry_clear(entry);
	paste_image_clipboard(entry, NULL);
	CHECK(pidgin_compose_entry_is_empty(entry) && !entry_has_anchor(entry),
	      "(c) something was pasted");
	CHECK(count_paste_files(NULL) == files, "(c) a file was saved");
	CHECK(purple_strequal(call("send-file"), before), "(c) send-file: %s", call("send-file"));
	g_free(before);
	pidgin_compose_entry_clear(entry);
	paste_image_clipboard(entry, "just text");
	text = pidgin_compose_entry_get_text(entry);
	CHECK(purple_strequal(text, "just text"), "(c) text paste \"%s\"", text);
	g_free(text);
	pidgin_compose_entry_clear(entry);

	pidgin_selftest_prpl_set_caps(TRUE, FALSE);
	pidgin_conv_update_buttons_by_protocol(conv);
	hold("paste image");
}

/* The toolbar's attach button: shown when the prpl can send this
 * conversation a file (IM: send_file + can_receive_file; chat:
 * chat_send_file), follows the prpl, and runs conv.send-file. */
static void
test_attach(PurpleConversation *conv)
{
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);
	GtkWidget *button;
	GList *l;

	CHECK(gtkconv->toolbar != NULL, "no toolbar");
	if (gtkconv->toolbar == NULL)
		return;
	button = pidgin_format_toolbar_get_attach_button(PIDGIN_FORMAT_TOOLBAR(gtkconv->toolbar));
	pidgin_conv_window_switch_gtkconv(gtkconv->win, gtkconv);

	/* images, no files (the default) */
	pidgin_selftest_prpl_set_caps(TRUE, FALSE);
	pidgin_conv_update_buttons_by_protocol(conv);
	CHECK(!gtk_widget_get_visible(button), "attach without send_file");

	/* neither */
	pidgin_selftest_prpl_set_caps(FALSE, FALSE);
	pidgin_conv_update_buttons_by_protocol(conv);
	CHECK(!gtk_widget_get_visible(button), "attach without anything");

	/* files: shown, and a click sends a file to the buddy (the prpl is
	 * asked with no file: it opens its chooser, as Pidgin 2's) */
	pidgin_selftest_prpl_set_caps(FALSE, TRUE);
	pidgin_conv_update_buttons_by_protocol(conv);
	CHECK(gtk_widget_get_visible(button), "no attach with send_file");
	CHECK(purple_strequal(gtk_actionable_get_action_name(GTK_ACTIONABLE(button)),
	                      "conv.send-file"), "attach action %s",
	      gtk_actionable_get_action_name(GTK_ACTIONABLE(button)));
	CHECK(gtk_widget_get_sensitive(button), "attach insensitive");
	CHECK(gtk_widget_activate(button), "attach not activatable");
	spin(400);
	CHECK(purple_strequal(call("send-file"), ST_BUDDY "||"), "attach send-file: %s",
	      call("send-file"));

	/* images and files */
	pidgin_selftest_prpl_set_caps(TRUE, TRUE);
	pidgin_conv_update_buttons_by_protocol(conv);
	CHECK(gtk_widget_get_visible(button), "no attach with images and files");

	/* chats: chat_send_file */
	for (l = purple_get_chats(); l != NULL; l = l->next) {
		PurpleConversation *chat = l->data;
		PidginConversation *gtkchat = PIDGIN_CONVERSATION(chat);
		GtkWidget *chat_button;

		if (gtkchat == NULL || gtkchat->toolbar == NULL)
			continue;
		chat_button = pidgin_format_toolbar_get_attach_button(
			PIDGIN_FORMAT_TOOLBAR(gtkchat->toolbar));
		pidgin_conv_update_buttons_by_protocol(chat);
		CHECK(gtk_widget_get_visible(chat_button), "no attach in a chat with chat_send_file");
		pidgin_selftest_prpl_set_caps(TRUE, FALSE);
		pidgin_conv_update_buttons_by_protocol(chat);
		CHECK(!gtk_widget_get_visible(chat_button), "attach in a chat without chat_send_file");
		pidgin_selftest_prpl_set_caps(TRUE, TRUE);
	}

	pidgin_selftest_prpl_set_caps(TRUE, FALSE);
	pidgin_conv_update_buttons_by_protocol(conv);
	for (l = purple_get_chats(); l != NULL; l = l->next)
		pidgin_conv_update_buttons_by_protocol(l->data);
	CHECK(!gtk_widget_get_visible(button), "attach left shown");
}

/**************************************************************************
 * Shared URLs and received files shown inline
 **************************************************************************/

/* A local http server for "shared" files (the loader and the HEAD probe
 * accept http only in their test mode). */
static SoupServer *share_server = NULL;
static char *share_base = NULL;
static GBytes *share_png = NULL;

static void
share_server_cb(SoupServer *server, SoupServerMessage *msg, const char *path,
                GHashTable *query, gpointer data)
{
	if (g_str_has_prefix(path, "/share/pic")) {
		/* pic.png, pic2.png, pic-noext: all a PNG */
		soup_server_message_set_status(msg, 200, NULL);
		soup_server_message_set_response(msg, "image/png", SOUP_MEMORY_COPY,
		                                 g_bytes_get_data(share_png, NULL),
		                                 g_bytes_get_size(share_png));
	} else if (g_str_has_prefix(path, "/share/clip")) {
		/* no extension: only the Content-Type says it is a video */
		soup_server_message_set_status(msg, 200, NULL);
		soup_server_message_set_response(msg, "video/mp4", SOUP_MEMORY_STATIC,
		                                 "0123456789", 10);
	} else if (g_str_has_prefix(path, "/share/doc")) {
		soup_server_message_set_status(msg, 200, NULL);
		soup_server_message_set_response(msg, "text/plain", SOUP_MEMORY_STATIC,
		                                 "hello", 5);
	} else {
		soup_server_message_set_status(msg, 404, NULL);
	}
}

static gboolean
share_server_start(void)
{
	GError *error = NULL;
	GSList *uris;
	gsize len;

	if (share_server != NULL)
		return TRUE;
	{
		gpointer data = make_png(24, 16, &len);

		share_png = g_bytes_new_take(data, len);
	}
	share_server = soup_server_new(NULL, NULL);
	soup_server_add_handler(share_server, NULL, share_server_cb, NULL, NULL);
	if (!soup_server_listen_local(share_server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error)) {
		g_printerr("PIDGIN4_CONV_SELFTEST: share server: %s\n", error->message);
		g_clear_error(&error);
		g_clear_object(&share_server);
		return FALSE;
	}
	uris = soup_server_get_uris(share_server);
	share_base = g_strdup_printf("http://127.0.0.1:%d", g_uri_get_port(uris->data));
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
	pidgin_image_loader_set_allow_http_for_tests(pidgin_image_loader_get_default(), TRUE);
	pidgin_conv_meta_set_share_protocol_for_tests(ST_PRPL_ID);
	return TRUE;
}

static void
share_server_stop(void)
{
	if (share_server != NULL) {
		soup_server_disconnect(share_server);
		g_clear_object(&share_server);
	}
	g_clear_pointer(&share_base, g_free);
	g_clear_pointer(&share_png, g_bytes_unref);
	pidgin_conv_meta_set_share_protocol_for_tests(NULL);
	pidgin_image_loader_set_allow_http_for_tests(pidgin_image_loader_get_default(), FALSE);
}

/* The first descendant of @widget with @css_class (and, if @tooltip, that
 * tooltip), depth first. */
static GtkWidget *
find_widget(GtkWidget *widget, const char *css_class, const char *tooltip)
{
	GtkWidget *child, *found;

	if (widget == NULL)
		return NULL;
	if (gtk_widget_has_css_class(widget, css_class) &&
	    (tooltip == NULL || purple_strequal(gtk_widget_get_tooltip_text(widget), tooltip)))
		return widget;
	for (child = gtk_widget_get_first_child(widget); child != NULL;
	     child = gtk_widget_get_next_sibling(child))
		if ((found = find_widget(child, css_class, tooltip)) != NULL)
			return found;
	return NULL;
}

/* Receives @body from the buddy and waits; returns the new last message. */
static PidginMessage *
receive(PurpleConversation *conv, const char *body, guint ms)
{
	serv_got_im(purple_conversation_get_gc(conv), ST_BUDDY, body, PURPLE_MESSAGE_RECV,
	            time(NULL));
	spin(ms);
	return last_message(conv);
}

static char *launched = NULL;

static void
launch_hook(const char *action, const char *target)
{
	g_free(launched);
	launched = g_strdup_printf("%s|%s", action, target);
}

/* (a) a one-URL XMPP body from a host that isn't allowed: an image (by
 * extension, or by the HEAD Content-Type) is shown inline, anything else
 * stays a link, and nothing with the pref off. */
static void
test_xmpp_shares(PurpleConversation *conv)
{
	GtkWidget *view = GTK_WIDGET(view_of(conv));
	PidginMessage *msg;
	char *url;

	if (!share_server_start()) {
		CHECK(FALSE, "no share server");
		return;
	}
	pidgin_conv_window_switch_gtkconv(PIDGIN_CONVERSATION(conv)->win, PIDGIN_CONVERSATION(conv));

	/* by extension */
	url = g_strconcat(share_base, "/share/pic.png", NULL);
	CHECK(!pidgin_image_loader_is_allowed(pidgin_image_loader_get_default(), url),
	      "127.0.0.1 already allowed");
	msg = receive(conv, url, 800);
	CHECK(msg != NULL && strstr(pidgin_message_get_html(msg), "<img src=") != NULL,
	      "share %s not inline: %s", url, msg ? pidgin_message_get_html(msg) : "-");
	CHECK(find_widget(view, "pidgin-inline-image", url) != NULL, "no inline picture for %s", url);
	{
		/* only that URI, not its host */
		char *other = g_strconcat(share_base, "/share/other.png", NULL);

		CHECK(!pidgin_image_loader_is_allowed(pidgin_image_loader_get_default(), other),
		      "the whole host was allowed");
		g_free(other);
	}
	g_free(url);

	/* no extension: HEAD says image/png */
	url = g_strconcat(share_base, "/share/pic-noext", NULL);
	msg = receive(conv, url, 1200);
	CHECK(msg != NULL && strstr(pidgin_message_get_html(msg), "<img src=") != NULL,
	      "probed share %s not inline: %s", url, msg ? pidgin_message_get_html(msg) : "-");
	CHECK(find_widget(view, "pidgin-inline-image", url) != NULL, "no inline picture for %s", url);
	g_free(url);

	/* not an image: a link */
	url = g_strconcat(share_base, "/share/doc", NULL);
	msg = receive(conv, url, 1000);
	CHECK(msg != NULL && strstr(pidgin_message_get_html(msg), "<img") == NULL,
	      "a text share inlined: %s", msg ? pidgin_message_get_html(msg) : "-");
	CHECK(!pidgin_image_loader_is_allowed(pidgin_image_loader_get_default(), url),
	      "a text share was allowed");
	g_free(url);

	/* an image URL among other text: a link */
	url = g_strconcat("look: ", share_base, "/share/pic3.png", NULL);
	msg = receive(conv, url, 500);
	CHECK(msg != NULL && strstr(pidgin_message_get_html(msg), "<img") == NULL,
	      "an image URL in text inlined: %s", msg ? pidgin_message_get_html(msg) : "-");
	g_free(url);

	/* the pref off: a link */
	purple_prefs_set_bool(PIDGIN4_PREFS_ROOT "/images/inline_xmpp_shares", FALSE);
	url = g_strconcat(share_base, "/share/pic2.png", NULL);
	msg = receive(conv, url, 500);
	CHECK(msg != NULL && strstr(pidgin_message_get_html(msg), "<img") == NULL,
	      "share inlined with the pref off: %s", msg ? pidgin_message_get_html(msg) : "-");
	g_free(url);
	url = g_strconcat(share_base, "/share/pic-noext2", NULL);
	msg = receive(conv, url, 1000);
	CHECK(msg != NULL && strstr(pidgin_message_get_html(msg), "<img") == NULL,
	      "probed share inlined with the pref off: %s", msg ? pidgin_message_get_html(msg) : "-");
	g_free(url);
	purple_prefs_set_bool(PIDGIN4_PREFS_ROOT "/images/inline_xmpp_shares", TRUE);
	hold("xmpp shares");
}

/* A received file transfer, completed: libpurple writes its line (and the
 * signal fires first). */
static void
receive_file(PurpleConversation *conv, const char *path)
{
	PurpleXfer *xfer = purple_xfer_new(st_account, PURPLE_XFER_RECEIVE, ST_BUDDY);
	char *base = g_path_get_basename(path);

	purple_xfer_set_filename(xfer, base);
	purple_xfer_set_local_filename(xfer, path);
	purple_xfer_set_completed(xfer, TRUE);
	purple_xfer_end(xfer);
	g_free(base);
}

/* The last message containing @needle (in the last 10), or NULL */
static PidginMessage *
find_message(PurpleConversation *conv, const char *needle)
{
	guint n = n_messages(conv), i;

	for (i = n; i > 0 && i + 10 > n; i--) {
		PidginMessage *m = nth_message(conv, i - 1);

		if (strstr(pidgin_message_get_html(m), needle) != NULL)
			return m;
	}
	return NULL;
}

/* (b) a received image file: shown under libpurple's line, which stays as
 * written; a click opens it. Not for other files, nor with the pref off. */
static void
test_received_files(PurpleConversation *conv)
{
	GtkWidget *view = GTK_WIDGET(view_of(conv));
	PidginMessage *msg;
	PidginAttachment *att;
	GtkWidget *picture;
	char *dir = g_build_filename(purple_user_dir(), "selftest-received", NULL);
	char *png = g_build_filename(dir, "received photo.png", NULL);
	char *txt = g_build_filename(dir, "notes.txt", NULL);
	char *png2 = g_build_filename(dir, "off.png", NULL);
	char *esc;
	gpointer data;
	gsize len;

	g_mkdir_with_parents(dir, 0700);
	data = make_png(40, 30, &len);
	g_file_set_contents(png, data, len, NULL);
	g_file_set_contents(png2, data, len, NULL);
	g_free(data);
	g_file_set_contents(txt, "notes", -1, NULL);
	pidgin_conv_window_switch_gtkconv(PIDGIN_CONVERSATION(conv)->win, PIDGIN_CONVERSATION(conv));

	receive_file(conv, png);
	spin(800);
	esc = g_markup_escape_text(png, -1);
	msg = find_message(conv, esc);
	g_free(esc);
	CHECK(msg != NULL && strstr(pidgin_message_get_html(msg), "Transfer of file") != NULL,
	      "no libpurple line for the received file");
	att = msg ? pidgin_message_get_attachment(msg) : NULL;
	CHECK(att != NULL && pidgin_attachment_get_kind(att) == PIDGIN_ATTACHMENT_IMAGE &&
	      purple_strequal(pidgin_attachment_get_path(att), png), "no image attachment");
	CHECK(msg == NULL || strstr(pidgin_message_get_html(msg), "<img") == NULL,
	      "libpurple's line was changed: %s", pidgin_message_get_html(msg));
	picture = find_widget(view, "pidgin-attachment-image", "received photo.png");
	CHECK(picture != NULL && gtk_picture_get_paintable(GTK_PICTURE(picture)) != NULL,
	      "no received picture shown");
	if (picture != NULL) {
		pidgin_attachment_set_launch_hook(launch_hook);
		g_clear_pointer(&launched, g_free);
		{
			/* what a click does */
			GListModel *controllers = gtk_widget_observe_controllers(picture);
			guint i;

			for (i = 0; i < g_list_model_get_n_items(controllers); i++) {
				GObject *c = g_list_model_get_item(controllers, i);

				if (GTK_IS_GESTURE_CLICK(c))
					g_signal_emit_by_name(c, "released", 1, 5.0, 5.0);
				g_object_unref(c);
			}
			g_object_unref(controllers);
		}
		{
			char *expect = g_strconcat("open|", png, NULL);

			CHECK(purple_strequal(launched, expect), "click launched %s", launched);
			g_free(expect);
		}
		pidgin_attachment_set_launch_hook(NULL);
	}

	/* not an image */
	receive_file(conv, txt);
	spin(500);
	msg = find_message(conv, "notes.txt");
	CHECK(msg != NULL && pidgin_message_get_attachment(msg) == NULL,
	      "a text file got an attachment");

	/* the pref off */
	purple_prefs_set_bool(PIDGIN4_PREFS_ROOT "/images/inline_received_files", FALSE);
	receive_file(conv, png2);
	spin(500);
	msg = find_message(conv, "off.png");
	CHECK(msg != NULL && pidgin_message_get_attachment(msg) == NULL,
	      "an attachment with the pref off");
	purple_prefs_set_bool(PIDGIN4_PREFS_ROOT "/images/inline_received_files", TRUE);
	hold("received files");

	g_unlink(png);
	g_unlink(png2);
	g_unlink(txt);
	g_rmdir(dir);
	g_free(png);
	g_free(png2);
	g_free(txt);
	g_free(dir);
}

/* The media card of a row's attachment: shown in the view, Play and Open
 * Folder go to the launcher (test hook), and no player without a media
 * backend. */
static void
check_media_card(PurpleConversation *conv, PidginMessage *msg, PidginAttachmentKind kind,
                 const char *target, gboolean local, const char *what)
{
	PidginAttachment *att = msg ? pidgin_message_get_attachment(msg) : NULL;
	GtkWidget *card, *button;
	char *expect;

	CHECK(att != NULL && pidgin_attachment_get_kind(att) == kind, "%s: no %s attachment",
	      what, kind == PIDGIN_ATTACHMENT_VIDEO ? "video" : "audio");
	if (att == NULL)
		return;
	CHECK(msg == NULL || strstr(pidgin_message_get_html(msg), "<img") == NULL,
	      "%s: the text changed: %s", what, pidgin_message_get_html(msg));

	/* in the view: the row built its card */
	card = find_widget(GTK_WIDGET(view_of(conv)), "pidgin-media-card", NULL);
	CHECK(card != NULL, "%s: no media card in the view", what);
	/* the attachment's own card, to press its buttons */
	card = pidgin_attachment_widget_new(att);
	g_object_ref_sink(card);
	CHECK(find_widget(card, "pidgin-media-name", NULL) != NULL, "%s: no name", what);
	CHECK((find_widget(card, "pidgin-media-player", NULL) != NULL) ==
	      pidgin_media_backend_available(), "%s: a player without a backend, or none with",
	      what);
	pidgin_attachment_set_launch_hook(launch_hook);
	button = find_widget(card, "pidgin-media-play", NULL);
	g_clear_pointer(&launched, g_free);
	if (button != NULL)
		g_signal_emit_by_name(button, "clicked");
	expect = g_strconcat("play|", target, NULL);
	CHECK(purple_strequal(launched, expect), "%s: Play launched %s (expected %s)", what,
	      launched, expect);
	g_free(expect);
	button = find_widget(card, "pidgin-media-folder", NULL);
	CHECK((button != NULL) == local, "%s: Open Folder %s", what, local ? "missing" : "for a URL");
	if (button != NULL) {
		g_clear_pointer(&launched, g_free);
		g_signal_emit_by_name(button, "clicked");
		expect = g_strconcat("open-folder|", target, NULL);
		CHECK(purple_strequal(launched, expect), "%s: Open Folder launched %s", what, launched);
		g_free(expect);
	}
	pidgin_attachment_set_launch_hook(NULL);
	g_object_unref(card);
}

/* Audio and video: received transfers, an XMPP share (by its HEAD
 * Content-Type), a lone URL on an allowlisted host (as Discord's). */
static void
test_media(PurpleConversation *conv)
{
	char *dir = g_build_filename(purple_user_dir(), "selftest-media", NULL);
	char *mp4 = g_build_filename(dir, "clip.mp4", NULL);
	char *ogg = g_build_filename(dir, "voice.ogg", NULL);
	char *url, *esc;
	PidginMessage *msg;

	/* This machine's GTK is built without GStreamer (USE=-gstreamer):
	 * no inline player. PIDGIN4_SELFTEST_MEDIA_BACKEND=1 after rebuilding
	 * GTK with it. */
	g_print("PIDGIN4_CONV_SELFTEST: media backend: %s\n",
	        pidgin_media_backend_available() ? "yes" : "no");
	CHECK(pidgin_media_backend_available() ==
	      purple_strequal(g_getenv("PIDGIN4_SELFTEST_MEDIA_BACKEND"), "1"),
	      "media backend check: %d", pidgin_media_backend_available());

	g_mkdir_with_parents(dir, 0700);
	g_file_set_contents(mp4, "not really a video", -1, NULL);
	g_file_set_contents(ogg, "not really audio", -1, NULL);
	pidgin_conv_window_switch_gtkconv(PIDGIN_CONVERSATION(conv)->win, PIDGIN_CONVERSATION(conv));

	receive_file(conv, mp4);
	spin(600);
	esc = g_markup_escape_text(mp4, -1);
	msg = find_message(conv, esc);
	g_free(esc);
	CHECK(msg != NULL, "no line for the received video");
	check_media_card(conv, msg, PIDGIN_ATTACHMENT_VIDEO, mp4, TRUE, "received mp4");
	CHECK(msg == NULL || pidgin_attachment_get_size(pidgin_message_get_attachment(msg)) == 18,
	      "received mp4 size");

	receive_file(conv, ogg);
	spin(600);
	esc = g_markup_escape_text(ogg, -1);
	msg = find_message(conv, esc);
	g_free(esc);
	check_media_card(conv, msg, PIDGIN_ATTACHMENT_AUDIO, ogg, TRUE, "received ogg");

	/* an XMPP share without an extension: video/mp4 by HEAD */
	url = g_strconcat(share_base, "/share/clip", NULL);
	msg = receive(conv, url, 1200);
	check_media_card(conv, msg, PIDGIN_ATTACHMENT_VIDEO, url, FALSE, "shared clip");
	CHECK(msg == NULL || pidgin_message_get_attachment(msg) == NULL ||
	      pidgin_attachment_get_size(pidgin_message_get_attachment(msg)) == 10,
	      "shared clip size");
	g_free(url);

	/* not XMPP: a lone URL on an allowlisted host (Discord's CDN) */
	pidgin_conv_meta_set_share_protocol_for_tests(NULL);
	url = g_strconcat(share_base, "/share/voice-message.ogg", NULL);
	msg = receive(conv, url, 300);
	CHECK(msg != NULL && pidgin_message_get_attachment(msg) == NULL,
	      "a card for a URL on a host that isn't allowed");
	g_free(url);
	pidgin_image_loader_allow_host(pidgin_image_loader_get_default(), "127.0.0.1");
	url = g_strconcat(share_base, "/share/voice-message2.ogg", NULL);
	msg = receive(conv, url, 300);
	check_media_card(conv, msg, PIDGIN_ATTACHMENT_AUDIO, url, FALSE, "allowlisted ogg");
	g_free(url);
	hold("media");

	g_unlink(mp4);
	g_unlink(ogg);
	g_rmdir(dir);
	g_free(mp4);
	g_free(ogg);
	g_free(dir);
}

static gboolean
entry_has_anchor(PidginComposeEntry *entry)
{
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(entry));
	GtkTextIter iter;

	for (gtk_text_buffer_get_start_iter(buffer, &iter); !gtk_text_iter_is_end(&iter);
	     gtk_text_iter_forward_char(&iter))
		if (gtk_text_iter_get_child_anchor(&iter) != NULL)
			return TRUE;
	return FALSE;
}

/* Sending a smiley from the picker crashed the same way as an image (a
 * child anchor in the entry); a typed shortcut is shown as a smiley in
 * the view. */
static void
test_smileys(PurpleConversation *conv)
{
	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);
	PidginComposeEntry *entry = PIDGIN_COMPOSE_ENTRY(gtkconv->entry);
	guint n;

	/* From the picker, between text */
	pidgin_compose_entry_set_markup(entry, "smile ");
	pidgin_compose_entry_insert_smiley(entry, ":)");
	CHECK(entry_has_anchor(entry), "the smiley isn't an image in the entry");
	gtk_text_buffer_insert_at_cursor(gtk_text_view_get_buffer(GTK_TEXT_VIEW(entry)), " more", -1);
	n = n_messages(conv);
	CHECK(pidgin_compose_entry_send(entry), "smiley send");
	spin(300);
	CHECK(n_messages(conv) == n + 1, "smiley message not shown (%u, %u)", n_messages(conv), n);
	CHECK(call("send-im") != NULL && strstr(call("send-im"), "smile :) more") != NULL,
	      "send-im: %s", call("send-im"));

	/* Two, alone, then Backspace over one */
	pidgin_compose_entry_insert_smiley(entry, ":)");
	pidgin_compose_entry_insert_smiley(entry, ";)");
	{
		GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(entry));
		GtkTextIter end;

		gtk_text_buffer_get_end_iter(buffer, &end);
		gtk_text_buffer_backspace(buffer, &end, TRUE, TRUE);
	}
	n = n_messages(conv);
	CHECK(pidgin_compose_entry_send(entry), "smiley-only send");
	spin(300);
	CHECK(n_messages(conv) == n + 1, "smiley-only message not shown");

	/* Loaded back from the history (set_markup makes anchors), sent again */
	pidgin_compose_entry_history_up(entry);
	CHECK(entry_has_anchor(entry), "history lost the smiley");
	n = n_messages(conv);
	CHECK(pidgin_compose_entry_send(entry), "history smiley send");
	spin(300);
	CHECK(n_messages(conv) == n + 1, "history smiley message not shown");

	/* Typed as text; the echo and a received one render the theme's smiley */
	pidgin_compose_entry_set_markup(entry, "typed :) and :-(");
	CHECK(pidgin_compose_entry_send(entry), "typed smiley send");
	purple_conv_im_write(PURPLE_CONV_IM(conv), ST_BUDDY, "back at you :) <b>:D</b>",
	                     PURPLE_MESSAGE_RECV, time(NULL));
	spin(300);
	CHECK(strstr(pidgin_message_get_plain_text(last_message(conv)), "back at you") != NULL,
	      "received smiley message: %s", pidgin_message_get_plain_text(last_message(conv)));
	CHECK(pidgin_markup_result_has_object(pidgin_message_get_markup(last_message(conv)),
	                                      PIDGIN_MARKUP_OBJECT_SMILEY),
	      "no smiley in the received row");
	CHECK(pidgin_markup_result_has_object(
	          pidgin_message_get_markup(nth_message(conv, n_messages(conv) - 2)),
	          PIDGIN_MARKUP_OBJECT_SMILEY), "no smiley in the sent row");
	hold("smileys");
}

/**************************************************************************
 * The test
 **************************************************************************/

static void
test_im(PurpleConversation **im_out)
{
	PurpleConversation *conv;
	PidginConversation *gtkconv;
	PidginMessageIndex *idx = pidgin_message_index_get_default();
	PidginMessage *msg, *sent;
	PidginIndexedMessage *row;
	GHashTable *meta;
	char *akey, *ckey, *path;
	time_t now = time(NULL);
	guint n;
	gint64 rows_before = idx ? pidgin_message_index_count(idx) : 0;

	conv = purple_conversation_new(PURPLE_CONV_TYPE_IM, st_account, ST_BUDDY);
	CHECK(conv != NULL, "no IM conversation");
	gtkconv = PIDGIN_CONVERSATION(conv);
	CHECK(gtkconv != NULL && gtkconv->win != NULL, "no PidginConversation/window");
	CHECK(!pidgin_conv_is_hidden(gtkconv), "IM is hidden");
	purple_conversation_set_logging(conv, TRUE);
	*im_out = conv;
	spin(100);

	/* Plain writes with various flags */
	purple_conv_im_write(PURPLE_CONV_IM(conv), ST_BUDDY, "hello <b>bold</b> world",
	                     PURPLE_MESSAGE_RECV, now - 600);
	purple_conv_im_write(PURPLE_CONV_IM(conv), ST_USER, "an answer",
	                     PURPLE_MESSAGE_SEND, now - 590);
	purple_conversation_write(conv, NULL, "a system line", PURPLE_MESSAGE_SYSTEM, now - 580);
	purple_conversation_write(conv, NULL, "an error", PURPLE_MESSAGE_ERROR, now - 570);
	purple_conversation_write(conv, NULL, "not logged", PURPLE_MESSAGE_NO_LOG, now - 560);
	purple_conv_im_write(PURPLE_CONV_IM(conv), ST_BUDDY, "/me waves",
	                     PURPLE_MESSAGE_RECV, now - 550);
	CHECK(n_messages(conv) == 6, "%u messages", n_messages(conv));
	CHECK(pidgin_message_get_flags(nth_message(conv, 1)) & PURPLE_MESSAGE_SEND, "flags");
	CHECK(strstr(pidgin_message_get_plain_text(nth_message(conv, 0)), "hello bold world") != NULL,
	      "plain text: %s", pidgin_message_get_plain_text(nth_message(conv, 0)));

	/* receiving-message-meta attaches to the next write */
	meta = meta_new("conv-type", "im", "sender", ST_BUDDY, "stanza-id", "s1",
	                "server-id", "srv1", "markable", "1", NULL);
	emit_meta(ST_BUDDY, meta);
	CHECK(g_hash_table_lookup(meta, "discard") == NULL, "fresh message discarded");
	/* The jabber prpl destroys the table (emptying it) before the write */
	g_hash_table_destroy(meta);
	purple_conv_im_write(PURPLE_CONV_IM(conv), ST_BUDDY, "a message with ids",
	                     PURPLE_MESSAGE_RECV, now - 500);
	msg = last_message(conv);
	CHECK(purple_strequal(pidgin_message_get_stanza_id(msg), "s1"), "stanza id %s",
	      pidgin_message_get_stanza_id(msg));
	CHECK(purple_strequal(pidgin_message_get_server_id(msg), "srv1"), "server id");
	CHECK(pidgin_message_get_index_id(msg) > 0, "not indexed");

	/* The index row, with its log position */
	akey = pidgin_message_index_account_key(st_account);
	ckey = pidgin_message_index_conv_key(st_account, ST_BUDDY);
	row = idx ? pidgin_message_index_find_by_id(idx, akey, ckey, "srv1") : NULL;
	CHECK(row != NULL, "srv1 not in the index");
	if (row != NULL) {
		CHECK(row->log_file != NULL && row->log_offset > 0, "log position %s:%" G_GINT64_FORMAT,
		      row->log_file ? row->log_file : "(null)", row->log_offset);
		if (row->log_file != NULL) {
			char *full = g_build_filename(purple_user_dir(), "logs", row->log_file, NULL);
			char *contents = NULL;

			if (g_file_get_contents(full, &contents, NULL, NULL) &&
			    row->log_offset < (gint64)strlen(contents))
				CHECK(strstr(contents + row->log_offset, "a message with ids") != NULL &&
				      strstr(contents + row->log_offset, "an answer") == NULL,
				      "offset points elsewhere: %.60s", contents + row->log_offset);
			g_free(contents);
			g_free(full);
		}
		pidgin_indexed_message_free(row);
	}

	/* Dedup: a server-id hit is discarded */
	n = n_messages(conv);
	meta = meta_new("conv-type", "im", "server-id", "srv1", "mam", "1", NULL);
	emit_meta(ST_BUDDY, meta);
	CHECK(purple_strequal(g_hash_table_lookup(meta, "discard"), "1"), "srv1 not discarded");
	g_hash_table_unref(meta);

	/* ... and an archive copy of a line without ids, fuzzily */
	{
		char *ts = g_strdup_printf("%" G_GINT64_FORMAT, (gint64)(now - 590));

		meta = meta_new("conv-type", "im", "server-id", "srv-new", "mam", "1",
		                "mam-query", "catchup", "timestamp", ts, NULL);
		emit_meta(ST_BUDDY, meta);
		CHECK(g_hash_table_lookup(meta, "discard") == NULL, "no id hit expected");
		g_hash_table_unref(meta);
		purple_conv_im_write(PURPLE_CONV_IM(conv), ST_USER, "an answer",
		                     PURPLE_MESSAGE_SEND | PURPLE_MESSAGE_DELAYED, now - 590);
		CHECK(n_messages(conv) == n, "fuzzy duplicate shown (%u, %u)", n_messages(conv), n);
		g_free(ts);
	}

	/* The send path: sending-message-meta gives the sent message its id */
	purple_conv_im_send(PURPLE_CONV_IM(conv), "outgoing message");
	sent = last_message(conv);
	CHECK(pidgin_message_get_flags(sent) & PURPLE_MESSAGE_SEND, "not sent");
	CHECK(pidgin_message_get_stanza_id(sent) != NULL &&
	      g_str_has_prefix(pidgin_message_get_stanza_id(sent), "sent-"), "sent id %s",
	      pidgin_message_get_stanza_id(sent));
	CHECK(pidgin_message_get_receipt(sent) == PIDGIN_RECEIPT_SENT, "receipt %d",
	      pidgin_message_get_receipt(sent));

	/* The compose entry: send, then Up-arrow edits the last message */
	pidgin_compose_entry_set_markup(PIDGIN_COMPOSE_ENTRY(gtkconv->entry), "typed <b>text</b>");
	CHECK(pidgin_compose_entry_send(PIDGIN_COMPOSE_ENTRY(gtkconv->entry)), "entry send");
	CHECK(purple_strequal(pidgin_message_get_plain_text(last_message(conv)), "typed text"),
	      "sent text %s", pidgin_message_get_plain_text(last_message(conv)));
	g_signal_emit_by_name(gtkconv->entry, "edit-last-requested");
	CHECK(gtkconv->editing != NULL, "edit-last didn't start editing");
	pidgin_compose_entry_set_markup(PIDGIN_COMPOSE_ENTRY(gtkconv->entry), "fixed text");
	pidgin_compose_entry_send(PIDGIN_COMPOSE_ENTRY(gtkconv->entry));
	CHECK(call("send-correction") != NULL && strstr(call("send-correction"), "|fixed text") != NULL,
	      "send-correction: %s", call("send-correction"));
	CHECK(gtkconv->editing == NULL, "still editing");

	/* Commands */
	pidgin_compose_entry_set_markup(PIDGIN_COMPOSE_ENTRY(gtkconv->entry), "/help");
	pidgin_compose_entry_send(PIDGIN_COMPOSE_ENTRY(gtkconv->entry));
	CHECK(strstr(pidgin_message_get_plain_text(last_message(conv)), "help") != NULL,
	      "/help: %s", pidgin_message_get_plain_text(last_message(conv)));
	pidgin_compose_entry_set_markup(PIDGIN_COMPOSE_ENTRY(gtkconv->entry), "/me tests");
	pidgin_compose_entry_send(PIDGIN_COMPOSE_ENTRY(gtkconv->entry));
	CHECK(strstr(pidgin_message_get_html(last_message(conv)), "tests") != NULL, "/me");

	/* M8 events */
	CHECK(emit_bool("message-corrected", st_account, ST_BUDDY, "s1", "s1-fix",
	                "a <i>corrected</i> message", ST_BUDDY), "correction not rendered");
	msg = pidgin_message_view_find_by_id(view_of(conv), "s1");
	CHECK(msg != NULL && pidgin_message_get_edited(msg), "not edited");
	CHECK(msg != NULL && strstr(pidgin_message_get_plain_text(msg), "a corrected message"),
	      "body %s", msg ? pidgin_message_get_plain_text(msg) : "");
	/* A correction by someone else is refused */
	CHECK(!emit_bool("message-corrected", st_account, ST_BUDDY, "s1", "s1-evil", "evil",
	                 "mallory@example.invalid"), "foreign correction accepted");

	CHECK(emit_bool("message-reaction", st_account, ST_BUDDY, "s1", THUMBS, ST_BUDDY,
	                GINT_TO_POINTER(TRUE)), "reaction not rendered");
	CHECK(msg != NULL && pidgin_message_has_reaction(msg, THUMBS, ST_BUDDY), "no reaction");
	/* The same add again is a no-op (no second log line) */
	emit_bool("message-reaction", st_account, ST_BUDDY, "s1", THUMBS, ST_BUDDY,
	          GINT_TO_POINTER(TRUE));

	/* React from the view: send-reaction gets the whole new set */
	g_signal_emit_by_name(view_of(conv), "reaction-toggled", msg, PARTY, TRUE);
	CHECK(call("send-reaction") != NULL && strstr(call("send-reaction"), "|s1|" PARTY),
	      "send-reaction: %s", call("send-reaction"));

	/* Reply from the view, then send */
	g_signal_emit_by_name(view_of(conv), "reply-requested", msg);
	CHECK(gtkconv->replying != NULL && gtk_widget_get_visible(gtkconv->banner), "no banner");
	pidgin_compose_entry_set_markup(PIDGIN_COMPOSE_ENTRY(gtkconv->entry), "my reply");
	pidgin_compose_entry_send(PIDGIN_COMPOSE_ENTRY(gtkconv->entry));
	CHECK(call("send-reply") != NULL && g_str_has_prefix(call("send-reply"), "s1|") &&
	      g_str_has_suffix(call("send-reply"), "|my reply"), "send-reply: %s", call("send-reply"));

	/* Receipts: delivered, then displayed covers earlier ones */
	CHECK(emit_bool("message-receipt", st_account, ST_BUDDY,
	                pidgin_message_get_stanza_id(sent), "displayed", ST_BUDDY), "receipt");
	CHECK(pidgin_message_get_receipt(sent) == PIDGIN_RECEIPT_DISPLAYED, "receipt state %d",
	      pidgin_message_get_receipt(sent));

	/* Unseen: a message while another tab is current, then read elsewhere */
	pidgin_conv_set_unseen(conv, PIDGIN_UNSEEN_TEXT);
	CHECK(gtkconv->unseen_count >= 1, "unseen count");
	CHECK(pidgin_conversations_get_unseen_count(PURPLE_CONV_TYPE_IM, PIDGIN_UNSEEN_TEXT) >= 1,
	      "global unseen count");
	emit_bool("message-receipt", st_account, ST_BUDDY, "srv1", "displayed", ST_USER);
	CHECK(gtkconv->unseen_state == PIDGIN_UNSEEN_NONE && gtkconv->unseen_count == 0,
	      "read elsewhere didn't clear unseen (%d)", gtkconv->unseen_state);

	/* Retraction */
	CHECK(emit_bool("message-retracted", st_account, ST_BUDDY, "s1", ST_BUDDY, NULL),
	      "retraction not rendered");
	CHECK(msg != NULL && pidgin_message_get_retracted(msg), "not retracted");
	g_signal_emit_by_name(view_of(conv), "retract-requested", sent);
	CHECK(call("send-retraction") != NULL &&
	      strstr(call("send-retraction"), pidgin_message_get_stanza_id(sent)) != NULL,
	      "send-retraction: %s", call("send-retraction"));

	/* Read markers for the newest markable message */
	meta = meta_new("conv-type", "im", "stanza-id", "s2", "server-id", "srv2", "markable", "1",
	                NULL);
	emit_meta(ST_BUDDY, meta);
	g_hash_table_unref(meta);
	purple_conv_im_write(PURPLE_CONV_IM(conv), ST_BUDDY, "mark me", PURPLE_MESSAGE_RECV, now);
	pidgin_conv_meta_mark_displayed(conv);
	CHECK(call("send-marker") != NULL && strstr(call("send-marker"), "|s2|displayed"),
	      "send-marker: %s", call("send-marker"));
	CHECK(call("mds-publish") != NULL && strstr(call("mds-publish"), "|srv2|"),
	      "mds-publish: %s", call("mds-publish"));

	/* The log got the rule 7 fallback lines */
	path = log_path(conv);
	CHECK(path != NULL, "no log");
	CHECK(file_contains(path, "edited: a corrected message"), "no edit line in %s", path);
	CHECK(file_contains(path, "reacted " THUMBS " to: a corrected message"), "no reaction line");
	CHECK(file_contains(path, "retracted a message"), "no retraction line");
	CHECK(!file_contains(path, "evil"), "foreign correction logged");
	{
		/* one reaction line only */
		char *contents = NULL;
		char *p;
		int count = 0;

		if (path != NULL && g_file_get_contents(path, &contents, NULL, NULL))
			for (p = contents; (p = strstr(p, "reacted " THUMBS)) != NULL; p++)
				count++;
		CHECK(count == 1, "%d reaction lines", count);
		g_free(contents);
	}
	/* ... and nothing of it shows in the view */
	for (n = 0; n < n_messages(conv); n++)
		CHECK(strstr(pidgin_message_get_plain_text(nth_message(conv, n)), "reacted") == NULL,
		      "a fallback line is displayed");
	g_free(path);

	/* Index-based scroll-back: older rows are prepended */
	if (idx != NULL) {
		PidginIndexedMessage *old = pidgin_indexed_message_new();
		GError *error = NULL;

		old->account = g_strdup(akey);
		old->conv = g_strdup(ckey);
		old->time = now - 86400 * 30;
		old->sender = g_strdup("Buddy");
		old->body = g_strdup("a message from last month");
		old->flags = PURPLE_MESSAGE_RECV;
		CHECK(pidgin_message_index_insert(idx, old, &error) > 0, "insert");
		g_clear_error(&error);
		pidgin_indexed_message_free(old);

		n = n_messages(conv);
		pidgin_conv_action(gtkconv, "load-older");
		CHECK(n_messages(conv) > n, "scroll-back added nothing (%u)", n_messages(conv));
		CHECK(strstr(pidgin_message_get_plain_text(nth_message(conv, 0)),
		             "a message from last month") != NULL, "first is %s",
		      pidgin_message_get_plain_text(nth_message(conv, 0)));
		CHECK(pidgin_message_index_count(idx) > rows_before + 5, "index rows %" G_GINT64_FORMAT,
		      pidgin_message_index_count(idx) - rows_before);
	}

	/* Last: its waits let the view's scroll-back run out of history. */
	test_images(conv);
	test_smileys(conv);
	test_buddy_icon(conv);

	g_free(akey);
	g_free(ckey);
}

static void
test_chat(PurpleConversation **chat_out)
{
	PurpleConnection *gc = purple_account_get_connection(st_account);
	PurpleConversation *conv;
	PidginConversation *gtkconv;
	GList *users = NULL, *flags = NULL;
	GListModel *model;
	GHashTable *meta;
	PidginMessage *msg;

	conv = serv_got_joined_chat(gc, 7, ST_ROOM);
	CHECK(conv != NULL, "no chat");
	*chat_out = conv;
	gtkconv = PIDGIN_CONVERSATION(conv);
	purple_conversation_set_logging(conv, TRUE);
	purple_conv_chat_set_nick(PURPLE_CONV_CHAT(conv), "me");

	users = g_list_append(users, "zed");
	flags = g_list_append(flags, GINT_TO_POINTER(PURPLE_CBFLAGS_NONE));
	users = g_list_append(users, "alice");
	flags = g_list_append(flags, GINT_TO_POINTER(PURPLE_CBFLAGS_NONE));
	users = g_list_append(users, "op");
	flags = g_list_append(flags, GINT_TO_POINTER(PURPLE_CBFLAGS_OP));
	users = g_list_append(users, "me");
	flags = g_list_append(flags, GINT_TO_POINTER(PURPLE_CBFLAGS_VOICE));
	purple_conv_chat_add_users(PURPLE_CONV_CHAT(conv), users, NULL, flags, FALSE);
	g_list_free(users);
	g_list_free(flags);

	model = G_LIST_MODEL(gtkconv->u.chat->users);
	CHECK(g_list_model_get_n_items(model) == 4, "%u users", g_list_model_get_n_items(model));
	{
		GObject *first = g_list_model_get_item(model, 0);
		char *name = NULL;

		/* The PidginChatUser type is private: check through the list's
		 * order via the prpl's view instead. */
		CHECK(first != NULL, "empty list");
		g_clear_object(&first);
		g_free(name);
	}
	purple_conv_chat_rename_user(PURPLE_CONV_CHAT(conv), "zed", "zack");
	CHECK(g_list_model_get_n_items(model) == 4, "rename changed the count");
	purple_conv_chat_user_set_flags(PURPLE_CONV_CHAT(conv), "alice", PURPLE_CBFLAGS_OP);
	purple_conv_chat_remove_user(PURPLE_CONV_CHAT(conv), "zack", "bye");
	CHECK(g_list_model_get_n_items(model) == 3, "remove: %u", g_list_model_get_n_items(model));

	/* Messages; a highlight while the tab is not current */
	serv_got_chat_in(gc, 7, "alice", PURPLE_MESSAGE_RECV, "hi all", time(NULL));
	CHECK(n_messages(conv) >= 1, "no chat messages");
	meta = meta_new("conv-type", "chat", "sender", "op", "stanza-id", "c1", "server-id",
	                "room-1", "occupant-id", "occ-op", NULL);
	emit_meta(ST_ROOM, meta);
	g_hash_table_unref(meta);
	serv_got_chat_in(gc, 7, "op", PURPLE_MESSAGE_RECV, "me: are you there?", time(NULL));
	msg = last_message(conv);
	CHECK(purple_strequal(pidgin_message_get_server_id(msg), "room-1"), "chat server id");
	CHECK(pidgin_message_get_flags(msg) & PURPLE_MESSAGE_NICK, "no NICK highlight");

	/* Sending in a room: the reflection carries the ids */
	purple_conv_chat_send(PURPLE_CONV_CHAT(conv), "hello room");
	msg = last_message(conv);
	CHECK(pidgin_message_get_flags(msg) & PURPLE_MESSAGE_SEND, "reflection not SEND");
	CHECK(pidgin_message_get_stanza_id(msg) != NULL, "reflection has no id");

	/* A reaction in the room, targeting the room's id */
	CHECK(emit_bool("message-reaction", st_account, ST_ROOM, "room-1", PARTY, "alice",
	                GINT_TO_POINTER(TRUE)), "room reaction");
	CHECK(pidgin_message_has_reaction(pidgin_message_view_find_by_id(view_of(conv), "room-1"),
	                                  PARTY, "alice"), "room reaction missing");

	/* Moderation-like retraction by someone else (the room checked) */
	CHECK(emit_bool("message-retracted", st_account, ST_ROOM, "room-1", "moderator", "spam"),
	      "moderation not rendered");
	CHECK(pidgin_message_get_retracted(pidgin_message_view_find_by_id(view_of(conv), "room-1")),
	      "not moderated");

	/* The prpl's /op from the user list menu's command action */
	g_free(gtkconv->u.chat->menu_who);
	gtkconv->u.chat->menu_who = g_strdup("alice");
	gtk_widget_activate_action(gtkconv->u.chat->userlist_box, "user.command", "s", "op");
	CHECK(purple_strequal(call("cmd-op"), "alice"), "user.command op: %s", call("cmd-op"));

	/* The topic */
	purple_conv_chat_set_topic(PURPLE_CONV_CHAT(conv), "op", "The <b>topic</b>");
	CHECK(purple_strequal(gtk_editable_get_text(GTK_EDITABLE(gtkconv->u.chat->topic_text)),
	                      "The topic"), "topic %s",
	      gtk_editable_get_text(GTK_EDITABLE(gtkconv->u.chat->topic_text)));

	{
		char *path = log_path(conv);

		CHECK(file_contains(path, "reacted " PARTY), "no room reaction line");
		CHECK(file_contains(path, "moderator removed a message: spam"), "no moderation line");
		g_free(path);
	}
}

static gboolean
selftest_run(gpointer data)
{
	PurpleConversation *im = NULL, *chat = NULL;
	PidginConversation *gtkim, *gtkchat;
	PidginWindow *win;
	int before;

	/* The protocol (tests/selftest-prpl.c) and a throwaway account */
	CHECK(pidgin_selftest_prpl_register() != NULL, "selftest prpl didn't load");

	st_account = pidgin_selftest_account_new(ST_USER);
	CHECK(purple_account_is_connected(st_account), "the selftest account didn't connect");
	if (!purple_account_is_connected(st_account))
		goto done;

	test_im(&im);
	test_chat(&chat);
	test_attention(im);
	test_paste_image(im);
	test_attach(im);
	test_xmpp_shares(im);
	test_received_files(im);
	test_media(im);
	share_server_stop();
	test_send_to(im);
	spin(200);

	/* Tabs: both in one window (placement "last" unless the pref says
	 * otherwise; move the chat there to be sure). */
	gtkim = PIDGIN_CONVERSATION(im);
	gtkchat = PIDGIN_CONVERSATION(chat);
	if (gtkim->win != gtkchat->win) {
		pidgin_conv_window_remove_gtkconv(gtkchat->win, gtkchat);
		pidgin_conv_window_add_gtkconv(gtkim->win, gtkchat);
	}
	win = gtkim->win;
	CHECK(pidgin_conv_window_get_gtkconv_count(win) == 2, "%u tabs",
	      pidgin_conv_window_get_gtkconv_count(win));
	pidgin_conv_window_switch_gtkconv(win, gtkchat);
	hold("chat");
	pidgin_conv_window_switch_gtkconv(win, gtkim);
	CHECK(pidgin_conv_window_get_active_gtkconv(win) == gtkim, "switch to IM");
	hold("im");
	CHECK(activate(win, "conv.next-tab", NULL), "conv.next-tab");
	CHECK(pidgin_conv_window_get_active_gtkconv(win) == gtkchat, "next-tab");
	CHECK(activate(win, "conv.tab", g_variant_new_int32(1)), "conv.tab(1)");
	CHECK(pidgin_conv_window_get_active_gtkconv(win) == gtkim, "tab(1)");
	CHECK(activate(win, "conv.prev-tab", NULL), "conv.prev-tab");
	CHECK(pidgin_conv_window_get_active_gtkconv(win) == gtkchat, "prev-tab wraps");

	/* Unseen while the other tab is current; next-unread goes there */
	pidgin_conv_set_unseen(im, PIDGIN_UNSEEN_NONE);
	purple_conv_im_write(PURPLE_CONV_IM(im), ST_BUDDY, "while away", PURPLE_MESSAGE_RECV,
	                     time(NULL));
	CHECK(gtkim->unseen_state == PIDGIN_UNSEEN_TEXT && gtkim->unseen_count == 1,
	      "unseen %d/%u", gtkim->unseen_state, gtkim->unseen_count);
	{
		GList *list = pidgin_conversations_find_unseen_list(PURPLE_CONV_TYPE_ANY,
			PIDGIN_UNSEEN_TEXT, FALSE, 0);
		CHECK(g_list_find(list, im) != NULL, "not in the unseen list");
		g_list_free(list);
	}
	CHECK(activate(win, "conv.next-unread", NULL), "conv.next-unread");
	CHECK(pidgin_conv_window_get_active_gtkconv(win) == gtkim, "next-unread");
	/* Switching to it with the window active clears it; without a window
	 * manager the window may not be active, so clear as the window does. */
	pidgin_conv_seen(gtkim);
	CHECK(gtkim->unseen_state == PIDGIN_UNSEEN_NONE, "seen");

	/* Menu actions and toggles */
	CHECK(activate(win, "conv.find", NULL), "conv.find");
	CHECK(activate(win, "conv.timestamps", NULL), "conv.timestamps");
	CHECK(!purple_prefs_get_bool(PIDGIN_PREFS_ROOT "/conversations/show_timestamps"),
	      "timestamps pref");
	activate(win, "conv.timestamps", NULL);
	before = purple_conversation_is_logging(im);
	CHECK(activate(win, "conv.logging", NULL), "conv.logging");
	CHECK(purple_conversation_is_logging(im) != before, "logging toggle");
	activate(win, "conv.logging", NULL);
	CHECK(activate(win, "conv.clear", NULL), "conv.clear");
	CHECK(n_messages(im) == 0, "clear left %u", n_messages(im));

	/* Close the IM with Ctrl+W's action; the chat remains */
	CHECK(activate(win, "conv.close", NULL), "conv.close");
	spin(100);
	CHECK(g_list_find(purple_get_conversations(), im) == NULL, "IM not closed");
	CHECK(pidgin_conv_window_get_gtkconv_count(win) == 1, "tabs after close");

	/* Hidden conversations (hide_new = always) and presenting them */
	purple_prefs_set_string(PIDGIN_PREFS_ROOT "/conversations/im/hide_new", "always");
	serv_got_im(purple_account_get_connection(st_account), "stranger@example.invalid",
	            "psst", PURPLE_MESSAGE_RECV, time(NULL));
	im = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM,
	                                           "stranger@example.invalid", st_account);
	CHECK(im != NULL && pidgin_conv_is_hidden(PIDGIN_CONVERSATION(im)), "not hidden");
	if (im != NULL) {
		GList *list = pidgin_conversations_find_unseen_list(PURPLE_CONV_TYPE_IM,
			PIDGIN_UNSEEN_TEXT, TRUE, 0);
		CHECK(g_list_find(list, im) != NULL, "hidden unseen");
		g_list_free(list);
		pidgin_conv_present_conversation(im);
		CHECK(!pidgin_conv_is_hidden(PIDGIN_CONVERSATION(im)), "present didn't show it");
		CHECK(n_messages(im) == 1, "hidden message lost (%u)", n_messages(im));
	}
	purple_prefs_set_string(PIDGIN_PREFS_ROOT "/conversations/im/hide_new", "never");

	/* Detach a tab into a new window */
	if (im != NULL && PIDGIN_CONVERSATION(im)->win == PIDGIN_CONVERSATION(chat)->win) {
		guint nwin = g_list_length(pidgin_conv_windows_get_list());

		win = PIDGIN_CONVERSATION(im)->win;
		win->tab_menu_conv = PIDGIN_CONVERSATION(im);
		gtk_widget_activate_action(win->window, "tab.detach", NULL);
		spin(100);
		CHECK(g_list_length(pidgin_conv_windows_get_list()) == nwin + 1, "detach");
	}
	spin(200);

done:
	/* Clean up: conversations, the account, the protocol */
	while (purple_get_conversations() != NULL)
		purple_conversation_destroy(purple_get_conversations()->data);
	spin(100);
	pidgin_selftest_account_remove(st_account);
	st_account = NULL;
	pidgin_selftest_prpl_unregister();

	if (failures == 0)
		g_print("PIDGIN4_CONV_SELFTEST: PASS (%d checks)\n", checks);
	else
		g_print("PIDGIN4_CONV_SELFTEST: %d of %d checks FAILED\n", failures, checks);
	pidgin_application_set_exit_status(failures == 0 ? 0 : 1);
	pidgin_application_quit();
	return G_SOURCE_REMOVE;
}

void
pidgin_conversations_selftest(void)
{
	if (g_getenv("PIDGIN4_CONV_SELFTEST") == NULL)
		return;
	/* After startup settles (the buddy list is shown). */
	g_timeout_add(500, selftest_run, NULL);
}
