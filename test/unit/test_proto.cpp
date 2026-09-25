// proto/: the version-agnostic HTTP model — Headers, Body, Request, Response.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include "simple_http.h"
#include "test_support.h"

using namespace simple_http;
using namespace simple_http::test;

// --- Headers -----------------------------------------------------------------

TEST_CASE("proto/headers: lookup is case-insensitive, storage is lowercased", "[proto]") {
    Headers h;
    h.add("Content-Type", "text/plain");
    h.add("X-Trace", "abc");

    CHECK(h.size() == 2);
    CHECK(h.fields()[0].first == "content-type");  // lowercased on the way in
    CHECK(h.get("content-type") == "text/plain");
    CHECK(h.get("CONTENT-TYPE") == "text/plain");
    CHECK(h.get("Content-Type") == "text/plain");
    CHECK(h.contains("x-trace"));
    CHECK_FALSE(h.contains("x-missing"));
    CHECK_FALSE(h.get("content").has_value());  // no prefix matching
}

TEST_CASE("proto/headers: order, duplicates and clearing", "[proto]") {
    Headers h;
    h.add_lower("a", "1");
    h.add_lower("b", "2");
    h.add_lower("a", "3");  // a duplicate field: the first one wins on lookup

    std::string seen;
    for (const auto& [name, value] : h)
        seen += name + "=" + value + ";";
    CHECK(seen == "a=1;b=2;a=3;");
    CHECK(h.get("a") == "1");

    h.clear();
    CHECK(h.empty());
    CHECK(h.size() == 0);
}

TEST_CASE("proto/headers: the allocation-free case-insensitive compare", "[proto]") {
    CHECK(Headers::iequals_ascii("content-length", "Content-Length"));
    CHECK(Headers::iequals_ascii("", ""));
    CHECK_FALSE(Headers::iequals_ascii("a", "ab"));
    CHECK_FALSE(Headers::iequals_ascii("abc", "abd"));
    // High-bit bytes must compare equal to themselves and not alias ASCII letters.
    CHECK(Headers::iequals_ascii("\xC3\xA9", "\xC3\xA9"));
    CHECK_FALSE(Headers::iequals_ascii("\xC3", "c"));
}

// --- Body --------------------------------------------------------------------

TEST_CASE("proto/body: feed, read and end-of-body", "[proto]") {
    asio::io_context ctx;
    Body body{ctx.get_executor()};

    CHECK_FALSE(body.eof());
    REQUIRE(body.feed("hello "));
    REQUIRE(body.feed("world"));
    REQUIRE(body.finish());

    // The channel carries the frames in order; finish() delivers end-of-body.
    auto first = run_on(ctx, body.read());
    REQUIRE(first.has_value());
    REQUIRE(first->has_value());
    CHECK_FALSE((*first)->eof);
    CHECK((*first)->data == "hello ");

    auto second = run_on(ctx, body.read());
    REQUIRE(second.has_value());
    REQUIRE(second->has_value());
    CHECK((*second)->data == "world");

    auto third = run_on(ctx, body.read());
    REQUIRE(third.has_value());
    REQUIRE(third->has_value());
    CHECK((*third)->eof);
    CHECK(body.eof());

    // Once ended, read() keeps reporting the end without touching the channel.
    auto fourth = run_on(ctx, body.read());
    REQUIRE(fourth.has_value());
    REQUIRE(fourth->has_value());
    CHECK((*fourth)->eof);
}

TEST_CASE("proto/body: read_all concatenates, fail() surfaces the error", "[proto]") {
    {
        asio::io_context ctx;
        Body body{ctx.get_executor()};
        REQUIRE(body.feed("ab"));
        REQUIRE(body.feed("cd"));
        REQUIRE(body.finish());
        auto all = run_on(ctx, body.read_all());
        REQUIRE(all.has_value());
        REQUIRE(all->has_value());
        CHECK(**all == "abcd");
    }
    {
        asio::io_context ctx;
        Body body{ctx.get_executor()};
        REQUIRE(body.finish());  // an empty body reads as an empty string
        auto all = run_on(ctx, body.read_all());
        REQUIRE(all.has_value());
        REQUIRE(all->has_value());
        CHECK((*all)->empty());
    }
    {
        asio::io_context ctx;
        Body body{ctx.get_executor()};
        REQUIRE(body.fail(make_error_code(asio::error::connection_reset)));
        auto frame = run_on(ctx, body.read());
        REQUIRE(frame.has_value());
        REQUIRE_FALSE(frame->has_value());
        CHECK(frame->error() == asio::error::connection_reset);
    }
}

TEST_CASE("proto/body: the channel is bounded and the consume hook skips empty chunks", "[proto]") {
    asio::io_context ctx;
    Body body{ctx.get_executor(), /*capacity=*/3};

    REQUIRE(body.feed("1"));
    REQUIRE(body.feed("2"));
    REQUIRE(body.feed(""));       // an empty frame is delivered like any other…
    CHECK_FALSE(body.feed("3"));  // …and a full channel refuses: the producer must pace itself

    std::size_t consumed = 0;
    body.set_on_consumed([&](std::size_t n) { consumed += n; });

    auto first = run_on(ctx, body.read());
    REQUIRE(first.has_value());
    REQUIRE(first->has_value());
    CHECK((*first)->data == "1");
    CHECK(consumed == 1);  // only non-empty chunks are reported as consumed

    auto second = run_on(ctx, body.read());
    REQUIRE(second.has_value());
    REQUIRE(second->has_value());
    CHECK((*second)->data == "2");

    auto third = run_on(ctx, body.read());
    REQUIRE(third.has_value());
    REQUIRE(third->has_value());
    CHECK((*third)->data.empty());
    CHECK_FALSE((*third)->eof);
    CHECK(consumed == 2);  // the empty frame added nothing
}

