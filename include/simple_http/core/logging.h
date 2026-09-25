#pragma once

// Logging facade.
//
// The library never writes to a stream itself. It formats a record and hands it
// to whatever LogSink is installed, so a deployment can route records into
// spdlog, an in-house library, or nothing at all — without the library depending
// on, or even naming, any of them.
//
// An adapter for an existing logging library is a small LogSink subclass written
// on the consumer side. That is deliberate: shipping adapters in the library
// would put the third-party headers on every consumer's include path and imply a
// dependency the library does not have. The interface therefore contains no
// third-party types, and spans no logging API's feature set — it carries the
// fields every logger needs (level, location, category, message) and nothing
// more, leaving structured fields, routing and formatting to the backend.
//
// Three properties the design is built around:
//
//   * The sink decides the level. `enabled()` is asked before the message is
//     built, so a sink configured to drop a level costs nothing for records at
//     that level — and a logger that already knows its own threshold does not
//     need it configured twice.
//
//   * Installing a sink is safe while other threads log: the swap is atomic, and
//     a thread already inside a sink keeps that sink alive until it returns.
//
//   * Nothing is allocated to log the common case. Messages up to
//     kInlineMessageBytes are formatted onto the stack.
//
// Logging can be compiled out entirely with SIMPLE_HTTP_ENABLE_LOG=0, or
// compiled down to a minimum level with SIMPLE_HTTP_LOG_ACTIVE_LEVEL.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace simple_http {

// Severity, ordered from most to least verbose and deliberately aligned with
// spdlog's set so an adapter is a lookup rather than a lossy translation.
enum class LogLevel : std::uint8_t {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Critical = 5,
};

inline constexpr std::string_view to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:
            return "trace";
        case LogLevel::Debug:
            return "debug";
        case LogLevel::Info:
            return "info";
        case LogLevel::Warn:
            return "warn";
        case LogLevel::Error:
            return "error";
        case LogLevel::Critical:
            return "critical";
    }
    return "unknown";
}

// The trailing component of a path (".../engine/h1_engine.h" -> "h1_engine.h"),
// so a record's location stays readable.
inline constexpr std::string_view basename(std::string_view path) noexcept {
    const auto separator = path.find_last_of("/\\");
    return separator == std::string_view::npos ? path : path.substr(separator + 1);
}

// One record, handed to a sink.
//
// `message` and `category` point at storage that is only guaranteed to live for
// the duration of the sink call; a sink that needs either beyond that must copy
// it. This is what keeps the facade allocation-free.
struct LogRecord {
    LogLevel level;
    std::string_view category;  // module tag; empty when the caller has none
    std::source_location where;
    std::string_view message;
};

class LogSink {
  public:
    virtual ~LogSink() = default;

    // Whether `level` is worth recording. Asked before the message is formatted,
    // so a dropped record costs a virtual call and nothing else.
    virtual bool enabled(LogLevel level) const noexcept = 0;

    // `record` is valid only for the duration of this call.
    virtual void log(const LogRecord& record) = 0;
};

namespace detail {

// The installed sink. std::atomic<std::shared_ptr<>> gives readers a single
// atomic load and writers a safe swap, with no lock on the logging path; a
// thread inside a sink holds its own shared_ptr, so a swap cannot pull the sink
// out from under it.
inline std::atomic<std::shared_ptr<LogSink>>& sink_slot() noexcept {
    static std::atomic<std::shared_ptr<LogSink>> slot;
    return slot;
}

// Messages up to this length are formatted onto the stack; longer ones fall back
// to a heap buffer. Sized for the usual case of an error plus its context.
inline constexpr std::size_t kInlineMessageBytes = 512;

template <typename... Args>
void dispatch(LogLevel level, std::string_view category, std::source_location where,
              std::format_string<Args...> fmt, Args&&... args) {
    auto sink = sink_slot().load(std::memory_order_acquire);
    if (!sink || !sink->enabled(level)) {
        return;  // nothing is formatted for a record the sink would drop
    }

    std::array<char, kInlineMessageBytes> inline_buffer{};
    const auto written = std::format_to_n(inline_buffer.data(), inline_buffer.size(), fmt, std::forward<Args>(args)...);
    // format_to_n reports the length it would have produced, as a signed
    // difference type, whether or not it was truncated.
    if (std::cmp_less_equal(written.size, inline_buffer.size())) {
        const auto length = static_cast<std::size_t>(written.size);
        sink->log(LogRecord{level, category, where, std::string_view{inline_buffer.data(), length}});
        return;
    }
    // Rare: the message outgrew the stack buffer. Format it again, this time into
    // a heap buffer that is freed when the sink returns.
    const std::string oversized = std::format(fmt, std::forward<Args>(args)...);
    sink->log(LogRecord{level, category, where, oversized});
}

// A one-line-per-record sink on a stdio stream. Enough for examples, tests and
// small tools; a deployment usually installs its own.
class StreamSink final : public LogSink {
  public:
    StreamSink(LogLevel minimum, std::FILE* stream) : m_minimum(minimum), m_stream(stream) {}

    bool enabled(LogLevel level) const noexcept override {
        return static_cast<std::uint8_t>(level) >= static_cast<std::uint8_t>(m_minimum);
    }

