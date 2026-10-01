// handler/cors.h: the built-in CORS policy — preflight handling, origin
// matching, the Vary rule, and the bounds on what gets echoed back.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
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

// A handler that records that it ran, so "the preflight never reached a route"
// is observable rather than inferred from the status.
Handler flag_handler(bool &ran, std::string body = "real") {
    return make_handler([&ran, body = std::move(body)](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        ran = true;
        co_await res->status(200).send(body);
    });
}

// Dispatches one request and hands back the fake writer, so a case reads as
// request -> assertions on the recorded status, body and headers.
std::shared_ptr<FakeResponseWriter> dispatch(asio::io_context &ctx, Router &router, RequestPtr req) {
    auto writer = std::make_shared<FakeResponseWriter>();
    auto res = std::make_shared<Response>(writer);
    REQUIRE(run_on(ctx, router.dispatch(req, res, std::nullopt)));
    return writer;
}

void add_origin(const RequestPtr &req, std::string origin = "https://app.example") {
    req->mutable_headers().add_lower("origin", std::move(origin));
}

// Origin plus Access-Control-Request-Method is what makes a request a
// preflight; OPTIONS alone is not.
void make_preflight(const RequestPtr &req, std::string method = "POST") {
    req->set_method(Method::Options);
    add_origin(req);
    req->mutable_headers().add_lower("access-control-request-method", std::move(method));
}

} // namespace

// --- preflight --------------------------------------------------------------

TEST_CASE("cors: a preflight is answered 204 and never reaches the route", "[cors]") {
    Router router;
    router.cors(CorsConfig{});
    bool ran = false;
    router.route(any_methods, "/api", flag_handler(ran));

    asio::io_context ctx;
    auto req = make_request(ctx, "/api");
    make_preflight(req);
    auto writer = dispatch(ctx, router, req);

    CHECK(writer->last_status == status::no_content);
    CHECK(writer->sent_bodyless); // no framing: a body on keep-alive would desync
    CHECK(writer->last_body.empty());
    CHECK(writer->header(field::access_control_allow_origin) == "*");
    CHECK(writer->header(field::access_control_allow_methods) == "POST");
    CHECK_FALSE(writer->has_header(field::access_control_allow_credentials));
    CHECK_FALSE(ran);
}

TEST_CASE("cors: a preflight for an unregistered path is still 204", "[cors]") {
    Router router;
    router.cors(CorsConfig{});

    asio::io_context ctx;
    auto req = make_request(ctx, "/nowhere");
    make_preflight(req);
    auto writer = dispatch(ctx, router, req);

    // The whole point: routing would have answered 404.
    CHECK(writer->last_status == status::no_content);
    CHECK(writer->header(field::access_control_allow_origin) == "*");
}

TEST_CASE("cors: a plain OPTIONS is not a preflight", "[cors]") {
    Router router;
    router.cors(CorsConfig{});
    bool ran = false;
    router.route(any_methods, "/api", flag_handler(ran));

    asio::io_context ctx;
    auto req = make_request(ctx, "/api", Method::Options);
    add_origin(req); // Origin, but no Access-Control-Request-Method
    auto writer = dispatch(ctx, router, req);

    CHECK(writer->last_status == status::ok); // the route ran
    CHECK(ran);
    CHECK_FALSE(writer->sent_bodyless);
    // It is still an ordinary cross-origin request, so the headers are added.
    CHECK(writer->header(field::access_control_allow_origin) == "*");
}

// --- the Origin gate --------------------------------------------------------

TEST_CASE("cors: no Origin means no CORS headers", "[cors]") {
    Router router;
    router.cors(CorsConfig{});
    bool ran = false;
    router.route(any_methods, "/api", flag_handler(ran));

    asio::io_context ctx;
    auto writer = dispatch(ctx, router, make_request(ctx, "/api"));

    CHECK(ran);
    CHECK_FALSE(writer->has_header(field::access_control_allow_origin));
    CHECK_FALSE(writer->has_header(field::vary));
}

