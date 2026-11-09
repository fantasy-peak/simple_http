// core/logging: the structured record path. Fields are typed attributes on the
// record (Go log/slog / Rust tracing shape); the sink owns the encoding, which
// a capturing sink stands in for here. (The JSON sink module was dropped from
// this port; test/cpp17 covers the remaining log surface.)

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "simple_http.h"
#include "test_support.h"

using namespace simple_http;
using namespace simple_http::test;

namespace {

// A sink that copies every record out, preserving each attr's rendered value.
struct CapturingSink final : LogSink {
    struct Rec {
        LogLevel level;
        std::string category;
        std::string message;
        std::vector<std::pair<std::string, std::string>> attrs;
    };
    std::vector<Rec> recs;

    bool enabled(LogLevel) const noexcept override { return true; }

    void log(const LogRecord &record) override {
        Rec rec{record.level, std::string{record.category}, std::string{record.message}, {}};
        for (const Attr &attr : record.attrs) {
            rec.attrs.emplace_back(std::string{attr.key}, detail::attr_text(attr));
        }
        recs.push_back(std::move(rec));
    }
};

// Swaps the installed sink for the test's, restoring it at scope exit.
struct SinkScope {
    std::shared_ptr<LogSink> prev;
    explicit SinkScope(std::shared_ptr<LogSink> sink) : prev(log_sink()) { set_log_sink(std::move(sink)); }
    ~SinkScope() { set_log_sink(std::move(prev)); }
};

} // namespace

TEST_CASE("core/logging: Attr overloads keep the value type", "[core][logging]") {
    CHECK(std::get<std::int64_t>(Attr{"n", 5}.value) == 5);
    CHECK(std::get<std::uint64_t>(Attr{"n", std::size_t{5}}.value) == 5u);
    CHECK(std::get<double>(Attr{"d", 1.25}.value) == 1.25);
    CHECK(std::get<bool>(Attr{"b", true}.value));
    CHECK(std::get<std::string_view>(Attr{"s", "hi"}.value) == "hi");
    CHECK(std::get<std::string_view>(Attr{"s", std::string{"hi"}}.value) == "hi");
}

TEST_CASE("core/logging: log_with_attrs reaches the sink with its fields", "[core][logging]") {
    auto sink = std::make_shared<CapturingSink>();
    CapturingSink *captured = sink.get();
    SinkScope scope{sink};

    const Attr attrs[] = {{"method", "POST"}, {"status", std::int64_t{status::created}}};
    log_with_attrs(LogLevel::Info, "access", source_location::current(), "request", attrs);

    REQUIRE(captured->recs.size() == 1);
    const auto &rec = captured->recs.front();
    CHECK(rec.level == LogLevel::Info);
    CHECK(rec.category == "access");
    CHECK(rec.message == "request");
    REQUIRE(rec.attrs.size() == 2);
    CHECK(rec.attrs[0].first == "method");
    CHECK(rec.attrs[0].second == "POST");
    CHECK(rec.attrs[1].first == "status");
    CHECK(rec.attrs[1].second == "201");
}

TEST_CASE("core/logging: request LogFields accumulate for the access log", "[core][logging]") {
    asio::io_context ctx;
    Request req{Version::Http11, ctx.get_executor(), asio::ip::tcp::endpoint{}};

    CHECK(req.get_state<LogFields>() == nullptr);
    req.log_fields().add("user_id", "u1");
    req.log_fields().add("tenant", "acme");

    const auto *fields = req.get_state<LogFields>();
    REQUIRE(fields != nullptr);
    REQUIRE(fields->kv.size() == 2);
    CHECK(fields->kv[0].first == "user_id");
    CHECK(fields->kv[0].second == "u1");
    CHECK(fields->kv[1].first == "tenant");
    CHECK(fields->kv[1].second == "acme");
}
