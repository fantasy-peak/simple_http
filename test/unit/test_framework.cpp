// handler/: application state, typed extractors, validation, host routing and
// {*rest} catch-all templates. These are registration/dispatch semantics, so
// the tests drive them through the same FakeResponseSink the router tests use.

#include <catch2/catch_test_macros.hpp>

#include <openssl/bn.h>
#include <openssl/buffer.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <memory>
#include <string>

#include "simple_http.h"
#include "test_support.h"

using namespace simple_http;
using namespace simple_http::test;

namespace {

struct AppState {
    std::string name;
};

struct UserParams {
    int id{};
};

struct CreateUser {
    std::string name;
    int age{};
};

struct ListQuery {
    std::string q;
    int page{};
};

struct RestParams {
    std::string rest;
};

RequestPtr make_request(asio::io_context &ctx, std::string path, Method method = Method::Get) {
    auto req = std::make_shared<Request>(Version::Http11, ctx.get_executor(), asio::ip::tcp::endpoint{});
    req->set_target(std::move(path));
    req->set_method(method);
    req->body().finish();
    return req;
}

RequestPtr make_request(asio::io_context &ctx, std::string path, std::string host, Method method = Method::Get) {
    auto req = make_request(ctx, std::move(path), method);
    req->mutable_headers().add("host", std::move(host));
    return req;
}

void set_body(RequestPtr &req, std::string body) {
    REQUIRE(req->body().feed(std::move(body)));
    req->body().finish();
}

Handler text_handler(std::string text) {
    return make_handler([text = std::move(text)](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(status::ok).async_send(text, asio::use_awaitable);
    });
}

std::shared_ptr<FakeResponseSink> dispatch(asio::io_context &ctx, Router &router, RequestPtr req) {
    auto sink = std::make_shared<FakeResponseSink>(ctx.get_executor());
    auto res = std::make_shared<ResponseWriter>(sink);
    REQUIRE(run_on(ctx, await_dispatch(router, req, res)));
    return sink;
}

} // namespace

// The Validator specialization must live in simple_http, next to the primary
// template; the test types above are in the anonymous namespace but visible.
namespace simple_http {
template <> struct Validator<CreateUser> {
    static std::expected<void, std::vector<FieldError>> validate(const CreateUser &value) {
        std::vector<FieldError> errors;
        push_if(errors, required_field("name", !value.name.empty()));
        push_if(errors, range("age", value.age, 1, 150));
        return errors.empty() ? std::expected<void, std::vector<FieldError>>{} : std::unexpected(std::move(errors));
    }
};
} // namespace simple_http

TEST_CASE("framework: application state is shared, type-keyed and nullable", "[framework][state]") {
    asio::io_context ctx;
    Router router;
    router.state(std::make_shared<AppState>(AppState{"shared"}));
    router.route({Method::Get}, "/state", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        const AppState *state = req->app_state<AppState>();
        co_await res->status(status::ok).async_send(state ? state->name : "missing", asio::use_awaitable);
    });

    CHECK(dispatch(ctx, router, make_request(ctx, "/state"))->last_body == "shared");

    Router without;
    without.route({Method::Get}, "/state", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(status::ok).async_send(req->app_state<AppState>() ? "present" : "missing", asio::use_awaitable);
    });
    CHECK(dispatch(ctx, without, make_request(ctx, "/state"))->last_body == "missing");
}

TEST_CASE("framework: host routing uses exact, wildcard and fallback tiers", "[framework][host]") {
    asio::io_context ctx;
    Router router;
    router.route({Method::Get}, "/who", text_handler("root"));
    router.host("api.example.com", [](Router &api) { api.route({Method::Get}, "/who", text_handler("api")); });
    router.host("*.example.com", [](Router &sub) { sub.route({Method::Get}, "/who", text_handler("sub")); });
    router.host("api.example.com:8080", [](Router &port) { port.route({Method::Get}, "/who", text_handler("port")); });

    CHECK(dispatch(ctx, router, make_request(ctx, "/who", "api.example.com"))->last_body == "api");
    CHECK(dispatch(ctx, router, make_request(ctx, "/who", "API.EXAMPLE.COM."))->last_body == "api");
    CHECK(dispatch(ctx, router, make_request(ctx, "/who", "other.example.com"))->last_body == "sub");
    CHECK(dispatch(ctx, router, make_request(ctx, "/who", "api.example.com:8080"))->last_body == "port");
    // The suffix wildcard must not match the bare domain or a look-alike.
    CHECK(dispatch(ctx, router, make_request(ctx, "/who", "example.com"))->last_body == "root");
    CHECK(dispatch(ctx, router, make_request(ctx, "/who", "evil-example.com"))->last_body == "root");
}

