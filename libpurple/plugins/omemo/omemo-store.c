/*
 * OMEMO for libpurple: SQLite state store (<profile>/pidgin4/omemo.db)
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
 * Everything is keyed by "account", our own bare JID, so one database serves
 * every XMPP account in the profile. Record blobs are the libomemo-c
 * serializations, so the schema never needs to know their layout.
 */
#include "internal.h"

#include <string.h>
#include <sqlite3.h>
#include <glib/gstdio.h>

#include "debug.h"

#include <key_helper.h>
#include <curve.h>
#include <ratchet.h>

#include "omemo.h"

#define OMEMO_SCHEMA_VERSION 1

struct _OmemoStore {
	sqlite3 *db;
};

static const char *schema_sql =
	"CREATE TABLE IF NOT EXISTS meta ("
	"  key TEXT PRIMARY KEY, value TEXT);"
	"CREATE TABLE IF NOT EXISTS own ("
	"  account TEXT PRIMARY KEY,"
	"  device_id INTEGER NOT NULL,"
	"  registration_id INTEGER NOT NULL,"
	"  identity_public BLOB NOT NULL,"
	"  identity_private BLOB NOT NULL,"
	"  signed_prekey_id INTEGER NOT NULL,"
	"  next_prekey_id INTEGER NOT NULL);"
	"CREATE TABLE IF NOT EXISTS prekeys ("
	"  account TEXT NOT NULL, id INTEGER NOT NULL, record BLOB NOT NULL,"
	"  PRIMARY KEY (account, id));"
	"CREATE TABLE IF NOT EXISTS signed_prekeys ("
	"  account TEXT NOT NULL, id INTEGER NOT NULL, record BLOB NOT NULL,"
	"  created INTEGER NOT NULL,"
	"  PRIMARY KEY (account, id));"
	"CREATE TABLE IF NOT EXISTS sessions ("
	"  account TEXT NOT NULL, jid TEXT NOT NULL, device_id INTEGER NOT NULL,"
	"  record BLOB NOT NULL, user_record BLOB,"
	"  PRIMARY KEY (account, jid, device_id));"
	"CREATE TABLE IF NOT EXISTS identities ("
	"  account TEXT NOT NULL, jid TEXT NOT NULL, device_id INTEGER NOT NULL,"
	"  identity_key BLOB NOT NULL,"
	"  trust TEXT NOT NULL DEFAULT 'undecided',"
	"  first_seen INTEGER NOT NULL,"
	"  PRIMARY KEY (account, jid, device_id));"
	"CREATE TABLE IF NOT EXISTS devices ("
	"  account TEXT NOT NULL, jid TEXT NOT NULL, device_id INTEGER NOT NULL,"
	"  active INTEGER NOT NULL DEFAULT 1, last_seen INTEGER NOT NULL,"
	"  PRIMARY KEY (account, jid, device_id));"
	"CREATE TABLE IF NOT EXISTS device_lists ("
	"  account TEXT NOT NULL, jid TEXT NOT NULL, updated INTEGER NOT NULL,"
	"  PRIMARY KEY (account, jid));";

/**************************************************************************
 * SQLite helpers
 **************************************************************************/

static sqlite3_stmt *
omemo_sql(OmemoAccount *oa, const char *sql)
{
	sqlite3_stmt *stmt = NULL;

	if (sqlite3_prepare_v2(oa->store->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
		purple_debug_error("omemo", "SQL prepare failed: %s (%s)\n",
		                   sqlite3_errmsg(oa->store->db), sql);
		return NULL;
	}
	sqlite3_bind_text(stmt, 1, oa->jid, -1, SQLITE_TRANSIENT);
	return stmt;
}

/* Run a statement that returns no rows; finalizes it. */
static gboolean
omemo_sql_done(OmemoAccount *oa, sqlite3_stmt *stmt)
{
	int rc;

	if (stmt == NULL)
		return FALSE;
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
		purple_debug_error("omemo", "SQL step failed: %s\n",
		                   sqlite3_errmsg(oa->store->db));
		return FALSE;
	}
	return TRUE;
}

static void
omemo_bind_address(sqlite3_stmt *stmt, const signal_protocol_address *addr)
{
	sqlite3_bind_text(stmt, 2, addr->name, addr->name_len, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, addr->device_id);
}

static signal_buffer *
omemo_column_buffer(sqlite3_stmt *stmt, int col)
{
	const void *data = sqlite3_column_blob(stmt, col);
	int len = sqlite3_column_bytes(stmt, col);

	if (data == NULL)
		return NULL;
	return signal_buffer_create(data, len);
}

