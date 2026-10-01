// proto/json.h (SIMPLE_HTTP_ENABLE_JSON): glaze-backed read_json_body /
// write_json — the axum Json<T> extractor / Go json.Marshal equivalent.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <string>

#include "simple_http.h"
#include "test_support.h"

using namespace simple_http;
using namespace simple_http::test;

namespace {

struct Pet {
    std::int64_t id{};
    std::string name;
};

} // namespace

TEST_CASE("proto/json: read_json_body parses a struct from the body", "[json]") {
    asio::io_context ctx;
    auto req = std::make_shared<Request>(Version::Http11, ctx.get_executor(), asio::ip::tcp::endpoint{});
    req->set_target("/pets");
    req->set_method(Method::Post);
    (void)req->body().feed(R"({"id":7,"name":"rex"})");
    req->body().finish();

    auto pet = run_on(ctx, read_json_body<Pet>(*req));
    REQUIRE(pet.has_value());
    REQUIRE(pet->has_value());
    CHECK((*pet)->id == 7);
    CHECK((*pet)->name == "rex");
}

TEST_CASE("proto/json: a malformed or type-mismatched body is std::unexpected", "[json]") {
    asio::io_context ctx;
    for (const char *bad : {R"({not json)", R"({"id":"seven","name":"x"})"}) {
        auto req = std::make_shared<Request>(Version::Http11, ctx.get_executor(), asio::ip::tcp::endpoint{});
        req->set_target("/pets");
        req->set_method(Method::Post);
        (void)req->body().feed(bad);
        req->body().finish();

        auto pet = run_on(ctx, read_json_body<Pet>(*req));
        REQUIRE(pet.has_value());      // the coroutine itself completed
        CHECK_FALSE(pet->has_value()); // the parse failed
    }
}

TEST_CASE("proto/json: write_json sends a one-shot JSON response", "[json]") {
    asio::io_context ctx;
    auto writer = std::make_shared<FakeResponseSink>();
    auto res = std::make_shared<ResponseWriter>(writer);

    auto ec = run_on(ctx, write_json(res, Pet{3, "yuki"}, 201));
    REQUIRE(ec.has_value());
    CHECK(*ec == error_code{});
    CHECK(writer->last_status == 201);
    CHECK(writer->header(field::content_type) == mime::app_json);
    CHECK(writer->last_body == R"({"id":3,"name":"yuki"})");
}