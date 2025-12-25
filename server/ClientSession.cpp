// ===== file: server/ClientSession.cpp =====
#include "ClientSession.hpp"
#include "FileServer.hpp"
#include "../common/Protocol.hpp"
#include "../common/Utils.hpp"
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <dirent.h>
#include <cstring>
#include <vector>
#include <algorithm>
#ifdef HAVE_LIBZIP
#include <zip.h>
#endif

using namespace std;
using namespace proto;

namespace {
// Hash mật khẩu đơn giản để tránh lưu plaintext (không dùng cho bảo mật thực tế).
string hash_password(const string &raw) {
    std::hash<string> hasher;
    size_t h = hasher(raw);
    stringstream ss;
    ss << hex << h;
    return ss.str();
}

bool is_txt_file(const string &path) {
    const string ext = ".txt";
    if (path.size() < ext.size()) return false;
    return path.compare(path.size() - ext.size(), ext.size(), ext) == 0;
}
} 

ClientSession::ClientSession(int sockfd, FileServer &server)
    : sockfd_(sockfd),
      server_(server) {}

ClientSession::~ClientSession() {
    if (counted_online_ && !username_.empty()) {
        server_.user_logout(username_);
    }
}


void ClientSession::run() {
    string line;
    while (recv_line(sockfd_, line)) {
        if (!handle_command(line)) break;
    }
}

bool ClientSession::handle_command(const string &line) {
    vector<string> tokens = split_tokens(line);
    if (tokens.empty()) {
        send_line(sockfd_, "ERR 400 Empty command");
        return true;
    }

    string cmd = tokens[0];

    if (cmd == "AUTH") {
        return cmd_auth(tokens);
    }
    if (cmd == "REGISTER") {
        return cmd_register(tokens);
    }
    if (cmd == "WHO")      return cmd_who();       // <- QUAN TRỌNG PHẢI ĐỂ TRÊN
    if (cmd == "STATS")    return cmd_stats(); 

    if (!ensure_authenticated()) return false;

    if (cmd == "UPLOAD")    return cmd_upload(tokens);
    if (cmd == "DOWNLOAD")  return cmd_download(tokens);
    if (cmd == "GET_TEXT")  return cmd_get_text(tokens);
    if (cmd == "PUT_TEXT")  return cmd_put_text(tokens);
    if (cmd == "LIST_DB")   return cmd_list_db(tokens);
    if (cmd == "LOGOUT")    return cmd_logout();
    
    // File operations
    if (cmd == "CREATE_FOLDER") return cmd_create_folder(tokens);
    if (cmd == "DELETE")    return cmd_delete(tokens);
    if (cmd == "RENAME")    return cmd_rename(tokens);
    if (cmd == "MOVE")      return cmd_move(tokens);
    if (cmd == "COPY")      return cmd_copy(tokens);
    if (cmd == "RESTORE")   return cmd_restore(tokens);
    if (cmd == "LIST_DELETED") return cmd_list_deleted(tokens);
    
    // Pause/Continue
    if (cmd == "PAUSE_UPLOAD")   return cmd_pause_upload(tokens);
    if (cmd == "CONTINUE_UPLOAD") return cmd_continue_upload(tokens);
    if (cmd == "PAUSE_DOWNLOAD") return cmd_pause_download(tokens);
    if (cmd == "CONTINUE_DOWNLOAD") return cmd_continue_download(tokens);
    
    // Permissions
    if (cmd == "SET_PERMISSION") return cmd_set_permission(tokens);
    if (cmd == "CHECK_PERMISSION") return cmd_check_permission(tokens);
    
    // Unzip
    if (cmd == "UNZIP")     return cmd_unzip(tokens);


    send_line(sockfd_, "ERR 400 Unknown command");
    return true;
}

bool ClientSession::ensure_authenticated() {
    if (!authenticated_) {
        send_line(sockfd_, "ERR 401 Not authenticated");
        return false;
    }
    return true;
}

bool ClientSession::cmd_auth(const vector<string> &tokens) {
    if (tokens.size() < 3) {
        send_line(sockfd_, "ERR 400 Usage: AUTH <user> <pass>");
        return true;
    }

    string user = tokens[1];
    string pass = tokens[2];

    UserRecord rec;
    string err;

    // user không tồn tại
    if (!server_.db().get_user_by_username(user, rec, err)) {
        server_.logger().log(user, "Login failed (user not found)");
        send_line(sockfd_, "ERR 403 Invalid credentials");
        return true;
    }

    // password sai
    string pass_hashed = hash_password(pass);
    if (!(pass_hashed == rec.password_hash || pass == rec.password_hash)) {
        server_.logger().log(user, "Login failed (wrong password)");
        send_line(sockfd_, "ERR 403 Invalid credentials");
        return true;
    }

    //CHECK ĐĂNG NHẬP TRÙNG
    if (!counted_online_ && server_.is_user_online(user)) {
        send_line(sockfd_, "ERR 409 User already logged in");
        return true;      // giữ socket, không đóng
    }

    // đánh dấu phiên này đã login
    authenticated_ = true;
    username_      = rec.username;
    user_id_       = rec.id;

    // đánh dấu user online (chỉ 1 lần / socket)
    if (!counted_online_) {
        counted_online_ = true;
        server_.user_login(username_);
    }

    // quota
    server_.quota_mgr().set_limit(username_, rec.quota_bytes);
    server_.quota_mgr().add_usage(username_, rec.used_bytes);

    // log
    server_.logger().log(user, "Login success");
    server_.db().insert_log(user_id_, "login", "Login success", "0.0.0.0", err);

    send_line(sockfd_, "OK 200 Authenticated");
    return true;
}



