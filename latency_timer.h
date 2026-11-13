#pragma once
#include <chrono>
#include <cstdint>

struct LatencyTimer {
    static std::int64_t now_us() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
};
