#pragma once

// The JSON sink for the logging facade, backed by glaze (already a dependency
// of the library). It lives in its own header rather than logging.h so the
// facade stays third-party-free and a translation unit that only logs through
// the macros does not pull glaze in — logging.h is included very widely.
//
// Every record is one line-delimited JSON object. Typed attributes stay typed:
// glaze's `generic_json` in `num_mode::u64` keeps integers exact (rather than
// collapsing them to double) and preserves field order.

#include <glaze/glaze.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

#include "logging.h"

namespace simple_http {

namespace detail {

// Milliseconds since the Unix epoch, the JSON timestamp.
inline std::int64_t now_millis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Renders one record as a single line-delimited JSON object (with a trailing
// newline). Exposed so a sink and the tests can share it. glaze handles string
// escaping and number formatting.
inline std::string format_json(const LogRecord &record) {
    glz::generic_json<glz::num_mode::u64> object;
    object["ts"] = now_millis();
    object["level"] = std::string{to_string(record.level)};
    object["cat"] = std::string{record.category};
    object["msg"] = std::string{record.message};
    if (record.where.file_name()[0] != '\0') {
        std::string src{basename(record.where.file_name())};
        src.push_back(':');
        src += std::to_string(record.where.line());
        object["src"] = std::move(src);
    }
    for (const Attr &attr : record.attrs) {
        std::visit([&](const auto &value) { object[attr.key] = value; }, attr.value);
    }
    auto dumped = object.dump();
    std::string line = dumped ? std::move(*dumped) : std::string{"{}"};
    line.push_back('\n');
    return line;
}

// Line-delimited JSON, one object per record. The production sink for log
// pipelines (Loki/ELK/Datadog/CloudWatch): fields stay typed and queryable.
class JsonSink final : public FileSinkBase {
  public:
    using FileSinkBase::FileSinkBase;

    void log(const LogRecord &record) override { write_line(format_json(record)); }
};

} // namespace detail

// Writes line-delimited JSON to stderr — the structured counterpart of
// make_stderr_sink, for a log pipeline.
inline std::shared_ptr<LogSink> make_json_sink(LogLevel minimum = LogLevel::Info) {
    return std::make_shared<detail::JsonSink>(minimum, stderr);
}

} // namespace simple_http
