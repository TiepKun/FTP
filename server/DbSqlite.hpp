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

    bool list_files(int owner_id, string &paths, string &err) override;

    // File operations
    bool delete_file_entry(int owner_id, const string &path, string &err) override;
    bool restore_file_entry(int owner_id, const string &path, string &err) override;
    bool rename_file_entry(int owner_id, const string &old_path, const string &new_path, string &err) override;
    bool move_file_entry(int owner_id, const string &old_path, const string &new_path, string &err) override;
    bool copy_file_entry(int owner_id, const string &src_path, const string &dst_path, string &err) override;
    bool get_file_entry(int owner_id, const string &path, int &file_id, uint64_t &size_bytes, bool &is_folder, bool &is_deleted, string &err) override;

    // Permissions
    bool check_permission(int file_id, int user_id, bool &can_view, bool &can_download, bool &can_edit, string &err) override;
    bool set_permission(int file_id, int grantee_id, bool can_view, bool can_download, bool can_edit, string &err) override;
    bool get_file_id_by_path(int owner_id, const string &path, int &file_id, string &err) override;
    bool find_shared_file(const string &path, int grantee_id, int &file_id, int &owner_id, string &owner_username, string &err) override;
    bool list_deleted_files(int owner_id, string &rows, string &err) override;

    // Transfer sessions
    bool create_transfer_session(int user_id, const string &path, const string &type, uint64_t size_bytes, uint64_t offset, int &session_id, string &err) override;
    bool get_transfer_session(int user_id, const string &path, const string &type, int &session_id, uint64_t &offset, uint64_t &size_bytes, string &err) override;
    bool update_transfer_session(int session_id, uint64_t offset, string &err) override;
    bool delete_transfer_session(int session_id, string &err) override;

private:
    string db_path_;
    sqlite3 *db_ = nullptr;
};
