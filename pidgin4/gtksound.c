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

#include <math.h>
#include <string.h>

#include <gsound.h>

#include "blist.h"
#include "connection.h"
#include "conversation.h"
#include "debug.h"
#include "prefs.h"
#include "signals.h"
#include "sound.h"
#include "util.h"

#include "gtksound.h"

/*
 * The event logic of pidgin/gtksound.c (which events play when, the
 * 10 s of silence after signing on, conv_focus), playing through GSound
 * instead of a GStreamer playbin: GTK here has no media backend
 * (USE=-gstreamer). See gtksound.h for the prefs.
 *
 * Not ported: sound themes (/pidgin/sound/theme; the theme loader is
 * TODO(M5) with the preferences window) and the per-conversation "Enable
 * Sounds" toggle (gtkconv's make_sound, TODO(M4b)).
 */

#define SOUND_PREFS PIDGIN_PREFS_ROOT "/sound"
#define SOUND4_PREFS PIDGIN4_PREFS_ROOT "/sound"

struct pidgin_sound_event {
	char *label;
	char *pref;
	char *def;
	/* freedesktop sound naming spec, for GSound's event.id */
	char *event_id;
};

static guint mute_login_sounds_timeout = 0;
static gboolean mute_login_sounds = FALSE;
static GSoundContext *gsound = NULL;
static gboolean gsound_failed = FALSE;

static const struct pidgin_sound_event sounds[PURPLE_NUM_SOUNDS] = {
	{N_("Buddy logs in"), "login", "login.wav", "service-login"},
	{N_("Buddy logs out"), "logout", "logout.wav", "service-logout"},
	{N_("Message received"), "im_recv", "receive.wav", "message-new-instant"},
	{N_("Message received begins conversation"), "first_im_recv", "receive.wav", "message-new-instant"},
	{N_("Message sent"), "send_im", "send.wav", "message-sent-instant"},
	{N_("Person enters chat"), "join_chat", "login.wav", "service-login"},
	{N_("Person leaves chat"), "left_chat", "logout.wav", "service-logout"},
	{N_("You talk in chat"), "send_chat_msg", "send.wav", "message-sent-instant"},
	{N_("Others talk in chat"), "chat_msg_recv", "receive.wav", "message-new-instant"},
	/* this isn't a terminator, it's the buddy pounce default sound event ;-) */
	{NULL, "pounce_default", "alert.wav", "message"},
	{N_("Someone says your username in chat"), "nick_said", "alert.wav", "message-new-instant"},
	{N_("Attention received"), "got_attention", "alert.wav", "window-attention"}
};

/**************************************************************************
 * Helpers (public for the tests)
 **************************************************************************/

const char *
pidgin_sound_get_event_option(PurpleSoundEventID event)
{
	if (event >= PURPLE_NUM_SOUNDS)
		return NULL;
	return sounds[event].pref;
}

const char *
pidgin_sound_get_event_label(PurpleSoundEventID event)
{
	if (event >= PURPLE_NUM_SOUNDS || sounds[event].label == NULL)
		return NULL;
	return _(sounds[event].label);
}

const char *
pidgin_sound_get_event_id(PurpleSoundEventID event)
{
	if (event >= PURPLE_NUM_SOUNDS)
		return NULL;
	return sounds[event].event_id;
}

char *
pidgin_sound_get_event_file(PurpleSoundEventID event)
{
	char *pref;
	const char *file;
	char *filename;

	if (event >= PURPLE_NUM_SOUNDS)
		return NULL;

	pref = g_strdup_printf(SOUND_PREFS "/file/%s", sounds[event].pref);
	file = purple_prefs_exists(pref) ? purple_prefs_get_path(pref) : NULL;
	if (file != NULL && *file != '\0')
		filename = g_strdup(file);
	else
		/* The default sounds come with libpurple (share/sounds/purple
		 * of its prefix). */
		filename = g_build_filename(PURPLE_DATADIR, "sounds", "purple",
		                            sounds[event].def, NULL);
	g_free(pref);

	return filename;
}

char *
pidgin_sound_build_command(const char *command, const char *filename)
{
	char *quoted, *result;

	if (command == NULL || *command == '\0')
		return NULL;

	quoted = g_shell_quote(filename);
	if (strstr(command, "%s"))
		result = purple_strreplace(command, "%s", quoted);
	else
		result = g_strdup_printf("%s %s", command, quoted);
	g_free(quoted);

	return result;
}

