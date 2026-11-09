// C++23 build of the C++17 include root: exercises the glaze-gated surface
// (typed params, JSON bodies, OpenAPI schema generation, JWT/JWKS).
//
// This target is deliberately compiled with -std=c++23 and linked against
// simple_http_cpp17 plus glaze. A plain C++17 build compiles the same headers
// with these helpers absent (see core/glaze_support.h).

#include <simple_http.h>

#include <boost/asio/io_context.hpp>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

#include <openssl/evp.h>
#include <openssl/hmac.h>

static_assert(__cplusplus > 202002L, "glaze smoke must build as C++23");
static_assert(SIMPLE_HTTP_HAS_GLAZE == 1, "glaze helpers must be enabled in a C++23 build");

struct Pet {
    std::int64_t id{};
    std::string name;
};

struct NewPet {
    std::string name;
    std::int64_t age{};
};

struct Q {
    std::string q;
    std::int64_t page{};
};

template <> struct glz::meta<Pet> {
    using T = Pet;
    static constexpr auto value = glz::object("id", &T::id, "name", &T::name);
};

template <> struct glz::meta<NewPet> {
    using T = NewPet;
    static constexpr auto value = glz::object("name", &T::name, "age", &T::age);
};

template <> struct glz::meta<Q> {
    using T = Q;
    static constexpr auto value = glz::object("q", &T::q, "page", &T::page);
};

