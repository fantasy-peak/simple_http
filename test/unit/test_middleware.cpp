// handler/builtin_middleware.h: the out-of-the-box middleware — request id,
// access log, recovery, basic auth.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>

#include "simple_http.h"
#include "test_support.h"

using namespace simple_http;
using namespace simple_http::test;

namespace {

RequestPtr make_request(asio::io_context &ctx, std::string path, Method method = Method::Get) {
    auto req = std::make_shared<Request>(Version::Http11, ctx.get_executor(), asio::ip::tcp::endpoint{});
    req->set_target(std::move(path));
    req->set_method(method);
    req->body().finish();
    return req;
}

std::shared_ptr<FakeResponseSink> dispatch(asio::io_context &ctx, Router &router, RequestPtr req) {
    auto writer = std::make_shared<FakeResponseSink>();
    auto res = std::make_shared<ResponseWriter>(writer);
    REQUIRE(run_on(ctx, router.dispatch(req, res, std::nullopt)));
    return writer;
}

} // namespace

TEST_CASE("middleware: request_id adopts, generates, echoes and publishes", "[middleware]") {
    Router router;
    std::string seen;
    router.use(middleware::request_id());
    router.route(any_methods, "/echo", [&](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        const auto *id = req->get_state<middleware::RequestId>();
        seen = id ? id->value : "";
        co_await res->status(200).send("ok");
    });

    asio::io_context ctx;
    {
        // A client-provided id is adopted and echoed back verbatim.
        auto req = make_request(ctx, "/echo");
        req->mutable_headers().add_lower(std::string{field::x_request_id}, "client-trace-1");
        auto writer = dispatch(ctx, router, req);
        CHECK(writer->header(field::x_request_id) == "client-trace-1");
        CHECK(seen == "client-trace-1");
    }
    {
        // Without one, a fresh id is generated, echoed in the response and
        // visible to the handler through the state.
        auto writer = dispatch(ctx, router, make_request(ctx, "/echo"));
        const std::string generated = writer->header(field::x_request_id);
        CHECK(generated.size() == 32); // 16 random bytes, hex
        CHECK(seen == generated);
    }
}

TEST_CASE("middleware: access_log records after the chain with the real status", "[middleware]") {
    Router router;
    ScopedLog capture;
    router.use(middleware::access_log());
    router.route({Method::Get}, "/ok",
                 [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(202).send("ok"); });

    asio::io_context ctx;
    dispatch(ctx, router, make_request(ctx, "/ok"));
    REQUIRE(capture.records.size() == 1);
    const auto &rec = capture.records.front();
    CHECK(rec.level == LogLevel::Info);
    // method, target, status and elapsed are all in the record.
    CHECK(rec.message.find("\"GET /ok\" 202") != std::string::npos);
}

TEST_CASE("middleware: recovery turns a throwing middleware into a 500", "[middleware]") {
    Router router;
    ScopedLog capture;
    router.use(middleware::recovery());
    router.use([](RequestPtr, ResponsePtr, SslHandle, Next) -> asio::awaitable<void> {
        throw std::runtime_error("boom");
        co_return;
    });
    router.route(any_methods, "/never", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).send("never"); // unreachable: the chain throws first
    });

    asio::io_context ctx;
    auto writer = dispatch(ctx, router, make_request(ctx, "/never"));
    CHECK(writer->last_status == status::internal_server_error);
    // The failure is logged; recovery does not swallow it silently.
    bool saw_boom = false;
    for (const auto &r : capture.records) {
        if (r.message.find("boom") != std::string::npos) {
            saw_boom = true;
        }
    }
    CHECK(saw_boom);
}

TEST_CASE("middleware: basic_auth accepts the right pair and 401s the rest", "[middleware]") {
    Router router;
    bool handler_ran = false;
    router.use(middleware::basic_auth("svc", "s3cret"));
    router.route(any_methods, "/secure", [&](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        handler_ran = true;
        co_await res->status(200).send("data");
    });

    asio::io_context ctx;
    {
        // Missing credentials: 401 + WWW-Authenticate, and the route never runs.
        auto writer = dispatch(ctx, router, make_request(ctx, "/secure"));
        CHECK(writer->last_status == status::unauthorized);
        CHECK(writer->header(field::www_authenticate).find("Basic") == 0);
        CHECK_FALSE(handler_ran);
    }
    handler_ran = false;
    {
        // Correct credentials (RFC 7617: user + ":" + pass, base64).
        auto req = make_request(ctx, "/secure");
        req->mutable_headers().add_lower(std::string{field::authorization}, "Basic " + base64_encode("svc:s3cret"));
        auto writer = dispatch(ctx, router, req);
        CHECK(writer->last_status == 200);
        CHECK(handler_ran);
    }
    handler_ran = false;
    {
        // Wrong password: 401. The scheme name is matched case-insensitively.
        auto req = make_request(ctx, "/secure");
        req->mutable_headers().add_lower(std::string{field::authorization}, "basic " + base64_encode("svc:wrong"));
        auto writer = dispatch(ctx, router, req);
        CHECK(writer->last_status == status::unauthorized);
        CHECK_FALSE(handler_ran);
    }
    handler_ran = false;
    {
        // A garbage token that is not even base64 does not crash; it is a 401.
        auto req = make_request(ctx, "/secure");
        req->mutable_headers().add_lower(std::string{field::authorization}, "Basic !!!not-base64!!!");
        auto writer = dispatch(ctx, router, req);
        CHECK(writer->last_status == status::unauthorized);
        CHECK_FALSE(handler_ran);
    }
}