OmemoStore *
omemo_store_open(const char *filename, GError **error)
{
	OmemoStore *store;
	sqlite3 *db = NULL;
	char *dir, *err = NULL;
	gboolean created;

	dir = g_path_get_dirname(filename);
	if (g_mkdir_with_parents(dir, 0700) != 0) {
		g_set_error(error, OMEMO_ERROR, 0, "Cannot create %s: %s", dir,
		            g_strerror(errno));
		g_free(dir);
		return NULL;
	}
	g_free(dir);

	created = !g_file_test(filename, G_FILE_TEST_EXISTS);

	if (sqlite3_open_v2(filename, &db,
	                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
	                    NULL) != SQLITE_OK) {
		g_set_error(error, OMEMO_ERROR, 0, "Cannot open %s: %s", filename,
		            db ? sqlite3_errmsg(db) : "out of memory");
		sqlite3_close(db);
		return NULL;
	}
	if (created)
		g_chmod(filename, 0600);

	sqlite3_busy_timeout(db, 2000);

	if (sqlite3_exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;",
	                 NULL, NULL, NULL) != SQLITE_OK ||
	    sqlite3_exec(db, schema_sql, NULL, NULL, &err) != SQLITE_OK) {
		g_set_error(error, OMEMO_ERROR, 0, "Cannot initialize %s: %s",
		            filename, err ? err : sqlite3_errmsg(db));
		sqlite3_free(err);
		sqlite3_close(db);
		return NULL;
	}
	{
		char *sql = g_strdup_printf("INSERT OR IGNORE INTO meta (key, value) "
		                            "VALUES ('schema_version', '%d');",
		                            OMEMO_SCHEMA_VERSION);
		sqlite3_exec(db, sql, NULL, NULL, NULL);
		g_free(sql);
	}

	store = g_new0(OmemoStore, 1);
	store->db = db;
	return store;
}

void
omemo_store_close(OmemoStore *store)
{
	if (store == NULL)
		return;
	sqlite3_close(store->db);
	g_free(store);
}

/**************************************************************************
 * Trust
 **************************************************************************/

const char *
omemo_trust_to_string(OmemoTrust trust)
{
	switch (trust) {
		case OMEMO_TRUST_TRUSTED:   return "trusted";
		case OMEMO_TRUST_VERIFIED:  return "verified";
		case OMEMO_TRUST_UNTRUSTED: return "untrusted";
		case OMEMO_TRUST_UNDECIDED:
		default:                    return "undecided";
	}
}

gboolean
omemo_trust_from_string(const char *str, OmemoTrust *trust)
{
	if (purple_strequal(str, "undecided"))
		*trust = OMEMO_TRUST_UNDECIDED;
	else if (purple_strequal(str, "trusted"))
		*trust = OMEMO_TRUST_TRUSTED;
	else if (purple_strequal(str, "verified"))
		*trust = OMEMO_TRUST_VERIFIED;
	else if (purple_strequal(str, "untrusted"))
		*trust = OMEMO_TRUST_UNTRUSTED;
	else
		return FALSE;
	return TRUE;
}

static gboolean
omemo_store_jid_has_verified(OmemoAccount *oa, const char *jid)
{
	sqlite3_stmt *stmt = omemo_sql(oa,
		"SELECT 1 FROM identities WHERE account = ?1 AND jid = ?2 "
		"AND trust = 'verified' LIMIT 1;");
	gboolean ret;

	if (stmt == NULL)
		return FALSE;
	sqlite3_bind_text(stmt, 2, jid, -1, SQLITE_TRANSIENT);
	ret = sqlite3_step(stmt) == SQLITE_ROW;
	sqlite3_finalize(stmt);
	return ret;
}

OmemoTrust
omemo_store_get_trust(OmemoAccount *oa, const char *jid, guint32 device_id,
                      gboolean *has_key)
{
	sqlite3_stmt *stmt = omemo_sql(oa,
		"SELECT trust FROM identities WHERE account = ?1 AND jid = ?2 "
		"AND device_id = ?3;");
	OmemoTrust trust = OMEMO_TRUST_UNDECIDED;
	gboolean found = FALSE;

	if (stmt != NULL) {
		sqlite3_bind_text(stmt, 2, jid, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 3, device_id);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			found = TRUE;
			omemo_trust_from_string(
				(const char *)sqlite3_column_text(stmt, 0), &trust);
		}
		sqlite3_finalize(stmt);
	}
	if (has_key)
		*has_key = found;
	return trust;
}

gboolean
omemo_store_set_trust(OmemoAccount *oa, const char *jid, guint32 device_id,
                      OmemoTrust trust)
{
	sqlite3_stmt *stmt = omemo_sql(oa,
		"UPDATE identities SET trust = ?4 WHERE account = ?1 AND jid = ?2 "
		"AND device_id = ?3;");
	gboolean ok;

	if (stmt == NULL)
		return FALSE;
	sqlite3_bind_text(stmt, 2, jid, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, device_id);
	sqlite3_bind_text(stmt, 4, omemo_trust_to_string(trust), -1,
	                  SQLITE_STATIC);
	ok = omemo_sql_done(oa, stmt);
	return ok && sqlite3_changes(oa->store->db) > 0;
}

/**************************************************************************
 * Session store callbacks
 **************************************************************************/

static int
omemo_load_session_cb(signal_buffer **record, signal_buffer **user_record,
                      const signal_protocol_address *address, void *user_data)
{
	OmemoAccount *oa = user_data;
	sqlite3_stmt *stmt = omemo_sql(oa,
		"SELECT record, user_record FROM sessions WHERE account = ?1 "
		"AND jid = ?2 AND device_id = ?3;");
	int ret = 0;

	if (stmt == NULL)
		return SG_ERR_UNKNOWN;
	omemo_bind_address(stmt, address);
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		*record = omemo_column_buffer(stmt, 0);
		if (sqlite3_column_type(stmt, 1) != SQLITE_NULL)
			*user_record = omemo_column_buffer(stmt, 1);
		ret = *record ? 1 : SG_ERR_NOMEM;
	}
	sqlite3_finalize(stmt);
	return ret;
}

