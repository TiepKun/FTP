// ===== file: server/ClientSession.cpp =====
#include "ClientSession.hpp"
#include "FileServer.hpp"
#include "../common/Protocol.hpp"
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <mutex>
#include <sstream>
#include <iomanip>

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
} // namespace

ClientSession::ClientSession(int sockfd, FileServer &server)
    : sockfd_(sockfd),
      server_(server) {}

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

    if (!ensure_authenticated()) return false;

    if (cmd == "UPLOAD")    return cmd_upload(tokens);
    if (cmd == "DOWNLOAD")  return cmd_download(tokens);
    if (cmd == "GET_TEXT")  return cmd_get_text(tokens);
    if (cmd == "PUT_TEXT")  return cmd_put_text(tokens);
    if (cmd == "STATS")     return cmd_stats();

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
    if (!server_.db().get_user_by_username(user, rec, err)) {
        server_.logger().log(user, "Login failed (user not found)");
        send_line(sockfd_, "ERR 403 Invalid credentials");
        return false;
    }

    // So khớp hash; tạm cho phép chuỗi cũ (plaintext) để tương thích.
    string pass_hashed = hash_password(pass);
    if (!(pass_hashed == rec.password_hash || pass == rec.password_hash)) {
        server_.logger().log(user, "Login failed (wrong password)");
        // Log chi tiết để kiểm tra sai lệch hash (chỉ để debug).
        server_.logger().log(user, "Login debug stored=" + rec.password_hash +
                                      " computed=" + pass_hashed);
        send_line(sockfd_, "ERR 403 Invalid credentials");
        return false;
    }

    authenticated_ = true;
    username_      = rec.username;
    user_id_       = rec.id;

    server_.quota_mgr().set_limit(username_, rec.quota_bytes);
    server_.quota_mgr().add_usage(username_, rec.used_bytes);

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
    if (!err.empty()) {
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

    static mutex file_mtx;
    {
        lock_guard<mutex> lock(file_mtx);
        // Ghi credentials (hash) vào file cố định do FileServer thiết lập.
        const string &account_path = server_.account_file_path();
        ofstream ofs(account_path, ios::app);
        if (!ofs) {
            send_line(sockfd_, "ERR 500 Cannot open user_account.txt");
            return true;
        }
        // Lưu username + hash (không lưu plaintext).
        ofs << user << " " << pass_hashed << "\n";
        server_.logger().log(user, "REGISTER wrote credentials to " + account_path);
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

    string rel_path = tokens[1];
    uint64_t size   = stoull(tokens[2]);

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

    ofstream ofs(tmp_path, ios::binary);
    if (!ofs) {
        send_line(sockfd_, "ERR 500 Cannot open temp file");
        return true;
    }

    send_line(sockfd_, "OK 100 Ready to receive");

    const size_t BUF_SIZE = 64 * 1024;
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
    server_.quota_mgr().adjust_usage(username_, delta);

    string err;
    uint64_t used = server_.quota_mgr().used(username_);
    server_.db().update_used_bytes(user_id_, used, err);
    // Lưu metadata file (kích thước, đường dẫn) để thống kê.
    server_.db().upsert_file_entry(user_id_, rel_path, size, false, err);

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
    string full_path = server_.root_dir() + "/" + username_ + "/" + rel_path;

    uint64_t size = file_size(full_path);
    if (size == 0) {
        send_line(sockfd_, "ERR 404 File not found or empty");
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
            return false;
        }
        remaining -= (uint64_t)got;
        server_.add_bytes_out((uint64_t)got);
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

    string full_path = server_.root_dir() + "/" + username_ + "/" + rel_path;

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

    uint64_t size   = stoull(tokens[2]);

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

    ofstream ofs(tmp_path);
    if (!ofs) {
        send_line(sockfd_, "ERR 500 Cannot open temp file");
        return true;
    }

    send_line(sockfd_, "OK 100 Ready to receive");

    const size_t BUF_SIZE = 64 * 1024;
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
    int64_t new_used = server_.quota_mgr().adjust_usage(username_, delta);

    string err;
    server_.db().update_used_bytes(user_id_, static_cast<uint64_t>(new_used), err);
    server_.db().upsert_file_entry(user_id_, rel_path, size, false, err);

    server_.logger().log(username_, "PUT_TEXT " + rel_path + " size=" + to_string(size));
    send_line(sockfd_, "OK 200 Text file updated");
    return true;
}

bool ClientSession::cmd_stats() {
    string msg = "OK 200 active=" + to_string(server_.active_users()) +
                 " bytes_in=" + to_string(server_.bytes_in()) +
                 " bytes_out=" + to_string(server_.bytes_out());
    send_line(sockfd_, msg);
    server_.logger().log(username_, "STATS");
    return true;
}