bool ClientSession::cmd_register(const vector<string> &tokens) {
    if (tokens.size() < 3) {
        send_line(sockfd_, "ERR 400 Usage: REGISTER <user> <pass>");
        return true;
    }

    string user = tokens[1];
    string pass = tokens[2];

    UserRecord rec;
    string err;
    if (server_.db().get_user_by_username(user, rec, err)) {
        send_line(sockfd_, "ERR 409 User already exists");
        return true;
    }
    if (!err.empty()) {server_.user_login(username_);
        send_line(sockfd_, "ERR 500 DB error: " + err);
        return true;
    }

    const uint64_t default_quota = 100ull * 1024ull * 1024ull; // 100 MB
    string pass_hashed = hash_password(pass);

    if (!server_.db().create_user(user, pass_hashed, default_quota, err)) {
        if (err.find("UNIQUE") != string::npos) {
            send_line(sockfd_, "ERR 409 User already exists");
        } else {
            send_line(sockfd_, "ERR 500 DB error: " + err);
        }
        return true;
    }

    server_.logger().log(user, "REGISTER success");
    send_line(sockfd_, "OK 201 Registered");
    return true;
}

uint64_t ClientSession::file_size(const string &path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) == 0) return (uint64_t)st.st_size;
    return 0;
}

bool ClientSession::cmd_upload(const vector<string> &tokens) {
    if (tokens.size() < 3) {
        send_line(sockfd_, "ERR 400 Usage: UPLOAD <path> <size>");
        return true;
    }

    uint64_t size;
    try {
        size = stoull(tokens[1]);      // <== SIZE nằm trước
    } catch (...) {
        send_line(sockfd_, "ERR 400 Invalid size");
        return true;
    }

    // GHÉP phần còn lại làm path (kể cả có space)
    string rel_path;
    for (size_t i = 2; i < tokens.size(); i++) {
        if (i > 2) rel_path += " ";
        rel_path += tokens[i];
    }


    string base_dir  = server_.root_dir() + "/" + username_;
    string full_path = base_dir + "/" + rel_path;
    uint64_t old_size = file_size(full_path);
    uint64_t additional = size > old_size ? size - old_size : 0;

    if (!server_.quota_mgr().can_allocate(username_, additional)) {
        send_line(sockfd_, "ERR 403 Quota exceeded");
        return true;
    }

    string tmp_path  = full_path + ".tmp";

    ::mkdir(server_.root_dir().c_str(), 0755);
    ::mkdir(base_dir.c_str(), 0755);
    size_t parent_pos = full_path.find_last_of('/');
    if (parent_pos != string::npos && parent_pos > 0) {
        utils::ensure_dir(full_path.substr(0, parent_pos));
    }

    ofstream ofs(tmp_path, ios::binary);
    if (!ofs) {
        send_line(sockfd_, "ERR 500 Cannot open temp file");
        return true;
    }

    send_line(sockfd_, "OK 100 Ready to receive");

    const size_t BUF_SIZE = 64 * 1024;
    vector<char> buf(BUF_SIZE);
    uint64_t remaining = size;
    int session_id = -1;
    string err;

    while (remaining > 0) {
        size_t chunk = remaining > BUF_SIZE ? BUF_SIZE : (size_t)remaining;
        if (!recv_exact(sockfd_, buf.data(), chunk)) {
            // Connection lost - save state for resume
            uint64_t current_offset = size - remaining;
            if (session_id < 0) {
                server_.db().create_transfer_session(user_id_, rel_path, "UPLOAD", size, current_offset, session_id, err);
            } else {
                server_.db().update_transfer_session(session_id, current_offset, err);
            }
            return false;
        }
        ofs.write(buf.data(), (streamsize)chunk);
        if (!ofs) {
            send_line(sockfd_, "ERR 500 Write error");
            return true;
        }
        remaining -= chunk;
        server_.add_bytes_in(chunk);
    }
    ofs.close();

    ::rename(tmp_path.c_str(), full_path.c_str());
    int64_t delta = static_cast<int64_t>(size) - static_cast<int64_t>(old_size);
    server_.quota_mgr().adjust_usage(username_, delta);

    uint64_t used = server_.quota_mgr().used(username_);
    server_.db().update_used_bytes(user_id_, used, err);
    // Lưu metadata file (kích thước, đường dẫn) để thống kê.
    server_.db().upsert_file_entry(user_id_, rel_path, size, false, err);

    // Clean up any transfer session on successful completion
    if (session_id >= 0) {
        server_.db().delete_transfer_session(session_id, err);
    } else {
        // Check if there was a previous session
        int old_session_id;
        uint64_t dummy_offset, dummy_size;
        if (server_.db().get_transfer_session(user_id_, rel_path, "UPLOAD", old_session_id, dummy_offset, dummy_size, err)) {
            server_.db().delete_transfer_session(old_session_id, err);
        }
    }

    server_.logger().log(username_, "UPLOAD " + rel_path + " size=" + to_string(size));
    send_line(sockfd_, "OK 200 Upload completed");
    return true;
}

