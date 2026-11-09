// Minimal reproductions for defects found while reviewing the C++17
// completion-token port. Each test asserts the *correct* behaviour, so it fails
// before the corresponding fix and passes afterwards.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>

#include "simple_http.h"
#include "test_support.h"

using namespace simple_http;
using namespace simple_http::test;

namespace {

// A ClientStream that is already finished: read() reports a normal EOF without
// producing bytes. Used to drive the buffered-Response path.
class FinishedClientStream final : public detail::ClientStream {
  public:
    explicit FinishedClientStream(asio::any_io_executor ex) : m_executor(std::move(ex)) {}

    void write(std::string, WriteCompletion done) override { std::move(done)(error_code{}); }
    void finish(std::string, WriteCompletion done) override { std::move(done)(error_code{}); }
    void read(detail::ReadCompletion done) override { std::move(done)(error_code{}, ReadResult::end()); }
    void cancel(WriteCompletion done) override { std::move(done)(error_code{}); }
    void await_head(WriteCompletion done) override { std::move(done)(error_code{}); }
    bool finished() const override { return true; }
    Version version() const override { return Version::Http11; }
    std::uint32_t id() const override { return 0; }
    asio::any_io_executor head_executor() const override { return m_executor; }

  private:
    asio::any_io_executor m_executor;
};

} // namespace

// A compress decorator is installed whenever the client's Accept-Encoding is
// negotiable (no middleware::compress required) and it keeps a strong
// shared_ptr<Request>. The session middleware stores a Session on the request,
// and that Session captures the ResponseWriter so it can set cookies. The two
// edges close a cycle that outlives the handler.
TEST_CASE("review: compress decorator must not close a Request/Response cycle", "[review][regression]") {
    asio::io_context ctx;
    auto req = std::make_shared<Request>(Version::Http11, ctx.get_executor());
    req->mutable_headers().add("accept-encoding", "gzip");

    auto inner = std::make_shared<FakeResponseSink>(ctx.get_executor());
    auto sink = maybe_compress_writer(inner, ctx.get_executor(), req, false);
    REQUIRE(sink != inner); // gzip was negotiated, so the decorator is installed

    auto res = std::make_shared<ResponseWriter>(sink);
    // This is the edge handler/session.h adds: the per-request Session (stored
    // in the request) holds the ResponseWriter to write the cookie header.
    req->set_state(res);

    std::weak_ptr<Request> weak_req = req;
    std::weak_ptr<ResponseWriter> weak_res = res;
    req.reset();
    res.reset();
    sink.reset();
    inner.reset();

    CHECK(weak_req.expired());
    CHECK(weak_res.expired());
}

// invoke_handler must always complete the handler, including when a callback
// handler throws synchronously. Otherwise the engine never runs after_handler /
// handler_finished: h1 stops reading, h2/h3 leak their concurrency slot.
TEST_CASE("review: invoke_handler completes a handler that throws synchronously", "[review][regression]") {
    asio::io_context ctx;
    auto req = std::make_shared<Request>(Version::Http11, ctx.get_executor());
    auto inner = std::make_shared<FakeResponseSink>(ctx.get_executor());
    auto res = std::make_shared<ResponseWriter>(inner);

    bool done_called = false;
    HandlerCompletion done = asio::any_completion_handler<void()>([&done_called]() { done_called = true; });
    auto handler = make_handler(
        [](RequestPtr, ResponsePtr, HandlerCompletion) { throw std::runtime_error("handler boom"); });

    invoke_handler(handler, req, res, SslHandle{}, std::move(done));
    ctx.run_for(std::chrono::milliseconds(200));

    CHECK(done_called);
}