static int
omemo_get_sub_device_sessions_cb(signal_int_list **sessions, const char *name,
                                 size_t name_len, void *user_data)
{
	OmemoAccount *oa = user_data;
	sqlite3_stmt *stmt = omemo_sql(oa,
		"SELECT device_id FROM sessions WHERE account = ?1 AND jid = ?2;");
	signal_int_list *list;

	if (stmt == NULL)
		return SG_ERR_UNKNOWN;
	list = signal_int_list_alloc();
	sqlite3_bind_text(stmt, 2, name, name_len, SQLITE_TRANSIENT);
	while (sqlite3_step(stmt) == SQLITE_ROW)
		signal_int_list_push_back(list, sqlite3_column_int(stmt, 0));
	sqlite3_finalize(stmt);

	*sessions = list;
	return signal_int_list_size(list);
}

static int
omemo_store_session_cb(const signal_protocol_address *address,
                       uint8_t *record, size_t record_len,
                       uint8_t *user_record, size_t user_record_len,
                       void *user_data)
{
	OmemoAccount *oa = user_data;
	sqlite3_stmt *stmt = omemo_sql(oa,
		"INSERT OR REPLACE INTO sessions (account, jid, device_id, record, "
		"user_record) VALUES (?1, ?2, ?3, ?4, ?5);");

	if (stmt == NULL)
		return SG_ERR_UNKNOWN;
	omemo_bind_address(stmt, address);
	sqlite3_bind_blob(stmt, 4, record, record_len, SQLITE_TRANSIENT);
	if (user_record)
		sqlite3_bind_blob(stmt, 5, user_record, user_record_len,
		                  SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 5);
	return omemo_sql_done(oa, stmt) ? SG_SUCCESS : SG_ERR_UNKNOWN;
}

static int
omemo_contains_session_cb(const signal_protocol_address *address,
                          void *user_data)
{
	OmemoAccount *oa = user_data;
	sqlite3_stmt *stmt = omemo_sql(oa,
		"SELECT 1 FROM sessions WHERE account = ?1 AND jid = ?2 "
		"AND device_id = ?3;");
	int ret;

	if (stmt == NULL)
		return 0;
	omemo_bind_address(stmt, address);
	ret = sqlite3_step(stmt) == SQLITE_ROW ? 1 : 0;
	sqlite3_finalize(stmt);
	return ret;
}

static int
omemo_delete_session_cb(const signal_protocol_address *address,
                        void *user_data)
{
	OmemoAccount *oa = user_data;
	sqlite3_stmt *stmt = omemo_sql(oa,
		"DELETE FROM sessions WHERE account = ?1 AND jid = ?2 "
		"AND device_id = ?3;");

	if (stmt == NULL)
		return SG_ERR_UNKNOWN;
	omemo_bind_address(stmt, address);
	if (!omemo_sql_done(oa, stmt))
		return SG_ERR_UNKNOWN;
	return sqlite3_changes(oa->store->db) > 0 ? 1 : 0;
}

static int
omemo_delete_all_sessions_cb(const char *name, size_t name_len,
                             void *user_data)
{
	OmemoAccount *oa = user_data;
	sqlite3_stmt *stmt = omemo_sql(oa,
		"DELETE FROM sessions WHERE account = ?1 AND jid = ?2;");

	if (stmt == NULL)
		return SG_ERR_UNKNOWN;
	sqlite3_bind_text(stmt, 2, name, name_len, SQLITE_TRANSIENT);
	if (!omemo_sql_done(oa, stmt))
		return SG_ERR_UNKNOWN;
	return sqlite3_changes(oa->store->db);
}

gboolean
omemo_store_has_session(OmemoAccount *oa, const char *jid, guint32 device_id)
{
	signal_protocol_address addr = { jid, strlen(jid), device_id };

	return omemo_contains_session_cb(&addr, oa) == 1;
}

/**************************************************************************
 * Pre-key and signed pre-key store callbacks
 **************************************************************************/

static int
omemo_load_prekey_generic(OmemoAccount *oa, const char *sql,
                          signal_buffer **record, uint32_t id)
{
	sqlite3_stmt *stmt = omemo_sql(oa, sql);
	int ret = SG_ERR_INVALID_KEY_ID;

	if (stmt == NULL)
		return SG_ERR_UNKNOWN;
	sqlite3_bind_int64(stmt, 2, id);
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		*record = omemo_column_buffer(stmt, 0);
		ret = *record ? SG_SUCCESS : SG_ERR_NOMEM;
	}
	sqlite3_finalize(stmt);
	return ret;
}

static int
omemo_contains_generic(OmemoAccount *oa, const char *sql, uint32_t id)
{
	sqlite3_stmt *stmt = omemo_sql(oa, sql);
	int ret;

	if (stmt == NULL)
		return 0;
	sqlite3_bind_int64(stmt, 2, id);
	ret = sqlite3_step(stmt) == SQLITE_ROW ? 1 : 0;
	sqlite3_finalize(stmt);
	return ret;
}

static int
omemo_load_prekey_cb(signal_buffer **record, uint32_t pre_key_id,
                     void *user_data)
{
	return omemo_load_prekey_generic(user_data,
		"SELECT record FROM prekeys WHERE account = ?1 AND id = ?2;",
		record, pre_key_id);
}