TEST_CASE("cors: an empty Origin is ignored", "[cors]") {
    Router router;
    router.cors(CorsConfig{});

    asio::io_context ctx;
    auto req = make_request(ctx, "/api");
    add_origin(req, "");
    auto writer = dispatch(ctx, router, req);

    CHECK_FALSE(writer->has_header(field::access_control_allow_origin));
}

// --- the actual request -----------------------------------------------------

TEST_CASE("cors: an actual request gets the headers and the route still runs", "[cors]") {
    Router router;
    router.cors(CorsConfig{});
    bool ran = false;
    router.route(any_methods, "/api", flag_handler(ran));

    asio::io_context ctx;
    auto req = make_request(ctx, "/api");
    add_origin(req);
    auto writer = dispatch(ctx, router, req);

    CHECK(writer->last_status == status::ok);
    CHECK(writer->last_body == "real");
    CHECK(ran);
    CHECK(writer->header(field::access_control_allow_origin) == "*");
    // "*" makes the reply origin-independent, so Vary would only fragment caches.
    CHECK_FALSE(writer->has_header(field::vary));
    CHECK_FALSE(writer->has_header(field::access_control_allow_credentials));
}

TEST_CASE("cors: an allowlist mirrors the origin and adds Vary", "[cors]") {
    Router router;
    router.cors(CorsConfig{.allow_origins = {"https://app.example"}});
    bool ran = false;
    router.route(any_methods, "/api", flag_handler(ran));

    asio::io_context ctx;
    auto req = make_request(ctx, "/api");
    add_origin(req);
    auto writer = dispatch(ctx, router, req);

    CHECK(ran);
    // Mirrored, not "*": the answer now depends on the request's Origin.
    CHECK(writer->header(field::access_control_allow_origin) == "https://app.example");
    CHECK(writer->header(field::vary) == "Origin");
}

TEST_CASE("cors: origin matching is case-insensitive", "[cors]") {
    Router router;
    router.cors(CorsConfig{.allow_origins = {"https://App.Example"}});

    asio::io_context ctx;
    auto req = make_request(ctx, "/api");
    add_origin(req, "https://app.example");
    auto writer = dispatch(ctx, router, req);

    CHECK(writer->has_header(field::access_control_allow_origin));
}

TEST_CASE("cors: several allowed origins each mirror the one that matched", "[cors]") {
    Router router;
    router.cors(CorsConfig{.allow_origins = {"https://a.example", "https://b.example"}});

    asio::io_context ctx;
    {
        auto req = make_request(ctx, "/api");
        add_origin(req, "https://b.example"); // the second entry, not the first
        auto writer = dispatch(ctx, router, req);
        // Still mirrored, never "*": with a list, the answer depends on which
        // origin asked, so a shared cache has to be told.
        CHECK(writer->header(field::access_control_allow_origin) == "https://b.example");
        CHECK(writer->header(field::vary) == "Origin");
    }
    {
        auto req = make_request(ctx, "/api");
        add_origin(req, "https://c.example"); // on neither list
        CHECK_FALSE(dispatch(ctx, router, req)->has_header(field::access_control_allow_origin));
    }
}

TEST_CASE("cors: an unlisted origin gets no headers but the route still runs", "[cors]") {
    Router router;
    router.cors(CorsConfig{.allow_origins = {"https://app.example"}});
    bool ran = false;
    router.route(any_methods, "/api", flag_handler(ran));

    asio::io_context ctx;
    auto req = make_request(ctx, "/api");
    add_origin(req, "https://evil.example");
    auto writer = dispatch(ctx, router, req);

    // CORS is a browser gate, not authorization — the request is served, and the
    // browser is the one that rejects the reply.
    CHECK(ran);
    CHECK(writer->last_status == status::ok);
    CHECK_FALSE(writer->has_header(field::access_control_allow_origin));
    CHECK_FALSE(writer->has_header(field::vary));
}