    void log(const LogRecord& record) override {
        // One string, one fwrite, under a lock: concurrent records must not
        // interleave halfway through a line.
        const std::string line = std::format("[{}] {}:{}: {}\n", to_string(record.level),
                                             basename(record.where.file_name()), record.where.line(), record.message);
        const std::lock_guard lock{m_mutex};
        std::fwrite(line.data(), 1, line.size(), m_stream);
        std::fflush(m_stream);
    }

  private:
    LogLevel m_minimum;
    std::FILE* m_stream;
    std::mutex m_mutex;
};

}  // namespace detail

// Installs `sink` (nullptr drops every record). Safe to call at any time, from
// any thread, while other threads are logging.
inline void set_log_sink(std::shared_ptr<LogSink> sink) {
    detail::sink_slot().store(std::move(sink), std::memory_order_release);
}

inline std::shared_ptr<LogSink> log_sink() noexcept {
    return detail::sink_slot().load(std::memory_order_acquire);
}

// Writes to stderr: "<level> <file>:<line>: <message>".
inline std::shared_ptr<LogSink> make_stderr_sink(LogLevel minimum = LogLevel::Info) {
    return std::make_shared<detail::StreamSink>(minimum, stderr);
}

// Writes to stdout, for examples and tests that want it on a captureable stream.
inline std::shared_ptr<LogSink> make_stdout_sink(LogLevel minimum = LogLevel::Info) {
    return std::make_shared<detail::StreamSink>(minimum, stdout);
}

// What the macros expand to. Call it directly to name the category, or to supply
// a location other than the call site. `where` must be std::source_location::
// current() evaluated at the call site — the type has no public constructor, so
// a wrapper function cannot capture the caller's position for you, which is the
// whole reason the macros exist.
template <typename... Args>
inline void log(LogLevel level, std::string_view category, std::source_location where,
                std::format_string<Args...> fmt, Args&&... args) {
    detail::dispatch(level, category, where, fmt, std::forward<Args>(args)...);
}

}  // namespace simple_http

#ifndef SIMPLE_HTTP_ENABLE_LOG
#define SIMPLE_HTTP_ENABLE_LOG 1
#endif

// Records below this level are compiled out (0 = Trace .. 5 = Critical). Unlike
// a runtime check this costs nothing at the call site, and the discarded branch
// still has to parse — so a record that has rotted still fails to compile.
#ifndef SIMPLE_HTTP_LOG_ACTIVE_LEVEL
#define SIMPLE_HTTP_LOG_ACTIVE_LEVEL 0
#endif

#if SIMPLE_HTTP_ENABLE_LOG
// `category` is a string_view; use "" when the call site has no module tag.
#define SIMPLE_HTTP_LOG_AT(level, category, ...)                                                 \
    do {                                                                                         \
        if constexpr (static_cast<int>(level) >= SIMPLE_HTTP_LOG_ACTIVE_LEVEL) {                 \
            ::simple_http::log(level, category, std::source_location::current(), __VA_ARGS__);   \
        }                                                                                        \
    } while (false)
#else
#define SIMPLE_HTTP_LOG_AT(level, category, ...) ((void)0)
#endif

#define SIMPLE_HTTP_TRACE_LOG(...) SIMPLE_HTTP_LOG_AT(::simple_http::LogLevel::Trace, "", __VA_ARGS__)
#define SIMPLE_HTTP_DEBUG_LOG(...) SIMPLE_HTTP_LOG_AT(::simple_http::LogLevel::Debug, "", __VA_ARGS__)
#define SIMPLE_HTTP_INFO_LOG(...) SIMPLE_HTTP_LOG_AT(::simple_http::LogLevel::Info, "", __VA_ARGS__)
#define SIMPLE_HTTP_WARN_LOG(...) SIMPLE_HTTP_LOG_AT(::simple_http::LogLevel::Warn, "", __VA_ARGS__)
#define SIMPLE_HTTP_ERROR_LOG(...) SIMPLE_HTTP_LOG_AT(::simple_http::LogLevel::Error, "", __VA_ARGS__)
#define SIMPLE_HTTP_CRITICAL_LOG(...) SIMPLE_HTTP_LOG_AT(::simple_http::LogLevel::Critical, "", __VA_ARGS__)

// Same, tagged with a module name ("h2", "router", …) that an adapter can route
// on. The library itself logs untagged; a sink decides what to make of that.
#define SIMPLE_HTTP_TRACE_LOG_CAT(category, ...) \
    SIMPLE_HTTP_LOG_AT(::simple_http::LogLevel::Trace, category, __VA_ARGS__)
#define SIMPLE_HTTP_DEBUG_LOG_CAT(category, ...) \
    SIMPLE_HTTP_LOG_AT(::simple_http::LogLevel::Debug, category, __VA_ARGS__)
#define SIMPLE_HTTP_INFO_LOG_CAT(category, ...) SIMPLE_HTTP_LOG_AT(::simple_http::LogLevel::Info, category, __VA_ARGS__)
#define SIMPLE_HTTP_WARN_LOG_CAT(category, ...) SIMPLE_HTTP_LOG_AT(::simple_http::LogLevel::Warn, category, __VA_ARGS__)
#define SIMPLE_HTTP_ERROR_LOG_CAT(category, ...) \
    SIMPLE_HTTP_LOG_AT(::simple_http::LogLevel::Error, category, __VA_ARGS__)
#define SIMPLE_HTTP_CRITICAL_LOG_CAT(category, ...) \
    SIMPLE_HTTP_LOG_AT(::simple_http::LogLevel::Critical, category, __VA_ARGS__)