bool ClientSession::cmd_download(const vector<string> &tokens) {
    if (tokens.size() < 2) {
        send_line(sockfd_, "ERR 400 Usage: DOWNLOAD <path>");
        return true;
    }

    string rel_path  = tokens[1];
    int owner_id = 0, file_id = 0;
    string owner_user;
    uint64_t size_meta = 0;
    bool is_folder = false;

    // Check permission and resolve owner (could be shared)
    if (!check_file_permission(rel_path, false, true, false, owner_id, owner_user, file_id, size_meta, is_folder)) {
        send_line(sockfd_, "ERR 403 Permission denied");
        return true;
    }

    string full_path = server_.root_dir() + "/" + owner_user + "/" + rel_path;

    uint64_t size = file_size(full_path);
    if (size == 0 && size_meta == 0) {
        send_line(sockfd_, "ERR 404 File not found");
        return true;
    }

    ifstream ifs(full_path, ios::binary);
    if (!ifs) {
        send_line(sockfd_, "ERR 500 Cannot open file");
        return true;
    }

    send_line(sockfd_, "OK 100 " + to_string(size));

    const size_t BUF_SIZE = 64 * 1024;
    vector<char> buf(BUF_SIZE);
    uint64_t remaining = size;

    while (remaining > 0) {
        size_t chunk = remaining > BUF_SIZE ? BUF_SIZE : (size_t)remaining;
        ifs.read(buf.data(), (streamsize)chunk);
        streamsize got = ifs.gcount();
        if (got <= 0) break;

        if (!send_all(sockfd_, buf.data(), (size_t)got)) {
            // Connection lost - could save state for resume
            string err;
            int session_id;
            uint64_t current_offset = size - remaining;
            if (!server_.db().get_transfer_session(user_id_, rel_path, "DOWNLOAD", session_id, current_offset, size, err)) {
                server_.db().create_transfer_session(user_id_, rel_path, "DOWNLOAD", size, current_offset, session_id, err);
            } else {
                server_.db().update_transfer_session(session_id, current_offset, err);
            }
            return false;
        }
        remaining -= (uint64_t)got;
        server_.add_bytes_out((uint64_t)got);
    }

    // Clean up any transfer session on successful completion
    string err;
    int session_id;
    uint64_t dummy_offset, dummy_size;
    if (server_.db().get_transfer_session(user_id_, rel_path, "DOWNLOAD", session_id, dummy_offset, dummy_size, err)) {
        server_.db().delete_transfer_session(session_id, err);
    }

    server_.logger().log(username_, "DOWNLOAD " + rel_path + " size=" + to_string(size));
    return true;
}

bool ClientSession::cmd_get_text(const vector<string> &tokens) {
    if (tokens.size() < 2) {
        send_line(sockfd_, "ERR 400 Usage: GET_TEXT <path>");
        return true;
    }

    string rel_path  = tokens[1];
    if (!is_txt_file(rel_path)) {
        send_line(sockfd_, "ERR 415 Only .txt allowed");
        return true;
    }

    int owner_id = 0, file_id = 0;
    string owner_user;
    uint64_t size_meta = 0;
    bool is_folder = false;

    // Check permission (view or edit)
    if (!check_file_permission(rel_path, true, false, false, owner_id, owner_user, file_id, size_meta, is_folder) &&
        !check_file_permission(rel_path, false, false, true, owner_id, owner_user, file_id, size_meta, is_folder)) {
        send_line(sockfd_, "ERR 403 Permission denied");
        return true;
    }

    string full_path = server_.root_dir() + "/" + owner_user + "/" + rel_path;

    ifstream ifs(full_path);
    if (!ifs) {
        send_line(sockfd_, "ERR 404 File not found");
        return true;
    }

    string content((istreambuf_iterator<char>(ifs)),
                   istreambuf_iterator<char>());

    uint64_t size = content.size();
    send_line(sockfd_, "OK 100 " + to_string(size));
    if (!send_all(sockfd_, content.data(), content.size())) {
        return false;
    }
    server_.add_bytes_out(size);
    server_.logger().log(username_, "GET_TEXT " + rel_path + " size=" + to_string(size));
    return true;
}

bool ClientSession::cmd_put_text(const vector<string> &tokens) {
    if (tokens.size() < 3) {
        send_line(sockfd_, "ERR 400 Usage: PUT_TEXT <path> <size>");
        return true;
    }
    string rel_path = tokens[1];
    if (!is_txt_file(rel_path)) {
        send_line(sockfd_, "ERR 415 Only .txt allowed");
        return true;
    }
    
    // Check permission (edit) - but allow if file doesn't exist yet (new file)
    string err;
    int owner_id = user_id_;
    int file_id = 0;
    string owner_user = username_;
    uint64_t meta_size = 0;
    bool is_folder = false;
    bool is_deleted = false;

    bool file_exists = server_.db().get_file_entry(user_id_, rel_path, file_id, meta_size, is_folder, is_deleted, err);
    if (file_exists) {
        if (!check_file_permission(rel_path, false, false, true, owner_id, owner_user, file_id, meta_size, is_folder)) {
            send_line(sockfd_, "ERR 403 Permission denied (edit required)");
            return true;
        }
    } else {
        // If file is shared, also allow edit if ACL permits
        if (check_file_permission(rel_path, false, false, true, owner_id, owner_user, file_id, meta_size, is_folder)) {
            // owner_id/owner_user set by helper
        } else {
            // New file under current user
            owner_id = user_id_;
            owner_user = username_;
        }
    }
    
    uint64_t size   = stoull(tokens[2]);
    string base_dir  = server_.root_dir() + "/" + owner_user;
    string full_path = base_dir + "/" + rel_path;
    uint64_t old_size = file_size(full_path);
    uint64_t additional = size > old_size ? size - old_size : 0;

    if (!server_.quota_mgr().can_allocate(owner_user, additional)) {
        send_line(sockfd_, "ERR 403 Quota exceeded");
        return true;
    }
    string tmp_path  = full_path + ".tmp";
    ::mkdir(server_.root_dir().c_str(), 0755);
    ::mkdir(base_dir.c_str(), 0755);
    ofstream ofs(tmp_path);
    if (!ofs) {
        send_line(sockfd_, "ERR 500 Cannot open temp file");
        return true;
    }
    send_line(sockfd_, "OK 100 Ready to receive");
    const size_t BUF_SIZE = 64 * 1024; //64KB
    vector<char> buf(BUF_SIZE);
    uint64_t remaining = size;
    while (remaining > 0) {
        size_t chunk = remaining > BUF_SIZE ? BUF_SIZE : (size_t)remaining;
        if (!recv_exact(sockfd_, buf.data(), chunk)) {
            send_line(sockfd_, "ERR 500 Receive error");
            return false;
        }
        ofs.write(buf.data(), (streamsize)chunk);
        if (!ofs) {
            send_line(sockfd_, "ERR 500 Write error");
            return true;
        }
        remaining -= chunk;
        server_.add_bytes_in(chunk);
    }
    ofs.close();
    ::rename(tmp_path.c_str(), full_path.c_str());
    int64_t delta = static_cast<int64_t>(size) - static_cast<int64_t>(old_size);
    int64_t new_used = server_.quota_mgr().adjust_usage(owner_user, delta);
    server_.db().update_used_bytes(owner_id, static_cast<uint64_t>(new_used), err);
    server_.db().upsert_file_entry(owner_id, rel_path, size, false, err);
    server_.logger().log(username_, "PUT_TEXT " + rel_path + " size=" + to_string(size));
    send_line(sockfd_, "OK 200 Text file updated");
    return true;
}