static int
omemo_store_prekey_cb(uint32_t pre_key_id, uint8_t *record, size_t record_len,
                      void *user_data)
{
	OmemoAccount *oa = user_data;
	sqlite3_stmt *stmt = omemo_sql(oa,
		"INSERT OR REPLACE INTO prekeys (account, id, record) "
		"VALUES (?1, ?2, ?3);");

	if (stmt == NULL)
		return SG_ERR_UNKNOWN;
	sqlite3_bind_int64(stmt, 2, pre_key_id);
	sqlite3_bind_blob(stmt, 3, record, record_len, SQLITE_TRANSIENT);
	return omemo_sql_done(oa, stmt) ? SG_SUCCESS : SG_ERR_UNKNOWN;
}

static int
omemo_contains_prekey_cb(uint32_t pre_key_id, void *user_data)
{
	return omemo_contains_generic(user_data,
		"SELECT 1 FROM prekeys WHERE account = ?1 AND id = ?2;",
		pre_key_id);
}

static int
omemo_remove_prekey_cb(uint32_t pre_key_id, void *user_data)
{
	OmemoAccount *oa = user_data;
	sqlite3_stmt *stmt = omemo_sql(oa,
		"DELETE FROM prekeys WHERE account = ?1 AND id = ?2;");

	if (stmt == NULL)
		return SG_ERR_UNKNOWN;
	sqlite3_bind_int64(stmt, 2, pre_key_id);
	if (!omemo_sql_done(oa, stmt))
		return SG_ERR_UNKNOWN;
	oa->prekeys_removed++;
	return SG_SUCCESS;
}

static int
omemo_load_signed_prekey_cb(signal_buffer **record, uint32_t id,
                            void *user_data)
{
	return omemo_load_prekey_generic(user_data,
		"SELECT record FROM signed_prekeys WHERE account = ?1 AND id = ?2;",
		record, id);
}

static int
omemo_store_signed_prekey_cb(uint32_t id, uint8_t *record, size_t record_len,
                             void *user_data)
{
	OmemoAccount *oa = user_data;
	sqlite3_stmt *stmt = omemo_sql(oa,
		"INSERT OR REPLACE INTO signed_prekeys (account, id, record, created) "
		"VALUES (?1, ?2, ?3, ?4);");

	if (stmt == NULL)
		return SG_ERR_UNKNOWN;
	sqlite3_bind_int64(stmt, 2, id);
	sqlite3_bind_blob(stmt, 3, record, record_len, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 4, (sqlite3_int64)time(NULL));
	return omemo_sql_done(oa, stmt) ? SG_SUCCESS : SG_ERR_UNKNOWN;
}

static int
omemo_contains_signed_prekey_cb(uint32_t id, void *user_data)
{
	return omemo_contains_generic(user_data,
		"SELECT 1 FROM signed_prekeys WHERE account = ?1 AND id = ?2;", id);
}

static int
omemo_remove_signed_prekey_cb(uint32_t id, void *user_data)
{
	OmemoAccount *oa = user_data;
	sqlite3_stmt *stmt = omemo_sql(oa,
		"DELETE FROM signed_prekeys WHERE account = ?1 AND id = ?2;");

	if (stmt == NULL)
		return SG_ERR_UNKNOWN;
	sqlite3_bind_int64(stmt, 2, id);
	return omemo_sql_done(oa, stmt) ? SG_SUCCESS : SG_ERR_UNKNOWN;
}

/**************************************************************************
 * Identity store callbacks
 **************************************************************************/

static int
omemo_get_identity_key_pair_cb(signal_buffer **public_data,
                               signal_buffer **private_data, void *user_data)
{
	OmemoAccount *oa = user_data;
	sqlite3_stmt *stmt = omemo_sql(oa,
		"SELECT identity_public, identity_private FROM own "
		"WHERE account = ?1;");
	int ret = SG_ERR_UNKNOWN;

	if (stmt == NULL)
		return SG_ERR_UNKNOWN;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		*public_data = omemo_column_buffer(stmt, 0);
		*private_data = omemo_column_buffer(stmt, 1);
		ret = (*public_data && *private_data) ? SG_SUCCESS : SG_ERR_NOMEM;
	}
	sqlite3_finalize(stmt);
	return ret;
}

static int
omemo_get_local_registration_id_cb(void *user_data, uint32_t *registration_id)
{
	OmemoAccount *oa = user_data;

	*registration_id = oa->registration_id;
	return SG_SUCCESS;
}

static GBytes *
omemo_store_get_identity(OmemoAccount *oa, const char *jid, gsize jid_len,
                         guint32 device_id)
{
	sqlite3_stmt *stmt = omemo_sql(oa,
		"SELECT identity_key FROM identities WHERE account = ?1 AND jid = ?2 "
		"AND device_id = ?3;");
	GBytes *ret = NULL;

	if (stmt == NULL)
		return NULL;
	sqlite3_bind_text(stmt, 2, jid, jid_len, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, device_id);
	if (sqlite3_step(stmt) == SQLITE_ROW)
		ret = g_bytes_new(sqlite3_column_blob(stmt, 0),
		                  sqlite3_column_bytes(stmt, 0));
	sqlite3_finalize(stmt);
	return ret;
}