TEST_CASE("framework: host child routers share application state", "[framework][host][state]") {
    asio::io_context ctx;
    Router router;
    router.state(std::make_shared<AppState>(AppState{"from-root"}));
    router.host("api.example.com", [](Router &api) {
        api.route({Method::Get}, "/state", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
            const AppState *state = req->app_state<AppState>();
            co_await res->status(status::ok).async_send(state ? state->name : "missing", asio::use_awaitable);
        });
    });

    CHECK(dispatch(ctx, router, make_request(ctx, "/state", "api.example.com"))->last_body == "from-root");
}

TEST_CASE("framework: {*rest} captures the remaining path", "[framework][wildcard]") {
    asio::io_context ctx;
    Router router;
    router.route({Method::Get}, "/files/static", text_handler("static"));
    router.route({Method::Get}, "/files/{id}", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(status::ok).async_send("id=" + std::string{*req->param("id")}, asio::use_awaitable);
    });
    router.route({Method::Get}, "/files/{*rest}", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        auto rest = req->param("rest");
        co_await res->status(status::ok).async_send("rest=" + std::string{rest.value_or("")}, asio::use_awaitable);
    });

    CHECK(dispatch(ctx, router, make_request(ctx, "/files/static"))->last_body == "static");
    CHECK(dispatch(ctx, router, make_request(ctx, "/files/42"))->last_body == "id=42");
    CHECK(dispatch(ctx, router, make_request(ctx, "/files/a/b/c"))->last_body == "rest=a/b/c");
    CHECK(dispatch(ctx, router, make_request(ctx, "/files/"))->last_body == "rest=");
}

TEST_CASE("framework: Path/Query/State extractors run before the handler", "[framework][extract]") {
    asio::io_context ctx;
    Router router;
    router.state(std::make_shared<AppState>(AppState{"state"}));

    router.route({Method::Get}, "/users/{id}",
                 extract<Path<UserParams>, State<AppState>>([](const UserParams &params, AppState *state, RequestPtr,
                                                               ResponsePtr res) -> asio::awaitable<void> {
                     co_await res->status(status::ok).async_send(std::to_string(params.id) + ":" + state->name, asio::use_awaitable);
                 }));

    router.route(
        {Method::Get}, "/list",
        extract<Query<ListQuery>>([](const ListQuery &query, RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
            co_await res->status(status::ok).async_send(query.q + ":" + std::to_string(query.page), asio::use_awaitable);
        }));

    CHECK(dispatch(ctx, router, make_request(ctx, "/users/42"))->last_body == "42:state");
    CHECK(dispatch(ctx, router, make_request(ctx, "/list?q=x&page=2"))->last_body == "x:2");

    // A missing path capture is a 400, and the handler is not entered.
    CHECK(dispatch(ctx, router, make_request(ctx, "/users/not-an-int"))->last_status == status::bad_request);

    // The State extractor is a configuration error when the type is absent.
    Router no_state;
    no_state.route(
        {Method::Get}, "/state",
        extract<State<AppState>>([](AppState *, RequestPtr, ResponsePtr) -> asio::awaitable<void> { co_return; }));
    CHECK(dispatch(ctx, no_state, make_request(ctx, "/state"))->last_status == status::internal_server_error);
}

TEST_CASE("framework: Header and Cookie extractors parse typed values", "[framework][extract]") {
    asio::io_context ctx;
    Router router;
    router.route({Method::Get}, "/headers",
                 extract<Header<int, "x-page">, Cookie<std::string, "sid">>(
                     [](int page, std::string sid, RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                         co_await res->status(status::ok).async_send(std::to_string(page) + ":" + sid, asio::use_awaitable);
                     }));

    auto req = make_request(ctx, "/headers");
    req->mutable_headers().add("x-page", "3");
    req->mutable_headers().add("cookie", "sid=abc");
    CHECK(dispatch(ctx, router, req)->last_body == "3:abc");
}

TEST_CASE("framework: extract supports a trailing SslHandle parameter", "[framework][extract]") {
    asio::io_context ctx;
    Router router;
    router.route({Method::Get}, "/users/{id}",
                 extract<Path<UserParams>>(
                     [](const UserParams &params, RequestPtr, ResponsePtr res, SslHandle ssl) -> asio::awaitable<void> {
                         co_await res->status(status::ok).async_send(std::to_string(params.id) + (ssl ? ":tls" : ":plain"), asio::use_awaitable);
                     }));
    CHECK(dispatch(ctx, router, make_request(ctx, "/users/7"))->last_body == "7:plain");
}