TEST_CASE("cors: a disallowed-origin preflight is 204 without headers", "[cors]") {
    Router router;
    router.cors(CorsConfig{.allow_origins = {"https://app.example"}});
    bool ran = false;
    router.route(any_methods, "/api", flag_handler(ran));

    asio::io_context ctx;
    auto req = make_request(ctx, "/api");
    req->set_method(Method::Options);
    add_origin(req, "https://evil.example");
    req->mutable_headers().add_lower("access-control-request-method", "POST");
    auto writer = dispatch(ctx, router, req);

    // Answered here rather than handed to routing: a 404 would read as a broken
    // server for an OPTIONS the browser made on purpose.
    CHECK(writer->last_status == status::no_content);
    CHECK_FALSE(writer->has_header(field::access_control_allow_origin));
    CHECK_FALSE(ran);
}

TEST_CASE("cors: allow_credentials mirrors the origin instead of *", "[cors]") {
    Router router;
    router.cors(CorsConfig{.allow_credentials = true});

    asio::io_context ctx;
    auto req = make_request(ctx, "/api");
    add_origin(req, "https://example.com");
    auto writer = dispatch(ctx, router, req);

    CHECK(writer->header(field::access_control_allow_origin) == "https://example.com");
    CHECK(writer->header(field::access_control_allow_credentials) == "true");
    CHECK(writer->header(field::vary) == "Origin");
}

TEST_CASE("cors: an explicit * entry with credentials still mirrors", "[cors]") {
    Router router;
    router.cors(CorsConfig{.allow_origins = {"*"}, .allow_credentials = true});

    asio::io_context ctx;
    auto req = make_request(ctx, "/api");
    add_origin(req, "https://example.com");
    auto writer = dispatch(ctx, router, req);

    CHECK(writer->header(field::access_control_allow_origin) == "https://example.com");
}

// --- what the preflight advertises ------------------------------------------

TEST_CASE("cors: allow_methods is sent verbatim when configured", "[cors]") {
    Router router;
    router.cors(CorsConfig{.allow_methods = {"GET", "POST"}});

    asio::io_context ctx;
    auto req = make_request(ctx, "/api");
    make_preflight(req, "DELETE"); // ignored: the configured list wins
    auto writer = dispatch(ctx, router, req);

    CHECK(writer->header(field::access_control_allow_methods) == "GET, POST");
}

TEST_CASE("cors: the echoed request-method is validated", "[cors]") {
    asio::io_context ctx;

    {
        Router router;
        router.cors(CorsConfig{});
        auto req = make_request(ctx, "/api");
        make_preflight(req, "POST");
        CHECK(dispatch(ctx, router, req)->header(field::access_control_allow_methods) == "POST");
    }
    {
        Router router;
        router.cors(CorsConfig{});
        auto req = make_request(ctx, "/api");
        make_preflight(req, "BAD METHOD"); // a space is not a tchar
        CHECK_FALSE(dispatch(ctx, router, req)->has_header(field::access_control_allow_methods));
    }
    {
        Router router;
        router.cors(CorsConfig{});
        auto req = make_request(ctx, "/api");
        make_preflight(req, std::string(64, 'A')); // over the 32-byte echo cap
        CHECK_FALSE(dispatch(ctx, router, req)->has_header(field::access_control_allow_methods));
    }
}

TEST_CASE("cors: an unconfigured allow_headers echoes the request's list", "[cors]") {
    Router router;
    router.cors(CorsConfig{});

    asio::io_context ctx;
    auto req = make_request(ctx, "/api");
    make_preflight(req);
    req->mutable_headers().add_lower("access-control-request-headers", "x-a, x-b");
    auto writer = dispatch(ctx, router, req);

    CHECK(writer->header(field::access_control_allow_headers) == "x-a, x-b");
}

TEST_CASE("cors: the echoed request-headers are bounded", "[cors]") {
    asio::io_context ctx;
    const std::string huge(4096, 'a');

    {
        Router router;
        router.cors(CorsConfig{});
        auto req = make_request(ctx, "/api");
        make_preflight(req);
        req->mutable_headers().add_lower("access-control-request-headers", huge);
        auto writer = dispatch(ctx, router, req);
        // Omitting it fails the preflight, the same as an unlisted name would.
        CHECK_FALSE(writer->has_header(field::access_control_allow_headers));
        CHECK(writer->last_status == status::no_content);
    }
    {
        Router router;
        router.cors(CorsConfig{.max_echoed_request_headers = 8192});
        auto req = make_request(ctx, "/api");
        make_preflight(req);
        req->mutable_headers().add_lower("access-control-request-headers", huge);
        CHECK(dispatch(ctx, router, req)->header(field::access_control_allow_headers) == huge);
    }
}