char *
pidgin_sound_volume_to_db(int volume)
{
	char buf[G_ASCII_DTOSTR_BUF_SIZE];

	volume = CLAMP(volume, 0, 100);
	if (volume == 0)
		return NULL;

	/* Pidgin 2 used volume / 50 as the gain factor. */
	g_ascii_formatd(buf, sizeof(buf), "%.2f", 20.0 * log10(volume / 50.0));
	return g_strdup(buf);
}

/**************************************************************************
 * Playing
 **************************************************************************/

static void
play_done_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	char *filename = data;
	GError *error = NULL;

	if (!gsound_context_play_full_finish(GSOUND_CONTEXT(source), result, &error)) {
		purple_debug_error("sound", "GSound could not play %s: %s\n",
		                   filename, error->message);
		g_error_free(error);
	} else if (g_getenv("PIDGIN4_SOUND_SELFTEST") != NULL) {
		purple_debug_info("sound", "played %s\n", filename);
	}
	g_free(filename);
}

static GSoundContext *
get_gsound(void)
{
	GError *error = NULL;

	if (gsound != NULL || gsound_failed)
		return gsound;

	gsound = gsound_context_new(NULL, &error);
	if (gsound == NULL) {
		gsound_failed = TRUE;
		purple_debug_error("sound", "GSound is not available: %s\n",
		                   error->message);
		g_error_free(error);
		return NULL;
	}

	gsound_context_set_attributes(gsound, NULL,
		GSOUND_ATTR_APPLICATION_NAME, PIDGIN_NAME,
		GSOUND_ATTR_APPLICATION_ID, PIDGIN4_APP_ID,
		GSOUND_ATTR_APPLICATION_ICON_NAME, PIDGIN4_APP_ID,
		NULL);
	return gsound;
}

static void
play_with_gsound(const char *filename, PurpleSoundEventID event)
{
	GSoundContext *context = get_gsound();
	char *db;

	if (context == NULL)
		return;

	db = pidgin_sound_volume_to_db(purple_prefs_get_int(SOUND4_PREFS "/volume"));
	if (db == NULL)
		return; /* volume 0 */

	/*
	 * media.filename wins over event.id in libcanberra, so the event id
	 * only names the event (for the sound server's event role and
	 * per-event settings). The cache stays off ("never"): a cached sample
	 * is named by its event id, so caching would replace other
	 * applications' "message-new-instant" with Pidgin's sound and vice
	 * versa.
	 */
	if (event < PURPLE_NUM_SOUNDS)
		gsound_context_play_full(context, NULL, play_done_cb, g_strdup(filename),
			GSOUND_ATTR_MEDIA_FILENAME, filename,
			GSOUND_ATTR_CANBERRA_CACHE_CONTROL, "never",
			GSOUND_ATTR_CANBERRA_VOLUME, db,
			GSOUND_ATTR_MEDIA_ROLE, "event",
			GSOUND_ATTR_EVENT_ID, sounds[event].event_id,
			GSOUND_ATTR_EVENT_DESCRIPTION, sounds[event].label ?
				_(sounds[event].label) : sounds[event].pref,
			NULL);
	else
		gsound_context_play_full(context, NULL, play_done_cb, g_strdup(filename),
			GSOUND_ATTR_MEDIA_FILENAME, filename,
			GSOUND_ATTR_CANBERRA_CACHE_CONTROL, "never",
			GSOUND_ATTR_CANBERRA_VOLUME, db,
			GSOUND_ATTR_MEDIA_ROLE, "event",
			NULL);
	g_free(db);
}

static void
play_with_command(const char *filename)
{
	char *command;
	char **argv = NULL;
	GError *error = NULL;

	command = pidgin_sound_build_command(
		purple_prefs_get_string(SOUND4_PREFS "/command"), filename);
	if (command == NULL) {
		purple_debug_error("sound", "'Command' sound method has been "
		                   "chosen, but no command has been set.\n");
		return;
	}

	if (!g_shell_parse_argv(command, NULL, &argv, &error)) {
		purple_debug_error("sound", "error parsing command %s (%s)\n",
		                   command, error->message);
		g_error_free(error);
	} else if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH |
	                          G_SPAWN_STDOUT_TO_DEV_NULL |
	                          G_SPAWN_STDERR_TO_DEV_NULL,
	                          NULL, NULL, NULL, &error)) {
		purple_debug_error("sound", "sound command could not be launched: %s\n",
		                   error->message);
		g_error_free(error);
	}

	g_strfreev(argv);
	g_free(command);
}

