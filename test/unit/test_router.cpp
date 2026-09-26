// handler/: route registration, matching order, the reverse-proxy lookups (with
// their $1-style rewrite expansion) and dispatch.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include "simple_http.h"
#include "../static_fixture.h"
#include "test_support.h"

using namespace simple_http;
using namespace simple_http::test;

namespace {

RequestPtr make_request(asio::io_context& ctx, std::string path) {
    auto req = std::make_shared<Request>(Version::Http11, ctx.get_executor(), asio::ip::tcp::endpoint{});
    req->set_target(std::move(path));
    // The engines always end the request body; the reverse proxy probes it before
    // dialing, so a test must do the same or that read waits forever.
    req->body().finish();
    return req;
}

// A handler that answers with a fixed body, so dispatch's routing is observable.
Handler body_handler(std::string body) {
    return make_handler([body = std::move(body)](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).send(body);
    });
}

}  // namespace

TEST_CASE("router: exact, regex and fallback matching", "[router]") {
    Router router;
    router.route("/exact", body_handler("exact"));
    router.route_regex("/api/.*", body_handler("regex"));
    router.fallback(body_handler("fallback"));

    asio::io_context ctx;
    auto dispatch = [&](const std::string& path) {
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
    router.route("/only", body_handler("only"));

    asio::io_context ctx;
    auto writer = std::make_shared<FakeResponseWriter>();
    auto req = make_request(ctx, "/missing");
    auto res = std::make_shared<Response>(writer);
    REQUIRE(run_on(ctx, router.dispatch(req, res, std::nullopt)));
    CHECK(writer->last_status == 404);
    CHECK(writer->last_body.empty());
}

TEST_CASE("router: filters short-circuit and see the request", "[router]") {
    Router router;
    bool before_called = false;
    bool cors_called = false;
    router.cors([&](RequestPtr, ResponsePtr) -> asio::awaitable<bool> {
        cors_called = true;
        co_return true;
    });
    router.before([&](RequestPtr req, ResponsePtr res) -> asio::awaitable<bool> {
        before_called = true;
        if (req->path() == "/blocked") {
            co_await res->status(403).send("denied");
            co_return false;  // handled here; the route must not run
        }
        co_return true;
    });
    router.route("/blocked", body_handler("should not run"));
    router.route("/allowed", body_handler("allowed"));

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
        cors_called = false;
        auto writer = std::make_shared<FakeResponseWriter>();
        auto req = make_request(ctx, "/allowed");  // no Origin header: cors is skipped
        auto res = std::make_shared<Response>(writer);
        REQUIRE(run_on(ctx, router.dispatch(req, res, std::nullopt)));
        CHECK_FALSE(cors_called);
        CHECK(writer->last_body == "allowed");
    }
    {
        cors_called = false;
        auto writer = std::make_shared<FakeResponseWriter>();
        auto req = make_request(ctx, "/allowed");
        req->mutable_headers().add_lower("origin", "https://example.com");
        auto res = std::make_shared<Response>(writer);
        REQUIRE(run_on(ctx, router.dispatch(req, res, std::nullopt)));
        CHECK(cors_called);
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
    templated.rewrite_path = "/v2/$1/$2/$$/$9";  // $0 whole match, $1..$9 groups, $$ a literal '$'
    router.http_proxy_regex(R"(/api/(\w+)/(\d+))", templated);

    {
        auto found = router.find_http_proxy("/p");
        REQUIRE(found.has_value());
        CHECK(found->host == "10.0.0.1");
        CHECK(found->port == 8080);
        CHECK(found->rewrite_path == "/fixed");
        CHECK_FALSE(found->tls);
        CHECK_FALSE(found->h2c);
    }
    {
        auto found = router.find_http_proxy("/api/users/42");
        REQUIRE(found.has_value());
        CHECK(found->host == "10.0.0.9");
        // $9 is past the last group: expanded to nothing, and $$ is a literal '$'.
        CHECK(found->rewrite_path == "/v2/users/42/$/");
    }
    CHECK_FALSE(router.find_http_proxy("/api/users").has_value());  // the regex needs both groups
    CHECK_FALSE(router.find_http_proxy("/other").has_value());

    // A local route with the same path loses: proxy routes are consulted first,
    // and a backend that cannot be reached turns into a 502. A closed port is not
    // portable for this (some environments drop rather than reject), so the
    // failure is pinned to a short connect timeout instead.
    ClientConfig proxy_cfg;
    proxy_cfg.connect_timeout = std::chrono::milliseconds(100);
    Router dead_backend{proxy_cfg};
    dead_backend.http_proxy("/p", HttpProxyTarget{"127.0.0.1", 1, {}});
    dead_backend.route("/p", body_handler("local"));
    asio::io_context ctx;
    auto writer = std::make_shared<FakeResponseWriter>();
    auto req = make_request(ctx, "/p");
    auto res = std::make_shared<Response>(writer);
    ScopedLog capture;
    REQUIRE(run_on(ctx, dead_backend.dispatch(req, res, std::nullopt), std::chrono::seconds(2)));
    CHECK(writer->last_status == 502);
    CHECK(writer->last_body == "Bad Gateway");
    CHECK_FALSE(capture.records.empty());  // the failed upstream is logged, not swallowed
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
    router.route_regex("(/broken", body_handler("never"));  // unbalanced group
    router.http_proxy_regex("([", HttpProxyTarget{});

    CHECK_FALSE(router.find_http_proxy("/broken").has_value());
    asio::io_context ctx;
    auto writer = std::make_shared<FakeResponseWriter>();
    auto req = make_request(ctx, "/broken");
    auto res = std::make_shared<Response>(writer);
    REQUIRE(run_on(ctx, router.dispatch(req, res, std::nullopt)));  // no route: the built-in 404
    CHECK(writer->last_status == 404);
}

TEST_CASE("router: a registered handler is found by path, first registration wins", "[router]") {
    Router router;
    router.route("/dup", body_handler("first"));
    router.route("/dup", body_handler("second"));  // emplace keeps the first

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
    router.route("/shared.html", body_handler("from the route"));
    router.route_regex("/r/.*", body_handler("from the regex"));
    router.static_files(site);
    router.fallback(body_handler("from the fallback"));

    asio::io_context ctx;
    auto dispatch = [&](const std::string& path) {
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

    CHECK(writer->last_status == 404);  // written by the router, not left hanging
}

TEST_CASE("router: registering a disabled site is a no-op, not a silent 404 machine", "[router]") {
    StaticFilesConfig cfg;  // empty root: disabled
    auto site = std::make_shared<StaticFiles>(std::move(cfg));
    std::string error;
    REQUIRE(site->load(error));
    REQUIRE_FALSE(site->enabled());

    Router router;
    router.static_files(site);  // ignored
    router.route("/", body_handler("route still works"));

    asio::io_context ctx;
    auto writer = std::make_shared<FakeResponseWriter>();
    auto req = make_request(ctx, "/");
    auto res = std::make_shared<Response>(writer);
    REQUIRE(run_on(ctx, router.dispatch(req, res, std::nullopt)));

    CHECK(writer->last_body == "route still works");
}
