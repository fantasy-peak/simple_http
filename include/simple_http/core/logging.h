#pragma once

// Logging facade.
//
// The library never writes to a stream itself. It formats a record and hands it
// to whatever LogSink is installed, so a deployment can route records into
// spdlog, an in-file JSON pipeline, an OpenTelemetry bridge, or nothing at all —
// without the library depending on, or even naming, any of them.
//
// A record carries two things a sink may format: a human-readable `message`, and
// a set of typed `attrs` (key + value), the shape Go's `log/slog` and Rust's
// `tracing` settled on. That split is deliberate:
//
//   * fields are first-class at the call site (typed, not flattened into the
//     message), so a sink can emit them as structured data;
//   * the *format* is the sink's decision (JSON lines, key=value text, an
//     spdlog record, ECS/OTEL schema), not the call site's. `make_json_sink`
//     is the ready-made line-delimited-JSON sink; a text sink renders the same
//     attrs as `key=value`.
//
// An adapter for an existing logging library is a small LogSink subclass written
// on the consumer side. That is deliberate: shipping adapters in the library
// would put the third-party headers on every consumer's include path and imply a
// dependency the library does not have. The interface therefore contains no
// third-party types.
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
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <mutex>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

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

// A structured field: a key and a typed value. The key and a string value are
// borrowed (they point at caller storage) and only guaranteed for the duration
// of the sink call, exactly like LogRecord::message; a sink that needs either
// beyond that must copy it.
struct Attr {
    using Value = std::variant<std::string_view, std::int64_t, std::uint64_t, double, bool>;

    std::string_view key;
    Value value;

    Attr(std::string_view k, std::string_view v) : key(k), value(v) {}
    Attr(std::string_view k, const char *v) : key(k), value(std::string_view{v}) {}
    Attr(std::string_view k, bool v) : key(k), value(v) {}
    // Any other arithmetic type (int, unsigned, long, size_t, float, ...)
    // narrows to a signed/unsigned 64-bit or a double, so there is one overload
    // rather than a ladder that can be ambiguous across platforms.
    template <typename T>
        requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
    Attr(std::string_view k, T v) : key(k) {
        if constexpr (std::is_floating_point_v<T>) {
            value = static_cast<double>(v);
        } else if constexpr (std::is_signed_v<T>) {
            value = static_cast<std::int64_t>(v);
        } else {
            value = static_cast<std::uint64_t>(v);
        }
    }
};

// Request-scoped key/value context for the access log — the cheap stand-in for a
// `tracing` span / `slog` logger.With. A middleware or handler attaches context
// once (a user id, a tenant, a trace id) and access_log() merges it into the
// per-request record. Values are owned (unlike Attr's borrowed ones), because
// they outlive the call that set them.
struct LogFields {
    std::vector<std::pair<std::string, std::string>> kv;

    void add(std::string key, std::string value) { kv.emplace_back(std::move(key), std::move(value)); }
};

// One record, handed to a sink.
//
// `message` and `category` point at storage that is only guaranteed to live for
// the duration of the sink call; a sink that needs either beyond that must copy
// it. `attrs` (empty when the record carries no structured fields) has the same
// contract; this is what keeps the facade allocation-free for the common case.
struct LogRecord {
    LogLevel level;
    std::string_view category; // module tag; empty when the caller has none
    std::source_location where;
    std::string_view message;
    std::span<const Attr> attrs{};
};

class LogSink {
  public:
    virtual ~LogSink() = default;

    // Whether `level` is worth recording. Asked before the message is formatted,
    // so a dropped record costs a virtual call and nothing else.
    virtual bool enabled(LogLevel level) const noexcept = 0;

    // `record` (its message, category and every attr key/value) is valid only
    // for the duration of this call.
    virtual void log(const LogRecord &record) = 0;
};

namespace detail {

// The installed sink. std::atomic<std::shared_ptr<>> gives readers a single
// atomic load and writers a safe swap, with no lock on the logging path; a
// thread inside a sink holds its own shared_ptr, so a swap cannot pull the sink
// out from under it.
inline std::atomic<std::shared_ptr<LogSink>> &sink_slot() noexcept {
    static std::atomic<std::shared_ptr<LogSink>> slot;
    return slot;
}

// Messages up to this length are formatted onto the stack; longer ones fall
// back to a heap buffer. Sized for the usual case of an error plus its context.
inline constexpr std::size_t kInlineMessageBytes = 512;

// The value of an attr rendered for the text sink.
inline std::string attr_text(const Attr &attr) {
    std::string out;
    std::visit(
        [&out](const auto &arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::string_view>) {
                out.assign(arg);
            } else if constexpr (std::is_same_v<T, bool>) {
                out = arg ? "true" : "false";
            } else {
                out = std::format("{}", arg);
            }
        },
        attr.value);
    return out;
}

