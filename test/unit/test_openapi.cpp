// openapi/: the OAS document model, the JSON emitter, and the typed-route
// collection (route<Req, Res> -> OpenApiSpec) plus the served endpoints.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <string>

#include "simple_http.h"
#include "test_support.h"

using namespace simple_http;
using namespace simple_http::test;

namespace {

struct UserProfile {
    std::int64_t id{};
    std::string name;
};

struct CreateUserReq {
    std::string name;
    int age{};
};

struct ErrorBody {
    std::string error;
};

RequestPtr make_request(asio::io_context &ctx, std::string path, Method method = Method::Get) {
    auto req = std::make_shared<Request>(Version::Http11, ctx.get_executor(), asio::ip::tcp::endpoint{});
    req->set_target(std::move(path));
    req->set_method(method);
    req->body().finish();
    return req;
}

std::shared_ptr<FakeResponseSink> dispatch(asio::io_context &ctx, Router &router, const std::string &path,
                                             Method method = Method::Get) {
    auto writer = std::make_shared<FakeResponseSink>();
    auto res = std::make_shared<ResponseWriter>(writer);
    REQUIRE(run_on(ctx, router.dispatch(make_request(ctx, path, method), res, std::nullopt)));
    return writer;
}

} // namespace

TEST_CASE("openapi: the JSON emitter renders a fixed shape", "[openapi]") {
    openapi::JsonOut j;
    j.begin_object();
    j.key("openapi");
    j.str("3.0.3");
    j.key("info");
    j.begin_object();
    j.key("title");
    j.str("API");
    j.end_object();
    j.key("tags");
    j.begin_array();
    j.str("a");
    j.str("b");
    j.end_array();
    j.key("body");
    j.raw(R"({"type":"object"})"); // a schema: spliced, not quoted
    j.key("on");
    j.boolean(true);
    j.end_object();

    CHECK(std::move(j).take() ==
          R"({"openapi":"3.0.3","info":{"title":"API"},"tags":["a","b"],"body":{"type":"object"},"on":true})");
}

TEST_CASE("openapi: OpenApiSpec renders an OAS 3.0 document", "[openapi]") {
    openapi::OpenApiSpec spec;
    spec.title("petshop").version("1.0.0").server("https://api.example");
    spec.add_operation({Method::Get}, "/pets", openapi::OperationInfo{}, "", R"({"type":"object"})");
    spec.add_operation({Method::Post}, "/pets", openapi::OperationInfo{"create a pet", "pets"}, R"({"type":"object"})",
                       R"({"type":"object"})");

    const std::string doc = spec.render_json();
    const std::string expected =
        R"({"openapi":"3.1.0","info":{"title":"petshop","version":"1.0.0"},)"
        R"("servers":[{"url":"https://api.example"}],"paths":{"/pets":{)"
        R"("get":{"responses":{"200":{"description":"OK","content":{"application/json":{"schema":{"type":"object"}}}}}},)"
        R"("post":{"tags":["pets"],"summary":"create a pet",)"
        R"("requestBody":{"required":true,"content":{"application/json":{"schema":{"type":"object"}}}},)"
        R"("responses":{"200":{"description":"OK","content":{"application/json":{"schema":{"type":"object"}}}}}}}}})";
    CHECK(doc == expected);
}

TEST_CASE("openapi: typed routes register normally and collect schemas", "[openapi]") {
    Router router;
    router.route<UserProfile>({Method::Get}, "/users/{id}", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).send("user");
    });
    router.route<CreateUserReq, UserProfile>(
        {Method::Post}, "/users",
        [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(201).send("created"); },
        openapi::OperationInfo{"create a user", "user"},
        openapi::query<std::string>("origin", {.description = "where from"}),
        openapi::resp<ErrorBody>(400, "invalid request body"), openapi::resp<ErrorBody>(409, "name already taken"));

    // The registration is a normal route: dispatch works exactly as untyped.
    asio::io_context ctx;
    CHECK(dispatch(ctx, router, "/users/{id}", Method::Get)->last_body == "user");
    CHECK(dispatch(ctx, router, "/users", Method::Post)->last_status == 201);
    CHECK(dispatch(ctx, router, "/users", Method::Post)->last_body == "created");
    // A wrong method is still the router's 405.
    CHECK(dispatch(ctx, router, "/users", Method::Get)->last_status == status::method_not_allowed);

    // The document collected the operations and their glaze schemas.
    const std::string doc = router.openapi().render_json();
    CHECK(doc.find(R"("/users/{id}")") != std::string::npos);
    CHECK(doc.find(R"("get")") != std::string::npos);
    CHECK(doc.find(R"("post")") != std::string::npos);
    CHECK(doc.find(R"("summary":"create a user")") != std::string::npos);
    CHECK(doc.find(R"("requestBody")") != std::string::npos);
    // The utoipa-style trailing annotations: the query parameter and the two
    // error responses all reached the document.
    CHECK(doc.find(R"("parameters":[{"name":"origin","in":"query","description":"where from","schema":)") !=
          std::string::npos);
    CHECK(doc.find(R"("400":{"description":"invalid request body")") != std::string::npos);
    CHECK(doc.find(R"("409":{"description":"name already taken")") != std::string::npos);
    // A glaze schema with the reflected fields is embedded (raw, unescaped).
    CHECK(doc.find("\"properties\"") != std::string::npos);
    CHECK(doc.find(R"("name")") != std::string::npos);
    CHECK(doc.find(R"("age")") != std::string::npos);
}

