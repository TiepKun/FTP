// ===== file: common/Utils.hpp =====
#pragma once
#include <string>
#include <vector>
#include <cstdint>

using namespace std;

namespace utils {

// Nối 2 path, đảm bảo chỉ có 1 dấu '/'
string join_path(const string &a, const string &b);

// Tạo thư mục kiểu "mkdir -p" (tạo từng cấp)
// Trả về true nếu tồn tại hoặc tạo thành công.
bool ensure_dir(const string &path);

// File có tồn tại không
bool file_exists(const string &path);

// Kích thước file (bytes), 0 nếu không tồn tại / lỗi
uint64_t file_size(const string &path);

// Tách path thành các thành phần (theo '/'), bỏ empty component
vector<string> split_path(const string &path);

} // namespace utils