TEST_CASE("framework: Json/Form extractors consume the body once", "[framework][extract][json]") {
    asio::io_context ctx;
    Router router;
    router.route({Method::Post}, "/json",
                 extract<Json<CreateUser>>([](CreateUser user, RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                     co_await res->status(status::created).async_send(user.name + ":" + std::to_string(user.age), asio::use_awaitable);
                 }));
    router.route({Method::Post}, "/form",
                 extract<Form<CreateUser>>([](CreateUser user, RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                     co_await res->status(status::created).async_send(user.name + ":" + std::to_string(user.age), asio::use_awaitable);
                 }));

    auto json = make_request(ctx, "/json", Method::Post);
    set_body(json, R"({"name":"ada","age":36})");
    CHECK(dispatch(ctx, router, json)->last_body == "ada:36");

    auto form = make_request(ctx, "/form", Method::Post);
    set_body(form, "name=ada&age=36");
    CHECK(dispatch(ctx, router, form)->last_body == "ada:36");

    auto bad = make_request(ctx, "/json", Method::Post);
    set_body(bad, "{not json");
    CHECK(dispatch(ctx, router, bad)->last_status == status::bad_request);
}

TEST_CASE("framework: Valid<E> turns a Validator failure into 422 with details", "[framework][validation]") {
    asio::io_context ctx;
    Router router;
    router.route(
        {Method::Post}, "/users",
        extract<Valid<Json<CreateUser>>>([](CreateUser user, RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
            co_await res->status(status::created).async_send(user.name, asio::use_awaitable);
        }));

    auto ok = make_request(ctx, "/users", Method::Post);
    set_body(ok, R"({"name":"ada","age":36})");
    CHECK(dispatch(ctx, router, ok)->last_status == status::created);

    auto bad = make_request(ctx, "/users", Method::Post);
    set_body(bad, R"({"name":"ada","age":0})");
    auto sink = dispatch(ctx, router, bad);
    CHECK(sink->last_status == status::unprocessable_content);
    CHECK(sink->last_body.find("validation_failed") != std::string::npos);
    CHECK(sink->last_body.find("age") != std::string::npos);
}

TEST_CASE("framework: a catch-all route still validates its OpenAPI path params", "[framework][wildcard][openapi]") {
    Router router;
    router.route({Method::Get}, "/files/{*rest}", text_handler("ok"), openapi::doc().path_params<RestParams>());
    router.verify_openapi();
    // Registration validates {name} against the Params fields; a mismatch is
    // skipped. If this route registered, dispatch reaches its handler.
    asio::io_context ctx;
    CHECK(dispatch(ctx, router, make_request(ctx, "/files/a/b"))->last_body == "ok");
}

namespace {

std::string cookie_value(std::string_view header) {
    const auto eq = header.find('=');
    if (eq == std::string_view::npos) {
        return {};
    }
    const auto semi = header.find(';', eq + 1);
    return std::string{header.substr(eq + 1, semi == std::string_view::npos ? std::string_view::npos : semi - eq - 1)};
}

// Rotation can emit a rolling cookie before the handler runs and a new one when
// regenerate_id() is called; the browser applies the last one, so the test must
// read the last Set-Cookie field.
std::string last_cookie_value(const FakeResponseSink &sink) {
    const auto values = sink.last_headers.get_all("set-cookie");
    return values.empty() ? std::string{} : cookie_value(values.back());
}

} // namespace

TEST_CASE("framework: session creates, loads and persists server-side data", "[framework][session]") {
    asio::io_context ctx;
    auto store = std::make_shared<MemorySessionStore>();
    Router router;
    router.use(middleware::session(store, SessionConfig{.cookie_name = "sid", .secure = false}));
    router.route({Method::Post}, "/set", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        auto *s = session(*req);
        if (!s) {
            co_await res->status(status::internal_server_error).async_send("no session", asio::use_awaitable);
            co_return;
        }
        s->set("uid", "42");
        co_await res->status(status::no_content).async_send("", asio::use_awaitable);
    });
    router.route({Method::Get}, "/get", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        auto *s = session(*req);
        auto uid = s ? s->get("uid") : std::nullopt;
        co_await res->status(status::ok).async_send(uid ? std::string{*uid} : "none", asio::use_awaitable);
    });
    router.route({Method::Get}, "/rotate", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        auto *s = session(*req);
        if (s) {
            s->regenerate_id();
            s->set("uid", "99");
        }
        co_await res->status(status::ok).async_send("rotated", asio::use_awaitable);
    });

    auto first = make_request(ctx, "/set", Method::Post);
    auto first_sink = dispatch(ctx, router, first);
    REQUIRE(first_sink->has_header("set-cookie"));
    const std::string id = cookie_value(first_sink->header("set-cookie"));
    REQUIRE_FALSE(id.empty());
    CHECK(store->size() == 1);

    auto second = make_request(ctx, "/get");
    second->mutable_headers().add("cookie", "sid=" + id);
    CHECK(dispatch(ctx, router, second)->last_body == "42");

    auto rotate = make_request(ctx, "/rotate");
    rotate->mutable_headers().add("cookie", "sid=" + id);
    auto rotate_sink = dispatch(ctx, router, rotate);
    const std::string rotated = last_cookie_value(*rotate_sink);
    REQUIRE_FALSE(rotated.empty());
    CHECK(rotated != id);

    auto stale = make_request(ctx, "/get");
    stale->mutable_headers().add("cookie", "sid=" + id);
    CHECK(dispatch(ctx, router, stale)->last_body == "none");

    auto fresh = make_request(ctx, "/get");
    fresh->mutable_headers().add("cookie", "sid=" + rotated);
    CHECK(dispatch(ctx, router, fresh)->last_body == "99");
}

