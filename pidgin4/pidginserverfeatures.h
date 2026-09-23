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
#ifndef _PIDGINSERVERFEATURES_H_
#define _PIDGINSERVERFEATURES_H_

/*
 * What the account's server supports, over the prpl's round-2 IPC
 * (doc/PIDGIN-UPGRADE.md, M8 "Landed (server features round 2)"):
 *   privacy-modes(account) -> GList of mode names
 *   status-invisible-supported(account) -> gboolean
 *   report-spam-supported(account) -> gboolean
 *   report-spam(account, jid, reason or NULL, guint abuse) -> gboolean
 * Any prpl that registers them is asked (prpl-jabber does); for the
 * others, and for accounts that aren't connected, nothing is known and
 * the UI offers everything, as before.
 */

#include <gtk/gtk.h>

#include "account.h"
#include "privacy.h"

G_BEGIN_DECLS

/**
 * The privacy modes the server can enforce, as a mask of
 * (1 << PurplePrivacyType). Every mode when nothing is known.
 */
guint pidgin_account_privacy_modes(PurpleAccount *account);

/** Whether @type is in @mask (from pidgin_account_privacy_modes()). */
#define PIDGIN_PRIVACY_MODE_SUPPORTED(mask, type) (((mask) & (1u << (type))) != 0)

/** TRUE if the mask leaves some mode out. */
gboolean pidgin_privacy_modes_restricted(guint mask);

/**
 * Whether the server supports invisibility: 1 yes, 0 no, -1 not known
 * (no such command, or the account isn't connected).
 */
int pidgin_account_invisible_supported(PurpleAccount *account);

/**
 * The note for an Invisible choice when a connected account can't be
 * invisible ("…you will appear available"), naming the accounts; or NULL.
 * With @account, only that account is considered (and not named).
 */
char *pidgin_invisible_unsupported_note(PurpleAccount *account);

/** Blocked: by the privacy mode, or on the deny list (which XEP-0191
 * fills whatever the mode). */
gboolean pidgin_account_is_blocked(PurpleAccount *account, const char *name);

/** Whether "Report spam and block…" is offered for @account. */
gboolean pidgin_account_report_spam_supported(PurpleAccount *account);

/** Calls report-spam; its result (FALSE also when there's no command). */
gboolean pidgin_account_report_spam(PurpleAccount *account, const char *jid,
                                    const char *reason, gboolean abuse);

/**
 * The "Report spam and block" dialog for @jid: an optional reason, an
 * "abuse" check box; the Report button calls report-spam. Returns the
 * window (its widgets are named pidgin-report-reason, -abuse, -send and
 * -cancel, for the selftest).
 */
GtkWidget *pidgin_report_spam_dialog_show(PurpleAccount *account, const char *jid,
                                          GtkWindow *parent);

/** Forgets which prpls have which commands (after registering some). */
void pidgin_server_features_reset_cache(void);

G_END_DECLS

#endif /* _PIDGINSERVERFEATURES_H_ */