TEST_CASE("openapi: success status and multiple error responses render", "[openapi]") {
    openapi::OpenApiSpec spec;
    spec.add_operation({Method::Post}, "/users", openapi::OperationInfo{"create", "users", 201}, R"({"type":"object"})",
                       R"({"type":"object"})", {},
                       {openapi::Response{400, "invalid request body", "application/json", R"({"type":"object"})"},
                        openapi::Response{409, "name already taken", "application/json", R"({"type":"object"})"}});

    const std::string doc = spec.render_json();
    // The success response uses the declared 201 (with the library's reason
    // phrase for it), and every error response gets its own entry, in
    // registration order.
    const std::string expected =
        R"({"openapi":"3.1.0","info":{"title":"API","version":"0.0.0"},"paths":{"/users":{)"
        R"("post":{"tags":["users"],"summary":"create",)"
        R"("requestBody":{"required":true,"content":{"application/json":{"schema":{"type":"object"}}}},)"
        R"("responses":{"201":{"description":"Created","content":{"application/json":{"schema":{"type":"object"}}}},)"
        R"("400":{"description":"invalid request body","content":{"application/json":{"schema":{"type":"object"}}}},)"
        R"("409":{"description":"name already taken","content":{"application/json":{"schema":{"type":"object"}}}}}}}}})";
    CHECK(doc == expected);
}

TEST_CASE("openapi: parameters and security render utoipa-style", "[openapi]") {
    openapi::OpenApiSpec spec;
    spec.security_scheme("bearer", openapi::bearer());
    spec.add_operation({Method::Get}, "/users",
                       openapi::OperationInfo{.summary = "list users",
                                              .tag = "user",
                                              .description = "page through users",
                                              .operation_id = "listUsers",
                                              .security = {"bearer"}},
                       "", R"({"type":"object"})",
                       {openapi::query<std::string>("q", {.description = "search term"}),
                        openapi::header<std::string>("x-token", {.required = true})},
                       {openapi::resp<ErrorBody>(400, "bad request")});

    const std::string doc = spec.render_json();
    // The security scheme is declared in components and referenced by the
    // operation; parameters render name/in/required/description/schema.
    CHECK(doc.find(R"("components":{"securitySchemes":{"bearer":{"type":"http","scheme":"bearer"}}})") !=
          std::string::npos);
    CHECK(doc.find(R"("security":[{"bearer":[]}])") != std::string::npos);
    CHECK(doc.find(R"("operationId":"listUsers")") != std::string::npos);
    CHECK(doc.find(R"("parameters":[{"name":"q","in":"query","description":"search term","schema":)") !=
          std::string::npos);
    CHECK(doc.find(R"({"name":"x-token","in":"header","required":true)") != std::string::npos);
}

TEST_CASE("openapi: document and Swagger UI are served", "[openapi]") {
    Router router;
    router.route<UserProfile>({Method::Get}, "/users/{id}", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).send("user");
    });
    router.serve_openapi("/openapi.json");
    router.serve_swagger_ui("/swagger", "/openapi.json");

    asio::io_context ctx;
    {
        auto writer = dispatch(ctx, router, "/openapi.json");
        CHECK(writer->last_status == status::ok);
        CHECK(writer->last_body.rfind(R"({"openapi":"3.1.0")", 0) == 0);
        CHECK(writer->last_body.find(R"("/users/{id}")") != std::string::npos);
        CHECK(writer->last_body.find(R"("properties")") != std::string::npos); // a reflected glaze schema
    }
    {
        auto writer = dispatch(ctx, router, "/swagger");
        CHECK(writer->last_status == status::ok);
        CHECK(writer->last_body.find("SwaggerUIBundle") != std::string::npos);
        CHECK(writer->last_body.find("/openapi.json") != std::string::npos);
    }
    // Missing on the quiet paths: nothing else was registered, so 404.
    CHECK(dispatch(ctx, router, "/other")->last_status == status::not_found);
}