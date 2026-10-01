// handler/: route registration, matching order, the reverse-proxy lookups (with
// their $1-style rewrite expansion) and dispatch.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>

#include "../static_fixture.h"
#include "simple_http.h"
#include "test_support.h"

using namespace simple_http;
using namespace simple_http::test;

namespace {

RequestPtr make_request(asio::io_context &ctx, std::string path, Method method = Method::Get) {
    auto req = std::make_shared<Request>(Version::Http11, ctx.get_executor(), asio::ip::tcp::endpoint{});
    req->set_target(std::move(path));
    req->set_method(method);
    // The engines always end the request body; the reverse proxy probes it before
    // dialing, so a test must do the same or that read waits forever.
    req->body().finish();
    return req;
}

// A handler that answers with a fixed body, so dispatch's routing is
// observable.
Handler body_handler(std::string body) {
    return make_handler([body = std::move(body)](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
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

} // namespace

TEST_CASE("router: exact, regex and fallback matching", "[router]") {
    Router router;
    router.route({Method::Get}, "/exact", body_handler("exact"));
    router.route_regex({Method::Get}, "/api/.*", body_handler("regex"));
    router.fallback(body_handler("fallback"));

    asio::io_context ctx;
    auto dispatch = [&](const std::string &path) {
        auto writer = std::make_shared<FakeResponseWriter>();
        auto req = make_request(ctx, path);
        auto res = std::make_shared<Response>(writer);
        REQUIRE(run_on(ctx, router.dispatch(req, res, std::nullopt)));
        return writer;
    };

    CHECK(dispatch("/exact")->last_body == "exact");
    CHECK(dispatch("/api/users")->last_body == "regex");
    CHECK(dispatch("/nothing-here")->last_body == "fallback");
    // An exact route wins over a regex that would also match.
    CHECK(dispatch("/exact")->last_status == 200);
}

TEST_CASE("router: dispatch without a fallback answers 404", "[router]") {
    Router router;
    router.route({Method::Get}, "/only", body_handler("only"));

    asio::io_context ctx;
    auto writer = std::make_shared<FakeResponseWriter>();
    auto req = make_request(ctx, "/missing");
    auto res = std::make_shared<Response>(writer);
    REQUIRE(run_on(ctx, router.dispatch(req, res, std::nullopt)));
    CHECK(writer->last_status == 404);
    CHECK(writer->last_body.empty());
}

TEST_CASE("router: middleware short-circuits and sees the request", "[router]") {
    Router router;
    bool mw_called = false;
    router.use([&](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next) -> asio::awaitable<void> {
        mw_called = true;
        if (req->path() == "/blocked") {
            co_await res->status(403).send("denied");
            co_return; // answered here: the route must not run
        }
        co_await next(std::move(req), std::move(res), ssl);
    });
    router.route(any_methods, "/blocked", body_handler("should not run"));
    router.route(any_methods, "/allowed", body_handler("allowed"));

    asio::io_context ctx;
    {
        auto writer = std::make_shared<FakeResponseWriter>();
        auto req = make_request(ctx, "/blocked");
        auto res = std::make_shared<Response>(writer);
        REQUIRE(run_on(ctx, router.dispatch(req, res, std::nullopt)));
        CHECK(mw_called);
        CHECK(writer->last_status == 403);
        CHECK(writer->last_body == "denied");
    }
    {
        auto writer = std::make_shared<FakeResponseWriter>();
        auto req = make_request(ctx, "/allowed");
        auto res = std::make_shared<Response>(writer);
        REQUIRE(run_on(ctx, router.dispatch(req, res, std::nullopt)));
        CHECK(writer->last_body == "allowed");
    }
}

TEST_CASE("router: middleware wraps the handler — before, after, and ordering", "[router]") {
    Router router;
    std::vector<std::string> trace;
    // Outermost first: mw1 sees the request before mw2, and its after-phase
    // (the code after next() resumes) runs last. use() runs in registration
    // order.
    router.use([&](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next) -> asio::awaitable<void> {
        trace.push_back("mw1:before");
        // A copy keeps this layer's own reference, so the after-phase below can
        // still touch req/res once next() has returned.
        co_await next(req, res, ssl);
        trace.push_back("mw1:after");
    });
    router.use([&](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next) -> asio::awaitable<void> {
        trace.push_back("mw2:before");
        co_await next(std::move(req), std::move(res), ssl);
        trace.push_back("mw2:after");
    });
    router.route(any_methods, "/chain", [&](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        trace.push_back("handler");
        co_await res->status(200).send("ok");
    });

    asio::io_context ctx;
    auto writer = std::make_shared<FakeResponseWriter>();
    auto req = make_request(ctx, "/chain");
    auto res = std::make_shared<Response>(writer);
    REQUIRE(run_on(ctx, router.dispatch(req, res, std::nullopt)));
    CHECK(writer->last_status == 200);
    CHECK(writer->last_body == "ok");
    // A middleware chain nests: before phases in registration order, then the
    // handler, then the after phases unwound in reverse.
    CHECK(trace == std::vector<std::string>{"mw1:before", "mw2:before", "handler", "mw2:after", "mw1:after"});
}

TEST_CASE("router: middleware short-circuits by not calling next", "[router]") {
    Router router;
    std::vector<std::string> trace;
    router.use([&](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next) -> asio::awaitable<void> {
        trace.push_back("gate:before");
        if (req->path() == "/blocked") {
            co_await res->status(403).send("denied");
            co_return; // answered here: the rest of the chain must not run
        }
        co_await next(std::move(req), std::move(res), ssl);
        trace.push_back("gate:after");
    });
    router.use([&](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next) -> asio::awaitable<void> {
        trace.push_back("inner");
        co_await next(std::move(req), std::move(res), ssl);
    });
    router.route(any_methods, "/blocked", body_handler("should not run"));
    router.route(any_methods, "/allowed", body_handler("allowed"));

    asio::io_context ctx;
    {
        auto writer = std::make_shared<FakeResponseWriter>();
        auto req = make_request(ctx, "/blocked");
        auto res = std::make_shared<Response>(writer);
        REQUIRE(run_on(ctx, router.dispatch(req, res, std::nullopt)));
        CHECK(writer->last_status == 403);
        CHECK(writer->last_body == "denied");
        CHECK(trace == std::vector<std::string>{"gate:before"});
    }
    trace.clear();
    {
        auto writer = std::make_shared<FakeResponseWriter>();
        auto req = make_request(ctx, "/allowed");
        auto res = std::make_shared<Response>(writer);
        REQUIRE(run_on(ctx, router.dispatch(req, res, std::nullopt)));
        CHECK(writer->last_body == "allowed");
        CHECK(trace == std::vector<std::string>{"gate:before", "inner", "gate:after"});
    }
}

TEST_CASE("router: the after-phase observes the response", "[router]") {
    Router router;
    int seen_status = 0;
    router.use([&](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next) -> asio::awaitable<void> {
        co_await next(req, res, ssl); // copy: keep our reference for the after-phase
        seen_status = res->status();
    });
    router.route(any_methods, "/created",
                 [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(201).send("made"); });

    asio::io_context ctx;
    {
        auto writer = std::make_shared<FakeResponseWriter>();
        auto req = make_request(ctx, "/created");
        auto res = std::make_shared<Response>(writer);
        REQUIRE(run_on(ctx, router.dispatch(req, res, std::nullopt)));
        CHECK(seen_status == 201);
    }
    seen_status = 0;
    {
        // A request nothing serves answers the router's built-in 404, which the
        // after-phase sees too — a logging middleware reports real answers, not
        // "the handler did not run".
        auto writer = std::make_shared<FakeResponseWriter>();
        auto req = make_request(ctx, "/nothing-here");
        auto res = std::make_shared<Response>(writer);
        REQUIRE(run_on(ctx, router.dispatch(req, res, std::nullopt)));
        CHECK(writer->last_status == 404);
        CHECK(seen_status == 404);
    }
}

TEST_CASE("router: middleware state flows to the handler", "[router]") {
    struct AuthPrincipal {
        std::string name;
        bool admin;
    };
    Router router;
    // Two middlewares set the same type: last set wins, so the handler sees the
    // inner middleware's principal.
    router.use([&](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next) -> asio::awaitable<void> {
        req->set_state(AuthPrincipal{"outer", false});
        co_await next(std::move(req), std::move(res), ssl);
    });
    router.use([&](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next) -> asio::awaitable<void> {
        req->set_state(AuthPrincipal{"alice", true});
        co_await next(std::move(req), std::move(res), ssl);
    });
    router.route(any_methods, "/who", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        const AuthPrincipal *user = req->get_state<AuthPrincipal>();
        co_await res->status(200).send(user ? user->name : "anonymous");
    });
    // A different type than the one set is not found.
    router.route(any_methods, "/none", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).send(req->get_state<std::string>() ? "string" : "no-string");
    });