TEST_CASE("framework: CSRF is a double-submit check for unsafe methods", "[framework][csrf]") {
    asio::io_context ctx;
    Router router;
    router.use(middleware::csrf(CsrfConfig{.cookie_name = "csrf", .secure = false}));
    bool submitted = false;
    router.route({Method::Get}, "/token", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(status::ok).async_send(std::string{csrf_token(*req)}, asio::use_awaitable);
    });
    router.route({Method::Post}, "/submit", [&submitted](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        submitted = true;
        co_await res->status(status::no_content).async_send("", asio::use_awaitable);
    });

    auto token_sink = dispatch(ctx, router, make_request(ctx, "/token"));
    REQUIRE(token_sink->has_header("set-cookie"));
    const std::string token = cookie_value(token_sink->header("set-cookie"));
    REQUIRE_FALSE(token.empty());
    CHECK(token_sink->last_body == token);

    auto good = make_request(ctx, "/submit", Method::Post);
    good->mutable_headers().add("cookie", "csrf=" + token);
    good->mutable_headers().add("x-csrf-token", token);
    CHECK(dispatch(ctx, router, good)->last_status == status::no_content);
    CHECK(submitted);

    submitted = false;
    auto bad = make_request(ctx, "/submit", Method::Post);
    bad->mutable_headers().add("cookie", "csrf=" + token);
    CHECK(dispatch(ctx, router, bad)->last_status == status::forbidden);
    CHECK_FALSE(submitted);
}

TEST_CASE("framework: CSRF exempts configured paths", "[framework][csrf]") {
    asio::io_context ctx;
    Router router;
    router.use(middleware::csrf(CsrfConfig{.exempt_paths = {"/webhook"}}));
    router.route({Method::Post}, "/webhook", text_handler("ok"));
    CHECK(dispatch(ctx, router, make_request(ctx, "/webhook", Method::Post))->last_body == "ok");
}

namespace {

std::string hs256_token(std::string_view header, std::string_view payload, std::string_view secret) {
    const std::string encoded_header = base64_url_encode(header);
    const std::string encoded_payload = base64_url_encode(payload);
    const std::string input = encoded_header + "." + encoded_payload;
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned int length = 0;
    HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char *>(input.data()), input.size(), digest, &length);
    return input + "." +
           base64_url_encode(
               std::string_view{reinterpret_cast<const char *>(digest), static_cast<std::size_t>(length)});
}

std::string jwt_payload(std::string_view subject, std::string_view scope, std::int64_t exp) {
    return std::string{"{\"sub\":\""} + std::string{subject} + "\",\"scope\":\"" + std::string{scope} +
           "\",\"roles\":[\"admin\"],\"iss\":\"iss\",\"aud\":\"aud\",\"exp\":" + std::to_string(exp) +
           ",\"nbf\":0,\"iat\":0}";
}

using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

struct TestKey {
    PkeyPtr key{nullptr, EVP_PKEY_free};
    std::string public_pem;
};

std::string public_pem(EVP_PKEY *key) {
    BIO *bio = BIO_new(BIO_s_mem());
    REQUIRE(bio != nullptr);
    REQUIRE(PEM_write_bio_PUBKEY(bio, key) == 1);
    BUF_MEM *memory = nullptr;
    BIO_get_mem_ptr(bio, &memory);
    std::string pem(memory->data, memory->length);
    BIO_free(bio);
    return pem;
}