static int
omemo_save_identity_cb(const signal_protocol_address *address,
                       uint8_t *key_data, size_t key_len, void *user_data)
{
	OmemoAccount *oa = user_data;
	char *jid = g_strndup(address->name, address->name_len);
	GBytes *old;
	sqlite3_stmt *stmt;
	OmemoTrust trust;
	gboolean is_new;
	int ret = SG_SUCCESS;

	if (key_data == NULL) {
		stmt = omemo_sql(oa, "DELETE FROM identities WHERE account = ?1 "
		                     "AND jid = ?2 AND device_id = ?3;");
		omemo_bind_address(stmt, address);
		ret = omemo_sql_done(oa, stmt) ? SG_SUCCESS : SG_ERR_UNKNOWN;
		g_free(jid);
		return ret;
	}

	old = omemo_store_get_identity(oa, address->name, address->name_len,
	                               address->device_id);
	if (old != NULL) {
		gsize old_len;
		const guint8 *old_data = g_bytes_get_data(old, &old_len);
		gboolean same = old_len == key_len &&
		                memcmp(old_data, key_data, key_len) == 0;

		g_bytes_unref(old);
		if (same) {
			g_free(jid);
			return SG_SUCCESS;
		}
		/* A changed key for a known device id: never keep the old trust. */
		purple_debug_warning("omemo", "identity key of %s:%u changed\n",
		                     jid, address->device_id);
		is_new = TRUE;
		trust = OMEMO_TRUST_UNDECIDED;
	} else {
		is_new = TRUE;
		if (oa->tofu && !omemo_store_jid_has_verified(oa, jid))
			trust = OMEMO_TRUST_TRUSTED;
		else
			trust = OMEMO_TRUST_UNDECIDED;
	}

	stmt = omemo_sql(oa,
		"INSERT OR REPLACE INTO identities (account, jid, device_id, "
		"identity_key, trust, first_seen) VALUES (?1, ?2, ?3, ?4, ?5, ?6);");
	if (stmt == NULL) {
		g_free(jid);
		return SG_ERR_UNKNOWN;
	}
	omemo_bind_address(stmt, address);
	sqlite3_bind_blob(stmt, 4, key_data, key_len, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, omemo_trust_to_string(trust), -1,
	                  SQLITE_STATIC);
	sqlite3_bind_int64(stmt, 6, (sqlite3_int64)time(NULL));
	if (!omemo_sql_done(oa, stmt))
		ret = SG_ERR_UNKNOWN;

	if (ret == SG_SUCCESS && is_new && oa->new_identity_cb) {
		char *fp = omemo_fingerprint_format(key_data, key_len);

		oa->new_identity_cb(oa, jid, address->device_id, fp,
		                    oa->new_identity_data);
		g_free(fp);
	}

	g_free(jid);
	return ret;
}

static int
omemo_is_trusted_identity_cb(const signal_protocol_address *address,
                             uint8_t *key_data, size_t key_len,
                             void *user_data)
{
	OmemoAccount *oa = user_data;
	GBytes *old;
	gsize old_len;
	const guint8 *old_data;
	int ret;

	/*
	 * Trust decisions (trusted/verified/untrusted) are enforced when we
	 * choose recipients, and messages from untrusted devices are still
	 * decrypted (as Conversations does). The only hard failure here is a
	 * device id whose identity key changed under us.
	 */
	old = omemo_store_get_identity(oa, address->name, address->name_len,
	                               address->device_id);
	if (old == NULL)
		return 1;

	old_data = g_bytes_get_data(old, &old_len);
	ret = (old_len == key_len && memcmp(old_data, key_data, key_len) == 0);
	g_bytes_unref(old);

	if (!ret)
		purple_debug_warning("omemo", "refusing changed identity key for "
		                     "%.*s:%d\n", (int)address->name_len,
		                     address->name, address->device_id);
	return ret;
}

/**************************************************************************
 * Own identity
 **************************************************************************/

static gboolean
omemo_account_load_own(OmemoAccount *oa)
{
	sqlite3_stmt *stmt = omemo_sql(oa,
		"SELECT device_id, registration_id FROM own WHERE account = ?1;");
	gboolean found = FALSE;

	if (stmt == NULL)
		return FALSE;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		oa->device_id = (guint32)sqlite3_column_int64(stmt, 0);
		oa->registration_id = (guint32)sqlite3_column_int64(stmt, 1);
		found = TRUE;
	}
	sqlite3_finalize(stmt);
	return found;
}

