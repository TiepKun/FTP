#pragma once
#include <atomic>
#include <cstdint>

struct UploadState {
    std::atomic<bool> paused{false};
    std::atomic<uint64_t> offset{0};
};

struct DownloadState {
    std::atomic<bool> paused{false};
    std::atomic<uint64_t> offset{0};
};
