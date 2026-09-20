#pragma once
#include "common.hpp"

namespace nvs30 {
struct Config {
    bool enable_osd = false;
    bool enable_d3d11_bridge = false;
    bool force_vsync = false;
    bool diagnostics = false;
    bool low_latency = true;
    bool half_refresh_cap = false;
    float base_fps_cap = 0.0f;
    bool bridge_linearize = false;
    std::wstring nvpresent_path;
};

const Config& config();
void load_config();
}

