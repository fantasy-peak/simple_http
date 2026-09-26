// Example server demonstrating the simple_http v2 API (coroutine-only handlers).
//
// It starts a plaintext server (HTTP/1.1, h2c upgrade, HTTP/2 prior-knowledge)
// and a TLS server (HTTP/1.1 and HTTP/2 via ALPN, mutual TLS), sharing the same
// routes. All handlers are coroutines; responses can be one-shot or streamed
// (chunked over HTTP/1.1, DATA frames over HTTP/2).

#include <charconv>
#include <format>
#include <print>
#include <string>
#include <thread>

#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "simple_http.h"

namespace asio = boost::asio;
using namespace simple_http;

// Returns the subject name of an X509 certificate, or "-" if none.
static std::string subject_name(X509* cert) {
    if (!cert) return "-";
    char buf[512] = {0};
    X509_NAME_oneline(X509_get_subject_name(cert), buf, sizeof(buf) - 1);
    return std::string{buf};
}

// Parses an unsigned integer query parameter "key=..." from a raw query string,
// returning `fallback` if absent or unparseable.
static std::size_t query_uint(std::string_view query, std::string_view key, std::size_t fallback) {
    std::string needle{key};
    needle += '=';
    for (std::size_t pos = 0; pos < query.size();) {
        auto amp = query.find('&', pos);
        auto pair = query.substr(pos, amp == std::string_view::npos ? std::string_view::npos : amp - pos);
        if (pair.starts_with(needle)) {
            auto val = pair.substr(needle.size());
            std::size_t out = 0;
            auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), out);
            if (ec == std::errc{}) return out;
            return fallback;
        }
        if (amp == std::string_view::npos) break;
        pos = amp + 1;
    }
    return fallback;
}