    asio::io_context ctx;
    {
        auto writer = std::make_shared<FakeResponseWriter>();
        auto req = make_request(ctx, "/who");
        auto res = std::make_shared<Response>(writer);
        REQUIRE(run_on(ctx, router.dispatch(req, res, std::nullopt)));
        CHECK(writer->last_body == "alice");
    }
    {
        auto writer = std::make_shared<FakeResponseWriter>();
        auto req = make_request(ctx, "/none");
        auto res = std::make_shared<Response>(writer);
        REQUIRE(run_on(ctx, router.dispatch(req, res, std::nullopt)));
        CHECK(writer->last_body == "no-string");
    }
}

TEST_CASE("router: cors is the outermost middleware whatever the registration order", "[router]") {
    Router router;
    bool user_ran = false;
    // Registered first, but cors() below must still end up ahead of it: a
    // preflight is answered before the user middleware ever sees the request.
    router.use([&](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next) -> asio::awaitable<void> {
        user_ran = true;
        co_await next(std::move(req), std::move(res), ssl);
    });
    router.cors(CorsConfig{});
    bool route_ran = false;
    router.route(any_methods, "/api", [&](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        route_ran = true;
        co_await res->status(200).send("real");
    });

    asio::io_context ctx;
    {
        // A preflight is answered 204 by the CORS middleware; neither the user
        // middleware nor the route runs.
        auto req = make_request(ctx, "/api");
        req->set_method(Method::Options);
        req->mutable_headers().add_lower("origin", "https://app.example");
        req->mutable_headers().add_lower("access-control-request-method", "POST");
        auto writer = dispatch(ctx, router, req);
        CHECK(writer->last_status == status::no_content);
        CHECK(writer->header(field::access_control_allow_origin) == "*");
        CHECK_FALSE(user_ran);
        CHECK_FALSE(route_ran);
    }
    {
        // An actual cross-origin request flows through the user middleware and
        // reaches the route with the CORS headers applied.
        auto req = make_request(ctx, "/api");
        req->mutable_headers().add_lower("origin", "https://app.example");
        auto writer = dispatch(ctx, router, req);
        CHECK(writer->last_status == 200);
        CHECK(writer->last_body == "real");
        CHECK(writer->header(field::access_control_allow_origin) == "*");
        CHECK(user_ran);
        CHECK(route_ran);
    }
}

