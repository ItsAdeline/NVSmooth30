#include "nvs30/d3d11_bridge.hpp"

#include "nvs30/config.hpp"
#include "nvs30/dxgi_hooks.hpp"
#include "nvs30/log.hpp"
#include "nvs30/nvpresent.hpp"

namespace nvs30 {
namespace {
D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource,
                                  D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
}

bool readable_range(const void* address, std::size_t size) {
    if (!address || !size) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto region = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    return begin >= region && size <= info.RegionSize &&
           begin - region <= info.RegionSize - size;
}

// The reference bridge enables the private NvPresent wrapper immediately
// after discovering it behind the shadow swapchain.  Validate the two method
// slots before calling them so a false-positive layout probe cannot jump into
// arbitrary memory.  Win64 has a single native calling convention; RCX is
// the wrapper object and the bool is passed as the second argument.
bool activate_nvp_wrapper(void* wrapper) {
    if (!readable_range(wrapper, sizeof(void*))) return false;
    void** table{};
    std::memcpy(&table, wrapper, sizeof(table));
    if (!readable_range(table, 21 * sizeof(void*))) return false;
    if (!table[19] || !table[20] ||
        !nvpresent::contains_address(table[19]) ||
        !nvpresent::contains_address(table[20]))
        return false;

    using ToggleFn = void (*)(void*, bool);
    reinterpret_cast<ToggleFn>(table[19])(wrapper, true);
    reinterpret_cast<ToggleFn>(table[20])(wrapper, true);
    logf("[nvs30-bridge] enabled NvPresent wrapper private toggles (slots 19/20): wrapper=%p.\n",
         wrapper);
    return true;
}
}

D3D11Bridge::~D3D11Bridge() { reset(); }

bool D3D11Bridge::wrapper_active() const noexcept {
    return nvpresent::wrapper_active(shadow_.Get());
}

