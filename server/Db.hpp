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
};