TEST_CASE("router: compose_middleware builds a standalone chain", "[router]") {
    std::vector<std::string> trace;
    Middleware a = [&](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next) -> asio::awaitable<void> {
        trace.push_back("a:before");
        co_await next(std::move(req), std::move(res), ssl);
        trace.push_back("a:after");
    };
    Middleware b = [&](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next) -> asio::awaitable<void> {
        trace.push_back("b:before");
        co_await next(std::move(req), std::move(res), ssl);
        trace.push_back("b:after");
    };
    Next terminal = [&](RequestPtr, ResponsePtr res, SslHandle) -> asio::awaitable<void> {
        trace.push_back("terminal");
        co_await res->status(200).send("ok");
    };

    asio::io_context ctx;
    auto writer = std::make_shared<FakeResponseWriter>();
    auto req = make_request(ctx, "/x");
    auto res = std::make_shared<Response>(writer);
    auto chain = compose_middleware({std::move(a), std::move(b)}, std::move(terminal));
    REQUIRE(run_on(ctx, chain(req, res, std::nullopt)));
    CHECK(trace == std::vector<std::string>{"a:before", "b:before", "terminal", "b:after", "a:after"});
}

TEST_CASE("router: per-route middleware wraps only its own handler", "[router]") {
    Router router;
    std::vector<std::string> trace;
    router.use([&](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next) -> asio::awaitable<void> {
        trace.push_back("global");
        co_await next(std::move(req), std::move(res), ssl);
    });
    router.route({Method::Get}, "/protected",
                 {[&](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next) -> asio::awaitable<void> {
                     trace.push_back("per-route:before");
                     co_await next(std::move(req), std::move(res), ssl);
                     trace.push_back("per-route:after");
                 }},
                 [&](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                     trace.push_back("protected-handler");
                     co_await res->status(200).send("secret");
                 });
    // A route without per-route middleware is untouched: the global chain runs,
    // the per-route chain does not.
    router.route(any_methods, "/open", body_handler("open"));

    asio::io_context ctx;
    {
        auto writer = dispatch(ctx, router, make_request(ctx, "/protected"));
        CHECK(writer->last_status == 200);
        CHECK(writer->last_body == "secret");
        CHECK(trace == std::vector<std::string>{"global", "per-route:before", "protected-handler", "per-route:after"});
    }
    trace.clear();
    {
        auto writer = dispatch(ctx, router, make_request(ctx, "/open"));
        CHECK(writer->last_status == 200);
        CHECK(writer->last_body == "open");
        CHECK(trace == std::vector<std::string>{"global"});
    }
    trace.clear();
    {
        // The path exists, but the method does not: 405 is answered by the
        // router and never reaches the per-route chain (gin/echo semantics).
        auto req = make_request(ctx, "/protected", Method::Post);
        auto writer = dispatch(ctx, router, req);
        CHECK(writer->last_status == status::method_not_allowed);
        CHECK(trace == std::vector<std::string>{"global"}); // per-route chain never ran
    }
}

