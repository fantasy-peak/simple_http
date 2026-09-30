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

TEST_CASE("router: before filters short-circuit and see the request", "[router]") {
    Router router;
    bool before_called = false;
    router.before([&](RequestPtr req, ResponsePtr res) -> asio::awaitable<bool> {
        before_called = true;
        if (req->path() == "/blocked") {
            co_await res->status(403).send("denied");
            co_return false; // handled here; the route must not run
        }
        co_return true;
    });
    router.route(any_methods, "/blocked", body_handler("should not run"));
    router.route(any_methods, "/allowed", body_handler("allowed"));

    asio::io_context ctx;
    {
        auto writer = std::make_shared<FakeResponseWriter>();
        auto req = make_request(ctx, "/blocked");
        auto res = std::make_shared<Response>(writer);
        REQUIRE(run_on(ctx, router.dispatch(req, res, std::nullopt)));
        CHECK(before_called);
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