/* @event: PURPLE_NUM_SOUNDS for a file without an event. */
static void
play(const char *filename, PurpleSoundEventID event, gboolean ignore_mute)
{
	const char *method;

	if (!ignore_mute && purple_prefs_get_bool(SOUND_PREFS "/mute"))
		return;

	method = purple_prefs_get_string(SOUND4_PREFS "/method");
	if (purple_strequal(method, "none"))
		return;

	if (filename == NULL || !g_file_test(filename, G_FILE_TEST_EXISTS)) {
		purple_debug_error("sound", "sound file (%s) does not exist.\n",
		                   filename ? filename : "(null)");
		return;
	}

	if (purple_strequal(method, "command"))
		play_with_command(filename);
	else
		play_with_gsound(filename, event);
}

void
pidgin_sound_play_file(const char *filename)
{
	play(filename, PURPLE_NUM_SOUNDS, FALSE);
}

/* The play_file UI op (purple_sound_play_file()). */
static void
pidgin_sound_play_file_op(const char *filename)
{
	play(filename, PURPLE_NUM_SOUNDS, FALSE);
}

static void
pidgin_sound_play_event(PurpleSoundEventID event)
{
	char *enable_pref;
	char *filename;

	if ((event == PURPLE_SOUND_BUDDY_ARRIVE) && mute_login_sounds)
		return;

	if (event >= PURPLE_NUM_SOUNDS) {
		purple_debug_error("sound", "got request for unknown sound: %d\n", event);
		return;
	}

	enable_pref = g_strdup_printf(SOUND_PREFS "/enabled/%s", sounds[event].pref);

	/* libpurple has checked /purple/sound/while_status already. */
	if (purple_prefs_get_bool(enable_pref)) {
		filename = pidgin_sound_get_event_file(event);
		play(filename, event, FALSE);
		g_free(filename);
	}

	g_free(enable_pref);
}

gboolean
pidgin_sound_is_customized(void)
{
	gint i;
	gchar *path;
	const char *file;

	for (i = 0; i < PURPLE_NUM_SOUNDS; i++) {
		path = g_strdup_printf(SOUND_PREFS "/file/%s", sounds[i].pref);
		file = purple_prefs_get_path(path);
		g_free(path);

		if (file && file[0] != '\0')
			return TRUE;
	}

	return FALSE;
}

/**************************************************************************
 * Events (pidgin/gtksound.c)
 **************************************************************************/

static gboolean
unmute_login_sounds_cb(gpointer data)
{
	mute_login_sounds = FALSE;
	mute_login_sounds_timeout = 0;
	return FALSE;
}

static gboolean
chat_nick_matches_name(PurpleConversation *conv, const char *aname)
{
	PurpleConvChat *chat = NULL;
	char *nick = NULL;
	char *name = NULL;
	gboolean ret = FALSE;
	chat = purple_conversation_get_chat_data(conv);

	if (chat==NULL)
		return ret;

	nick = g_strdup(purple_normalize(conv->account, chat->nick));
	name = g_strdup(purple_normalize(conv->account, aname));

	if (g_utf8_collate(nick, name) == 0)
		ret = TRUE;

	g_free(nick);
	g_free(name);

	return ret;
}

/*
 * play a sound event for a conversation, checking for focus if the
 * conv_focus pref is set
 */
static void
play_conv_event(PurpleConversation *conv, PurpleSoundEventID event)
{
	/* If we should not play the sound for some reason, then exit early */
	if (conv != NULL && purple_conversation_has_focus(conv) &&
	    !purple_prefs_get_bool(SOUND_PREFS "/conv_focus"))
		return;

	purple_sound_play_event(event, conv ? purple_conversation_get_account(conv) : NULL);
}

static void
buddy_state_cb(PurpleBuddy *buddy, PurpleSoundEventID event)
{
	purple_sound_play_event(event, purple_buddy_get_account(buddy));
}

static void
im_msg_received_cb(PurpleAccount *account, char *sender,
				   char *message, PurpleConversation *conv,
				   PurpleMessageFlags flags, PurpleSoundEventID event)
{
	if (flags & PURPLE_MESSAGE_DELAYED || flags & PURPLE_MESSAGE_NOTIFY)
		return;

	if (conv==NULL)
		purple_sound_play_event(PURPLE_SOUND_FIRST_RECEIVE, account);
	else
		play_conv_event(conv, event);
}

static void
im_msg_sent_cb(PurpleAccount *account, const char *receiver,
			   const char *message, PurpleSoundEventID event)
{
	PurpleConversation *conv = purple_find_conversation_with_account(
		PURPLE_CONV_TYPE_IM, receiver, account);
	play_conv_event(conv, event);
}

