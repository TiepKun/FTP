// ===== file: server/ClientSession.hpp =====
#pragma once
#include <cstdint> 
#include <string>
#include <vector>

using namespace std;

class FileServer;

class ClientSession {
public:
    ClientSession(int sockfd, FileServer &server);
    void run();

private:
    bool handle_command(const string &line);
    bool cmd_auth(const vector<string> &tokens);
    bool cmd_register(const vector<string> &tokens);
    bool cmd_upload(const vector<string> &tokens);
    bool cmd_download(const vector<string> &tokens);
    bool cmd_get_text(const vector<string> &tokens);
    bool cmd_put_text(const vector<string> &tokens);
    bool cmd_stats();

    bool cmd_list_db(const vector<string> &tokens);


    bool ensure_authenticated();
    uint64_t file_size(const string &path);

    int sockfd_;
    FileServer &server_;
    string username_;
    int user_id_ = 0;
    bool authenticated_ = false;
};