TEST_CASE("cors: a configured allow_headers list is enforced", "[cors]") {
    asio::io_context ctx;

    {
        Router router;
        router.cors(CorsConfig{.allow_headers = {"x-token"}});
        auto req = make_request(ctx, "/api");
        make_preflight(req);
        req->mutable_headers().add_lower("access-control-request-headers", "x-token");
        CHECK(dispatch(ctx, router, req)->header(field::access_control_allow_headers) == "x-token");
    }
    {
        Router router;
        router.cors(CorsConfig{.allow_headers = {"x-token"}});
        auto req = make_request(ctx, "/api");
        make_preflight(req);
        req->mutable_headers().add_lower("access-control-request-headers", "x-token, authorization");
        // One unlisted name fails the whole list: a preflight is all-or-nothing.
        CHECK_FALSE(dispatch(ctx, router, req)->has_header(field::access_control_allow_headers));
    }
}

TEST_CASE("cors: expose_headers appears on an actual response only", "[cors]") {
    Router router;
    router.cors(CorsConfig{.expose_headers = {"x-total", "x-request-id"}});

    asio::io_context ctx;
    {
        auto req = make_request(ctx, "/api");
        add_origin(req);
        auto writer = dispatch(ctx, router, req);
        CHECK(writer->header(field::access_control_expose_headers) == "x-total, x-request-id");
    }
    {
        auto req = make_request(ctx, "/api");
        make_preflight(req);
        // The preflight has no response body for the browser to read headers off.
        CHECK_FALSE(dispatch(ctx, router, req)->has_header(field::access_control_expose_headers));
    }
}

TEST_CASE("cors: max_age is sent only on a preflight, and only when set", "[cors]") {
    asio::io_context ctx;

    {
        Router router;
        router.cors(CorsConfig{});
        auto req = make_request(ctx, "/api");
        make_preflight(req);
        CHECK_FALSE(dispatch(ctx, router, req)->has_header(field::access_control_max_age));
    }
    {
        Router router;
        router.cors(CorsConfig{.max_age = std::chrono::seconds{600}});
        auto req = make_request(ctx, "/api");
        make_preflight(req);
        CHECK(dispatch(ctx, router, req)->header(field::access_control_max_age) == "600");
    }
    {
        Router router;
        router.cors(CorsConfig{.max_age = std::chrono::seconds{600}});
        auto req = make_request(ctx, "/api");
        add_origin(req);
        CHECK_FALSE(dispatch(ctx, router, req)->has_header(field::access_control_max_age));
    }
}

// --- injection mechanics ----------------------------------------------------

TEST_CASE("cors: each header is injected exactly once", "[cors]") {
    Router router;
    router.cors(CorsConfig{.allow_origins = {"https://app.example"}, .allow_credentials = true});

    asio::io_context ctx;
    auto req = make_request(ctx, "/api");
    add_origin(req);
    auto writer = dispatch(ctx, router, req);

    // Response::header appends, so a second injection would show up as a second
    // field — which browsers reject outright for Access-Control-Allow-Origin.
    CHECK(writer->last_headers.count(field::access_control_allow_origin) == 1);
    CHECK(writer->last_headers.count(field::vary) == 1);
}

TEST_CASE("cors: the headers reach a built-in 404", "[cors]") {
    Router router;
    router.cors(CorsConfig{});

    asio::io_context ctx;
    auto req = make_request(ctx, "/nowhere");
    add_origin(req);
    auto writer = dispatch(ctx, router, req);

    CHECK(writer->last_status == status::not_found);
    CHECK(writer->header(field::access_control_allow_origin) == "*");
}

