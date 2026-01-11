// ===== file: client/NetworkClient.cpp =====
#include "NetworkClient.hpp"
#include "../common/Protocol.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fstream>
#include <vector>

using namespace std;
using namespace proto;

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

    if (line.rfind("OK", 0) == 0) return true;
    err = line;    return false;
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

bool NetworkClient::get_text(const string &path, string &content, string &err) {
    if (sockfd_ < 0) {
        err = "Not connected";
        return false;
    }

    string cmd = "GET_TEXT " + path;
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

    uint64_t size = stoull(tokens[2]);
    content.clear();
    content.resize(size);

    if (!recv_exact(sockfd_, &content[0], size)) {
        err = "Receive error";
        return false;
    }

    return true;
}

bool NetworkClient::put_text(const string &path, const string &content, string &err) {
    if (sockfd_ < 0) {
        err = "Not connected";
        return false;
    }

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

    if (!send_all(sockfd_, content.data(), content.size())) {
        err = "Send body error";
        return false;
    }

    if (!recv_line(sockfd_, line)) {
        err = "No final response";
        return false;
    }

    if (line.rfind("OK 200", 0) == 0) return true;
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