TEST_CASE("proto/body: pull mode reads on demand and never touches the channel", "[proto]") {
    asio::io_context ctx;
    Body body{ctx.get_executor()};
    CHECK_FALSE(body.is_pull());

    int calls = 0;
    body.set_pull_provider([&calls]() -> asio::awaitable<std::expected<ReadResult, error_code>> {
        ++calls;
        if (calls == 1)
            co_return ReadResult::chunk("pulled");
        co_return ReadResult::end();
    });
    CHECK(body.is_pull());

    auto first = run_on(ctx, body.read());
    REQUIRE(first.has_value());
    REQUIRE(first->has_value());
    CHECK((*first)->data == "pulled");
    CHECK_FALSE(body.eof());

    auto second = run_on(ctx, body.read());
    REQUIRE(second.has_value());
    REQUIRE(second->has_value());
    CHECK((*second)->eof);
    CHECK(body.eof());

    // eof sticks: the provider is not consulted again.
    int before = calls;
    auto third = run_on(ctx, body.read());
    REQUIRE(third.has_value());
    CHECK((*third)->eof);
    CHECK(calls == before);
}

// --- Request -----------------------------------------------------------------

TEST_CASE("proto/request: target splitting and method tokens", "[proto]") {
    asio::io_context ctx;
    Request req{Version::Http11, ctx.get_executor(), asio::ip::tcp::endpoint{}};

    req.set_target("/a/b?c=d&e=f");
    CHECK(req.target() == "/a/b?c=d&e=f");
    CHECK(req.path() == "/a/b");
    CHECK(req.query() == "c=d&e=f");

    req.set_target("/only");
    CHECK(req.path() == "/only");
    CHECK(req.query().empty());

    req.set_target("/odd?");
    CHECK(req.path() == "/odd");
    CHECK(req.query().empty());

    req.set_target("/q?a?b");  // only the first '?' separates
    CHECK(req.path() == "/q");
    CHECK(req.query() == "a?b");

    req.set_method_token("PROPFIND");  // an extension method survives as a token
    CHECK(req.method() == Method::Unknown);
    CHECK(req.method_token() == "PROPFIND");
    req.set_method_token("POST");
    CHECK(req.method() == Method::Post);
}

// --- Response + a fake writer ------------------------------------------------

TEST_CASE("proto/response: defaults are filled in but never override", "[proto]") {
    asio::io_context ctx;
    auto writer = std::make_shared<FakeResponseWriter>();

    {
        Response res{writer};
        REQUIRE(run_on(ctx, res.status(201).send("created")));
        CHECK(writer->last_status == 201);
        CHECK(writer->last_body == "created");
        CHECK(writer->header("content-type") == "text/plain");  // default
        CHECK(writer->header("server") == std::string{server_version});
    }
    {
        auto w2 = std::make_shared<FakeResponseWriter>();
        Response res{w2};
        REQUIRE(run_on(ctx, res.status(200).content_type("application/json").header("server", "mine").send("{}")));
        CHECK(w2->header("content-type") == "application/json");
        CHECK(w2->header("server") == "mine");  // explicit values win
    }
}

TEST_CASE("proto/response: bodyless, streaming and state forwarding", "[proto]") {
    asio::io_context ctx;
    auto writer = std::make_shared<FakeResponseWriter>();
    Response res{writer};

    auto open = run_on(ctx, res.connected());
    REQUIRE(open.has_value());
    CHECK(*open);
    CHECK(res.version() == Version::Http11);
    writer->ver = Version::Http2;
    CHECK(res.version() == Version::Http2);

    REQUIRE(run_on(ctx, res.status(204).send_bodyless()));
    CHECK(writer->sent_bodyless);
    CHECK(writer->last_status == 204);
    CHECK(writer->last_body.empty());
    CHECK(writer->header("server") == std::string{server_version});  // defaults apply here too

    REQUIRE(run_on(ctx, res.status(200).begin()));
    CHECK(writer->begun);
    REQUIRE(run_on(ctx, res.write("a")));
    REQUIRE(run_on(ctx, res.finish("b")));
    CHECK(writer->chunks == std::vector<std::string>{"a", "b"});
    CHECK(writer->finished);

    REQUIRE(run_on(ctx, res.close()));
    auto closed = run_on(ctx, res.connected());
    REQUIRE(closed.has_value());
    CHECK_FALSE(*closed);
}

TEST_CASE("proto/response: a writer error is reported to the caller", "[proto]") {
    // The fake reports success; a writer that fails must surface through send().
    class FailingWriter : public FakeResponseWriter {
      public:
        asio::awaitable<error_code> send(int, Headers, std::string) override {
            co_return make_error_code(asio::error::broken_pipe);
        }
    };

    asio::io_context ctx;
    auto writer = std::make_shared<FailingWriter>();
    Response res{writer};
    auto ec = run_on(ctx, res.send("x"));
    REQUIRE(ec.has_value());
    CHECK(*ec == asio::error::broken_pipe);
}
