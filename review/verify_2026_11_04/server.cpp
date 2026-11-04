// Verification fixture for REVIEW-2026-11-04.
//
// Not part of the library or its test suites. It exposes the handful of
// behaviors the review's verification programs assert on, on a caller-chosen
// port. Built from the working tree's headers, so the same binary shape can be
// built before and after a fix.
//
//   verify_server <h1-port> <h2-port>
//
// Env:
//   VERIFY_STATIC_ROOT   document root for static-file checks (optional)
//   VERIFY_CLEAN_PATH=1  mount middleware::clean_path() globally
//   VERIFY_WS_DEFLATE=1  enable ws_compression

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#include "simple_http.h"

namespace asio = boost::asio;
using namespace simple_http;

namespace {

std::string sv(std::string_view v) { return std::string{v}; }

template <typename ServerT> void register_common(ServerT &server) {
    server.route({Method::Get, Method::Head}, "/echo-host",
                 [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
                     std::string out = "host=" + sv(req->header("host").value_or("")) + ";path=" + sv(req->path()) +
                                       ";target=" + sv(req->target());
                     co_await res->status(status::ok).content_type("text/plain").send(std::move(out));
                 });

    server.route(any_methods, "/echo-body",
                 [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
                     auto body = co_await req->body().read_all();
                     if (!body) {
                         co_await res->status(status::bad_request).send("read error");
                         co_return;
                     }
                     std::string out = "len=" + std::to_string(body->size()) + ";body=" + *body;
                     co_await res->status(status::ok).content_type("text/plain").send(std::move(out));
                 });

    // Echoes a named request header, for the HTTP/2 HPACK table test.
    server.route({Method::Get}, "/echo-header",
                 [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
                     std::string name = sv(req->query_params().get("name").value_or("x-poison"));
                     std::string out = "value=" + sv(req->header(name).value_or(""));
                     co_await res->status(status::ok).content_type("text/plain").send(std::move(out));
                 });

    // 204 whose handler sets a Content-Length: RFC 9110 §15.3.5 forbids it on
    // the wire; Go strips it, and so must this server.
    server.route({Method::Get}, "/204-cl", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        res->header(field::content_length, "99");
        co_await res->status(status::no_content).send_bodyless();
    });

    // Surfaces the trailer fields the engine accepted.
    server.route(any_methods, "/trailers", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        (void)co_await req->body().read_all();
        std::string out;
        for (const auto &[name, value] : req->trailers()) {
            out += name;
            out += '=';
            out += value;
            out += ';';
        }
        co_await res->status(status::ok).content_type("text/plain").send(std::move(out));
    });

    // A handler that stays on the connection long enough for a same-connection
    // HTTP/2 stream to be refused while it runs.
    server.route({Method::Get}, "/slow", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        asio::steady_timer timer{co_await asio::this_coro::executor};
        timer.expires_after(std::chrono::milliseconds(600));
        co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        co_await res->status(status::ok).content_type("text/plain").send("slow");
    });

    // Two separate Vary fields from the handler; compression must merge both.
    server.route({Method::Get}, "/vary", {middleware::compress(CompressionConfig{.min_bytes = 0})},
                 [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                     res->header(field::vary, "Origin");
                     res->header(field::vary, "Cookie");
                     co_await res->status(status::ok)
                         .content_type("text/plain")
                         .send(std::string(4096, 'v')); // compressible, > min_bytes
                 });

    // CORS policy whose only decision maker is the predicate. The predicate
    // denies everything, so no Access-Control-Allow-Origin is allowed to appear.
    server.route({Method::Get}, "/cors",
                 {make_cors_middleware(CorsConfig{.allow_origin_fn = [](std::string_view) { return false; }})},
                 [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                     co_await res->status(status::ok).content_type("text/plain").send("cors");
                 });

    // HS256 verifier. A normal token with a `jti` claim must still be accepted.
    server.route({Method::Get}, "/jwt",
                 {middleware::jwt(JwtConfig{.algorithms = {"HS256"}, .secret = "verify-secret"})},
                 [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
                     const Principal *p = principal(*req);
                     co_await res->status(status::ok).content_type("text/plain").send(p ? p->subject : "none");
                 });

    // JWKS verifier with an oct key and a token carrying unknown-but-valid
    // claims. A standard JWKS also has `use`/`x5c`, which must not poison the
    // whole document.
    server.route({Method::Get}, "/jwks",
                 {middleware::jwt(JwtConfig{
                     .algorithms = {"HS256"},
                     .jwks_json = R"({"keys":[{"kty":"oct","kid":"k1","alg":"HS256","use":"sig","x5c":["AA"],"k":"dmVyaWZ5LXNlY3JldA"}]})"})},
                 [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
                     const Principal *p = principal(*req);
                     co_await res->status(status::ok).content_type("text/plain").send(p ? p->subject : "none");
                 });
}

