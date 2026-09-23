/*
 * OMEMO for libpurple: libgcrypt crypto provider, AES-GCM, aesgcm:// URLs
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
#include "internal.h"

#include <string.h>
#include <gcrypt.h>
#include <glib/gstdio.h>

#include "debug.h"

#include "omemo.h"

#define GCM_TAG_LEN 16

static signal_context *global_ctx = NULL;
static GRecMutex global_lock;

GQuark
omemo_error_quark(void)
{
	return g_quark_from_static_string("purple-omemo-error");
}

/**************************************************************************
 * signal_crypto_provider over libgcrypt
 **************************************************************************/

static int
omemo_random_cb(uint8_t *data, size_t len, void *user_data)
{
	gcry_randomize(data, len, GCRY_STRONG_RANDOM);
	return SG_SUCCESS;
}

static int
omemo_hmac_init_cb(void **hmac_context, const uint8_t *key, size_t key_len,
                   void *user_data)
{
	gcry_mac_hd_t hd;

	if (gcry_mac_open(&hd, GCRY_MAC_HMAC_SHA256, 0, NULL))
		return SG_ERR_UNKNOWN;
	if (gcry_mac_setkey(hd, key, key_len)) {
		gcry_mac_close(hd);
		return SG_ERR_UNKNOWN;
	}
	*hmac_context = hd;
	return SG_SUCCESS;
}

static int
omemo_hmac_update_cb(void *hmac_context, const uint8_t *data, size_t data_len,
                     void *user_data)
{
	return gcry_mac_write(hmac_context, data, data_len) ? SG_ERR_UNKNOWN
	                                                    : SG_SUCCESS;
}

static int
omemo_hmac_final_cb(void *hmac_context, signal_buffer **output,
                    void *user_data)
{
	guint8 md[32];
	size_t len = sizeof(md);

	if (gcry_mac_read(hmac_context, md, &len))
		return SG_ERR_UNKNOWN;
	*output = signal_buffer_create(md, len);
	return *output ? SG_SUCCESS : SG_ERR_NOMEM;
}

static void
omemo_hmac_cleanup_cb(void *hmac_context, void *user_data)
{
	if (hmac_context)
		gcry_mac_close(hmac_context);
}

static int
omemo_sha512_init_cb(void **digest_context, void *user_data)
{
	gcry_md_hd_t hd;

	if (gcry_md_open(&hd, GCRY_MD_SHA512, 0))
		return SG_ERR_UNKNOWN;
	*digest_context = hd;
	return SG_SUCCESS;
}

static int
omemo_sha512_update_cb(void *digest_context, const uint8_t *data,
                       size_t data_len, void *user_data)
{
	gcry_md_write(digest_context, data, data_len);
	return SG_SUCCESS;
}

static int
omemo_sha512_final_cb(void *digest_context, signal_buffer **output,
                      void *user_data)
{
	unsigned char *md = gcry_md_read(digest_context, GCRY_MD_SHA512);

	if (md == NULL)
		return SG_ERR_UNKNOWN;
	*output = signal_buffer_create(md, gcry_md_get_algo_dlen(GCRY_MD_SHA512));
	gcry_md_reset(digest_context);
	return *output ? SG_SUCCESS : SG_ERR_NOMEM;
}

static void
omemo_sha512_cleanup_cb(void *digest_context, void *user_data)
{
	if (digest_context)
		gcry_md_close(digest_context);
}

static int
omemo_aes_algo(size_t key_len)
{
	switch (key_len) {
		case 16: return GCRY_CIPHER_AES128;
		case 24: return GCRY_CIPHER_AES192;
		case 32: return GCRY_CIPHER_AES256;
		default: return 0;
	}
}