static gboolean
omemo_account_generate(OmemoAccount *oa, GError **error)
{
	signal_context *ctx = omemo_signal_context();
	ratchet_identity_key_pair *pair = NULL;
	session_signed_pre_key *spk = NULL;
	signal_buffer *pub = NULL, *priv = NULL, *spk_buf = NULL;
	sqlite3_stmt *stmt;
	gboolean ok = FALSE;

	if (signal_protocol_key_helper_generate_identity_key_pair(&pair, ctx) ||
	    signal_protocol_key_helper_generate_registration_id(
			&oa->registration_id, 0, ctx) ||
	    ec_public_key_serialize(&pub,
			ratchet_identity_key_pair_get_public(pair)) ||
	    ec_private_key_serialize(&priv,
			ratchet_identity_key_pair_get_private(pair)) ||
	    signal_protocol_key_helper_generate_signed_pre_key(&spk, pair, 1,
			(guint64)g_get_real_time() / 1000, ctx) ||
	    session_signed_pre_key_serialize(&spk_buf, spk)) {
		g_set_error_literal(error, OMEMO_ERROR, 0,
		                    "Cannot generate the OMEMO identity");
		goto out;
	}

	oa->device_id = omemo_random_device_id();

	sqlite3_exec(oa->store->db, "BEGIN;", NULL, NULL, NULL);

	stmt = omemo_sql(oa,
		"INSERT INTO own (account, device_id, registration_id, "
		"identity_public, identity_private, signed_prekey_id, next_prekey_id) "
		"VALUES (?1, ?2, ?3, ?4, ?5, 1, 1);");
	if (stmt == NULL)
		goto rollback;
	sqlite3_bind_int64(stmt, 2, oa->device_id);
	sqlite3_bind_int64(stmt, 3, oa->registration_id);
	sqlite3_bind_blob(stmt, 4, signal_buffer_data(pub), signal_buffer_len(pub),
	                  SQLITE_TRANSIENT);
	sqlite3_bind_blob(stmt, 5, signal_buffer_data(priv),
	                  signal_buffer_len(priv), SQLITE_TRANSIENT);
	if (!omemo_sql_done(oa, stmt))
		goto rollback;

	if (omemo_store_signed_prekey_cb(1, signal_buffer_data(spk_buf),
	                                 signal_buffer_len(spk_buf), oa))
		goto rollback;

	sqlite3_exec(oa->store->db, "COMMIT;", NULL, NULL, NULL);

	purple_debug_info("omemo", "generated identity for %s, device id %u\n",
	                  oa->jid, oa->device_id);
	ok = omemo_account_replenish_prekeys(oa, OMEMO_PREKEY_TARGET,
	                                     OMEMO_PREKEY_TARGET);
	if (!ok)
		g_set_error_literal(error, OMEMO_ERROR, 0,
		                    "Cannot generate OMEMO pre-keys");
	goto out;

rollback:
	sqlite3_exec(oa->store->db, "ROLLBACK;", NULL, NULL, NULL);
	g_set_error_literal(error, OMEMO_ERROR, 0,
	                    "Cannot store the OMEMO identity");
out:
	if (pub)
		signal_buffer_free(pub);
	if (priv)
		signal_buffer_bzero_free(priv);
	if (spk_buf)
		signal_buffer_bzero_free(spk_buf);
	if (spk)
		SIGNAL_UNREF(spk);
	if (pair)
		SIGNAL_UNREF(pair);
	return ok;
}

OmemoAccount *
omemo_account_new(OmemoStore *store, const char *jid, PurpleAccount *account,
                  GError **error)
{
	OmemoAccount *oa;
	signal_protocol_session_store session_store = {
		.load_session_func = omemo_load_session_cb,
		.get_sub_device_sessions_func = omemo_get_sub_device_sessions_cb,
		.store_session_func = omemo_store_session_cb,
		.contains_session_func = omemo_contains_session_cb,
		.delete_session_func = omemo_delete_session_cb,
		.delete_all_sessions_func = omemo_delete_all_sessions_cb,
		.destroy_func = NULL,
	};
	signal_protocol_pre_key_store prekey_store = {
		.load_pre_key = omemo_load_prekey_cb,
		.store_pre_key = omemo_store_prekey_cb,
		.contains_pre_key = omemo_contains_prekey_cb,
		.remove_pre_key = omemo_remove_prekey_cb,
		.destroy_func = NULL,
	};
	signal_protocol_signed_pre_key_store signed_store = {
		.load_signed_pre_key = omemo_load_signed_prekey_cb,
		.store_signed_pre_key = omemo_store_signed_prekey_cb,
		.contains_signed_pre_key = omemo_contains_signed_prekey_cb,
		.remove_signed_pre_key = omemo_remove_signed_prekey_cb,
		.destroy_func = NULL,
	};
	signal_protocol_identity_key_store identity_store = {
		.get_identity_key_pair = omemo_get_identity_key_pair_cb,
		.get_local_registration_id = omemo_get_local_registration_id_cb,
		.save_identity = omemo_save_identity_cb,
		.is_trusted_identity = omemo_is_trusted_identity_cb,
		.destroy_func = NULL,
	};

	g_return_val_if_fail(store != NULL, NULL);
	g_return_val_if_fail(jid != NULL, NULL);
	g_return_val_if_fail(omemo_signal_context() != NULL, NULL);

	oa = g_new0(OmemoAccount, 1);
	oa->store = store;
	oa->jid = g_strdup(jid);
	oa->account = account;
	oa->tofu = TRUE;

	if (!omemo_account_load_own(oa) && !omemo_account_generate(oa, error)) {
		omemo_account_free(oa);
		return NULL;
	}

	session_store.user_data = oa;
	prekey_store.user_data = oa;
	signed_store.user_data = oa;
	identity_store.user_data = oa;

	signal_protocol_store_context_create(&oa->store_ctx,
	                                     omemo_signal_context());
	signal_protocol_store_context_set_session_store(oa->store_ctx,
	                                                &session_store);
	signal_protocol_store_context_set_pre_key_store(oa->store_ctx,
	                                                &prekey_store);
	signal_protocol_store_context_set_signed_pre_key_store(oa->store_ctx,
	                                                       &signed_store);
	signal_protocol_store_context_set_identity_key_store(oa->store_ctx,
	                                                     &identity_store);
	return oa;
}

void
omemo_account_free(OmemoAccount *oa)
{
	if (oa == NULL)
		return;
	if (oa->store_ctx)
		signal_protocol_store_context_destroy(oa->store_ctx);
	g_free(oa->jid);
	g_free(oa);
}