// A buffered Response already owns the whole body; async_read_all must return
// it rather than walking the (finished) stream, which reports session_closed or
// an empty EOF.
TEST_CASE("review: buffered Response::async_read_all returns the buffered body", "[review][regression]") {
    asio::io_context ctx;
    detail::ResponseHead head;
    head.status = 200;
    head.version = Version::Http11;
    Response response{head, "hello"};

    std::optional<std::pair<error_code, std::string>> got;
    response.async_read_all(0, [&got](error_code ec, std::string body) {
        got = std::make_pair(ec, std::move(body));
    });
    ctx.run_for(std::chrono::milliseconds(200));

    REQUIRE(got.has_value());
    CHECK_FALSE(got->first);
    CHECK(got->second == "hello");
}

// A stream may outlive its exchange (Response keeps it). Once the connection
// has been reused, that stale handle must not write into the new request or
// read/teardown the new response.
TEST_CASE("review: a finished h1 stream cannot act on the next exchange", "[review][regression]") {
    using Session = detail::Http1ClientSession<MockTransport>;
    asio::io_context ctx;
    auto pump = [&ctx] { ctx.restart(); ctx.run_for(std::chrono::milliseconds(500)); };
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    auto session = std::make_shared<Session>(transport, "example.com", EngineLimits{});

    auto request_a = std::make_shared<Request>(Version::Http11, ctx.get_executor());
    request_a->set_method(Method::Get);
    request_a->set_target("/a");
    std::shared_ptr<detail::ClientStream> stream_a;
    session->open_stream(request_a, [&stream_a](error_code ec, std::shared_ptr<detail::ClientStream> s) {
        if (!ec)
            stream_a = std::move(s);
    });
    pump();
    REQUIRE(stream_a);

    transport->push("HTTP/1.1 200 OK\r\ncontent-length: 0\r\n\r\n");
    bool head_a = false;
    stream_a->async_read_head([&head_a](error_code ec, detail::ResponseHead) { head_a = !ec; });
    pump();
    REQUIRE(head_a);

    bool body_a = false;
    stream_a->async_read([&body_a](error_code ec, ReadResult result) { body_a = !ec && result.eof; });
    pump();
    REQUIRE(body_a);

    // Reuse the idle h1 session with a streaming POST (chunked framing).
    auto request_b = std::make_shared<Request>(Version::Http11, ctx.get_executor());
    request_b->set_method(Method::Post);
    request_b->set_target("/b");
    request_b->set_stream_body(true);
    request_b->set_content_length(-1);
    std::shared_ptr<detail::ClientStream> stream_b;
    session->open_stream(request_b, [&stream_b](error_code ec, std::shared_ptr<detail::ClientStream> s) {
        if (!ec)
            stream_b = std::move(s);
    });
    pump();
    REQUIRE(stream_b);

    const std::string written_before = transport->written();
    std::optional<error_code> stale_ec;
    stream_a->async_write("evil", [&stale_ec](error_code ec) { stale_ec = ec; });
    pump();

    REQUIRE(stale_ec.has_value());
    CHECK(*stale_ec);                                          // stale handle rejected
    CHECK(transport->written() == written_before);             // nothing reached the wire
}

