// Compile check for the code samples in README.md.
//
// The README had drifted to APIs that no longer exist (`HttpServer`,
// `setHttpHandler`, `Config{...}`) and nobody noticed, because documented code
// is not compiled by anything. The main samples in the README are reproduced
// here so that drift becomes a build failure instead.
//
// This is a syntax/shape check only: the functions are never called. Running
// them would bind ports and block.

#include <simple_http.h>

#include <chrono>
#include <expected>
#include <memory>
#include <optional>
#include <print>
#include <string_view>
#include <thread>
#include <vector>

namespace asio = boost::asio;
using simple_http::RequestPtr;
using simple_http::ResponsePtr;

// --- Server (README → Quick Start) -------------------------------------------

simple_http::ServerConfig readme_server_config() {
    simple_http::ServerConfig cfg;
    cfg.listen = simple_http::InetAddress{"0.0.0.0", 7788, false}; // InetAddress{host, port, v6_only}
    cfg.worker_threads = 4;
    return cfg;
}

void readme_server() {
    simple_http::ServerConfig cfg = readme_server_config();
    simple_http::Server server{cfg};

    server.route({simple_http::Method::Get}, "/hello", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).content_type(simple_http::mime::text_plain).send("Hello World!");
    });

    server.fallback(
        [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(404).send("not found"); });

    if (!server.start()) {
        return; // a listener failed to bind
    }
    for (;;) {
        std::this_thread::sleep_for(std::chrono::hours(1)); // the pool's threads do the work
    }
}

// A response is fluent, one-shot or streamed.
asio::awaitable<void> readme_response_shapes(RequestPtr req, ResponsePtr res) {
    std::string payload = "{}";
    co_await res->status(200).content_type(simple_http::mime::app_json).send(payload);

    co_await res->status(200).begin();
    co_await res->write("first ");
    co_await res->finish("last");
    co_return;
}

// A UNIX-domain socket replaces the port with a path.
simple_http::ServerConfig readme_unix_config() {
    simple_http::ServerConfig cfg;
    cfg.listen = simple_http::UnixAddress{"/run/myapp.sock"};
    return cfg;
}

// --- Client (README → Quick Start) -------------------------------------------

asio::awaitable<void> readme_client() {
    // Pinned to the caller's executor: pass an executor (or io_context) whose
    // single thread drives every operation of this client (model A).
    simple_http::Client http{co_await asio::this_coro::executor}; // default policy

    auto r = co_await http.get("https://example.com/").send();
    if (!r) {
        co_return;
    }
    (void)r->status();
    (void)r->version();
    (void)r->bodyless();
    auto body = co_await r->read_all();
    (void)body;
}

asio::awaitable<void> readme_client_streaming() {
    simple_http::Client http{co_await asio::this_coro::executor};
    auto up = co_await http.open_stream("https://example.com/upload", {.method = simple_http::Method::Post});
    if (!up)
        co_return;

    co_await up->write("hello "); // body chunks…
    co_await up->finish("world"); // …and the end of the body

    auto head = co_await up->read_head(); // status + headers
    (void)head->status;
    while (auto chunk = co_await up->read()) { // then the body
        if (chunk->eof)
            break;
        (void)chunk->data;
    }
}

// --- TLS & mTLS (README → TLS & mTLS) ----------------------------------------

simple_http::ServerConfig readme_tls_config() {
    simple_http::ServerConfig cfg;
    cfg.listen = simple_http::InetAddress{"0.0.0.0", 8443, false};
    cfg.tls = simple_http::TlsConfig{
        .cert_chain_file = "server_cert.pem",
        .private_key_file = "server_key.pem",
        .mutual = true, // require a client certificate
        .ca_file = "ca_cert.pem",
    };
    return cfg;
}