TEST_CASE("cors: a subdomain wildcard accepts any number of leading labels", "[cors]") {
    Router router;
    router.cors(CorsConfig{.allow_origins = {"https://*.example.com"}});
    bool route_ran = false;
    router.route(any_methods, "/api", flag_handler(route_ran, "real"));

    asio::io_context ctx;
    {
        // A single leading label matches; the origin is mirrored (not "*") and
        // Vary: Origin is set, because the reply depends on which subdomain hit.
        auto req = make_request(ctx, "/api");
        add_origin(req, "https://app.example.com");
        auto writer = dispatch(ctx, router, req);
        CHECK(writer->header(field::access_control_allow_origin) == "https://app.example.com");
        CHECK(writer->has_header(field::vary));
        CHECK(route_ran);
    }
    route_ran = false;
    {
        // Several leading labels match too.
        auto req = make_request(ctx, "/api");
        add_origin(req, "https://a.b.example.com");
        auto writer = dispatch(ctx, router, req);
        CHECK(writer->header(field::access_control_allow_origin) == "https://a.b.example.com");
    }
    {
        // The bare domain is not a subdomain: no CORS headers, route still runs.
        auto req = make_request(ctx, "/api");
        add_origin(req, "https://example.com");
        auto writer = dispatch(ctx, router, req);
        CHECK_FALSE(writer->has_header(field::access_control_allow_origin));
        CHECK(route_ran);
    }
    {
        // A scheme outside the pattern is rejected even with a matching host.
        auto req = make_request(ctx, "/api");
        add_origin(req, "http://app.example.com");
        auto writer = dispatch(ctx, router, req);
        CHECK_FALSE(writer->has_header(field::access_control_allow_origin));
    }
}

TEST_CASE("cors: a scheme-less wildcard accepts any scheme", "[cors]") {
    Router router;
    router.cors(CorsConfig{.allow_origins = {"*.example.com"}});

    asio::io_context ctx;
    for (const char *origin : {"https://app.example.com", "http://a.b.example.com"}) {
        auto req = make_request(ctx, "/api");
        add_origin(req, origin);
        auto writer = dispatch(ctx, router, req);
        CHECK(writer->header(field::access_control_allow_origin) == origin);
    }
    {
        auto req = make_request(ctx, "/api");
        add_origin(req, "https://example.com"); // bare domain still not a subdomain
        auto writer = dispatch(ctx, router, req);
        CHECK_FALSE(writer->has_header(field::access_control_allow_origin));
    }
}

TEST_CASE("cors: allow_origin_fn is consulted and ORs with the list", "[cors]") {
    Router router;
    router.cors(CorsConfig{
        .allow_origins = {"https://listed.example"},
        .allow_origin_fn = [](std::string_view origin) { return origin.ends_with(".internal"); },
    });
    bool route_ran = false;
    router.route(any_methods, "/api", flag_handler(route_ran, "real"));

    asio::io_context ctx;
    {
        // The fn accepts an origin the list does not.
        auto req = make_request(ctx, "/api");
        add_origin(req, "https://svc.internal");
        auto writer = dispatch(ctx, router, req);
        CHECK(writer->header(field::access_control_allow_origin) == "https://svc.internal");
        CHECK(route_ran);
    }
    route_ran = false;
    {
        // A list entry still works without the fn firing.
        auto req = make_request(ctx, "/api");
        add_origin(req, "https://listed.example");
        auto writer = dispatch(ctx, router, req);
        CHECK(writer->header(field::access_control_allow_origin) == "https://listed.example");
        CHECK(route_ran);
    }
    route_ran = false;
    {
        // Neither accepts it: no CORS headers, route still runs.
        auto req = make_request(ctx, "/api");
        add_origin(req, "https://evil.example");
        auto writer = dispatch(ctx, router, req);
        CHECK_FALSE(writer->has_header(field::access_control_allow_origin));
        CHECK(route_ran);
    }
    route_ran = false;
    {
        // A preflight for an fn-accepted origin is answered with the fn's
        // decision reflected.
        auto req = make_request(ctx, "/api");
        req->set_method(Method::Options);
        req->mutable_headers().add_lower("origin", "https://svc.internal");
        req->mutable_headers().add_lower("access-control-request-method", "POST");
        auto writer = dispatch(ctx, router, req);
        CHECK(writer->last_status == status::no_content);
        CHECK(writer->header(field::access_control_allow_origin) == "https://svc.internal");
        CHECK_FALSE(route_ran);
    }
}
