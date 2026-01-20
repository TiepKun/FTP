#include "FileServer.hpp"
#include "ClientSession.hpp"
#include "DbSqlite.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>
#include <iostream>
#include <filesystem>
#include <netinet/tcp.h>

using namespace std;

namespace {
// Chọn đường dẫn log ổn định; cho phép override bằng env để tránh nhầm thư mục.
string resolve_log_path() {
    if (const char *p = ::getenv("FS_LOG_PATH")) return string(p);
    namespace fs = std::filesystem;
    return (fs::current_path() / "server.log").string();
}

// Đường dẫn lưu user_account.txt; cho phép override bằng env.
string resolve_account_path() {
    if (const char *p = ::getenv("FS_ACCOUNT_PATH")) return string(p);
    namespace fs = std::filesystem;
    return (fs::current_path() / "user_account.txt").string();
}

// Bật TCP keepalive để phát hiện client chết sớm hơn.
void enable_keepalive(int fd) {
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));

    // Các tham số dưới đây phụ thuộc hệ thống; nếu không hỗ trợ sẽ bỏ qua.
    int idle = 60;      // giây không lưu lượng trước khi gửi keepalive đầu tiên
    int intvl = 15;     // khoảng cách giữa các keepalive
    int cnt = 4;        // số lần thử trước khi coi như mất kết nối
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
}
} // namespace

FileServer::FileServer(const string &root_dir, int port)
    : root_dir_(root_dir),
      port_(port),
      log_file_path_(resolve_log_path()),
      account_file_path_(resolve_account_path()),
      logger_(log_file_path_) {

    db_ = make_unique<DbSqlite>("fileshare.db");
    string err;
    if (!db_->init_schema(err)) {
        cerr << "DB init failed: " << err << "\n";
    }
}

void FileServer::run() {
    int listenfd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        perror("socket");
        return;
    }
    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port_);
    if (::bind(listenfd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listenfd);
        return;
    }
    if (::listen(listenfd, 16) < 0) {
        perror("listen");
        close(listenfd);
        return;
    }
    cout << "Server listening on port " << port_ << "\n";

    while (true) {
        sockaddr_in cli{};
        socklen_t len = sizeof(cli);
        int connfd = ::accept(listenfd, (sockaddr*)&cli, &len);
        if (connfd < 0) {
            perror("accept");
            continue;
        }
        enable_keepalive(connfd);
        thread([this, connfd]() {
            ClientSession session(connfd, *this);
            session.run();
            close(connfd);
        }).detach();
    }

    close(listenfd);
}

bool FileServer::is_user_online(const std::string &user) {
    auto it = online_users_.find(user);
    return it != online_users_.end() && it->second > 0;
}

void FileServer::user_login(const std::string &user) {
    online_users_[user]++;
}

void FileServer::user_logout(const std::string &user) {
    auto &c = online_users_[user];
    c--;
    if (c <= 0) online_users_.erase(user);
}

int FileServer::online_users_count() const {
    return online_users_.size();
}

std::shared_ptr<UploadState> FileServer::get_upload_state(
    const std::string &username,
    const std::string &path
) {
    std::string key = username + ":" + path;
    std::lock_guard<std::mutex> lk(upload_states_mutex_);
    auto it = upload_states_.find(key);
    if (it == upload_states_.end()) return nullptr;
    return it->second;
}

void FileServer::remove_upload_state(
    const std::string &username,
    const std::string &path
) {
    std::lock_guard<std::mutex> lk(upload_states_mutex_);
    upload_states_.erase(username + ":" + path);
}


std::shared_ptr<UploadState> FileServer::create_upload_state(
    const std::string &username,
    const std::string &path
) {
    std::string key = username + ":" + path;
    std::lock_guard<std::mutex> lk(upload_states_mutex_);

    auto state = std::make_shared<UploadState>();
    upload_states_[key] = state;
    return state;
}

std::shared_ptr<DownloadState> FileServer::get_download_state(
    const std::string &username,
    const std::string &path
) {
    std::string key = username + ":" + path;
    std::lock_guard<std::mutex> lk(download_states_mutex_);
    auto it = download_states_.find(key);
    if (it == download_states_.end()) return nullptr;
    return it->second;
}

void FileServer::remove_download_state(
    const std::string &username,
    const std::string &path
) {
    std::lock_guard<std::mutex> lk(download_states_mutex_);
    download_states_.erase(username + ":" + path);
}

std::shared_ptr<DownloadState> FileServer::create_download_state(
    const std::string &username,
    const std::string &path
) {
    std::string key = username + ":" + path;
    std::lock_guard<std::mutex> lk(download_states_mutex_);

    auto state = std::make_shared<DownloadState>();
    download_states_[key] = state;
    return state;
}

std::unique_lock<std::mutex> FileServer::lock_file(
    const std::string &owner_user,
    const std::string &path
) {
    std::string key = owner_user + ":" + path;
    std::shared_ptr<std::mutex> mtx;
    {
        std::lock_guard<std::mutex> lk(file_locks_mutex_);
        auto it = file_locks_.find(key);
        if (it == file_locks_.end()) {
            mtx = std::make_shared<std::mutex>();
            file_locks_.emplace(key, mtx);
        } else {
            mtx = it->second;
        }
    }
    return std::unique_lock<std::mutex>(*mtx);
}

bool FileServer::try_lock_edit(
    const std::string &owner_user,
    const std::string &path,
    const std::string &username,
    std::string &locked_by
) {
    std::string key = owner_user + ":" + path;
    std::lock_guard<std::mutex> lk(edit_locks_mutex_);
    auto it = edit_locks_.find(key);
    if (it == edit_locks_.end()) {
        edit_locks_[key] = username;
        return true;
    }
    if (it->second == username) {
        return true;
    }
    locked_by = it->second;
    return false;
}

bool FileServer::get_edit_lock_owner(
    const std::string &owner_user,
    const std::string &path,
    std::string &locked_by
) {
    std::string key = owner_user + ":" + path;
    std::lock_guard<std::mutex> lk(edit_locks_mutex_);
    auto it = edit_locks_.find(key);
    if (it == edit_locks_.end()) return false;
    locked_by = it->second;
    return true;
}

void FileServer::release_edit_lock(
    const std::string &owner_user,
    const std::string &path,
    const std::string &username
) {
    std::string key = owner_user + ":" + path;
    std::lock_guard<std::mutex> lk(edit_locks_mutex_);
    auto it = edit_locks_.find(key);
    if (it != edit_locks_.end() && it->second == username) {
        edit_locks_.erase(it);
    }
}

void FileServer::release_all_edit_locks(const std::string &username) {
    std::lock_guard<std::mutex> lk(edit_locks_mutex_);
    for (auto it = edit_locks_.begin(); it != edit_locks_.end(); ) {
        if (it->second == username) {
            it = edit_locks_.erase(it);
        } else {
            ++it;
        }
    }
}
