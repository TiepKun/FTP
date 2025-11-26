#pragma once
#include <string>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>

using namespace std;

namespace proto {

// Đọc 1 dòng kết thúc bằng '\n'
bool recv_line(int sockfd, string &line);

// Gửi đủ len bytes
bool send_all(int sockfd, const void *buf, size_t len);

// Nhận chính xác len bytes
bool recv_exact(int sockfd, void *buf, size_t len);

// Gửi 1 dòng text có '\n'
bool send_line(int sockfd, const string &line);

// Tách token theo space/tab
vector<string> split_tokens(const string &s);

} // namespace proto