static int
omemo_aes_open(gcry_cipher_hd_t *hd, int cipher, const uint8_t *key,
               size_t key_len, const uint8_t *iv, size_t iv_len)
{
	int algo = omemo_aes_algo(key_len);
	int mode;
	gcry_error_t err;

	if (!algo)
		return SG_ERR_INVAL;

	if (cipher == SG_CIPHER_AES_CBC_PKCS5)
		mode = GCRY_CIPHER_MODE_CBC;
	else if (cipher == SG_CIPHER_AES_CTR_NOPADDING)
		mode = GCRY_CIPHER_MODE_CTR;
	else
		return SG_ERR_INVAL;

	if (gcry_cipher_open(hd, algo, mode, 0))
		return SG_ERR_UNKNOWN;

	err = gcry_cipher_setkey(*hd, key, key_len);
	if (!err) {
		if (mode == GCRY_CIPHER_MODE_CTR)
			err = gcry_cipher_setctr(*hd, iv, iv_len);
		else
			err = gcry_cipher_setiv(*hd, iv, iv_len);
	}
	if (err) {
		gcry_cipher_close(*hd);
		return SG_ERR_UNKNOWN;
	}
	return SG_SUCCESS;
}

static int
omemo_encrypt_cb(signal_buffer **output, int cipher,
                 const uint8_t *key, size_t key_len,
                 const uint8_t *iv, size_t iv_len,
                 const uint8_t *plaintext, size_t plaintext_len,
                 void *user_data)
{
	gcry_cipher_hd_t hd;
	guint8 *buf;
	size_t len = plaintext_len;
	int ret;

	ret = omemo_aes_open(&hd, cipher, key, key_len, iv, iv_len);
	if (ret)
		return ret;

	if (cipher == SG_CIPHER_AES_CBC_PKCS5) {
		/* PKCS#7 padding to the block size, always at least one byte */
		guint8 pad = 16 - (plaintext_len % 16);

		len = plaintext_len + pad;
		buf = g_malloc(len);
		memcpy(buf, plaintext, plaintext_len);
		memset(buf + plaintext_len, pad, pad);
	} else {
		buf = g_memdup2(plaintext, plaintext_len);
	}

	if (gcry_cipher_encrypt(hd, buf, len, NULL, 0)) {
		ret = SG_ERR_UNKNOWN;
	} else {
		*output = signal_buffer_create(buf, len);
		ret = *output ? SG_SUCCESS : SG_ERR_NOMEM;
	}

	gcry_cipher_close(hd);
	memset(buf, 0, len);
	g_free(buf);
	return ret;
}

static int
omemo_decrypt_cb(signal_buffer **output, int cipher,
                 const uint8_t *key, size_t key_len,
                 const uint8_t *iv, size_t iv_len,
                 const uint8_t *ciphertext, size_t ciphertext_len,
                 void *user_data)
{
	gcry_cipher_hd_t hd;
	guint8 *buf;
	size_t len = ciphertext_len;
	int ret;

	if (cipher == SG_CIPHER_AES_CBC_PKCS5 &&
	    (ciphertext_len == 0 || ciphertext_len % 16 != 0))
		return SG_ERR_INVAL;

	ret = omemo_aes_open(&hd, cipher, key, key_len, iv, iv_len);
	if (ret)
		return ret;

	buf = g_memdup2(ciphertext, ciphertext_len);
	if (gcry_cipher_decrypt(hd, buf, len, NULL, 0)) {
		ret = SG_ERR_UNKNOWN;
		goto out;
	}

	if (cipher == SG_CIPHER_AES_CBC_PKCS5) {
		guint8 pad = buf[len - 1];
		size_t i;

		if (pad == 0 || pad > 16 || pad > len) {
			ret = SG_ERR_UNKNOWN;
			goto out;
		}
		for (i = len - pad; i < len; i++) {
			if (buf[i] != pad) {
				ret = SG_ERR_UNKNOWN;
				goto out;
			}
		}
		len -= pad;
	}

	*output = signal_buffer_create(buf, len);
	ret = *output ? SG_SUCCESS : SG_ERR_NOMEM;

out:
	gcry_cipher_close(hd);
	memset(buf, 0, ciphertext_len);
	g_free(buf);
	return ret;
}

