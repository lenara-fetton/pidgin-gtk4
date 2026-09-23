/**
 * @file gtksound.h GTK+ Sound API
 * @ingroup pidgin
 */

/* pidgin
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
#ifndef _PIDGINSOUND_H_
#define _PIDGINSOUND_H_

#include "sound.h"

/**************************************************************************/
/** @name GTK+ Sound API                                                  */
/**************************************************************************/
/*@{*/

/**
 * Get the prefs option for an event.
 *
 * @param event The event.
 * @return The option.
 */
const char *pidgin_sound_get_event_option(PurpleSoundEventID event);

/**
 * Get the label for an event.
 *
 * @param event The event.
 * @return The label.
 */
const char *pidgin_sound_get_event_label(PurpleSoundEventID event);

/**
 * Gets GTK+ sound UI ops.
 *
 * @return The UI operations structure.
 */
PurpleSoundUiOps *pidgin_sound_get_ui_ops(void);

/**
 * Get the handle for the GTK+ sound system.
 *
 * @return The handle to the sound system
 */
void *pidgin_sound_get_handle(void);

/**
 * Returns true Pidgin is using customized sounds
 *
 * @return TRUE if non default sounds are used.
 *
 * @since 2.6.0
 */
gboolean pidgin_sound_is_customized(void);

/*
 * pidgin4 (M6): GSound (libcanberra) or a custom command.
 *
 * Prefs, shared with Pidgin 2 (same keys, types, defaults and meaning):
 *   /pidgin/sound/enabled/<event>, /pidgin/sound/file/<event> (the
 *   <event> names are pidgin_sound_get_event_option()),
 *   /pidgin/sound/conv_focus, /pidgin/sound/mute.
 * pidgin4's own (Pidgin 2's method, command and volume meant GStreamer
 * sinks and are left alone):
 *   /pidgin4/sound/method   "gsound" (default), "command" or "none"
 *   /pidgin4/sound/command  the command; "%s" is replaced by the quoted
 *                           file name (appended when there is no "%s")
 *   /pidgin4/sound/volume   0-100, 50 = the file's own level (as Pidgin 2)
 * "While away" is libpurple's /purple/sound/while_status.
 */

/**
 * Plays @filename now with the configured method, as the preferences
 * window's "Play" button does. Honours mute and the method, not the
 * status rules.
 */
void pidgin_sound_play_file(const char *filename);

/* Internals, public for tests/test-sound.c. */

/** Registers the shared and the /pidgin4/sound prefs (idempotent). */
void pidgin_sound_register_prefs(void);

/**
 * The file for @event: /pidgin/sound/file/<event> when set, else the
 * default sound in <libpurple datadir>/sounds/purple. Newly allocated.
 */
char *pidgin_sound_get_event_file(PurpleSoundEventID event);

/** The freedesktop sound naming spec name for @event (GSound event.id). */
const char *pidgin_sound_get_event_id(PurpleSoundEventID event);

/**
 * The custom command line for @filename: "%s" in @command replaced by the
 * shell-quoted @filename, or the quoted name appended. NULL if @command is
 * empty.
 */
char *pidgin_sound_build_command(const char *command, const char *filename);

/**
 * /pidgin4/sound/volume (0-100, 50 = 0 dB) as a canberra.volume string in
 * dB, or NULL for 0 (silent).
 */
char *pidgin_sound_volume_to_db(int volume);

/*@}*/

#endif /* _PIDGINSOUND_H_ */
