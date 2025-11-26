// ===== file: server/ClientSession.cpp =====
#include "ClientSession.hpp"
#include "FileServer.hpp"
#include "../common/Protocol.hpp"
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>

using namespace std;
using namespace proto;

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

    // Demo: pass == password_hash
    if (pass != rec.password_hash) {
        server_.logger().log(user, "Login failed (wrong password)");
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

    if (!server_.quota_mgr().can_allocate(username_, size)) {
        send_line(sockfd_, "ERR 403 Quota exceeded");
        return true;
    }

    string base_dir  = server_.root_dir() + "/" + username_;
    string full_path = base_dir + "/" + rel_path;
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
    server_.quota_mgr().add_usage(username_, size);

    string err;
    uint64_t used = server_.quota_mgr().used(username_);
    server_.db().update_used_bytes(user_id_, used, err);

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
    uint64_t size   = stoull(tokens[2]);

    if (!server_.quota_mgr().can_allocate(username_, size)) {
        send_line(sockfd_, "ERR 403 Quota exceeded");
        return true;
    }

    string base_dir  = server_.root_dir() + "/" + username_;
    string full_path = base_dir + "/" + rel_path;
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
    server_.quota_mgr().add_usage(username_, size);

    string err;
    uint64_t used = server_.quota_mgr().used(username_);
    server_.db().update_used_bytes(user_id_, used, err);

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