DXGI_FORMAT D3D11Bridge::linear_format(DXGI_FORMAT format) {
    // Exact mapping recovered from the working reference DLL's format helper.
    // Flip-model DXGI swapchains cannot be created with the *_SRGB variants;
    // the reference therefore maps those two source formats to their linear
    // UNORM equivalents before creating both the shared texture and the D3D12
    // shadow swapchain.  Do this unconditionally: SM86_BRIDGE_LINEARIZE is not
    // allowed to make the DXGI descriptor invalid.
    switch (format) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:  // 10 -> 10
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_UNORM:   // 24 -> 24
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM:      // 28 -> 28
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: // 29 -> 28
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM:      // 87 -> 87
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: // 91 -> 87
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

void D3D11Bridge::reset() {
    if (queue12_ && completion_fence_ && completion_value_)
        wait_for_gpu(completion_value_);
    if (completion_event_) CloseHandle(completion_event_);
    completion_event_ = nullptr;
    shadow_.Reset();
    completion_fence_.Reset();
    copy_query_.Reset();
    shared12_.Reset();
    shared11_.Reset();
    list12_.Reset();
    allocator12_.Reset();
    queue12_.Reset();
    device12_.Reset();
    context11_.Reset();
    device11_.Reset();
    active_ = false;
    wrapper_confirmed_ = false;
    wrapper_retired_ = false;
    frames_presented_ = 0;
    hwnd_ = nullptr;
    width_ = height_ = 0;
    source_format_ = DXGI_FORMAT_UNKNOWN;
    completion_value_ = 0;
}

bool D3D11Bridge::wait_for_gpu(std::uint64_t value) {
    if (!completion_fence_ || completion_fence_->GetCompletedValue() >= value) return true;
    if (FAILED(completion_fence_->SetEventOnCompletion(value, completion_event_))) return false;
    return WaitForSingleObject(completion_event_, INFINITE) == WAIT_OBJECT_0;
}

bool D3D11Bridge::wait_for_d3d11_copy() {
    if (!context11_ || !copy_query_) return false;
    context11_->End(copy_query_.Get());
    context11_->Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    for (;;) {
        const HRESULT hr = context11_->GetData(copy_query_.Get(), nullptr, 0,
                                                D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK) return true;
        if (FAILED(hr) || std::chrono::steady_clock::now() >= deadline) {
            logf("[nvs30-bridge] D3D11 completion query failed/timed out: 0x%08X.\n",
                 static_cast<unsigned>(hr));
            return false;
        }
        SwitchToThread();
    }
}

bool D3D11Bridge::build_shared_resources(IDXGISwapChain* source, DXGI_FORMAT source_format,
                                         UINT /*width*/, UINT /*height*/) {
    ComPtr<ID3D11Texture2D> source_buffer;
    HRESULT hr = source->GetBuffer(0, IID_PPV_ARGS(&source_buffer));
    if (FAILED(hr)) {
        logf("[nvs30-bridge] source GetBuffer failed: 0x%08X.\n", static_cast<unsigned>(hr));
        return false;
    }

    D3D11_TEXTURE2D_DESC texture_desc{};
    source_buffer->GetDesc(&texture_desc);
    texture_desc.Format = linear_format(source_format);
    texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    texture_desc.CPUAccessFlags = 0;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.ArraySize = 1;
    texture_desc.MipLevels = 1;
    texture_desc.SampleDesc = {1, 0};

    // Match the working bridge: prefer a plain NT-handle shared texture.
    // Some D3D11 drivers reject that descriptor, so use the same legacy
    // D3D11_RESOURCE_MISC_SHARED fallback as the reference DLL.  Do not add
    // SHARED_KEYEDMUTEX here; the reference synchronizes the producer with a
    // D3D11_QUERY_EVENT instead.
    bool nt_handle = true;
    texture_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    hr = device11_->CreateTexture2D(&texture_desc, nullptr, &shared11_);
    if (FAILED(hr)) {
        logf("[nvs30-bridge] CreateTexture2D(SHARED_NTHANDLE) failed: 0x%08X; retrying legacy SHARED.\n",
             static_cast<unsigned>(hr));
        shared11_.Reset();
        nt_handle = false;
        texture_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        hr = device11_->CreateTexture2D(&texture_desc, nullptr, &shared11_);
    }
    if (FAILED(hr)) {
        logf("[nvs30-bridge] CreateTexture2D(shared fallback) failed: 0x%08X.\n",
             static_cast<unsigned>(hr));
        return false;
    }

    HANDLE texture_handle{};
    if (nt_handle) {
        ComPtr<IDXGIResource1> resource1;
        hr = shared11_.As(&resource1);
        if (FAILED(hr)) {
            logf("[nvs30-bridge] shared texture IDXGIResource1 failed: 0x%08X.\n",
                 static_cast<unsigned>(hr));
            return false;
        }
        hr = resource1->CreateSharedHandle(nullptr,
                                           DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                           nullptr, &texture_handle);
        if (FAILED(hr)) {
            logf("[nvs30-bridge] CreateSharedHandle(texture) failed: 0x%08X.\n",
                 static_cast<unsigned>(hr));
            return false;
        }
    } else {
        ComPtr<IDXGIResource> resource;
        hr = shared11_.As(&resource);
        if (FAILED(hr)) {
            logf("[nvs30-bridge] legacy shared texture IDXGIResource failed: 0x%08X.\n",
                 static_cast<unsigned>(hr));
            return false;
        }
        hr = resource->GetSharedHandle(&texture_handle);
        if (FAILED(hr) || !texture_handle) {
            logf("[nvs30-bridge] GetSharedHandle(texture) failed: 0x%08X.\n",
                 static_cast<unsigned>(hr));
            return false;
        }
    }

    const HRESULT open_texture =
        device12_->OpenSharedHandle(texture_handle, IID_PPV_ARGS(&shared12_));
    // CreateSharedHandle returns an NT handle owned by us. GetSharedHandle
    // returns a legacy shared handle that must not be passed to CloseHandle.
    if (nt_handle) CloseHandle(texture_handle);
    if (FAILED(open_texture)) {
        logf("[nvs30-bridge] D3D12 OpenSharedHandle(texture) failed: 0x%08X.\n",
             static_cast<unsigned>(open_texture));
        return false;
    }

    D3D11_QUERY_DESC query_desc{};
    query_desc.Query = D3D11_QUERY_EVENT;
    hr = device11_->CreateQuery(&query_desc, &copy_query_);
    if (FAILED(hr)) {
        logf("[nvs30-bridge] CreateQuery(D3D11_QUERY_EVENT) failed: 0x%08X.\n",
             static_cast<unsigned>(hr));
        return false;
    }

    hr = device12_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           IID_PPV_ARGS(&allocator12_));
    if (FAILED(hr)) {
        logf("[nvs30-bridge] CreateCommandAllocator failed: 0x%08X.\n",
             static_cast<unsigned>(hr));
        return false;
    }
    hr = device12_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                      allocator12_.Get(), nullptr, IID_PPV_ARGS(&list12_));
    if (FAILED(hr)) {
        logf("[nvs30-bridge] CreateCommandList failed: 0x%08X.\n", static_cast<unsigned>(hr));
        return false;
    }
    hr = list12_->Close();
    if (FAILED(hr)) {
        logf("[nvs30-bridge] initial command-list Close failed: 0x%08X.\n",
             static_cast<unsigned>(hr));
        return false;
    }
    hr = device12_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&completion_fence_));
    if (FAILED(hr)) {
        logf("[nvs30-bridge] CreateFence(completion) failed: 0x%08X.\n",
             static_cast<unsigned>(hr));
        return false;
    }
    completion_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!completion_event_)
        logf("[nvs30-bridge] CreateEvent(completion) failed: %lu.\n", GetLastError());
    return completion_event_ != nullptr;
}