void register_ws(Server &server) {
    server.ws_route("/ws", [](RequestPtr, std::shared_ptr<WebSocket> ws) -> asio::awaitable<void> {
        for (;;) {
            auto msg = co_await ws->read();
            if (!msg) {
                break;
            }
            if (auto ec = co_await ws->write(msg->data, msg->text)) {
                break;
            }
        }
    });
}

std::shared_ptr<StaticFiles> make_static(const char *root) {
    if (root == nullptr || *root == '\0') {
        return nullptr;
    }
    StaticFilesConfig cfg;
    cfg.table.root = root;
    cfg.table.not_found_file = ""; // no site 404 page: a miss stays a miss
    auto site = std::make_shared<StaticFiles>(std::move(cfg));
    std::string error;
    if (!site->load(error)) {
        std::cerr << "static load failed: " << error << "\n";
        return nullptr;
    }
    return site;
}

ServerConfig base_config(const char *port, PlaintextProtocols protocols) {
    ServerConfig cfg;
    cfg.listen = InetAddress{"127.0.0.1", static_cast<std::uint16_t>(std::atoi(port)), false};
    cfg.worker_threads = 2;
    cfg.reuse_port = false;
    cfg.plaintext_protocols = protocols;
    if (std::getenv("VERIFY_WS_DEFLATE") != nullptr) {
        cfg.limits.ws_compression = true;
    }
    return cfg;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "usage: verify_server <h1-port> <h2-port>\n";
        return 2;
    }
    set_log_sink(make_stderr_sink(LogLevel::Warn));

    const char *root = std::getenv("VERIFY_STATIC_ROOT");
    const bool clean = std::getenv("VERIFY_CLEAN_PATH") != nullptr;

    ServerConfig h1_cfg = base_config(argv[1], PlaintextProtocols::Http1);
    Server h1{h1_cfg};
    register_common(h1);
    register_ws(h1);
    if (clean) {
        h1.use(middleware::clean_path());
    }
    if (std::getenv("VERIFY_GROUP_STATIC") != nullptr) {
        // The documented shape: a static site inside a group protected by a
        // group middleware. The check asks whether the prefix and the middleware
        // are actually applied.
        auto deny = [](RequestPtr, ResponsePtr res, SslHandle, Next) -> asio::awaitable<void> {
            co_await res->status(status::forbidden).send("denied");
        };
        h1.group("/admin", {deny}, [root](Router &r) {
            if (auto site = make_static(root)) {
                r.static_files(std::move(site));
            }
        });
    } else if (auto site = make_static(root)) {
        h1.static_files(std::move(site));
    }

    ServerConfig h2_cfg = base_config(argv[2], PlaintextProtocols::Http2);
    h2_cfg.limits.h2_max_concurrent_streams = 1; // make the refusal path reachable
    Server h2{h2_cfg};
    register_common(h2);

    if (!h1.start()) {
        std::cerr << "h1 start failed\n";
        return 1;
    }
    if (!h2.start()) {
        std::cerr << "h2 start failed\n";
        return 1;
    }
    std::cout << "verify_server ready " << argv[1] << " " << argv[2] << std::endl;

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "stop") {
            break;
        }
    }
    h1.stop();
    h2.stop();
    return 0;
}
