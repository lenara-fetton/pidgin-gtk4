/*
 * OMEMO for libpurple: bundles, device lists and message (de)encryption
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
 * The functions here only transform xmlnodes and talk to the store; they
 * never touch a connection, so the unit tests can drive them directly.
 *
 * Legacy OMEMO (0.3) wire format:
 *
 *   <encrypted xmlns='eu.siacs.conversations.axolotl'>
 *     <header sid='SENDER_DEVICE'>
 *       <key rid='DEVICE' [prekey='true']>base64(SignalMessage)</key>...
 *       <iv>base64(12 bytes)</iv>
 *     </header>
 *     <payload>base64(AES-128-GCM(body))</payload>
 *   </encrypted>
 *
 * The Signal message in each <key> carries the 16-byte AES key followed by
 * the 16-byte GCM tag. Old clients sent a bare 16-byte key and appended the
 * tag to the payload; both forms are accepted.
 */
#include "internal.h"

#include <string.h>

#include "debug.h"
#include "util.h"

#include <curve.h>
#include <protocol.h>
#include <session_builder.h>
#include <session_cipher.h>

#include "omemo.h"

#define OMEMO_KEY_LEN 16
#define OMEMO_TAG_LEN 16
#define OMEMO_IV_LEN  12

#define OMEMO_FALLBACK_BODY \
	"I sent you an OMEMO encrypted message but your client doesn't seem " \
	"to support that."

/**************************************************************************
 * Helpers
 **************************************************************************/

GArray *
omemo_address_array_new(void)
{
	return g_array_new(FALSE, TRUE, sizeof(OmemoAddress));
}

void
omemo_address_array_add(GArray *array, const char *jid, guint32 device_id)
{
	OmemoAddress addr;

	addr.jid = g_strdup(jid);
	addr.device_id = device_id;
	g_array_append_val(array, addr);
}

static gboolean
omemo_parse_uint32(const char *str, guint32 *out)
{
	guint64 v;
	char *end = NULL;

	if (str == NULL || *str == '\0')
		return FALSE;
	v = g_ascii_strtoull(str, &end, 10);
	if (end == NULL || *end != '\0' || v == 0 || v > G_MAXUINT32)
		return FALSE;
	*out = (guint32)v;
	return TRUE;
}

static void
omemo_add_b64_child(xmlnode *parent, const char *name, const guint8 *data,
                    gsize len)
{
	xmlnode *node = xmlnode_new_child(parent, name);
	char *b64 = purple_base64_encode(data, len);

	xmlnode_insert_data(node, b64, -1);
	g_free(b64);
}

static guint8 *
omemo_node_b64(xmlnode *node, gsize *len)
{
	char *data;
	guint8 *ret;

	*len = 0;
	if (node == NULL)
		return NULL;
	data = xmlnode_get_data(node);
	if (data == NULL)
		return NULL;
	g_strstrip(data);
	ret = purple_base64_decode(data, len);
	g_free(data);
	if (ret != NULL && *len == 0) {
		g_free(ret);
		ret = NULL;
	}
	return ret;
}

static void
omemo_add_pubkey(xmlnode *parent, const char *name, ec_public_key *key)
{
	signal_buffer *buf = NULL;

	if (ec_public_key_serialize(&buf, key) == 0) {
		omemo_add_b64_child(parent, name, signal_buffer_data(buf),
		                    signal_buffer_len(buf));
		signal_buffer_free(buf);
	}
}

/**************************************************************************
 * Bundles and device lists
 **************************************************************************/

