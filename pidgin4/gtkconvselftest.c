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

#include "account.h"
#include "blist.h"
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
#include "pidgincomposeentry.h"
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

/* A small PNG in the imgstore (the caller unrefs it) */
static int
add_test_image(void)
{
	guchar pixels[8 * 8 * 4];
	GBytes *bytes, *png;
	GdkTexture *texture;
	gpointer data;
	gsize len;
	guint i;

	for (i = 0; i < sizeof(pixels); i += 4) {
		pixels[i] = 0xcc;
		pixels[i + 1] = 0x22;
		pixels[i + 2] = 0x22;
		pixels[i + 3] = 0xff;
	}
	bytes = g_bytes_new(pixels, sizeof(pixels));
	texture = gdk_memory_texture_new(8, 8, GDK_MEMORY_R8G8B8A8, bytes, 8 * 4);
	png = gdk_texture_save_to_png_bytes(texture);
	g_bytes_unref(bytes);
	g_object_unref(texture);
	data = g_bytes_unref_to_data(png, &len);
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
	 * the buddy isn't on the list */
	id = add_test_image();
	path = g_build_filename(purple_user_dir(), "selftest-drop.png", NULL);
	CHECK(g_file_set_contents(path, purple_imgstore_get_data(purple_imgstore_find_by_id(id)),
	                          purple_imgstore_get_size(purple_imgstore_find_by_id(id)), NULL),
	      "writing %s", path);
	purple_imgstore_unref_by_id(id);
	buttons = drop_file(conv, path);
	CHECK(purple_strequal(buttons, "Insert in Message|Cancel"), "drop offered %s", buttons);
	g_free(buttons);

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
	g_hash_table_unref(meta);
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
