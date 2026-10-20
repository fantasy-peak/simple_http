// handler/builtin_middleware.h: the out-of-the-box middleware — request id,
// access log, recovery, basic auth.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
    // The structured record carries method, path and the real status as attrs.
    CHECK(rec.message == "request");
    CHECK(rec.attr("method") == "GET");
    CHECK(rec.attr("path") == "/ok");
    CHECK(rec.attr("status") == "202");
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

TEST_CASE("middleware: real_ip trusts forwarded headers only from a trusted peer", "[middleware]") {
    asio::io_context ctx;

    auto make_from = [&](const char *peer, std::string_view name, std::string_view value) {
        auto req = std::make_shared<Request>(Version::Http11, ctx.get_executor(),
                                             asio::ip::tcp::endpoint{asio::ip::make_address(peer), 4444});
        req->set_target("/");
        req->mutable_headers().add_lower(std::string{name}, std::string{value});
        return req;
    };
    auto run_mw = [](RequestPtr req, std::vector<std::string> trusted) -> asio::awaitable<std::optional<std::string>> {
        auto mw = middleware::real_ip(std::move(trusted));
        std::optional<std::string> seen;
        Next next = [&seen](RequestPtr r, ResponsePtr, SslHandle) -> asio::awaitable<void> {
            if (auto *ip = r->get_state<middleware::ClientIp>()) {
                seen = ip->value;
            }
            co_return;
        };
        co_await mw(std::move(req), nullptr, std::nullopt, std::move(next));
        co_return seen;
    };
    auto run = [&](RequestPtr req, std::vector<std::string> trusted) -> std::optional<std::string> {
        auto out = run_on(ctx, run_mw(std::move(req), std::move(trusted)));
        REQUIRE(out.has_value());
        return std::move(*out);
    };

    const std::vector<std::string> trusted{"10.0.0.0/8"};
    // A direct, untrusted peer cannot speak for anyone else — even on loopback,
    // and even with the header a proxy would have written.
    CHECK_FALSE(run(make_from("127.0.0.1", "x-real-ip", "1.2.3.4"), trusted).has_value());
    CHECK_FALSE(run(make_from("127.0.0.1", "x-forwarded-for", "1.2.3.4"), trusted).has_value());
    // An empty trust list means "trust no proxy" (fail closed).
    CHECK_FALSE(run(make_from("127.0.0.1", "x-real-ip", "1.2.3.4"), {}).has_value());

    // From a trusted proxy, X-Real-IP wins...
    CHECK(run(make_from("10.1.2.3", "x-real-ip", "1.2.3.4"), trusted) == std::optional<std::string>{"1.2.3.4"});
    // ...otherwise XFF is walked right-to-left and the first non-proxy is the
    // client.
    CHECK(run(make_from("10.1.2.3", "x-forwarded-for", "1.2.3.4, 10.0.0.5"), trusted) ==
          std::optional<std::string>{"1.2.3.4"});
    CHECK(run(make_from("10.1.2.3", "x-forwarded-for", "1.2.3.4, 8.8.8.8"), trusted) ==
          std::optional<std::string>{"8.8.8.8"});

    {
        auto req = make_from("10.1.2.3", "x-real-ip", "9.9.9.9");
        req->mutable_headers().add_lower("x-forwarded-for", "1.2.3.4");
        CHECK(run(std::move(req), trusted) == std::optional<std::string>{"9.9.9.9"});
    }
}