// ===== file: client/NetworkClient.cpp =====
#include "NetworkClient.hpp"
#include "../common/Protocol.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fstream>
#include <vector>
#include <cstdint>
#include <atomic>

using namespace std;
using namespace proto;

static bool parse_u64_safe(const std::string &s, uint64_t &out) {
    try {
        out = stoull(s);
        return true;
    } catch (...) {
        out = 0;
        return false;
    }
}

NetworkClient::NetworkClient() {}

NetworkClient::~NetworkClient() {
    close();
}

bool NetworkClient::connect_to(const string &host, int port) {
    close();
    sockfd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd_ < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        ::close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    if (::connect(sockfd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        ::close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    // remember host/port for possible reconnects
    last_host_ = host;
    last_port_ = port;

    return true;
}

void NetworkClient::close() {
    if (sockfd_ >= 0) {
        ::close(sockfd_);
        sockfd_ = -1;
    }
}

bool NetworkClient::auth(const string &user, const string &pass, string &err) {
    if (sockfd_ < 0) {
        err = "Not connected";
        return false;
    }

    string cmd = "AUTH " + user + " " + pass;
    if (!send_line(sockfd_, cmd)) {
        err = "Send error";
        return false;
    }

    string line;
    if (!recv_line(sockfd_, line)) {
        err = "No response";
        return false;
    }

    if (line.rfind("OK", 0) == 0) {
        // remember credentials for reconnect+auth
        last_user_ = user;
        last_pass_ = pass;
        return true;
    }
    err = line;    return false;
}

bool NetworkClient::ensure_connected(string &err) {
    if (sockfd_ >= 0) return true;
    if (last_host_.empty() || last_port_ == 0) { err = "No stored host/port to reconnect"; return false; }
    if (last_user_.empty() || last_pass_.empty()) { err = "No stored credentials to re-authenticate"; return false; }
    if (!connect_to(last_host_, last_port_)) { err = "Cannot reconnect to server"; return false; }
    if (!auth(last_user_, last_pass_, err)) {
        close();
        return false;
    }
    return true;
}

bool NetworkClient::register_user(const string &user, const string &pass, string &err) {
    if (sockfd_ < 0) {
        err = "Not connected";
        return false;
    }

    string cmd = "REGISTER " + user + " " + pass;
    if (!send_line(sockfd_, cmd)) {
        err = "Send error";
        return false;
    }

    string line;
    if (!recv_line(sockfd_, line)) {
        err = "No response";
        return false;
    }

    if (line.rfind("OK 201", 0) == 0) return true;
    err = line;
    return false;
}

bool NetworkClient::get_text(const string &path, string &content, string &err, bool lock_edit) {
    if (sockfd_ < 0) {
        err = "Not connected";
        return false;
    }
    string cmd = "GET_TEXT " + path;
    if (lock_edit) {
        cmd += " LOCK";
    }
    if (!send_line(sockfd_, cmd)) {
        err = "Send error";
        return false;
    }
    string line;
    if (!recv_line(sockfd_, line)) {
        err = "No response";
        return false;
    }
    if (line.rfind("OK 100", 0) != 0) {
        err = line;
        return false;
    }
    vector<string> tokens = split_tokens(line);
    if (tokens.size() < 3) {
        err = "Invalid response: " + line;
        return false;
    }
    uint64_t size = 0;
    if (!parse_u64_safe(tokens[2], size)) {
        err = "Invalid size in response: " + line;
        return false;
    }
    content.clear();
    content.resize(size);
    if (!recv_exact(sockfd_, &content[0], size)) {
        err = "Receive error";
        return false;
    }
    return true;
}

bool NetworkClient::put_text(const string &path, const string &content, string &new_version, string &err) {
    if (sockfd_ < 0) {
        err = "Not connected";
        return false;
    }
    new_version.clear();
    uint64_t size = content.size();
    string cmd = "PUT_TEXT " + path + " " + to_string(size);
    if (!send_line(sockfd_, cmd)) {
        err = "Send error";
        return false;
    }
    string line;
    if (!recv_line(sockfd_, line)) {
        err = "No response";
        return false;
    }
    if (line.rfind("OK 100", 0) != 0) {
        err = line;
        return false;
    }
    if (size > 0 && !send_all(sockfd_, content.data(), content.size())) {
        err = "Send body error";
        return false;
    }
    if (!recv_line(sockfd_, line)) {
        err = "No final response";
        return false;
    }
    if (line.rfind("OK 200", 0) == 0) {
        auto tokens = split_tokens(line);
        for (const auto &tok : tokens) {
            if (tok.rfind("v=", 0) == 0) {
                new_version = tok.substr(2);
            }
        }
        return true;
    }
    err = line;
    return false;
}


bool NetworkClient::upload_file(const string &local_path,
                                const string &remote_path,
                                string &err) 
{
    if (sockfd_ < 0) {
        err = "Not connected";
        return false;
    }
    // Đọc file trên máy người dùng
    std::ifstream ifs(local_path, std::ios::binary);
    if (!ifs) {
        err = "Cannot open local file";
        return false;
    }

    ifs.seekg(0, std::ios::end);
    uint64_t size = ifs.tellg();
    ifs.seekg(0);

    // Gửi lệnh UPLOAD lên server
    string cmd = "UPLOAD " + to_string(size) + " " + remote_path;
    if (!send_line(sockfd_, cmd)) {
        err = "Send error";
        return false;
    }

    string line;
    if (!recv_line(sockfd_, line)) {
        err = "No response";
        return false;
    }

    if (line != "OK 100 Ready to receive") {
        err = line;
        return false;
    }

    // Gửi dữ liệu file
    const size_t BUF = 64 * 1024;
    vector<char> buf(BUF);

    while (true) {
        ifs.read(buf.data(), BUF);
        std::streamsize n = ifs.gcount();
        if (n <= 0) break;

        if (!send_all(sockfd_, buf.data(), (size_t)n)) {
            err = "Send data error";
            return false;
        }
    }

    // OK, xong
    if (!recv_line(sockfd_, line)) {
        err = "No final response";
        return false;
    }

    if (line.rfind("OK 200", 0) != 0) {
        err = line;
        return false;
    }

    return true;
}

bool NetworkClient::download_file(const string &remote_path,
                                  const string &local_path,
                                  string &err) {
    if (sockfd_ < 0) {
        err = "Not connected";
        return false;
    }

    string cmd = "DOWNLOAD " + remote_path;
    if (!send_line(sockfd_, cmd)) {
        err = "Send error";
        return false;
    }

    string line;
    if (!recv_line(sockfd_, line)) {
        err = "No response";
        return false;
    }
    auto tok = split_tokens(line);
    if (tok.size() < 3 || tok[0] != "OK" || tok[1] != "100") {
        err = line;
        return false;
    }
    uint64_t size = 0;
    if (!parse_u64_safe(tok[2], size)) { err = string("Invalid size in response: ") + line; return false; }

    ofstream ofs(local_path, ios::binary);
    if (!ofs) {
        err = "Cannot open local path";
        return false;
    }

    const size_t BUF = 64 * 1024;
    vector<char> buf(BUF);
    uint64_t remaining = size;
    while (remaining > 0) {
        size_t chunk = remaining > BUF ? BUF : (size_t)remaining;
        if (!recv_exact(sockfd_, buf.data(), chunk)) {
            err = "Receive data error";
            return false;
        }
        ofs.write(buf.data(), (streamsize)chunk);
        if (!ofs) {
            err = "Write local file error";
            return false;
        }
        remaining -= chunk;
    }
    return true;
}

bool NetworkClient::download_file_with_progress(
    const string &remote_path,
    const string &local_path,
    std::atomic<uint64_t> &received,
    uint64_t &total,
    string &err
) {
    if (sockfd_ < 0) { err = "Not connected"; return false; }

    string cmd = "DOWNLOAD " + remote_path;
    if (!send_line(sockfd_, cmd)) { err = "Send error"; return false; }

    string line;
    if (!recv_line(sockfd_, line)) { err = "No response"; return false; }
    auto tok = split_tokens(line);
    if (tok.size() < 3 || tok[0] != "OK" || tok[1] != "100") { err = line; return false; }
    uint64_t size = 0;
    if (!parse_u64_safe(tok[2], size)) { err = string("Invalid size in response: ") + line; return false; }
    total = size;
    received = 0;

    ofstream ofs(local_path, ios::binary);
    if (!ofs) { err = "Cannot open local path"; return false; }

    const size_t BUF = 64 * 1024;
    vector<char> buf(BUF);
    uint64_t remaining = size;
    while (remaining > 0) {
        size_t chunk = remaining > BUF ? BUF : (size_t)remaining;
        if (!recv_exact(sockfd_, buf.data(), chunk)) {
            err = "Receive data error";
            return false;
        }
        ofs.write(buf.data(), (streamsize)chunk);
        if (!ofs) { err = "Write local file error"; return false; }
        remaining -= chunk;
        received += chunk;
    }
    return true;
}

bool NetworkClient::continue_download_with_progress(const string &remote_path, const string &local_path,
                                                    std::atomic<uint64_t> &received, uint64_t &total, string &err) {
    if (sockfd_ < 0) { err = "Not connected"; return false; }
    string cmd = "CONTINUE_DOWNLOAD " + remote_path;
    if (!send_line(sockfd_, cmd)) { err = "Send error"; return false; }
    string line;
    if (!recv_line(sockfd_, line)) { err = "No response"; return false; }
    auto tok = split_tokens(line);
    if (tok.size() < 6 || tok[0] != "OK") { err = line; return false; }
    uint64_t offset = 0, remaining = 0;
    if (!parse_u64_safe(tok[3], offset) || !parse_u64_safe(tok[5], remaining)) { err = string("Invalid offset/remaining in response: ") + line; return false; }

    ofstream ofs(local_path, ios::binary | ios::app);
    if (!ofs) { err = "Cannot open local file"; return false; }

    received = offset;
    total = offset + remaining;

    const size_t BUF = 64 * 1024;
    vector<char> buf(BUF);
    uint64_t recvd = 0;
    while (recvd < remaining) {
        size_t chunk = (remaining - recvd) > BUF ? BUF : (size_t)(remaining - recvd);
        if (!recv_exact(sockfd_, buf.data(), chunk)) { err = "Receive data error"; return false; }
        ofs.write(buf.data(), (streamsize)chunk);
        if (!ofs) { err = "Write local file error"; return false; }
        recvd += chunk;
        received += chunk;
    }
    return true;
}

bool NetworkClient::pause_upload(const string &remote_path, string &err) {
    if (sockfd_ < 0) { err = "Not connected"; return false; }

    string cmd = "PAUSE_UPLOAD " + remote_path;
    if (!send_line(sockfd_, cmd)) { err = "Send error"; return false; }

    string line;
    if (!recv_line(sockfd_, line)) { err = "No response"; return false; }

    if (line.rfind("OK 200", 0) == 0) return true;
    err = line;
    return false;
}


bool NetworkClient::continue_upload(const string &remote_path, const string &local_path, string &err) {
    if (sockfd_ < 0) { err = "Not connected"; return false; }
    // include local total size so server can resume even if DB session missing
    ifstream ifs(local_path, ios::binary | ios::ate);
    uint64_t local_size = 0;
    if (ifs) local_size = (uint64_t)ifs.tellg();
    string cmd = "CONTINUE_UPLOAD " + remote_path + " " + to_string(local_size);
    if (!send_line(sockfd_, cmd)) { err = "Send error"; return false; }
    string line;
    if (!recv_line(sockfd_, line)) { err = "No response"; return false; }
    auto tok = split_tokens(line);
    // Expect: OK 100 Continue from <offset> size <remaining>
    if (tok.size() < 6 || tok[0] != "OK") { err = line; return false; }
    uint64_t offset = 0, remaining = 0;
    if (!parse_u64_safe(tok[3], offset) || !parse_u64_safe(tok[5], remaining)) { err = string("Invalid offset/remaining in response: ") + line; return false; }

    ifstream ifs2(local_path, ios::binary);
    if (!ifs2) { err = "Cannot open local file"; return false; }
    ifs2.seekg((streamsize)offset);

    const size_t BUF = 64 * 1024;
    vector<char> buf(BUF);
    uint64_t sent = 0;
    while (sent < remaining) {
        size_t chunk = (remaining - sent) > BUF ? BUF : (size_t)(remaining - sent);
        ifs2.read(buf.data(), (streamsize)chunk);
        streamsize got = ifs2.gcount();
        if (got <= 0) break;
        if (!send_all(sockfd_, buf.data(), (size_t)got)) { err = "Send data error"; return false; }
        sent += (uint64_t)got;
    }

    if (!recv_line(sockfd_, line)) { err = "No final response"; return false; }
    if (line.rfind("OK 200", 0) == 0) return true;
    err = line; return false;
}

bool NetworkClient::continue_upload_with_progress(const string &remote_path, const string &local_path,
                                                 std::atomic<uint64_t> &sent, uint64_t &total, string &err) {
    if (sockfd_ < 0) { err = "Not connected"; return false; }
    // include local file total so server can resume even if DB session missing
    ifstream ifs_probe(local_path, ios::binary | ios::ate);
    uint64_t local_total = 0;
    if (ifs_probe) local_total = (uint64_t)ifs_probe.tellg();
    string cmd = "CONTINUE_UPLOAD " + remote_path + " " + to_string(local_total);
    if (!send_line(sockfd_, cmd)) { err = "Send error"; return false; }
    string line;
    if (!recv_line(sockfd_, line)) { err = "No response"; return false; }
    auto tok = split_tokens(line);
    if (tok.size() < 6 || tok[0] != "OK") { err = line; return false; }
    uint64_t offset = 0, remaining = 0;
    if (!parse_u64_safe(tok[3], offset) || !parse_u64_safe(tok[5], remaining)) { err = string("Invalid offset/remaining in response: ") + line; return false; }

    ifstream ifs(local_path, ios::binary);
    if (!ifs) { err = "Cannot open local file"; return false; }
    ifs.seekg((streamsize)offset);

    sent = offset;
    total = offset + remaining;

    const size_t BUF = 64 * 1024;
    vector<char> buf(BUF);
    uint64_t sent_here = 0;
    while (sent_here < remaining) {
        size_t chunk = (remaining - sent_here) > BUF ? BUF : (size_t)(remaining - sent_here);
        ifs.read(buf.data(), (streamsize)chunk);
        streamsize got = ifs.gcount();
        if (got <= 0) break;
        if (!send_all(sockfd_, buf.data(), (size_t)got)) { err = "Send data error"; return false; }
        sent_here += (uint64_t)got;
        sent += (uint64_t)got;
    }

    if (!recv_line(sockfd_, line)) { err = "No final response"; return false; }
    if (line.rfind("OK 200", 0) == 0) return true;
    err = line; return false;
}

bool NetworkClient::resume_upload_stream(const string &remote_path, const string &local_path,
                                        uint64_t offset, std::atomic<uint64_t> &sent, uint64_t total, string &err) {
    if (sockfd_ < 0) { err = "Not connected"; return false; }

    std::ifstream ifs(local_path, std::ios::binary);
    if (!ifs) { err = "Cannot open local file"; return false; }
    uint64_t local_size = 0;
    try {
        ifs.seekg(0, std::ios::end);
        local_size = (uint64_t)ifs.tellg();
    } catch (...) {}
    if (offset > local_size) { err = "Local file smaller than offset"; return false; }

    ifs.clear();
    ifs.seekg((std::streamoff)offset);

    const size_t BUF = 64 * 1024;
    std::vector<char> buf(BUF);
    uint64_t remaining = total > offset ? total - offset : 0;
    uint64_t sent_here = 0;

    while (sent_here < remaining) {
        size_t chunk = (remaining - sent_here > BUF) ? BUF : (size_t)(remaining - sent_here);
        ifs.read(buf.data(), (std::streamsize)chunk);
        std::streamsize got = ifs.gcount();
        if (got <= 0) break;
        if (!send_all(sockfd_, buf.data(), (size_t)got)) { err = "Send data error"; return false; }
        sent_here += (uint64_t)got;
        sent += (uint64_t)got;
    }

    // Read final response from server
    string line;
    if (!recv_line(sockfd_, line)) { err = "No final response"; return false; }
    if (line.rfind("OK 200", 0) == 0) return true;
    err = line; return false;
}

bool NetworkClient::pause_download(const string &remote_path, uint64_t offset, string &err) {
    if (sockfd_ < 0) { err = "Not connected"; return false; }
    string cmd = "PAUSE_DOWNLOAD " + remote_path + " " + to_string(offset);
    if (!send_line(sockfd_, cmd)) { err = "Send error"; return false; }
    string line;
    if (!recv_line(sockfd_, line)) { err = "No response"; return false; }
    if (line.rfind("OK 200", 0) == 0) return true;
    err = line; return false;
}

bool NetworkClient::continue_download(const string &remote_path, const string &local_path, string &err) {
    if (sockfd_ < 0) { err = "Not connected"; return false; }
    string cmd = "CONTINUE_DOWNLOAD " + remote_path;
    if (!send_line(sockfd_, cmd)) { err = "Send error"; return false; }
    string line;
    if (!recv_line(sockfd_, line)) { err = "No response"; return false; }
    auto tok = split_tokens(line);
    if (tok.size() < 6 || tok[0] != "OK") { err = line; return false; }
    uint64_t offset = 0, remaining = 0;
    if (!parse_u64_safe(tok[3], offset) || !parse_u64_safe(tok[5], remaining)) { err = string("Invalid offset/remaining in response: ") + line; return false; }

    ofstream ofs(local_path, ios::binary | ios::app);
    if (!ofs) { err = "Cannot open local file"; return false; }

    const size_t BUF = 64 * 1024;
    vector<char> buf(BUF);
    uint64_t received = 0;
    while (received < remaining) {
        size_t chunk = (remaining - received) > BUF ? BUF : (size_t)(remaining - received);
        if (!recv_exact(sockfd_, buf.data(), chunk)) { err = "Receive data error"; return false; }
        ofs.write(buf.data(), (streamsize)chunk);
        if (!ofs) { err = "Write local file error"; return false; }
        received += chunk;
    }
    return true;
}

bool NetworkClient::unzip_remote(const string &zip_path, const string &target_dir, string &err) {
    if (sockfd_ < 0) { err = "Not connected"; return false; }
    string cmd = "UNZIP " + zip_path;
    if (!target_dir.empty()) cmd += " " + target_dir;
    if (!send_line(sockfd_, cmd)) { err = "Send error"; return false; }
    string line;
    if (!recv_line(sockfd_, line)) { err = "No response"; return false; }
    if (line.rfind("OK 200", 0) == 0) return true;
    err = line; return false;
}

bool NetworkClient::create_remote_folder(const string &remote_path, string &err) {
    if (sockfd_ < 0) { err = "Not connected"; return false; }
    string cmd = "CREATE_FOLDER " + remote_path;
    if (!send_line(sockfd_, cmd)) { err = "Send error"; return false; }
    string line;
    if (!recv_line(sockfd_, line)) { err = "No response"; return false; }
    if (line.rfind("OK 200", 0) == 0) return true;
    err = line;
    return false;
}

bool NetworkClient::rename_remote(const string &old_path, const string &new_path, string &err) {
    if (sockfd_ < 0) { err = "Not connected"; return false; }
    string cmd = "RENAME " + old_path + " " + new_path;
    if (!send_line(sockfd_, cmd)) { err = "Send error"; return false; }
    string line;
    if (!recv_line(sockfd_, line)) { err = "No response"; return false; }
    if (line.rfind("OK 200", 0) == 0) return true;
    err = line; return false;
}

bool NetworkClient::move_remote(const string &old_path, const string &new_path, string &err) {
    if (sockfd_ < 0) { err = "Not connected"; return false; }
    string cmd = "MOVE " + old_path + " " + new_path;
    if (!send_line(sockfd_, cmd)) { err = "Send error"; return false; }
    string line;
    if (!recv_line(sockfd_, line)) { err = "No response"; return false; }
    if (line.rfind("OK 200", 0) == 0) return true;
    err = line; return false;
}

bool NetworkClient::delete_remote(const string &path, string &err) {
    if (sockfd_ < 0) { err = "Not connected"; return false; }
    string cmd = "DELETE " + path;
    if (!send_line(sockfd_, cmd)) { err = "Send error"; return false; }
    string line;
    if (!recv_line(sockfd_, line)) { err = "No response"; return false; }
    if (line.rfind("OK 200", 0) == 0) return true;
    err = line; return false;
}

bool NetworkClient::restore_remote(const string &path, string &err) {
    if (sockfd_ < 0) { err = "Not connected"; return false; }
    string cmd = "RESTORE " + path;
    if (!send_line(sockfd_, cmd)) { err = "Send error"; return false; }
    string line;
    if (!recv_line(sockfd_, line)) { err = "No response"; return false; }
    if (line.rfind("OK 200", 0) == 0) return true;
    err = line; return false;
}

bool NetworkClient::list_deleted(string &rows, string &err) {
    rows.clear();
    if (sockfd_ < 0) { err = "Not connected"; return false; }
    if (!send_line(sockfd_, "LIST_DELETED")) { err = "Send error"; return false; }
    string line;
    if (!recv_line(sockfd_, line)) { err = "No response"; return false; }
    auto tok = split_tokens(line);
    if (tok.size() < 3 || tok[0] != "OK" || tok[1] != "200") { err = line; return false; }
    int count = stoi(tok[2]);
    for (int i = 0; i < count; ++i) {
        if (!recv_line(sockfd_, line)) { err = "Receive error"; return false; }
        rows += line + "\n";
    }
    return true;
}

bool NetworkClient::set_permission(const string &path, const string &target_user, bool can_view, bool can_download, bool can_edit, string &err) {
    if (sockfd_ < 0) { err = "Not connected"; return false; }
    string cmd = "SET_PERMISSION " + path + " " + target_user + " " +
                 (can_view ? "1" : "0") + " " +
                 (can_download ? "1" : "0") + " " +
                 (can_edit ? "1" : "0");
    if (!send_line(sockfd_, cmd)) { err = "Send error"; return false; }
    string line;
    if (!recv_line(sockfd_, line)) { err = "No response"; return false; }
    if (line.rfind("OK 200", 0) == 0) return true;
    err = line; return false;
}

bool NetworkClient::list_acl(const string &path, string &rows, string &err) {
    rows.clear();
    if (sockfd_ < 0) { err = "Not connected"; return false; }
    string cmd = "LIST_ACL " + path;
    if (!send_line(sockfd_, cmd)) { err = "Send error"; return false; }
    string line;
    if (!recv_line(sockfd_, line)) { err = "No response"; return false; }
    auto tok = split_tokens(line);
    if (tok.size() < 3 || tok[0] != "OK" || tok[1] != "200") { err = line; return false; }
    int count = stoi(tok[2]);
    for (int i = 0; i < count; ++i) {
        if (!recv_line(sockfd_, line)) { err = "Receive error"; return false; }
        rows += line + "\n";
    }
    return true;
}

bool NetworkClient::list_files_db(string &paths, string &err) {
    paths.clear();

    if (!send_line(sockfd_, "LIST_DB")) {
        err = "Send error";
        return false;
    }

    string line;
    if (!recv_line(sockfd_, line)) {
        err = "No response";
        return false;
    }

    auto tok = split_tokens(line);
    if (tok.size() < 3 || tok[0] != "OK" || tok[1] != "200") {
        err = line;
        return false;
    }

    int count = stoi(tok[2]);
    uint64_t bytes_expected = 0;

    // vì mỗi dòng kết thúc bằng '\n', total byte chính là tổng chiều dài paths
    // ta đọc liên tục đến khi nhận được count dòng
    for (int i = 0; i < count; i++) {
        if (!recv_line(sockfd_, line)) {
            err = "Receive error";
            return false;
        }
        paths += line + "\n";
    }

    return true;
}

bool NetworkClient::ping(string &err) {
    if (sockfd_ < 0) { err = "Not connected"; return false; }
    if (!send_line(sockfd_, "PING")) { err = "Send error"; return false; }
    string line;
    if (!recv_line(sockfd_, line)) { err = "No response"; return false; }
    if (line.rfind("OK 200", 0) == 0) return true;
    err = line; return false;
}

bool NetworkClient::send_raw_command(const string &cmd, string &out, string &err) {
    if (sockfd_ < 0) {
        err = "Not connected";
        return false;
    }

    // Gửi lệnh
    if (!send_line(sockfd_, cmd)) {
        err = "Send error";
        return false;
    }

    // Nhận 1 dòng phản hồi
    if (!recv_line(sockfd_, out)) {
        err = "No response";
        return false;
    }

    return true;
}

bool NetworkClient::upload_file_with_progress(
    const string &local_path,
    const string &remote_path,
    std::atomic<uint64_t> &sent,
    string &err
) {
    if (sockfd_ < 0) {
        err = "Not connected";
        return false;
    }

    ifstream ifs(local_path, ios::binary);
    if (!ifs) {
        err = "Cannot open local file";
        return false;
    }

    ifs.seekg(0, ios::end);
    uint64_t size = ifs.tellg();
    ifs.seekg(0);

    string cmd = "UPLOAD " + to_string(size) + " " + remote_path;
    if (!send_line(sockfd_, cmd)) {
        err = "Send error";
        return false;
    }

    string line;
    if (!recv_line(sockfd_, line) || line != "OK 100 Ready to receive") {
        err = line;
        return false;
    }

    const size_t BUF = 64 * 1024;
    vector<char> buf(BUF);

    while (ifs) {
        ifs.read(buf.data(), BUF);
        streamsize n = ifs.gcount();
        if (n <= 0) break;

        if (!send_all(sockfd_, buf.data(), (size_t)n)) {
            err = "Send data error";
            return false;
        }

        sent += (uint64_t)n;
    }

    if (!recv_line(sockfd_, line)) {
        err = "No final response";
        return false;
    }

    if (line.rfind("OK 200", 0) != 0) {
        err = line;
        return false;
    }

    return true;
}
