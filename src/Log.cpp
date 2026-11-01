#include "Log.h"

#include <windows.h>

#include <share.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace Log {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
ULONGLONG g_start = 0;

} // namespace

void Enable(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) return;
    g_file = _wfsopen(path.c_str(), L"w", _SH_DENYNO); // readable while the program runs
    g_start = GetTickCount64();
}

bool Enabled() {
    return g_file != nullptr;
}

void Write(const char* format, ...) {
    if (!g_file) return;
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) return;
    const ULONGLONG ms = GetTickCount64() - g_start;
    fprintf(g_file, "[%6llu.%03llu] %s\n", ms / 1000, ms % 1000, line);
    fflush(g_file);
}

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size > 0 ? size : 0, '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    }
    return result;
}

} // namespace Log