bool ClientSession::cmd_stats() {
    string msg = "OK 200 online=" + to_string(server_.online_users_count()) +
                 " bytes_in=" + to_string(server_.bytes_in()) +
                 " bytes_out=" + to_string(server_.bytes_out());
    send_line(sockfd_, msg);
    server_.logger().log(username_, "STATS");
    return true;
}




bool ClientSession::cmd_list_db(const vector<string> &tokens) {
    string err;
    string paths;

    if (!server_.db().list_files(user_id_, paths, err)) {
        send_line(sockfd_, "ERR 500 DB error: " + err);
        return true;
    }

    // Trả số dòng + nội dung
    int count = 0;
    for (char c : paths) if (c == '\n') count++;

    send_line(sockfd_, "OK 200 " + to_string(count));
    send_all(sockfd_, paths.data(), paths.size());  // gửi raw
    return true;
}

bool ClientSession::cmd_logout() {
    if (authenticated_) {
        authenticated_ = false;

        if (counted_online_) {
            server_.user_logout(username_);
            counted_online_ = false;
        }
    }

    send_line(sockfd_, "OK 200 Logged out");
    return true;   // tiếp tục run(), không đóng socket
}

bool ClientSession::cmd_who() {
    auto &online = server_.get_online_users();

    string msg = "OK 200 Users online: ";
    bool first = true;

    for (auto &p : online) {
        if (!first) msg += ", ";
        msg += p.first;
        first = false;
    }

    send_line(sockfd_, msg);
    return true;
}

bool ClientSession::check_file_permission(const string &path,
                                          bool need_view,
                                          bool need_download,
                                          bool need_edit,
                                          int &owner_id_out,
                                          string &owner_user_out,
                                          int &file_id_out,
                                          uint64_t &size_out,
                                          bool &is_folder_out) {
    string err;
    uint64_t size_bytes = 0;
    bool is_folder = false;
    bool is_deleted = false;

    // First try owned file
    if (server_.db().get_file_entry(user_id_, path, file_id_out, size_bytes, is_folder, is_deleted, err) && !is_deleted) {
        owner_id_out = user_id_;
        owner_user_out = username_;
    } else {
        // Try shared file
        int owner_id = 0;
        string owner_user;
        if (!server_.db().find_shared_file(path, user_id_, file_id_out, owner_id, owner_user, err)) {
            return false;
        }
        owner_id_out = owner_id;
        owner_user_out = owner_user;
        if (!server_.db().get_file_entry(owner_id_out, path, file_id_out, size_bytes, is_folder, is_deleted, err) || is_deleted) {
            return false;
        }
    }

    bool can_view = false, can_download = false, can_edit = false;
    if (!server_.db().check_permission(file_id_out, user_id_, can_view, can_download, can_edit, err)) {
        return false;
    }

    if (need_view && !can_view) return false;
    if (need_download && !can_download) return false;
    if (need_edit && !can_edit) return false;

    size_out = size_bytes;
    is_folder_out = is_folder;
    return true;
}

bool ClientSession::cmd_create_folder(const vector<string> &tokens) {
    if (tokens.size() < 2) {
        send_line(sockfd_, "ERR 400 Usage: CREATE_FOLDER <path>");
        return true;
    }
    
    string rel_path = tokens[1];
    string base_dir = server_.root_dir() + "/" + username_;
    string full_path = base_dir + "/" + rel_path;
    
    namespace fs = std::filesystem;
    if (!utils::ensure_dir(full_path)) {
        send_line(sockfd_, "ERR 500 Cannot create folder");
        return true;
    }
    
    string err;
    server_.db().upsert_file_entry(user_id_, rel_path, 0, true, err);
    server_.logger().log(username_, "CREATE_FOLDER " + rel_path);
    send_line(sockfd_, "OK 200 Folder created");
    return true;
}

