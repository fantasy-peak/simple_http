#pragma once

// Lightweight, allocation-light logging facade. The library never writes to any
// stream itself; it forwards formatted messages to a user-installed callback
// (LOG_CB). Logging can be compiled out entirely via SIMPLE_HTTP_ENABLE_LOG=0.

#include <atomic>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace simple_http {

enum class LogLevel : std::uint8_t {
    Debug = 0,
    Info = 1,
    Error = 2,
};

inline constexpr std::string_view to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Debug:
            return "Debug";
        case LogLevel::Info:
            return "Info";
        case LogLevel::Error:
            return "Error";
        default:
            return "Unknown";
    }
}

inline std::atomic<LogLevel> log_level = LogLevel::Info;

inline void set_log_level(LogLevel level) {
    log_level.store(level, std::memory_order_relaxed);
}

// Installed by the user to receive log records: (level, file, line, message).
inline std::function<void(LogLevel, std::string_view, int, std::string)> LOG_CB = [](auto, auto, auto, auto) {};

template <typename... Args>
inline void log(LogLevel level, std::string_view file, int line, std::format_string<Args...> fmt, Args&&... args) {
    if (level >= log_level.load(std::memory_order_relaxed)) {
        LOG_CB(level, file, line, std::format(fmt, std::forward<Args>(args)...));
    }
}

}  // namespace simple_http

#ifndef SIMPLE_HTTP_ENABLE_LOG
#define SIMPLE_HTTP_ENABLE_LOG 1
#endif

#if SIMPLE_HTTP_ENABLE_LOG
#define SIMPLE_HTTP_DEBUG_LOG(...) simple_http::log(simple_http::LogLevel::Debug, __FILE__, __LINE__, __VA_ARGS__)
#define SIMPLE_HTTP_INFO_LOG(...) simple_http::log(simple_http::LogLevel::Info, __FILE__, __LINE__, __VA_ARGS__)
#define SIMPLE_HTTP_ERROR_LOG(...) simple_http::log(simple_http::LogLevel::Error, __FILE__, __LINE__, __VA_ARGS__)
#else
#define SIMPLE_HTTP_DEBUG_LOG(...) ((void)0)
#define SIMPLE_HTTP_INFO_LOG(...) ((void)0)
#define SIMPLE_HTTP_ERROR_LOG(...) ((void)0)
#endif