void readme_ssl_handle_route(simple_http::Server &server) {
    server.route({simple_http::Method::Get}, "/whoami",
                 [](RequestPtr, ResponsePtr res, simple_http::SslHandle ssl) -> asio::awaitable<void> {
                     if (!ssl) {
                         co_await res->status(400).send("plaintext");
                         co_return;
                     }
                     X509 *cert = SSL_get1_peer_certificate(*ssl); // owned by the caller
                     if (cert)
                         X509_free(cert);
                     co_await res->status(200).send("ok");
                 });
}

// --- Middleware (README → Routing, Middleware & Proxy) -------------------------

void readme_middleware(simple_http::Server &server) {
    // Global middleware: outermost first, each may wrap the chain (before/after)
    // or short-circuit it by never calling next().
    server.use(simple_http::middleware::request_id());
    server.use(simple_http::middleware::access_log());
    server.use(simple_http::middleware::recovery());

    // A group: a path prefix plus group middleware, applied only to the routes
    // registered inside (chi/gin Group, axum nest).
    server.group("/api", {simple_http::middleware::basic_auth("svc", "s3cret")}, [](simple_http::Router &api) {
        api.route({simple_http::Method::Get}, "/users", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
            co_await res->status(200).send("users");
        });
        // Per-route middleware: wraps this route's handler only.
        api.route(
            {simple_http::Method::Post}, "/users",
            {[](RequestPtr req, ResponsePtr res, simple_http::SslHandle ssl,
                simple_http::Next next) -> asio::awaitable<void> {
                if (!req->header("x-csrf-token")) {
                    co_await res->status(403).send("csrf");
                    co_return;
                }
                co_await next(std::move(req), std::move(res), ssl);
            }},
            [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(201).send("created"); });
    });
}

// --- OpenAPI & Swagger UI (README → OpenAPI & Swagger UI) -----------------------
namespace openapi_readme {

struct Pet {
    std::int64_t id{};
    std::string name;
    std::string status{"available"};
    struct glaze_json_schema {
        glz::schema status{.description = "lifecycle state",
                           .enumeration = std::vector<std::string_view>{"available", "pending", "sold"}};
    };
};
struct PetParams {
    std::int64_t id{};
};
struct ErrorBody {
    std::string error;
};

} // namespace openapi_readme

void readme_openapi() {
    simple_http::ServerConfig cfg = {.listen = simple_http::InetAddress{"127.0.0.1", 7795, false}, .worker_threads = 4};
    simple_http::Server server{cfg};
    using namespace openapi_readme;
    namespace openapi = simple_http::openapi;

    server.openapi().title("petshop").version("1.0.0").server("http://127.0.0.1:7795");
    server.route(
        {simple_http::Method::Get}, "/pets/{id}",
        [](simple_http::RequestPtr req, simple_http::ResponsePtr res) -> asio::awaitable<void> {
            const auto p = openapi::path_params<PetParams>(*req);
            const auto body = glz::write_json(Pet{.id = p->id, .name = "rex"});
            co_await res->status(200).content_type(simple_http::mime::app_json).send(body ? *body : std::string{"{}"});
        },
        openapi::doc()
            .path_params<PetParams>()
            .response<Pet>()
            .summary("get a pet")
            .operation_id("getPet")
            .error<ErrorBody>(404, "no such pet"));

    server.serve_openapi("/openapi.json");
    server.serve_swagger_ui("/swagger", "/openapi.json");
}

using simple_http::HttpProxyTarget;

void readme_routing(simple_http::Server &server) {
    server.route({simple_http::Method::Get}, "/world",
                 [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(200).send("world"); });
    server.route_regex(
        {simple_http::Method::Get}, "^/api/(.*)$",
        [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(200).send("api"); });
    server.fallback(
        [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(404).send("not found"); });

    // An auth middleware: short-circuits by never calling next().
    server.use([](RequestPtr req, ResponsePtr res, simple_http::SslHandle ssl,
                  simple_http::Next next) -> asio::awaitable<void> {
        if (!req->header("authorization")) {
            co_await res->status(401).send("unauthorized");
            co_return;
        }
        co_await next(std::move(req), std::move(res), ssl);
    });

    // CORS: an OPTIONS preflight is answered 204 here and never reaches a route.
    // See the CORS section for the policy.
    server.cors(simple_http::CorsConfig{.allow_origins = {"https://app.example"}});
}