xmlnode *
omemo_xml_build_bundle(OmemoAccount *oa)
{
	session_signed_pre_key *spk;
	signal_buffer *identity;
	xmlnode *bundle, *node, *prekeys;
	GList *keys, *l;
	char *id;

	spk = omemo_store_load_signed_prekey(oa);
	identity = omemo_account_identity_public(oa);
	if (spk == NULL || identity == NULL) {
		if (spk)
			SIGNAL_UNREF(spk);
		if (identity)
			signal_buffer_free(identity);
		return NULL;
	}

	bundle = xmlnode_new("bundle");
	xmlnode_set_namespace(bundle, OMEMO_NS);

	omemo_add_pubkey(bundle, "signedPreKeyPublic",
	                 ec_key_pair_get_public(session_signed_pre_key_get_key_pair(spk)));
	node = xmlnode_get_child(bundle, "signedPreKeyPublic");
	id = g_strdup_printf("%u", session_signed_pre_key_get_id(spk));
	xmlnode_set_attrib(node, "signedPreKeyId", id);
	g_free(id);

	omemo_add_b64_child(bundle, "signedPreKeySignature",
	                    session_signed_pre_key_get_signature(spk),
	                    session_signed_pre_key_get_signature_len(spk));
	omemo_add_b64_child(bundle, "identityKey", signal_buffer_data(identity),
	                    signal_buffer_len(identity));

	prekeys = xmlnode_new_child(bundle, "prekeys");
	keys = omemo_store_load_prekeys(oa);
	for (l = keys; l; l = l->next) {
		session_pre_key *pk = l->data;
		signal_buffer *buf = NULL;

		if (ec_public_key_serialize(&buf,
		        ec_key_pair_get_public(session_pre_key_get_key_pair(pk))) == 0) {
			node = xmlnode_new_child(prekeys, "preKeyPublic");
			id = g_strdup_printf("%u", session_pre_key_get_id(pk));
			xmlnode_set_attrib(node, "preKeyId", id);
			g_free(id);
			{
				char *b64 = purple_base64_encode(signal_buffer_data(buf),
				                                 signal_buffer_len(buf));
				xmlnode_insert_data(node, b64, -1);
				g_free(b64);
			}
			signal_buffer_free(buf);
		}
		SIGNAL_UNREF(pk);
	}
	g_list_free(keys);

	SIGNAL_UNREF(spk);
	signal_buffer_free(identity);
	return bundle;
}

xmlnode *
omemo_xml_build_devicelist(GArray *ids)
{
	xmlnode *list = xmlnode_new("list");
	guint i;

	xmlnode_set_namespace(list, OMEMO_NS);
	for (i = 0; ids && i < ids->len; i++) {
		xmlnode *dev = xmlnode_new_child(list, "device");
		char *id = g_strdup_printf("%u", g_array_index(ids, guint32, i));

		xmlnode_set_attrib(dev, "id", id);
		g_free(id);
	}
	return list;
}

/* 'list' is the <list xmlns='eu.siacs.conversations.axolotl'/> element. */
GArray *
omemo_xml_parse_devicelist(xmlnode *list)
{
	GArray *ids = g_array_new(FALSE, FALSE, sizeof(guint32));
	xmlnode *dev;

	if (list == NULL)
		return ids;
	for (dev = xmlnode_get_child(list, "device"); dev;
	     dev = xmlnode_get_next_twin(dev)) {
		guint32 id, i;
		gboolean dup = FALSE;

		if (!omemo_parse_uint32(xmlnode_get_attrib(dev, "id"), &id))
			continue;
		for (i = 0; i < ids->len; i++)
			if (g_array_index(ids, guint32, i) == id)
				dup = TRUE;
		if (!dup)
			g_array_append_val(ids, id);
	}
	return ids;
}

static ec_public_key *
omemo_decode_pubkey(xmlnode *node)
{
	ec_public_key *key = NULL;
	guint8 *data;
	gsize len;

	data = omemo_node_b64(node, &len);
	if (data == NULL)
		return NULL;
	if (curve_decode_point(&key, data, len, omemo_signal_context()) != 0)
		key = NULL;
	g_free(data);
	return key;
}

/*
 * Build (or rebuild) a session with jid:device_id from their bundle, using
 * one of their pre-keys picked at random.
 */
