/*
 * pidgin4: the in-process selftest protocol plugin (see selftest-prpl.c).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef _PIDGIN_SELFTEST_PRPL_H_
#define _PIDGIN_SELFTEST_PRPL_H_

#include "account.h"
#include "ft.h"
#include "plugin.h"

#define PIDGIN_SELFTEST_PRPL_ID "prpl-pidgin4-selftest"

/** Registers and loads the protocol (idempotent). NULL if it didn't load. */
PurplePlugin *pidgin_selftest_prpl_register(void);
/** Unloads and destroys it, and forgets the recorded calls. */
void pidgin_selftest_prpl_unregister(void);

/** A new account on it, added, enabled for pidgin4 and connected. */
PurpleAccount *pidgin_selftest_account_new(const char *username);
/** Disables and deletes it. */
void pidgin_selftest_account_remove(PurpleAccount *account);

/**
 * The arguments of the last call of an IPC command ("send-reaction", ...)
 * or of the "op" command ("cmd-op"), joined with '|', or NULL.
 */
const char *pidgin_selftest_prpl_get_call(const char *command);
/** Forgets the last call of @command (get_call() returns NULL again). */
void pidgin_selftest_prpl_clear_call(const char *command);

/**
 * Toggles OPT_PROTO_IM_IMAGE and file transfer (send_file,
 * can_receive_file, chat_send_file, chat_can_receive_file; recorded as
 * "send-file" and "chat-send-file"). The defaults: images, no files. Call
 * pidgin_conv_update_buttons_by_protocol() after.
 */
void pidgin_selftest_prpl_set_caps(gboolean im_images, gboolean files);

/**
 * The transfer the last send_file / chat_send_file with a file started
 * (accepted, never progressing), or NULL. The test ends it (e.g.
 * purple_xfer_cancel_local()) and then calls forget_xfer().
 */
PurpleXfer *pidgin_selftest_prpl_get_last_xfer(void);
void pidgin_selftest_prpl_forget_xfer(void);

/** Runs the main loop for @ms milliseconds. */
void pidgin_selftest_spin(guint ms);

#endif /* _PIDGIN_SELFTEST_PRPL_H_ */
