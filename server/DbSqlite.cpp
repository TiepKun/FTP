// ===== file: server/DbSqlite.cpp =====
#include "DbSqlite.hpp"
#include <iostream>

DbSqlite::DbSqlite(const string &db_path) : db_path_(db_path) {
    if (sqlite3_open(db_path_.c_str(), &db_) != SQLITE_OK) {
        cerr << "Cannot open SQLite: " << sqlite3_errmsg(db_) << "\n";
    } else {
        char *errmsg = nullptr;
        sqlite3_exec(db_, "PRAGMA foreign_keys = ON;", nullptr, nullptr, &errmsg);
        if (errmsg) sqlite3_free(errmsg);
    }
}

DbSqlite::~DbSqlite() {
    if (db_) sqlite3_close(db_);
}

bool DbSqlite::init_schema(string &err) {
    const char *sql = R"SQL(
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS app_user (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    username      TEXT UNIQUE NOT NULL,
    password_hash TEXT NOT NULL,
    quota_bytes   INTEGER NOT NULL DEFAULT 0,
    used_bytes    INTEGER NOT NULL DEFAULT 0,
    created_at    DATETIME DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS file_entry (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    owner_id    INTEGER NOT NULL,
    path        TEXT NOT NULL,
    size_bytes  INTEGER NOT NULL,
    is_folder   INTEGER NOT NULL DEFAULT 0,
    created_at  DATETIME DEFAULT CURRENT_TIMESTAMP,
    updated_at  DATETIME DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY(owner_id) REFERENCES app_user(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS file_acl (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    file_id     INTEGER NOT NULL,
    grantee_id  INTEGER NOT NULL,
    perm_read   INTEGER DEFAULT 1,
    perm_write  INTEGER DEFAULT 0,
    created_at  DATETIME DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY(file_id) REFERENCES file_entry(id) ON DELETE CASCADE,
    FOREIGN KEY(grantee_id) REFERENCES app_user(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS audit_log (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id     INTEGER,
    action      TEXT NOT NULL,
    detail      TEXT,
    created_at  DATETIME DEFAULT CURRENT_TIMESTAMP,
    remote_ip   TEXT,
    FOREIGN KEY(user_id) REFERENCES app_user(id) ON DELETE SET NULL
);

CREATE UNIQUE INDEX IF NOT EXISTS idx_file_entry_owner_path
    ON file_entry(owner_id, path);
)SQL";

    char *errmsg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        err = errmsg ? errmsg : "Unknown SQLite error";
        if (errmsg) sqlite3_free(errmsg);
        return false;
    }
    return true;
}

bool DbSqlite::get_user_by_username(const string &username,
                                    UserRecord &out,
                                    string &err) {
    const char *sql =
        "SELECT id, username, password_hash, quota_bytes, used_bytes "
        "FROM app_user WHERE username = ?;";

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        out.id            = sqlite3_column_int(stmt, 0);
        out.username      = (const char*)sqlite3_column_text(stmt, 1);
        out.password_hash = (const char*)sqlite3_column_text(stmt, 2);
        out.quota_bytes   = (uint64_t)sqlite3_column_int64(stmt, 3);
        out.used_bytes    = (uint64_t)sqlite3_column_int64(stmt, 4);
        sqlite3_finalize(stmt);
        return true;
    } else if (rc == SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return false;
    } else {
        err = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        return false;
    }
}

bool DbSqlite::update_used_bytes(int user_id,
                                 uint64_t used_bytes,
                                 string &err) {
    const char *sql =
        "UPDATE app_user SET used_bytes = ? WHERE id = ?;";

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)used_bytes);
    sqlite3_bind_int(stmt, 2, user_id);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        err = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        return false;
    }
    sqlite3_finalize(stmt);
    return true;
}

bool DbSqlite::create_user(const string &username,
                           const string &password_hash,
                           uint64_t quota_bytes,
                           string &err) {
    const char *sql =
        "INSERT INTO app_user (username, password_hash, quota_bytes, used_bytes) "
        "VALUES (?, ?, ?, 0);";

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, password_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)quota_bytes);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        err = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        return false;
    }
    sqlite3_finalize(stmt);
    return true;
}

bool DbSqlite::insert_log(int user_id,
                          const string &action,
                          const string &detail,
                          const string &remote_ip,
                          string &err) {
    const char *sql =
        "INSERT INTO audit_log (user_id, action, detail, remote_ip) "
        "VALUES (?, ?, ?, ?);";

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }

    if (user_id > 0)
        sqlite3_bind_int(stmt, 1, user_id);
    else
        sqlite3_bind_null(stmt, 1);

    sqlite3_bind_text(stmt, 2, action.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, detail.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, remote_ip.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        err = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        return false;
    }
    sqlite3_finalize(stmt);
    return true;
}

bool DbSqlite::upsert_file_entry(int owner_id,
                                 const string &path,
                                 uint64_t size_bytes,
                                 bool is_folder,
                                 string &err) {
    const char *sql =
        "INSERT INTO file_entry (owner_id, path, size_bytes, is_folder) "
        "VALUES (?, ?, ?, ?) "
        "ON CONFLICT(owner_id, path) DO UPDATE SET "
        "size_bytes = excluded.size_bytes, "
        "is_folder = excluded.is_folder, "
        "updated_at = CURRENT_TIMESTAMP;";

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int(stmt, 1, owner_id);
    sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)size_bytes);
    sqlite3_bind_int(stmt, 4, is_folder ? 1 : 0);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        err = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        return false;
    }
    sqlite3_finalize(stmt);
    return true;
}

bool DbSqlite::list_files(int owner_id, string &paths, string &err) {
    paths.clear();

    const char *sql =
        "SELECT path, size_bytes FROM file_entry "
        "WHERE owner_id = ? ORDER BY path;";

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int(stmt, 1, owner_id);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *p = (const char*)sqlite3_column_text(stmt, 0);
        uint64_t size = sqlite3_column_int64(stmt, 1);

        if (p) {
            paths += string(p) + "|" + to_string(size) + "\n";
        }
    }

    if (rc != SQLITE_DONE) {
        err = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}
