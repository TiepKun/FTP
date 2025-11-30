// ===== file: client/NetworkClient.hpp =====
#pragma once
#include <string>

using namespace std;

class NetworkClient {
public:
    NetworkClient();
    ~NetworkClient();

    bool connect_to(const string &host, int port);
    void close();

    bool auth(const string &user, const string &pass, string &err);
    bool register_user(const string &user, const string &pass, string &err);
    bool get_text(const string &path, string &content, string &err);
    bool put_text(const string &path, const string &content, string &err);

private:
    int sockfd_ = -1;
};