TEST_CASE("router: per-route middleware short-circuits its route", "[router]") {
    Router router;
    bool handler_ran = false;
    router.route(any_methods, "/admin", {[&](RequestPtr, ResponsePtr res, SslHandle, Next) -> asio::awaitable<void> {
                     co_await res->status(403).send("denied"); // never calls next()
                 }},
                 [&](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                     handler_ran = true;
                     co_await res->status(200).send("admin");
                 });
    router.route(any_methods, "/public", body_handler("public"));

    asio::io_context ctx;
    {
        auto writer = dispatch(ctx, router, make_request(ctx, "/admin"));
        CHECK(writer->last_status == 403);
        CHECK_FALSE(handler_ran);
    }
    {
        auto writer = dispatch(ctx, router, make_request(ctx, "/public"));
        CHECK(writer->last_body == "public");
    }
}

TEST_CASE("router: group applies prefix and group middleware to its routes only", "[router]") {
    Router router;
    std::vector<std::string> trace;
    router.group("/api", {[&](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next) -> asio::awaitable<void> {
                     trace.push_back("group:before");
                     co_await next(std::move(req), std::move(res), ssl);
                     trace.push_back("group:after");
                 }},
                 [&](Router &api) {
                     api.route(any_methods, "/users", [&](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                         trace.push_back("users");
                         co_await res->status(200).send("users");
                     });
                     // A nested group accumulates the prefix and its own
                     // middleware, which runs between the outer group and the handler.
                     api.group(
                         "/admin",
                         {[&](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next) -> asio::awaitable<void> {
                             trace.push_back("nested:before");
                             co_await next(std::move(req), std::move(res), ssl);
                             trace.push_back("nested:after");
                         }},
                         [&](Router &adm) {
                             adm.route(any_methods, "/kick", [&](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                                 trace.push_back("kick");
                                 co_await res->status(200).send("kicked");
                             });
                         });
                 });
    // A route outside the group does not get the prefix or the group middleware.
    router.route(any_methods, "/health", [&](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        trace.push_back("health");
        co_await res->status(200).send("ok");
    });

    asio::io_context ctx;
    {
        auto writer = dispatch(ctx, router, make_request(ctx, "/api/users"));
        CHECK(writer->last_status == 200);
        CHECK(writer->last_body == "users");
        CHECK(trace == std::vector<std::string>{"group:before", "users", "group:after"});
    }
    trace.clear();
    {
        auto writer = dispatch(ctx, router, make_request(ctx, "/api/admin/kick"));
        CHECK(writer->last_status == 200);
        CHECK(writer->last_body == "kicked");
        CHECK(trace ==
              std::vector<std::string>{"group:before", "nested:before", "kick", "nested:after", "group:after"});
    }
    trace.clear();
    {
        auto writer = dispatch(ctx, router, make_request(ctx, "/health"));
        CHECK(writer->last_body == "ok");
        // The outer group middleware ran for /api/* only; /health saw neither
        // the prefix-rebasing (it is registered raw) nor the group chain.
        CHECK(trace == std::vector<std::string>{"health"});
    }
    {
        // The group prefix is really a path prefix: /api/users misses nothing,
        // /api (bare) does.
        auto writer = dispatch(ctx, router, make_request(ctx, "/api"));
        CHECK(writer->last_status == 404);
    }
}