gboolean
omemo_session_from_bundle(OmemoAccount *oa, const char *jid, guint32 device_id,
                          xmlnode *bundle, GError **error)
{
	signal_context *ctx = omemo_signal_context();
	signal_protocol_address addr = { jid, strlen(jid), (int32_t)device_id };
	xmlnode *spk_node, *prekeys, *pk;
	ec_public_key *spk = NULL, *identity = NULL, *prekey = NULL;
	session_pre_key_bundle *spkb = NULL;
	session_builder *builder = NULL;
	guint8 *signature = NULL;
	gsize signature_len = 0;
	guint32 spk_id = 0, pk_id = 0;
	GPtrArray *candidates;
	gboolean ok = FALSE;
	int ret;

	g_return_val_if_fail(bundle != NULL, FALSE);

	spk_node = xmlnode_get_child(bundle, "signedPreKeyPublic");
	if (spk_node == NULL ||
	    !omemo_parse_uint32(xmlnode_get_attrib(spk_node, "signedPreKeyId"),
	                        &spk_id)) {
		/* id 0 is allowed for the signed pre-key */
		if (spk_node == NULL ||
		    !purple_strequal(xmlnode_get_attrib(spk_node, "signedPreKeyId"),
		                     "0")) {
			g_set_error_literal(error, OMEMO_ERROR, 0,
			                    "bundle has no signed pre-key");
			return FALSE;
		}
		spk_id = 0;
	}
	spk = omemo_decode_pubkey(spk_node);
	identity = omemo_decode_pubkey(xmlnode_get_child(bundle, "identityKey"));
	signature = omemo_node_b64(xmlnode_get_child(bundle,
	                                             "signedPreKeySignature"),
	                           &signature_len);
	if (spk == NULL || identity == NULL || signature == NULL) {
		g_set_error_literal(error, OMEMO_ERROR, 0, "malformed bundle");
		goto out;
	}

	candidates = g_ptr_array_new();
	prekeys = xmlnode_get_child(bundle, "prekeys");
	if (prekeys != NULL) {
		for (pk = xmlnode_get_child(prekeys, "preKeyPublic"); pk;
		     pk = xmlnode_get_next_twin(pk))
			g_ptr_array_add(candidates, pk);
	}
	/* Try pre-keys in random order until one decodes. */
	while (candidates->len > 0 && prekey == NULL) {
		guint i;

		omemo_random_bytes((guint8 *)&i, sizeof(i));
		i %= candidates->len;
		pk = g_ptr_array_index(candidates, i);
		g_ptr_array_remove_index_fast(candidates, i);

		if (!omemo_parse_uint32(xmlnode_get_attrib(pk, "preKeyId"), &pk_id))
			continue;
		prekey = omemo_decode_pubkey(pk);
	}
	g_ptr_array_free(candidates, TRUE);
	if (prekey == NULL) {
		g_set_error_literal(error, OMEMO_ERROR, 0, "bundle has no pre-keys");
		goto out;
	}

	/* Legacy bundles carry no registration id; 0 is what everyone uses. */
	ret = session_pre_key_bundle_create(&spkb, 0, (int)device_id, pk_id, prekey,
	                                    spk_id, spk, signature, signature_len,
	                                    identity);
	if (ret != 0) {
		g_set_error(error, OMEMO_ERROR, ret, "cannot create bundle (%d)", ret);
		goto out;
	}

	ret = session_builder_create(&builder, oa->store_ctx, &addr, ctx);
	if (ret == 0)
		ret = session_builder_process_pre_key_bundle(builder, spkb);
	if (ret != 0) {
		g_set_error(error, OMEMO_ERROR, ret,
		            ret == SG_ERR_UNTRUSTED_IDENTITY ?
		            "identity key changed" :
		            ret == SG_ERR_INVALID_KEY ? "invalid bundle signature" :
		            "session setup failed (%d)", ret);
		goto out;
	}
	ok = TRUE;

out:
	if (builder)
		session_builder_free(builder);
	if (spkb)
		SIGNAL_UNREF(spkb);
	if (spk)
		SIGNAL_UNREF(spk);
	if (identity)
		SIGNAL_UNREF(identity);
	if (prekey)
		SIGNAL_UNREF(prekey);
	g_free(signature);
	return ok;
}

/**************************************************************************
 * Encryption
 **************************************************************************/

/*
 * Encrypt 'plaintext' for every address in 'recipients' (OmemoAddress) that
 * has a session. Returns the <encrypted/> element, or NULL (with 'error')
 * if not a single key could be produced. '*n_keys' gets the number of
 * <key/> elements.
 */
