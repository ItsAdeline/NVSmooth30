#include "nvs30/pacer.hpp"
#include "nvs30/config.hpp"
#include "nvs30/log.hpp"

namespace nvs30 {
void FramePacer::configure(HWND hwnd) {
    QueryPerformanceFrequency(&frequency_);
    target_fps_ = config().base_fps_cap;
    if (config().half_refresh_cap && hwnd) {
        const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (GetMonitorInfoW(monitor, &info) &&
            EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
            mode.dmDisplayFrequency > 1) {
            target_fps_ = mode.dmDisplayFrequency * 0.5f;
        }
    }
    if (target_fps_ > 1.0f) {
        interval_ = static_cast<std::int64_t>(frequency_.QuadPart / target_fps_);
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        next_tick_ = now.QuadPart + interval_;
        logf("[nvs30] base-frame pacer enabled at %.3f FPS.\n", target_fps_);
    }
}

void FramePacer::wait() {
    if (!interval_) return;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (next_tick_ <= now.QuadPart) {
        next_tick_ = now.QuadPart + interval_;
        return;
    }
    const auto remaining_ticks = next_tick_ - now.QuadPart;
    const double remaining_ms = 1000.0 * remaining_ticks / frequency_.QuadPart;
    if (remaining_ms > 1.5) Sleep(static_cast<DWORD>(remaining_ms - 0.7));
    do {
        SwitchToThread();
        QueryPerformanceCounter(&now);
    } while (now.QuadPart < next_tick_);
    next_tick_ += interval_;
}
}

