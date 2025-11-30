// ===== file: common/Protocol.cpp =====
#include "Protocol.hpp"

using namespace std;

namespace proto {

bool recv_line(int sockfd, string &line) {
    line.clear();
    char c;
    while (true) {
        ssize_t n = ::recv(sockfd, &c, 1, 0);
        if (n <= 0) return false;      // lỗi hoặc đóng kết nối
        if (c == '\n') break;
        if (c != '\r') line.push_back(c);
    }
    return true;
}

bool send_all(int sockfd, const void *buf, size_t len) {
    const char *p = static_cast<const char*>(buf);
    size_t total = 0;
    while (total < len) {
        ssize_t n = ::send(sockfd, p + total, len - total, 0);
        if (n <= 0) return false;
        total += (size_t)n;
    }
    return true;
}

bool recv_exact(int sockfd, void *buf, size_t len) {
    char *p = static_cast<char*>(buf);
    size_t total = 0;
    while (total < len) {
        ssize_t n = ::recv(sockfd, p + total, len - total, 0);
        if (n <= 0) return false;
        total += (size_t)n;
    }
    return true;
}

bool send_line(int sockfd, const string &line) {
    string tmp = line;
    if (tmp.empty() || tmp.back() != '\n') tmp.push_back('\n');
    return send_all(sockfd, tmp.data(), tmp.size());
}

vector<string> split_tokens(const string &s) {
    vector<string> tokens;
    string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) {
                tokens.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

} // namespace proto
