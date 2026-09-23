/*
 * pidgin4: temporary stand-ins for UI parts that later milestones port.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Every stub names the milestone that replaces it. UI ops that are simply
 * not set in M2 (and what libpurple does without them) are listed in
 * gtkmain.c, pidgin_ui_init().
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "blist.h"
#include "debug.h"
#include "pounce.h"

#include "stubs.h"

/* TODO(M3): gtkblist.c. See stubs.h for why this must not be NULL. */
static PurpleBlistUiOps blist_ui_ops = { NULL };

PurpleBlistUiOps *
pidgin_blist_get_ui_ops(void)
{
	return &blist_ui_ops;
}

/**************************************************************************
 * Buddy pounces. TODO(M5): gtkpounce.c (editor, pounce list, and the
 * open-window, popup-notify, send-message and play-sound actions).
 **************************************************************************/

/*
 * The UI must register its pounce actions on every new pounce, as
 * pidgin/gtkpounce.c:new_pounce() does: while pounces.xml is parsed,
 * libpurple only keeps an action's <param>s if that action is registered
 * already, so without this the "command" of an execute-command action
 * would be dropped and pounces.xml rewritten without it.
 */
static void
new_pounce(PurplePounce *pounce)
{
	purple_pounce_action_register(pounce, "open-window");
	purple_pounce_action_register(pounce, "popup-notify");
	purple_pounce_action_register(pounce, "send-message");
	purple_pounce_action_register(pounce, "execute-command");
	purple_pounce_action_register(pounce, "play-sound");
}

static void
free_pounce(PurplePounce *pounce)
{
}

static void
pounce_cb(PurplePounce *pounce, PurplePounceEvent events, void *data)
{
	const char *pouncee = purple_pounce_get_pouncee(pounce);

	/* Only execute-command so far, run like Pidgin 2 does it (sh -c,
	 * in the locale's encoding). */
	if (purple_pounce_action_is_enabled(pounce, "execute-command")) {
		const char *command = purple_pounce_action_get_attribute(pounce,
				"execute-command", "command");

		if (command != NULL) {
			char *localecmd = g_locale_from_utf8(command, -1, NULL, NULL, NULL);
			char *argv[] = { "sh", "-c", localecmd, NULL };
			GError *error = NULL;

			if (localecmd != NULL &&
			    !g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
			                   NULL, NULL, NULL, &error)) {
				purple_debug_error("pounce", "Could not run '%s': %s\n",
				                   command, error->message);
				g_error_free(error);
			}
			g_free(localecmd);
		}
	}

	purple_debug_info("pounce", "Pounce on %s fired (events 0x%x); only "
	                  "execute-command is implemented before M5\n",
	                  pouncee, events);
}

void
pidgin_pounces_init(void)
{
	purple_pounces_register_handler(PIDGIN_UI, pounce_cb, new_pounce,
	                                free_pounce);
}
