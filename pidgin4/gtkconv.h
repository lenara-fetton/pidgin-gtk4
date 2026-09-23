/**
 * @file gtkconv.h GTK+ Conversation API
 * @ingroup pidgin
 * @see @ref gtkconv-signals
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
 * pidgin4 (M4b): the conversation UI on GTK 4. The message display is a
 * PidginMessageView, the entry a PidginComposeEntry with a
 * PidginFormatToolbar, the chat user list a GtkListView. The plugin API of
 * Pidgin 2 is not preserved as such (see M7 in doc/PIDGIN-UPGRADE.md), but
 * PidginConversation/PidginWindow keep their names and the members and
 * accessors the ported plugins use.
 *
 * Signals on pidgin_conversations_get_handle(), with Pidgin 2's names and
 * signatures: conversation-timestamp, displaying-im-msg, displayed-im-msg,
 * displaying-chat-msg, displayed-chat-msg (registered by the message view),
 * conversation-switched, conversation-hiding, conversation-displayed,
 * chat-nick-autocomplete and chat-nick-clicked. conversation-dragging is
 * gone (GtkNotebook moves tabs between windows itself).
 */
#ifndef _PIDGIN_CONVERSATION_H_
#define _PIDGIN_CONVERSATION_H_

#include <gtk/gtk.h>

#include "conversation.h"

#include "gtkconvwin.h"

typedef struct _PidginImPane       PidginImPane;
typedef struct _PidginChatPane     PidginChatPane;

/**
 * Unseen text states.
 */
typedef enum
{
	PIDGIN_UNSEEN_NONE,   /**< No unseen text in the conversation. */
	PIDGIN_UNSEEN_EVENT,  /**< Unseen events in the conversation.  */
	PIDGIN_UNSEEN_NO_LOG, /**< Unseen text with NO_LOG flag.       */
	PIDGIN_UNSEEN_TEXT,   /**< Unseen text in the conversation.    */
	PIDGIN_UNSEEN_NICK    /**< Unseen text and the nick was said.  */
} PidginUnseenState;

#define PIDGIN_CONVERSATION(conv) \
	((PidginConversation *)(conv)->ui_data)

#define PIDGIN_IS_PIDGIN_CONVERSATION(conv) \
	(purple_conversation_get_ui_ops(conv) == \
	 pidgin_conversations_get_conv_ui_ops())

/**************************************************************************
 * @name Structures
 **************************************************************************/
/*@{*/

/**
 * A GTK+ Instant Message pane.
 */
struct _PidginImPane
{
	GtkWidget *block;            /**< Unused (menu actions now). */
	GtkWidget *send_file;        /**< Unused (menu actions now). */

	guint typing_timer;

	/* Buddy icon (infopane) */
	GtkWidget *icon_container;
	GtkWidget *icon;             /**< GtkPicture. */
	gboolean show_icon;
	gboolean animate;
};

/**
 * GTK+ Chat panes.
 */
struct _PidginChatPane
{
	GtkWidget *count;            /**< "N people in room" label. */
	GtkWidget *list;             /**< GtkListView of the users. */
	GtkWidget *topic_text;       /**< GtkEntry. */

	/*< private >*/
	GListStore *users;           /**< PidginChatUser, sorted. */
	GHashTable *by_name;         /**< normalized name -> PidginChatUser. */
	GtkWidget *userlist_box;
	GtkWidget *menu;             /**< The user context menu popover. */
	char *menu_who;
	char *self_occupant_id;      /**< Our occupant-id, once seen. */
};

/**
 * A GTK+ representation of a graphical conversation.
 */
struct _PidginConversation
{
	PurpleConversation *active_conv;
	GList *convs;                /**< Always just active_conv in pidgin4. */
	GList *send_history;         /**< Unused (the entry keeps it). */

	PidginWindow *win;

	gboolean make_sound;

	GtkWidget *tab_cont;         /**< The notebook page. */
	GtkWidget *tabby;            /**< The tab label box. */
	GtkWidget *menu_tabby;       /**< Unused. */

	GtkWidget *imhtml;           /**< The PidginMessageView (Pidgin 2's name). */
	GtkTextBuffer *entry_buffer; /**< The compose entry's buffer. */
	GtkWidget *entry;            /**< The PidginComposeEntry. */

	gboolean auto_resize;
	gboolean entry_growing;

	GtkWidget *close;            /**< "x" on the tab. */
	GtkWidget *icon;             /**< Tab status icon (GtkImage). */
	GtkWidget *tab_label;
	GtkWidget *menu_icon;        /**< Unused. */
	GtkWidget *menu_label;       /**< Unused. */

	gpointer depr1;

	GtkWidget *lower_hbox;       /**< The compose area (toolbar, entry, send). */
	GtkWidget *toolbar;          /**< The PidginFormatToolbar. */

	PidginUnseenState unseen_state;
	guint unseen_count;

	union
	{
		PidginImPane   *im;
		PidginChatPane *chat;
	} u;

	time_t newday;
	GtkWidget *infopane_hbox;
	GtkWidget *infopane;         /**< The name/status label box. */

