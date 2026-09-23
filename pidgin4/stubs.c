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

#include "stubs.h"

/* TODO(M3): gtkblist.c. See stubs.h for why this must not be NULL. */
static PurpleBlistUiOps blist_ui_ops = { NULL };

PurpleBlistUiOps *
pidgin_blist_get_ui_ops(void)
{
	return &blist_ui_ops;
}
