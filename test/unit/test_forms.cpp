// Framework-core gaps closed against the Go/Rust ecosystems:
// proto/query.h + Request::query_params (Go r.URL.Query / axum Query<T>),
// proto/multipart.h (axum Multipart / Go ParseMultipartForm),
// Response::redirect / sse_* (http.Redirect / axum sse), and the
// real_ip / clean_path / strip_prefix middleware (chi RealIP / CleanPath /
// StripPrefix, tower-http equivalents).

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <vector>

#include "simple_http.h"
#include "test_support.h"

using namespace simple_http;
using namespace simple_http::test;

namespace {

RequestPtr make_request(asio::io_context &ctx, std::string target, Method method = Method::Get) {
    auto req = std::make_shared<Request>(Version::Http11, ctx.get_executor(), asio::ip::tcp::endpoint{});
    req->set_target(std::move(target));
    req->set_method(method);
    req->body().finish();
    return req;
}

std::shared_ptr<FakeResponseWriter> dispatch(asio::io_context &ctx, Router &router, RequestPtr req) {
    auto writer = std::make_shared<FakeResponseWriter>();
    auto res = std::make_shared<Response>(writer);
    REQUIRE(run_on(ctx, router.dispatch(req, res, std::nullopt)));
    return writer;
}

} // namespace

// --- query parsing -----------------------------------------------------------

TEST_CASE("proto/query: decodes pairs, plus-as-space and percent escapes", "[forms]") {
    auto q = QueryParams::parse("a=1&b=hello+world&c=%E4%B8%AD&flag");
    CHECK(q.get("a") == "1");
    CHECK(q.get("b") == "hello world");
    CHECK(q.get("c") == "\xE4\xB8\xAD"); // UTF-8 bytes pass through
    CHECK(q.get("flag") == "");
    CHECK_FALSE(q.get("missing").has_value());
    CHECK(q.contains("flag"));
    CHECK(q.items().size() == 4);
}

TEST_CASE("proto/query: repeated names keep every value in order", "[forms]") {
    auto q = QueryParams::parse("tag=a&tag=b&x=1&tag=c");
    CHECK(q.get("tag") == "a"); // first wins for get()
    CHECK(q.get_all("tag").size() == 3);
    CHECK(q.get_all("tag")[1] == "b");
    // Undecodable escapes survive verbatim rather than failing the parse.
    auto bad = QueryParams::parse("v=%zz&ok=1");
    CHECK(bad.get("v") == "%zz");
    CHECK(bad.get("ok") == "1");
    // An empty string is an empty parse; a lone '&' too.
    CHECK(QueryParams::parse("").empty());
    CHECK(QueryParams::parse("&").empty());
}

TEST_CASE("proto/query: a request's own query is parsed on the Request", "[forms]") {
    asio::io_context ctx;
    auto req = make_request(ctx, "/search?q=cat&page=2");
    CHECK(req->query_params().get("q") == "cat");
    CHECK(req->query_params().get("page") == "2");
    CHECK_FALSE(req->query_params().empty());
    auto bare = make_request(ctx, "/plain");
    CHECK(bare->query_params().empty());
}

// --- multipart --------------------------------------------------------------

TEST_CASE("proto/multipart: boundary extraction", "[forms]") {
    CHECK(multipart_boundary("multipart/form-data; boundary=----abc") == "----abc");
    CHECK(multipart_boundary("multipart/form-data; boundary=\"----abc\"") == "----abc");
    CHECK(multipart_boundary("multipart/form-data; charset=utf-8; boundary=X") == "X");
    CHECK_FALSE(multipart_boundary("multipart/form-data").has_value());
    CHECK_FALSE(multipart_boundary("text/plain; boundary=").has_value());
}

TEST_CASE("proto/multipart: fields and files split by the boundary", "[forms]") {
    const std::string body = "--B\r\n"
                             "Content-Disposition: form-data; name=\"title\"\r\n"
                             "\r\n"
                             "hello\r\n"
                             "--B\r\n"
                             "Content-Disposition: form-data; name=\"up\"; filename=\"a.txt\"\r\n"
                             "Content-Type: text/plain\r\n"
                             "\r\n"
                             "file bytes\r\n"
                             "--B--\r\n";
    auto parts = parse_multipart(body, "B");
    REQUIRE(parts.has_value());
    REQUIRE(parts->size() == 2);
    CHECK((*parts)[0].name == "title");
    CHECK((*parts)[0].data == "hello");
    CHECK((*parts)[0].filename.empty());
    CHECK((*parts)[1].name == "up");
    CHECK((*parts)[1].filename == "a.txt");
    CHECK((*parts)[1].content_type == "text/plain");
    CHECK((*parts)[1].data == "file bytes");
}

