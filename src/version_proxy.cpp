#include "nvs30/common.hpp"

namespace {
HMODULE system_version() {
    static HMODULE module = [] {
        wchar_t system[MAX_PATH]{};
        const UINT length = GetSystemDirectoryW(system, MAX_PATH);
        if (!length || length >= MAX_PATH - 12) return HMODULE{};
        std::wstring path(system, length);
        path += L"\\version.dll";
        return LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }();
    return module;
}

template <class Fn>
Fn resolve(const char* name) {
    return reinterpret_cast<Fn>(GetProcAddress(system_version(), name));
}
}

#define NVS_PROXY(ret, cc, exported, target, params, args, fallback) \
extern "C" ret cc exported params { \
    using Fn = ret (cc*) params; \
    static Fn fn = resolve<Fn>(target); \
    return fn ? fn args : fallback; \
}

NVS_PROXY(BOOL, WINAPI, nvs_GetFileVersionInfoA, "GetFileVersionInfoA",
          (LPCSTR a, DWORD b, DWORD c, LPVOID d), (a,b,c,d), FALSE)
NVS_PROXY(BOOL, WINAPI, nvs_GetFileVersionInfoW, "GetFileVersionInfoW",
          (LPCWSTR a, DWORD b, DWORD c, LPVOID d), (a,b,c,d), FALSE)
NVS_PROXY(BOOL, WINAPI, nvs_GetFileVersionInfoExA, "GetFileVersionInfoExA",
          (DWORD a, LPCSTR b, DWORD c, DWORD d, LPVOID e), (a,b,c,d,e), FALSE)
NVS_PROXY(BOOL, WINAPI, nvs_GetFileVersionInfoExW, "GetFileVersionInfoExW",
          (DWORD a, LPCWSTR b, DWORD c, DWORD d, LPVOID e), (a,b,c,d,e), FALSE)
NVS_PROXY(DWORD, WINAPI, nvs_GetFileVersionInfoSizeA, "GetFileVersionInfoSizeA",
          (LPCSTR a, LPDWORD b), (a,b), 0)
NVS_PROXY(DWORD, WINAPI, nvs_GetFileVersionInfoSizeW, "GetFileVersionInfoSizeW",
          (LPCWSTR a, LPDWORD b), (a,b), 0)
NVS_PROXY(DWORD, WINAPI, nvs_GetFileVersionInfoSizeExA, "GetFileVersionInfoSizeExA",
          (DWORD a, LPCSTR b, LPDWORD c), (a,b,c), 0)
NVS_PROXY(DWORD, WINAPI, nvs_GetFileVersionInfoSizeExW, "GetFileVersionInfoSizeExW",
          (DWORD a, LPCWSTR b, LPDWORD c), (a,b,c), 0)
NVS_PROXY(BOOL, WINAPI, nvs_GetFileVersionInfoByHandle, "GetFileVersionInfoByHandle",
          (DWORD a, HANDLE b, DWORD c, DWORD d, LPVOID e), (a,b,c,d,e), FALSE)
NVS_PROXY(DWORD, WINAPI, nvs_VerFindFileA, "VerFindFileA",
          (DWORD a,LPCSTR b,LPCSTR c,LPCSTR d,LPSTR e,PUINT f,LPSTR g,PUINT h),
          (a,b,c,d,e,f,g,h), 0)
NVS_PROXY(DWORD, WINAPI, nvs_VerFindFileW, "VerFindFileW",
          (DWORD a,LPCWSTR b,LPCWSTR c,LPCWSTR d,LPWSTR e,PUINT f,LPWSTR g,PUINT h),
          (a,b,c,d,e,f,g,h), 0)
NVS_PROXY(DWORD, WINAPI, nvs_VerInstallFileA, "VerInstallFileA",
          (DWORD a,LPCSTR b,LPCSTR c,LPCSTR d,LPCSTR e,LPCSTR f,LPSTR g,PUINT h),
          (a,b,c,d,e,f,g,h), 0)
NVS_PROXY(DWORD, WINAPI, nvs_VerInstallFileW, "VerInstallFileW",
          (DWORD a,LPCWSTR b,LPCWSTR c,LPCWSTR d,LPCWSTR e,LPCWSTR f,LPWSTR g,PUINT h),
          (a,b,c,d,e,f,g,h), 0)
NVS_PROXY(DWORD, WINAPI, nvs_VerLanguageNameA, "VerLanguageNameA",
          (DWORD a,LPSTR b,DWORD c), (a,b,c), 0)
NVS_PROXY(DWORD, WINAPI, nvs_VerLanguageNameW, "VerLanguageNameW",
          (DWORD a,LPWSTR b,DWORD c), (a,b,c), 0)
NVS_PROXY(BOOL, WINAPI, nvs_VerQueryValueA, "VerQueryValueA",
          (LPCVOID a,LPCSTR b,LPVOID* c,PUINT d), (a,b,c,d), FALSE)
NVS_PROXY(BOOL, WINAPI, nvs_VerQueryValueW, "VerQueryValueW",
          (LPCVOID a,LPCWSTR b,LPVOID* c,PUINT d), (a,b,c,d), FALSE)

#undef NVS_PROXY

