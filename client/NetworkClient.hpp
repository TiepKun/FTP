// ===== file: client/NetworkClient.hpp =====
#pragma once
#include <string>
#include <cstdint>

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

    bool upload_file(const string &local_path,
                 const string &remote_path,
                 string &err);
    bool download_file(const string &remote_path,
                       const string &local_path,
                       string &err);
    bool pause_upload(const string &remote_path, uint64_t total_size, string &err);
    bool continue_upload(const string &remote_path, const string &local_path, string &err);
    bool pause_download(const string &remote_path, uint64_t offset, string &err);
    bool continue_download(const string &remote_path, const string &local_path, string &err);
    bool unzip_remote(const string &zip_path, const string &target_dir, string &err);
    bool create_remote_folder(const string &remote_path, string &err);
    bool list_files_db(string &paths, string &err);
    bool send_raw_command(const string& cmd, string& out, string& err);
             

private:
    int sockfd_ = -1;
};