TEST_CASE("proto/multipart: a leading CRLF is tolerated and framing errors are nullopt", "[forms]") {
    const std::string body = "\r\n--B\r\n"
                             "Content-Disposition: form-data; name=\"x\"\r\n"
                             "\r\n"
                             "1\r\n"
                             "--B--\r\n";
    auto parts = parse_multipart(body, "B");
    REQUIRE(parts.has_value());
    CHECK((*parts)[0].data == "1");

    CHECK_FALSE(parse_multipart("no delimiter here", "B").has_value());
    // A body that never closes (no --B-- and no next --B break) is rejected.
    CHECK_FALSE(parse_multipart("--B\r\nContent-Disposition: form-data; name=\"x\"\r\n\r\ndata", "B").has_value());
    // The per-part cap is enforced (0 = no part may carry a body).
    CHECK_FALSE(parse_multipart(body, "B", /*max_part_bytes=*/0, /*max_parts=*/100).has_value());
}

TEST_CASE("proto/multipart: read_multipart_body on a request", "[forms]") {
    asio::io_context ctx;
    // Built by hand, not make_request: a real request body is fed before
    // finish(), whereas make_request's unconditional finish() would put the
    // Eof frame ahead of the data we feed here.
    auto req = std::make_shared<Request>(Version::Http11, ctx.get_executor(), asio::ip::tcp::endpoint{});
    req->set_target("/up");
    req->set_method(Method::Post);
    req->mutable_headers().add_lower("content-type", "multipart/form-data; boundary=xyz");
    (void)req->body().feed("--xyz\r\nContent-Disposition: form-data; name=\"n\"\r\n\r\nv\r\n--xyz--\r\n");
    req->body().finish();

    auto form = run_on(ctx, read_multipart_body(*req));
    REQUIRE(form.has_value());
    REQUIRE(form->has_value());
    CHECK((**form)[0].name == "n");
    CHECK((**form)[0].data == "v");
}

TEST_CASE("proto/multipart: read_urlencoded_body on a request", "[forms]") {
    asio::io_context ctx;
    auto req = std::make_shared<Request>(Version::Http11, ctx.get_executor(), asio::ip::tcp::endpoint{});
    req->set_target("/form");
    req->set_method(Method::Post);
    (void)req->body().feed("a=1&b=two+words");
    req->body().finish();

    auto form = run_on(ctx, read_urlencoded_body(*req));
    REQUIRE(form.has_value());
    REQUIRE(form->has_value());
    CHECK((*form)->get("a") == "1");
    CHECK((*form)->get("b") == "two words");
}

// --- redirect & SSE ----------------------------------------------------------

TEST_CASE("proto/response: redirect sets Status and Location", "[forms]") {
    asio::io_context ctx;
    auto writer = std::make_shared<FakeResponseWriter>();
    auto res = std::make_shared<Response>(writer);
    REQUIRE(run_on(ctx, res->redirect("/login").send("")));
    CHECK(writer->last_status == status::found);
    CHECK(writer->header(field::location) == "/login");

    auto writer2 = std::make_shared<FakeResponseWriter>();
    auto res2 = std::make_shared<Response>(writer2);
    REQUIRE(run_on(ctx, res2->redirect("/moved", status::permanent_redirect).send("")));
    CHECK(writer2->last_status == status::permanent_redirect);
    CHECK(writer2->header(field::location) == "/moved");
}

TEST_CASE("proto/response: SSE frames are well-formed events", "[forms]") {
    asio::io_context ctx;
    auto writer = std::make_shared<FakeResponseWriter>();
    auto res = std::make_shared<Response>(writer);

    REQUIRE(run_on(ctx, res->sse_begin()));
    CHECK(writer->begun);
    CHECK(writer->last_headers.get("content-type") == "text/event-stream; charset=utf-8");
    CHECK(writer->last_headers.get("cache-control") == "no-cache");

    REQUIRE(run_on(ctx, res->sse_event("hello")));
    REQUIRE(run_on(ctx, res->sse_event("l1\nl2", "update", "42")));
    REQUIRE(run_on(ctx, res->sse_comment("keepalive")));

    CHECK(writer->chunks.size() == 3);
    CHECK(writer->chunks[0] == "data: hello\n\n");
    CHECK(writer->chunks[1] == "event: update\nid: 42\ndata: l1\ndata: l2\n\n");
    CHECK(writer->chunks[2] == ": keepalive\n\n");
}

