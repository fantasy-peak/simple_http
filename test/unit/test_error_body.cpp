#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>

#include "simple_http/proto/error_body.h"

namespace {

std::string serialize(const auto &value) {
    auto out = glz::write_json(value);
    REQUIRE(out);
    return *out;
}

} // namespace

TEST_CASE("error_body: code and message are serialized", "[proto][json]") {
    const simple_http::JsonError body{"not_found", "no such pet", std::nullopt};
    const auto json = serialize(body);
    CHECK(json.find("\"code\":\"not_found\"") != std::string::npos);
    CHECK(json.find("\"message\":\"no such pet\"") != std::string::npos);
}

TEST_CASE("error_body: details is included when present", "[proto][json]") {
    const simple_http::JsonError body{"bad_request", "invalid", std::string{"field x"}};
    const auto json = serialize(body);
    CHECK(json.find("\"details\":\"field x\"") != std::string::npos);
}

TEST_CASE("problem_details: RFC 9457 members are serialized", "[proto][json]") {
    simple_http::ProblemDetails problem{"https://example.test/errors/oops", "Oops", 422, "the body is invalid",
                                        "/pets/1"};
    const auto json = serialize(problem);
    CHECK(json.find("\"type\":\"https://example.test/errors/oops\"") != std::string::npos);
    CHECK(json.find("\"title\":\"Oops\"") != std::string::npos);
    CHECK(json.find("\"status\":422") != std::string::npos);
    CHECK(json.find("\"detail\":\"the body is invalid\"") != std::string::npos);
    CHECK(json.find("\"instance\":\"/pets/1\"") != std::string::npos);
}
