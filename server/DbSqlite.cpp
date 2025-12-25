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
    const char *sql_tables = R"SQL(
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
    is_deleted  INTEGER NOT NULL DEFAULT 0,
    deleted_at  DATETIME,
    created_at  DATETIME DEFAULT CURRENT_TIMESTAMP,
    updated_at  DATETIME DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY(owner_id) REFERENCES app_user(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS file_acl (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    file_id     INTEGER NOT NULL,
    grantee_id  INTEGER NOT NULL,
    perm_read   INTEGER DEFAULT 1,
    perm_download INTEGER DEFAULT 1,
    perm_write  INTEGER DEFAULT 0,
    created_at  DATETIME DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY(file_id) REFERENCES file_entry(id) ON DELETE CASCADE,
    FOREIGN KEY(grantee_id) REFERENCES app_user(id) ON DELETE CASCADE,
    UNIQUE(file_id, grantee_id)
);

CREATE TABLE IF NOT EXISTS transfer_session (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id     INTEGER NOT NULL,
    path        TEXT NOT NULL,
    type        TEXT NOT NULL,
    offset      INTEGER NOT NULL DEFAULT 0,
    size_bytes  INTEGER NOT NULL,
    last_update DATETIME DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY(user_id) REFERENCES app_user(id) ON DELETE CASCADE
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

CREATE INDEX IF NOT EXISTS idx_transfer_session_user_path
    ON transfer_session(user_id, path, type);
)SQL";

    char *errmsg = nullptr;
    int rc = sqlite3_exec(db_, sql_tables, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        err = errmsg ? errmsg : "Unknown SQLite error";
        if (errmsg) sqlite3_free(errmsg);
        return false;
    }

    // ===== Minimal migrations for old DBs =====
    auto has_column = [this](const string &table, const string &col) -> bool {
        string sql = "PRAGMA table_info(" + table + ");";
        sqlite3_stmt *stmt = nullptr;
        bool found = false;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char *name = sqlite3_column_text(stmt, 1);
                if (name && col == reinterpret_cast<const char*>(name)) {
                    found = true;
                    break;
                }
            }
        }
        if (stmt) sqlite3_finalize(stmt);
        return found;
    };

    auto add_column_if_missing = [this, &err](const string &table, const string &def) -> bool {
        string sql = "ALTER TABLE " + table + " ADD COLUMN " + def + ";";
        char *errmsg_local = nullptr;
        int rc_local = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errmsg_local);
        if (rc_local != SQLITE_OK && errmsg_local) {
            string msg = errmsg_local;
            sqlite3_free(errmsg_local);
            // Ignore duplicate column errors
            if (msg.find("duplicate column") == string::npos &&
                msg.find("exists") == string::npos) {
                err = msg;
                return false;
            }
        }
        return true;
    };

    if (!has_column("file_entry", "is_deleted")) {
        if (!add_column_if_missing("file_entry", "is_deleted INTEGER NOT NULL DEFAULT 0")) {
            return false;
        }
    }
    if (!has_column("file_entry", "deleted_at")) {
        if (!add_column_if_missing("file_entry", "deleted_at DATETIME")) {
            return false;
        }
    }

    const char *sql_index =
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_file_entry_owner_path "
        "ON file_entry(owner_id, path) WHERE is_deleted = 0;";
    rc = sqlite3_exec(db_, sql_index, nullptr, nullptr, &errmsg);
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
        "SELECT path, size_bytes, is_folder FROM file_entry "
        "WHERE owner_id = ? AND is_deleted = 0 ORDER BY path;";

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
        int is_folder = sqlite3_column_int(stmt, 2);

        if (p) {
            paths += string(p) + "|" + to_string(size) + "|" + to_string(is_folder) + "\n";
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

bool DbSqlite::delete_file_entry(int owner_id, const string &path, string &err) {
    const char *sql =
        "UPDATE file_entry SET is_deleted = 1, deleted_at = CURRENT_TIMESTAMP "
        "WHERE owner_id = ? AND path = ? AND is_deleted = 0;";

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int(stmt, 1, owner_id);
    sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        err = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}

bool DbSqlite::restore_file_entry(int owner_id, const string &path, string &err) {
    const char *sql =
        "UPDATE file_entry SET is_deleted = 0, deleted_at = NULL "
        "WHERE owner_id = ? AND path = ? AND is_deleted = 1;";

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int(stmt, 1, owner_id);
    sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        err = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}

bool DbSqlite::rename_file_entry(int owner_id, const string &old_path, const string &new_path, string &err) {
    const char *sql =
        "UPDATE file_entry SET path = ?, updated_at = CURRENT_TIMESTAMP "
        "WHERE owner_id = ? AND path = ?;";

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_text(stmt, 1, new_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, owner_id);
    sqlite3_bind_text(stmt, 3, old_path.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        err = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}

bool DbSqlite::move_file_entry(int owner_id, const string &old_path, const string &new_path, string &err) {
    return rename_file_entry(owner_id, old_path, new_path, err);
}

bool DbSqlite::copy_file_entry(int owner_id, const string &src_path, const string &dst_path, string &err) {
    // Get source file info
    int src_file_id;
    uint64_t size_bytes;
    bool is_folder;
    bool is_deleted;
    if (!get_file_entry(owner_id, src_path, src_file_id, size_bytes, is_folder, is_deleted, err)) {
        return false;
    }

    if (is_deleted) {
        err = "Cannot copy deleted file";
        return false;
    }

    // Create new entry
    if (!upsert_file_entry(owner_id, dst_path, size_bytes, is_folder, err)) {
        return false;
    }

    // Copy permissions if any
    const char *sql_acl =
        "INSERT INTO file_acl (file_id, grantee_id, perm_read, perm_download, perm_write) "
        "SELECT (SELECT id FROM file_entry WHERE owner_id = ? AND path = ?), grantee_id, perm_read, perm_download, perm_write "
        "FROM file_acl WHERE file_id = ?;";

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql_acl, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int(stmt, 1, owner_id);
    sqlite3_bind_text(stmt, 2, dst_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, src_file_id);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_CONSTRAINT) {
        err = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}

bool DbSqlite::get_file_entry(int owner_id, const string &path, int &file_id, uint64_t &size_bytes, bool &is_folder, bool &is_deleted, string &err) {
    const char *sql =
        "SELECT id, size_bytes, is_folder, is_deleted FROM file_entry "
        "WHERE owner_id = ? AND path = ?;";

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int(stmt, 1, owner_id);
    sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        file_id = sqlite3_column_int(stmt, 0);
        size_bytes = (uint64_t)sqlite3_column_int64(stmt, 1);
        is_folder = sqlite3_column_int(stmt, 2) != 0;
        is_deleted = sqlite3_column_int(stmt, 3) != 0;
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

bool DbSqlite::get_file_id_by_path(int owner_id, const string &path, int &file_id, string &err) {
    uint64_t size_bytes;
    bool is_folder, is_deleted;
    return get_file_entry(owner_id, path, file_id, size_bytes, is_folder, is_deleted, err);
}

bool DbSqlite::find_shared_file(const string &path, int grantee_id, int &file_id, int &owner_id, string &owner_username, string &err) {
    const char *sql =
        "SELECT f.id, f.owner_id, u.username "
        "FROM file_entry f "
        "JOIN file_acl a ON a.file_id = f.id "
        "JOIN app_user u ON u.id = f.owner_id "
        "WHERE a.grantee_id = ? AND f.path = ? AND f.is_deleted = 0 "
        "ORDER BY f.updated_at DESC LIMIT 1;";

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int(stmt, 1, grantee_id);
    sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        file_id = sqlite3_column_int(stmt, 0);
        owner_id = sqlite3_column_int(stmt, 1);
        const unsigned char *u = sqlite3_column_text(stmt, 2);
        if (u) owner_username = reinterpret_cast<const char*>(u);
        sqlite3_finalize(stmt);
        return true;
    }
    sqlite3_finalize(stmt);
    return false;
}

bool DbSqlite::list_deleted_files(int owner_id, string &rows, string &err) {
    rows.clear();
    const char *sql =
        "SELECT path, size_bytes, COALESCE(deleted_at, '') "
        "FROM file_entry WHERE owner_id = ? AND is_deleted = 1 "
        "ORDER BY deleted_at DESC;";

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int(stmt, 1, owner_id);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        uint64_t size = static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
        const char *ts = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        if (!p) p = "";
        if (!ts) ts = "";
        rows += string(p) + "|" + to_string(size) + "|" + string(ts) + "\n";
    }
    if (rc != SQLITE_DONE) {
        err = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        return false;
    }
    sqlite3_finalize(stmt);
    return true;
}

bool DbSqlite::check_permission(int file_id, int user_id, bool &can_view, bool &can_download, bool &can_edit, string &err) {
    // First check if user is owner
    const char *sql_owner = "SELECT owner_id FROM file_entry WHERE id = ?;";
    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql_owner, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int(stmt, 1, file_id);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        err = "File not found";
        sqlite3_finalize(stmt);
        return false;
    }

    int owner_id = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    // Owner has all permissions
    if (owner_id == user_id) {
        can_view = true;
        can_download = true;
        can_edit = true;
        return true;
    }

    // Check ACL
    const char *sql_acl =
        "SELECT perm_read, perm_download, perm_write FROM file_acl "
        "WHERE file_id = ? AND grantee_id = ?;";

    rc = sqlite3_prepare_v2(db_, sql_acl, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int(stmt, 1, file_id);
    sqlite3_bind_int(stmt, 2, user_id);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        can_view = sqlite3_column_int(stmt, 0) != 0;
        can_download = sqlite3_column_int(stmt, 1) != 0;
        can_edit = sqlite3_column_int(stmt, 2) != 0;
        sqlite3_finalize(stmt);
        return true;
    } else {
        // No ACL entry means no permission
        can_view = false;
        can_download = false;
        can_edit = false;
        sqlite3_finalize(stmt);
        return true;
    }
}

