// Async entry points may be started from a thread other than the object's
// owner: the composed operation reposts itself onto the owner executor and
// acquires a strong reference to the object there.
//
// If the managed object is destroyed before that queued step runs, the acquire
// must fail and complete with not_connected. Falling back to the captured raw
// pointer (meant for stack-constructed, unbound objects) dereferences a
// destroyed object -- an ASan heap-use-after-free.

#include <boost/asio.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>

#include "simple_http.h"
#include "test_support.h"

using namespace simple_http;
using simple_http::test::FakeResponseSink;
using simple_http::test::MockTransport;

namespace {

// A managed writer whose last strong reference is dropped after the operation
// has been queued on the owner executor. `life` is returned separately so the
// test can keep the Lifecycle alive: the queued operation holds only a weak
// reference, and the lifecycle gate would otherwise reject it before the
// owner-acquire step this test is about.
std::shared_ptr<ResponseWriter> make_managed_writer(asio::io_context &ctx, std::shared_ptr<FakeResponseSink> &sink,
                                                    std::shared_ptr<Lifecycle> &life) {
    sink = std::make_shared<FakeResponseSink>(ctx.get_executor());
    auto writer = std::make_shared<ResponseWriter>(sink);
    life = std::make_shared<Lifecycle>(asio::any_io_executor{ctx.get_executor()});
    writer->bind_lifecycle(life);
    return writer;
}

} // namespace

TEST_CASE("response: managed writer dropped before a queued cross-thread write", "[lifecycle][review]") {
    asio::io_context ctx;
    std::shared_ptr<FakeResponseSink> sink;
    std::shared_ptr<Lifecycle> life;
    auto writer = make_managed_writer(ctx, sink, life);

    std::optional<error_code> result;
    std::thread initiator([&] {
        // Started off the owner thread: dispatch() queues the hop instead of
        // running it inline, so the completion is pending when we drop `writer`.
        writer->async_write("chunk", [&](error_code ec) { result = ec; });
    });
    initiator.join();

    writer.reset();
    ctx.run();

    REQUIRE(result.has_value());
    CHECK(*result == asio::error::not_connected);
    CHECK(sink->calls == 0);
}

TEST_CASE("response: managed writer dropped before a queued cross-thread connected query", "[lifecycle][review]") {
    asio::io_context ctx;
    std::shared_ptr<FakeResponseSink> sink;
    std::shared_ptr<Lifecycle> life;
    auto writer = make_managed_writer(ctx, sink, life);

    std::optional<error_code> result;
    bool reusable = true;
    std::thread initiator([&] {
        writer->async_connected([&](error_code ec, bool value) {
            result = ec;
            reusable = value;
        });
    });
    initiator.join();

    writer.reset();
    ctx.run();

    REQUIRE(result.has_value());
    CHECK(*result == asio::error::not_connected);
    CHECK_FALSE(reusable);
}

TEST_CASE("body: cancellation delivered while the read_all continuation is posted", "[lifecycle][review]") {
    asio::io_context ctx;
    Body body{ctx.get_executor()};
    // Both chunks and the terminator are already buffered, so the initiation
    // consumes the first one synchronously and posts the continuation for the
    // next. The cancellation below lands exactly in that gap.
    (void)body.feed("a");
    (void)body.feed("b");
    body.finish();

    asio::cancellation_signal signal;
    std::optional<error_code> result;
    std::string text;
    body.async_read_all(asio::bind_cancellation_slot(signal.slot(), [&](error_code ec, std::string value) {
        result = ec;
        text = std::move(value);
    }));

    signal.emit(asio::cancellation_type::terminal);
    ctx.run();

    REQUIRE(result.has_value());
    CHECK(*result == asio::error::operation_aborted);
    CHECK(text.empty()); // the buffered second chunk must not be delivered
}

TEST_CASE("h1 sink: body write after the response completed is rejected", "[lifecycle][review]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    auto deadline = std::make_shared<std::chrono::steady_clock::time_point>();
    auto sink = std::make_shared<Http1ResponseSink<MockTransport>>(transport, Version::Http11, deadline,
                                                                  std::chrono::seconds{30});
    sink->set_keep_alive(true);

    error_code first_ec;
    sink->send(200, {}, "A", [&](error_code ec) { first_ec = ec; });
    ctx.run();
    REQUIRE_FALSE(first_ec);
    const std::size_t first_size = transport->written().size();

    error_code late_ec;
    bool late_done = false;
    sink->send_chunk("B", [&](error_code ec) {
        late_ec = ec;
        late_done = true;
    });
    ctx.restart();
    ctx.run();

    REQUIRE(late_done);
    CHECK(late_ec == asio::error::not_connected);
    CHECK(transport->written().size() == first_size);
}
