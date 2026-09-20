#include "nvs30/osd.hpp"
#include "nvs30/config.hpp"

#include <cstdio>

namespace nvs30 {
void Osd::on_frame(HWND hwnd, bool smooth_active, bool bridge_active) {
    if (!config().enable_osd || !hwnd) return;
    const bool down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    if (down && !key_down_) visible_ = !visible_;
    key_down_ = down;
    if (!visible_) return;

    ++frames_;
    const auto now = std::chrono::steady_clock::now();
    const float elapsed = std::chrono::duration<float>(now - sample_start_).count();
    if (elapsed >= 0.5f) {
        fps_ = frames_ / elapsed;
        frames_ = 0;
        sample_start_ = now;
    }

    char text[256]{};
    snprintf(text, sizeof(text), "NVSmooth30  Base: %.1f FPS  Smooth: %s  Bridge: %s  [F11]",
             fps_, smooth_active ? "ACTIVE" : "waiting", bridge_active ? "D3D11>D3D12" : "native");
    HDC dc = GetDC(hwnd);
    if (!dc) return;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(118, 255, 90));
    TextOutA(dc, 16, 16, text, static_cast<int>(std::strlen(text)));
    ReleaseDC(hwnd, dc);
}
}