bool DbSqlite::set_permission(int file_id, int grantee_id, bool can_view, bool can_download, bool can_edit, string &err) {
    const char *sql =
        "INSERT INTO file_acl (file_id, grantee_id, perm_read, perm_download, perm_write) "
        "VALUES (?, ?, ?, ?, ?) "
        "ON CONFLICT(file_id, grantee_id) DO UPDATE SET "
        "perm_read = excluded.perm_read, "
        "perm_download = excluded.perm_download, "
        "perm_write = excluded.perm_write;";

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int(stmt, 1, file_id);
    sqlite3_bind_int(stmt, 2, grantee_id);
    sqlite3_bind_int(stmt, 3, can_view ? 1 : 0);
    sqlite3_bind_int(stmt, 4, can_download ? 1 : 0);
    sqlite3_bind_int(stmt, 5, can_edit ? 1 : 0);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        err = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}

bool DbSqlite::create_transfer_session(int user_id, const string &path, const string &type, uint64_t size_bytes, uint64_t offset, int &session_id, string &err) {
    const char *sql =
        "INSERT INTO transfer_session (user_id, path, type, offset, size_bytes) "
        "VALUES (?, ?, ?, ?, ?);";

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int(stmt, 1, user_id);
    sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)offset);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)size_bytes);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        err = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        return false;
    }

    session_id = (int)sqlite3_last_insert_rowid(db_);
    sqlite3_finalize(stmt);
    return true;
}