TEST_CASE("router: use() inside a group scopes to that group", "[router]") {
    Router router;
    std::vector<std::string> trace;
    router.group("/mq", [&](Router &mq) {
        mq.use([&](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next) -> asio::awaitable<void> {
            trace.push_back("scoped");
            co_await next(std::move(req), std::move(res), ssl);
        });
        mq.route(any_methods, "/pull", [&](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
            trace.push_back("pull");
            co_await res->status(200).send("msg");
        });
    });
    router.route(any_methods, "/plain", [&](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        trace.push_back("plain");
        co_await res->status(200).send("plain");
    });

    asio::io_context ctx;
    {
        auto writer = dispatch(ctx, router, make_request(ctx, "/mq/pull"));
        CHECK(writer->last_body == "msg");
        CHECK(trace == std::vector<std::string>{"scoped", "pull"});
    }
    trace.clear();
    {
        auto writer = dispatch(ctx, router, make_request(ctx, "/plain"));
        CHECK(writer->last_body == "plain");
        CHECK(trace == std::vector<std::string>{"plain"}); // the group middleware stayed in the group
    }
}

TEST_CASE("router: group middleware, global middleware and per-route middleware nest in order", "[router]") {
    Router router;
    std::vector<std::string> trace;
    router.use([&](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next) -> asio::awaitable<void> {
        trace.push_back("global");
        co_await next(std::move(req), std::move(res), ssl);
    });
    router.group("/g", {[&](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next) -> asio::awaitable<void> {
                     trace.push_back("group");
                     co_await next(std::move(req), std::move(res), ssl);
                 }},
                 [&](Router &g) {
                     g.route(any_methods, "/x",
                             {[&](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next) -> asio::awaitable<void> {
                                 trace.push_back("per-route");
                                 co_await next(std::move(req), std::move(res), ssl);
                             }},
                             [&](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                                 trace.push_back("handler");
                                 co_await res->status(200).send("ok");
                             });
                 });

    asio::io_context ctx;
    auto writer = dispatch(ctx, router, make_request(ctx, "/g/x"));
    CHECK(writer->last_status == 200);
    // Outer chain first: global, then group, then per-route, then the handler.
    CHECK(trace == std::vector<std::string>{"global", "group", "per-route", "handler"});
}

TEST_CASE("router: regex routes are scoped to a group prefix", "[router]") {
    Router router;
    router.group("/api", [&](Router &api) {
        api.route_regex({Method::Get}, "^/items/(\\d+)$", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
            co_await res->status(200).send("item");
        });
        // A pattern with metacharacters in the prefix is still literal.
        api.route_regex({Method::Get}, "/raw.*", body_handler("raw"));
    });

    asio::io_context ctx;
    {
        auto writer = dispatch(ctx, router, make_request(ctx, "/api/items/42"));
        CHECK(writer->last_status == 200);
        CHECK(writer->last_body == "item");
    }
    {
        // A path the group prefix rejects must not match even if the inner
        // pattern would: the '^' anchor stays at the head.
        auto writer = dispatch(ctx, router, make_request(ctx, "/nope/items/42"));
        CHECK(writer->last_status == 404);
    }
    {
        auto writer = dispatch(ctx, router, make_request(ctx, "/api/raw-deal"));
        CHECK(writer->last_body == "raw");
    }
}