signal_buffer *
omemo_account_identity_public(OmemoAccount *oa)
{
	sqlite3_stmt *stmt = omemo_sql(oa,
		"SELECT identity_public FROM own WHERE account = ?1;");
	signal_buffer *ret = NULL;

	if (stmt == NULL)
		return NULL;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		ret = omemo_column_buffer(stmt, 0);
	sqlite3_finalize(stmt);
	return ret;
}

char *
omemo_account_fingerprint(OmemoAccount *oa)
{
	signal_buffer *pub = omemo_account_identity_public(oa);
	char *fp;

	if (pub == NULL)
		return NULL;
	fp = omemo_fingerprint_format(signal_buffer_data(pub),
	                              signal_buffer_len(pub));
	signal_buffer_free(pub);
	return fp;
}

guint
omemo_store_count_prekeys(OmemoAccount *oa)
{
	sqlite3_stmt *stmt = omemo_sql(oa,
		"SELECT COUNT(*) FROM prekeys WHERE account = ?1;");
	guint n = 0;

	if (stmt == NULL)
		return 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		n = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return n;
}

/*
 * Top the pre-keys up to 'target' when fewer than 'minimum' are left.
 * Returns TRUE when the pre-key set is fine (whether or not keys were
 * generated); check the count before and after to know if the bundle must
 * be republished.
 */
gboolean
omemo_account_replenish_prekeys(OmemoAccount *oa, guint minimum, guint target)
{
	signal_protocol_key_helper_pre_key_list_node *head = NULL, *node;
	sqlite3_stmt *stmt;
	guint have = omemo_store_count_prekeys(oa);
	guint32 next = 1;
	guint count;
	gboolean ok = TRUE;

	if (have >= minimum)
		return TRUE;
	count = target - have;

	stmt = omemo_sql(oa, "SELECT next_prekey_id FROM own WHERE account = ?1;");
	if (stmt == NULL)
		return FALSE;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		next = (guint32)sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);
	if (next == 0 || next > 0xfffff0)
		next = 1;

	if (signal_protocol_key_helper_generate_pre_keys(&head, next, count,
	                                                  omemo_signal_context()))
		return FALSE;

	sqlite3_exec(oa->store->db, "BEGIN;", NULL, NULL, NULL);
	for (node = head; node;
	     node = signal_protocol_key_helper_key_list_next(node)) {
		session_pre_key *pk = signal_protocol_key_helper_key_list_element(node);
		signal_buffer *buf = NULL;

		if (session_pre_key_serialize(&buf, pk) ||
		    omemo_store_prekey_cb(session_pre_key_get_id(pk),
		                          signal_buffer_data(buf),
		                          signal_buffer_len(buf), oa)) {
			ok = FALSE;
		}
		if (buf)
			signal_buffer_bzero_free(buf);
		if (!ok)
			break;
	}
	signal_protocol_key_helper_key_list_free(head);

	if (ok) {
		stmt = omemo_sql(oa, "UPDATE own SET next_prekey_id = ?2 "
		                     "WHERE account = ?1;");
		sqlite3_bind_int64(stmt, 2, next + count);
		ok = omemo_sql_done(oa, stmt);
	}
	sqlite3_exec(oa->store->db, ok ? "COMMIT;" : "ROLLBACK;",
	             NULL, NULL, NULL);

	if (ok)
		purple_debug_info("omemo", "%s: generated %u pre-keys from id %u\n",
		                  oa->jid, count, next);
	return ok;
}

GList *
omemo_store_load_prekeys(OmemoAccount *oa)
{
	sqlite3_stmt *stmt = omemo_sql(oa,
		"SELECT record FROM prekeys WHERE account = ?1 ORDER BY id;");
	GList *ret = NULL;

	if (stmt == NULL)
		return NULL;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		session_pre_key *pk = NULL;

		if (session_pre_key_deserialize(&pk, sqlite3_column_blob(stmt, 0),
		                                sqlite3_column_bytes(stmt, 0),
		                                omemo_signal_context()) == 0)
			ret = g_list_prepend(ret, pk);
	}
	sqlite3_finalize(stmt);
	return g_list_reverse(ret);
}

session_signed_pre_key *
omemo_store_load_signed_prekey(OmemoAccount *oa)
{
	sqlite3_stmt *stmt = omemo_sql(oa,
		"SELECT record FROM signed_prekeys WHERE account = ?1 "
		"ORDER BY id DESC LIMIT 1;");
	session_signed_pre_key *spk = NULL;

	if (stmt == NULL)
		return NULL;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		session_signed_pre_key_deserialize(&spk, sqlite3_column_blob(stmt, 0),
		                                   sqlite3_column_bytes(stmt, 0),
		                                   omemo_signal_context());
	sqlite3_finalize(stmt);
	return spk;
}

/**************************************************************************
 * Device lists
 **************************************************************************/

gboolean
omemo_store_device_list_known(OmemoAccount *oa, const char *jid)
{
	sqlite3_stmt *stmt = omemo_sql(oa,
		"SELECT 1 FROM device_lists WHERE account = ?1 AND jid = ?2;");
	gboolean ret;

	if (stmt == NULL)
		return FALSE;
	sqlite3_bind_text(stmt, 2, jid, -1, SQLITE_TRANSIENT);
	ret = sqlite3_step(stmt) == SQLITE_ROW;
	sqlite3_finalize(stmt);
	return ret;
}

