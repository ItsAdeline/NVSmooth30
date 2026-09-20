#include "nvs30/dxgi_hooks.hpp"

#include "nvs30/config.hpp"
#include "nvs30/d3d11_bridge.hpp"
#include "nvs30/log.hpp"
#include "nvs30/nvpresent.hpp"
#include "nvs30/osd.hpp"
#include "nvs30/pacer.hpp"
#include "nvs30/pe.hpp"

namespace nvs30::dxgi {
namespace {
using PresentFn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT,
                                                const DXGI_PRESENT_PARAMETERS*);

PresentFn g_present{};
Present1Fn g_present1{};
std::atomic<IDXGISwapChain*> g_primary{};
std::atomic_bool g_smooth_active{};
thread_local bool g_internal_creation{};
std::once_flag g_pacer_once;
D3D11Bridge g_bridge;
FramePacer g_pacer;
Osd g_osd;

HRESULT STDMETHODCALLTYPE hook_present(IDXGISwapChain*, UINT, UINT);
HRESULT STDMETHODCALLTYPE hook_present1(IDXGISwapChain1*, UINT, UINT,
                                        const DXGI_PRESENT_PARAMETERS*);

template <class T>
bool patch_vtable(void* object, std::size_t slot, void* hook, T& original) {
    if (!object) return false;
    auto** table = *reinterpret_cast<void***>(object);
    if (!table || table[slot] == hook) return true;
    if (!original) original = reinterpret_cast<T>(table[slot]);
    return pe::write_memory(&table[slot], &hook, sizeof(hook));
}

bool wrapper_detected(IDXGISwapChain* swapchain) {
    return nvpresent::wrapper_active(swapchain);
}

HWND swapchain_hwnd(IDXGISwapChain* swapchain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    return swapchain && SUCCEEDED(swapchain->GetDesc(&desc)) ? desc.OutputWindow : nullptr;
}

void select_primary(IDXGISwapChain* swapchain) {
    if (!swapchain || g_primary.load()) return;
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapchain->GetDesc(&desc)) || desc.BufferDesc.Width < 854 ||
        desc.BufferDesc.Height < 480) return;
    IDXGISwapChain* expected{};
    if (!g_primary.compare_exchange_strong(expected, swapchain)) return;
    if (config().low_latency) {
        ComPtr<IDXGISwapChain2> swap2;
        if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&swap2))))
            swap2->SetMaximumFrameLatency(1);
    }
    std::call_once(g_pacer_once, [&] { g_pacer.configure(desc.OutputWindow); });
    auto** table = *reinterpret_cast<void***>(swapchain);
    const char* api = "unknown";
    ComPtr<ID3D11Device> as11;
    ComPtr<ID3D12Device> as12;
    if (SUCCEEDED(swapchain->GetDevice(IID_PPV_ARGS(&as11))) && as11) api = "D3D11";
    else if (SUCCEEDED(swapchain->GetDevice(IID_PPV_ARGS(&as12))) && as12) api = "D3D12";
    UINT sc_flags = 0;
    ComPtr<IDXGISwapChain1> swap1;
    if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&swap1))) && swap1) {
        DXGI_SWAP_CHAIN_DESC1 desc1{};
        if (SUCCEEDED(swap1->GetDesc1(&desc1))) sc_flags = desc1.Flags;
    }
    logf("[nvs30] primary swapchain=%p hwnd=%p %ux%u api=%s fmt=%u buffers=%u effect=%u flags=0x%x slot0=%p slot7=%p\n",
         swapchain, desc.OutputWindow, desc.BufferDesc.Width, desc.BufferDesc.Height, api,
         unsigned(desc.BufferDesc.Format), desc.BufferCount, unsigned(desc.SwapEffect), sc_flags,
         table ? table[0] : nullptr, table ? table[7] : nullptr);
}

void hook_swapchain(IDXGISwapChain* swapchain) {
    if (!swapchain || g_internal_creation) return;
    patch_vtable(swapchain, 8, reinterpret_cast<void*>(&hook_present), g_present);
    ComPtr<IDXGISwapChain1> swap1;
    if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&swap1))))
        patch_vtable(swap1.Get(), 22, reinterpret_cast<void*>(&hook_present1), g_present1);
    select_primary(swapchain);
}