void readme_proxy(simple_http::Server &server) {
    server.http_proxy("/api", "backend.internal", 8080);
    server.http_proxy("/v2", HttpProxyTarget{.host = "10.0.0.5", .port = 8443, .tls = true});
    server.http_proxy_regex("^/v1/(.*)$", "10.0.0.5", 9000, "/$1");
    server.ws_proxy("/ws", "backend.internal", 9000);
}

void readme_websocket(simple_http::Server &server) {
    server.ws_route("/chat", [](RequestPtr, std::shared_ptr<simple_http::WebSocket> ws) -> asio::awaitable<void> {
        for (;;) {
            auto msg = co_await ws->read(); // expected<WsMessage, error_code>
            if (!msg)
                break; // peer closed, or an error
            if (co_await ws->write("echo: " + msg->data, msg->text))
                break;
        }
        co_return; // returning sends a Close frame and shuts the socket down
    });
}

// --- CORS (README → CORS) ----------------------------------------------------

void readme_cors(simple_http::Server &server) {
    server.cors(simple_http::CorsConfig{
        .allow_origins = {"https://app.example.com"}, // exact origins; {} = any
        .allow_credentials = true,                    // mirrors the origin, never "*"
        .allow_methods = {"GET", "POST", "DELETE"},   // {} = whatever the browser asks
        .allow_headers = {"content-type", "authorization"},
        .expose_headers = {"x-request-id"},
        .max_age = std::chrono::seconds{600},
    });
}

void readme_cors_custom(simple_http::Server &server) {
    server.use([](RequestPtr req, ResponsePtr res, simple_http::SslHandle ssl,
                  simple_http::Next next) -> asio::awaitable<void> {
        const auto origin = req->header("origin");
        if (origin && (req->path().starts_with("/public") || *origin == "https://app.example")) {
            res->header(simple_http::field::access_control_allow_origin, std::string{*origin});
            res->header(simple_http::field::vary, "Origin");
        }
        co_await next(std::move(req), std::move(res), ssl);
    });
}

// --- Static files (README → Static Files) ------------------------------------

void readme_static_files(simple_http::Server &server) {
    simple_http::StaticFilesConfig cfg;
    cfg.table.root = "./web/dist";
    cfg.table.immutable_prefixes = {"/assets/"}; // content-hashed build output
    cfg.spa_fallback = "index.html";             // for a client-side router; "" = off

    auto site = std::make_shared<simple_http::StaticFiles>(std::move(cfg));
    std::string error;
    if (!site->load(error)) {
        std::println(stderr, "{}",
                     error); // a bad root is a startup failure, not a 404
        return;
    }
    server.static_files(std::move(site));
}

// --- Logging (README → Logging) ---------------------------------------------

void readme_logging() { simple_http::set_log_sink(simple_http::make_stdout_sink(simple_http::LogLevel::Info)); }

// --- Framework layer (README → State / Extractors / Validation / Auth) -------

namespace framework_readme {

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
struct RestParams {
    std::string rest;
};

} // namespace framework_readme

namespace simple_http {

template <> struct Validator<framework_readme::CreateUser> {
    static std::expected<void, std::vector<FieldError>> validate(const framework_readme::CreateUser &value) {
        std::vector<FieldError> errors;
        push_if(errors, required_field("name", !value.name.empty()));
        push_if(errors, range("age", value.age, 1, 150));
        return errors.empty() ? std::expected<void, std::vector<FieldError>>{} : std::unexpected(std::move(errors));
    }
};

} // namespace simple_http