TestKey make_rsa_key() {
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    REQUIRE(ctx != nullptr);
    REQUIRE(EVP_PKEY_keygen_init(ctx) == 1);
    REQUIRE(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) == 1);
    EVP_PKEY *raw = nullptr;
    REQUIRE(EVP_PKEY_keygen(ctx, &raw) == 1);
    EVP_PKEY_CTX_free(ctx);
    TestKey out{PkeyPtr{raw, EVP_PKEY_free}, {}};
    out.public_pem = public_pem(raw);
    return out;
}

TestKey make_ec_key() {
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    REQUIRE(ctx != nullptr);
    REQUIRE(EVP_PKEY_keygen_init(ctx) == 1);
    REQUIRE(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, NID_X9_62_prime256v1) == 1);
    EVP_PKEY *raw = nullptr;
    REQUIRE(EVP_PKEY_keygen(ctx, &raw) == 1);
    EVP_PKEY_CTX_free(ctx);
    TestKey out{PkeyPtr{raw, EVP_PKEY_free}, {}};
    out.public_pem = public_pem(raw);
    return out;
}

std::string sign_evp(EVP_PKEY *key, std::string_view input) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    REQUIRE(ctx != nullptr);
    REQUIRE(EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, key) == 1);
    REQUIRE(EVP_DigestSignUpdate(ctx, input.data(), input.size()) == 1);
    std::size_t length = 0;
    REQUIRE(EVP_DigestSignFinal(ctx, nullptr, &length) == 1);
    std::string signature(length, '\0');
    REQUIRE(EVP_DigestSignFinal(ctx, reinterpret_cast<unsigned char *>(signature.data()), &length) == 1);
    signature.resize(length);
    EVP_MD_CTX_free(ctx);
    return signature;
}

std::string ecdsa_der_to_raw(std::string_view der) {
    const unsigned char *cursor = reinterpret_cast<const unsigned char *>(der.data());
    ECDSA_SIG *sig = d2i_ECDSA_SIG(nullptr, &cursor, static_cast<long>(der.size()));
    REQUIRE(sig != nullptr);
    const BIGNUM *r = nullptr;
    const BIGNUM *s = nullptr;
    ECDSA_SIG_get0(sig, &r, &s);
    std::string raw(64, '\0');
    REQUIRE(BN_bn2binpad(r, reinterpret_cast<unsigned char *>(raw.data()), 32) == 32);
    REQUIRE(BN_bn2binpad(s, reinterpret_cast<unsigned char *>(raw.data()) + 32, 32) == 32);
    ECDSA_SIG_free(sig);
    return raw;
}

std::string jws(std::string_view header, std::string_view payload, std::string_view signature) {
    return base64_url_encode(header) + "." + base64_url_encode(payload) + "." + base64_url_encode(signature);
}

std::string b64_bn(const BIGNUM *value) {
    std::string bytes(BN_num_bytes(value), '\0');
    BN_bn2bin(value, reinterpret_cast<unsigned char *>(bytes.data()));
    return base64_url_encode(bytes);
}

std::string rsa_jwk(EVP_PKEY *key, std::string_view kid) {
    BIGNUM *n = nullptr;
    BIGNUM *e = nullptr;
    REQUIRE(EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_N, &n) == 1);
    REQUIRE(EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_E, &e) == 1);
    std::string json = "{\"kty\":\"RSA\",\"kid\":\"" + std::string{kid} + "\",\"alg\":\"RS256\",\"n\":\"" + b64_bn(n) +
                       "\",\"e\":\"" + b64_bn(e) + "\"}";
    BN_free(n);
    BN_free(e);
    return json;
}

std::string ec_jwk(EVP_PKEY *key, std::string_view kid) {
    unsigned char point[65]{};
    std::size_t length = 0;
    REQUIRE(EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY, point, sizeof(point), &length) == 1);
    REQUIRE((length == 65 && point[0] == 0x04));
    const std::string_view x{reinterpret_cast<const char *>(point + 1), 32};
    const std::string_view y{reinterpret_cast<const char *>(point + 33), 32};
    return "{\"kty\":\"EC\",\"kid\":\"" + std::string{kid} + "\",\"alg\":\"ES256\",\"crv\":\"P-256\",\"x\":\"" +
           base64_url_encode(x) + "\",\"y\":\"" + base64_url_encode(y) + "\"}";
}

} // namespace

