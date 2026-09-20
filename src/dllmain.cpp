#include "nvs30/config.hpp"
#include "nvs30/dxgi_hooks.hpp"
#include "nvs30/log.hpp"
#include "nvs30/nvpresent.hpp"

namespace {
DWORD WINAPI bootstrap(void*) {
    nvs30::load_config();
    nvs30::log_open();
    const auto& c = nvs30::config();
    nvs30::logf("[nvs30] startup %s: OSD=%d D3D11Bridge=%d ForceVSync=%d Diagnostics=%d LowLatency=%d HalfRefreshCap=%d BaseFpsCap=%.3f Linearize=%d\n",
                NVS30_VERSION, c.enable_osd, c.enable_d3d11_bridge, c.force_vsync,
                c.diagnostics, c.low_latency, c.half_refresh_cap, c.base_fps_cap,
                c.bridge_linearize);
    if (!nvs30::nvpresent::initialize()) return 0;
    nvs30::dxgi::install_hooks();
    return 0;
}
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        if (HANDLE thread = CreateThread(nullptr, 0, bootstrap, nullptr, 0, nullptr))
            CloseHandle(thread);
    }
    return TRUE;
}