void readme_framework(simple_http::Server &server) {
    using namespace framework_readme;

    server.state(std::make_shared<AppState>(AppState{"app"}));

    server.host("api.example.com", [](simple_http::Router &api) {
        api.route(
            {simple_http::Method::Get}, "/users/{id}",
            simple_http::extract<simple_http::Path<UserParams>, simple_http::State<AppState>>(
                [](const UserParams &params, AppState *state, RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                    co_await res->status(200).send(std::to_string(params.id) + ":" + state->name);
                }));
    });

    server.route({simple_http::Method::Post}, "/users",
                 simple_http::extract<simple_http::Valid<simple_http::Json<CreateUser>>>(
                     [](CreateUser user, RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                         co_await res->status(201).send(user.name);
                     }));

    server.route(
        {simple_http::Method::Get}, "/files/{*rest}",
        [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
            co_await res->status(200).send(std::string{req->param("rest").value_or("")});
        },
        simple_http::openapi::doc().path_params<RestParams>());

    auto sessions = std::make_shared<simple_http::MemorySessionStore>();
    server.use(simple_http::middleware::session(sessions, simple_http::SessionConfig{.secure = false}));
    server.use(simple_http::middleware::csrf(simple_http::CsrfConfig{.secure = false}));
    server.use(simple_http::middleware::jwt(simple_http::JwtConfig{
        .algorithms = {"HS256"},
        .secret = "secret",
    }));
    server.use(simple_http::middleware::require_scope("read"));
}

// --- Routing details (README → Quick Start / Router) -------------------------

void readme_routing_details(simple_http::Server &server) {
    server.route({simple_http::Method::Get}, "/users/{id}",
                 [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
                     auto id = req->param("id"); // std::optional<std::string_view>
                     if (!id) {
                         co_await res->status(400).send("bad id");
                         co_return;
                     }
                     co_await res->status(200).send("user " + std::string{*id});
                 });

    server.group("/api", {simple_http::middleware::basic_auth("svc", "s3cret")}, [](simple_http::Router &api) {
        api.route(
            {simple_http::Method::Get, simple_http::Method::Post}, "/items",
            [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(200).send("items"); });
    });
}

// --- Response details (README → ResponseWriter) ------------------------------

asio::awaitable<void> readme_response_details(ResponsePtr res) {
    res->header("x-trace", "abc");
    res->replace_header("x-frame-options", "SAMEORIGIN");
    res->set_cookie("session", "abc", std::chrono::seconds{3600}, "/", "", true, true, "Lax");

    co_await res->status(200).begin();
    co_await res->write("body");
    res->trailer("x-checksum", "abc123");
    co_await res->finish_trailers();

    co_await res->status(200).begin();
    co_await res->write("first ");
    co_await res->finish("last");

    co_await res->redirect("https://example.com/new", simple_http::status::temporary_redirect).send("");

    co_await res->sse_begin();
    co_await res->sse_event(R"({"ok":true})", "message", "42");
    co_await res->sse_comment("keep-alive");
}

// --- Files, uploads and errors (README → Static files / Error bodies) --------

asio::awaitable<void> readme_send_file(ResponsePtr res) {
    (void)co_await simple_http::send_file(*res, "./data/report.pdf");
}

asio::awaitable<void> readme_upload(RequestPtr req, ResponsePtr res) {
    auto n = co_await simple_http::read_body_to_file(req->body(), "./upload.bin", 64ull * 1024 * 1024);
    if (!n) {
        co_await simple_http::write_error(res, simple_http::status::bad_request,
                                          {.code = "upload_failed", .message = n.error().message()});
        co_return;
    }
    co_await res->status(201).send(std::to_string(*n));
}

asio::awaitable<void> readme_error_bodies(RequestPtr req, ResponsePtr res) {
    co_await simple_http::write_error(res, simple_http::status::not_found,
                                      {.code = "not_found", .message = "no such pet", .details = "id=42"});
    co_await simple_http::write_problem(res, simple_http::status::bad_request,
                                        simple_http::ProblemDetails{.title = "Invalid request",
                                                                    .detail = "age must be positive",
                                                                    .instance = std::string{req->path()}});
}

// --- Forms, JSON, typed params and multipart (README → Data helpers) ---------