TEST_CASE("router: reverse-proxy routes are found and rewritten", "[router]") {
    Router router;
    HttpProxyTarget exact;
    exact.host = "10.0.0.1";
    exact.port = 8080;
    exact.rewrite_path = "/fixed";
    router.http_proxy("/p", exact);

    HttpProxyTarget templated;
    templated.host = "10.0.0.9";
    templated.port = 9090;
    templated.rewrite_path = "/v2/$1/$2/$$/$9"; // $0 whole match, $1..$9 groups, $$ a literal '$'
    router.http_proxy_regex(R"(/api/(\w+)/(\d+))", templated);

    {
        auto found = router.find_http_proxy("/p");
        REQUIRE(found.has_value());
        CHECK(found->target.host == "10.0.0.1");
        CHECK(found->target.port == 8080);
        CHECK(found->target.rewrite_path == "/fixed");
        CHECK_FALSE(found->target.tls);
        CHECK_FALSE(found->target.h2c);
    }
    {
        auto found = router.find_http_proxy("/api/users/42");
        REQUIRE(found.has_value());
        CHECK(found->target.host == "10.0.0.9");
        // $9 is past the last group: expanded to nothing, and $$ is a literal '$'.
        CHECK(found->target.rewrite_path == "/v2/users/42/$/");
    }
    CHECK_FALSE(router.find_http_proxy("/api/users").has_value()); // the regex needs both groups
    CHECK_FALSE(router.find_http_proxy("/other").has_value());

    // Nginx-style precedence: a local exact route wins over a proxy route for
    // the same path (nginx's `location = /path` beats everything), so the
    // unreachable backend is never contacted for it. A path that only the
    // proxy knows still proxies, and a dead backend fails into a 502. A closed
    // port is not portable for that (some environments drop rather than
    // reject), so the failure is pinned to a short connect timeout instead.
    ClientConfig proxy_cfg;
    proxy_cfg.connect_timeout = std::chrono::milliseconds(100);
    asio::io_context ctx;
    {
        // Proxy-only path: find_http_proxy is consulted after the exact local
        // miss, the dead backend fails, and the 502 is logged, not swallowed.
        // The short connect timeout travels with the route's own client.
        Router dead_backend;
        dead_backend.http_proxy("/p", HttpProxyTarget{"127.0.0.1", 1, {}}, proxy_cfg);
        auto writer = std::make_shared<FakeResponseWriter>();
        auto req = make_request(ctx, "/p");
        auto res = std::make_shared<Response>(writer);
        ScopedLog capture;
        REQUIRE(run_on(ctx, dead_backend.dispatch(req, res, std::nullopt), std::chrono::seconds(2)));
        CHECK(writer->last_status == 502);
        CHECK(writer->last_body == "Bad Gateway");
        CHECK_FALSE(capture.records.empty()); // the failed upstream is logged
    }
    {
        // Local exact route registered alongside the same-path proxy: the local
        // route wins and the dead backend is never contacted.
        Router both;
        both.http_proxy("/p", HttpProxyTarget{"127.0.0.1", 1, {}}, proxy_cfg);
        both.route({Method::Get}, "/p", body_handler("local"));
        auto writer = std::make_shared<FakeResponseWriter>();
        auto req = make_request(ctx, "/p");
        auto res = std::make_shared<Response>(writer);
        ScopedLog capture;
        REQUIRE(run_on(ctx, both.dispatch(req, res, std::nullopt), std::chrono::seconds(2)));
        CHECK(writer->last_status == 200);
        CHECK(writer->last_body == "local");
        CHECK(capture.records.empty()); // the proxy was never consulted
    }
}

TEST_CASE("router: websocket proxy routes and their rewrite", "[router]") {
    Router router;
    WsProxyTarget exact;
    exact.host = "127.0.0.1";
    exact.port = 7788;
    exact.rewrite_path = "/chat";
    router.ws_proxy("/wsproxy", exact);

    WsProxyTarget templated;
    templated.host = "127.0.0.1";
    templated.port = 7789;
    templated.rewrite_path = "/$1";
    router.ws_proxy_regex(R"(/proxy/(.*))", templated);

    auto found = router.find_ws_proxy("/wsproxy");
    REQUIRE(found.has_value());
    CHECK(found->port == 7788);
    CHECK(found->rewrite_path == "/chat");

    auto other = router.find_ws_proxy("/proxy/chat");
    REQUIRE(other.has_value());
    CHECK(other->port == 7789);
    CHECK(other->rewrite_path == "/chat");
    CHECK_FALSE(router.find_ws_proxy("/nope").has_value());
}

TEST_CASE("router: invalid regexes are dropped, not thrown", "[router]") {
    ScopedLog capture;
    Router router;
    router.route_regex({Method::Get}, "(/broken",
                       body_handler("never")); // unbalanced group
    router.http_proxy_regex("([", HttpProxyTarget{});

    CHECK_FALSE(router.find_http_proxy("/broken").has_value());
    asio::io_context ctx;
    auto writer = std::make_shared<FakeResponseWriter>();
    auto req = make_request(ctx, "/broken");
    auto res = std::make_shared<Response>(writer);
    REQUIRE(run_on(ctx, router.dispatch(req, res,
                                        std::nullopt))); // no route: the built-in 404
    CHECK(writer->last_status == 404);
}

TEST_CASE("router: a registered handler is found by path, first registration wins", "[router]") {
    Router router;
    router.route({Method::Get}, "/dup", body_handler("first"));
    router.route({Method::Get}, "/dup",
                 body_handler("second")); // emplace keeps the first

    asio::io_context ctx;
    auto writer = std::make_shared<FakeResponseWriter>();
    auto req = make_request(ctx, "/dup");
    auto res = std::make_shared<Response>(writer);
    REQUIRE(run_on(ctx, router.dispatch(req, res, std::nullopt)));
    CHECK(writer->last_body == "first");
}

