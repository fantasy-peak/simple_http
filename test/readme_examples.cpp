// Compile check for the code samples in README.md.
//
// The README had drifted to APIs that no longer exist (`HttpServer`,
// `setHttpHandler`, `Config{...}`) and nobody noticed, because documented code
// is not compiled by anything. Every sample in the README is reproduced here so
// that drift becomes a build failure instead.
//
// This is a syntax/shape check only: the functions are never called. Running
// them would bind ports and block.

#include <simple_http.h>

#include <chrono>
#include <memory>
#include <print>
#include <thread>

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
    simple_http::HttpClient http; // default policy

    auto r = co_await http.get("https://example.com/");
    if (!r) {
        co_return;
    }
    (void)r->status;
    (void)simple_http::to_string(r->version);
    (void)r->body.size();
}

asio::awaitable<void> readme_client_streaming() {
    simple_http::HttpClient http;
    simple_http::ClientTarget target{.host = "127.0.0.1", .port = 7789, .use_tls = true};
    auto session = co_await http.connect(target);
    if (!session)
        co_return;

    simple_http::RequestSpec spec{.method = simple_http::Method::Post, .target = "/upload", .stream_body = true};
    auto stream = co_await (*session)->open_stream(spec);
    if (!stream)
        co_return;

    co_await (*stream)->write("hello "); // body chunks…
    co_await (*stream)->finish("world"); // …and the end of the body

    auto head = co_await (*stream)->read_head(); // status + headers
    (void)head->status;
    while (auto chunk = co_await (*stream)->read()) { // then the body
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

// --- OpenAPI & Swagger UI (README → OpenAPI & Swagger UI) -----------------------
#if defined(SIMPLE_HTTP_ENABLE_OPENAPI)
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

}  // namespace openapi_readme

void readme_openapi() {
    simple_http::ServerConfig cfg = {.listen = simple_http::InetAddress{"127.0.0.1", 7795, false},
                                     .worker_threads = 4};
    simple_http::Server server{cfg};
    using namespace openapi_readme;
    namespace openapi = simple_http::openapi;

    server.openapi().title("petshop").version("1.0.0").server("http://127.0.0.1:7795");
    server.route<PetParams, openapi::NoBody, Pet>(
        {simple_http::Method::Get}, "/pets/{id}",
        [](simple_http::RequestPtr req, simple_http::ResponsePtr res) -> asio::awaitable<void> {
            const auto p = openapi::path_params<PetParams>(*req);
            const auto body = glz::write_json(Pet{.id = p->id, .name = "rex"});
            co_await res->status(200).content_type(simple_http::mime::app_json)
                .send(body ? *body : std::string{"{}"});
        },
        openapi::OperationInfo{.summary = "get a pet", .operation_id = "getPet"},
        openapi::resp<ErrorBody>(404, "no such pet"));

    server.serve_openapi("/openapi.json");
    server.serve_swagger_ui("/swagger", "/openapi.json");
}
#endif  // SIMPLE_HTTP_ENABLE_OPENAPI

using simple_http::HttpProxyTarget;

void readme_routing(simple_http::Server &server) {
    server.route({simple_http::Method::Get}, "/world",
                 [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(200).send("world"); });
    server.route_regex(
        {simple_http::Method::Get}, "^/api/(.*)$",
        [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(200).send("api"); });
    server.fallback(
        [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(404).send("not found"); });

    server.before([](RequestPtr req, ResponsePtr res) -> asio::awaitable<bool> {
        if (req->path().empty()) {
            co_await res->status(401).send("unauthorized");
            co_return false;
        }
        co_return true;
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
    server.before([](RequestPtr req, ResponsePtr res) -> asio::awaitable<bool> {
        const auto origin = req->header("origin");
        if (!origin) {
            co_return true; // not a CORS request
        }
        if (req->path().starts_with("/public") || *origin == "https://app.example") {
            res->header(simple_http::field::access_control_allow_origin, std::string{*origin});
            res->header(simple_http::field::vary, "Origin");
        }
        co_return true;
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

int main() {
    std::println("README samples compile");
    return 0;
}
