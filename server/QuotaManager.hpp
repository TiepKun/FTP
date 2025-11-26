// ===== file: server/QuotaManager.hpp =====
#pragma once
#include <unordered_map>
#include <mutex>
#include <string>
#include <cstdint>

using namespace std;

struct UserQuota {
    uint64_t used_bytes = 0;
    uint64_t max_bytes  = 0; // 0 = unlimited
};

class QuotaManager {
public:
    void set_limit(const string &user, uint64_t max_bytes);
    bool can_allocate(const string &user, uint64_t additional_bytes);
    void add_usage(const string &user, uint64_t delta);
    uint64_t used(const string &user);

private:
    mutex mtx_;
    unordered_map<string, UserQuota> quotas_;
};
