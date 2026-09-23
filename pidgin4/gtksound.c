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

#include "gtksound.h"

/*
 * TODO(M6): sounds through GSound (pidginsound.c) plus the custom command
 * option, with pidgin4's own /pidgin4/sound prefs. Until then there are no
 * sound UI ops and libpurple's purple_sound_play_*() do nothing.
 */

/* In PurpleSoundEventID order, as in pidgin/gtksound.c. */
static const char *const event_options[PURPLE_NUM_SOUNDS] = {
	"login", "logout", "im_recv", "first_im_recv", "send_im", "join_chat",
	"left_chat", "send_chat_msg", "chat_msg_recv", "pounce_default",
	"nick_said", "got_attention",
};

static const char *const event_labels[PURPLE_NUM_SOUNDS] = {
	N_("Buddy logs in"), N_("Buddy logs out"), N_("Message received"),
	N_("Message received begins conversation"), N_("Message sent"),
	N_("Person enters chat"), N_("Person leaves chat"),
	N_("You talk in chat"), N_("Others talk in chat"),
	NULL, /* the buddy pounce default sound */
	N_("Someone says your username in chat"), N_("Attention received"),
};

const char *
pidgin_sound_get_event_option(PurpleSoundEventID event)
{
	if (event >= PURPLE_NUM_SOUNDS)
		return NULL;
	return event_options[event];
}

const char *
pidgin_sound_get_event_label(PurpleSoundEventID event)
{
	if (event >= PURPLE_NUM_SOUNDS || event_labels[event] == NULL)
		return NULL;
	return _(event_labels[event]);
}

PurpleSoundUiOps *
pidgin_sound_get_ui_ops(void)
{
	return NULL;
}

void *
pidgin_sound_get_handle(void)
{
	static int handle;

	return &handle;
}

gboolean
pidgin_sound_is_customized(void)
{
	return FALSE;
}
