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
 */

/*
 * "Send this file?": the confirmation shown before a pasted or dropped
 * image (or a dropped file) goes out as a file transfer (gtkconv.c,
 * "Images: paste and drop"; pref /pidgin4/images/confirm_file_send).
 *
 * A modal dialog transient for the conversation window (and a secondary
 * window, so tiling compositors float it) with a preview of the image
 * (capped to PIDGIN_FILE_CONFIRM_PREVIEW_MAX px, aspect kept) or, for
 * other files, the type's icon; the name, the dimensions, the size and
 * the format; the recipient; and Cancel (Escape) and Send (the default,
 * Enter).
 */
#ifndef _PIDGINFILECONFIRM_H_
#define _PIDGINFILECONFIRM_H_

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define PIDGIN_FILE_CONFIRM_PREVIEW_MAX 480

/** Called when Send is activated, just before the dialog is destroyed. */
typedef void (*PidginFileConfirmFunc)(gpointer user_data);

/**
 * Creates and presents the dialog. The file is @data (encoded image data,
 * e.g. a paste) or, if @data is NULL, the file at @path. @filename: the
 * name shown. @recipient and @account: shown as "Send to <recipient> via
 * <account>". @send is called only on Send; @destroy (may be NULL) is
 * called on @user_data when the dialog goes, however it goes (Send,
 * Cancel, Escape, closed or destroyed by its owner). Send is the window's
 * default widget. Returns the dialog (owned by GTK: destroy it with
 * gtk_window_destroy() to drop it).
 */
GtkWidget *pidgin_file_confirm_new(GtkWindow *parent, GBytes *data, const char *path,
                                   const char *filename, const char *recipient,
                                   const char *account, PidginFileConfirmFunc send,
                                   gpointer user_data, GDestroyNotify destroy);

G_END_DECLS

#endif /* _PIDGINFILECONFIRM_H_ */
