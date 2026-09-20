#pragma once
#include "common.hpp"

namespace nvs30 {
class FramePacer {
public:
    void configure(HWND hwnd);
    void wait();
    float target_fps() const noexcept { return target_fps_; }

private:
    LARGE_INTEGER frequency_{};
    std::int64_t next_tick_{};
    std::int64_t interval_{};
    float target_fps_{};
};
}