// Chunked trailers are per-exchange state. A keep-alive connection must not
// publish the previous exchange's trailer section on the next one.
TEST_CASE("review: h1 response trailers do not leak into the next exchange", "[review][regression]") {
    using Session = detail::Http1ClientSession<MockTransport>;
    asio::io_context ctx;
    auto pump = [&ctx] { ctx.restart(); ctx.run_for(std::chrono::milliseconds(500)); };
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    auto session = std::make_shared<Session>(transport, "example.com", EngineLimits{});

    auto read_to_end = [&pump](const std::shared_ptr<detail::ClientStream> &stream) {
        auto done = std::make_shared<bool>(false);
        auto state = std::make_shared<std::function<void()>>();
        std::weak_ptr<std::function<void()>> weak = state;
        *state = [stream, done, weak] {
            stream->async_read([done, weak](error_code ec, ReadResult result) {
                if (ec || result.eof) {
                    *done = true;
                    return;
                }
                if (auto keep = weak.lock())
                    (*keep)();
            });
        };
        (*state)();
        for (int i = 0; i < 20 && !*done; ++i)
            pump();
        return *done;
    };

    auto open_get = [&](const char *path) {
        auto request = std::make_shared<Request>(Version::Http11, ctx.get_executor());
        request->set_method(Method::Get);
        request->set_target(path);
        std::shared_ptr<detail::ClientStream> stream;
        session->open_stream(request, [&stream](error_code ec, std::shared_ptr<detail::ClientStream> s) {
            if (!ec)
                stream = std::move(s);
        });
        pump();
        return stream;
    };

    auto stream_a = open_get("/a");
    REQUIRE(stream_a);
    transport->push("HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n"
                    "2\r\nhi\r\n0\r\nx-first: 1\r\n\r\n");
    bool head_a = false;
    stream_a->async_read_head([&head_a](error_code ec, detail::ResponseHead) { head_a = !ec; });
    pump();
    REQUIRE(head_a);
    REQUIRE(read_to_end(stream_a));
    CHECK(stream_a->response_trailers().contains("x-first"));

    auto stream_b = open_get("/b");
    REQUIRE(stream_b);
    transport->push("HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n"
                    "2\r\nyo\r\n0\r\n\r\n");
    bool head_b = false;
    stream_b->async_read_head([&head_b](error_code ec, detail::ResponseHead) { head_b = !ec; });
    pump();
    REQUIRE(head_b);
    REQUIRE(read_to_end(stream_b));
    CHECK_FALSE(stream_b->response_trailers().contains("x-first"));
}

// A recording inner stream: the decompression decorator must forward the
// caller's associated cancellation slot to it, or Stream's head/body deadlines
// can never abort the inner read.
namespace {
class RecordingClientStream final : public detail::ClientStream {
  public:
    explicit RecordingClientStream(asio::any_io_executor ex) : m_executor(std::move(ex)) {}

    void write(std::string, WriteCompletion done) override { std::move(done)(error_code{}); }
    void finish(std::string, WriteCompletion done) override { std::move(done)(error_code{}); }
    void read(detail::ReadCompletion done) override {
        read_slot = asio::get_associated_cancellation_slot(done);
        std::move(done)(error_code{}, ReadResult::end());
    }
    void cancel(WriteCompletion done) override { std::move(done)(error_code{}); }
    void await_head(WriteCompletion done) override {
        head_slot = asio::get_associated_cancellation_slot(done);
        detail::ResponseHead head;
        head.status = 200;
        head.version = Version::Http11;
        set_head(std::move(head));
        std::move(done)(error_code{});
    }
    bool finished() const override { return true; }
    Version version() const override { return Version::Http11; }
    std::uint32_t id() const override { return 0; }
    asio::any_io_executor head_executor() const override { return m_executor; }

    asio::cancellation_slot head_slot;
    asio::cancellation_slot read_slot;

  private:
    asio::any_io_executor m_executor;
};
} // namespace

TEST_CASE("review: decompression forwards the caller's cancellation slot", "[review][regression]") {
    asio::io_context ctx;
    auto inner = std::make_shared<RecordingClientStream>(ctx.get_executor());
    detail::ResponseDecoding policy; // decoding disabled: only forwarding is tested
    auto stream = std::make_shared<detail::DecompressingClientStream>(inner, policy);

    asio::cancellation_signal signal;
    bool completed = false;
    stream->async_read(asio::bind_cancellation_slot(signal.slot(),
                                                    [&completed](error_code, ReadResult) { completed = true; }));
    ctx.restart();
    ctx.run_for(std::chrono::milliseconds(200));

    REQUIRE(completed);
    CHECK(inner->head_slot.is_connected());
    CHECK(inner->read_slot.is_connected());
}

namespace {
void append_h2_frame(std::string &out, codec::H2FrameType type, std::uint8_t flags, std::uint32_t stream_id,
                     std::string_view payload) {
    codec::serialize_frame_header(out, static_cast<std::uint32_t>(payload.size()),
                                  static_cast<std::uint8_t>(type), flags, stream_id);
    out.append(payload);
}
} // namespace