GArray *
omemo_store_get_devices(OmemoAccount *oa, const char *jid, gboolean active_only)
{
	sqlite3_stmt *stmt = omemo_sql(oa, active_only ?
		"SELECT device_id FROM devices WHERE account = ?1 AND jid = ?2 "
		"AND active = 1 ORDER BY device_id;" :
		"SELECT device_id FROM devices WHERE account = ?1 AND jid = ?2 "
		"ORDER BY device_id;");
	GArray *ret = g_array_new(FALSE, FALSE, sizeof(guint32));

	if (stmt == NULL)
		return ret;
	sqlite3_bind_text(stmt, 2, jid, -1, SQLITE_TRANSIENT);
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		guint32 id = (guint32)sqlite3_column_int64(stmt, 0);

		g_array_append_val(ret, id);
	}
	sqlite3_finalize(stmt);
	return ret;
}

/*
 * Replace the active device set of 'jid' with 'ids'. Devices that left the
 * list are kept (inactive) so their trust decisions survive a reinstall.
 * Returns the ids that were never seen before (GUINT_TO_POINTER).
 */
GList *
omemo_store_update_device_list(OmemoAccount *oa, const char *jid,
                               const guint32 *ids, guint n)
{
	GArray *known = omemo_store_get_devices(oa, jid, FALSE);
	GList *added = NULL;
	sqlite3_stmt *stmt;
	guint i, j;

	sqlite3_exec(oa->store->db, "BEGIN;", NULL, NULL, NULL);

	stmt = omemo_sql(oa, "UPDATE devices SET active = 0 WHERE account = ?1 "
	                     "AND jid = ?2;");
	sqlite3_bind_text(stmt, 2, jid, -1, SQLITE_TRANSIENT);
	omemo_sql_done(oa, stmt);

	for (i = 0; i < n; i++) {
		gboolean seen = FALSE;

		for (j = 0; j < known->len; j++)
			if (g_array_index(known, guint32, j) == ids[i])
				seen = TRUE;
		if (!seen)
			added = g_list_append(added, GUINT_TO_POINTER(ids[i]));

		stmt = omemo_sql(oa,
			"INSERT OR REPLACE INTO devices (account, jid, device_id, active, "
			"last_seen) VALUES (?1, ?2, ?3, 1, ?4);");
		sqlite3_bind_text(stmt, 2, jid, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 3, ids[i]);
		sqlite3_bind_int64(stmt, 4, (sqlite3_int64)time(NULL));
		omemo_sql_done(oa, stmt);
	}

	stmt = omemo_sql(oa, "INSERT OR REPLACE INTO device_lists (account, jid, "
	                     "updated) VALUES (?1, ?2, ?3);");
	sqlite3_bind_text(stmt, 2, jid, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, (sqlite3_int64)time(NULL));
	omemo_sql_done(oa, stmt);

	sqlite3_exec(oa->store->db, "COMMIT;", NULL, NULL, NULL);

	g_array_free(known, TRUE);
	return added;
}

void
omemo_device_info_free(OmemoDeviceInfo *info)
{
	if (info == NULL)
		return;
	g_free(info->fingerprint);
	g_free(info);
}

/*
 * All devices we know of for 'jid': the device list plus any identity we
 * learnt from a message (a device that is no longer listed).
 */
GList *
omemo_store_list_devices(OmemoAccount *oa, const char *jid)
{
	sqlite3_stmt *stmt = omemo_sql(oa,
		"SELECT d.device_id, d.active, i.identity_key, i.trust FROM "
		"  (SELECT device_id, active FROM devices WHERE account = ?1 "
		"     AND jid = ?2 "
		"   UNION SELECT device_id, 0 FROM identities WHERE account = ?1 "
		"     AND jid = ?2 AND device_id NOT IN (SELECT device_id FROM "
		"     devices WHERE account = ?1 AND jid = ?2)) AS d "
		"LEFT JOIN identities AS i ON i.account = ?1 AND i.jid = ?2 "
		"  AND i.device_id = d.device_id "
		"ORDER BY d.active DESC, d.device_id;");
	GList *ret = NULL;

	if (stmt == NULL)
		return NULL;
	sqlite3_bind_text(stmt, 2, jid, -1, SQLITE_TRANSIENT);
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		OmemoDeviceInfo *info = g_new0(OmemoDeviceInfo, 1);

		info->device_id = (guint32)sqlite3_column_int64(stmt, 0);
		info->active = sqlite3_column_int(stmt, 1) != 0;
		if (sqlite3_column_type(stmt, 2) != SQLITE_NULL) {
			info->has_key = TRUE;
			info->fingerprint = omemo_fingerprint_format(
				sqlite3_column_blob(stmt, 2), sqlite3_column_bytes(stmt, 2));
			omemo_trust_from_string(
				(const char *)sqlite3_column_text(stmt, 3), &info->trust);
		}
		ret = g_list_prepend(ret, info);
	}
	sqlite3_finalize(stmt);

	ret = g_list_reverse(ret);
	{
		GList *l;

		for (l = ret; l; l = l->next) {
			OmemoDeviceInfo *info = l->data;

			info->has_session = omemo_store_has_session(oa, jid,
			                                            info->device_id);
		}
	}
	return ret;
}