TEST_CASE("framework: JWT verifies HS256, claims and scopes", "[framework][jwt]") {
    asio::io_context ctx;
    Router router;
    router.use(middleware::jwt(JwtConfig{
        .algorithms = {"HS256"},
        .secret = "secret",
        .issuer = "iss",
        .audience = "aud",
    }));
    router.route({Method::Get}, "/me", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        const Principal *identity = principal(*req);
        if (!identity) {
            co_await res->status(status::internal_server_error).async_send("no principal", asio::use_awaitable);
            co_return;
        }
        std::string scopes;
        for (const auto &scope : identity->scopes) {
            scopes += (scopes.empty() ? "" : ",") + scope;
        }
        co_await res->status(status::ok).async_send(identity->subject + ":" + scopes, asio::use_awaitable);
    });
    router.route({Method::Get}, "/write", {middleware::require_scope("write")},
                 [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                     co_await res->status(status::no_content).async_send("", asio::use_awaitable);
                 });

    const std::string header = R"({"alg":"HS256","typ":"JWT"})";
    const std::string payload = jwt_payload("alice", "read write", now_unix() + 3600);
    const std::string token = hs256_token(header, payload, "secret");

    auto authorized = make_request(ctx, "/me");
    authorized->mutable_headers().add("authorization", "Bearer " + token);
    CHECK(dispatch(ctx, router, authorized)->last_body == "alice:read,write");

    auto missing = make_request(ctx, "/me");
    auto missing_sink = dispatch(ctx, router, missing);
    CHECK(missing_sink->last_status == status::unauthorized);
    CHECK(missing_sink->header("www-authenticate").find("invalid_token") != std::string::npos);

    auto wrong_signature = make_request(ctx, "/me");
    wrong_signature->mutable_headers().add("authorization", "Bearer " + hs256_token(header, payload, "wrong"));
    CHECK(dispatch(ctx, router, wrong_signature)->last_status == status::unauthorized);

    auto expired = make_request(ctx, "/me");
    expired->mutable_headers().add(
        "authorization", "Bearer " + hs256_token(header, jwt_payload("alice", "read", now_unix() - 3600), "secret"));
    CHECK(dispatch(ctx, router, expired)->last_status == status::unauthorized);

    auto unscoped = make_request(ctx, "/write");
    unscoped->mutable_headers().add(
        "authorization", "Bearer " + hs256_token(header, jwt_payload("alice", "read", now_unix() + 3600), "secret"));
    CHECK(dispatch(ctx, router, unscoped)->last_status == status::forbidden);

    auto scoped = make_request(ctx, "/write");
    scoped->mutable_headers().add("authorization", "Bearer " + token);
    CHECK(dispatch(ctx, router, scoped)->last_status == status::no_content);
}

