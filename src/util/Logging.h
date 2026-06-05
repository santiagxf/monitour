#pragma once
#include <format>
#include <string>
#include <string_view>

namespace monitour::log {

enum class Level { Trace, Debug, Info, Warn, Error };

void init(std::wstring_view logPath);
void shutdown();
void write(Level level, std::wstring_view message);

template <typename... Args>
void info(std::wformat_string<Args...> fmt, Args&&... args) {
    write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void warn(std::wformat_string<Args...> fmt, Args&&... args) {
    write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void error(std::wformat_string<Args...> fmt, Args&&... args) {
    write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void debug(std::wformat_string<Args...> fmt, Args&&... args) {
    write(Level::Debug, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace monitour::log
