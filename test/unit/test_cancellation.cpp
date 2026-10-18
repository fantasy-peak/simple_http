// Request-scoped cancellation (proto/request.h) and the deadline middleware
// built on it (middleware::timeout): the Go r.Context() / axum CancellationToken
// shape, adapted to asio coroutines.

#include <boost/asio/experimental/awaitable_operators.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <string>

#include "simple_http.h"
#include "test_support.h"

using namespace simple_http;
using namespace simple_http::test;

namespace {

RequestPtr make_request(asio::io_context &ctx, std::string path = "/") {
    auto req = std::make_shared<Request>(Version::Http11, ctx.get_executor(), asio::ip::tcp::endpoint{});
    req->set_target(std::move(path));
    req->body().finish();
    return req;
}

// Spawns a coroutine that cancels `req` after `delay`, so a test can drive the
// signal from "another coroutine on the same executor".
void cancel_after(asio::io_context &ctx, RequestPtr req, std::chrono::milliseconds delay) {
    asio::co_spawn(
        ctx,
        [req, delay]() -> asio::awaitable<void> {
            asio::steady_timer t{co_await asio::this_coro::executor};
            t.expires_after(delay);
            co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
            req->cancel();
        },
        asio::detached);
}

std::shared_ptr<FakeResponseSink> run_chain(asio::io_context &ctx, Router &router, RequestPtr req) {
    auto writer = std::make_shared<FakeResponseSink>();
    auto res = std::make_shared<ResponseWriter>(writer);
    REQUIRE(run_on(ctx, router.dispatch(req, res, std::nullopt)));
    return writer;
}

} // namespace

TEST_CASE("request: wait_cancelled returns at once when already cancelled", "[cancellation]") {
    asio::io_context ctx;
    auto req = make_request(ctx);
    req->cancel();

    bool woke = false;
    REQUIRE(run_on(ctx, [&]() -> asio::awaitable<void> {
        co_await req->wait_cancelled();
        woke = true;
    }()));
    CHECK(woke);
    CHECK(req->cancelled());
    CHECK(req->cancel_token().stop_requested());
}

TEST_CASE("request: wait_cancelled wakes on cancel from another coroutine", "[cancellation]") {
    asio::io_context ctx;
    auto req = make_request(ctx);

    bool woke = false;
    cancel_after(ctx, req, std::chrono::milliseconds(20));
    REQUIRE(run_on(ctx, [&]() -> asio::awaitable<void> {
        co_await req->wait_cancelled();
        woke = true;
    }()));
    CHECK(woke);
    CHECK(req->cancelled());
}

TEST_CASE("request: wait_cancelled composes with a timer via operator||", "[cancellation]") {
    using namespace asio::experimental::awaitable_operators;
    asio::io_context ctx;
    auto req = make_request(ctx);

    bool cancelled_won = false;
    cancel_after(ctx, req, std::chrono::milliseconds(20));
    REQUIRE(run_on(ctx, [&]() -> asio::awaitable<void> {
        asio::steady_timer slow{co_await asio::this_coro::executor};
        slow.expires_after(std::chrono::seconds(30));
        auto outcome = co_await (slow.async_wait(asio::as_tuple(asio::use_awaitable)) || req->wait_cancelled());
        cancelled_won = outcome.index() == 1; // the token (second operand) won
    }()));
    CHECK(cancelled_won);
    CHECK(req->cancelled());
}

TEST_CASE("middleware: timeout answers 504 when the handler overruns", "[cancellation]") {
    Router router;
    router.use(middleware::timeout(std::chrono::milliseconds(30)));
    router.route({Method::Get}, "/slow", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        asio::steady_timer t{co_await asio::this_coro::executor};
        t.expires_after(std::chrono::seconds(5));
        co_await t.async_wait(asio::use_awaitable); // the deadline must cancel this
        co_await res->status(200).send("too late");
    });

    asio::io_context ctx;
    auto writer = run_chain(ctx, router, make_request(ctx, "/slow"));
    CHECK(writer->last_status == status::gateway_timeout);
    CHECK(writer->last_body == "Gateway Timeout");
    // Exactly one reply: the handler's aborted await must NOT be turned into a
    // 500 by invoke_handler before the middleware answers (that would be a second
    // send — response-splitting on HTTP/1.1).
    CHECK(writer->calls == 1);
}