static void
omemo_lock_cb(void *user_data)
{
	g_rec_mutex_lock(&global_lock);
}

static void
omemo_unlock_cb(void *user_data)
{
	g_rec_mutex_unlock(&global_lock);
}

static void
omemo_log_cb(int level, const char *message, size_t len, void *user_data)
{
	PurpleDebugLevel plevel;

	switch (level) {
		case SG_LOG_ERROR:   plevel = PURPLE_DEBUG_ERROR; break;
		case SG_LOG_WARNING: plevel = PURPLE_DEBUG_WARNING; break;
		case SG_LOG_NOTICE:
		case SG_LOG_INFO:    plevel = PURPLE_DEBUG_INFO; break;
		default:             plevel = PURPLE_DEBUG_MISC; break;
	}
	purple_debug(plevel, "omemo", "libomemo-c: %.*s\n", (int)len, message);
}

gboolean
omemo_crypto_init(GError **error)
{
	signal_crypto_provider provider = {
		.random_func = omemo_random_cb,
		.hmac_sha256_init_func = omemo_hmac_init_cb,
		.hmac_sha256_update_func = omemo_hmac_update_cb,
		.hmac_sha256_final_func = omemo_hmac_final_cb,
		.hmac_sha256_cleanup_func = omemo_hmac_cleanup_cb,
		.sha512_digest_init_func = omemo_sha512_init_cb,
		.sha512_digest_update_func = omemo_sha512_update_cb,
		.sha512_digest_final_func = omemo_sha512_final_cb,
		.sha512_digest_cleanup_func = omemo_sha512_cleanup_cb,
		.encrypt_func = omemo_encrypt_cb,
		.decrypt_func = omemo_decrypt_cb,
		.user_data = NULL
	};

	if (global_ctx != NULL)
		return TRUE;

	/* libpurple itself does not use libgcrypt (ssl-nss is the TLS backend),
	 * so we may be the first user in the process. */
	if (!gcry_control(GCRYCTL_INITIALIZATION_FINISHED_P)) {
		if (!gcry_check_version(GCRYPT_VERSION)) {
			g_set_error(error, OMEMO_ERROR, 0,
			            "libgcrypt version mismatch (need %s)",
			            GCRYPT_VERSION);
			return FALSE;
		}
		gcry_control(GCRYCTL_DISABLE_SECMEM, 0);
		gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);
	}

	g_rec_mutex_init(&global_lock);

	if (signal_context_create(&global_ctx, NULL) != 0) {
		g_set_error(error, OMEMO_ERROR, 0, "signal_context_create failed");
		global_ctx = NULL;
		return FALSE;
	}
	signal_context_set_crypto_provider(global_ctx, &provider);
	signal_context_set_locking_functions(global_ctx, omemo_lock_cb,
	                                     omemo_unlock_cb);
	signal_context_set_log_function(global_ctx, omemo_log_cb);

	return TRUE;
}

signal_context *
omemo_signal_context(void)
{
	return global_ctx;
}

void
omemo_crypto_uninit(void)
{
	if (global_ctx == NULL)
		return;
	signal_context_destroy(global_ctx);
	global_ctx = NULL;
	g_rec_mutex_clear(&global_lock);
}

void
omemo_random_bytes(guint8 *buf, gsize len)
{
	gcry_randomize(buf, len, GCRY_STRONG_RANDOM);
}

guint32
omemo_random_device_id(void)
{
	guint32 id;

	do {
		omemo_random_bytes((guint8 *)&id, sizeof(id));
		id &= 0x7fffffff;
	} while (id == 0);

	return id;
}

/**************************************************************************
 * AES-GCM (message payloads and aesgcm:// media)
 **************************************************************************/

