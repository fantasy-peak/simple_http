// Small-cost backlog: core/url_encoding.h (Go net/url.QueryEscape/PathEscape),
// handler/conditionals.h (handler-level ETag → 304), and the real_ip CIDR /
// access_log request-id additions to builtin_middleware.h.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <string>

#include "simple_http.h"
#include "test_support.h"

using namespace simple_http;
using namespace simple_http::test;

namespace {

RequestPtr make_request(asio::io_context &ctx, std::string target,
                        std::vector<std::pair<std::string, std::string>> headers = {}) {
    auto req = std::make_shared<Request>(Version::Http11, ctx.get_executor(), asio::ip::tcp::endpoint{});
    req->set_target(std::move(target));
    req->set_method(Method::Get);
    req->body().finish();
    for (auto &[name, value] : headers) {
        req->mutable_headers().add_lower(std::move(name), std::move(value));
    }
    return req;
}

std::shared_ptr<FakeResponseWriter> dispatch(asio::io_context &ctx, Router &router, RequestPtr req) {
    auto writer = std::make_shared<FakeResponseWriter>();
    auto res = std::make_shared<Response>(writer);
    REQUIRE(run_on(ctx, router.dispatch(req, res, std::nullopt)));
    return writer;
}

} // namespace

// --- URL encoding (core/url_encoding.h) --------------------------------------

TEST_CASE("url encoding: query_escape and path_escape", "[tools]") {
    // Unreserved and letters pass through; space→'+' only in query form. Like
    // Go's QueryEscape, the whole string is a *value* — '=' and '&' are
    // encoded, so a value cannot splice additional parameters.
    CHECK(query_escape("a b") == "a+b");
    CHECK(query_escape("q=\"x&y\"") == "q%3D%22x%26y%22");
    CHECK(query_escape("café") == "caf%C3%A9");
    // Path escaping keeps '/' (a path) and percent-encodes the rest.
    CHECK(path_escape("/a/b c") == "/a/b%20c");
    CHECK(path_escape("x&y") == "x%26y");
    // A constructed query survives a round-trip through the library's parser.
    const std::string q = "tag=" + query_escape("socks & shoes");
    const auto parsed = QueryParams::parse(q);
    CHECK(parsed.get("tag") == "socks & shoes");
}

// --- conditionals (handler/conditionals.h) -----------------------------------

TEST_CASE("conditionals: If-None-Match answers 304 and sets validators", "[tools]") {
    asio::io_context ctx;
    const auto etag = make_etag(1700000000, 100, "id");
    {
        auto writer = std::make_shared<FakeResponseWriter>();
        auto res = std::make_shared<Response>(writer);
        auto req = make_request(ctx, "/doc", {{"if-none-match", std::string{etag}}});
        bool not_modified = run_on(ctx, maybe_not_modified(req, res, etag, 1700000000)).value_or(false);
        CHECK(not_modified);
        CHECK(writer->last_status == status::not_modified);
        CHECK(writer->sent_bodyless); // 304 is bodyless, no frame
        CHECK(writer->header(field::etag) == etag);
    }
    {
        // A different validator: the 200 path, but the validators ride along
        // once the handler sends (a fresh Response; maybe_not_modified only
        // staged the headers until now).
        auto writer = std::make_shared<FakeResponseWriter>();
        auto res = std::make_shared<Response>(writer);
        auto req = make_request(ctx, "/doc", {{"if-none-match", "\"other\""}});
        bool not_modified = run_on(ctx, maybe_not_modified(req, res, etag, 1700000000)).value_or(false);
        CHECK_FALSE(not_modified);
        REQUIRE(run_on(ctx, res->status(200).send("body")));
        CHECK(writer->header(field::etag) == etag);
        CHECK(writer->header(field::last_modified) == http_date(1700000000));
    }
    {
        // "*" matches anything current.
        auto writer = std::make_shared<FakeResponseWriter>();
        auto res = std::make_shared<Response>(writer);
        auto req = make_request(ctx, "/doc", {{"if-none-match", "*"}});
        bool not_modified = run_on(ctx, maybe_not_modified(req, res, etag, 1700000000)).value_or(false);
        CHECK(not_modified);
    }
}