// A peer may send the complete response and then close the connection before
// the application reads it. The bytes already parsed into the stream's queue
// must still be delivered, not replaced by a bare EOF.
TEST_CASE("review: h2 delivers a buffered body after the connection closes", "[review][regression]") {
    using Session = detail::Http2ClientSession<MockTransport>;
    asio::io_context ctx;
    auto pump = [&ctx] { ctx.restart(); ctx.run_for(std::chrono::milliseconds(200)); };
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    transport->hold_open();

    std::string settings;
    append_h2_frame(settings, codec::H2FrameType::Settings, 0, 0, {});
    transport->push(std::move(settings));

    detail::ClientTarget target;
    target.host = "example.com";
    target.port = 80;
    auto session = std::make_shared<Session>(transport, target, EngineLimits{}, std::chrono::milliseconds{0});
    bool started = false;
    session->start([&started](error_code ec) { started = !ec; });
    pump();
    REQUIRE(started);

    auto request = std::make_shared<Request>(Version::Http11, ctx.get_executor());
    request->set_method(Method::Get);
    request->set_target("/x");
    std::shared_ptr<detail::ClientStream> stream;
    session->open_stream(request, [&stream](error_code ec, std::shared_ptr<detail::ClientStream> s) {
        if (!ec)
            stream = std::move(s);
    });
    pump();
    REQUIRE(stream);

    std::string response;
    {
        std::string block;
        codec::hpack_append_indexed(block, 8); // :status 200
        append_h2_frame(response, codec::H2FrameType::Headers, codec::H2_FLAG_END_HEADERS, 1, block);
        append_h2_frame(response, codec::H2FrameType::Data, codec::H2_FLAG_END_STREAM, 1, "hello");
    }
    transport->push(std::move(response));
    pump(); // parse HEADERS + DATA into the stream queue
    transport->release();
    pump(); // the next read reports EOF; the session tears down

    std::string body;
    bool finished = false;
    auto state = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weak = state;
    *state = [stream, &body, &finished, weak] {
        stream->async_read([&body, &finished, weak](error_code ec, ReadResult result) {
            if (ec || result.eof) {
                finished = true;
                return;
            }
            body += result.data;
            if (auto keep = weak.lock())
                (*keep)();
        });
    };
    (*state)();
    for (int i = 0; i < 20 && !finished; ++i)
        pump();

    CHECK(body == "hello");
}

// Go's automatic 100-continue fallback: if the peer does not answer the probe,
// the body is sent anyway. The probe timeout is a cancellation of that one
// read, not a fatal exchange error.
TEST_CASE("review: auto Expect-continue timeout still sends the body", "[review][regression]") {
    using Session = detail::Http1ClientSession<MockTransport>;
    asio::io_context ctx;
    auto pump = [&ctx] { ctx.restart(); ctx.run_for(std::chrono::milliseconds(200)); };
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    transport->hold_open();
    auto session = std::make_shared<Session>(transport, "example.com", EngineLimits{});

    auto request = std::make_shared<Request>(Version::Http11, ctx.get_executor());
    request->set_method(Method::Post);
    request->set_target("/upload");
    request->mutable_headers().add("expect", "100-continue");
    request->set_stream_body(true);
    request->set_content_length(7);

    std::shared_ptr<detail::ClientStream> inner;
    session->open_stream(request, [&inner](error_code ec, std::shared_ptr<detail::ClientStream> s) {
        if (!ec)
            inner = std::move(s);
    });
    pump();
    REQUIRE(inner);

    Stream stream{inner, std::chrono::milliseconds{0}, std::chrono::milliseconds{0}, true,
                  std::chrono::milliseconds{50}};
    std::optional<error_code> write_ec;
    stream.async_write("payload", [&write_ec](error_code ec) { write_ec = ec; });
    for (int i = 0; i < 20 && !write_ec; ++i)
        pump();

    REQUIRE(write_ec.has_value());
    CHECK_FALSE(*write_ec);
    CHECK(transport->written().find("payload") != std::string::npos);
}