// Registers the shared route table on a server.
template <typename ServerT>
void register_routes(ServerT& server) {
    // One-shot response.
    server.route("/world", [](std::shared_ptr<Request> req, std::shared_ptr<Response> res) -> asio::awaitable<void> {
        std::string body = std::string{"hello from "} + std::string{to_string(res->version())};
        co_await res->status(200).send(body);
    });

    // Bidirectional streaming: read each request-body frame, log it, and echo a
    // response frame back per received frame. Chunked over HTTP/1.1, DATA frames
    // over HTTP/2 — every frame is flushed as it is produced.
    server.route("/hello", [](std::shared_ptr<Request> req, std::shared_ptr<Response> res) -> asio::awaitable<void> {
        co_await res->status(200).content_type("text/plain").begin();
        int i = 0;
        for (;;) {
            auto frame = co_await req->body().read();
            if (!frame) {
                std::println("[SERVER] body read error: {}", frame.error().message());
                break;
            }
            if (frame->eof) {
                std::println("[SERVER] <-- request body EOF");
                break;
            }
            std::println("[SERVER] <-- recv frame #{} ({} bytes): {}", i, frame->data.size(), frame->data);
            std::string out = std::format("echo #{}: {}", i, frame->data);
            co_await res->write(out);
            std::println("[SERVER] --> sent frame #{} ({} bytes)", i, out.size());
            ++i;
        }
        co_await res->finish(std::format("done, {} frames\n", i));
        std::println("[SERVER] --> sent final frame (total {} frames)", i);
    });

    // Reads the whole request body and echoes it back (one-shot).
    server.route("/echo", [](std::shared_ptr<Request> req, std::shared_ptr<Response> res) -> asio::awaitable<void> {
        auto body = co_await req->body().read_all();
        if (!body) {
            co_await res->status(400).send("read error");
            co_return;
        }
        co_await res->status(200).send(*body);
    });

    // Root: the same echo. Protocol-conformance suites target "/" and cannot be
    // pointed elsewhere, so this is not a duplicate of /echo but the path they
    // actually need — h1spec sends every case to it and checks the body comes
    // back verbatim under any method.
    server.route("/", [](std::shared_ptr<Request> req, std::shared_ptr<Response> res) -> asio::awaitable<void> {
        auto body = co_await req->body().read_all();
        if (!body) {
            co_await res->status(400).send("read error");
            co_return;
        }
        co_await res->status(200).send(*body);
    });

    // A simple one-shot handler.
    server.route("/sync", [](std::shared_ptr<Request> req, std::shared_ptr<Response> res) -> asio::awaitable<void> {
        co_await res->status(200).content_type("text/plain").send("simple one-shot reply");
    });

    // Echoes all received request headers. Handy to inspect what a reverse-proxy
    // forwards (X-Forwarded-For / X-Forwarded-Proto / X-Forwarded-Host / Host).
    server.route("/headers", [](std::shared_ptr<Request> req, std::shared_ptr<Response> res) -> asio::awaitable<void> {
        std::string out;
        for (const auto& [name, value] : req->headers()) {
            out += name + ": " + value + "\n";
        }
        co_await res->status(200).content_type("text/plain").send(out);
    });

    // TLS-aware handler: inspects the peer (client) certificate.
    server.route("/whoami",
                 [](std::shared_ptr<Request> req, std::shared_ptr<Response> res, SslHandle ssl) -> asio::awaitable<void> {
        std::string who = "no TLS";
        if (ssl && *ssl) {
            X509* peer = SSL_get_peer_certificate(*ssl);
            who = std::string{"client="} + subject_name(peer);
            if (peer) X509_free(peer);
        }
        co_await res->status(200).send(who);
    });

    // Large one-shot response body (default 1 MiB) — exercises flow control /
    // multiple DATA frames (h2) or a large chunked body (h1). Size via ?n=BYTES.
    server.route("/big", [](std::shared_ptr<Request> req, std::shared_ptr<Response> res) -> asio::awaitable<void> {
        std::size_t n = query_uint(req->query(), "n", 1024 * 1024);
        co_await res->status(200).send(std::string(n, 'x'));
    });

    // Reads the entire request body and replies with its length — exercises
    // large body upload + flow control.
    server.route("/drain", [](std::shared_ptr<Request> req, std::shared_ptr<Response> res) -> asio::awaitable<void> {
        auto body = co_await req->body().read_all();
        if (!body) {
            co_await res->status(400).send("read error");
            co_return;
        }
        co_await res->status(200).send(std::format("received {} bytes", body->size()));
    });

    // Delays before responding (default 500 ms) — verifies the idle watchdog
    // does not kill an active-but-slow handler. Delay via ?ms=MILLIS.
    server.route("/delay", [](std::shared_ptr<Request> req, std::shared_ptr<Response> res) -> asio::awaitable<void> {
        int ms = static_cast<int>(query_uint(req->query(), "ms", 500));
        asio::steady_timer timer{co_await asio::this_coro::executor};
        timer.expires_after(std::chrono::milliseconds(ms));
        co_await timer.async_wait(asio::use_awaitable);
        co_await res->status(200).send(std::format("waited {} ms", ms));
    });

    // Handler that throws — the engine must not crash; the connection stays sane.
    server.route("/throw", [](std::shared_ptr<Request> req, std::shared_ptr<Response> res) -> asio::awaitable<void> {
        throw std::runtime_error("handler failure (intentional)");
        co_return;
    });

    // Empty response (204 No Content, no body).
    server.route("/empty", [](std::shared_ptr<Request> req, std::shared_ptr<Response> res) -> asio::awaitable<void> {
        co_await res->status(204).send("");
    });

    // Response carrying connection-specific headers that are illegal in HTTP/2;
    // the engine must strip them so the response stays valid.
    server.route("/badhdr", [](std::shared_ptr<Request> req, std::shared_ptr<Response> res) -> asio::awaitable<void> {
        co_await res->status(200)
            .header("connection", "keep-alive")
            .header("transfer-encoding", "chunked")
            .header("x-ok", "kept")
            .send("body-with-illegal-headers-stripped");
    });

    // Full-duplex, HTTP/2 only. Demonstrates a reader coroutine and a writer
    // coroutine running INDEPENDENTLY and CONCURRENTLY on the same stream, both
    // spawned AFTER the handler returns (they capture shared_ptr<Request>/<Response>
    // so both outlive the handler body):
    //   * reader: drains the request body frame by frame (upstream), on its own
    //   * writer: pushes a server frame every 200ms on its own schedule, then ends
    // Neither waits on the other — true bidirectional flow, which only HTTP/2
    // supports. HTTP/1.1 is half-duplex, so it falls back to read-then-reply.
    server.route("/duplex", [](std::shared_ptr<Request> req, std::shared_ptr<Response> res) -> asio::awaitable<void> {
        auto exec = co_await asio::this_coro::executor;

        if (req->version() != Version::Http2) {
            // HTTP/1.x: half-duplex. Read the whole body, then reply once.
            auto body = co_await req->body().read_all();
            std::size_t n = body ? body->size() : 0;
            co_await res->status(200).send(std::format("half-duplex (HTTP/1.x): drained {} bytes\n", n));
            co_return;
        }

        // Reader coroutine: independently consume the uplink body to completion.
        asio::co_spawn(
            exec,
            [req]() -> asio::awaitable<void> {
                int i = 0;
                for (;;) {
                    auto frame = co_await req->body().read();
                    if (!frame || frame->eof) break;
                    std::println("[SERVER] duplex <-- recv frame #{} ({} bytes): {}", i, frame->data.size(),
                                 frame->data);
                    ++i;
                }
                std::println("[SERVER] duplex reader done ({} frames)", i);
            },
            asio::detached);

        // Writer coroutine: independently push frames on a timer, not tied to reads.
        asio::co_spawn(
            exec,
            [res, exec]() -> asio::awaitable<void> {
                if (auto ec = co_await res->status(200).content_type("text/plain").begin(); ec) co_return;
                asio::steady_timer timer{exec};
                for (int i = 0; i < 5; ++i) {
                    timer.expires_after(std::chrono::milliseconds(200));
                    co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
                    if (auto ec = co_await res->write(std::format("push #{}\n", i)); ec) co_return;
                    std::println("[SERVER] duplex --> push #{}", i);
                }
                co_await res->finish("done\n");
                std::println("[SERVER] duplex writer done");
            },
            asio::detached);

        co_return;  // handler returns immediately; the two coroutines keep going
    });

    // Regex catch-all.
    server.route_regex("/api/.*",
                       [](std::shared_ptr<Request> req, std::shared_ptr<Response> res) -> asio::awaitable<void> {
        co_await res->status(200).send(std::string{"api path: "} + std::string{req->path()});
    });



    // Full-duplex WebSocket endpoint (ws over plaintext, wss over TLS):
    //   * a spawned writer coroutine pushes a server message every second
    //   * the read loop echoes each client message, preserving its text/binary type
    // Both write concurrently; the WebSocket serializes writes internally.
    server.ws_route("/chat", [](std::shared_ptr<Request> req, std::shared_ptr<WebSocket> ws) -> asio::awaitable<void> {
        std::println("[SERVER] websocket connected: {}", req->path());
        auto exec = co_await asio::this_coro::executor;

        // Concurrent writer: periodic server-initiated pushes.
        asio::co_spawn(
            exec,
            [ws]() -> asio::awaitable<void> {
                asio::steady_timer timer{co_await asio::this_coro::executor};
                for (int i = 0; ws->is_open(); ++i) {
                    timer.expires_after(std::chrono::seconds(1));
                    co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
                    if (!ws->is_open()) break;
                    if (co_await ws->write_text(std::format("server-push #{}", i))) break;
                }
                co_return;
            },
            asio::detached);

        // Read loop: echo each message back with the same type (text/binary).
        for (;;) {
            auto msg = co_await ws->read();
            if (!msg) {
                std::println("[SERVER] websocket closed: {}", msg.error().message());
                break;
            }
            std::println("[SERVER] ws recv ({} bytes, {}): {}", msg->data.size(), msg->text ? "text" : "binary",
                         msg->text ? msg->data : std::string{"<binary>"});
            if (auto ec = co_await ws->write("echo: " + msg->data, msg->text); ec) {
                break;
            }
        }
        co_return;
    });

    // Plain echo, for protocol-conformance suites (Autobahn's fuzzingclient).
    // It must return exactly what it was sent — same text/binary type, no prefix,
    // no server-initiated frames — or every case reads the extra traffic as a
    // protocol error. /chat above is the demo, this is the test surface.
    server.ws_route("/echo", [](std::shared_ptr<Request>, std::shared_ptr<WebSocket> ws) -> asio::awaitable<void> {
        for (;;) {
            auto msg = co_await ws->read();
            if (!msg) break;
            if (auto ec = co_await ws->write(msg->data, msg->text)) break;
        }
        co_return;
    });

    // Byte-level WebSocket reverse proxy: an Upgrade: websocket on /wsproxy is
    // spliced verbatim to the backend below. Frames, fragmentation, masking and
    // control frames all pass through untouched (no re-framing, no size cap).
    // Here the backend is this same plaintext server's /chat endpoint (the
    // request target is rewritten to /chat so the backend routes to a real ws
    // handler instead of looping back through the proxy), so /wsproxy behaves
    // exactly like connecting to /chat directly.
    server.ws_proxy("/wsproxy", "127.0.0.1", 7788, "/chat");

    // Regex proxy with capture-group rewrite: /proxy/<name> is spliced to the
    // backend with its target rewritten to /<name>. $1 is the first capture
    // group. e.g. /proxy/chat -> backend /chat. Every /proxy/* path routes to a
    // single rule, and the backend still sees the meaningful sub-path.
    server.ws_proxy_regex("/proxy/(.*)", "127.0.0.1", 7788, "/$1");

    // HTTP reverse proxy (request-level). /rproxy/<rest> is forwarded to this
    // same plaintext server with the target rewritten to /<rest>, so e.g.
    // /rproxy/world -> backend /world, /rproxy/echo -> backend /echo. Standard
    // X-Forwarded-* headers are added and the response is streamed back. Works
    // for HTTP/1.1, h2c and HTTP/2 clients (backend hop is HTTP/1.1).
    server.http_proxy_regex("/rproxy/(.*)", "127.0.0.1", 7788, "/$1");

    server.fallback([](std::shared_ptr<Request> req, std::shared_ptr<Response> res) -> asio::awaitable<void> {
        co_await res->status(404).send("not found");
    });
}

