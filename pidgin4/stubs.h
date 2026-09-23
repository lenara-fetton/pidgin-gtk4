/*
 * pidgin4: temporary stand-ins for UI parts that later milestones port.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef _PIDGIN_STUBS_H_
#define _PIDGIN_STUBS_H_

/**
 * TODO(M5): replaced by gtkpounce.c. Registers the pounce handler so that
 * pounces.xml round-trips (see stubs.c) and runs execute-command actions.
 */
void pidgin_pounces_init(void);

/*
 * M6: what the tray (gtkdocklet.c) and the notifications (pidginnotify.c)
 * need from later milestones. stubs.c defines them as weak symbols; the
 * real ones (M4b's gtkconv.c, M5's gtksavedstatuses.c) replace them at
 * link time without any change here.
 */
#include "conversation.h"
#include "savedstatuses.h"

#if __has_include("gtkconv.h")
#include "gtkconv.h"
#else
/* The names and values of pidgin/gtkconv.h. */
typedef enum
{
	PIDGIN_UNSEEN_NONE,
	PIDGIN_UNSEEN_EVENT,
	PIDGIN_UNSEEN_NO_LOG,
	PIDGIN_UNSEEN_TEXT,
	PIDGIN_UNSEEN_NICK
} PidginUnseenState;

/**
 * pidgin/gtkconv.h: the conversations of @type with an unseen state of at
 * least @min_state (@max_count 0: all). TODO(M4b): gtkconv.c. The weak
 * fallback reads the "unseen-state" conversation data, which Pidgin 2's
 * gtkconv.c keeps for conversations without a window.
 */
GList *pidgin_conversations_find_unseen_list(PurpleConversationType type,
                                             PidginUnseenState min_state,
                                             gboolean hidden_only,
                                             guint max_count);
#endif

/**
 * The number of unseen messages in @conv (PidginConversation's
 * unseen_count). TODO(M4b): gtkconv.c. The weak fallback reads the
 * "unseen-count" conversation data (as Pidgin 2's docklet did for
 * conversations without a window).
 */
guint pidgin_conversations_get_unseen_count(PurpleConversation *conv);

/** pidgin/gtksavedstatuses.h. TODO(M5): the status editor and window. */
void pidgin_status_editor_show(gboolean edit, PurpleSavedStatus *saved_status);
void pidgin_status_window_show(void);

#endif /* _PIDGIN_STUBS_H_ */
