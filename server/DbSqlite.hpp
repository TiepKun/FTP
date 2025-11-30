// ===== file: server/DbSqlite.hpp =====
#pragma once
#include "Db.hpp"
#include <sqlite3.h>

using namespace std;

class DbSqlite : public Db {
public:
    explicit DbSqlite(const string &db_path);
    ~DbSqlite() override;

    bool init_schema(string &err) override;

    bool get_user_by_username(const string &username,
                              UserRecord &out,
                              string &err) override;

    bool update_used_bytes(int user_id,
                           uint64_t used_bytes,
                           string &err) override;

    bool insert_log(int user_id,
                    const string &action,
                    const string &detail,
                    const string &remote_ip,
                    string &err) override;

    bool create_user(const string &username,
                     const string &password_hash,
                     uint64_t quota_bytes,
                     string &err) override;

    bool upsert_file_entry(int owner_id,
                           const string &path,
                           uint64_t size_bytes,
                           bool is_folder,
                           string &err) override;

private:
    string db_path_;
    sqlite3 *db_ = nullptr;
};
