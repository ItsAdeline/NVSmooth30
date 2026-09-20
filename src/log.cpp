#include "nvs30/log.hpp"

#include <cstdarg>
#include <cstdio>

namespace nvs30 {
namespace {
std::mutex g_mutex;
FILE* g_file{};
}

void log_open() {
    std::scoped_lock lock(g_mutex);
    if (g_file) return;
    wchar_t module[MAX_PATH]{};
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    std::filesystem::path path(module);
    path = path.parent_path() / L"nvsmooth30.log";
    // logf formats UTF-8/narrow text and writes it with fputs.  Opening with
    // ccs=UTF-8 makes the CRT stream wide-oriented, so narrow writes fail and
    // leave an empty log file.  Keep the stream byte-oriented instead.
    _wfopen_s(&g_file, path.c_str(), L"ab");
}

void log_close() {
    std::scoped_lock lock(g_mutex);
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
}

void logf(const char* format, ...) {
    char line[2048]{};
    va_list args;
    va_start(args, format);
    vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);
    va_end(args);

    OutputDebugStringA(line);
    std::scoped_lock lock(g_mutex);
    if (!g_file) return;
    std::fputs(line, g_file);
    std::fflush(g_file);
}
}
