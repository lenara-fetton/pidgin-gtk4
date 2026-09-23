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
 * PidginAttachment: a file that belongs to a message row and is shown
 * under its text (pidgin_message_set_attachment()): a received file
 * transfer's image, shown inline under libpurple's "Transfer of file ...
 * complete" line. The row's text (and so the HTML log and the message
 * index) is never changed; the attachment is display only.
 *
 * pidgin_attachment_widget_new() builds what the row shows: for an image
 * a picture (a thumbnail at most PIDGIN_ATTACHMENT_THUMB_SIZE px, loaded
 * off the main thread; files above PIDGIN_ATTACHMENT_MAX_FILE_SIZE are
 * not decoded) that opens the file when clicked.
 */
#ifndef _PIDGINATTACHMENT_H_
#define _PIDGINATTACHMENT_H_

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define PIDGIN_ATTACHMENT_THUMB_SIZE 320
#define PIDGIN_ATTACHMENT_MAX_FILE_SIZE (64 * 1024 * 1024)

typedef enum
{
	PIDGIN_ATTACHMENT_NONE,
	PIDGIN_ATTACHMENT_IMAGE
} PidginAttachmentKind;

#define PIDGIN_TYPE_ATTACHMENT (pidgin_attachment_get_type())
G_DECLARE_FINAL_TYPE(PidginAttachment, pidgin_attachment, PIDGIN, ATTACHMENT, GObject)

/**
 * What a file is, from @content_type (a MIME type; ignored when NULL, ""
 * or application/octet-stream) or else from the extension of @name (a
 * file name, a path or a URL; a URL's query and fragment are ignored).
 */
PidginAttachmentKind pidgin_attachment_classify(const char *name, const char *content_type);

/** A local file (its name and size are read from @path). */
PidginAttachment *pidgin_attachment_new_for_file(const char *path, PidginAttachmentKind kind);

PidginAttachmentKind pidgin_attachment_get_kind(PidginAttachment *attachment);
/** The local path, or NULL. */
const char *pidgin_attachment_get_path(PidginAttachment *attachment);
/** The file:// (or remote) URI. */
const char *pidgin_attachment_get_uri(PidginAttachment *attachment);
/** The display name (the file's basename). */
const char *pidgin_attachment_get_name(PidginAttachment *attachment);
/** The size in bytes, or -1 if unknown. */
goffset pidgin_attachment_get_size(PidginAttachment *attachment);

/** The widget a message row shows for @attachment. */
GtkWidget *pidgin_attachment_widget_new(PidginAttachment *attachment);

/**
 * Opens @attachment with the desktop's default application
 * (GtkFileLauncher for a local file, GtkUriLauncher otherwise). @widget
 * gives the parent window.
 */
void pidgin_attachment_open(GtkWidget *widget, PidginAttachment *attachment);

/**
 * TEST ONLY: called instead of launching anything; @action is "open",
 * and @target the path or URI. NULL restores launching.
 */
typedef void (*PidginAttachmentLaunchHook)(const char *action, const char *target);
void pidgin_attachment_set_launch_hook(PidginAttachmentLaunchHook hook);

G_END_DECLS

#endif /* _PIDGINATTACHMENT_H_ */
