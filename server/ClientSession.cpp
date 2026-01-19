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

constexpr uint64_t k_fnv_offset = 1469598103934665603ULL;
constexpr uint64_t k_fnv_prime  = 1099511628211ULL;

uint64_t fnv1a64_update(uint64_t hash, const char *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        hash ^= static_cast<unsigned char>(data[i]);
        hash *= k_fnv_prime;
    }
    return hash;
}

uint64_t fnv1a64(const char *data, size_t len) {
    return fnv1a64_update(k_fnv_offset, data, len);
}

string hex_u64(uint64_t v) {
    stringstream ss;
    ss << hex << setw(16) << setfill('0') << v;
    return ss.str();
}

string normalize_version(const string &version) {
    string out = version;
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool parse_version_token(const string &token, string &out) {
    if (token.rfind("v=", 0) != 0) return false;
    out = normalize_version(token.substr(2));
    return !out.empty();
}

} 

uint64_t ClientSession::compute_disk_usage(const string &base_dir) {
    namespace fs = std::filesystem;
    uint64_t total = 0;
    std::error_code ec;
    if (!fs::exists(base_dir)) return 0;
    for (auto &entry : fs::recursive_directory_iterator(base_dir, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        auto rel = fs::relative(entry.path(), base_dir, ec);
        if (ec) continue;
        if (!rel.empty() && rel.begin()->string() == ".trash" ) continue;
        if (entry.is_regular_file()) {
            total += (uint64_t)entry.file_size();
        }
    }
    return total;
}

ClientSession::ClientSession(int sockfd, FileServer &server)
    : sockfd_(sockfd),
      server_(server) {}

ClientSession::~ClientSession() {
    if (counted_online_ && !username_.empty()) {
        server_.release_all_edit_locks(username_);
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
    if (cmd == "PING")     return cmd_ping();
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
    if (cmd == "LIST_ACL") return cmd_list_acl(tokens);
    
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

    const uint64_t default_quota = 2ull * 1024ull * 1024ull * 1024ull; // 2 GB
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
        size = stoull(tokens[1]);
    } catch (...) {
        send_line(sockfd_, "ERR 400 Invalid size");
        return true;
    }

    string rel_path;
    for (size_t i = 2; i < tokens.size(); i++) {
        if (i > 2) rel_path += " ";
        rel_path += tokens[i];
    }

    string base_dir  = server_.root_dir() + "/" + username_;
    string full_path = base_dir + "/" + rel_path;
    uint64_t old_size = file_size(full_path);

    // Refresh quota from DB and recompute disk usage for accurate check
    uint64_t quota_limit = 0;
    UserRecord quota_user;
    string err_quota;
    if (server_.db().get_user_by_username(username_, quota_user, err_quota)) {
        quota_limit = quota_user.quota_bytes;
        server_.quota_mgr().set_limit(username_, quota_limit);
        uint64_t disk_used = compute_disk_usage(base_dir);
        server_.quota_mgr().adjust_usage(username_, (int64_t)disk_used - (int64_t)server_.quota_mgr().used(username_));
        server_.db().update_used_bytes(user_id_, disk_used, err_quota);
    }

    uint64_t current_used = server_.quota_mgr().used(username_);
    uint64_t additional = size > old_size ? size - old_size : 0;
    
    // Check quota BEFORE accepting upload
    if (quota_limit > 0 && current_used + additional > quota_limit) {
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

    // ===== ADD: upload state for pause =====
    auto state = server_.create_upload_state(username_, rel_path);
    state->offset = 0;
    state->paused = false;

    while (remaining > 0) {
        // ===== ADD: check pause =====
        if (state->paused) {
            uint64_t current_offset = size - remaining;

            if (session_id < 0) {
                server_.db().create_transfer_session(
                    user_id_, rel_path, "UPLOAD",
                    size, current_offset, session_id, err
                );
            } else {
                server_.db().update_transfer_session(session_id, current_offset, err);
            }

            ofs.flush();
            ofs.close();
            send_line(sockfd_, "OK 200 Upload paused");
            return true;
        }
        // ===== END ADD =====

        size_t chunk = remaining > BUF_SIZE ? BUF_SIZE : (size_t)remaining;

        if (!recv_exact(sockfd_, buf.data(), chunk)) {
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
        state->offset = size - remaining;   // ⭐ ADD
        server_.add_bytes_in(chunk);
    }

    ofs.close();
    server_.remove_upload_state(username_, rel_path);
    ::rename(tmp_path.c_str(), full_path.c_str());
    int64_t delta = static_cast<int64_t>(size) - static_cast<int64_t>(old_size);
    server_.quota_mgr().adjust_usage(username_, delta);

    uint64_t used = server_.quota_mgr().used(username_);
    server_.db().update_used_bytes(user_id_, used, err);
    if (!server_.db().upsert_file_entry(user_id_, rel_path, size, false, err)) {
        server_.logger().log(username_, "UPLOAD DB error: " + err);
        send_line(sockfd_, "ERR 500 DB error: " + err);
        return true;
    }

    if (session_id >= 0) {
        server_.db().delete_transfer_session(session_id, err);
    } else {
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

    string rel_path = tokens[1];

    int owner_id = 0, file_id = 0;
    string owner_user;
    uint64_t size_meta = 0;
    bool is_folder = false;

    if (!check_file_permission(rel_path, false, true, false,
                               owner_id, owner_user, file_id, size_meta, is_folder)) {
        send_line(sockfd_, "ERR 403 Permission denied");
        return true;
    }

    string full_path = server_.root_dir() + "/" + owner_user + "/" + rel_path;
    uint64_t size = file_size(full_path);

    if (size == 0) {
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

    uint64_t offset = 0;
    uint64_t sent   = 0;

    while (sent < size) {
        size_t chunk = min<uint64_t>(BUF_SIZE, size - sent);
        ifs.read(buf.data(), (streamsize)chunk);
        streamsize got = ifs.gcount();
        if (got <= 0) break;

        if (!send_all(sockfd_, buf.data(), (size_t)got)) {
            // ❗ offset = số byte gửi thành công
            string err;
            int session_id;

            if (!server_.db().get_transfer_session(
                    user_id_, rel_path, "DOWNLOAD",
                    session_id, offset, size, err)) {
                server_.db().create_transfer_session(
                    user_id_, rel_path, "DOWNLOAD",
                    size, offset, session_id, err);
            } else {
                server_.db().update_transfer_session(session_id, offset, err);
            }
            return false;
        }

        sent   += (uint64_t)got;
        offset += (uint64_t)got;
        server_.add_bytes_out((uint64_t)got);

        // update mỗi ~640KB
        if (sent % (BUF_SIZE * 10) == 0) {
            string err;
            int session_id;
            if (server_.db().get_transfer_session(
                    user_id_, rel_path, "DOWNLOAD",
                    session_id, offset, size, err)) {
                server_.db().update_transfer_session(session_id, offset, err);
            }
        }
    }

    // done → xóa session
    string err;
    int session_id;
    uint64_t dummy1, dummy2;
    if (server_.db().get_transfer_session(
            user_id_, rel_path, "DOWNLOAD",
            session_id, dummy1, dummy2, err)) {
        server_.db().delete_transfer_session(session_id, err);
    }

    server_.logger().log(username_, "DOWNLOAD completed " + rel_path);
    return true;
}


bool ClientSession::cmd_get_text(const vector<string> &tokens) {
    if (tokens.size() < 2) {
        send_line(sockfd_, "ERR 400 Usage: GET_TEXT <path> [LOCK]");
        return true;
    }

    string rel_path;
    bool want_lock = false;
    size_t last = tokens.size();
    if (tokens.back() == "LOCK") {
        want_lock = true;
        last = tokens.size() - 1;
    }
    if (last < 2) {
        send_line(sockfd_, "ERR 400 Usage: GET_TEXT <path> [LOCK]");
        return true;
    }
    for (size_t i = 1; i < last; ++i) {
        if (i > 1) rel_path += " ";
        rel_path += tokens[i];
    }
    if (!is_txt_file(rel_path)) {
        send_line(sockfd_, "ERR 415 Only .txt allowed");
        return true;
    }

    int owner_id = 0, file_id = 0;
    string owner_user;
    uint64_t size_meta = 0;
    bool is_folder = false;

    if (want_lock) {
        if (!check_file_permission(rel_path, false, false, true, owner_id, owner_user, file_id, size_meta, is_folder)) {
            send_line(sockfd_, "ERR 403 Permission denied (edit required)");
            return true;
        }
        string locked_by;
        if (!server_.try_lock_edit(owner_user, rel_path, username_, locked_by)) {
            send_line(sockfd_, "ERR 423 Locked by " + locked_by);
            return true;
        }
    } else {
        // Check permission (view or edit)
        if (!check_file_permission(rel_path, true, false, false, owner_id, owner_user, file_id, size_meta, is_folder) &&
            !check_file_permission(rel_path, false, false, true, owner_id, owner_user, file_id, size_meta, is_folder)) {
            send_line(sockfd_, "ERR 403 Permission denied");
            return true;
        }
    }

    string full_path = server_.root_dir() + "/" + owner_user + "/" + rel_path;

    ifstream ifs(full_path);
    if (!ifs) {
        if (want_lock) {
            server_.release_edit_lock(owner_user, rel_path, username_);
        }
        send_line(sockfd_, "ERR 404 File not found");
        return true;
    }

    string content((istreambuf_iterator<char>(ifs)),
                   istreambuf_iterator<char>());

    uint64_t size = content.size();
    string version = "v=" + hex_u64(fnv1a64(content.data(), content.size()));
    send_line(sockfd_, "OK 100 " + to_string(size) + " " + version);
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

    string rel_path;
    string version_token;
    size_t last = tokens.size();
    if (parse_version_token(tokens.back(), version_token)) {
        last = tokens.size() - 1;
    }
    static_cast<void>(version_token);
    if (last < 3) {
        send_line(sockfd_, "ERR 400 Usage: PUT_TEXT <path> <size>");
        return true;
    }
    for (size_t i = 1; i + 1 < last; ++i) {
        if (i > 1) rel_path += " ";
        rel_path += tokens[i];
    }
    if (rel_path.empty()) {
        send_line(sockfd_, "ERR 400 Usage: PUT_TEXT <path> <size>");
        return true;
    }
    if (!is_txt_file(rel_path)) {
        send_line(sockfd_, "ERR 415 Only .txt allowed");
        return true;
    }

    uint64_t size = 0;
    try {
        size = stoull(tokens[last - 1]);
    } catch (...) {
        send_line(sockfd_, "ERR 400 Invalid size");
        return true;
    }

    string err;
    int owner_id = user_id_;
    int file_id = 0;
    string owner_user = username_;
    uint64_t meta_size = 0;
    bool is_folder = false;
    bool is_deleted = false;

    bool file_exists = server_.db().get_file_entry(user_id_, rel_path, file_id, meta_size, is_folder, is_deleted, err);
    if (file_exists && !is_deleted) {
        if (!check_file_permission(rel_path, false, false, true, owner_id, owner_user, file_id, meta_size, is_folder)) {
            send_line(sockfd_, "ERR 403 Permission denied (edit required)");
            return true;
        }
    } else {
        if (check_file_permission(rel_path, false, false, true, owner_id, owner_user, file_id, meta_size, is_folder)) {
            // owner_id/owner_user set by helper
        } else {
            owner_id = user_id_;
            owner_user = username_;
        }
    }

    auto file_lock = server_.lock_file(owner_user, rel_path);
    static_cast<void>(file_lock);

    string base_dir  = server_.root_dir() + "/" + owner_user;
    string full_path = base_dir + "/" + rel_path;
    bool disk_exists = utils::file_exists(full_path);
    uint64_t old_size = file_size(full_path);

    if (disk_exists) {
        string locked_by;
        if (!server_.get_edit_lock_owner(owner_user, rel_path, locked_by)) {
            send_line(sockfd_, "ERR 409 Not locked (use GET_TEXT LOCK)");
            return true;
        }
        if (locked_by != username_) {
            send_line(sockfd_, "ERR 423 Locked by " + locked_by);
            return true;
        }
    }

    // Refresh quota state from DB
    uint64_t quota_limit = 0;
    UserRecord quota_user;
    string err_user;
    if (server_.db().get_user_by_username(owner_user, quota_user, err_user)) {
        quota_limit = quota_user.quota_bytes;
        server_.quota_mgr().set_limit(owner_user, quota_limit);
        uint64_t disk_used = compute_disk_usage(base_dir);
        server_.quota_mgr().adjust_usage(owner_user, (int64_t)disk_used - (int64_t)server_.quota_mgr().used(owner_user));
        server_.db().update_used_bytes(owner_id, disk_used, err_user);
    }

    uint64_t current_used = server_.quota_mgr().used(owner_user);
    uint64_t additional = size > old_size ? size - old_size : 0;
    if (quota_limit > 0 && current_used + additional > quota_limit) {
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
    uint64_t new_hash = k_fnv_offset;
    uint64_t remaining = size;
    while (remaining > 0) {
        size_t chunk = remaining > BUF_SIZE ? BUF_SIZE : (size_t)remaining;
        if (!recv_exact(sockfd_, buf.data(), chunk)) {
            send_line(sockfd_, "ERR 500 Receive error");
            return false;
        }
        new_hash = fnv1a64_update(new_hash, buf.data(), chunk);
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
    int64_t delta = (int64_t)size - (int64_t)old_size;
    server_.quota_mgr().adjust_usage(owner_user, delta);

    uint64_t used = server_.quota_mgr().used(owner_user);
    server_.db().update_used_bytes(owner_id, used, err);
    if (!server_.db().upsert_file_entry(owner_id, rel_path, size, false, err)) {
        server_.logger().log(username_, "PUT_TEXT DB error: " + err);
        send_line(sockfd_, "ERR 500 DB error: " + err);
        return true;
    }
    if (disk_exists) {
        server_.release_edit_lock(owner_user, rel_path, username_);
    }
    string new_version = hex_u64(new_hash);
    server_.logger().log(username_, "PUT_TEXT " + rel_path + " size=" + to_string(size) + " v=" + new_version);
    send_line(sockfd_, "OK 200 Text file updated v=" + new_version);
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

bool ClientSession::cmd_ping() {
    // Simple heartbeat to confirm the TCP session is still alive.
    send_line(sockfd_, "OK 200 PONG");
    return true;
}




bool ClientSession::cmd_list_db(const vector<string> &tokens) {
    string err;
    string paths;

    if (!server_.db().list_files(user_id_, paths, err)) {
        send_line(sockfd_, "ERR 500 DB error: " + err);
        return true;
    }

    // Build combined list with ownership + permissions:
    // path|size|is_folder|owner|can_view|can_download|can_edit
    vector<string> lines;
    string line;
    stringstream ss_owned(paths);
    while (getline(ss_owned, line)) {
        if (line.empty()) continue;
        size_t p1 = line.find('|');
        size_t p2 = line.find('|', p1 + 1);
        if (p1 == string::npos || p2 == string::npos) continue;
        string path = line.substr(0, p1);
        string size_str = line.substr(p1 + 1, p2 - p1 - 1);
        string folder_flag = line.substr(p2 + 1);
        lines.push_back(path + "|" + size_str + "|" + folder_flag + "|" + username_ + "|1|1|1");
    }

    string shared;
    server_.db().list_shared_files(user_id_, shared, err);
    stringstream ss_shared(shared);
    while (getline(ss_shared, line)) {
        if (line.empty()) continue;
        lines.push_back(line);
    }

    send_line(sockfd_, "OK 200 " + to_string((int)lines.size()));
    string out;
    for (auto &l : lines) {
        out += l + "\n";
    }
    if (!out.empty()) {
        send_all(sockfd_, out.data(), out.size());
    }
    return true;
}

bool ClientSession::cmd_logout() {
    if (authenticated_) {
        authenticated_ = false;

        if (counted_online_) {
            server_.release_all_edit_locks(username_);
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
    int perm_file_id = 0;

    // First try owned file
    if (server_.db().get_file_entry(user_id_, path, file_id_out, size_bytes, is_folder, is_deleted, err) && !is_deleted) {
        owner_id_out = user_id_;
        owner_user_out = username_;
    } else {
        // Try shared file
        int owner_id = 0;
        string owner_user;
        if (!server_.db().find_shared_file(path, user_id_, perm_file_id, owner_id, owner_user, err)) {
            return false;
        }
        owner_id_out = owner_id;
        owner_user_out = owner_user;
        if (!server_.db().get_file_entry(owner_id_out, path, file_id_out, size_bytes, is_folder, is_deleted, err) || is_deleted) {
            return false;
        }
    }

    bool can_view = false, can_download = false, can_edit = false;
    int perm_id_to_check = (owner_id_out == user_id_) ? file_id_out : (perm_file_id ? perm_file_id : file_id_out);
    if (!server_.db().check_permission(perm_id_to_check, user_id_, can_view, can_download, can_edit, err)) {
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
    
    // Support paths containing spaces and optional provided total size as last token
    string rel_path;
    uint64_t provided_total = 0;
    if (tokens.size() >= 3) {
        // if last token is all digits, treat it as provided_total
        bool last_is_number = true;
        for (char c : tokens.back()) if (!isdigit((unsigned char)c)) { last_is_number = false; break; }
        if (last_is_number) {
            try { provided_total = stoull(tokens.back()); } catch (...) { provided_total = 0; }
            // rel_path is tokens[1..n-2]
            for (size_t i = 1; i + 1 < tokens.size(); ++i) {
                if (i > 1) rel_path += " ";
                rel_path += tokens[i];
            }
        } else {
            // no total provided, rel_path is tokens[1..]
            for (size_t i = 1; i < tokens.size(); ++i) {
                if (i > 1) rel_path += " ";
                rel_path += tokens[i];
            }
        }
    } else {
        rel_path = tokens[1];
    }
    string base_dir = server_.root_dir() + "/" + username_;
    string full_path = base_dir + "/" + rel_path;
    
    namespace fs = std::filesystem;
    if (!utils::ensure_dir(full_path)) {
        send_line(sockfd_, "ERR 500 Cannot create folder");
        return true;
    }
    
    string err;
    if (!server_.db().upsert_file_entry(user_id_, rel_path, 0, true, err)) {
        server_.logger().log(username_, "CREATE_FOLDER DB error: " + err);
        send_line(sockfd_, "ERR 500 DB error: " + err);
        return true;
    }
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
    
    // Paths
    string base_dir = server_.root_dir() + "/" + username_;
    string full_path = base_dir + "/" + rel_path;
    
    // Ensure it exists on disk
    struct stat st{};
    if (::stat(full_path.c_str(), &st) != 0) {
        send_line(sockfd_, "ERR 404 File not found");
        return true;
    }

    // Ensure DB entry exists so restore/list work
    string err;
    int file_id;
    if (!server_.db().get_file_id_by_path(user_id_, rel_path, file_id, err)) {
        bool is_folder = S_ISDIR(st.st_mode);
        uint64_t sz = is_folder ? 0 : (uint64_t)st.st_size;
        server_.db().upsert_file_entry(user_id_, rel_path, sz, is_folder, err);
    }
    
    // Move to trash (folder or file)
    string trash_dir = base_dir + "/.trash";
    utils::ensure_dir(trash_dir);
    string trash_path = trash_dir + "/" + rel_path;
    size_t parent_pos = trash_path.find_last_of('/');
    if (parent_pos != string::npos && parent_pos > 0) {
        utils::ensure_dir(trash_path.substr(0, parent_pos));
    }

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::rename(full_path, trash_path, ec);
    if (ec) {
        fs::remove_all(trash_path, ec);
        ec.clear();
        fs::rename(full_path, trash_path, ec);
    }
    if (ec) {
        fs::copy(full_path, trash_path, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (!ec) fs::remove_all(full_path, ec);
    }
    if (ec) {
        send_line(sockfd_, "ERR 500 Move to trash failed");
        return true;
    }
    
    // Update quota: subtract total size of subtree moved
    uint64_t moved_bytes = compute_disk_usage(trash_path);
    server_.quota_mgr().adjust_usage(username_, -(int64_t)moved_bytes);
    uint64_t used = server_.quota_mgr().used(username_);
    server_.db().update_used_bytes(user_id_, used, err);

    // Mark DB entries (path and children) as deleted
    string paths;
    if (server_.db().list_files(user_id_, paths, err)) {
        string line;
        for (char c : paths) {
            if (c == '\n') {
                if (!line.empty()) {
                    size_t p = line.find('|');
                    if (p != string::npos) {
                        string pth = line.substr(0, p);
                        if (pth == rel_path || (pth.size() > rel_path.size() + 1 && pth.rfind(rel_path + "/", 0) == 0)) {
                            server_.db().delete_file_entry(user_id_, pth, err);
                        }
                    }
                    line.clear();
                }
            } else {
                line += c;
            }
        }
        if (!line.empty()) {
            size_t p = line.find('|');
            if (p != string::npos) {
                string pth = line.substr(0, p);
                if (pth == rel_path || (pth.size() > rel_path.size() + 1 && pth.rfind(rel_path + "/", 0) == 0)) {
                    server_.db().delete_file_entry(user_id_, pth, err);
                }
            }
        }
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
    
    struct stat st{};
    if (::stat(src_full.c_str(), &st) != 0) {
        send_line(sockfd_, "ERR 404 Source not found");
        return true;
    }
    
    // Check quota BEFORE copying
    uint64_t quota_limit = 0;
    UserRecord quota_user;
    string err_quota;
    if (server_.db().get_user_by_username(username_, quota_user, err_quota)) {
        quota_limit = quota_user.quota_bytes;
        server_.quota_mgr().set_limit(username_, quota_limit);
        uint64_t disk_used = compute_disk_usage(base_dir);
        server_.quota_mgr().adjust_usage(username_, (int64_t)disk_used - (int64_t)server_.quota_mgr().used(username_));
        server_.db().update_used_bytes(user_id_, disk_used, err_quota);
    }
    
    uint64_t current_used = server_.quota_mgr().used(username_);
    uint64_t total_copy_size = 0;
    
    // Calculate total size to copy
    if (S_ISREG(st.st_mode)) {
        total_copy_size = (uint64_t)st.st_size;
    } else if (S_ISDIR(st.st_mode)) {
        namespace fs = std::filesystem;
        for (auto &entry : fs::recursive_directory_iterator(src_full)) {
            if (entry.is_regular_file()) {
                total_copy_size += (uint64_t)entry.file_size();
            }
        }
    }
    
    if (quota_limit > 0 && current_used + total_copy_size > quota_limit) {
        send_line(sockfd_, "ERR 403 Quota exceeded");
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
        string err;
        server_.db().copy_file_entry(user_id_, src_path, dst_path, err);
        server_.quota_mgr().adjust_usage(username_, (int64_t)size);
        uint64_t used = server_.quota_mgr().used(username_);
        server_.db().update_used_bytes(user_id_, used, err);
    } else if (S_ISDIR(st.st_mode)) {
        namespace fs = std::filesystem;
        try {
            if (!utils::ensure_dir(dst_full)) {
                send_line(sockfd_, "ERR 500 Cannot create destination directory");
                return true;
            }
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
    string tmp_path = base_dir + "/" + rel_path + ".tmp";

    // (will lookup state after verifying transfer session)

    if (!utils::file_exists(tmp_path)) {
        send_line(sockfd_, "ERR 404 No uploading file to pause");
        return true;
    }

    int session_id;
    uint64_t offset, total_size;
    string err;

    if (!server_.db().get_transfer_session(
            user_id_, rel_path, "UPLOAD",
            session_id, offset, total_size, err)) {
        send_line(sockfd_, "ERR 409 No active upload session");
        return true;
    }

    auto state = server_.get_upload_state(username_, rel_path);
    if (!state) {
        send_line(sockfd_, "ERR 409 No active upload session");
        return true;
    }

    state->paused = true;
    uint64_t current_offset = state->offset;


    server_.db().update_transfer_session(session_id, current_offset, err);

    server_.logger().log(
        username_, "PAUSE_UPLOAD " + rel_path +
        " at " + to_string(current_offset));

    send_line(sockfd_,
        "OK 200 Upload paused at offset " +
        to_string(current_offset));

    return true;
}


bool ClientSession::cmd_continue_upload(const vector<string> &tokens) {
    if (tokens.size() < 2) {
        send_line(sockfd_, "ERR 400 Usage: CONTINUE_UPLOAD <path>");
        return true;
    }

    // Support paths containing spaces and optional provided total size as last token
    string rel_path;
    uint64_t provided_total = 0;
    if (tokens.size() >= 3) {
        bool last_is_number = true;
        for (char c : tokens.back()) if (!isdigit((unsigned char)c)) { last_is_number = false; break; }
        if (last_is_number) {
            try { provided_total = stoull(tokens.back()); } catch (...) { provided_total = 0; }
            for (size_t i = 1; i + 1 < tokens.size(); ++i) {
                if (i > 1) rel_path += " ";
                rel_path += tokens[i];
            }
        } else {
            for (size_t i = 1; i < tokens.size(); ++i) {
                if (i > 1) rel_path += " ";
                rel_path += tokens[i];
            }
        }
    } else {
        rel_path = tokens[1];
    }

    string base_dir = server_.root_dir() + "/" + username_;
    string full_path = base_dir + "/" + rel_path;
    string tmp_path  = full_path + ".tmp";

    int session_id;
    uint64_t offset, total_size;
    string err;

    if (!server_.db().get_transfer_session(
            user_id_, rel_path, "UPLOAD",
            session_id, offset, total_size, err)) {
        // fallback: if no DB session but temp file exists, allow resume if client provided total size or can infer
        if (utils::file_exists(tmp_path)) {
            uint64_t actual_tmp = file_size(tmp_path);

            if (provided_total == 0) {
                // try to get total from file_entry metadata
                int fid; bool is_folder, is_deleted; uint64_t meta_size = 0;
                string derr;
                if (server_.db().get_file_entry(user_id_, rel_path, fid, meta_size, is_folder, is_deleted, derr) && !is_deleted) {
                    provided_total = meta_size;
                }
            }

            if (provided_total == 0) {
                send_line(sockfd_, "ERR 404 No paused upload found");
                return true;
            }

            // create a DB session so subsequent flows treat it as paused
            if (!server_.db().create_transfer_session(user_id_, rel_path, "UPLOAD", provided_total, actual_tmp, session_id, err)) {
                send_line(sockfd_, "ERR 500 DB error: " + err);
                return true;
            }
            offset = actual_tmp;
            total_size = provided_total;
        } else {
            send_line(sockfd_, "ERR 404 No paused upload found");
            return true;
        }
    }

    if (!utils::file_exists(tmp_path)) {
        send_line(sockfd_, "ERR 409 Temp file missing");
        return true;
    }

    uint64_t actual_size = file_size(tmp_path);
    if (actual_size != offset) {
        send_line(sockfd_, "ERR 409 Offset mismatch");
        return true;
    }

    if (offset >= total_size) {
        server_.db().delete_transfer_session(session_id, err);
        send_line(sockfd_, "OK 200 Upload already completed");
        return true;
    }

    uint64_t remaining = total_size - offset;

    if (offset >= total_size) {
        server_.db().delete_transfer_session(session_id, err);
        send_line(sockfd_, "ERR 409 Invalid resume offset");
        return true;
    }


    send_line(sockfd_,
        "OK 100 Continue from " +
        to_string(offset) +
        " size " +
        to_string(remaining));

    ofstream ofs(tmp_path, ios::binary | ios::app);
    if (!ofs) {
        send_line(sockfd_, "ERR 500 Cannot open temp file");
        return true;
    }

    const size_t BUF_SIZE = 64 * 1024;
    vector<char> buf(BUF_SIZE);
    uint64_t received = 0;

    while (received < remaining) {
        size_t chunk =
            (remaining - received > BUF_SIZE)
            ? BUF_SIZE
            : (size_t)(remaining - received);

        if (!recv_exact(sockfd_, buf.data(), chunk)) {
            // client disconnected while uploading -> persist offset and treat as paused
            server_.db().update_transfer_session(session_id, offset + received, err);
            server_.logger().log(username_, "CONTINUE_UPLOAD client disconnected, paused at " + to_string(offset + received));
            // Return true to indicate command handled; connection likely closed by client.
            return true;
        }

        ofs.write(buf.data(), (streamsize)chunk);
        if (!ofs) {
            send_line(sockfd_, "ERR 500 Write error");
            return true;
        }

        received += chunk;
        server_.add_bytes_in(chunk);
    }

    ofs.close();

    // ✅ Hoàn tất
    ::rename(tmp_path.c_str(), full_path.c_str());
    server_.db().delete_transfer_session(session_id, err);

    uint64_t final_size = file_size(full_path);
    server_.db().upsert_file_entry(
        user_id_, rel_path, final_size, false, err);

    server_.logger().log(
        username_, "CONTINUE_UPLOAD completed " + rel_path);

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

    if (!server_.db().get_transfer_session(
            user_id_, rel_path, "DOWNLOAD",
            session_id, offset, total_size, err)) {
        send_line(sockfd_, "ERR 404 No paused download found");
        return true;
    }

    int owner_id = 0, file_id = 0;
    string owner_user;
    uint64_t size_meta = 0;
    bool is_folder = false;

    if (!check_file_permission(rel_path, false, true, false,
                               owner_id, owner_user, file_id, size_meta, is_folder)) {
        send_line(sockfd_, "ERR 403 Permission denied");
        return true;
    }

    string full_path = server_.root_dir() + "/" + owner_user + "/" + rel_path;
    uint64_t actual_size = file_size(full_path);

    if (offset >= actual_size) {
        server_.db().delete_transfer_session(session_id, err);
        send_line(sockfd_, "OK 200 Download already completed");
        return true;
    }

    ifstream ifs(full_path, ios::binary);
    if (!ifs) {
        send_line(sockfd_, "ERR 500 Cannot open file");
        return true;
    }

    ifs.seekg((streamsize)offset);
    uint64_t remaining = actual_size - offset;

    send_line(sockfd_, "OK 100 Continue from " +
                         to_string(offset) +
                         " size " +
                         to_string(remaining));

    const size_t BUF_SIZE = 64 * 1024;
    vector<char> buf(BUF_SIZE);
    uint64_t sent = 0;

    while (sent < remaining) {
        size_t chunk = min<uint64_t>(BUF_SIZE, remaining - sent);
        ifs.read(buf.data(), (streamsize)chunk);
        streamsize got = ifs.gcount();
        if (got <= 0) break;

        if (!send_all(sockfd_, buf.data(), (size_t)got)) {
            server_.db().update_transfer_session(session_id, offset + sent, err);
            server_.logger().log(username_, "CONTINUE_DOWNLOAD client disconnected, paused at " + to_string(offset + sent));
            // Return true to indicate command handled; connection likely closed by client.
            return true;
        }

        sent   += (uint64_t)got;
        offset += (uint64_t)got;
        server_.add_bytes_out((uint64_t)got);

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
    
    // tokens: CMD path(with spaces) target view download edit
    if (tokens.size() < 6) {
        send_line(sockfd_, "ERR 400 Usage: SET_PERMISSION <path> <target_user> <view> <download> <edit>");
        return true;
    }
    size_t target_idx = tokens.size() - 4;
    if (target_idx < 1) {
        send_line(sockfd_, "ERR 400 Usage: SET_PERMISSION <path> <target_user> <view> <download> <edit>");
        return true;
    }
    string rel_path = tokens[1];
    for (size_t i = 2; i < target_idx; ++i) {
        rel_path += " " + tokens[i];
    }
    string target_user = tokens[target_idx];
    string view_tok = tokens[target_idx + 1];
    string dl_tok = tokens[target_idx + 2];
    string edit_tok = tokens[target_idx + 3];
    bool can_view = view_tok == "1" || view_tok == "true";
    bool can_download = dl_tok == "1" || dl_tok == "true";
    bool can_edit = edit_tok == "1" || edit_tok == "true";

    // Get file_id
    string err;
    int file_id;
    uint64_t sz = 0;
    bool is_folder = false, is_deleted = false;
    if (!server_.db().get_file_entry(user_id_, rel_path, file_id, sz, is_folder, is_deleted, err) || is_deleted) {
        send_line(sockfd_, "ERR 404 File not found");
        return true;
    }
    
    // Get target user_id
    UserRecord target_rec;
    if (!server_.db().get_user_by_username(target_user, target_rec, err)) {
        send_line(sockfd_, "ERR 404 Target user not found");
        return true;
    }
    
    auto apply_perm = [&](int fid) -> bool {
        if (!server_.db().set_permission(fid, target_rec.id, can_view, can_download, can_edit, err)) {
            return false;
        }
        return true;
    };

    if (!is_folder) {
        if (!apply_perm(file_id)) {
            send_line(sockfd_, "ERR 500 Cannot set permission: " + err);
            return true;
        }
    } else {
        // Apply to folder and all descendants to keep role consistent
        std::string paths_blob;
        if (!server_.db().list_files(user_id_, paths_blob, err)) {
            send_line(sockfd_, "ERR 500 DB error: " + err);
            return true;
        }
        // Always include the folder itself
        if (!apply_perm(file_id)) {
            send_line(sockfd_, "ERR 500 Cannot set permission: " + err);
            return true;
        }
        std::string line;
        std::stringstream ss(paths_blob);
        std::string prefix = rel_path;
        if (!prefix.empty() && prefix.back() != '/') prefix += "/";
        while (std::getline(ss, line)) {
            if (line.empty()) continue;
            size_t p1 = line.find('|');
            if (p1 == std::string::npos) continue;
            std::string pth = line.substr(0, p1);
            if (pth == rel_path) continue;
            if (pth.rfind(prefix, 0) != 0) continue;
            int child_id;
            if (server_.db().get_file_id_by_path(user_id_, pth, child_id, err)) {
                apply_perm(child_id);
            }
        }
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

bool ClientSession::cmd_list_acl(const vector<string> &tokens) {
    if (tokens.size() < 2) {
        send_line(sockfd_, "ERR 400 Usage: LIST_ACL <path>");
        return true;
    }
    string rel_path = tokens[1];
    for (size_t i = 2; i < tokens.size(); ++i) {
        rel_path += " " + tokens[i];
    }

    // Only owner can list ACL
    string err;
    int file_id;
    if (!server_.db().get_file_id_by_path(user_id_, rel_path, file_id, err)) {
        send_line(sockfd_, "ERR 404 File not found");
        return true;
    }

    string rows;
    if (!server_.db().list_file_acl(user_id_, file_id, rows, err)) {
        send_line(sockfd_, "ERR 500 DB error: " + err);
        return true;
    }

    int count = 0;
    for (char c : rows) if (c == '\n') count++;
    send_line(sockfd_, "OK 200 " + to_string(count));
    if (!rows.empty()) send_all(sockfd_, rows.data(), rows.size());
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

    // Scan extracted content to update DB/quota
    namespace fs = std::filesystem;
    string extract_root = target_dir.empty() ? base_dir : base_dir + "/" + target_dir;

    auto flatten_single_nested = [](const string &root) {
        namespace fs = std::filesystem;
        fs::path rp(root);
        if (!fs::exists(rp) || !fs::is_directory(rp)) return;
        size_t count = 0;
        fs::path only;
        for (auto &entry : fs::directory_iterator(rp)) {
            ++count;
            only = entry.path();
            if (count > 1) break;
        }
        if (count == 1 && fs::is_directory(only)) {
            for (auto &sub : fs::directory_iterator(only)) {
                fs::path dest = rp / sub.path().filename();
                std::error_code ec;
                fs::rename(sub.path(), dest, ec);
                if (ec) {
                    fs::copy(sub.path(), dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
                    fs::remove_all(sub.path(), ec);
                }
            }
            std::error_code ec;
            fs::remove_all(only, ec);
        }
    };

    // Avoid double folder level (zip already contains a root folder)
    flatten_single_nested(extract_root);

    if (utils::ensure_dir(extract_root)) {
        for (auto &entry : fs::recursive_directory_iterator(extract_root)) {
            string rel_path = fs::relative(entry.path(), base_dir).generic_string();
            if (entry.is_directory()) {
                string err;
                server_.db().upsert_file_entry(user_id_, rel_path, 0, true, err);
                num_entries++;
            } else if (entry.is_regular_file()) {
                uint64_t sz = (uint64_t)entry.file_size();
                total_extracted += sz;
                string err;

                // adjust quota vs previous size
                int file_id_dummy;
                uint64_t old_size = 0;
                bool is_folder = false;
                bool is_deleted = false;
                if (server_.db().get_file_entry(user_id_, rel_path, file_id_dummy, old_size, is_folder, is_deleted, err) && !is_deleted) {
                    int64_t delta = (int64_t)sz - (int64_t)old_size;
                    server_.quota_mgr().adjust_usage(username_, delta);
                } else {
                    server_.quota_mgr().adjust_usage(username_, (int64_t)sz);
                }
                server_.db().update_used_bytes(user_id_, server_.quota_mgr().used(username_), err);
                server_.db().upsert_file_entry(user_id_, rel_path, sz, false, err);
                num_entries++;
            }
        }
    }
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
