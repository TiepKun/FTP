// ===== file: client/NetworkClient.hpp =====
#pragma once
#include <string>
#include <cstdint>
#include <atomic>

using namespace std;

class NetworkClient {
public:
    NetworkClient();
    ~NetworkClient();

    bool connect_to(const string &host, int port);
    void close();

    bool auth(const string &user, const string &pass, string &err);
    bool register_user(const string &user, const string &pass, string &err);
    bool get_text(const string &path, string &content, string &err, bool lock_edit);
    bool put_text(const string &path, const string &content, string &new_version, string &err);
    bool upload_file(const std::string &local_path,
                 const std::string &remote_path,
                 std::string &err);

    bool upload_file_with_progress(const std::string &local_path,
                               const std::string &remote_path,
                               std::atomic<uint64_t> &sent,
                               std::string &err);
    bool download_file(const string &remote_path,
                       const string &local_path,
                       string &err);
    bool download_file_with_progress(
        const string &remote_path,
        const string &local_path,
        std::atomic<uint64_t> &received,
        uint64_t &total,
        string &err
    );
    bool pause_upload(const string &remote_path, string &err);
    bool continue_upload(const string &remote_path, const string &local_path, string &err);
    bool continue_upload_with_progress(const string &remote_path, const string &local_path,
                                       std::atomic<uint64_t> &sent, uint64_t &total, string &err);
    // After server has replied OK 100 Continue from <offset> size <remaining>,
    // send remaining bytes from local file starting at `offset`.
    bool resume_upload_stream(const string &remote_path, const string &local_path,
                              uint64_t offset, std::atomic<uint64_t> &sent, uint64_t total, string &err);
    bool pause_download(const string &remote_path, uint64_t offset, string &err);
    bool continue_download(const string &remote_path, const string &local_path, string &err);
    bool continue_download_with_progress(const string &remote_path, const string &local_path,
                                         std::atomic<uint64_t> &received, uint64_t &total, string &err);
    // Ensure socket is connected and authenticated (reconnect+auth when needed)
    bool ensure_connected(string &err);
    bool unzip_remote(const string &zip_path, const string &target_dir, string &err);
    bool create_remote_folder(const string &remote_path, string &err);
    bool rename_remote(const string &old_path, const string &new_path, string &err);
    bool move_remote(const string &old_path, const string &new_path, string &err);
    bool delete_remote(const string &path, string &err);
    bool restore_remote(const string &path, string &err);
    bool list_deleted(string &rows, string &err);
    bool set_permission(const string &path, const string &target_user, bool can_view, bool can_download, bool can_edit, string &err);
    bool list_acl(const string &path, string &rows, string &err);
    bool list_files_db(string &paths, string &err);
    bool send_raw_command(const string& cmd, string& out, string& err);
    bool ping(string &err);
             

private:
    int sockfd_ = -1;
    // store last connected host/port and last successful auth credentials
    std::string last_host_;
    int last_port_ = 0;
    std::string last_user_;
    std::string last_pass_;
};
