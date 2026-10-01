// handler/rate_limit.h: token-bucket / per-key rate limiting — 429 +
// Retry-After, the tower-http RateLimitLayer / golang.org/x/time/rate shape.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <thread>

#include "simple_http.h"
#include "test_support.h"

using namespace simple_http;
using namespace simple_http::test;

namespace {

RequestPtr make_request(asio::io_context &ctx, std::string path) {
    auto req = std::make_shared<Request>(Version::Http11, ctx.get_executor(), asio::ip::tcp::endpoint{});
    req->set_target(std::move(path));
    req->set_method(Method::Get);
    return req;
}

std::shared_ptr<FakeResponseWriter> dispatch(asio::io_context &ctx, Router &router, RequestPtr req) {
    auto writer = std::make_shared<FakeResponseWriter>();
    auto res = std::make_shared<Response>(writer);
    REQUIRE(run_on(ctx, router.dispatch(req, res, std::nullopt)));
    return writer;
}

} // namespace

TEST_CASE("rate limit: the token bucket admits burst then refills", "[rate_limit]") {
    // 0.01 tokens/s: effectively no refill during the test, so the third
    // acquisition can only fail on the exhausted burst.
    auto bucket = middleware::make_token_bucket(/*rate=*/0.01, /*burst=*/2);
    CHECK(bucket->try_acquire());
    CHECK(bucket->try_acquire());
    CHECK_FALSE(bucket->try_acquire()); // empty: over the burst

    // A fast bucket refills as time passes: 20/s fills at least one token in
    // 120 ms, so the acquisition succeeds again.
    auto refill = middleware::make_token_bucket(/*rate=*/20, /*burst=*/1);
    CHECK(refill->try_acquire());
    CHECK_FALSE(refill->try_acquire());
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    CHECK(refill->try_acquire());
    CHECK_FALSE(refill->try_acquire()); // consumed again
}

TEST_CASE("rate limit: per-key limiter keeps independent buckets", "[rate_limit]") {
    // Burst 1 so the second acquisition of the same key must fail; a slow rate
    // keeps no refill from sneaking in between assertions.
    auto limiter = std::make_shared<middleware::RateLimiter>(0.01, 1);
    CHECK(limiter->try_acquire("alice"));
    CHECK_FALSE(limiter->try_acquire("alice"));
    CHECK(limiter->try_acquire("bob")); // a different key has its own bucket
}

TEST_CASE("rate limit: a refused request is 429 and never reaches the route", "[rate_limit]") {
    Router router;
    auto bucket = middleware::make_token_bucket(/*rate=*/0.01, /*burst=*/1);
    router.use(middleware::rate_limit(bucket));
    bool handler_ran = false;
    router.route(any_methods, "/work", [&](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        handler_ran = true;
        co_await res->status(200).send("done");
    });

    asio::io_context ctx;
    {
        // The single token is consumed by the first request.
        auto writer = dispatch(ctx, router, make_request(ctx, "/work"));
        CHECK(writer->last_status == 200);
        CHECK(handler_ran);
    }
    handler_ran = false;
    {
        // The bucket is empty: 429, Retry-After present, route not run. At
        // 0.01 tokens/s a refill takes ~100s, safely past the test window.
        auto writer = dispatch(ctx, router, make_request(ctx, "/work"));
        CHECK(writer->last_status == status::too_many_requests);
        CHECK(writer->header(field::retry_after) == "100");
        CHECK_FALSE(handler_ran);
    }
}

TEST_CASE("rate limit: per-key middleware limits each key separately", "[rate_limit]") {
    Router router;
    auto limiter = std::make_shared<middleware::RateLimiter>(0.01, 1);
    router.use(middleware::rate_limit(limiter)); // default key = peer address
    bool ran = false;
    router.route(any_methods, "/w", [&](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        ran = true;
        co_await res->status(200).send("ok");
    });

    asio::io_context ctx;
    {
        auto writer = dispatch(ctx, router, make_request(ctx, "/w"));
        CHECK(writer->last_status == 200);
    }
    {
        auto writer = dispatch(ctx, router, make_request(ctx, "/w"));
        CHECK(writer->last_status == status::too_many_requests); // same peer, budget exhausted
    }
    {
        // Requests from a different peer pass — the middleware's key_from
        // could be replaced with any per-request discriminator.
        auto req = make_request(ctx, "/w");
        req->set_state(middleware::ClientIp{"203.0.113.9"});
        auto writer = dispatch(ctx, router, req);
        CHECK(writer->last_status == 200);
        CHECK(ran);
    }
}