static void
chat_buddy_join_cb(PurpleConversation *conv, const char *name,
				   PurpleConvChatBuddyFlags flags, gboolean new_arrival,
				   PurpleSoundEventID event)
{
	if (new_arrival && !chat_nick_matches_name(conv, name))
		play_conv_event(conv, event);
}

static void
chat_buddy_left_cb(PurpleConversation *conv, const char *name,
				   const char *reason, PurpleSoundEventID event)
{
	if (!chat_nick_matches_name(conv, name))
		play_conv_event(conv, event);
}

static void
chat_msg_sent_cb(PurpleAccount *account, const char *message,
				 int id, PurpleSoundEventID event)
{
	PurpleConnection *conn = purple_account_get_connection(account);
	PurpleConversation *conv = NULL;

	if (conn!=NULL)
		conv = purple_find_chat(conn,id);

	play_conv_event(conv, event);
}

static void
chat_msg_received_cb(PurpleAccount *account, char *sender,
					 char *message, PurpleConversation *conv,
					 PurpleMessageFlags flags, PurpleSoundEventID event)
{
	PurpleConvChat *chat;

	if (flags & PURPLE_MESSAGE_DELAYED || flags & PURPLE_MESSAGE_NOTIFY)
		return;

	chat = purple_conversation_get_chat_data(conv);
	g_return_if_fail(chat != NULL);

	if (purple_conv_chat_is_user_ignored(chat, sender))
		return;

	if (chat_nick_matches_name(conv, sender))
		return;

	if (flags & PURPLE_MESSAGE_NICK || purple_utf8_has_word(message, chat->nick))
		/* This isn't quite right; if you have the PURPLE_SOUND_CHAT_NICK event disabled
		 * and the PURPLE_SOUND_CHAT_SAY event enabled, you won't get a sound at all */
		play_conv_event(conv, PURPLE_SOUND_CHAT_NICK);
	else
		play_conv_event(conv, event);
}

static void
got_attention_cb(PurpleAccount *account, const char *who,
	PurpleConversation *conv, guint type, PurpleSoundEventID event)
{
	play_conv_event(conv, event);
}

/*
 * We mute sounds for the 10 seconds after you log in so that
 * you don't get flooded with sounds when the blist shows all
 * your buddies logging in.
 */
static void
account_signon_cb(PurpleConnection *gc, gpointer data)
{
	if (mute_login_sounds_timeout != 0)
		purple_timeout_remove(mute_login_sounds_timeout);
	mute_login_sounds = TRUE;
	mute_login_sounds_timeout = purple_timeout_add_seconds(10, unmute_login_sounds_cb, NULL);
}

void *
pidgin_sound_get_handle(void)
{
	static int handle;

	return &handle;
}

/**************************************************************************
 * Setup
 **************************************************************************/

void
pidgin_sound_register_prefs(void)
{
	int i;

	/* Shared: Pidgin 2's keys, types and defaults (pidgin/gtksound.c). A
	 * profile Pidgin 2 has used has them all already. */
	purple_prefs_add_none(PIDGIN_PREFS_ROOT);
	purple_prefs_add_none(SOUND_PREFS);
	purple_prefs_add_none(SOUND_PREFS "/enabled");
	purple_prefs_add_none(SOUND_PREFS "/file");
	for (i = 0; i < PURPLE_NUM_SOUNDS; i++) {
		static const gboolean defaults[PURPLE_NUM_SOUNDS] = {
			TRUE, TRUE, TRUE, FALSE, TRUE, FALSE, FALSE, FALSE, FALSE,
			TRUE, FALSE, TRUE
		};
		char *pref;

		pref = g_strdup_printf(SOUND_PREFS "/enabled/%s", sounds[i].pref);
		purple_prefs_add_bool(pref, defaults[i]);
		g_free(pref);
		pref = g_strdup_printf(SOUND_PREFS "/file/%s", sounds[i].pref);
		purple_prefs_add_path(pref, "");
		g_free(pref);
	}
	purple_prefs_add_bool(SOUND_PREFS "/enabled/sent_attention", TRUE);
	purple_prefs_add_path(SOUND_PREFS "/file/sent_attention", "");
	purple_prefs_add_bool(SOUND_PREFS "/conv_focus", TRUE);
	purple_prefs_add_bool(SOUND_PREFS "/mute", FALSE);

	/* pidgin4's own. */
	purple_prefs_add_none(PIDGIN4_PREFS_ROOT);
	purple_prefs_add_none(SOUND4_PREFS);
	purple_prefs_add_string(SOUND4_PREFS "/method", "gsound");
	purple_prefs_add_string(SOUND4_PREFS "/command", "");
	purple_prefs_add_int(SOUND4_PREFS "/volume", 50);
}