TEST_CASE("conditionals: If-Modified-Since and non-GET skip", "[tools]") {
    asio::io_context ctx;
    const auto etag = make_etag(1700000000, 100, "id");
    {
        // Client's If-Modified-Since is not newer than the resource: 304.
        auto writer = std::make_shared<FakeResponseWriter>();
        auto res = std::make_shared<Response>(writer);
        auto req = make_request(ctx, "/doc",
                                {{"if-modified-since", http_date(1700000000 + 5000)}}); // client newer -> not modified
        bool not_modified = run_on(ctx, maybe_not_modified(req, res, etag, 1700000000)).value_or(false);
        CHECK(not_modified);
    }
    {
        // Only If-Modified-Since, no ETag path.
        auto writer = std::make_shared<FakeResponseWriter>();
        auto res = std::make_shared<Response>(writer);
        auto req =
            make_request(ctx, "/doc", {{"if-modified-since", http_date(1600000000)}}); // resource newer -> modified
        bool not_modified = run_on(ctx, maybe_not_modified(req, res, "", 1700000000)).value_or(false);
        CHECK_FALSE(not_modified);
    }
    {
        // A POST must never be answered 304 by If-None-Match.
        auto req = make_request(ctx, "/doc");
        req->set_method(Method::Post);
        req->mutable_headers().add_lower("if-none-match", std::string{etag});
        auto writer = std::make_shared<FakeResponseWriter>();
        auto res = std::make_shared<Response>(writer);
        bool not_modified = run_on(ctx, maybe_not_modified(req, res, etag, 1700000000)).value_or(false);
        CHECK_FALSE(not_modified);
    }
}

// --- real_ip CIDR & access_log request-id (builtin_middleware.h) --------------

TEST_CASE("real_ip: trusted entries accept CIDR prefixes", "[tools]") {
    Router router;
    std::string seen;
    router.use(middleware::real_ip({"10.0.0.0/8", "192.168.1.1"}));
    router.route(any_methods, "/ip", [&](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        const auto *ip = req->get_state<middleware::ClientIp>();
        seen = ip ? ip->value : "";
        co_await res->status(200).send("ok");
    });

    asio::io_context ctx;
    {
        // "10.5.5.5" is inside 10.0.0.0/8, so the hop before it (the real
        // client) is chosen.
        auto req = make_request(ctx, "/ip", {{"x-forwarded-for", "203.0.113.7, 10.5.5.5"}});
        dispatch(ctx, router, req);
        CHECK(seen == "203.0.113.7");
    }
    {
        // "11.5.5.5" is outside the CIDR: it is the first untrusted hop itself.
        auto req = make_request(ctx, "/ip", {{"x-forwarded-for", "203.0.113.7, 11.5.5.5"}});
        dispatch(ctx, router, req);
        CHECK(seen == "11.5.5.5");
    }
    {
        // An exact trusted address still works (regression for the old form).
        auto req = make_request(ctx, "/ip", {{"x-forwarded-for", "203.0.113.7, 192.168.1.1"}});
        dispatch(ctx, router, req);
        CHECK(seen == "203.0.113.7");
    }
}

TEST_CASE("access_log: the request id is included when request_id ran", "[tools]") {
    Router router;
    auto writer0 = std::make_shared<FakeResponseWriter>();
    ScopedLog capture;
    // request_id inside access_log, so the id is set before the after-phase.
    router.use(middleware::access_log());
    router.use(middleware::request_id());
    router.route(any_methods, "/ok",
                 [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(200).send("ok"); });

    asio::io_context ctx;
    dispatch(ctx, router, make_request(ctx, "/ok"));
    REQUIRE(capture.records.size() >= 1);
    // The record carries the id in brackets: `peer [<id>] "GET /ok" 200 ...`.
    const auto &rec = capture.records.front();
    CHECK(rec.message.find("] \"GET /ok\" 200") != std::string::npos);
}