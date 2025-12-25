// ===== file: server/Db.hpp =====
#pragma once
#include <string>
#include <cstdint>

using namespace std;

struct UserRecord {
    int id = 0;
    string username;
    string password_hash;
    uint64_t quota_bytes = 0;
    uint64_t used_bytes  = 0;
};

class Db {
public:
    virtual ~Db() = default;

    virtual bool init_schema(string &err) = 0;

    virtual bool get_user_by_username(const string &username,
                                      UserRecord &out,
                                      string &err) = 0;

    virtual bool update_used_bytes(int user_id,
                                   uint64_t used_bytes,
                                   string &err) = 0;

    virtual bool insert_log(int user_id,
                            const string &action,
                            const string &detail,
                            const string &remote_ip,
                            string &err) = 0;

    virtual bool create_user(const string &username,
                             const string &password_hash,
                             uint64_t quota_bytes,
                             string &err) = 0;

    virtual bool upsert_file_entry(int owner_id,
                                   const string &path,
                                   uint64_t size_bytes,
                                   bool is_folder,
                                   string &err) = 0;

    virtual bool list_files(int owner_id, string &paths, string &err) = 0;

    // File operations
    virtual bool delete_file_entry(int owner_id, const string &path, string &err) = 0;
    virtual bool restore_file_entry(int owner_id, const string &path, string &err) = 0;
    virtual bool rename_file_entry(int owner_id, const string &old_path, const string &new_path, string &err) = 0;
    virtual bool move_file_entry(int owner_id, const string &old_path, const string &new_path, string &err) = 0;
    virtual bool copy_file_entry(int owner_id, const string &src_path, const string &dst_path, string &err) = 0;
    virtual bool get_file_entry(int owner_id, const string &path, int &file_id, uint64_t &size_bytes, bool &is_folder, bool &is_deleted, string &err) = 0;

    // Permissions
    virtual bool check_permission(int file_id, int user_id, bool &can_view, bool &can_download, bool &can_edit, string &err) = 0;
    virtual bool set_permission(int file_id, int grantee_id, bool can_view, bool can_download, bool can_edit, string &err) = 0;
    virtual bool get_file_id_by_path(int owner_id, const string &path, int &file_id, string &err) = 0;
    virtual bool find_shared_file(const string &path, int grantee_id, int &file_id, int &owner_id, string &owner_username, string &err) = 0;
    virtual bool list_deleted_files(int owner_id, string &rows, string &err) = 0;

    // Transfer sessions (pause/continue)
    virtual bool create_transfer_session(int user_id, const string &path, const string &type, uint64_t size_bytes, uint64_t offset, int &session_id, string &err) = 0;
    virtual bool get_transfer_session(int user_id, const string &path, const string &type, int &session_id, uint64_t &offset, uint64_t &size_bytes, string &err) = 0;
    virtual bool update_transfer_session(int session_id, uint64_t offset, string &err) = 0;
    virtual bool delete_transfer_session(int session_id, string &err) = 0;
};