HRESULT dispatch_present(IDXGISwapChain* swapchain, UINT sync_interval, UINT flags,
                         const DXGI_PRESENT_PARAMETERS* parameters) {
    select_primary(swapchain);
    if (config().force_vsync && !(flags & DXGI_PRESENT_TEST)) sync_interval = 1;

    HRESULT hr{};
    const bool primary = g_primary.load() == swapchain;
    bool bridged{};
    if (primary && config().enable_d3d11_bridge && !(flags & DXGI_PRESENT_TEST)) {
        ComPtr<ID3D11Device> d3d11;
        if (SUCCEEDED(swapchain->GetDevice(IID_PPV_ARGS(&d3d11)))) {
            hr = g_bridge.present(swapchain, swapchain_hwnd(swapchain), sync_interval, flags);
            bridged = SUCCEEDED(hr);
        }
    }
    if (!bridged) {
        if (parameters && g_present1)
            hr = g_present1(reinterpret_cast<IDXGISwapChain1*>(swapchain), sync_interval, flags, parameters);
        else if (g_present)
            hr = g_present(swapchain, sync_interval, flags);
        else
            return DXGI_ERROR_INVALID_CALL;
    }

    if (primary && !(flags & DXGI_PRESENT_TEST)) {
        const bool active = bridged ? g_bridge.wrapper_active() : wrapper_detected(swapchain);
        if (active && !g_smooth_active.exchange(true))
            logf("[nvs30] Smooth Motion activated on PRIMARY wrapper.\n");
        g_osd.on_frame(swapchain_hwnd(swapchain), g_smooth_active.load(), bridged);
        g_pacer.wait();
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_present(IDXGISwapChain* swapchain, UINT sync, UINT flags) {
    return dispatch_present(swapchain, sync, flags, nullptr);
}

HRESULT STDMETHODCALLTYPE hook_present1(IDXGISwapChain1* swapchain, UINT sync, UINT flags,
                                        const DXGI_PRESENT_PARAMETERS* parameters) {
    return dispatch_present(swapchain, sync, flags, parameters);
}

LRESULT CALLBACK dummy_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
}

bool install_hooks() {
    // Patch the shared DXGI Present vtable through a tiny native D3D11
    // swapchain. Do not replace factory creation slots: NvPresent owns those
    // hooks and needs them intact to wrap the bridge's D3D12 shadow swapchain.
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    constexpr wchar_t class_name[] = L"NVSmooth30DummyWindow";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = dummy_window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name;
    const ATOM atom = RegisterClassW(&window_class);
    if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        logf("[nvs30] dummy window registration failed: %lu.\n", GetLastError());
        return false;
    }
    const HWND window = CreateWindowExW(0, class_name, L"", WS_OVERLAPPED,
                                         0, 0, 16, 16, nullptr, nullptr, instance, nullptr);
    if (!window) {
        logf("[nvs30] dummy window creation failed: %lu.\n", GetLastError());
        if (atom) UnregisterClassW(class_name, instance);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.Width = 16;
    desc.BufferDesc.Height = 16;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc = {1, 0};
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.OutputWindow = window;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapchain;
    const HRESULT create_hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &desc, &swapchain, &device, nullptr, &context);
    bool ok = false;
    if (SUCCEEDED(create_hr)) {
        hook_swapchain(swapchain.Get());
        ok = g_present != nullptr;
    } else {
        logf("[nvs30] dummy D3D11 swapchain creation failed: 0x%08X.\n",
             static_cast<unsigned>(create_hr));
    }
    swapchain.Reset();
    context.Reset();
    device.Reset();
    DestroyWindow(window);
    if (atom) UnregisterClassW(class_name, instance);
    nvpresent::note_present_trampoline(reinterpret_cast<void*>(g_present),
                                       reinterpret_cast<void*>(g_present1));
    logf("[nvs30] DXGI Present hooks %s (slots 8 and 22).\n",
         ok ? "installed" : "failed");
    if (ok)
        logf("[nvs30] Present trampoline: present=%p(in_nvp=%d) present1=%p(in_nvp=%d) nvp=%p.\n",
             reinterpret_cast<void*>(g_present),
             g_present && nvpresent::contains_address(reinterpret_cast<const void*>(g_present)) ? 1 : 0,
             reinterpret_cast<void*>(g_present1),
             g_present1 && nvpresent::contains_address(reinterpret_cast<const void*>(g_present1)) ? 1 : 0,
             nvpresent::module());
    return ok;
}

void shutdown() { g_bridge.reset(); }
void set_internal_creation(bool value) { g_internal_creation = value; }
bool internal_creation() { return g_internal_creation; }
}