TEST_CASE("framework: JWT rejects malformed, unsigned and mismatched tokens", "[framework][jwt]") {
    asio::io_context ctx;
    const std::string header = R"({"alg":"HS256","typ":"JWT"})";
    const auto now = now_unix();

    auto claims = [](std::string_view iss, std::string_view aud, std::int64_t exp, std::int64_t nbf,
                     std::string_view roles) {
        return "{\"sub\":\"alice\",\"scope\":\"read\",\"roles\":[" + std::string{roles} + "],\"iss\":\"" +
               std::string{iss} + "\",\"aud\":\"" + std::string{aud} + "\",\"exp\":" + std::to_string(exp) +
               ",\"nbf\":" + std::to_string(nbf) + "}";
    };
    auto bearer = [&](const std::string &token, std::string path = "/me") {
        auto req = make_request(ctx, std::move(path));
        req->mutable_headers().add("authorization", "Bearer " + token);
        return req;
    };

    Router router;
    router.use(middleware::jwt(JwtConfig{
        .algorithms = {"HS256"},
        .secret = "secret",
        .issuer = "iss",
        .audience = "aud",
    }));
    router.route({Method::Get}, "/me", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        const Principal *identity = principal(*req);
        co_await res->status(status::ok).async_send(identity ? identity->subject : std::string{"none"}, asio::use_awaitable);
    });
    router.route({Method::Get}, "/admin", {middleware::require_role("admin")}, text_handler("admin"));

    const std::string admin_roles = R"("admin")";
    const std::string user_roles = R"("user")";
    const std::string valid = hs256_token(header, claims("iss", "aud", now + 3600, 0, admin_roles), "secret");
    CHECK(dispatch(ctx, router, bearer(valid))->last_body == "alice");

    // exp / nbf / iss / aud claims.
    CHECK(dispatch(ctx, router, bearer(hs256_token(header, claims("iss", "aud", now - 3600, 0, admin_roles), "secret")))
              ->last_status == status::unauthorized);
    CHECK(dispatch(ctx, router,
                   bearer(hs256_token(header, claims("iss", "aud", now + 3600, now + 3600, admin_roles), "secret")))
              ->last_status == status::unauthorized);
    CHECK(
        dispatch(ctx, router, bearer(hs256_token(header, claims("other", "aud", now + 3600, 0, admin_roles), "secret")))
            ->last_status == status::unauthorized);
    CHECK(
        dispatch(ctx, router, bearer(hs256_token(header, claims("iss", "other", now + 3600, 0, admin_roles), "secret")))
            ->last_status == status::unauthorized);
    // require_exp is on by default.
    CHECK(dispatch(ctx, router, bearer(hs256_token(header, R"({"sub":"alice","iss":"iss","aud":"aud"})", "secret")))
              ->last_status == status::unauthorized);

    // Signature and algorithm allowlist.
    CHECK(dispatch(ctx, router, bearer(hs256_token(header, claims("iss", "aud", now + 3600, 0, admin_roles), "wrong")))
              ->last_status == status::unauthorized);
    const std::string unsigned_token = base64_url_encode(R"({"alg":"none"})") + "." +
                                       base64_url_encode(claims("iss", "aud", now + 3600, 0, admin_roles)) + ".";
    CHECK(dispatch(ctx, router, bearer(unsigned_token))->last_status == status::unauthorized);

    TestKey rsa = make_rsa_key();
    Router rs_only;
    rs_only.use(middleware::jwt(JwtConfig{
        .algorithms = {"RS256"},
        .public_key_pem = rsa.public_pem,
        .issuer = "iss",
        .audience = "aud",
    }));
    rs_only.route({Method::Get}, "/me", text_handler("rs"));
    // Algorithm confusion: an HS256 token must not be accepted by an RS256-only
    // verifier even though its MAC would verify.
    CHECK(dispatch(ctx, rs_only, bearer(valid))->last_status == status::unauthorized);

    // Malformed compact JWS and a non-Bearer scheme.
    CHECK(dispatch(ctx, router, bearer("not-a-jwt"))->last_status == status::unauthorized);
    CHECK(dispatch(ctx, router, bearer("a.b"))->last_status == status::unauthorized);
    {
        auto req = make_request(ctx, "/me");
        req->mutable_headers().add("authorization", "Basic abc");
        CHECK(dispatch(ctx, router, req)->last_status == status::unauthorized);
    }

    // require_role: a valid but under-privileged token is 403, not 401.
    const std::string user_token = hs256_token(header, claims("iss", "aud", now + 3600, 0, user_roles), "secret");
    CHECK(dispatch(ctx, router, bearer(user_token, "/admin"))->last_status == status::forbidden);
    CHECK(dispatch(ctx, router, bearer(valid, "/admin"))->last_body == "admin");

    // optional=true: no header passes through anonymously; an invalid token is
    // still rejected.
    Router optional;
    optional.use(middleware::jwt(JwtConfig{
        .algorithms = {"HS256"},
        .secret = "secret",
        .issuer = "iss",
        .audience = "aud",
        .optional = true,
    }));
    optional.route({Method::Get}, "/me", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        const Principal *identity = principal(*req);
        co_await res->status(status::ok).async_send(identity ? identity->subject : std::string{"none"}, asio::use_awaitable);
    });
    CHECK(dispatch(ctx, optional, make_request(ctx, "/me"))->last_body == "none");
    CHECK(
        dispatch(ctx, optional, bearer(hs256_token(header, claims("iss", "aud", now + 3600, 0, admin_roles), "wrong")))
            ->last_status == status::unauthorized);
}

TEST_CASE("framework: JWT verifies RS256 and ES256 signatures", "[framework][jwt]") {
    asio::io_context ctx;
    const std::string payload = jwt_payload("alice", "read", now_unix() + 3600);

    TestKey rsa = make_rsa_key();
    const std::string rs_header = R"({"alg":"RS256","typ":"JWT"})";
    const std::string rs_input = base64_url_encode(rs_header) + "." + base64_url_encode(payload);
    const std::string rs_token = jws(rs_header, payload, sign_evp(rsa.key.get(), rs_input));

    Router rs_router;
    rs_router.use(middleware::jwt(JwtConfig{
        .algorithms = {"RS256"},
        .public_key_pem = rsa.public_pem,
        .issuer = "iss",
        .audience = "aud",
    }));
    rs_router.route({Method::Get}, "/me", text_handler("rs"));
    auto rs_request = make_request(ctx, "/me");
    rs_request->mutable_headers().add("authorization", "Bearer " + rs_token);
    CHECK(dispatch(ctx, rs_router, rs_request)->last_body == "rs");

    TestKey ec = make_ec_key();
    const std::string es_header = R"({"alg":"ES256","typ":"JWT"})";
    const std::string es_input = base64_url_encode(es_header) + "." + base64_url_encode(payload);
    const std::string es_token = jws(es_header, payload, ecdsa_der_to_raw(sign_evp(ec.key.get(), es_input)));

    Router es_router;
    es_router.use(middleware::jwt(JwtConfig{
        .algorithms = {"ES256"},
        .public_key_pem = ec.public_pem,
        .issuer = "iss",
        .audience = "aud",
    }));
    es_router.route({Method::Get}, "/me", text_handler("es"));
    auto es_request = make_request(ctx, "/me");
    es_request->mutable_headers().add("authorization", "Bearer " + es_token);
    CHECK(dispatch(ctx, es_router, es_request)->last_body == "es");
}