// --- real_ip / clean_path / strip_prefix middleware --------------------------

TEST_CASE("middleware: real_ip resolves from X-Forwarded-For", "[forms]") {
    Router router;
    std::string seen;
    router.use(middleware::real_ip());
    router.route(any_methods, "/ip", [&](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        const auto *ip = req->get_state<middleware::ClientIp>();
        seen = ip ? ip->value : "";
        co_await res->status(200).send("ok");
    });

    asio::io_context ctx;
    {
        auto req = make_request(ctx, "/ip");
        req->mutable_headers().add_lower("x-forwarded-for", "203.0.113.7, 10.0.0.1");
        auto writer = dispatch(ctx, router, req);
        CHECK(seen == "203.0.113.7"); // original client, not the proxy
    }
    // No forwarded header: no ClientIp state, and the route still runs.
    seen = "cleared";
    dispatch(ctx, router, make_request(ctx, "/ip"));
    CHECK(seen.empty()); // handler saw no ClientIp and stored ""
}

TEST_CASE("middleware: real_ip skips trusted proxies right-to-left", "[forms]") {
    Router router;
    std::string seen;
    router.use(middleware::real_ip({"10.0.0.1"}));
    router.route(any_methods, "/ip", [&](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        const auto *ip = req->get_state<middleware::ClientIp>();
        seen = ip ? ip->value : "";
        co_await res->status(200).send("ok");
    });

    asio::io_context ctx;
    auto req = make_request(ctx, "/ip");
    req->mutable_headers().add_lower("x-real-ip", "203.0.113.9"); // X-Real-IP wins
    auto writer = dispatch(ctx, router, req);
    CHECK(seen == "203.0.113.9");

    seen = "";
    auto req2 = make_request(ctx, "/ip");
    req2->mutable_headers().add_lower("x-forwarded-for", "203.0.113.7, 10.0.0.1");
    dispatch(ctx, router, req2);
    CHECK(seen == "203.0.113.7"); // 10.0.0.1 is trusted, so the hop before it wins
}

TEST_CASE("middleware: clean_path normalizes and rejects traversal", "[forms]") {
    Router router;
    std::vector<std::string> seen;
    router.use(middleware::clean_path());
    router.route({Method::Get}, "/a/b", [&](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        seen.push_back("ab");
        co_await res->status(200).send("ok");
    });

    asio::io_context ctx;
    {
        // Double slashes and dot segments collapse; the clean target reaches the route.
        auto req = make_request(ctx, "/a//./b");
        auto writer = dispatch(ctx, router, req);
        CHECK(writer->last_status == 200);
        CHECK(seen.back() == "ab");
        CHECK(req->path() == "/a/b"); // the request itself was rewritten
    }
    {
        // ".." segments are refused with a 400 before they can reach anything.
        auto req = make_request(ctx, "/a/../etc/passwd");
        auto writer = dispatch(ctx, router, req);
        CHECK(writer->last_status == status::bad_request);
    }
    {
        // A canonical path passes through untouched.
        auto req = make_request(ctx, "/a/b?q=1");
        auto writer = dispatch(ctx, router, req);
        CHECK(writer->last_status == 200);
        CHECK(req->query() == "q=1"); // the query survives
    }
}

TEST_CASE("middleware: strip_prefix re-mounts a sub-application at its root", "[forms]") {
    Router router;
    std::vector<std::string> seen;
    router.use(middleware::strip_prefix("/api"));
    router.route({Method::Get}, "/users", [&](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        seen.push_back("users");
        co_await res->status(200).send("users");
    });

    asio::io_context ctx;
    {
        auto req = make_request(ctx, "/api/users?page=2");
        auto writer = dispatch(ctx, router, req);
        CHECK(writer->last_status == 200);
        CHECK(writer->last_body == "users");
        CHECK(seen.back() == "users");
        CHECK(req->path() == "/users");
        CHECK(req->query() == "page=2");
    }
    {
        // The bare prefix maps to "/".
        auto req = make_request(ctx, "/api");
        auto writer = dispatch(ctx, router, req);
        CHECK(writer->last_status == 404); // no route at "/"
    }
    {
        // Outside the prefix, the request passes through untouched: no route at
        // "/health" ... answer 404, but the route table saw "/health".
        auto req = make_request(ctx, "/health");
        dispatch(ctx, router, req);
        CHECK(req->path() == "/health");
    }
}