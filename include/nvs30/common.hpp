#pragma once

#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nvs30 {
using Microsoft::WRL::ComPtr;

inline bool succeeded(HRESULT hr) noexcept { return SUCCEEDED(hr); }
inline bool failed(HRESULT hr) noexcept { return FAILED(hr); }

template <class T>
constexpr T align_up(T value, T alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}
}