static gboolean
omemo_gcm_open(gcry_cipher_hd_t *hd, const guint8 *key, gsize key_len,
               const guint8 *iv, gsize iv_len)
{
	int algo = omemo_aes_algo(key_len);

	if (!algo || iv_len == 0)
		return FALSE;
	if (gcry_cipher_open(hd, algo, GCRY_CIPHER_MODE_GCM, 0))
		return FALSE;
	if (gcry_cipher_setkey(*hd, key, key_len) ||
	    gcry_cipher_setiv(*hd, iv, iv_len)) {
		gcry_cipher_close(*hd);
		return FALSE;
	}
	return TRUE;
}

gboolean
omemo_aes_gcm_encrypt(const guint8 *key, gsize key_len,
                      const guint8 *iv, gsize iv_len,
                      const guint8 *in, gsize len,
                      guint8 **out, guint8 tag[16])
{
	gcry_cipher_hd_t hd;
	guint8 *buf;

	if (!omemo_gcm_open(&hd, key, key_len, iv, iv_len))
		return FALSE;

	buf = g_malloc(len ? len : 1);
	if (len)
		memcpy(buf, in, len);

	if (gcry_cipher_final(hd) ||
	    gcry_cipher_encrypt(hd, buf, len, NULL, 0) ||
	    gcry_cipher_gettag(hd, tag, GCM_TAG_LEN)) {
		gcry_cipher_close(hd);
		g_free(buf);
		return FALSE;
	}

	gcry_cipher_close(hd);
	*out = buf;
	return TRUE;
}

gboolean
omemo_aes_gcm_decrypt(const guint8 *key, gsize key_len,
                      const guint8 *iv, gsize iv_len,
                      const guint8 *in, gsize len,
                      const guint8 tag[16], guint8 **out)
{
	gcry_cipher_hd_t hd;
	guint8 *buf;

	if (!omemo_gcm_open(&hd, key, key_len, iv, iv_len))
		return FALSE;

	/* One spare byte so callers can NUL-terminate text payloads. */
	buf = g_malloc0(len + 1);
	if (len)
		memcpy(buf, in, len);

	if (gcry_cipher_final(hd) ||
	    gcry_cipher_decrypt(hd, buf, len, NULL, 0) ||
	    gcry_cipher_checktag(hd, tag, GCM_TAG_LEN)) {
		gcry_cipher_close(hd);
		memset(buf, 0, len);
		g_free(buf);
		return FALSE;
	}

	gcry_cipher_close(hd);
	*out = buf;
	return TRUE;
}

/*
 * Legacy OMEMO fingerprints are the 32-byte Curve25519 public key (the
 * serialized form carries a 0x05 type prefix), shown as 8 groups of 8 hex
 * digits, which is what Conversations, Dino and Gajim display.
 */
char *
omemo_fingerprint_format(const guint8 *key, gsize len)
{
	GString *str;
	gsize i;

	if (key == NULL)
		return NULL;
	if (len == 33 && key[0] == 0x05) {
		key++;
		len--;
	}

	str = g_string_sized_new(len * 2 + len / 4);
	for (i = 0; i < len; i++) {
		if (i && i % 4 == 0)
			g_string_append_c(str, ' ');
		g_string_append_printf(str, "%02x", key[i]);
	}
	return g_string_free(str, FALSE);
}

/*
 * aesgcm://host/path#<iv><key>: hex, iv is 12 bytes (current Conversations)
 * or 16 bytes (older clients), key is 32 bytes (AES-256-GCM). The
 * ciphertext on the server has the 16-byte GCM tag appended.
 */
gboolean
omemo_aesgcm_parse_fragment(const char *url, guint8 *iv, gsize *iv_len,
                            guint8 key[32])
{
	const char *frag;
	gsize hexlen, i;

	g_return_val_if_fail(url != NULL, FALSE);

	frag = strrchr(url, '#');
	frag = frag ? frag + 1 : url;
	hexlen = strlen(frag);

	if (hexlen == 88)
		*iv_len = 12;
	else if (hexlen == 96)
		*iv_len = 16;
	else
		return FALSE;

	for (i = 0; i < hexlen / 2; i++) {
		int hi = g_ascii_xdigit_value(frag[2 * i]);
		int lo = g_ascii_xdigit_value(frag[2 * i + 1]);
		guint8 b;

		if (hi < 0 || lo < 0)
			return FALSE;
		b = (hi << 4) | lo;
		if (i < *iv_len)
			iv[i] = b;
		else
			key[i - *iv_len] = b;
	}
	return TRUE;
}