// A reader parked on a stream must observe the stream's terminal state
// (stream_reset here), not a generic session_closed after the entry is erased.
TEST_CASE("review: a parked h2 reader keeps the stream's terminal error", "[review][regression]") {
    using Session = detail::Http2ClientSession<MockTransport>;
    asio::io_context ctx;
    auto pump = [&ctx] { ctx.restart(); ctx.run_for(std::chrono::milliseconds(200)); };
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    transport->hold_open();

    std::string settings;
    append_h2_frame(settings, codec::H2FrameType::Settings, 0, 0, {});
    transport->push(std::move(settings));

    detail::ClientTarget target;
    target.host = "example.com";
    target.port = 80;
    auto session = std::make_shared<Session>(transport, target, EngineLimits{}, std::chrono::milliseconds{0});
    bool started = false;
    session->start([&started](error_code ec) { started = !ec; });
    pump();
    REQUIRE(started);

    auto request = std::make_shared<Request>(Version::Http11, ctx.get_executor());
    request->set_method(Method::Get);
    request->set_target("/x");
    std::shared_ptr<detail::ClientStream> stream;
    session->open_stream(request, [&stream](error_code ec, std::shared_ptr<detail::ClientStream> s) {
        if (!ec)
            stream = std::move(s);
    });
    pump();
    REQUIRE(stream);

    // Response head only (no END_STREAM), so the body reader parks.
    std::string response;
    {
        std::string block;
        codec::hpack_append_indexed(block, 8); // :status 200
        append_h2_frame(response, codec::H2FrameType::Headers, codec::H2_FLAG_END_HEADERS, 1, block);
    }
    transport->push(std::move(response));
    pump();

    std::optional<error_code> read_ec;
    stream->async_read([&read_ec](error_code ec, ReadResult) { read_ec = ec; });
    pump();
    REQUIRE_FALSE(read_ec.has_value()); // parked on the empty body queue

    // The peer resets the stream.
    std::string rst;
    codec::append_u32(rst, 8); // CANCEL
    std::string frame;
    append_h2_frame(frame, codec::H2FrameType::RstStream, 0, 1, rst);
    transport->push(std::move(frame));
    pump();

    REQUIRE(read_ec.has_value());
    CHECK(*read_ec == make_error_code(client_errc::stream_reset));
}

// RFC 6265 §5.1.3: domain matching is case-insensitive.
TEST_CASE("review: cookie domain matching is case-insensitive", "[review][regression]") {
    CookieJar jar;
    Headers headers;
    headers.add_lower("set-cookie", "sid=1; Domain=example.com; Path=/");
    jar.store("http://App.Example.COM/", headers);
    CHECK(jar.cookie_header("http://App.Example.COM/") == "sid=1");
}

// RFC 6265 §5.4: expired cookies are filtered when sending, not only when
// storing (the jar can outlive Max-Age between stores).
TEST_CASE("review: an expired cookie is not sent", "[review][regression]") {
    CookieJar jar;
    Headers headers;
    headers.add_lower("set-cookie", "sid=1; Max-Age=1; Path=/");
    jar.store("http://example.com/", headers);
    REQUIRE(jar.cookie_header("http://example.com/") == "sid=1");
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    CHECK(jar.cookie_header("http://example.com/").empty());
}

// RFC 3986 §3.2.2: IPv6 literals must be bracketed. A bare one is a bad URL,
// not a host named "fe80:" with port 1.
TEST_CASE("review: a bare IPv6 authority is rejected", "[review][regression]") {
    CHECK_FALSE(detail::parse_url("http://fe80::1/").has_value());
    CHECK_FALSE(detail::parse_url("http://::1/").has_value());
    CHECK(detail::parse_url("http://[fe80::1]/").has_value());
}

