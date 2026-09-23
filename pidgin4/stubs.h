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

#endif /* _PIDGIN_STUBS_H_ */
