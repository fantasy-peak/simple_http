// Compile check for the code samples in README.md.
//
// The README had drifted to APIs that no longer exist (`HttpServer`,
// `setHttpHandler`, `Config{...}`) and nobody noticed, because documented code is
// not compiled by anything. Every sample in the README is reproduced here so that
// drift becomes a build failure instead.
//
// This is a syntax/shape check only: the functions are never called. Running them
// would bind ports and block.

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
    cfg.listen = simple_http::InetAddress{"0.0.0.0", 7788, false};  // InetAddress{host, port, v6_only}
    cfg.worker_threads = 4;
    return cfg;
}

void readme_server() {
    simple_http::ServerConfig cfg = readme_server_config();
    simple_http::Server server{cfg};

    server.route("/hello", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).content_type(simple_http::mime::text_plain).send("Hello World!");
    });

    server.fallback([](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(404).send("not found");
    });

    if (!server.start()) {
        return;  // a listener failed to bind
    }
    for (;;) {
        std::this_thread::sleep_for(std::chrono::hours(1));  // the pool's threads do the work
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
    simple_http::HttpClient http;  // default policy

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
    if (!session) co_return;

    simple_http::RequestSpec spec{.method = simple_http::Method::Post, .target = "/upload", .stream_body = true};
    auto stream = co_await (*session)->open_stream(spec);
    if (!stream) co_return;

    co_await (*stream)->write("hello ");  // body chunks…
    co_await (*stream)->finish("world");  // …and the end of the body

    auto head = co_await (*stream)->read_head();  // status + headers
    (void)head->status;
    while (auto chunk = co_await (*stream)->read()) {  // then the body
        if (chunk->eof) break;
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
        .mutual = true,  // require a client certificate
        .ca_file = "ca_cert.pem",
    };
    return cfg;
}

void readme_ssl_handle_route(simple_http::Server& server) {
    server.route("/whoami", [](RequestPtr, ResponsePtr res, simple_http::SslHandle ssl) -> asio::awaitable<void> {
        if (!ssl) {
            co_await res->status(400).send("plaintext");
            co_return;
        }
        X509* cert = SSL_get1_peer_certificate(*ssl);  // owned by the caller
        if (cert) X509_free(cert);
        co_await res->status(200).send("ok");
    });
}

// --- Routing, middleware & proxy (README → Routing, Middleware & Proxy) ------

using simple_http::HttpProxyTarget;

void readme_routing(simple_http::Server& server) {
    server.route("/world", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).send("world");
    });
    server.route_regex("^/api/(.*)$", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).send("api");
    });
    server.fallback([](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(404).send("not found");
    });

    server.before([](RequestPtr req, ResponsePtr res) -> asio::awaitable<bool> {
        if (req->path().empty()) {
            co_await res->status(401).send("unauthorized");
            co_return false;
        }
        co_return true;
    });

    server.cors([](RequestPtr, ResponsePtr) -> asio::awaitable<bool> { co_return true; });
}

void readme_proxy(simple_http::Server& server) {
    server.http_proxy("/api", "backend.internal", 8080);
    server.http_proxy("/v2", HttpProxyTarget{.host = "10.0.0.5", .port = 8443, .tls = true});
    server.http_proxy_regex("^/v1/(.*)$", "10.0.0.5", 9000, "/$1");
    server.ws_proxy("/ws", "backend.internal", 9000);
}

void readme_websocket(simple_http::Server& server) {
    server.ws_route("/chat",
                    [](RequestPtr, std::shared_ptr<simple_http::WebSocket> ws) -> asio::awaitable<void> {
                        for (;;) {
                            auto msg = co_await ws->read();  // expected<WsMessage, error_code>
                            if (!msg) break;                 // peer closed, or an error
                            if (co_await ws->write("echo: " + msg->data, msg->text)) break;
                        }
                        co_return;  // returning sends a Close frame and shuts the socket down
                    });
}

// --- Static files (README → Static Files) ------------------------------------

void readme_static_files(simple_http::Server& server) {
    simple_http::StaticFilesConfig cfg;
    cfg.table.root = "./web/dist";
    cfg.table.immutable_prefixes = {"/assets/"};  // content-hashed build output
    cfg.spa_fallback = "index.html";              // for a client-side router; "" = off

    auto site = std::make_shared<simple_http::StaticFiles>(std::move(cfg));
    std::string error;
    if (!site->load(error)) {
        std::println(stderr, "{}", error);  // a bad root is a startup failure, not a 404
        return;
    }
    server.static_files(std::move(site));
}

// --- Logging (README → Logging) ---------------------------------------------

void readme_logging() {
    simple_http::set_log_sink(simple_http::make_stdout_sink(simple_http::LogLevel::Info));
}

int main() {
    std::println("README samples compile");
    return 0;
}