xmlnode *
omemo_encrypt(OmemoAccount *oa, const char *plaintext, GArray *recipients,
              guint *n_keys, GError **error)
{
	signal_context *ctx = omemo_signal_context();
	guint8 key[OMEMO_KEY_LEN + OMEMO_TAG_LEN], iv[OMEMO_IV_LEN];
	guint8 *payload = NULL;
	xmlnode *encrypted, *header, *knode;
	gsize len;
	guint i, count = 0;
	char *sid;

	g_return_val_if_fail(plaintext != NULL, NULL);

	len = strlen(plaintext);
	omemo_random_bytes(key, OMEMO_KEY_LEN);
	omemo_random_bytes(iv, sizeof(iv));
	if (!omemo_aes_gcm_encrypt(key, OMEMO_KEY_LEN, iv, sizeof(iv),
	                           (const guint8 *)plaintext, len, &payload,
	                           key + OMEMO_KEY_LEN)) {
		g_set_error_literal(error, OMEMO_ERROR, 0, "AES-GCM failed");
		return NULL;
	}

	encrypted = xmlnode_new("encrypted");
	xmlnode_set_namespace(encrypted, OMEMO_NS);
	header = xmlnode_new_child(encrypted, "header");
	sid = g_strdup_printf("%u", oa->device_id);
	xmlnode_set_attrib(header, "sid", sid);
	g_free(sid);

	for (i = 0; recipients && i < recipients->len; i++) {
		OmemoAddress *a = &g_array_index(recipients, OmemoAddress, i);
		signal_protocol_address addr = { a->jid, strlen(a->jid),
		                                 (int32_t)a->device_id };
		session_cipher *cipher = NULL;
		ciphertext_message *msg = NULL;
		signal_buffer *ser;
		char *rid, *b64;
		int ret;

		if (!omemo_store_has_session(oa, a->jid, a->device_id))
			continue;

		ret = session_cipher_create(&cipher, oa->store_ctx, &addr, ctx);
		if (ret == 0)
			ret = session_cipher_encrypt(cipher, key, sizeof(key), &msg);
		if (ret != 0) {
			purple_debug_warning("omemo", "cannot encrypt for %s:%u (%d)\n",
			                     a->jid, a->device_id, ret);
			if (cipher)
				session_cipher_free(cipher);
			continue;
		}

		ser = ciphertext_message_get_serialized(msg);
		knode = xmlnode_new_child(header, "key");
		rid = g_strdup_printf("%u", a->device_id);
		xmlnode_set_attrib(knode, "rid", rid);
		g_free(rid);
		if (ciphertext_message_get_type(msg) == CIPHERTEXT_PREKEY_TYPE)
			xmlnode_set_attrib(knode, "prekey", "true");
		b64 = purple_base64_encode(signal_buffer_data(ser),
		                           signal_buffer_len(ser));
		xmlnode_insert_data(knode, b64, -1);
		g_free(b64);

		SIGNAL_UNREF(msg);
		session_cipher_free(cipher);
		count++;
	}
	memset(key, 0, sizeof(key));

	if (n_keys)
		*n_keys = count;
	if (count == 0) {
		g_free(payload);
		xmlnode_free(encrypted);
		g_set_error_literal(error, OMEMO_ERROR, 0,
		                    _("no device to encrypt for"));
		return NULL;
	}

	omemo_add_b64_child(header, "iv", iv, sizeof(iv));
	omemo_add_b64_child(encrypted, "payload", payload, len);
	g_free(payload);
	return encrypted;
}

/**************************************************************************
 * Decryption
 **************************************************************************/

guint32
omemo_message_sender_device(xmlnode *message)
{
	xmlnode *enc = xmlnode_get_child_with_namespace(message, "encrypted",
	                                                OMEMO_NS);
	xmlnode *header = enc ? xmlnode_get_child(enc, "header") : NULL;
	guint32 sid = 0;

	if (header)
		omemo_parse_uint32(xmlnode_get_attrib(header, "sid"), &sid);
	return sid;
}

static const char *
omemo_sg_strerror(int ret)
{
	switch (ret) {
		case SG_ERR_DUPLICATE_MESSAGE: return _("duplicate message");
		case SG_ERR_INVALID_KEY_ID:    return _("unknown pre-key");
		case SG_ERR_INVALID_MAC:       return _("bad MAC");
		case SG_ERR_INVALID_MESSAGE:   return _("invalid message");
		case SG_ERR_NO_SESSION:        return _("no session");
		case SG_ERR_UNTRUSTED_IDENTITY:return _("identity key changed");
		case SG_ERR_LEGACY_MESSAGE:
		case SG_ERR_INVALID_VERSION:   return _("unsupported version");
		default:                       return _("session error");
	}
}

/*
 * Decrypt the <encrypted/> element 'encrypted' from 'sender' (a bare JID).
 * On OMEMO_DECRYPT_OK '*plaintext' is the message text; on
 * OMEMO_DECRYPT_FAILED '*reason' says why (localized).
 */