bool DbSqlite::get_transfer_session(int user_id, const string &path, const string &type, int &session_id, uint64_t &offset, uint64_t &size_bytes, string &err) {
    const char *sql =
        "SELECT id, offset, size_bytes FROM transfer_session "
        "WHERE user_id = ? AND path = ? AND type = ? "
        "ORDER BY last_update DESC LIMIT 1;";

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int(stmt, 1, user_id);
    sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, type.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        session_id = sqlite3_column_int(stmt, 0);
        offset = (uint64_t)sqlite3_column_int64(stmt, 1);
        size_bytes = (uint64_t)sqlite3_column_int64(stmt, 2);
        sqlite3_finalize(stmt);
        return true;
    } else {
        sqlite3_finalize(stmt);
        return false;
    }
}

bool DbSqlite::update_transfer_session(int session_id, uint64_t offset, string &err) {
    const char *sql =
        "UPDATE transfer_session SET offset = ?, last_update = CURRENT_TIMESTAMP "
        "WHERE id = ?;";

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)offset);
    sqlite3_bind_int(stmt, 2, session_id);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        err = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}

bool DbSqlite::delete_transfer_session(int session_id, string &err) {
    const char *sql = "DELETE FROM transfer_session WHERE id = ?;";

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int(stmt, 1, session_id);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        err = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}