TEST_CASE("middleware: timeout leaves a handler that finishes in time alone", "[cancellation]") {
    Router router;
    router.use(middleware::timeout(std::chrono::seconds(5)));
    router.route({Method::Get}, "/fast",
                 [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(200).send("ok"); });

    asio::io_context ctx;
    auto writer = run_chain(ctx, router, make_request(ctx, "/fast"));
    CHECK(writer->last_status == 200);
    CHECK(writer->last_body == "ok");
}

TEST_CASE("middleware: timeout does not kill a stream that keeps producing", "[cancellation]") {
    Router router;
    router.use(middleware::timeout(std::chrono::milliseconds(50)));
    router.route({Method::Get}, "/stream", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).content_type("text/plain").begin();
        // Ten chunks, 20 ms apart: ~200 ms total, four times the deadline, but
        // never idle for more than 20 ms — the stream must survive.
        for (int i = 0; i < 10; ++i) {
            asio::steady_timer t{co_await asio::this_coro::executor};
            t.expires_after(std::chrono::milliseconds(20));
            co_await t.async_wait(asio::use_awaitable);
            co_await res->write("x");
        }
        co_await res->finish();
    });

    asio::io_context ctx;
    auto writer = run_chain(ctx, router, make_request(ctx, "/stream"));
    CHECK(writer->begun);
    CHECK(writer->finished);
    CHECK(writer->chunks.size() == 11); // 10 writes + the final chunk
}

TEST_CASE("middleware: timeout aborts a stream that goes silent", "[cancellation]") {
    Router router;
    router.use(middleware::timeout(std::chrono::milliseconds(50)));
    router.route({Method::Get}, "/stall", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).content_type("text/plain").begin();
        co_await res->write("first");
        asio::steady_timer t{co_await asio::this_coro::executor};
        t.expires_after(std::chrono::seconds(5)); // then silence
        co_await t.async_wait(asio::use_awaitable);
        co_await res->finish();
    });

    asio::io_context ctx;
    auto writer = run_chain(ctx, router, make_request(ctx, "/stall"));
    CHECK(writer->begun);
    CHECK_FALSE(writer->finished); // never completed
    CHECK_FALSE(writer->open);     // aborted with close(), not a 504
}

TEST_CASE("middleware: timeout fires the token for work the handler left running", "[cancellation]") {
    Router router;
    router.use(middleware::timeout(std::chrono::milliseconds(30)));

    auto detached_saw_cancel = std::make_shared<bool>(false);
    router.route({Method::Get}, "/detached",
                 [detached_saw_cancel](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
                     auto exec = co_await asio::this_coro::executor;
                     // A coroutine the handler spawns and does not join is not part of the
                     // chain `operator||` cancels — the token is how it learns to stop.
                     asio::co_spawn(
                         exec,
                         [req, detached_saw_cancel]() -> asio::awaitable<void> {
                             co_await req->wait_cancelled();
                             *detached_saw_cancel = true;
                         },
                         asio::detached);
                     asio::steady_timer t{exec};
                     t.expires_after(std::chrono::seconds(5));
                     co_await t.async_wait(asio::use_awaitable); // the deadline must cancel this
                     co_await res->status(200).send("too late");
                 });

    asio::io_context ctx;
    auto writer = run_chain(ctx, router, make_request(ctx, "/detached"));
    CHECK(writer->last_status == status::gateway_timeout);
    CHECK(*detached_saw_cancel);
}

TEST_CASE("response: started() flips once the head goes out", "[cancellation]") {
    auto sink = std::make_shared<FakeResponseSink>();
    ResponseWriter res{sink};
    CHECK_FALSE(res.started());
    REQUIRE(run([&]() -> asio::awaitable<void> { co_await res.send("x"); }()));
    CHECK(res.started());
}