namespace readme_data {

struct CreateUser {
    std::string name;
    int age{};
};
struct User {
    std::string name;
};
struct UserParams {
    std::int64_t id{};
};
struct ListQuery {
    std::optional<std::string> q;
    int page{1};
};

} // namespace readme_data

asio::awaitable<void> readme_data_helpers(RequestPtr req, ResponsePtr res) {
    using namespace readme_data;

    auto form = co_await simple_http::read_urlencoded_body(*req);
    (void)form;
    auto parts = co_await simple_http::read_multipart_body(*req);
    (void)parts;

    auto boundary = simple_http::multipart_boundary(req->header("content-type").value_or(""));
    if (boundary) {
        simple_http::MultipartStreamCallbacks cb;
        cb.on_part_begin = [](const simple_http::MultipartPart &) { return true; };
        cb.on_part_data = [](std::string_view) { return true; };
        cb.on_part_end = [] {};
        (void)co_await simple_http::read_multipart_stream(req->body(), *boundary, cb);
    }

    auto in = co_await simple_http::read_json_body<CreateUser>(*req);
    if (!in) {
        co_await simple_http::write_error(res, simple_http::status::bad_request,
                                          {.code = "invalid_body", .message = "bad JSON"});
        co_return;
    }
    co_await simple_http::write_json(res, User{.name = in->name}, simple_http::status::created);

    auto p = simple_http::path_params<UserParams>(*req);
    auto q = simple_http::query_params<ListQuery>(*req);
    (void)p;
    (void)q;
}

// --- Middleware, CORS and compression (README → Middleware) ------------------

void readme_all_middleware(simple_http::Server &server) {
    using namespace simple_http;
    server.use(middleware::request_id());
    server.use(middleware::access_log());
    server.use(middleware::recovery());
    server.use(middleware::real_ip({"10.0.0.0/8"}));
    server.use(middleware::clean_path());
    server.use(middleware::strip_prefix("/api"));
    server.use(middleware::redirect_slashes());
    server.use(middleware::ws_origin({"https://app.example"}));
    server.use(middleware::timeout(std::chrono::seconds{5}));
    server.use(middleware::compress(
        CompressionConfig{.min_bytes = 1024, .gzip_level = 6, .brotli_quality = 5, .compress_streamed = true}));
    server.use(middleware::secure_headers(middleware::SecureHeadersConfig{
        .content_security_policy = "default-src 'self'", .hsts_max_age = std::chrono::seconds{31536000}}));
    server.use(middleware::rate_limit(middleware::make_token_bucket(100, 200)));

    auto limiter = std::make_shared<middleware::RateLimiter>(10, 20, 10000);
    server.use(middleware::rate_limit(limiter, [](const RequestPtr &r) { return std::string{r->peer_address()}; }));

    server.cors(CorsConfig{
        .allow_origins = {"https://app.example.com", "https://*.example.com"},
        .allow_credentials = true,
        .allow_methods = {"GET", "POST", "DELETE"},
        .allow_headers = {"content-type", "authorization"},
        .expose_headers = {"x-request-id"},
        .max_age = std::chrono::seconds{600},
    });
}

// --- Server configuration (README → ServerConfig / TLS) ----------------------

void readme_server_extras(simple_http::Server &server) {
    simple_http::EngineLimits limits;
    limits.idle_timeout = std::chrono::seconds{120};
    limits.max_header_bytes = 64 * 1024;
    limits.max_body_bytes = 64ull * 1024 * 1024;
    limits.ws_read_ahead_bytes = 4 * 1024 * 1024;
    limits.ws_compression = false;
    limits.h2_max_concurrent_streams = 200;
    limits.h2_max_frame_size = 16384;
    limits.h2_initial_window = 65535;
    limits.h2_enable_connect_protocol = true;
    limits.h2_max_header_list_size = 0;
    limits.h2_max_queued_streams = 0;
    limits.h3_max_concurrent_streams = 0;
    (void)limits;

    server.listen(simple_http::InetAddress{"0.0.0.0", 80})
        .name("http")
        .plaintext(simple_http::PlaintextHttp::Http1WithH2cUpgrade);
    server.listen(simple_http::InetAddress{"0.0.0.0", 443})
        .tls(simple_http::TlsConfig{.cert_chain_file = "cert.pem", .private_key_file = "key.pem"})
        .alpn(simple_http::TlsHttp::Http1AndHttp2);
}

