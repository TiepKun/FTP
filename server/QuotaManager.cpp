// ===== file: server/QuotaManager.cpp =====
#include "QuotaManager.hpp"

void QuotaManager::set_limit(const string &user, uint64_t max_bytes) {
    lock_guard<mutex> lock(mtx_);
    quotas_[user].max_bytes = max_bytes;
}

bool QuotaManager::can_allocate(const string &user, uint64_t additional_bytes) {
    lock_guard<mutex> lock(mtx_);
    auto &q = quotas_[user];
    if (q.max_bytes == 0) return true;
    return (q.used_bytes + additional_bytes <= q.max_bytes);
}

void QuotaManager::add_usage(const string &user, uint64_t delta) {
    lock_guard<mutex> lock(mtx_);
    quotas_[user].used_bytes += delta;
}

int64_t QuotaManager::adjust_usage(const string &user, int64_t delta) {
    lock_guard<mutex> lock(mtx_);
    auto &q = quotas_[user];
    int64_t new_used = static_cast<int64_t>(q.used_bytes) + delta;
    if (new_used < 0) new_used = 0;
    q.used_bytes = static_cast<uint64_t>(new_used);
    return new_used;
}

uint64_t QuotaManager::used(const string &user) {
    lock_guard<mutex> lock(mtx_);
    return quotas_[user].used_bytes;
}
