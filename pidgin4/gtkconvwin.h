/**
 * @file gtkconvwin.h GTK+ Conversation Window API
 * @ingroup pidgin
 * @see @ref gtkconversation-signals
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

/*
 * pidgin4 (M4b): a conversation window is a GtkApplicationWindow holding a
 * GtkPopoverMenuBar and a GtkNotebook with one page per
 * PidginConversation. Tabs can be reordered and dragged between windows
 * (or out of one, which makes a new window). The menubar is a GMenuModel
 * over the window's "conv" action group; its accelerators are set with
 * gtk_application_set_accels_for_action().
 *
 * The struct keeps Pidgin 2's name and its first members so the ported
 * plugins keep compiling; prefer the accessors.
 */
#ifndef _PIDGIN_CONVERSATION_WINDOW_H_
#define _PIDGIN_CONVERSATION_WINDOW_H_

#include <gtk/gtk.h>

#include "conversation.h"

typedef struct _PidginWindow       PidginWindow;
typedef struct _PidginConversation PidginConversation;

/**************************************************************************
 * @name Structures
 **************************************************************************/
/*@{*/

/**
 * A GTK+ representation of a graphical window containing one or more
 * conversations.
 */
struct _PidginWindow
{
	GtkWidget *window;           /**< The window (GtkApplicationWindow). */
	GtkWidget *notebook;         /**< The notebook of conversations.   */
	GList *gtkconvs;             /**< PidginConversation*, in tab order. */

	struct
	{
		GtkWidget *menubar;      /**< GtkPopoverMenuBar. */
		GMenu *model;            /**< The menubar's model. */
		GMenu *more;             /**< Conversation → More (prpl/plugins). */
		GtkWidget *typing_icon;  /**< Typing indicator next to the menubar. */
	} menu;

	/*< private >*/
	GSimpleActionGroup *actions; /**< "conv" actions (menubar, accels). */
	GSimpleActionGroup *more_actions;
	GtkWidget *tab_menu;         /**< The tab context menu popover. */
	PidginConversation *tab_menu_conv;
	gboolean closing;
	guint destroy_idle;
};

/*@}*/

/**************************************************************************
 * @name GTK+ Conversation Window API
 **************************************************************************/
/*@{*/

PidginWindow *pidgin_conv_window_new(void);
void pidgin_conv_window_destroy(PidginWindow *win);
/** The windows, newest last (the hidden window is not in the list). */
GList *pidgin_conv_windows_get_list(void);
void pidgin_conv_window_show(PidginWindow *win);
void pidgin_conv_window_hide(PidginWindow *win);
/** Presents the window (gtk_window_present: xdg-activation on Wayland). */
void pidgin_conv_window_raise(PidginWindow *win);
void pidgin_conv_window_switch_gtkconv(PidginWindow *win, PidginConversation *gtkconv);
void pidgin_conv_window_add_gtkconv(PidginWindow *win, PidginConversation *gtkconv);
void pidgin_conv_window_remove_gtkconv(PidginWindow *win, PidginConversation *gtkconv);
PidginConversation *pidgin_conv_window_get_gtkconv_at_index(const PidginWindow *win, int index);
PidginConversation *pidgin_conv_window_get_active_gtkconv(const PidginWindow *win);
PurpleConversation *pidgin_conv_window_get_active_conversation(const PidginWindow *win);
gboolean pidgin_conv_window_is_active_conversation(const PurpleConversation *conv);
gboolean pidgin_conv_window_has_focus(PidginWindow *win);
GList *pidgin_conv_window_get_gtkconvs(PidginWindow *win);
guint pidgin_conv_window_get_gtkconv_count(PidginWindow *win);
/** M7: the GtkApplicationWindow and the GtkNotebook (for plugins). */
GtkWidget *pidgin_conv_window_get_window(PidginWindow *win);
GtkWidget *pidgin_conv_window_get_notebook(PidginWindow *win);
PidginWindow *pidgin_conv_window_first_with_type(PurpleConversationType type);
PidginWindow *pidgin_conv_window_last_with_type(PurpleConversationType type);

/** TRUE for the window that holds hidden conversations (never shown). */
gboolean pidgin_conv_window_is_hidden(const PidginWindow *win);

/** Rebuilds the menubar state (actions' enabled/checked, More submenu). */
void pidgin_conv_window_update_menu(PidginWindow *win);

/** Moves tab @index to position @new_index. */
void pidgin_conv_window_move_tab(PidginWindow *win, int index, int new_index);

/*@}*/

/**************************************************************************
 * @name GTK+ Conversation Placement API
 **************************************************************************/
/*@{*/

typedef void (*PidginConvPlacementFunc)(PidginConversation *);

GList *pidgin_conv_placement_get_options(void);
void pidgin_conv_placement_add_fnc(const char *id, const char *name, PidginConvPlacementFunc fnc);
void pidgin_conv_placement_remove_fnc(const char *id);
const char *pidgin_conv_placement_get_name(const char *id);
PidginConvPlacementFunc pidgin_conv_placement_get_fnc(const char *id);
void pidgin_conv_placement_set_current_func(PidginConvPlacementFunc func);
PidginConvPlacementFunc pidgin_conv_placement_get_current_func(void);
void pidgin_conv_placement_place(PidginConversation *gtkconv);

/*@}*/

/* Internal: gtkconv.c <-> gtkconvwin.c. */
void pidgin_conv_windows_init(void);
void pidgin_conv_windows_uninit(void);
PidginWindow *pidgin_conv_window_get_hidden(void);

#endif /* _PIDGIN_CONVERSATION_WINDOW_H_ */
