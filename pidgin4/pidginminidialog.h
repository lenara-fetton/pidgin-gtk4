/**
 * @file pidginminidialog.h A small dialog shown inside the buddy list
 * @ingroup pidgin
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
 * GTK 4 replacement for pidgin/minidialog.c: an icon, a bold title, a
 * description, optional extra content and a row of buttons, as a plain
 * GtkBox subclass. Add it to the buddy list with pidgin_blist_add_alert().
 *
 * A button added with pidgin_mini_dialog_add_button() runs its callback and
 * then closes the dialog (pidgin_mini_dialog_close()); a non-closing button
 * only runs its callback. Closing removes the dialog from its parent, which
 * disposes it unless someone else holds a reference; connect to "destroy"
 * for cleanup, as with Pidgin 2.
 */
#ifndef _PIDGINMINIDIALOG_H_
#define _PIDGINMINIDIALOG_H_

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define PIDGIN_TYPE_MINI_DIALOG (pidgin_mini_dialog_get_type())
G_DECLARE_FINAL_TYPE(PidginMiniDialog, pidgin_mini_dialog, PIDGIN, MINI_DIALOG, GtkBox)

typedef void (*PidginMiniDialogCallback)(PidginMiniDialog *mini_dialog,
                                         GtkButton *button,
                                         gpointer user_data);

/** @icon_name may be NULL (no icon), as may @description. */
PidginMiniDialog *pidgin_mini_dialog_new(const char *title,
                                         const char *description,
                                         const char *icon_name);

void pidgin_mini_dialog_set_title(PidginMiniDialog *mini_dialog, const char *title);
void pidgin_mini_dialog_set_description(PidginMiniDialog *mini_dialog,
                                        const char *description);
/** The description as Pango markup; links emit "activate-link" on the label. */
void pidgin_mini_dialog_set_description_markup(PidginMiniDialog *mini_dialog,
                                               const char *markup);
GtkLabel *pidgin_mini_dialog_get_description_label(PidginMiniDialog *mini_dialog);
void pidgin_mini_dialog_set_icon_name(PidginMiniDialog *mini_dialog, const char *icon_name);
void pidgin_mini_dialog_set_gicon(PidginMiniDialog *mini_dialog, GIcon *icon);

/** A vertical box between the description and the buttons, for extra widgets. */
GtkBox *pidgin_mini_dialog_get_contents(PidginMiniDialog *mini_dialog);
/** The number of widgets in the contents box. */
guint pidgin_mini_dialog_get_num_children(PidginMiniDialog *mini_dialog);

/** @text is a mnemonic label. Returns the button. */
GtkWidget *pidgin_mini_dialog_add_button(PidginMiniDialog *mini_dialog,
                                         const char *text,
                                         PidginMiniDialogCallback callback,
                                         gpointer user_data);
GtkWidget *pidgin_mini_dialog_add_non_closing_button(PidginMiniDialog *mini_dialog,
                                                     const char *text,
                                                     PidginMiniDialogCallback callback,
                                                     gpointer user_data);

/** Removes the dialog from its parent. */
void pidgin_mini_dialog_close(PidginMiniDialog *mini_dialog);

G_END_DECLS

#endif /* _PIDGINMINIDIALOG_H_ */
