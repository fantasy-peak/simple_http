// proto/params.h: typed request-parameter extraction — the axum Path<T> /
// Query<T> extractors, on by default via glaze reflection.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <string>

#include "simple_http.h"
#include "test_support.h"

using namespace simple_http;
using namespace simple_http::test;

namespace {

struct ItemParams {
    std::string order_id;
    std::int64_t item_id;
};

struct Page {
    std::int64_t page;
    std::string tag;
};

RequestPtr make_request(asio::io_context &ctx, std::string target) {
    auto req = std::make_shared<Request>(Version::Http11, ctx.get_executor(), asio::ip::tcp::endpoint{});
    req->set_target(std::move(target));
    req->set_method(Method::Get);
    req->body().finish();
    return req;
}

} // namespace

TEST_CASE("proto/params: path_params parses template captures by type", "[params]") {
    asio::io_context ctx;
    auto req = make_request(ctx, "/orders/A-42/items/7");
    req->set_param("order_id", "A-42");
    req->set_param("item_id", "7"); // string_view into the target above

    const auto p = path_params<ItemParams>(*req);
    REQUIRE(p.has_value());
    CHECK(p->order_id == "A-42");
    CHECK(p->item_id == 7);

    // A missing capture yields nullopt.
    auto partial = std::make_shared<Request>(Version::Http11, ctx.get_executor(), asio::ip::tcp::endpoint{});
    partial->set_target("/orders/A-42");
    partial->set_param("order_id", "A-42");
    CHECK_FALSE(path_params<ItemParams>(*partial).has_value());

    // An unparsable numeric capture yields nullopt too.
    auto bad = std::make_shared<Request>(Version::Http11, ctx.get_executor(), asio::ip::tcp::endpoint{});
    bad->set_target("/orders/A-42/items/not-a-number");
    bad->set_param("order_id", "A-42");
    bad->set_param("item_id", "not-a-number");
    CHECK_FALSE(path_params<ItemParams>(*bad).has_value());
}

TEST_CASE("proto/params: query_params parses the query string", "[params]") {
    asio::io_context ctx;
    auto req = make_request(ctx, "/pets?page=2&tag=dog");

    const auto q = query_params<Page>(*req);
    REQUIRE(q.has_value());
    CHECK(q->page == 2);
    CHECK(q->tag == "dog");

    // A missing key or a bad number is nullopt.
    auto missing = make_request(ctx, "/pets?tag=dog");
    CHECK_FALSE(query_params<Page>(*missing).has_value());
    auto bad = make_request(ctx, "/pets?page=two");
    CHECK_FALSE(query_params<Page>(*bad).has_value());
}

TEST_CASE("proto/params: parse_params works on any QueryParams (a form body)", "[params]") {
    const auto form = QueryParams::parse("page=9&tag=cat");
    const auto p = parse_params<Page>(form);
    REQUIRE(p.has_value());
    CHECK(p->page == 9);
    CHECK(p->tag == "cat");
    CHECK_FALSE(parse_params<Page>(QueryParams::parse("tag=cat")).has_value());
}

TEST_CASE("proto/headers: get_all returns every value of a repeated header", "[params]") {
    Headers h;
    h.add("x-forwarded-for", "203.0.113.7");
    h.add("x-forwarded-for", "10.0.0.1"); // a proxy chain
    h.add("set-cookie", "a=1");
    h.add("set-cookie", "b=2");

    const auto hops = h.get_all("x-forwarded-for");
    REQUIRE(hops.size() == 2);
    // Appearance order: the leftmost hop is the original client.
    CHECK(hops[0] == "203.0.113.7");
    CHECK(hops[1] == "10.0.0.1");
    CHECK(h.get_all("set-cookie").size() == 2);
    CHECK(h.get_all("missing").empty());
}