// Shared tail of the stdio-backed sinks: the level gate and one locked,
// flushed fwrite per record. Subclasses supply only the formatted line.
class FileSinkBase : public LogSink {
  public:
    FileSinkBase(LogLevel minimum, std::FILE *stream) : m_minimum(minimum), m_stream(stream) {}

    bool enabled(LogLevel level) const noexcept override {
        return static_cast<std::uint8_t>(level) >= static_cast<std::uint8_t>(m_minimum);
    }

  protected:
    void write_line(std::string_view line) {
        const std::lock_guard lock{m_mutex};
        std::fwrite(line.data(), 1, line.size(), m_stream);
        std::fflush(m_stream);
    }

  private:
    LogLevel m_minimum;
    std::FILE *m_stream;
    std::mutex m_mutex;
};

// A one-line-per-record text sink on a stdio stream. Enough for examples, tests
// and small tools; a deployment usually installs its own. Attributes are
// appended as ` key=value`.
class StreamSink final : public FileSinkBase {
  public:
    using FileSinkBase::FileSinkBase;

    void log(const LogRecord &record) override {
        // One string, one fwrite, under a lock: concurrent records must not
        // interleave halfway through a line.
        std::string line = std::format("[{}] {}:{}: {}", to_string(record.level), basename(record.where.file_name()),
                                       record.where.line(), record.message);
        for (const Attr &attr : record.attrs) {
            line += ' ';
            line += attr.key;
            line += '=';
            line += attr_text(attr);
        }
        line.push_back('\n');
        write_line(line);
    }
};

// Line-delimited JSON, one object per record. The production sink for log
// pipelines (Loki/ELK/Datadog/CloudWatch): fields stay typed and queryable.
// Defined in logging_json.h (glaze-backed) so this header stays third-party
// free; make_json_sink() is declared there.

// Message-only entry point (used by the per-level macros). Formats `fmt` and
// its args onto the stack and emits a record with no attrs.
template <typename... Args>
void dispatch(LogLevel level, std::string_view category, std::source_location where, std::format_string<Args...> fmt,
              Args &&...args) {
    auto sink = sink_slot().load(std::memory_order_acquire);
    if (!sink || !sink->enabled(level)) {
        return; // nothing is formatted for a record the sink would drop
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

// Structured entry point: a ready message plus typed attributes.
inline void emit_attrs(LogLevel level, std::string_view category, std::source_location where, std::string_view message,
                       std::span<const Attr> attrs) {
    auto sink = sink_slot().load(std::memory_order_acquire);
    if (!sink || !sink->enabled(level)) {
        return;
    }
    sink->log(LogRecord{level, category, where, message, attrs});
}

} // namespace detail

// Installs `sink` (nullptr drops every record). Safe to call at any time, from
// any thread, while other threads are logging.
inline void set_log_sink(std::shared_ptr<LogSink> sink) {
    detail::sink_slot().store(std::move(sink), std::memory_order_release);
}

inline std::shared_ptr<LogSink> log_sink() noexcept { return detail::sink_slot().load(std::memory_order_acquire); }

// Writes to stderr: "<level> <file>:<line>: <message> [key=value ...]".
inline std::shared_ptr<LogSink> make_stderr_sink(LogLevel minimum = LogLevel::Info) {
    return std::make_shared<detail::StreamSink>(minimum, stderr);
}

// Writes to stdout, for examples and tests that want it on a captureable
// stream.
inline std::shared_ptr<LogSink> make_stdout_sink(LogLevel minimum = LogLevel::Info) {
    return std::make_shared<detail::StreamSink>(minimum, stdout);
}

// Writes line-delimited JSON to stderr (glaze-backed, see logging_json.h). The
// structured counterpart of make_stderr_sink, for a log pipeline.

// What the macros expand to. Call it directly to name the category, or to
// supply a location other than the call site. `where` must be
// std::source_location::current() evaluated at the call site — the type has no
// public constructor, so a wrapper function cannot capture the caller's
// position for you, which is the whole reason the macros exist.
template <typename... Args>
inline void log(LogLevel level, std::string_view category, std::source_location where, std::format_string<Args...> fmt,
                Args &&...args) {
    detail::dispatch(level, category, where, fmt, std::forward<Args>(args)...);
}

// The structured entry point: a ready message plus typed attributes. Call it
// directly (it has no format string, so it needs no macro); see access_log() for
// the library's own use.
inline void log_with_attrs(LogLevel level, std::string_view category, std::source_location where,
                           std::string_view message, std::span<const Attr> attrs) {
    detail::emit_attrs(level, category, where, message, attrs);
}

} // namespace simple_http

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
#define SIMPLE_HTTP_LOG_AT(level, category, ...)                                                                       \
    do {                                                                                                               \
        if constexpr (static_cast<int>(level) >= SIMPLE_HTTP_LOG_ACTIVE_LEVEL) {                                       \
            ::simple_http::log(level, category, std::source_location::current(), __VA_ARGS__);                         \
        }                                                                                                              \
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
