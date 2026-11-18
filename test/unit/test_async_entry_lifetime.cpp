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

// A transport that parks the request head and body writes until the test
// completes them. This is what makes it possible to drop the user-facing
// Stream handle while an operation is in flight.
class DeferredClientStream final : public detail::ClientStream {
  public:
    explicit DeferredClientStream(asio::any_io_executor ex) : m_ex(std::move(ex)) {}

    bool finished() const override { return false; }
    Version version() const override { return Version::Http11; }
    std::uint32_t id() const override { return 1; }

    void write(std::string, WriteCompletion done) override { pending_write = std::move(done); }
    void finish(std::string, WriteCompletion done) override { pending_write = std::move(done); }
    void read(detail::ReadCompletion done) override { std::move(done)(error_code{}, ReadResult::end()); }
    void cancel(WriteCompletion done) override { std::move(done)(error_code{}); }
    void await_head(WriteCompletion done) override { pending_head = std::move(done); }
    asio::any_io_executor head_executor() const override { return m_ex; }

    // Publishes the interim head and wakes the parked head read.
    void complete_head(int code) {
        detail::ResponseHead head;
        head.status = code;
        set_head(std::move(head));
        auto done = std::move(pending_head);
        std::move(done)(error_code{});
    }

    WriteCompletion pending_head;
    WriteCompletion pending_write;

  private:
    asio::any_io_executor m_ex;
};

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

TEST_CASE("response: a token bound to another executor still runs on the owner", "[lifecycle][review]") {
    asio::io_context owner_ctx;
    asio::io_context other_ctx;
    auto life = std::make_shared<Lifecycle>(asio::any_io_executor{owner_ctx.get_executor()});
    auto sink = std::make_shared<FakeResponseSink>(owner_ctx.get_executor());
    auto writer = std::make_shared<ResponseWriter>(sink);
    writer->bind_lifecycle(life);

    bool done = false;
    // The caller says "complete on other_ctx". The state machine (and the sink
    // call) must still be posted to the owner context, so running other_ctx
    // alone must not touch the sink.
    writer->async_write("x", asio::bind_executor(other_ctx.get_executor(), [&](error_code) { done = true; }));

    other_ctx.run();
    CHECK_FALSE(done);
    CHECK(sink->chunks.empty());

    owner_ctx.run();
    CHECK(done);
    CHECK_FALSE(sink->chunks.empty());
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

TEST_CASE("stream: a write keeps the exchange alive after the handle is dropped", "[lifecycle][review]") {
    asio::io_context ctx;
    std::weak_ptr<DeferredClientStream> weak_inner;
    std::optional<error_code> result;
    bool completed = false;

    {
        auto inner = std::make_shared<DeferredClientStream>(ctx.get_executor());
        weak_inner = inner;
        auto stream = std::make_shared<Stream>(inner, std::chrono::milliseconds{0}, std::chrono::milliseconds{0},
                                               /*auto_expect_continue=*/true, std::chrono::milliseconds{50});

        stream->async_write("x", [&](error_code ec) {
            result = ec;
            completed = true;
        });
        REQUIRE(inner->pending_head);

        // Drop the caller's last handle; the in-flight write must keep its
        // exchange state (and the transport) alive on its own.
        stream.reset();
        CHECK_FALSE(weak_inner.expired());

        asio::post(ctx, [inner] { inner->complete_head(status::continue_); });
        // The mock transport parks the body write without registering I/O, so
        // the composed operation's work guard would keep run() waiting. Pump
        // with a deadline, as a real transport's readiness would drive it.
        (void)ctx.run_for(std::chrono::milliseconds{200});

        REQUIRE(inner->pending_write);
        auto done = std::move(inner->pending_write);
        std::move(done)(error_code{});
    }

    (void)ctx.run_for(std::chrono::milliseconds{200});
    REQUIRE(completed);
    CHECK_FALSE(*result);
    // The operation released the exchange when it finished: no
    // operation<->transport cycle remains.
    CHECK(weak_inner.expired());
}
