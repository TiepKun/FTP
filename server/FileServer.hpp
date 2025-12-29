#pragma once
#include <string>
#include <atomic>
#include <memory>
#include <filesystem>
#include "Logger.hpp"
#include "QuotaManager.hpp"
#include "Db.hpp"
#include <unordered_map>
#include "UploadState.hpp"
#include <mutex>

using namespace std;

class FileServer {
public:
    FileServer(const string &root_dir, int port);

    void run();

    Logger& logger() { return logger_; }
    QuotaManager& quota_mgr() { return quota_mgr_; }
    Db& db() { return *db_; }

    const string& account_file_path() const { return account_file_path_; }
    const string& log_file_path() const { return log_file_path_; }

    void add_bytes_in(uint64_t n)  { bytes_in_  += n; }
    void add_bytes_out(uint64_t n) { bytes_out_ += n; }

    uint64_t bytes_in()  const { return bytes_in_.load(); }
    uint64_t bytes_out() const { return bytes_out_.load(); }

    const string& root_dir() const { return root_dir_; }

    // ===== ONLINE USER MANAGEMENT =====
    bool is_user_online(const std::string &user);
    void user_login(const std::string &user);
    void user_logout(const std::string &user);
    int online_users_count() const;

    const unordered_map<string, int>& get_online_users() const {
        return online_users_;
    }

    std::shared_ptr<UploadState> get_upload_state(
        const std::string &username,
        const std::string &path
    );

    std::shared_ptr<UploadState> create_upload_state(
        const std::string &username,
        const std::string &path
    );

    void remove_upload_state(
        const std::string &username,
        const std::string &path
    );

private:
    string root_dir_;
    int port_;
    string log_file_path_;
    string account_file_path_;
    Logger logger_;
    QuotaManager quota_mgr_;
    atomic<uint64_t> bytes_in_{0};
    atomic<uint64_t> bytes_out_{0};
    unique_ptr<Db> db_;

    // user -> số session đang logged-in
    unordered_map<string, int> online_users_;
    // upload state per user:path
    std::unordered_map<std::string, std::shared_ptr<UploadState>> upload_states_;
    std::mutex upload_states_mutex_;
};