OmemoDecryptStatus
omemo_decrypt(OmemoAccount *oa, xmlnode *encrypted, const char *sender,
              char **plaintext, char **reason)
{
	signal_context *ctx = omemo_signal_context();
	xmlnode *header, *knode, *pnode;
	guint32 sid = 0;
	guint8 *kdata = NULL, *iv = NULL, *payload = NULL, *plain = NULL;
	gsize klen = 0, iv_len = 0, plen = 0;
	gboolean prekey = FALSE;
	session_cipher *cipher = NULL;
	signal_buffer *keybuf = NULL;
	signal_protocol_address addr;
	OmemoDecryptStatus status = OMEMO_DECRYPT_FAILED;
	const guint8 *key, *tag;
	gsize cipher_len;
	int ret;

	*plaintext = NULL;
	*reason = NULL;

	header = xmlnode_get_child(encrypted, "header");
	if (header == NULL ||
	    !omemo_parse_uint32(xmlnode_get_attrib(header, "sid"), &sid)) {
		*reason = g_strdup(_("malformed header"));
		return OMEMO_DECRYPT_FAILED;
	}

	for (knode = xmlnode_get_child(header, "key"); knode;
	     knode = xmlnode_get_next_twin(knode)) {
		guint32 rid;

		if (omemo_parse_uint32(xmlnode_get_attrib(knode, "rid"), &rid) &&
		    rid == oa->device_id)
			break;
	}
	if (knode == NULL) {
		if (sid == oa->device_id && purple_strequal(sender, oa->jid))
			return OMEMO_DECRYPT_OWN_DEVICE;
		*reason = g_strdup(_("the message was not encrypted for this device"));
		return OMEMO_DECRYPT_NOT_FOR_US;
	}

	{
		const char *pk = xmlnode_get_attrib(knode, "prekey");

		prekey = purple_strequal(pk, "true") || purple_strequal(pk, "1");
	}

	kdata = omemo_node_b64(knode, &klen);
	iv = omemo_node_b64(xmlnode_get_child(header, "iv"), &iv_len);
	if (kdata == NULL || iv == NULL) {
		*reason = g_strdup(_("malformed header"));
		goto out;
	}

	addr.name = sender;
	addr.name_len = strlen(sender);
	addr.device_id = (int32_t)sid;

	ret = session_cipher_create(&cipher, oa->store_ctx, &addr, ctx);
	if (ret != 0) {
		*reason = g_strdup(omemo_sg_strerror(ret));
		goto out;
	}

	if (prekey) {
		pre_key_signal_message *msg = NULL;

		ret = pre_key_signal_message_deserialize(&msg, kdata, klen, ctx);
		if (ret == 0) {
			ret = session_cipher_decrypt_pre_key_signal_message(cipher, msg,
			                                                    NULL, &keybuf);
			SIGNAL_UNREF(msg);
		}
	} else {
		signal_message *msg = NULL;

		ret = signal_message_deserialize(&msg, kdata, klen, ctx);
		if (ret == 0) {
			ret = session_cipher_decrypt_signal_message(cipher, msg, NULL,
			                                            &keybuf);
			SIGNAL_UNREF(msg);
		}
	}
	if (ret != 0) {
		*reason = g_strdup(omemo_sg_strerror(ret));
		purple_debug_warning("omemo", "cannot decrypt key from %s:%u: %d\n",
		                     sender, sid, ret);
		goto out;
	}

	pnode = xmlnode_get_child(encrypted, "payload");
	if (pnode == NULL) {
		status = OMEMO_DECRYPT_KEY_ONLY;
		goto out;
	}
	payload = omemo_node_b64(pnode, &plen);
	if (payload == NULL) {
		/* An empty <payload/> is a key transport element as well. */
		status = OMEMO_DECRYPT_KEY_ONLY;
		goto out;
	}

	key = signal_buffer_data(keybuf);
	if (signal_buffer_len(keybuf) >= OMEMO_KEY_LEN + OMEMO_TAG_LEN) {
		tag = key + OMEMO_KEY_LEN;
		cipher_len = plen;
	} else if (signal_buffer_len(keybuf) == OMEMO_KEY_LEN &&
	           plen >= OMEMO_TAG_LEN) {
		/* old style: the tag is appended to the payload */
		tag = payload + plen - OMEMO_TAG_LEN;
		cipher_len = plen - OMEMO_TAG_LEN;
	} else {
		*reason = g_strdup(_("bad key length"));
		goto out;
	}

	if (!omemo_aes_gcm_decrypt(key, OMEMO_KEY_LEN, iv, iv_len, payload,
	                           cipher_len, tag, &plain)) {
		*reason = g_strdup(_("payload authentication failed"));
		goto out;
	}
	if (!g_utf8_validate((const char *)plain, cipher_len, NULL)) {
		g_free(plain);
		*reason = g_strdup(_("payload is not UTF-8 text"));
		goto out;
	}
	*plaintext = (char *)plain;
	status = OMEMO_DECRYPT_OK;

out:
	if (keybuf)
		signal_buffer_bzero_free(keybuf);
	if (cipher)
		session_cipher_free(cipher);
	g_free(kdata);
	g_free(iv);
	g_free(payload);
	return status;
}