TEST_CASE("router: the websocket handler lookup", "[router]") {
    Router router;
    router.ws_route("/chat", [](RequestPtr, std::shared_ptr<WebSocket>) -> asio::awaitable<void> { co_return; });

    CHECK(router.find_ws("/chat") != nullptr);
    CHECK(router.find_ws("/other") == nullptr);
}

TEST_CASE("router: the static stage sits between the routes and the fallback", "[router]") {
    StaticFixture fx;
    fx.write("index.html", "<html>root</html>");
    fx.write("shared.html", "<html>from the site</html>");

    StaticFilesConfig cfg;
    cfg.table.root = fx.root_string();
    auto site = std::make_shared<StaticFiles>(std::move(cfg));
    std::string error;
    REQUIRE(site->load(error));

    Router router;
    router.route({Method::Get}, "/shared.html", body_handler("from the route"));
    router.route_regex({Method::Get}, "/r/.*", body_handler("from the regex"));
    router.static_files(site);
    router.fallback(body_handler("from the fallback"));

    asio::io_context ctx;
    auto dispatch = [&](const std::string &path) {
        auto writer = std::make_shared<FakeResponseWriter>();
        auto req = make_request(ctx, path);
        auto res = std::make_shared<Response>(writer);
        REQUIRE(run_on(ctx, router.dispatch(req, res, std::nullopt)));
        return writer;
    };

    // A real route always wins over a file of the same name — the file is there,
    // and the route still answers.
    CHECK(dispatch("/shared.html")->last_body == "from the route");
    CHECK(dispatch("/r/anything")->last_body == "from the regex");

    // The site answers what no route claimed.
    CHECK(dispatch("/")->last_body == "<html>root</html>");
    CHECK(dispatch("/index.html")->last_body == "<html>root</html>");

    // And the fallback still sees what the site declines.
    CHECK(dispatch("/nowhere")->last_body == "from the fallback");
}

TEST_CASE("router: a declining static stage cannot leave a request unanswered", "[router]") {
    StaticFixture fx;
    fx.write("index.html", "hi");

    StaticFilesConfig cfg;
    cfg.table.root = fx.root_string();
    auto site = std::make_shared<StaticFiles>(std::move(cfg));
    std::string error;
    REQUIRE(site->load(error));

    // No fallback registered. The stage declines everything except "/", and the
    // router's own 404 is what answers the rest — which is the structural fix for
    // the old arrangement, where a site had to be registered as a catch-all regex
    // and an early return would have hung the client.
    Router router;
    router.static_files(site);

    asio::io_context ctx;
    auto writer = std::make_shared<FakeResponseWriter>();
    auto req = make_request(ctx, "/not-in-the-site");
    auto res = std::make_shared<Response>(writer);
    REQUIRE(run_on(ctx, router.dispatch(req, res, std::nullopt)));

    CHECK(writer->last_status == 404); // written by the router, not left hanging
}

TEST_CASE("router: registering a disabled site is a no-op, not a silent 404 machine", "[router]") {
    StaticFilesConfig cfg; // empty root: disabled
    auto site = std::make_shared<StaticFiles>(std::move(cfg));
    std::string error;
    REQUIRE(site->load(error));
    REQUIRE_FALSE(site->enabled());

    Router router;
    router.static_files(site); // ignored
    router.route({Method::Get}, "/", body_handler("route still works"));

    asio::io_context ctx;
    auto writer = std::make_shared<FakeResponseWriter>();
    auto req = make_request(ctx, "/");
    auto res = std::make_shared<Response>(writer);
    REQUIRE(run_on(ctx, router.dispatch(req, res, std::nullopt)));

    CHECK(writer->last_body == "route still works");
}

// --- method-aware routing ----------------------------------------------------

// Dispatches one request on its own router state and returns the writer.
auto method_dispatch = [](Router &router, asio::io_context &ctx, const std::string &path, Method m) {
    auto writer = std::make_shared<FakeResponseWriter>();
    auto req = make_request(ctx, path, m);
    auto res = std::make_shared<Response>(writer);
    REQUIRE(run_on(ctx, router.dispatch(req, res, std::nullopt)));
    return writer;
};