bool ClientSession::cmd_delete(const vector<string> &tokens) {
    if (tokens.size() < 2) {
        send_line(sockfd_, "ERR 400 Usage: DELETE <path>");
        return true;
    }
    
    string rel_path = tokens[1];
    
    // Check permission (owner can always delete)
    int file_id;
    string err;
    if (!server_.db().get_file_id_by_path(user_id_, rel_path, file_id, err)) {
        send_line(sockfd_, "ERR 404 File not found");
        return true;
    }
    
    string base_dir = server_.root_dir() + "/" + username_;
    string full_path = base_dir + "/" + rel_path;
    
    // Mark as deleted in DB
    if (!server_.db().delete_file_entry(user_id_, rel_path, err)) {
        send_line(sockfd_, "ERR 500 DB error: " + err);
        return true;
    }
    
    string trash_dir = base_dir + "/.trash";
    utils::ensure_dir(trash_dir);
    string trash_path = trash_dir + "/" + rel_path;
    size_t parent_pos = trash_path.find_last_of('/');
    if (parent_pos != string::npos && parent_pos > 0) {
        utils::ensure_dir(trash_path.substr(0, parent_pos));
    }

    // Move file/folder into trash for potential restore
    if (::rename(full_path.c_str(), trash_path.c_str()) != 0) {
        send_line(sockfd_, "ERR 500 Move to trash failed");
        return true;
    }

    // Update quota if it's a file (not folder)
    struct stat st{};
    if (::stat(trash_path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
        uint64_t size = (uint64_t)st.st_size;
        server_.quota_mgr().adjust_usage(username_, -(int64_t)size);
        uint64_t used = server_.quota_mgr().used(username_);
        server_.db().update_used_bytes(user_id_, used, err);
    }
    
    server_.logger().log(username_, "DELETE " + rel_path);
    send_line(sockfd_, "OK 200 Deleted");
    return true;
}

bool ClientSession::cmd_rename(const vector<string> &tokens) {
    if (tokens.size() < 3) {
        send_line(sockfd_, "ERR 400 Usage: RENAME <old_path> <new_path>");
        return true;
    }
    
    string old_path = tokens[1];
    string new_path = tokens[2];
    
    string base_dir = server_.root_dir() + "/" + username_;
    string old_full = base_dir + "/" + old_path;
    string new_full = base_dir + "/" + new_path;
    
    if (::rename(old_full.c_str(), new_full.c_str()) != 0) {
        send_line(sockfd_, "ERR 500 Rename failed");
        return true;
    }
    
    string err;
    if (!server_.db().rename_file_entry(user_id_, old_path, new_path, err)) {
        send_line(sockfd_, "ERR 500 DB error: " + err);
        return true;
    }
    
    server_.logger().log(username_, "RENAME " + old_path + " -> " + new_path);
    send_line(sockfd_, "OK 200 Renamed");
    return true;
}

bool ClientSession::cmd_move(const vector<string> &tokens) {
    if (tokens.size() < 3) {
        send_line(sockfd_, "ERR 400 Usage: MOVE <old_path> <new_path>");
        return true;
    }
    
    return cmd_rename(tokens); // Move is same as rename
}

bool ClientSession::cmd_copy(const vector<string> &tokens) {
    if (tokens.size() < 3) {
        send_line(sockfd_, "ERR 400 Usage: COPY <src_path> <dst_path>");
        return true;
    }
    
    string src_path = tokens[1];
    string dst_path = tokens[2];
    
    string base_dir = server_.root_dir() + "/" + username_;
    string src_full = base_dir + "/" + src_path;
    string dst_full = base_dir + "/" + dst_path;
    
    // Check if source exists
    struct stat st{};
    if (::stat(src_full.c_str(), &st) != 0) {
        send_line(sockfd_, "ERR 404 Source not found");
        return true;
    }
    
    // Copy file
    if (S_ISREG(st.st_mode)) {
        ifstream src(src_full, ios::binary);
        ofstream dst(dst_full, ios::binary);
        if (!src || !dst) {
            send_line(sockfd_, "ERR 500 Copy failed");
            return true;
        }
        dst << src.rdbuf();
        
        uint64_t size = (uint64_t)st.st_size;
        uint64_t additional = size;
        if (!server_.quota_mgr().can_allocate(username_, additional)) {
            ::unlink(dst_full.c_str());
            send_line(sockfd_, "ERR 403 Quota exceeded");
            return true;
        }
        
        string err;
        server_.db().copy_file_entry(user_id_, src_path, dst_path, err);
        server_.quota_mgr().adjust_usage(username_, (int64_t)size);
        uint64_t used = server_.quota_mgr().used(username_);
        server_.db().update_used_bytes(user_id_, used, err);
    } else if (S_ISDIR(st.st_mode)) {
        // Copy directory - create destination and copy recursively
        namespace fs = std::filesystem;
        try {
            if (!utils::ensure_dir(dst_full)) {
                send_line(sockfd_, "ERR 500 Cannot create destination directory");
                return true;
            }
            // Simple recursive copy
            DIR *dir = opendir(src_full.c_str());
            if (dir) {
                struct dirent *entry;
                while ((entry = readdir(dir)) != nullptr) {
                    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
                    string src_sub = src_full + "/" + entry->d_name;
                    string dst_sub = dst_full + "/" + entry->d_name;
                    string rel_sub_src = src_path + "/" + entry->d_name;
                    string rel_sub_dst = dst_path + "/" + entry->d_name;
                    
                    struct stat sub_st;
                    if (stat(src_sub.c_str(), &sub_st) == 0) {
                        if (S_ISDIR(sub_st.st_mode)) {
                            vector<string> sub_tokens = {"COPY", rel_sub_src, rel_sub_dst};
                            cmd_copy(sub_tokens);
                        } else {
                            vector<string> sub_tokens = {"COPY", rel_sub_src, rel_sub_dst};
                            cmd_copy(sub_tokens);
                        }
                    }
                }
                closedir(dir);
            }
            string err;
            server_.db().upsert_file_entry(user_id_, dst_path, 0, true, err);
        } catch (...) {
            send_line(sockfd_, "ERR 500 Copy directory failed");
            return true;
        }
    }
    
    server_.logger().log(username_, "COPY " + src_path + " -> " + dst_path);
    send_line(sockfd_, "OK 200 Copied");
    return true;
}

bool ClientSession::cmd_restore(const vector<string> &tokens) {
    if (tokens.size() < 2) {
        send_line(sockfd_, "ERR 400 Usage: RESTORE <path>");
        return true;
    }
    
    string rel_path = tokens[1];
    string err;
    
    if (!server_.db().restore_file_entry(user_id_, rel_path, err)) {
        send_line(sockfd_, "ERR 404 File not found in deleted list");
        return true;
    }
    
    string base_dir = server_.root_dir() + "/" + username_;
    string full_path = base_dir + "/" + rel_path;
    string trash_path = base_dir + "/.trash/" + rel_path;

    struct stat st{};
    if (::stat(trash_path.c_str(), &st) != 0) {
        send_line(sockfd_, "ERR 404 Cannot find deleted file content");
        return true;
    }

    size_t parent_pos = full_path.find_last_of('/');
    if (parent_pos != string::npos && parent_pos > 0) {
        utils::ensure_dir(full_path.substr(0, parent_pos));
    }

    if (::rename(trash_path.c_str(), full_path.c_str()) != 0) {
        send_line(sockfd_, "ERR 500 Restore failed");
        return true;
    }

    // Restore quota
    if (S_ISREG(st.st_mode)) {
        uint64_t size = (uint64_t)st.st_size;
        server_.quota_mgr().adjust_usage(username_, (int64_t)size);
        uint64_t used = server_.quota_mgr().used(username_);
        server_.db().update_used_bytes(user_id_, used, err);
    }
    
    server_.logger().log(username_, "RESTORE " + rel_path);
    send_line(sockfd_, "OK 200 Restored");
    return true;
}

bool ClientSession::cmd_list_deleted(const vector<string> &tokens) {
    string err;
    string rows;
    if (!server_.db().list_deleted_files(user_id_, rows, err)) {
        send_line(sockfd_, "ERR 500 DB error: " + err);
        return true;
    }

    int count = 0;
    for (char c : rows) if (c == '\n') count++;

    send_line(sockfd_, "OK 200 " + to_string(count));
    if (!rows.empty()) {
        send_all(sockfd_, rows.data(), rows.size());
    }
    server_.logger().log(username_, "LIST_DELETED");
    return true;
}

bool ClientSession::cmd_pause_upload(const vector<string> &tokens) {
    if (tokens.size() < 2) {
        send_line(sockfd_, "ERR 400 Usage: PAUSE_UPLOAD <path>");
        return true;
    }
    
    string rel_path = tokens[1];
    string base_dir = server_.root_dir() + "/" + username_;
    string full_path = base_dir + "/" + rel_path;

    string tmp_path = full_path + ".tmp";
    uint64_t current_size = utils::file_exists(tmp_path) ? file_size(tmp_path) : file_size(full_path);
    
    string err;
    int session_id;
    if (!server_.db().get_transfer_session(user_id_, rel_path, "UPLOAD", session_id, current_size, current_size, err)) {
        // Create new session
        uint64_t total_size = 0;
        if (tokens.size() >= 3) {
            try {
                total_size = stoull(tokens[2]);
            } catch (...) {}
        }
        if (!server_.db().create_transfer_session(user_id_, rel_path, "UPLOAD", total_size, current_size, session_id, err)) {
            send_line(sockfd_, "ERR 500 Cannot create session");
            return true;
        }
    } else {
        server_.db().update_transfer_session(session_id, current_size, err);
    }
    
    server_.logger().log(username_, "PAUSE_UPLOAD " + rel_path + " at " + to_string(current_size));
    send_line(sockfd_, "OK 200 Upload paused at offset " + to_string(current_size));
    return true;
}

bool ClientSession::cmd_continue_upload(const vector<string> &tokens) {
    if (tokens.size() < 2) {
        send_line(sockfd_, "ERR 400 Usage: CONTINUE_UPLOAD <path>");
        return true;
    }
    
    string rel_path = tokens[1];
    string err;
    int session_id;
    uint64_t offset, total_size;
    
    if (!server_.db().get_transfer_session(user_id_, rel_path, "UPLOAD", session_id, offset, total_size, err)) {
        send_line(sockfd_, "ERR 404 No paused upload found");
        return true;
    }
    
    string base_dir = server_.root_dir() + "/" + username_;
    string full_path = base_dir + "/" + rel_path;
    string tmp_path = full_path + ".tmp";
    string target_path = utils::file_exists(tmp_path) ? tmp_path : full_path;
    utils::ensure_dir(base_dir);
    size_t parent_pos = full_path.find_last_of('/');
    if (parent_pos != string::npos && parent_pos > 0) {
        utils::ensure_dir(full_path.substr(0, parent_pos));
    }
    
    if (total_size < offset) {
        send_line(sockfd_, "ERR 400 Invalid resume offset");
        return true;
    }

    // Continue from offset
    uint64_t remaining = total_size > offset ? total_size - offset : 0;
    if (remaining == 0) {
        server_.db().delete_transfer_session(session_id, err);
        send_line(sockfd_, "OK 200 Upload already completed");
        return true;
    }
    
    send_line(sockfd_, "OK 100 Continue from " + to_string(offset) + " size " + to_string(remaining));
    
    // Open file in append mode
    ofstream ofs(target_path, ios::binary | ios::app);
    if (!ofs) {
        send_line(sockfd_, "ERR 500 Cannot open file");
        return true;
    }
    
    const size_t BUF_SIZE = 64 * 1024;
    vector<char> buf(BUF_SIZE);
    uint64_t received = 0;
    
    while (received < remaining) {
        size_t chunk = (remaining - received) > BUF_SIZE ? BUF_SIZE : (size_t)(remaining - received);
        if (!recv_exact(sockfd_, buf.data(), chunk)) {
            // Update session on disconnect
            server_.db().update_transfer_session(session_id, offset + received, err);
            return false;
        }
        ofs.write(buf.data(), (streamsize)chunk);
        if (!ofs) {
            send_line(sockfd_, "ERR 500 Write error");
            return true;
        }
        received += chunk;
        offset += chunk;
        server_.add_bytes_in(chunk);
        
        // Update progress periodically
        if (received % (BUF_SIZE * 10) == 0) {
            server_.db().update_transfer_session(session_id, offset, err);
        }
    }
    
    ofs.close();
    server_.db().delete_transfer_session(session_id, err);
    
    if (target_path == tmp_path) {
        ::rename(tmp_path.c_str(), full_path.c_str());
    }
    uint64_t final_size = file_size(full_path);

    // Calculate delta from previous stored size (if any)
    uint64_t prev_size = 0;
    bool is_folder = false;
    bool is_deleted = false;
    int prev_file_id;
    if (server_.db().get_file_entry(user_id_, rel_path, prev_file_id, prev_size, is_folder, is_deleted, err) && !is_deleted) {
        // prev_size loaded
    } else {
        prev_size = 0;
    }

    int64_t delta = static_cast<int64_t>(final_size) - static_cast<int64_t>(prev_size);
    server_.quota_mgr().adjust_usage(username_, delta);
    uint64_t used = server_.quota_mgr().used(username_);
    server_.db().update_used_bytes(user_id_, used, err);
    server_.db().upsert_file_entry(user_id_, rel_path, final_size, false, err);
    
    server_.logger().log(username_, "CONTINUE_UPLOAD completed " + rel_path);
    send_line(sockfd_, "OK 200 Upload completed");
    return true;
}

bool ClientSession::cmd_pause_download(const vector<string> &tokens) {
    if (tokens.size() < 2) {
        send_line(sockfd_, "ERR 400 Usage: PAUSE_DOWNLOAD <path>");
        return true;
    }
    
    string rel_path = tokens[1];
    int owner_id = 0, file_id = 0;
    string owner_user;
    uint64_t size_meta = 0;
    bool is_folder = false;

    if (!check_file_permission(rel_path, false, true, false, owner_id, owner_user, file_id, size_meta, is_folder)) {
        send_line(sockfd_, "ERR 403 Permission denied");
        return true;
    }

    string base_dir = server_.root_dir() + "/" + owner_user;
    string full_path = base_dir + "/" + rel_path;
    
    uint64_t total_size = file_size(full_path);
    if (total_size == 0) total_size = size_meta;
    if (total_size == 0) {
        send_line(sockfd_, "ERR 404 File not found");
        return true;
    }
    
    // Get current offset from client if provided
    uint64_t offset = 0;
    if (tokens.size() >= 3) {
        try {
            offset = stoull(tokens[2]);
        } catch (...) {}
    }
    
    string err;
    int session_id;
    if (!server_.db().get_transfer_session(user_id_, rel_path, "DOWNLOAD", session_id, offset, total_size, err)) {
        if (!server_.db().create_transfer_session(user_id_, rel_path, "DOWNLOAD", total_size, offset, session_id, err)) {
            send_line(sockfd_, "ERR 500 Cannot create session");
            return true;
        }
    } else {
        server_.db().update_transfer_session(session_id, offset, err);
    }
    
    server_.logger().log(username_, "PAUSE_DOWNLOAD " + rel_path + " at " + to_string(offset));
    send_line(sockfd_, "OK 200 Download paused at offset " + to_string(offset));
    return true;
}

bool ClientSession::cmd_continue_download(const vector<string> &tokens) {
    if (tokens.size() < 2) {
        send_line(sockfd_, "ERR 400 Usage: CONTINUE_DOWNLOAD <path>");
        return true;
    }
    
    string rel_path = tokens[1];
    string err;
    int session_id;
    uint64_t offset, total_size;
    
    if (!server_.db().get_transfer_session(user_id_, rel_path, "DOWNLOAD", session_id, offset, total_size, err)) {
        send_line(sockfd_, "ERR 404 No paused download found");
        return true;
    }

    int owner_id = 0, file_id = 0;
    string owner_user;
    uint64_t size_meta = 0;
    bool is_folder = false;
    if (!check_file_permission(rel_path, false, true, false, owner_id, owner_user, file_id, size_meta, is_folder)) {
        send_line(sockfd_, "ERR 403 Permission denied");
        return true;
    }
    
    string base_dir = server_.root_dir() + "/" + owner_user;
    string full_path = base_dir + "/" + rel_path;
    
    ifstream ifs(full_path, ios::binary);
    if (!ifs) {
        send_line(sockfd_, "ERR 500 Cannot open file");
        return true;
    }
    
    ifs.seekg((streamsize)offset);
    uint64_t remaining = total_size - offset;
    
    send_line(sockfd_, "OK 100 Continue from " + to_string(offset) + " size " + to_string(remaining));
    
    const size_t BUF_SIZE = 64 * 1024;
    vector<char> buf(BUF_SIZE);
    uint64_t sent = 0;
    
    while (sent < remaining) {
        size_t chunk = (remaining - sent) > BUF_SIZE ? BUF_SIZE : (size_t)(remaining - sent);
        ifs.read(buf.data(), (streamsize)chunk);
        streamsize got = ifs.gcount();
        if (got <= 0) break;
        
        if (!send_all(sockfd_, buf.data(), (size_t)got)) {
            // Update session on disconnect
            server_.db().update_transfer_session(session_id, offset + sent, err);
            return false;
        }
        sent += (uint64_t)got;
        offset += (uint64_t)got;
        server_.add_bytes_out((uint64_t)got);
        
        // Update progress periodically
        if (sent % (BUF_SIZE * 10) == 0) {
            server_.db().update_transfer_session(session_id, offset, err);
        }
    }
    
    server_.db().delete_transfer_session(session_id, err);
    server_.logger().log(username_, "CONTINUE_DOWNLOAD completed " + rel_path);
    return true;
}

bool ClientSession::cmd_set_permission(const vector<string> &tokens) {
    if (tokens.size() < 5) {
        send_line(sockfd_, "ERR 400 Usage: SET_PERMISSION <path> <target_user> <view> <download> <edit>");
        return true;
    }
    
    string rel_path = tokens[1];
    string target_user = tokens[2];
    bool can_view = tokens[3] == "1" || tokens[3] == "true";
    bool can_download = tokens[4] == "1" || tokens[4] == "true";
    bool can_edit = tokens.size() >= 6 && (tokens[5] == "1" || tokens[5] == "true");
    
    // Get file_id
    string err;
    int file_id;
    if (!server_.db().get_file_id_by_path(user_id_, rel_path, file_id, err)) {
        send_line(sockfd_, "ERR 404 File not found");
        return true;
    }
    
    // Get target user_id
    UserRecord target_rec;
    if (!server_.db().get_user_by_username(target_user, target_rec, err)) {
        send_line(sockfd_, "ERR 404 Target user not found");
        return true;
    }
    
    if (!server_.db().set_permission(file_id, target_rec.id, can_view, can_download, can_edit, err)) {
        send_line(sockfd_, "ERR 500 Cannot set permission: " + err);
        return true;
    }
    
    server_.logger().log(username_, "SET_PERMISSION " + rel_path + " for " + target_user);
    send_line(sockfd_, "OK 200 Permission set");
    return true;
}

bool ClientSession::cmd_check_permission(const vector<string> &tokens) {
    if (tokens.size() < 2) {
        send_line(sockfd_, "ERR 400 Usage: CHECK_PERMISSION <path>");
        return true;
    }
    
    string rel_path = tokens[1];
    string err;
    int file_id;
    if (!server_.db().get_file_id_by_path(user_id_, rel_path, file_id, err)) {
        send_line(sockfd_, "ERR 404 File not found");
        return true;
    }
    
    bool can_view, can_download, can_edit;
    if (!server_.db().check_permission(file_id, user_id_, can_view, can_download, can_edit, err)) {
        send_line(sockfd_, "ERR 500 Cannot check permission: " + err);
        return true;
    }
    
    string msg = "OK 200 view=" + string(can_view ? "1" : "0") +
                 " download=" + string(can_download ? "1" : "0") +
                 " edit=" + string(can_edit ? "1" : "0");
    send_line(sockfd_, msg);
    return true;
}

bool ClientSession::cmd_unzip(const vector<string> &tokens) {
    if (tokens.size() < 2) {
        send_line(sockfd_, "ERR 400 Usage: UNZIP <zip_path> [target_dir]");
        return true;
    }
    
    string zip_path = tokens[1];
    string target_dir = tokens.size() >= 3 ? tokens[2] : "";
    
    string base_dir = server_.root_dir() + "/" + username_;
    string zip_full = base_dir + "/" + zip_path;
    
    if (!utils::file_exists(zip_full)) {
        send_line(sockfd_, "ERR 404 Zip file not found");
        return true;
    }
    
    // Check if it's a zip file
    if (zip_path.size() < 4 || zip_path.substr(zip_path.size() - 4) != ".zip") {
        send_line(sockfd_, "ERR 415 Not a zip file");
        return true;
    }
    
#ifdef HAVE_LIBZIP
    int err_code = 0;
    zip_t *zip = zip_open(zip_full.c_str(), ZIP_RDONLY, &err_code);
    if (!zip) {
        send_line(sockfd_, "ERR 500 Cannot open zip file");
        return true;
    }
    
    zip_int64_t num_entries = zip_get_num_entries(zip, 0);
    uint64_t total_zip_size = 0;
    for (zip_int64_t i = 0; i < num_entries; i++) {
        zip_stat_t stat;
        if (zip_stat_index(zip, i, 0, &stat) == 0) {
            total_zip_size += stat.size;
        }
    }
    if (!server_.quota_mgr().can_allocate(username_, total_zip_size)) {
        zip_close(zip);
        send_line(sockfd_, "ERR 403 Quota exceeded for unzip");
        return true;
    }

    string extract_dir = target_dir.empty() ? base_dir : base_dir + "/" + target_dir;
    utils::ensure_dir(extract_dir);
    
    uint64_t total_extracted = 0;
    for (zip_int64_t i = 0; i < num_entries; i++) {
        zip_stat_t stat;
        if (zip_stat_index(zip, i, 0, &stat) == 0) {
            if (stat.size > 0) { // File, not directory
                zip_file_t *zf = zip_fopen_index(zip, i, 0);
                if (zf) {
                    string entry_path = extract_dir + "/" + string(stat.name);
                    size_t last_slash = entry_path.find_last_of('/');
                    if (last_slash != string::npos) {
                        string entry_dir = entry_path.substr(0, last_slash);
                        utils::ensure_dir(entry_dir);
                    }
                    
                    ofstream ofs(entry_path, ios::binary);
                    if (ofs) {
                        vector<char> buf(stat.size);
                        zip_int64_t read = zip_fread(zf, buf.data(), stat.size);
                        if (read > 0) {
                            ofs.write(buf.data(), read);
                            total_extracted += read;
                        }
                        ofs.close();
                        
                        // Update quota
                        uint64_t additional = read;
                        if (!server_.quota_mgr().can_allocate(username_, additional)) {
                            zip_fclose(zf);
                            zip_close(zip);
                            send_line(sockfd_, "ERR 403 Quota exceeded during unzip");
                            return true;
                        }
                        server_.quota_mgr().adjust_usage(username_, (int64_t)read);
                        string err;
                        server_.db().upsert_file_entry(user_id_, 
                            (target_dir.empty() ? "" : target_dir + "/") + string(stat.name),
                            read, false, err);
                    }
                    zip_fclose(zf);
                }
            }
        }
    }
    
    zip_close(zip);
#else
    // Fallback: use system unzip command
    // -o to overwrite without prompting (avoid interactive question on macOS __MACOSX entries)
    string cmd = "cd \"" + base_dir + "\" && unzip -qo \"" + zip_path + "\"";
    if (!target_dir.empty()) {
        cmd += " -d \"" + target_dir + "\"";
    }
    int ret = system(cmd.c_str());
    if (ret != 0) {
        send_line(sockfd_, "ERR 500 Unzip failed (unzip command not available)");
        return true;
    }
    uint64_t total_extracted = 0;
    int64_t num_entries = 0; // Unknown
#endif
    
    string err;
    uint64_t used = server_.quota_mgr().used(username_);
    server_.db().update_used_bytes(user_id_, used, err);
    
    server_.logger().log(username_, "UNZIP " + zip_path + " extracted " + to_string(total_extracted) + " bytes");
#ifdef HAVE_LIBZIP
    send_line(sockfd_, "OK 200 Unzipped " + to_string(num_entries) + " entries");
#else
    send_line(sockfd_, "OK 200 Unzipped (using system unzip)");
#endif
    return true;
}