bool D3D11Bridge::initialize(IDXGISwapChain* source, HWND hwnd) {
    reset();
    DXGI_SWAP_CHAIN_DESC source_desc{};
    if (!source || !hwnd) {
        logf("[nvs30-bridge] initialization skipped: source=%p hwnd=%p.\n", source, hwnd);
        return false;
    }
    HRESULT hr = source->GetDesc(&source_desc);
    if (FAILED(hr)) {
        logf("[nvs30-bridge] source GetDesc failed: 0x%08X.\n", static_cast<unsigned>(hr));
        return false;
    }
    if (source_desc.BufferDesc.Width < 480 || source_desc.BufferDesc.Height < 480) {
        logf("[nvs30-bridge] resolution %ux%u is below the bridge threshold.\n",
             source_desc.BufferDesc.Width, source_desc.BufferDesc.Height);
        return false;
    }
    width_ = source_desc.BufferDesc.Width;
    height_ = source_desc.BufferDesc.Height;
    source_format_ = source_desc.BufferDesc.Format;
    hwnd_ = hwnd;

    const DXGI_FORMAT bridge_format = linear_format(source_format_);
    if (bridge_format == DXGI_FORMAT_UNKNOWN) {
        logf("[nvs30-bridge] format %u is not safe for the shared bridge; native passthrough.\n",
             unsigned(source_format_));
        return false;
    }

    logf("[nvs30-bridge] initializing source=%p hwnd=%p %ux%u format=%u bridge_format=%u.\n",
         source, hwnd, width_, height_, unsigned(source_format_), unsigned(bridge_format));
    hr = source->GetDevice(IID_PPV_ARGS(&device11_));
    if (FAILED(hr)) {
        logf("[nvs30-bridge] source is not D3D11: GetDevice failed 0x%08X.\n",
             static_cast<unsigned>(hr));
        return false;
    }
    device11_->GetImmediateContext(&context11_);
    if (!context11_) return false;

    // Resolve the game's adapter so the D3D12 device lives on the same GPU
    // the game is rendering on. The shadow chain itself is created on a fresh
    // factory (matching the reference bridge); NvPresent observes that path
    // after NVP_Init_D3D without us touching the game's factory instance.
    ComPtr<IDXGIAdapter> game_adapter;
    ComPtr<IDXGIFactory2> game_factory;
    DXGI_ADAPTER_DESC game_adapter_desc{};
    bool have_game_adapter = false;
    if (ComPtr<IDXGIDevice> dxgi_device; SUCCEEDED(device11_.As(&dxgi_device)) && dxgi_device) {
        ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(dxgi_device->GetAdapter(&adapter)) && adapter) {
            game_adapter = adapter;
            if (SUCCEEDED(adapter->GetDesc(&game_adapter_desc))) have_game_adapter = true;
            ComPtr<IDXGIFactory> factory0;
            if (SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory0))) && factory0)
                factory0.As(&game_factory);
        }
    }
    logf("[nvs30-bridge] game adapter=%p factory=%p desc='%ls' vid=0x%X dev=0x%X luid=%08X:%08X.\n",
         game_adapter.Get(), game_factory.Get(),
         have_game_adapter ? game_adapter_desc.Description : L"<unknown>",
         have_game_adapter ? game_adapter_desc.VendorId : 0,
         have_game_adapter ? game_adapter_desc.DeviceId : 0,
         have_game_adapter ? static_cast<unsigned>(game_adapter_desc.AdapterLuid.HighPart) : 0,
         have_game_adapter ? static_cast<unsigned>(game_adapter_desc.AdapterLuid.LowPart) : 0);

    // Exact reference order: D3D12CreateDevice is called with a null adapter.
    // On the target hybrid-laptop workload Windows/NVIDIA GPU preference picks
    // the same RTX adapter as the game. Explicitly passing the D3D11 adapter
    // changes the device-creation path NvPresent observes and was the last
    // material difference in the shadow-chain bootstrap.
    const auto cuda_before_device = nvpresent::cuda_intercept_count();
    hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12_));
    if (FAILED(hr) && game_adapter) {
        logf("[nvs30-bridge] D3D12CreateDevice(default) failed: 0x%08X; retrying game adapter.\n",
             static_cast<unsigned>(hr));
        hr = D3D12CreateDevice(game_adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12_));
    }
    if (FAILED(hr)) {
        logf("[nvs30-bridge] D3D12CreateDevice failed: 0x%08X.\n", static_cast<unsigned>(hr));
        return false;
    }
    const LUID device12_luid = device12_->GetAdapterLuid();
    logf("[nvs30-bridge] D3D12 device LUID=%08X:%08X game_match=%d CUDA_intercepts=%llu->%llu.\n",
         static_cast<unsigned>(device12_luid.HighPart),
         static_cast<unsigned>(device12_luid.LowPart),
         have_game_adapter &&
             device12_luid.HighPart == game_adapter_desc.AdapterLuid.HighPart &&
             device12_luid.LowPart == game_adapter_desc.AdapterLuid.LowPart ? 1 : 0,
         static_cast<unsigned long long>(cuda_before_device),
         static_cast<unsigned long long>(nvpresent::cuda_intercept_count()));

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = device12_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue12_));
    if (FAILED(hr)) {
        logf("[nvs30-bridge] CreateCommandQueue failed: 0x%08X.\n", static_cast<unsigned>(hr));
        return false;
    }

    ComPtr<IDXGIFactory2> factory;
    dxgi::set_internal_creation(true);
    const HRESULT factory_hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(factory_hr)) {
        dxgi::set_internal_creation(false);
        logf("[nvs30-bridge] CreateDXGIFactory1 failed: 0x%08X.\n",
             static_cast<unsigned>(factory_hr));
        return false;
    }
    logf("[nvs30-bridge] shadow factory=%p (fresh; game factory left untouched).\n",
         factory.Get());
    DXGI_SWAP_CHAIN_DESC1 shadow_desc{};
    shadow_desc.Width = width_;
    shadow_desc.Height = height_;
    shadow_desc.Format = linear_format(source_format_);
    shadow_desc.SampleDesc = {1, 0};
    shadow_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    // Match NvPresent's supported HWND flip-chain shape. Its D3D12 wrapper is
    // descriptor-sensitive and does not attach to the previous 3-buffer,
    // ALPHA_MODE_IGNORE variant.
    shadow_desc.BufferCount = 2;
    shadow_desc.Scaling = DXGI_SCALING_STRETCH;
    shadow_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    shadow_desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    // Exact reference descriptor: NvPresent attached to a plain two-buffer
    // flip-discard HWND chain with no swapchain flags.
    shadow_desc.Flags = 0;
    ComPtr<IDXGISwapChain1> shadow1;
    const auto cuda_before_swapchain = nvpresent::cuda_intercept_count();
    const HRESULT swap_hr = factory->CreateSwapChainForHwnd(queue12_.Get(), hwnd_, &shadow_desc,
                                                             nullptr, nullptr, &shadow1);
    const auto cuda_after_swapchain = nvpresent::cuda_intercept_count();
    dxgi::set_internal_creation(false);
    if (FAILED(swap_hr)) {
        logf("[nvs30-bridge] CreateSwapChainForHwnd(shadow) failed: 0x%08X.\n",
             static_cast<unsigned>(swap_hr));
        return false;
    }
    logf("[nvs30-bridge] shadow creation CUDA intercepts=%llu->%llu.\n",
         static_cast<unsigned long long>(cuda_before_swapchain),
         static_cast<unsigned long long>(cuda_after_swapchain));
    hr = shadow1.As(&shadow_);
    if (FAILED(hr)) {
        logf("[nvs30-bridge] shadow IDXGISwapChain3 query failed: 0x%08X.\n",
             static_cast<unsigned>(hr));
        return false;
    }


    if (!build_shared_resources(source, source_format_, width_, height_)) {
        logf("[nvs30-bridge] shared-resource initialization failed.\n");
        return false;
    }
    void* wrapper{};
    std::size_t wrapper_offset{};
    const bool wrapped = nvpresent::wrapper_active(shadow_.Get(), &wrapper, &wrapper_offset);
    if (wrapped && wrapper != shadow_.Get())
        logf("[nvs30-bridge] Wrapper layout discovered dynamically: swap=%p offset=+0x%zx wrapper=%p.\n",
             shadow_.Get(), wrapper_offset, wrapper);
    // The reference only invokes the private enable methods on a real wrapper
    // object discovered at a non-zero object offset.  The Present-trampoline
    // fallback is an activity signal, not a wrapper object.
    const bool real_wrapper_enabled =
        wrapped && wrapper_offset != 0 && activate_nvp_wrapper(wrapper);
    if (real_wrapper_enabled) {
        logf("[nvs30-bridge] >>> SUCCESS: NvPresent64 wrapped D3D12 shadow swapchain @ %p (wrapper @ %p) <<<\n",
             shadow_.Get(), wrapper);
    }
    // NvPresent may attach lazily (on first shadow Presents) and may hook
    // Present in place rather than replacing the swapchain object, so a
    // negative read immediately after CreateSwapChainForHwnd is not fatal.
    // Stay active optimistically; per-Present checks below confirm the
    // wrapper once it appears. Log the full vtable picture for diagnosis.
    {
        auto** table = *reinterpret_cast<void***>(shadow_.Get());
        void* slot8 = table ? table[8] : nullptr;
        void* slot22 = nullptr;
        MEMORY_BASIC_INFORMATION info{};
        if (table && VirtualQuery(table, &info, sizeof(info)) && info.State == MEM_COMMIT &&
            info.RegionSize >= 24 * sizeof(void*))
            slot22 = table[22];
        logf("[nvs30-bridge] shadow=%p wrapper=%p detected=%d hook_on_path=%d slot8=%p(in_nvp=%d) slot22=%p(in_nvp=%d) nvp=%p.\n",
             shadow_.Get(), wrapper, wrapped ? 1 : 0,
             nvpresent::present_hook_on_path() ? 1 : 0, slot8,
             slot8 && nvpresent::contains_address(slot8) ? 1 : 0, slot22,
             slot22 && nvpresent::contains_address(slot22) ? 1 : 0,
             nvpresent::module());
        if (!wrapped)
            logf("[nvs30-bridge] wrapper not visible yet; keeping bridge active for lazy attach (same-HWND second chain can take a few Presents).\n");
    }
    active_ = true;
    wrapper_confirmed_ = real_wrapper_enabled;
    wrapper_retired_ = false;
    frames_presented_ = 0;
    initialization_failed_ = false;
    logf("[nvs30-bridge] D3D11>D3D12 shadow swapchain active: %ux%u format=%u wrapper=%p detected=%d.\n",
         width_, height_, unsigned(source_format_), wrapper, wrapped);
    return true;
}

