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

#include "blist.h"

/**
 * TODO(M3): replaced by gtkblist.c.
 *
 * An all-NULL PurpleBlistUiOps. It must still be set: libpurple only
 * saves blist.xml through the blist UI ops, and purple_blist_set_ui_ops()
 * fills the NULL save_node/remove_node/save_account slots with its own
 * savers. With no blist UI ops at all, buddy list and privacy changes
 * (including buddies a prpl adds while connected) would never be written.
 */
PurpleBlistUiOps *pidgin_blist_get_ui_ops(void);

/**
 * TODO(M5): replaced by gtkpounce.c. Registers the pounce handler so that
 * pounces.xml round-trips (see stubs.c) and runs execute-command actions.
 */
void pidgin_pounces_init(void);

#endif /* _PIDGIN_STUBS_H_ */
