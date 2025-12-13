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