	/*< private >*/
	GtkWidget *infopane_name;
	GtkWidget *infopane_status;
	GtkWidget *infopane_prpl;
	GtkWidget *typing_label;
	GtkWidget *send_button;
	GtkWidget *banner;           /**< "Replying to"/"Editing" banner. */
	GtkWidget *banner_label;
	GtkWidget *history_spinner;
	GObject *replying;           /**< PidginMessage replied to. */
	GObject *editing;            /**< PidginMessage being corrected. */
	gboolean history_busy;       /**< Loading older messages. */
	gboolean history_exhausted;
	gboolean closing;
};

/*@}*/

/**************************************************************************
 * @name GTK+ Conversation API
 **************************************************************************/
/*@{*/

PurpleConversationUiOps *pidgin_conversations_get_conv_ui_ops(void);

void pidgin_conv_update_buddy_icon(PurpleConversation *conv);
void pidgin_conv_switch_active_conversation(PurpleConversation *conv);
void pidgin_conv_update_buttons_by_protocol(PurpleConversation *conv);

/**
 * Conversations of @type (PURPLE_CONV_TYPE_ANY for all) whose unseen state
 * is at least @min_state, only hidden ones if @hidden_only, at most
 * @max_count (0: all). Free the list, not the elements.
 */
GList *pidgin_conversations_find_unseen_list(PurpleConversationType type,
                                             PidginUnseenState min_state,
                                             gboolean hidden_only,
                                             guint max_count);

/** Sum of the unseen counts of the conversations of @type. */
guint pidgin_conversations_get_unseen_count(PurpleConversationType type,
                                            PidginUnseenState min_state);

/**
 * Appends one item per conversation in @convs (and "Show All") to @menu,
 * activating app.pidgin4-present-conv. Returns the number added. For the
 * tray (M6).
 */
guint pidgin_conversations_fill_menu(GMenu *menu, GList *convs);

void pidgin_conv_present_conversation(PurpleConversation *conv);
gboolean pidgin_conv_attach_to_conversation(PurpleConversation *conv);
PidginWindow *pidgin_conv_get_window(PidginConversation *gtkconv);

/** The icon for the conversation's tab (status, typing), transfer full. */
GIcon *pidgin_conv_get_tab_icon(PurpleConversation *conv, gboolean small_icon);

void pidgin_conv_new(PurpleConversation *conv);
gboolean pidgin_conv_is_hidden(PidginConversation *gtkconv);

/** Sets the unseen state (NONE clears it and the count). */
void pidgin_conv_set_unseen(PurpleConversation *conv, PidginUnseenState state);

/* Accessors for plugins */
GtkWidget *pidgin_conv_get_message_view(PidginConversation *gtkconv);
GtkWidget *pidgin_conv_get_entry(PidginConversation *gtkconv);
GtkWidget *pidgin_conv_get_toolbar(PidginConversation *gtkconv);
GtkWidget *pidgin_conv_get_tab_container(PidginConversation *gtkconv);
PurpleConversation *pidgin_conv_get_conversation(PidginConversation *gtkconv);

/** Called by the window when a tab becomes current or the window gets focus. */
void pidgin_conv_seen(PidginConversation *gtkconv);
/** Updates the tab label (name, colour for the unseen state, typing). */
void pidgin_conv_update_tab(PidginConversation *gtkconv);
/** Called by the window when a notebook page moved to it (DnD). */
void pidgin_conv_set_window(PidginConversation *gtkconv, PidginWindow *win);
/** Closes the conversation (as the tab's close button). */
void pidgin_conv_close(PidginConversation *gtkconv);

/* Window menu actions on the active conversation (gtkconvwin.c). */
void pidgin_conv_action(PidginConversation *gtkconv, const char *action);
/** Fills Conversation → More: the prpl's extended menu. */
void pidgin_conv_fill_more_menu(PidginConversation *gtkconv, GMenu *menu,
                                GSimpleActionGroup *group);
/** Whether @action ("send-file", "invite", ...) applies to @gtkconv now. */
gboolean pidgin_conv_action_enabled(PidginConversation *gtkconv, const char *action);

/*@}*/

/**************************************************************************
 * @name GTK+ Conversations Subsystem
 **************************************************************************/
/*@{*/

void *pidgin_conversations_get_handle(void);

/**
 * Registers the /pidgin/conversations prefs (Pidgin 2's types and
 * defaults) and the pidgin4 ones under /pidgin4/conversations. Called from
 * pidgin_prefs_init().
 */
void pidgin_conversations_prefs_init(void);

void pidgin_conversations_init(void);
void pidgin_conversations_uninit(void);

/**
 * No-op unless PIDGIN4_CONV_SELFTEST is set: exercises conversations,
 * the M8 signals, tabs and the index on a disabled account, then quits
 * (exit status 0 on success). See pidgin4/TESTING.md.
 */
void pidgin_conversations_selftest(void);

/*@}*/

#endif /* _PIDGIN_CONVERSATION_H_ */