TEST_CASE("framework: JWKS selects RSA/EC keys by kid", "[framework][jwt][jwks]") {
    asio::io_context ctx;
    TestKey rsa = make_rsa_key();
    TestKey ec = make_ec_key();
    const std::string payload = jwt_payload("alice", "read", now_unix() + 3600);
    const std::string jwks = R"({"keys":[)" + rsa_jwk(rsa.key.get(), "rsa1") + "," + ec_jwk(ec.key.get(), "ec1") + "]}";

    auto sign = [](EVP_PKEY *key, std::string_view alg, std::string_view input) {
        std::string signature = sign_evp(key, input);
        return alg == "ES256" ? ecdsa_der_to_raw(signature) : signature;
    };
    auto make_token = [&](EVP_PKEY *key, std::string alg, std::string kid) {
        std::string header = R"({"alg":")" + alg + R"(","typ":"JWT","kid":")" + kid + R"("})";
        const std::string input = base64_url_encode(header) + "." + base64_url_encode(payload);
        return jws(header, payload, sign(key, alg, input));
    };
    auto make_no_kid_token = [&](EVP_PKEY *key, std::string alg) {
        std::string header = R"({"alg":")" + alg + R"(","typ":"JWT"})";
        const std::string input = base64_url_encode(header) + "." + base64_url_encode(payload);
        return jws(header, payload, sign(key, alg, input));
    };

    Router router;
    router.use(middleware::jwt(JwtConfig{
        .algorithms = {"RS256", "ES256"},
        .jwks_json = jwks,
        .issuer = "iss",
        .audience = "aud",
    }));
    router.route({Method::Get}, "/me", text_handler("ok"));

    auto with_token = [&](const std::string &token) {
        auto req = make_request(ctx, "/me");
        req->mutable_headers().add("authorization", "Bearer " + token);
        return dispatch(ctx, router, req);
    };

    CHECK(with_token(make_token(rsa.key.get(), "RS256", "rsa1"))->last_body == "ok");
    CHECK(with_token(make_token(ec.key.get(), "ES256", "ec1"))->last_body == "ok");
    CHECK(with_token(make_token(rsa.key.get(), "RS256", "nope"))->last_status == status::unauthorized);
    // No kid is fine when the algorithm leaves exactly one candidate...
    CHECK(with_token(make_no_kid_token(rsa.key.get(), "RS256"))->last_body == "ok");
    {
        const std::string header = R"({"alg":"ES256","typ":"JWT","kid":"rsa1"})";
        const std::string input = base64_url_encode(header) + "." + base64_url_encode(payload);
        CHECK(with_token(jws(header, payload, sign_evp(rsa.key.get(), input)))->last_status == status::unauthorized);
    }

    // ...but is rejected when several keys share the algorithm.
    Router ambiguous;
    ambiguous.use(middleware::jwt(JwtConfig{
        .algorithms = {"RS256"},
        .jwks_json = R"({"keys":[)" + rsa_jwk(rsa.key.get(), "rsa1") + "," + rsa_jwk(rsa.key.get(), "rsa2") + "]}",
        .issuer = "iss",
        .audience = "aud",
    }));
    ambiguous.route({Method::Get}, "/me", text_handler("ambiguous"));
    auto ambiguous_request = make_request(ctx, "/me");
    ambiguous_request->mutable_headers().add("authorization", "Bearer " + make_no_kid_token(rsa.key.get(), "RS256"));
    CHECK(dispatch(ctx, ambiguous, ambiguous_request)->last_status == status::unauthorized);

    // A single key is usable without a kid.
    Router single;
    single.use(middleware::jwt(JwtConfig{
        .algorithms = {"RS256"},
        .jwks_json = R"({"keys":[)" + rsa_jwk(rsa.key.get(), "rsa1") + "]}",
        .issuer = "iss",
        .audience = "aud",
    }));
    single.route({Method::Get}, "/me", text_handler("single"));
    auto single_request = make_request(ctx, "/me");
    single_request->mutable_headers().add("authorization", "Bearer " + make_no_kid_token(rsa.key.get(), "RS256"));
    CHECK(dispatch(ctx, single, single_request)->last_body == "single");
}