HRESULT D3D11Bridge::present(IDXGISwapChain* source, HWND hwnd,
                             UINT sync_interval, UINT flags) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(source->GetDesc(&desc))) return DXGI_ERROR_INVALID_CALL;
    if (!active_ || hwnd != hwnd_ || desc.BufferDesc.Width != width_ ||
        desc.BufferDesc.Height != height_ || desc.BufferDesc.Format != source_format_) {
        // Hard resource failures suppress retries only for the same shape;
        // a resize/HWND/format change always gets one fresh attempt.
        if (initialization_failed_ && hwnd == hwnd_ && desc.BufferDesc.Width == width_ &&
            desc.BufferDesc.Height == height_ && desc.BufferDesc.Format == source_format_)
            return DXGI_ERROR_UNSUPPORTED;
        // Resize/HWND change: retry init instead of suppressing forever. Only
        // a hard resource failure sets initialization_failed_, and even then
        // a different size/HWND clears the way for one fresh attempt.
        if (!initialize(source, hwnd)) {
            initialization_failed_ = true;
            logf("[nvs30-bridge] initialization failed; falling back to native for this shape (retry on resize/HWND change).\n");
            return DXGI_ERROR_UNSUPPORTED;
        }
    }
    if (completion_value_ && !wait_for_gpu(completion_value_)) return DXGI_ERROR_DEVICE_HUNG;

    ComPtr<ID3D11Texture2D> source_buffer;
    const HRESULT buffer_hr = source->GetBuffer(0, IID_PPV_ARGS(&source_buffer));
    if (FAILED(buffer_hr)) return DXGI_ERROR_DEVICE_REMOVED;
    context11_->CopyResource(shared11_.Get(), source_buffer.Get());
    if (!wait_for_d3d11_copy()) return DXGI_ERROR_DEVICE_REMOVED;

    if (FAILED(allocator12_->Reset()) || FAILED(list12_->Reset(allocator12_.Get(), nullptr)))
        return DXGI_ERROR_DEVICE_REMOVED;
    ComPtr<ID3D12Resource> destination;
    if (FAILED(shadow_->GetBuffer(shadow_->GetCurrentBackBufferIndex(),
                                  IID_PPV_ARGS(&destination)))) return DXGI_ERROR_DEVICE_REMOVED;
    std::array<D3D12_RESOURCE_BARRIER, 2> before{
        transition(shared12_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
        transition(destination.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST)};
    list12_->ResourceBarrier(static_cast<UINT>(before.size()), before.data());
    list12_->CopyResource(destination.Get(), shared12_.Get());
    std::array<D3D12_RESOURCE_BARRIER, 2> after{
        transition(destination.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT),
        transition(shared12_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON)};
    list12_->ResourceBarrier(static_cast<UINT>(after.size()), after.data());
    if (FAILED(list12_->Close())) return DXGI_ERROR_DEVICE_REMOVED;
    ID3D12CommandList* lists[]{list12_.Get()};
    queue12_->ExecuteCommandLists(1, lists);
    ++completion_value_;
    queue12_->Signal(completion_fence_.Get(), completion_value_);

    if (!wrapper_confirmed_ && !wrapper_retired_) {
        void* wrapper{};
        std::size_t wrapper_offset{};
        if (nvpresent::wrapper_active(shadow_.Get(), &wrapper, &wrapper_offset) &&
            wrapper_offset != 0 && activate_nvp_wrapper(wrapper)) {
            wrapper_confirmed_ = true;
            logf("[nvs30-bridge] wrapper enabled after %d shadow Presents: wrapper=%p offset=+0x%zx.\n",
                 frames_presented_ + 1, wrapper, wrapper_offset);
        } else if (++frames_presented_ >= 64) {
            // Match the reference bridge: retire probing after 64 Presents
            // and keep driving the NvPresent config path instead of logging
            // forever.
            wrapper_retired_ = true;
            logf("[nvs30-bridge] wrapper probing retired after %d shadow Presents; bridge continues on the NvPresent config path.\n",
                 frames_presented_);
        } else if (frames_presented_ == 1 || frames_presented_ % 32 == 0) {
            logf("[nvs30-bridge] wrapper still not visible after %d shadow Presents; continuing (hook_on_path=%d).\n",
                 frames_presented_, nvpresent::present_hook_on_path() ? 1 : 0);
        }
    }
    return shadow_->Present(sync_interval, flags & ~DXGI_PRESENT_TEST);
}
}
