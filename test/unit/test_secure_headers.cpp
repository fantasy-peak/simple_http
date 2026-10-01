// handler/secure_headers.h: the defensive security-headers baseline — written
// before the chain so the handler's send() carries them; handlers override
// with replace_header().

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

std::shared_ptr<FakeResponseSink> dispatch(asio::io_context &ctx, Router &router, RequestPtr req,
                                             SslHandle ssl = std::nullopt) {
    auto writer = std::make_shared<FakeResponseSink>();
    auto res = std::make_shared<ResponseWriter>(writer);
    REQUIRE(run_on(ctx, router.dispatch(req, res, ssl)));
    return writer;
}

} // namespace

TEST_CASE("secure headers: the defensive baseline rides every response", "[secure_headers]") {
    Router router;
    router.use(middleware::secure_headers());
    router.route(any_methods, "/ok",
                 [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(200).send("ok"); });

    asio::io_context ctx;
    auto writer = dispatch(ctx, router, make_request(ctx, "/ok"));

    CHECK(writer->header(field::x_frame_options) == "DENY");
    CHECK(writer->header(field::x_content_type_options) == "nosniff");
    CHECK(writer->header(field::referrer_policy) == "strict-origin-when-cross-origin");
    // Off unless configured: CSP is a site policy, HSTS is off, XSS is off.
    CHECK_FALSE(writer->has_header(field::content_security_policy));
    CHECK_FALSE(writer->has_header(field::strict_transport_security));
    CHECK_FALSE(writer->has_header(field::x_xss_protection));
}

TEST_CASE("secure headers: the router's own 404/405 answers get them too", "[secure_headers]") {
    Router router;
    router.use(middleware::secure_headers());
    router.route({Method::Get}, "/only",
                 [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(200).send("ok"); });

    asio::io_context ctx;
    {
        auto writer = dispatch(ctx, router, make_request(ctx, "/missing"));
        CHECK(writer->last_status == 404);
        CHECK(writer->header(field::x_frame_options) == "DENY");
    }
    {
        auto writer = dispatch(ctx, router, make_request(ctx, "/only", Method::Post));
        CHECK(writer->last_status == 405);
        CHECK(writer->header(field::x_frame_options) == "DENY");
    }
}

TEST_CASE("secure headers: HSTS is written only over TLS and only when configured", "[secure_headers]") {
    Router router;
    middleware::SecureHeadersConfig cfg;
    cfg.hsts_max_age = std::chrono::seconds{31536000};
    cfg.hsts_include_subdomains = true;
    cfg.hsts_preload = true;
    router.use(middleware::secure_headers(cfg));
    router.route(any_methods, "/ok",
                 [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(200).send("ok"); });

    asio::io_context ctx;
    {
        // Plaintext: no HSTS (a browser would ignore it after an insecure hop).
        auto writer = dispatch(ctx, router, make_request(ctx, "/ok"));
        CHECK_FALSE(writer->has_header(field::strict_transport_security));
    }
    {
        // TLS: the full policy. The ssl handle is only probed for presence.
        auto writer = dispatch(ctx, router, make_request(ctx, "/ok"), SslHandle{reinterpret_cast<SSL *>(1)});
        CHECK(writer->header(field::strict_transport_security) == "max-age=31536000; includeSubDomains; preload");
    }
}

TEST_CASE("secure headers: a configured policy is emitted, and a handler overrides with replace_header",
          "[secure_headers]") {
    Router router;
    middleware::SecureHeadersConfig cfg;
    cfg.content_security_policy = "default-src 'self'";
    cfg.frame_options = "SAMEORIGIN";       // a site that legitimately embeds itself
    cfg.x_xss_protection = "1; mode=block"; // e.g. legacy compliance
    router.use(middleware::secure_headers(cfg));
    router.route(any_methods, "/override", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        // A handler that needs a different value replaces the default in place,
        // so the wire sees exactly one X-Frame-Options.
        res->replace_header(field::x_frame_options, "DENY");
        co_await res->status(200).send("ok");
    });

    asio::io_context ctx;
    auto writer = dispatch(ctx, router, make_request(ctx, "/override"));
    CHECK(writer->header(field::content_security_policy) == "default-src 'self'");
    CHECK(writer->header(field::x_xss_protection) == "1; mode=block");
    CHECK(writer->header(field::x_frame_options) == "DENY"); // replaced, not duplicated
    CHECK(writer->last_headers.count(field::x_frame_options) == 1);
}