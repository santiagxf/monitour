#include "Logging.h"

#include <Windows.h>
#include <chrono>
#include <cstdio>
#include <mutex>

namespace monitour::log {

namespace {

std::mutex g_mutex;
HANDLE g_file = INVALID_HANDLE_VALUE;

std::wstring_view levelStr(Level level) {
    switch (level) {
        case Level::Trace: return L"TRACE";
        case Level::Debug: return L"DEBUG";
        case Level::Info:  return L"INFO ";
        case Level::Warn:  return L"WARN ";
        case Level::Error: return L"ERROR";
    }
    return L"?????";
}

}  // namespace

void init(std::wstring_view logPath) {
    std::lock_guard lock{g_mutex};
    if (g_file != INVALID_HANDLE_VALUE) {
        return;
    }
    g_file = CreateFileW(
        std::wstring{logPath}.c_str(),
        FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void shutdown() {
    std::lock_guard lock{g_mutex};
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
}

void write(Level level, std::wstring_view message) {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto line = std::format(L"{:%FT%TZ} [{}] {}\r\n",
                            floor<milliseconds>(now), levelStr(level), message);

    std::lock_guard lock{g_mutex};
    OutputDebugStringW(line.c_str());
    if (g_file != INVALID_HANDLE_VALUE) {
        // Convert to UTF-8 for the file. Keep it simple; logs are not hot-path.
        int needed = WideCharToMultiByte(CP_UTF8, 0, line.data(),
                                         static_cast<int>(line.size()),
                                         nullptr, 0, nullptr, nullptr);
        if (needed > 0) {
            std::string utf8(static_cast<size_t>(needed), '\0');
            WideCharToMultiByte(CP_UTF8, 0, line.data(),
                                static_cast<int>(line.size()),
                                utf8.data(), needed, nullptr, nullptr);
            DWORD written = 0;
            WriteFile(g_file, utf8.data(),
                      static_cast<DWORD>(utf8.size()), &written, nullptr);
        }
    }
}

}  // namespace monitour::log
