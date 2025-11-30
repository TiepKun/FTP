// ===== file: server/FileServer.cpp =====
#include "FileServer.hpp"
#include "ClientSession.hpp"
#include "DbSqlite.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>
#include <iostream>

using namespace std;

FileServer::FileServer(const string &root_dir, int port)
    : root_dir_(root_dir),
      port_(port),
      logger_("server.log") {

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
            inc_active();
            ClientSession session(connfd, *this);
            session.run();
            dec_active();
            close(connfd);
        }).detach();
    }

    close(listenfd);
}
