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
    ~ClientSession();
    void run();

private:
    bool handle_command(const string &line);
    bool cmd_auth(const vector<string> &tokens);
    bool cmd_register(const vector<string> &tokens);
    bool cmd_upload(const vector<string> &tokens);
    bool cmd_download(const vector<string> &tokens);
    bool cmd_get_text(const vector<string> &tokens);
    bool cmd_put_text(const vector<string> &tokens);
    bool cmd_logout();
    bool cmd_stats();
    bool cmd_who();

    bool cmd_list_db(const vector<string> &tokens);
    
    // File operations
    bool cmd_create_folder(const vector<string> &tokens);
    bool cmd_delete(const vector<string> &tokens);
    bool cmd_rename(const vector<string> &tokens);
    bool cmd_move(const vector<string> &tokens);
    bool cmd_copy(const vector<string> &tokens);
    bool cmd_restore(const vector<string> &tokens);
    bool cmd_list_deleted(const vector<string> &tokens);
    
    // Pause/Continue
    bool cmd_pause_upload(const vector<string> &tokens);
    bool cmd_continue_upload(const vector<string> &tokens);
    bool cmd_pause_download(const vector<string> &tokens);
    bool cmd_continue_download(const vector<string> &tokens);
    
    // Permissions
    bool cmd_set_permission(const vector<string> &tokens);
    bool cmd_check_permission(const vector<string> &tokens);
    
    // Unzip
    bool cmd_unzip(const vector<string> &tokens);

    bool ensure_authenticated();
    uint64_t file_size(const string &path);
    bool check_file_permission(const string &path,
                               bool need_view,
                               bool need_download,
                               bool need_edit,
                               int &owner_id_out,
                               string &owner_user_out,
                               int &file_id_out,
                               uint64_t &size_out,
                               bool &is_folder_out);

    int sockfd_;
    FileServer &server_;
    string username_;
    int user_id_ = 0;
    bool authenticated_ = false;
    bool counted_online_ = false;
};
