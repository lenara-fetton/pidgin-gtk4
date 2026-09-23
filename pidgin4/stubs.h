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

#include "gtkconv.h"

/** pidgin/gtksavedstatuses.h. TODO(M5): the status editor and window. */
void pidgin_status_editor_show(gboolean edit, PurpleSavedStatus *saved_status);
void pidgin_status_window_show(void);

#endif /* _PIDGIN_STUBS_H_ */
