#pragma once
#include "common.hpp"

namespace nvs30 {
class Osd {
public:
    void on_frame(HWND hwnd, bool smooth_active, bool bridge_active);

private:
    bool visible_{true};
    bool key_down_{};
    std::uint64_t frames_{};
    float fps_{};
    std::chrono::steady_clock::time_point sample_start_{std::chrono::steady_clock::now()};
};
}