/**************************************************************************
 * Message stanza transforms
 **************************************************************************/

static void
omemo_remove_children(xmlnode *message, const char *name, const char *xmlns)
{
	xmlnode *child;

	while ((child = xmlns ? xmlnode_get_child_with_namespace(message, name,
	                                                         xmlns)
	                      : xmlnode_get_child(message, name)) != NULL)
		xmlnode_free(child);
}

void
omemo_message_set_body(xmlnode *message, const char *text)
{
	xmlnode *body;

	omemo_remove_children(message, "body", NULL);
	/* XHTML-IM in an OMEMO message is at best a fallback; never show it. */
	omemo_remove_children(message, "html", "http://jabber.org/protocol/xhtml-im");
	if (text == NULL)
		return;
	body = xmlnode_new_child(message, "body");
	xmlnode_insert_data(body, text, -1);
}

/* Remove everything that could carry the plaintext of an outgoing message. */
void
omemo_message_strip_plaintext(xmlnode *message)
{
	omemo_message_set_body(message, NULL);
}

void
omemo_message_apply_encrypted(xmlnode *message, xmlnode *encrypted)
{
	xmlnode *node;

	omemo_message_set_body(message, OMEMO_FALLBACK_BODY);
	xmlnode_insert_child(message, encrypted);

	if (xmlnode_get_child_with_namespace(message, "store",
	                                     "urn:xmpp:hints") == NULL) {
		node = xmlnode_new_child(message, "store");
		xmlnode_set_namespace(node, "urn:xmpp:hints");
	}
	omemo_remove_children(message, "encryption", "urn:xmpp:eme:0");
	node = xmlnode_new_child(message, "encryption");
	xmlnode_set_namespace(node, "urn:xmpp:eme:0");
	xmlnode_set_attrib(node, "namespace", OMEMO_NS);
	xmlnode_set_attrib(node, "name", "OMEMO");
}

/*
 * Decrypt 'message' in place: the <body/> gets the plaintext (or a
 * localized error text) and the OMEMO elements are removed, so the prpl
 * handles and logs it like any other message. Key-only messages lose their
 * body entirely and so are not displayed.
 */
OmemoDecryptStatus
omemo_message_decrypt(OmemoAccount *oa, xmlnode *message, const char *sender)
{
	xmlnode *enc;
	char *plaintext = NULL, *reason = NULL, *text;
	OmemoDecryptStatus status;

	enc = xmlnode_get_child_with_namespace(message, "encrypted", OMEMO_NS);
	if (enc == NULL)
		return OMEMO_DECRYPT_NOT_FOR_US;

	status = omemo_decrypt(oa, enc, sender, &plaintext, &reason);

	switch (status) {
		case OMEMO_DECRYPT_OK:
			omemo_message_set_body(message, plaintext);
			break;
		case OMEMO_DECRYPT_KEY_ONLY:
			omemo_message_set_body(message, NULL);
			break;
		case OMEMO_DECRYPT_OWN_DEVICE:
			/* The caller may know the plaintext; leave the stanza alone. */
			g_free(plaintext);
			return status;
		default:
			text = g_strdup_printf(_("[OMEMO: could not decrypt (%s)]"),
			                       reason ? reason : _("unknown error"));
			omemo_message_set_body(message, text);
			g_free(text);
			purple_debug_warning("omemo", "message from %s not decrypted: %s\n",
			                     sender, reason ? reason : "?");
			break;
	}

	omemo_remove_children(message, "encrypted", OMEMO_NS);
	omemo_remove_children(message, "encryption", "urn:xmpp:eme:0");

	if (plaintext) {
		memset(plaintext, 0, strlen(plaintext));
		g_free(plaintext);
	}
	g_free(reason);
	return status;
}