static void
selftest_cb(gpointer data)
{
	char *filename = pidgin_sound_get_event_file(PURPLE_SOUND_RECEIVE);

	/* The default receive sound, whatever mute says. */
	purple_debug_info("sound", "selftest: playing %s (method %s)\n", filename,
	                  purple_prefs_get_string(SOUND4_PREFS "/method"));
	play(filename, PURPLE_SOUND_RECEIVE, TRUE);
	g_free(filename);
}

static void
pidgin_sound_init(void)
{
	void *gtk_sound_handle = pidgin_sound_get_handle();
	void *blist_handle = purple_blist_get_handle();
	void *conv_handle = purple_conversations_get_handle();

	purple_signal_connect(purple_connections_get_handle(), "signed-on",
						gtk_sound_handle, PURPLE_CALLBACK(account_signon_cb),
						NULL);

	pidgin_sound_register_prefs();

	purple_signal_connect(blist_handle, "buddy-signed-on",
						gtk_sound_handle, PURPLE_CALLBACK(buddy_state_cb),
						GINT_TO_POINTER(PURPLE_SOUND_BUDDY_ARRIVE));
	purple_signal_connect(blist_handle, "buddy-signed-off",
						gtk_sound_handle, PURPLE_CALLBACK(buddy_state_cb),
						GINT_TO_POINTER(PURPLE_SOUND_BUDDY_LEAVE));
	purple_signal_connect(conv_handle, "received-im-msg",
						gtk_sound_handle, PURPLE_CALLBACK(im_msg_received_cb),
						GINT_TO_POINTER(PURPLE_SOUND_RECEIVE));
	purple_signal_connect(conv_handle, "sent-im-msg",
						gtk_sound_handle, PURPLE_CALLBACK(im_msg_sent_cb),
						GINT_TO_POINTER(PURPLE_SOUND_SEND));
	purple_signal_connect(conv_handle, "chat-buddy-joined",
						gtk_sound_handle, PURPLE_CALLBACK(chat_buddy_join_cb),
						GINT_TO_POINTER(PURPLE_SOUND_CHAT_JOIN));
	purple_signal_connect(conv_handle, "chat-buddy-left",
						gtk_sound_handle, PURPLE_CALLBACK(chat_buddy_left_cb),
						GINT_TO_POINTER(PURPLE_SOUND_CHAT_LEAVE));
	purple_signal_connect(conv_handle, "sent-chat-msg",
						gtk_sound_handle, PURPLE_CALLBACK(chat_msg_sent_cb),
						GINT_TO_POINTER(PURPLE_SOUND_CHAT_YOU_SAY));
	purple_signal_connect(conv_handle, "received-chat-msg",
						gtk_sound_handle, PURPLE_CALLBACK(chat_msg_received_cb),
						GINT_TO_POINTER(PURPLE_SOUND_CHAT_SAY));
	purple_signal_connect(conv_handle, "got-attention", gtk_sound_handle,
						PURPLE_CALLBACK(got_attention_cb),
						  GINT_TO_POINTER(PURPLE_SOUND_GOT_ATTENTION));
	/* for the time being, don't handle sent-attention here, since playing a
	 sound would result induplicate sounds. And fixing that would require changing the
	 conversation signal for msg-recv */

	if (g_getenv("PIDGIN4_SOUND_SELFTEST") != NULL)
		g_idle_add_once(selftest_cb, NULL);
}

static void
pidgin_sound_uninit(void)
{
	purple_signals_disconnect_by_handle(pidgin_sound_get_handle());
	if (mute_login_sounds_timeout != 0) {
		purple_timeout_remove(mute_login_sounds_timeout);
		mute_login_sounds_timeout = 0;
	}
	g_clear_object(&gsound);
	gsound_failed = FALSE;
}

static PurpleSoundUiOps sound_ui_ops =
{
	pidgin_sound_init,
	pidgin_sound_uninit,
	pidgin_sound_play_file_op,
	pidgin_sound_play_event,
	NULL,
	NULL,
	NULL,
	NULL
};

PurpleSoundUiOps *
pidgin_sound_get_ui_ops(void)
{
	return &sound_ui_ops;
}