void readme_proxy_details(simple_http::Server &server) {
    server.http_proxy("/api", "backend.internal", 8080);
    server.http_proxy("/v2", simple_http::HttpProxyTarget{.host = "10.0.0.5",
                                                          .port = 8443,
                                                          .tls = true,
                                                          .h2c = false,
                                                          .h2c_prior_knowledge = false,
                                                          .preserve_host = false,
                                                          .max_attempts = 3});
    server.http_proxy_regex("^/v1/(.*)$", "10.0.0.5", 9000, "/$1");
    server.ws_proxy("/ws", "backend.internal", 9000);
}

// --- Client configuration and helpers (README → Client) ----------------------

namespace readme_client_types {
struct Item {
    std::string name;
};
} // namespace readme_client_types

asio::awaitable<void> readme_client_config() {
    using namespace readme_client_types;

    simple_http::ClientConfig cfg;
    cfg.connect_timeout = std::chrono::seconds{10};
    cfg.request_timeout = std::chrono::seconds{60};
    cfg.response_head_timeout = std::chrono::seconds{30};
    cfg.body_idle_timeout = std::chrono::seconds{0};
    cfg.max_redirects = 5;
    cfg.cookie_jar = std::make_shared<simple_http::CookieJar>();
    cfg.auto_decompress = true;
    cfg.default_version = simple_http::HttpVersionPolicy::Auto;
    cfg.default_h2c = simple_http::H2cMode::Upgrade;
    cfg.retry.max_retries = 2;
    cfg.retry.initial_backoff = std::chrono::milliseconds{100};
    cfg.proxy = simple_http::ProxyConfig{.kind = simple_http::ProxyKind::Socks5, .host = "127.0.0.1", .port = 1080};
    cfg.tls = simple_http::TlsClientConfig{.verify_peer = true,
                                           .verify_host = true,
                                           .ca_file = "ca.pem",
                                           .cert_chain_file = "client_cert.pem",
                                           .private_key_file = "client_key.pem",
                                           .min_version = simple_http::TlsMinVersion::Tls12};

    simple_http::Client http{co_await asio::this_coro::executor, cfg};
    auto r = co_await http.post("https://api.example.com/items")
                 .query({{"dry_run", "1"}})
                 .json(Item{.name = "book"})
                 .bearer_auth("token")
                 .timeout(std::chrono::seconds{10})
                 .send();
    if (r) {
        if (auto ok = r->error_for_status(); ok) {
            auto item = co_await r->json<Item>();
            (void)item;
        }
    }

    auto up = co_await http.open_stream("https://example.com/upload", {.method = simple_http::Method::Post,
                                                                       .response_head_timeout = std::chrono::seconds{5},
                                                                       .body_idle_timeout = std::chrono::seconds{30}});
    if (up) {
        co_await up->write("hello ");
        co_await up->finish("world");
        (void)co_await up->read_head();
        while (auto chunk = co_await up->read()) {
            if (chunk->eof)
                break;
            (void)chunk->data;
        }
        (void)up->trailers();
    }

    auto ws = co_await http.open_websocket("wss://host/chat", {.origin = "https://app.example"});
    if (ws) {
        for (;;) {
            auto msg = co_await (*ws)->read();
            if (!msg)
                break;
            if (co_await (*ws)->write(msg->data, msg->text))
                break;
        }
    }
}

asio::awaitable<void> readme_sse(simple_http::Client &http) {
    auto sse = co_await simple_http::SseStream::open(http, "https://host/events");
    if (!sse)
        co_return;
    while (auto ev = co_await sse->next()) {
        (void)ev->event;
        (void)ev->data;
        (void)ev->id;
    }
}

int main() {
    std::println("README samples compile");
    return 0;
}