TEST_CASE("router: methods are part of the route; a mismatch is a 405 with Allow", "[router]") {
    Router router;
    router.route({Method::Put, Method::Delete}, "/multi", body_handler("multi"));
    router.route({Method::Post}, "/postonly", body_handler("postonly"));

    asio::io_context ctx;
    CHECK(method_dispatch(router, ctx, "/multi", Method::Put)->last_body == "multi");
    CHECK(method_dispatch(router, ctx, "/multi", Method::Delete)->last_body == "multi");

    // Wrong method, existing path: 405 whose Allow lists the path's methods —
    // OPTIONS included, because the router answers it automatically.
    {
        auto w = method_dispatch(router, ctx, "/multi", Method::Get);
        CHECK(w->last_status == status::method_not_allowed);
        CHECK(w->header(field::allow) == "PUT, DELETE, OPTIONS");
    }
    {
        auto w = method_dispatch(router, ctx, "/postonly", Method::Get);
        CHECK(w->last_status == status::method_not_allowed);
        CHECK(w->header(field::allow) == "POST, OPTIONS");
    }
    // An extension method token lands on the same 405 path.
    CHECK(method_dispatch(router, ctx, "/postonly", Method::Unknown)->last_status == status::method_not_allowed);
    // A path nothing services is a 404, not a 405 (resolved first, rejected
    // second).
    CHECK(method_dispatch(router, ctx, "/nothing", Method::Get)->last_status == status::not_found);
}

TEST_CASE("router: GET implies HEAD, auto-OPTIONS answers Allow", "[router]") {
    Router router;
    router.route({Method::Get}, "/g", body_handler("g"));

    asio::io_context ctx;
    // HEAD is served by the GET route (the engine strips the body on the wire).
    CHECK(method_dispatch(router, ctx, "/g", Method::Head)->last_status == status::ok);
    CHECK(method_dispatch(router, ctx, "/g", Method::Head)->last_body == "g");
    // OPTIONS is answered with the Allow list; the route does not run.
    {
        auto w = method_dispatch(router, ctx, "/g", Method::Options);
        CHECK(w->last_status == status::no_content);
        CHECK(w->header(field::allow) == "GET, HEAD, OPTIONS");
        CHECK(w->last_body.empty());
    }
}

TEST_CASE("router: an any-method route serves every method, extension tokens "
          "included",
          "[router]") {
    Router router;
    router.route(any_methods, "/open", body_handler("open"));

    asio::io_context ctx;
    CHECK(method_dispatch(router, ctx, "/open", Method::Get)->last_body == "open");
    CHECK(method_dispatch(router, ctx, "/open", Method::Post)->last_body == "open");
    // OPTIONS reaches the handler; it is not auto-answered for an any-method
    // route.
    CHECK(method_dispatch(router, ctx, "/open", Method::Options)->last_body == "open");
    CHECK(method_dispatch(router, ctx, "/open", Method::Unknown)->last_body == "open"); // e.g. PROPFIND
}

TEST_CASE("router: methods may come from a runtime container (config file case)", "[router]") {
    Router router;
    // What a config parser fills from JSON/env/cli — not a braced literal.
    std::vector<Method> from_config = {Method::Post, Method::Put};
    router.route(std::move(from_config), "/cfg", body_handler("cfg"));

    asio::io_context ctx;
    CHECK(method_dispatch(router, ctx, "/cfg", Method::Post)->last_body == "cfg");
    CHECK(method_dispatch(router, ctx, "/cfg", Method::Put)->last_body == "cfg");
    {
        auto w = method_dispatch(router, ctx, "/cfg", Method::Get);
        CHECK(w->last_status == status::method_not_allowed);
        CHECK(w->header(field::allow) == "POST, PUT, OPTIONS");
    }
}

TEST_CASE("router: regex routes method-check too, and later patterns can resolve", "[router]") {
    Router router;
    router.route_regex({Method::Get}, "/r.*", body_handler("r-get"));
    router.route_regex({Method::Post}, "/r.*", body_handler("r-post"));

    asio::io_context ctx;
    CHECK(method_dispatch(router, ctx, "/r/x", Method::Get)->last_body == "r-get");
    CHECK(method_dispatch(router, ctx, "/r/x", Method::Post)->last_body == "r-post");
    {
        // The Allow accumulates across every pattern the path matched.
        auto w = method_dispatch(router, ctx, "/r/x", Method::Delete);
        CHECK(w->last_status == status::method_not_allowed);
        CHECK(w->header(field::allow) == "GET, HEAD, POST, OPTIONS");
    }
}
