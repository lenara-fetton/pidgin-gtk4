/*
 * System tray icon (aka docklet) plugin for Purple
 *
 * Copyright (C) 2002-3 Robert McQueen <robot101@debian.org>
 * Copyright (C) 2003 Herman Bloggs <hermanator12002@yahoo.com>
 * Inspired by a similar plugin by:
 *  John (J5) Palmieri <johnp@martianrock.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02111-1301, USA.
 */

#ifndef _GTKDOCKLET_H_
#define _GTKDOCKLET_H_

#include <gio/gio.h>

#include "status.h"

/*
 * pidgin4: the docklet logic of Pidgin 2 (status and pending icons, the
 * unread tooltip, the menu) with one implementation of the UI ops, the
 * StatusNotifierItem in gtkdocklet-sni.c (pidginsni.c). Blinking is gone:
 * pending messages set the item's NeedsAttention status instead, which
 * hosts show with the attention icon.
 *
 * The menu is a GMenuModel with the actions in pidgin_docklet_get_actions()
 * under the "tray." prefix, exported to the tray host as dbusmenu.
 *
 * Prefs: the shared /pidgin/docklet/show ("always", "pending", "never";
 * same meaning as in Pidgin 2) and /pidgin/docklet/blink (kept, unused),
 * and pidgin4's /pidgin4/docklet/show, which overrides the shared one when
 * it is not empty (default: empty).
 */

struct docklet_ui_ops
{
	void (*create)(void);
	void (*destroy)(void);
	void (*update_icon)(PurpleStatusPrimitive, gboolean, gboolean);
	void (*blank_icon)(void);
	void (*set_tooltip)(gchar *);
	/* GtkMenuPositionFunc in Pidgin 2. Unused: the tray host places the
	 * menu. */
	gpointer position_menu;
};


/* functions in gtkdocklet.c */
void pidgin_docklet_update_icon(void);
/* 1: left click, 2: middle click, 3: right click (as in Pidgin 2) */
void pidgin_docklet_clicked(int);
void pidgin_docklet_embedded(void);
void pidgin_docklet_remove(void);
void pidgin_docklet_set_ui_ops(struct docklet_ui_ops *);
void pidgin_docklet_init(void);
void pidgin_docklet_uninit(void);
void*pidgin_docklet_get_handle(void);

/* function in gtkdocklet-sni.c */
void docklet_ui_init(void);

/* pidgin4 additions */

/**
 * TRUE while the tray icon is shown permanently (a StatusNotifierWatcher
 * accepted it and the show pref is "always"). Closing the buddy list then
 * hides it instead of quitting.
 */
gboolean pidgin_docklet_is_embedded(void);

/**
 * TRUE if the buddy list should not be shown at startup: it was hidden in
 * the tray when pidgin4 last quit and a tray host is running now.
 */
gboolean pidgin_docklet_start_hidden(void);

/** The tray menu and its actions ("tray." prefix). */
GMenuModel *pidgin_docklet_get_menu(void);
GActionGroup *pidgin_docklet_get_actions(void);

/** Rebuilds the dynamic parts of the menu (unread list, statuses, plugins). */
void pidgin_docklet_refresh_menu(void);

/** Unread messages in conversations with unseen text (IMs) or nick (chats). */
guint pidgin_docklet_get_unread_count(void);

/** Presents the next conversation with unread messages; FALSE if none. */
gboolean pidgin_docklet_present_pending(void);

/**
 * Test hook: with PIDGIN4_DOCKLET_SELFTEST set, marks a conversation as
 * unread (conversation data), checks the pending state and the menu, and
 * clears it again. Logs the result; changes no saved data.
 */
void pidgin_docklet_selftest(void);

#endif /* _GTKDOCKLET_H_ */