int main() {
    set_log_sink(make_stdout_sink(LogLevel::Info));

    // Plaintext server: HTTP/1.1, h2c upgrade, HTTP/2 prior-knowledge.
    ServerConfig plain_cfg;
    plain_cfg.listen = InetAddress{"0.0.0.0", 7788, false};
    plain_cfg.worker_threads = 4;
    Server plain{plain_cfg};
    register_routes(plain);

    // TLS server: HTTP/1.1 and HTTP/2 selected by ALPN, with mutual TLS.
    ServerConfig tls_cfg;
    tls_cfg.listen = InetAddress{"0.0.0.0", 7789, false};
    tls_cfg.worker_threads = 4;
    tls_cfg.tls = TlsConfig{
        .cert_chain_file = "./test/tls_certificates/server_cert.pem",
        .private_key_file = "./test/tls_certificates/server_key.pem",
        .mutual = true,
        .ca_file = "./test/tls_certificates/ca_cert.pem",
    };
    Server tls{tls_cfg};
    register_routes(tls);

    // Single-protocol plaintext listeners, for the conformance suites. Each one
    // declares what it speaks instead of sniffing, because on the sniffing port
    // above a malformed HTTP/2 opening and an HTTP/1.x request line are the same
    // bytes: h2spec's "invalid connection preface" wants a GOAWAY, h1spec's
    // "invalid prefix of request" wants a 400, and sniffing can only pick one.
    ServerConfig h2c_cfg;
    h2c_cfg.listen = InetAddress{"0.0.0.0", 7790, false};
    h2c_cfg.worker_threads = 4;
    h2c_cfg.plaintext_protocols = PlaintextProtocols::Http2;
    Server h2c{h2c_cfg};
    register_routes(h2c);

    ServerConfig h1_cfg;
    h1_cfg.listen = InetAddress{"0.0.0.0", 7791, false};
    h1_cfg.worker_threads = 4;
    h1_cfg.plaintext_protocols = PlaintextProtocols::Http1;
    Server h1{h1_cfg};
    register_routes(h1);

    bool ok = plain.start() && tls.start() && h2c.start() && h1.start();
    std::println("servers started: plaintext :7788 (sniffing), tls :7789, h2c-only :7790, h1-only :7791 (ok={})", ok);

    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(60));
    }
    return 0;
}
