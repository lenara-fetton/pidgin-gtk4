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
 * Also audio and video: a received file transfer, an XMPP file share (a
 * message that is one URL) or a Discord attachment URL.
 *
 * pidgin_attachment_widget_new() builds what the row shows:
 *  - an image: a picture (a thumbnail at most PIDGIN_ATTACHMENT_THUMB_SIZE
 *    px, loaded off the main thread; files above
 *    PIDGIN_ATTACHMENT_MAX_FILE_SIZE are not decoded) that opens the file
 *    when clicked;
 *  - audio or video: a media card: an icon, the name, the size if known,
 *    "Play" (the desktop's default player: GtkFileLauncher for a file,
 *    GtkUriLauncher for a URL) and, for a file, "Open Folder". When GTK
 *    has a media backend (pidgin_media_backend_available()) and
 *    /pidgin4/media/inline_playback is on (the default), the card also
 *    embeds a GtkVideo (controls, no autoplay, at most
 *    PIDGIN_ATTACHMENT_VIDEO_WIDTH px wide); without one (GTK built
 *    without GStreamer) only the card shows. The player is given a stream
 *    opened asynchronously, never the GFile (GTK's GStreamer backend
 *    fails an assertion on a file it can't open); if the file can't be
 *    opened, or the backend reports an error (not media, no codec), the
 *    player is removed and the card stays.
 */
#ifndef _PIDGINATTACHMENT_H_
#define _PIDGINATTACHMENT_H_

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define PIDGIN_ATTACHMENT_THUMB_SIZE 320
#define PIDGIN_ATTACHMENT_MAX_FILE_SIZE (64 * 1024 * 1024)
#define PIDGIN_ATTACHMENT_VIDEO_WIDTH 480

typedef enum
{
	PIDGIN_ATTACHMENT_NONE,
	PIDGIN_ATTACHMENT_IMAGE,
	PIDGIN_ATTACHMENT_AUDIO,
	PIDGIN_ATTACHMENT_VIDEO
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

/** A remote file (@size in bytes, or -1). The name is the URL's last path
 * segment. */
PidginAttachment *pidgin_attachment_new_for_uri(const char *uri, PidginAttachmentKind kind,
                                                goffset size);

PidginAttachmentKind pidgin_attachment_get_kind(PidginAttachment *attachment);
/** The local path, or NULL. */
const char *pidgin_attachment_get_path(PidginAttachment *attachment);
/** The file:// (or remote) URI. */
const char *pidgin_attachment_get_uri(PidginAttachment *attachment);
/** The display name (the file's basename). */
const char *pidgin_attachment_get_name(PidginAttachment *attachment);
/** The size in bytes, or -1 if unknown. */
goffset pidgin_attachment_get_size(PidginAttachment *attachment);

/*
 * File shares described by metadata (XEP-0447 stateless file sharing,
 * XEP-0385 SIMS; the jabber prpl's sfs-* meta keys): the card shows at
 * once, from the metadata alone: the name, size, type, a thumbnail
 * decoded from the sfs-thumbnail data: URI, space for an image of the
 * given dimensions, and the description as a caption. Images are then
 * loaded from the URL through the image loader (if it allows the URI) and
 * the downloaded bytes checked against sfs-hash ("algo:base64";
 * sha-256, sha-512 and sha-1 can be checked); the card says "verified"
 * or "hash mismatch". Audio and video get the media card; other files a
 * file card with Open.
 */
typedef enum
{
	PIDGIN_ATTACHMENT_HASH_NONE,      /* no hash, or nothing downloaded yet */
	PIDGIN_ATTACHMENT_HASH_PENDING,   /* checking */
	PIDGIN_ATTACHMENT_HASH_VERIFIED,
	PIDGIN_ATTACHMENT_HASH_MISMATCH,
	PIDGIN_ATTACHMENT_HASH_UNCHECKED  /* an algorithm we can't compute, or
	                                     the bytes aren't available */
} PidginAttachmentHashState;

/**
 * An attachment from a message's sfs-* metadata, or NULL when @meta has
 * neither sfs-url nor sfs-name.
 */
PidginAttachment *pidgin_attachment_new_for_share(GHashTable *meta);

/** TRUE for one made by pidgin_attachment_new_for_share(). */
gboolean pidgin_attachment_is_share(PidginAttachment *attachment);
/** The MIME type the sender gave, or NULL. */
const char *pidgin_attachment_get_media_type(PidginAttachment *attachment);
/** The image/video dimensions the sender gave (0 when unknown). */
void pidgin_attachment_get_dimensions(PidginAttachment *attachment, int *width, int *height);
/** The sender's description, or NULL. */
const char *pidgin_attachment_get_description(PidginAttachment *attachment);
/** The thumbnail from the metadata, or NULL. */
GdkTexture *pidgin_attachment_get_thumbnail(PidginAttachment *attachment);
/** The full image once loaded ("notify::texture"), or NULL. */
GdkTexture *pidgin_attachment_get_texture(PidginAttachment *attachment);
/** "algo:base64", or NULL. */
const char *pidgin_attachment_get_hash(PidginAttachment *attachment);
/** "notify::hash-state" when it changes. */
PidginAttachmentHashState pidgin_attachment_get_hash_state(PidginAttachment *attachment);

/**
 * Checks @data (the downloaded file) against the hash in a worker thread
 * and sets the hash state. Called for a loaded image; public for tests.
 */
void pidgin_attachment_verify_bytes(PidginAttachment *attachment, GBytes *data);

/** The widget a message row shows for @attachment. */
GtkWidget *pidgin_attachment_widget_new(PidginAttachment *attachment);

/**
 * Opens @attachment with the desktop's default application
 * (GtkFileLauncher for a local file, GtkUriLauncher otherwise). @widget
 * gives the parent window.
 */
void pidgin_attachment_open(GtkWidget *widget, PidginAttachment *attachment);

/** Plays @attachment with the desktop's default player (as open). */
void pidgin_attachment_play(GtkWidget *widget, PidginAttachment *attachment);

/**
 * Whether GTK can play media inline: GTK picks its media backend (the
 * GStreamer one, when GTK is built with it) when a GtkMediaFile is made;
 * without one it makes a stub that fails at once with "GTK could not find
 * a media module". So: a GtkMediaFile for a bundled 0.1 s silent WAV
 * (resource media/silence.wav) that has no error right away means there
 * is a backend. Checked once; an error that arrives later (the backend
 * can't decode even that) turns it off for the next cards.
 */
gboolean pidgin_media_backend_available(void);

/** TEST ONLY: from now on pidgin_media_backend_available() is FALSE (the
 *  path of a GTK built without GStreamer). */
void pidgin_media_backend_disable_for_tests(void);

/**
 * TEST ONLY: called instead of launching anything; @action is "open",
 * "play" or "open-folder", and @target the path or URI. NULL restores
 * launching.
 */
typedef void (*PidginAttachmentLaunchHook)(const char *action, const char *target);
void pidgin_attachment_set_launch_hook(PidginAttachmentLaunchHook hook);

G_END_DECLS

#endif /* _PIDGINATTACHMENT_H_ */