namespace {

int failures = 0;

void check(bool condition, const char *what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

// Records the one response a test writes.
struct MockSink final : simple_http::ResponseSink {
    explicit MockSink(boost::asio::any_io_executor exec) : m_exec(std::move(exec)) {}

    void send(int status, simple_http::Headers, std::string body, simple_http::WriteCompletion done) override {
        status_code = status;
        response_body = std::move(body);
        std::move(done)(simple_http::error_code{});
    }
    void send_headers(int status, simple_http::Headers, simple_http::WriteCompletion done) override {
        status_code = status;
        std::move(done)(simple_http::error_code{});
    }
    void send_chunk(std::string, simple_http::WriteCompletion done) override {
        std::move(done)(simple_http::error_code{});
    }
    void send_last(std::string, simple_http::WriteCompletion done) override {
        std::move(done)(simple_http::error_code{});
    }
    void send_bodyless(int status, simple_http::Headers, simple_http::WriteCompletion done) override {
        status_code = status;
        std::move(done)(simple_http::error_code{});
    }
    void connected(simple_http::ConnectedCompletion done) const override {
        std::move(done)(simple_http::error_code{}, true);
    }
    void close(simple_http::WriteCompletion done) override { std::move(done)(simple_http::error_code{}); }
    boost::asio::any_io_executor get_executor() const override { return m_exec; }
    simple_http::Version version() const override { return simple_http::Version::Http11; }

    boost::asio::any_io_executor m_exec;
    int status_code{0};
    std::string response_body;
};

void test_schema_json() {
    const std::string schema = simple_http::openapi::schema_json<Pet>();
    check(contains(schema, "\"type\":\"object\""), "schema_json: object");
    check(contains(schema, "\"id\":{\"type\":\"integer\"}"), "schema_json: id integer");
    check(contains(schema, "\"name\":{\"type\":\"string\"}"), "schema_json: name string");
}

void test_typed_query_params(boost::asio::io_context &ctx) {
    simple_http::Request req{simple_http::Version::Http11, ctx.get_executor()};
    req.set_target("/search?q=hello&page=3");
    auto params = simple_http::query_params<Q>(req);
    check(params && params->q == "hello" && params->page == 3, "query_params<Q>");
    simple_http::Request empty{simple_http::Version::Http11, ctx.get_executor()};
    empty.set_target("/search");
    check(!simple_http::query_params<Q>(empty).has_value(), "query_params<Q> missing values -> nullopt");
}

void test_openapi_document() {
    simple_http::openapi::OpenApiSpec spec;
    spec.title("petshop").version("1.0.0");
    auto op = simple_http::openapi::doc()
                  .request_body<NewPet>()
                  .response<Pet>(201)
                  .summary("create a pet")
                  .operation_id("createPet");
    simple_http::openapi::record_operation(spec, op, {simple_http::Method::Post}, "/pets");
    const std::string json = spec.render_json();
    check(contains(json, "\"openapi\":\"3.1.0\""), "openapi: version");
    check(contains(json, "\"/pets\""), "openapi: path");
    check(contains(json, "\"post\""), "openapi: method");
    check(contains(json, "\"201\""), "openapi: success status");
    check(contains(json, "\"operationId\":\"createPet\""), "openapi: operationId");
    check(contains(json, "\"$ref\":\"#/components/schemas/schema_"), "openapi: hoisted schemas");
}

void test_read_json_body(boost::asio::io_context &ctx) {
    simple_http::Request req{simple_http::Version::Http11, ctx.get_executor()};
    req.body().feed(std::string{R"({"id":7,"name":"rex"})"});
    req.body().finish();

    simple_http::result<Pet> value{boost::system::errc::make_error_code(boost::system::errc::invalid_argument)};
    bool called = false;
    simple_http::async_read_json_body<Pet>(req, [&](simple_http::error_code ec, simple_http::result<Pet> parsed) {
        called = true;
        check(!ec, "async_read_json_body: no error");
        value = std::move(parsed);
    });
    ctx.restart();
    ctx.run();
    check(called && value && value->id == 7 && value->name == "rex", "async_read_json_body<Pet>");
}

void test_write_json(boost::asio::io_context &ctx) {
    auto sink = std::make_shared<MockSink>(ctx.get_executor());
    auto writer = std::make_shared<simple_http::ResponseWriter>(sink);
    Pet pet{9, "milo"};
    simple_http::async_write_json(writer, pet, 201, [](simple_http::error_code) {});
    ctx.restart();
    ctx.run();
    check(sink->status_code == 201, "async_write_json: status");
    check(contains(sink->response_body, "\"id\":9") && contains(sink->response_body, "\"milo\""),
          "async_write_json: body");
}

void test_extract_query(boost::asio::io_context &ctx) {
    auto sink = std::make_shared<MockSink>(ctx.get_executor());
    auto res = std::make_shared<simple_http::ResponseWriter>(sink);
    auto req = std::make_shared<simple_http::Request>(simple_http::Version::Http11, ctx.get_executor());
    req->set_target("/x?q=abc&page=5");

    bool ran = false;
    auto handler = simple_http::extract<simple_http::Query<Q>>(
        [&](const Q &q, simple_http::RequestPtr, simple_http::ResponsePtr, simple_http::HandlerCompletion done) {
            ran = q.q == "abc" && q.page == 5;
            std::move(done)();
        });
    bool completed = false;
    simple_http::invoke_handler(handler, req, res, std::nullopt, [&] { completed = true; });
    ctx.restart();
    ctx.run();
    check(ran && completed, "extract<Query<Q>> runs the handler with the values");
}

std::string make_hs256_token(std::string_view secret, std::string_view payload_json, std::string_view alg) {
    const std::string header = R"({"alg":")" + std::string{alg} + R"(","typ":"JWT"})";
    const std::string signing_input =
        simple_http::base64_url_encode(header) + "." + simple_http::base64_url_encode(payload_json);
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned int length = 0;
    HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char *>(signing_input.data()), signing_input.size(), digest, &length);
    return signing_input + "." +
           simple_http::base64_url_encode(
               std::string_view{reinterpret_cast<const char *>(digest), static_cast<std::size_t>(length)});
}

void test_jwt(boost::asio::io_context &ctx) {
    const std::int64_t now = simple_http::now_unix();
    const std::string payload =
        R"({"sub":"u1","scope":"users:write admin","iss":"iss","aud":"aud","exp":)" + std::to_string(now + 3600) + "}";
    const std::string token = make_hs256_token("secret", payload, "HS256");

    simple_http::JwtConfig config;
    config.secret = "secret";
    config.issuer = "iss";
    config.audience = "aud";
    auto mw = simple_http::middleware::jwt(config);

    auto sink = std::make_shared<MockSink>(ctx.get_executor());
    auto res = std::make_shared<simple_http::ResponseWriter>(sink);
    auto req = std::make_shared<simple_http::Request>(simple_http::Version::Http11, ctx.get_executor());
    req->mutable_headers().add("authorization", "Bearer " + token);

    bool next_called = false;
    auto next = [&](simple_http::RequestPtr request, simple_http::ResponsePtr, simple_http::SslHandle,
                    simple_http::HandlerCompletion done) {
        next_called = true;
        const auto *identity = simple_http::principal(*request);
        check(identity != nullptr && identity->subject == "u1", "jwt: subject");
        check(identity != nullptr && simple_http::jwt_detail::has_scope(*identity, "users:write"), "jwt: scope split");
        std::move(done)();
    };
    bool completed = false;
    mw(req, res, std::nullopt, next, [&] { completed = true; });
    ctx.restart();
    ctx.run();
    check(next_called && completed, "jwt middleware accepts a valid HS256 token");

    // An expired token is rejected with 401.
    const std::string expired = make_hs256_token(
        "secret", R"({"sub":"u1","iss":"iss","aud":"aud","exp":)" + std::to_string(now - 3600) + "}", "HS256");
    auto sink2 = std::make_shared<MockSink>(ctx.get_executor());
    auto res2 = std::make_shared<simple_http::ResponseWriter>(sink2);
    auto req2 = std::make_shared<simple_http::Request>(simple_http::Version::Http11, ctx.get_executor());
    req2->mutable_headers().add("authorization", "Bearer " + expired);
    bool rejected_next = false;
    mw(
        req2, res2, std::nullopt,
        [&](simple_http::RequestPtr, simple_http::ResponsePtr, simple_http::SslHandle,
            simple_http::HandlerCompletion done) {
            rejected_next = true;
            std::move(done)();
        },
        [] {});
    ctx.restart();
    ctx.run();
    check(!rejected_next && sink2->status_code == simple_http::status::unauthorized,
          "jwt middleware rejects an expired token");

    // A token with alg=none is never accepted, even if the allowlist is widened.
    simple_http::JwtConfig none_config;
    none_config.algorithms = {"HS256", "none"};
    const std::string unsigned_token =
        simple_http::base64_url_encode(R"({"alg":"none","typ":"JWT"})") + "." +
        simple_http::base64_url_encode(R"({"sub":"u1","exp":)" + std::to_string(now + 60) + "}") + ".";
    auto sink3 = std::make_shared<MockSink>(ctx.get_executor());
    auto res3 = std::make_shared<simple_http::ResponseWriter>(sink3);
    auto req3 = std::make_shared<simple_http::Request>(simple_http::Version::Http11, ctx.get_executor());
    req3->mutable_headers().add("authorization", "Bearer " + unsigned_token);
    simple_http::middleware::jwt(none_config)(
        req3, res3, std::nullopt,
        [](simple_http::RequestPtr, simple_http::ResponsePtr, simple_http::SslHandle,
           simple_http::HandlerCompletion done) { std::move(done)(); },
        [] {});
    ctx.restart();
    ctx.run();
    check(sink3->status_code == simple_http::status::unauthorized, "jwt middleware rejects alg=none");
}

void test_handler_adaptation(boost::asio::io_context &ctx) {
    auto sink = std::make_shared<MockSink>(ctx.get_executor());
    auto res = std::make_shared<simple_http::ResponseWriter>(sink);
    auto req = std::make_shared<simple_http::Request>(simple_http::Version::Http11, ctx.get_executor());

    bool sync_done = false;
    auto sync = simple_http::make_handler([](simple_http::RequestPtr, simple_http::ResponsePtr) {});
    simple_http::invoke_handler(sync, req, res, std::nullopt, [&] { sync_done = true; });
    check(sync_done, "make_handler adapts a synchronous void handler");

    bool callback_done = false;
    auto callback = simple_http::make_handler([](simple_http::RequestPtr, simple_http::ResponsePtr,
                                                 simple_http::HandlerCompletion done) { std::move(done)(); });
    simple_http::invoke_handler(callback, req, res, std::nullopt, [&] { callback_done = true; });
    check(callback_done, "make_handler adapts a callback handler");

    bool coro_done = false;
    auto coro = simple_http::make_handler(
        [](simple_http::RequestPtr, simple_http::ResponsePtr) -> boost::asio::awaitable<void> { co_return; });
    simple_http::invoke_handler(coro, req, res, std::nullopt, [&] { coro_done = true; });
    ctx.restart();
    ctx.run();
    check(coro_done, "make_handler adapts a coroutine handler");
}

void test_jwks() {
    const std::string document = R"({"keys":[{"kty":"oct","kid":"k1","alg":"HS256","k":")" +
                                 simple_http::base64_url_encode("secret") + R"("}]})";
    auto parsed = simple_http::Jwks::parse(document);
    check(parsed.has_value(), "Jwks::parse oct key");
    if (!parsed) {
        return;
    }
    check(parsed->size() == 1, "Jwks::size");
    const auto *key = parsed->find("k1", "HS256");
    check(key != nullptr && key->secret == "secret", "Jwks::find by kid");
    check(parsed->find("nope", "HS256") == nullptr, "Jwks::find unknown kid");
}

} // namespace

int main() {
    boost::asio::io_context ctx;

    test_schema_json();
    test_typed_query_params(ctx);
    test_openapi_document();
    test_read_json_body(ctx);
    test_write_json(ctx);
    test_extract_query(ctx);
    test_jwt(ctx);
    test_jwks();
    test_handler_adaptation(ctx);

    if (failures != 0) {
        std::fprintf(stderr, "cpp17 glaze smoke: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("cpp17 glaze smoke: PASS\n");
    return 0;
}
