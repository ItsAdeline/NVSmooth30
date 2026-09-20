#pragma once
#include "common.hpp"

namespace nvs30 {
class D3D11Bridge {
public:
    ~D3D11Bridge();
    HRESULT present(IDXGISwapChain* source, HWND hwnd, UINT sync_interval, UINT flags);
    bool active() const noexcept { return active_; }
    bool wrapper_active() const noexcept;
    void reset();

private:
    bool initialize(IDXGISwapChain* source, HWND hwnd);
    bool build_shared_resources(IDXGISwapChain* source, DXGI_FORMAT source_format,
                                UINT width, UINT height);
    bool wait_for_gpu(std::uint64_t value);
    bool wait_for_d3d11_copy();
    static DXGI_FORMAT linear_format(DXGI_FORMAT format);

    bool active_{};
    bool initialization_failed_{};
    bool wrapper_confirmed_{};
    bool wrapper_retired_{};
    int frames_presented_{};
    HWND hwnd_{};
    UINT width_{};
    UINT height_{};
    DXGI_FORMAT source_format_{DXGI_FORMAT_UNKNOWN};

    ComPtr<ID3D11Device> device11_;
    ComPtr<ID3D11DeviceContext> context11_;
    ComPtr<ID3D11Texture2D> shared11_;
    ComPtr<ID3D11Query> copy_query_;

    ComPtr<ID3D12Device> device12_;
    ComPtr<ID3D12CommandQueue> queue12_;
    ComPtr<ID3D12CommandAllocator> allocator12_;
    ComPtr<ID3D12GraphicsCommandList> list12_;
    ComPtr<ID3D12Resource> shared12_;
    ComPtr<ID3D12Fence> completion_fence_;
    ComPtr<IDXGISwapChain3> shadow_;
    HANDLE completion_event_{};
    std::uint64_t completion_value_{};
};
}
