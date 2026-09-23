/*
 * OMEMO (XEP-0384 v0.3, eu.siacs.conversations.axolotl) for libpurple
 *
 * Purple is the legal property of its developers, whose names are too numerous
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
 * Internal header shared by the OMEMO plugin's source files. Nothing here is
 * installed: the UI talks to the plugin through purple_plugin_ipc_call() and
 * the plugin's signals (see omemo.c).
 */
#ifndef PURPLE_OMEMO_H
#define PURPLE_OMEMO_H

#include <glib.h>

#include <signal_protocol.h>
#include <session_pre_key.h>

#include "account.h"
#include "xmlnode.h"

#define OMEMO_NS              "eu.siacs.conversations.axolotl"
#define OMEMO_NS_DEVICELIST   OMEMO_NS ".devicelist"
#define OMEMO_NS_BUNDLES      OMEMO_NS ".bundles:"
#define OMEMO_NS_NOTIFY       OMEMO_NS_DEVICELIST "+notify"

#define OMEMO_PREKEY_TARGET   100
#define OMEMO_PREKEY_MINIMUM  20

#define OMEMO_ERROR           (omemo_error_quark())

typedef enum {
	OMEMO_TRUST_UNDECIDED = 0,
	OMEMO_TRUST_TRUSTED,      /* trusted on first use */
	OMEMO_TRUST_VERIFIED,     /* fingerprint verified by the user */
	OMEMO_TRUST_UNTRUSTED
} OmemoTrust;

typedef enum {
	OMEMO_DECRYPT_OK = 0,      /* body replaced with the plaintext */
	OMEMO_DECRYPT_KEY_ONLY,    /* session set-up / heartbeat, no payload */
	OMEMO_DECRYPT_NOT_FOR_US,  /* no <key rid=our device> */
	OMEMO_DECRYPT_OWN_DEVICE,  /* sent by this very device, not keyed to us */
	OMEMO_DECRYPT_FAILED
} OmemoDecryptStatus;

typedef struct _OmemoStore OmemoStore;
typedef struct _OmemoAccount OmemoAccount;

typedef void (*OmemoNewIdentityFunc)(OmemoAccount *oa, const char *jid,
                                     guint32 device_id, const char *fingerprint,
                                     gpointer data);

struct _OmemoAccount {
	PurpleAccount *account;   /* NULL in the unit tests */
	char *jid;                /* our bare JID; the database key */
	OmemoStore *store;
	signal_protocol_store_context *store_ctx;

	guint32 device_id;
	guint32 registration_id;

	gboolean tofu;            /* trust new devices on first use */
	guint prekeys_removed;    /* bumped when libomemo-c consumes a pre-key */

	OmemoNewIdentityFunc new_identity_cb;
	gpointer new_identity_data;
};

/* One recipient device: a (bare JID, device id) pair. */
typedef struct {
	char *jid;
	guint32 device_id;
} OmemoAddress;

/* One row of omemo_store_list_devices(). */
typedef struct {
	guint32 device_id;
	gboolean active;          /* in the contact's current device list */
	gboolean has_key;         /* identity key known (bundle or message seen) */
	gboolean has_session;
	OmemoTrust trust;
	char *fingerprint;        /* NULL unless has_key */
} OmemoDeviceInfo;

GQuark omemo_error_quark(void);

/* omemo-crypto.c */
gboolean omemo_crypto_init(GError **error);
signal_context *omemo_signal_context(void);
void omemo_crypto_uninit(void);
void omemo_random_bytes(guint8 *buf, gsize len);
guint32 omemo_random_device_id(void);
gboolean omemo_aes_gcm_encrypt(const guint8 *key, gsize key_len,
                               const guint8 *iv, gsize iv_len,
                               const guint8 *in, gsize len,
                               guint8 **out, guint8 tag[16]);
gboolean omemo_aes_gcm_decrypt(const guint8 *key, gsize key_len,
                               const guint8 *iv, gsize iv_len,
                               const guint8 *in, gsize len,
                               const guint8 tag[16], guint8 **out);
char *omemo_fingerprint_format(const guint8 *key, gsize len);
gboolean omemo_aesgcm_parse_fragment(const char *url, guint8 *iv, gsize *iv_len,
                                     guint8 key[32]);
GByteArray *omemo_aesgcm_decrypt(const char *url, const guint8 *data, gsize len,
                                 GError **error);
char *omemo_aesgcm_encrypt_file(const char *path, char **fragment,
                                GError **error);

/* omemo-store.c */
OmemoStore *omemo_store_open(const char *filename, GError **error);
void omemo_store_close(OmemoStore *store);
OmemoAccount *omemo_account_new(OmemoStore *store, const char *jid,
                                PurpleAccount *account, GError **error);
void omemo_account_free(OmemoAccount *oa);
char *omemo_account_fingerprint(OmemoAccount *oa);
guint omemo_store_count_prekeys(OmemoAccount *oa);
gboolean omemo_account_replenish_prekeys(OmemoAccount *oa, guint minimum,
                                         guint target);
GList *omemo_store_load_prekeys(OmemoAccount *oa);
session_signed_pre_key *omemo_store_load_signed_prekey(OmemoAccount *oa);
signal_buffer *omemo_account_identity_public(OmemoAccount *oa);
gboolean omemo_store_device_list_known(OmemoAccount *oa, const char *jid);
GArray *omemo_store_get_devices(OmemoAccount *oa, const char *jid,
                                gboolean active_only);
GList *omemo_store_update_device_list(OmemoAccount *oa, const char *jid,
                                      const guint32 *ids, guint n);
gboolean omemo_store_has_session(OmemoAccount *oa, const char *jid,
                                 guint32 device_id);
OmemoTrust omemo_store_get_trust(OmemoAccount *oa, const char *jid,
                                 guint32 device_id, gboolean *has_key);
gboolean omemo_store_set_trust(OmemoAccount *oa, const char *jid,
                               guint32 device_id, OmemoTrust trust);
GList *omemo_store_list_devices(OmemoAccount *oa, const char *jid);
void omemo_device_info_free(OmemoDeviceInfo *info);
const char *omemo_trust_to_string(OmemoTrust trust);
gboolean omemo_trust_from_string(const char *str, OmemoTrust *trust);

/* omemo-xmpp.c */
xmlnode *omemo_xml_build_bundle(OmemoAccount *oa);
xmlnode *omemo_xml_build_devicelist(GArray *ids);
GArray *omemo_xml_parse_devicelist(xmlnode *list);
gboolean omemo_session_from_bundle(OmemoAccount *oa, const char *jid,
                                   guint32 device_id, xmlnode *bundle,
                                   GError **error);
xmlnode *omemo_encrypt(OmemoAccount *oa, const char *plaintext,
                       GArray *recipients, guint *n_keys, GError **error);
OmemoDecryptStatus omemo_decrypt(OmemoAccount *oa, xmlnode *encrypted,
                                 const char *sender, char **plaintext,
                                 char **reason);
void omemo_message_set_body(xmlnode *message, const char *text);
void omemo_message_strip_plaintext(xmlnode *message);
void omemo_message_apply_encrypted(xmlnode *message, xmlnode *encrypted);
OmemoDecryptStatus omemo_message_decrypt(OmemoAccount *oa, xmlnode *message,
                                         const char *sender);
guint32 omemo_message_sender_device(xmlnode *message);
void omemo_address_array_add(GArray *array, const char *jid, guint32 device_id);
GArray *omemo_address_array_new(void);

#endif /* PURPLE_OMEMO_H */