TEST_CASE("middleware: callback form is accepted by make_middleware", "[middleware][review]") {
    asio::io_context ctx;
    auto req = std::make_shared<Request>(Version::Http11, ctx.get_executor());
    auto sink = std::make_shared<FakeResponseSink>(ctx.get_executor());
    auto res = std::make_shared<ResponseWriter>(sink);
    std::vector<std::string> order;

    auto mw = make_middleware([&order](RequestPtr req, ResponsePtr res, SslHandle, Next next,
                                       HandlerCompletion done) {
        order.push_back("before");
        next(std::move(req), std::move(res), SslHandle{},
             HandlerCompletion([&order, done = std::move(done)]() mutable {
                 order.push_back("after");
                 std::move(done)();
             }));
    });
    Next terminal = [&order](RequestPtr, ResponsePtr, SslHandle, HandlerCompletion done) {
        order.push_back("next");
        std::move(done)();
    };
    bool finished = false;
    mw(req, res, SslHandle{}, terminal, HandlerCompletion([&finished] { finished = true; }));
    ctx.run_for(std::chrono::milliseconds(200));

    CHECK(order == std::vector<std::string>{"before", "next", "after"});
    CHECK(finished);
}

#if SIMPLE_HTTP_CPP17_CORO_HANDLERS
TEST_CASE("middleware: coroutine form is accepted by make_middleware", "[middleware][review]") {
    asio::io_context ctx;
    auto req = std::make_shared<Request>(Version::Http11, ctx.get_executor());
    auto sink = std::make_shared<FakeResponseSink>(ctx.get_executor());
    auto res = std::make_shared<ResponseWriter>(sink);
    std::vector<std::string> order;

    auto mw = make_middleware([&order](RequestPtr req, ResponsePtr res, SslHandle, CoroNext next) -> asio::awaitable<void> {
        order.push_back("before");
        co_await next(std::move(req), std::move(res), SslHandle{});
        order.push_back("after");
    });
    Next terminal = [&order](RequestPtr, ResponsePtr, SslHandle, HandlerCompletion done) {
        order.push_back("next");
        std::move(done)();
    };
    bool finished = false;
    mw(req, res, SslHandle{}, terminal, HandlerCompletion([&finished] { finished = true; }));
    ctx.restart();
    ctx.run_for(std::chrono::milliseconds(500));

    CHECK(order == std::vector<std::string>{"before", "next", "after"});
    CHECK(finished);
}
#endif

// An asynchronous ResolveFn may complete on its own thread. The completion must
// still run on the caller's executor, or Client's cached-session state and the
// dial/socket setup race with the connection executor.
TEST_CASE("review: async ResolveFn completion is marshalled to the caller executor", "[review][regression]") {
    asio::io_context ctx;
    auto ex = ctx.get_executor();
    const auto caller_thread = std::this_thread::get_id();

    std::thread worker;
    ResolveFn hook = [&worker](std::string, std::string, ResolveCompletion done) {
        worker = std::thread([done = std::move(done)]() mutable {
            std::vector<asio::ip::tcp::endpoint> endpoints{
                asio::ip::tcp::endpoint{asio::ip::make_address("192.0.2.1"), 80}};
            std::move(done)(error_code{}, std::move(endpoints));
        });
    };

    std::optional<std::thread::id> completed_on;
    async_resolve_endpoints(hook, "example.com", 80, ex,
                            [&completed_on](error_code, std::vector<asio::ip::tcp::endpoint>) {
                                completed_on = std::this_thread::get_id();
                            });

    ctx.run_for(std::chrono::seconds(2));
    if (worker.joinable())
        worker.join();
    ctx.restart();
    ctx.run_for(std::chrono::milliseconds(200));

    REQUIRE(completed_on.has_value());
    CHECK(*completed_on == caller_thread);
}