GByteArray *
omemo_aesgcm_decrypt(const char *url, const guint8 *data, gsize len,
                     GError **error)
{
	guint8 iv[16], key[32];
	gsize iv_len = 0;
	guint8 *plain = NULL;
	GByteArray *ret;

	if (url == NULL || !omemo_aesgcm_parse_fragment(url, iv, &iv_len, key)) {
		g_set_error_literal(error, OMEMO_ERROR, 0,
		                    _("Invalid aesgcm URL fragment"));
		return NULL;
	}
	if (data == NULL || len < GCM_TAG_LEN) {
		g_set_error_literal(error, OMEMO_ERROR, 0,
		                    _("Encrypted file is too short"));
		return NULL;
	}

	if (!omemo_aes_gcm_decrypt(key, sizeof(key), iv, iv_len,
	                           data, len - GCM_TAG_LEN,
	                           data + len - GCM_TAG_LEN, &plain)) {
		memset(key, 0, sizeof(key));
		g_set_error_literal(error, OMEMO_ERROR, 0,
		                    _("Decryption failed (wrong key or corrupted file)"));
		return NULL;
	}
	memset(key, 0, sizeof(key));

	ret = g_byte_array_new_take(plain, len - GCM_TAG_LEN);
	return ret;
}

char *
omemo_aesgcm_encrypt_file(const char *path, char **fragment, GError **error)
{
	guint8 iv[12], key[32], tag[GCM_TAG_LEN];
	gchar *contents = NULL;
	gsize len = 0;
	guint8 *cipher = NULL;
	gchar *outpath = NULL;
	GString *frag;
	gsize i;
	int fd;
	gboolean ok;

	g_return_val_if_fail(path != NULL, NULL);

	if (!g_file_get_contents(path, &contents, &len, error))
		return NULL;

	omemo_random_bytes(iv, sizeof(iv));
	omemo_random_bytes(key, sizeof(key));

	ok = omemo_aes_gcm_encrypt(key, sizeof(key), iv, sizeof(iv),
	                           (guint8 *)contents, len, &cipher, tag);
	memset(contents, 0, len);
	g_free(contents);
	if (!ok) {
		memset(key, 0, sizeof(key));
		g_set_error_literal(error, OMEMO_ERROR, 0, _("Encryption failed"));
		return NULL;
	}

	fd = g_file_open_tmp("pidgin4-omemo-XXXXXX", &outpath, error);
	if (fd < 0) {
		memset(key, 0, sizeof(key));
		g_free(cipher);
		return NULL;
	}
	close(fd);

	{
		gchar *blob = g_malloc(len + GCM_TAG_LEN);

		if (len)
			memcpy(blob, cipher, len);
		memcpy(blob + len, tag, GCM_TAG_LEN);
		ok = g_file_set_contents(outpath, blob, len + GCM_TAG_LEN, error);
		g_free(blob);
	}
	g_free(cipher);

	if (!ok) {
		memset(key, 0, sizeof(key));
		g_unlink(outpath);
		g_free(outpath);
		return NULL;
	}

	frag = g_string_sized_new(2 * (sizeof(iv) + sizeof(key)));
	for (i = 0; i < sizeof(iv); i++)
		g_string_append_printf(frag, "%02x", iv[i]);
	for (i = 0; i < sizeof(key); i++)
		g_string_append_printf(frag, "%02x", key[i]);
	memset(key, 0, sizeof(key));

	if (fragment)
		*fragment = g_string_free(frag, FALSE);
	else
		g_string_free(frag, TRUE);

	return outpath;
}
