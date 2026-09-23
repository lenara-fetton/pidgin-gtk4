/*
 * pidgin4
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

/* PLACEHOLDER (M5 scaffolding): the pounce handler moved here from stubs.c;
 * replaced by the pounce manager and editor. */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "debug.h"
#include "pounce.h"

#include "gtkpounce.h"

static int handle;

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
	purple_debug_info("pounce", "placeholder: pounce on %s fired\n",
	                  purple_pounce_get_pouncee(pounce));
}

void
pidgin_pounce_editor_show(PurpleAccount *account, const char *name,
                          PurplePounce *cur_pounce)
{
	purple_debug_info("gtkpounce", "placeholder\n");
}

void pidgin_pounces_manager_show(void) { purple_debug_info("gtkpounce", "placeholder\n"); }
void pidgin_pounces_manager_hide(void) { }
void *pidgin_pounces_get_handle(void) { return &handle; }
void pidgin_pounces_uninit(void) { }
void pidgin_pounces_selftest(void) { }

void
pidgin_pounces_init(void)
{
	purple_pounces_register_handler(PIDGIN_UI, pounce_cb, new_pounce,
	                                free_pounce);
}
