// ===== file: common/Utils.cpp =====
#include "Utils.hpp"
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

using namespace std;

namespace utils {

string join_path(const string &a, const string &b) {
    if (a.empty()) return b;
    if (b.empty()) return a;

    bool a_slash_end = (!a.empty() && a.back() == '/');
    bool b_slash_start = (!b.empty() && b.front() == '/');

    if (a_slash_end && b_slash_start) {
        return a + b.substr(1);
    } else if (!a_slash_end && !b_slash_start) {
        return a + "/" + b;
    } else {
        return a + b;
    }
}

static bool mkdir_single(const string &path) {
    if (path.empty()) return true;
    int rc = ::mkdir(path.c_str(), 0755);
    if (rc == 0) return true;
    if (errno == EEXIST) return true;
    return false;
}

vector<string> split_path(const string &path) {
    vector<string> parts;
    string cur;
    for (char c : path) {
        if (c == '/') {
            if (!cur.empty()) {
                parts.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

bool ensure_dir(const string &path) {
    if (path.empty()) return true;

    // Nếu path không bắt đầu bằng '/', coi như relative:
    // build dần từ trước ra sau.
    vector<string> parts = split_path(path);
    string cur;
    if (!path.empty() && path.front() == '/') {
        cur = "/";
    }

    for (size_t i = 0; i < parts.size(); ++i) {
        if (cur == "/" || cur.empty())
            cur += parts[i];
        else
            cur = join_path(cur, parts[i]);

        if (!mkdir_single(cur)) {
            // nếu mkdir lỗi mà không phải EEXIST thì fail
            struct stat st{};
            if (::stat(cur.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
                return false;
            }
        }
    }
    return true;
}

bool file_exists(const string &path) {
    struct stat st{};
    return (::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode));
}

uint64_t file_size(const string &path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
        return (uint64_t)st.st_size;
    }
    return 0;
}

} // namespace utils
