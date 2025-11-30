// ===== file: server/FileServer.hpp =====
#pragma once
#include <string>
#include <atomic>
#include <memory>
#include <filesystem>
#include "Logger.hpp"
#include "QuotaManager.hpp"
#include "Db.hpp"

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
    void inc_active()              { ++active_users_; }
    void dec_active()              { --active_users_; }

    uint64_t bytes_in()  const { return bytes_in_.load(); }
    uint64_t bytes_out() const { return bytes_out_.load(); }
    int active_users()   const { return active_users_.load(); }

    const string& root_dir() const { return root_dir_; }

private:
    string root_dir_;
    int port_;
    string log_file_path_;
    string account_file_path_;
    Logger logger_;
    QuotaManager quota_mgr_;
    atomic<uint64_t> bytes_in_{0};
    atomic<uint64_t> bytes_out_{0};
    atomic<int>      active_users_{0};
    unique_ptr<Db>   db_;
};
