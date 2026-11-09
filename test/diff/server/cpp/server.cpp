// C++ reference server for the server differential.
//
// One process, one route table, four listeners. The Go reference server
// (test/diff/server/go/server) registers the same observable behavior; a Go
// client probe sends identical requests to both and the runner compares the
// responses.
//
//   7820  plaintext, h1 + h2c sniffing
//   7821  plaintext, HTTP/2 only (h2c)
//   7822  TLS, ALPN h2 + http/1.1
//   7823  TCP + UDP (HTTP/3), TLS, no client certificate
//
// Static files are served from --static-root so the runner can create a
// deterministic tree (fixed mtimes and precompressed siblings) for both
// servers.

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <glaze/glaze.hpp>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "simple_http.h"

namespace asio = boost::asio;
using namespace simple_http;

namespace {

volatile std::sig_atomic_t g_stop = 0;

// The token port reports results as completion arguments; these coroutine
// adapters keep the reference server's value-or-error style.
template <typename T>
asio::awaitable<std::expected<T, error_code>> as_expected(asio::awaitable<std::tuple<error_code, T>> op) {
    auto [ec, value] = co_await std::move(op);
    if (ec)
        co_return std::unexpected{ec};
    co_return std::move(value);
}

inline asio::awaitable<error_code> as_ec(asio::awaitable<std::tuple<error_code>> op) {
    auto [ec] = co_await std::move(op);
    co_return ec;
}

inline asio::awaitable<std::expected<std::string, error_code>> body_read_all(Body &body) {
    return as_expected(body.async_read_all(asio::as_tuple(asio::use_awaitable)));
}

inline asio::awaitable<std::expected<WsMessage, error_code>> ws_read(WebSocket &ws) {
    return as_expected(ws.async_read(asio::as_tuple(asio::use_awaitable)));
}

inline asio::awaitable<error_code> ws_write(WebSocket &ws, std::string data, bool text) {
    return as_ec(ws.async_write(std::move(data), text, asio::as_tuple(asio::use_awaitable)));
}

void request_stop(int) { g_stop = 1; }

std::string read_file(const std::string &path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::size_t query_uint(std::string_view query, std::string_view key, std::size_t fallback) {
    std::string needle{key};
    needle += '=';
    for (std::size_t pos = 0; pos < query.size();) {
        const auto amp = query.find('&', pos);
        const auto pair = query.substr(pos, amp == std::string_view::npos ? std::string_view::npos : amp - pos);
        if (pair.starts_with(needle)) {
            std::size_t out = 0;
            const auto value = pair.substr(needle.size());
            const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
            return ec == std::errc{} ? out : fallback;
        }
        if (amp == std::string_view::npos) {
            break;
        }
        pos = amp + 1;
    }
    return fallback;
}

// --- application/framework types -------------------------------------------

struct AppState {
    std::string name{"from-state"};
};

struct UserParams {
    std::int64_t id{};
};

struct CreateUser {
    std::string name;
    int age{};
};

struct ListQuery {
    std::string q;
    int page{};
};

} // namespace

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

namespace {

struct MultipartEchoPart {
    std::string name;
    std::string filename;
    std::string content_type;
    std::size_t size{};
    std::string body_b64;
};

struct EchoResponse {
    std::string method;
    std::string path;
    std::string raw_query;
    std::string proto;
    std::string host;
    std::map<std::string, std::vector<std::string>> headers;
    std::map<std::string, std::vector<std::string>> trailers;
    std::string body_b64;
};

std::string proto_name(Version version) {
    switch (version) {
    case Version::Http2:
        return "HTTP/2";
    case Version::Http3:
        return "HTTP/3";
    default:
        return std::string{to_string(version)};
    }
}

asio::awaitable<std::string> echo_body(Request &req) {
    auto body = co_await body_read_all(req.body());
    if (!body) {
        co_return std::string{};
    }
    EchoResponse echo;
    echo.method = std::string{req.method_token()};
    echo.path = std::string{req.path()};
    echo.raw_query = std::string{req.query()};
    echo.proto = proto_name(req.version());
    echo.host = std::string{req.header("host").value_or("")};
    for (const auto &[name, value] : req.headers()) {
        if (name == "host" || name == "content-length" || name == "transfer-encoding" || name == "connection" ||
            name == "keep-alive" || name == "upgrade" || name == "proxy-connection" || name == "te" ||
            name == "trailer") {
            continue;
        }
        echo.headers[name].push_back(value);
    }
    for (const auto &[name, value] : req.trailers()) {
        echo.trailers[name].push_back(value);
    }
    echo.body_b64 = base64_encode(*body);
    auto encoded = glz::write_json(echo);
    co_return encoded ? std::move(*encoded) : std::string{"{}"};
}

asio::awaitable<void> echo_handler(std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res) {
    co_await res->status(status::ok).content_type("application/json").async_send(co_await echo_body(*req), asio::use_awaitable);
}

asio::awaitable<void> multipart_handler(std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res) {
    auto parts = co_await as_expected(read_multipart_body(*req, asio::as_tuple(asio::use_awaitable)));
    if (!parts) {
        co_await res->status(status::bad_request)
            .content_type("application/json")
            .async_send("{\"error\":\"not multipart\"}", asio::use_awaitable);
        co_return;
    }
    std::vector<MultipartEchoPart> out;
    for (const auto &part : *parts) {
        out.push_back(
            MultipartEchoPart{part.name, part.filename, part.content_type, part.data.size(), base64_encode(part.data)});
    }
    auto encoded = glz::write_json(out);
    co_await res->status(status::ok).content_type("application/json").async_send(encoded ? *encoded : "[]", asio::use_awaitable);
}

// The client certificate's Common Name, or "none" when the connection is not
// mutually authenticated. The Go reference exposes the same string from
// r.TLS.PeerCertificates so the probe can compare them.
std::string peer_common_name(SslHandle ssl) {
    if (!ssl || !*ssl) {
        return "none";
    }
    X509 *cert = SSL_get1_peer_certificate(*ssl);
    if (!cert) {
        return "none";
    }
    char name[256]{};
    const int length = X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, name, sizeof(name));
    X509_free(cert);
    return length > 0 ? std::string{name, static_cast<std::size_t>(length)} : std::string{"none"};
}

struct StaticFilesConfigArgs {
    std::string root;
};

std::shared_ptr<StaticFiles> load_static(const std::string &root) {
    StaticFilesConfig config;
    config.table.root = root;
    config.spa_fallback = "index.html";
    auto site = std::make_shared<StaticFiles>(std::move(config));
    std::string error;
    if (!site->load(error)) {
        std::cerr << "static root " << root << ": " << error << '\n';
        return nullptr;
    }
    return site;
}

template <typename ServerT> void register_routes(ServerT &server, const std::shared_ptr<StaticFiles> &site) {
    server.state(std::make_shared<AppState>());

    server.route(any_methods, "/plain", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(status::ok).async_send("hello", asio::use_awaitable);
    });
    server.route(any_methods, "/echo", echo_handler);
    server.route(any_methods, "/multipart", multipart_handler);
    // Mutually authenticated listeners expose the verified client certificate;
    // the rest report "none".
    server.route({Method::Get}, "/client-cert",
                 [](RequestPtr, ResponsePtr res, SslHandle ssl) -> asio::awaitable<void> {
                     co_await res->status(status::ok).async_send(peer_common_name(ssl), asio::use_awaitable);
                 });

    // The status template needs a numeric path capture.
    server.route({Method::Get}, "/status/{code}",
                 [](std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res) -> asio::awaitable<void> {
                     const auto value = req->param("code");
                     int code = 200;
                     if (value) {
                         std::from_chars(value->data(), value->data() + value->size(), code);
                     }
                     co_await res->status(code).async_send("status", asio::use_awaitable);
                 });

    server.route({Method::Get}, "/redirect/{code}",
                 [](std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res) -> asio::awaitable<void> {
                     int code = 302;
                     if (const auto value = req->param("code")) {
                         std::from_chars(value->data(), value->data() + value->size(), code);
                     }
                     co_await res->status(code).header("location", "/echo").async_send("", asio::use_awaitable);
                 });

    // Routing precedence: exact beats template beats catch-all.
    server.route({Method::Get}, "/files/static", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(status::ok).async_send("static", asio::use_awaitable);
    });
    server.route({Method::Get}, "/files/{id}",
                 [](std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res) -> asio::awaitable<void> {
                     co_await res->status(status::ok).async_send("id=" + std::string{req->param("id").value_or("")}, asio::use_awaitable);
                 });
    server.route({Method::Get}, "/files/{*rest}",
                 [](std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res) -> asio::awaitable<void> {
                     co_await res->status(status::ok).async_send("rest=" + std::string{req->param("rest").value_or("")}, asio::use_awaitable);
                 });
    server.route_regex({Method::Get}, "/api/.*",
                       [](std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res) -> asio::awaitable<void> {
                           co_await res->status(status::ok).async_send("api path: " + std::string{req->path()}, asio::use_awaitable);
                       });

    server.route({Method::Get, Method::Post}, "/method",
                 [](std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res) -> asio::awaitable<void> {
                     co_await res->status(status::ok).async_send(std::string{"method:"} + std::string{req->method_token()}, asio::use_awaitable);
                 });

    server.route(
        {Method::Get}, "/users/{id}",
        extract<Path<UserParams>>([](const UserParams &params, RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
            co_await res->status(status::ok).async_send("id=" + std::to_string(params.id), asio::use_awaitable);
        }));

    server.route({Method::Get}, "/state", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        const AppState *state = req->app_state<AppState>();
        co_await res->status(status::ok).async_send(state ? state->name : "missing", asio::use_awaitable);
    });
    server.route(
        {Method::Get}, "/list",
        extract<Query<ListQuery>>([](const ListQuery &query, RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
            co_await res->status(status::ok).async_send(query.q + ":" + std::to_string(query.page), asio::use_awaitable);
        }));
    server.route({Method::Post}, "/json",
                 extract<Json<CreateUser>>([](CreateUser value, RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                     co_await res->status(status::created).async_send(value.name + ":" + std::to_string(value.age), asio::use_awaitable);
                 }));
    server.route(
        {Method::Post}, "/valid",
        extract<Valid<Json<CreateUser>>>([](CreateUser value, RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
            co_await res->status(status::created).async_send(value.name, asio::use_awaitable);
        }));
    server.route({Method::Post}, "/form",
                 extract<Form<CreateUser>>([](CreateUser value, RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                     co_await res->status(status::created).async_send(value.name + ":" + std::to_string(value.age), asio::use_awaitable);
                 }));
    server.route({Method::Get}, "/header",
                 extract<Header<int, "x-page">, Cookie<std::string, "sid">>(
                     [](int page, std::string sid, RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                         co_await res->status(status::ok).async_send(std::to_string(page) + ":" + sid, asio::use_awaitable);
                     }));

    // Reverse proxy to this same server's /backend. Go's reference server uses
    // httputil.ReverseProxy, so the observable part is the backend body.
    server.route(any_methods, "/backend", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(status::ok).async_send("backend", asio::use_awaitable);
    });
    server.http_proxy("/proxy", HttpProxyTarget{.host = "127.0.0.1", .port = 7820, .rewrite_path = "/backend"});

    // Request decompression is opt-in middleware; the handler sees the decoded
    // bytes and the headers are removed, matching the Go reference handler.
    server.route({Method::Post}, "/decode",
                 {middleware::decompress_request(RequestDecompressionConfig{.max_decoded_bytes = 1 << 20})},
                 [](std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res) -> asio::awaitable<void> {
                     auto body = co_await body_read_all(req->body());
                     if (!body) {
                         const int code = body.error() == asio::error::message_size ? status::payload_too_large
                                                                                    : status::bad_request;
                         co_await res->status(code).async_send("decode error", asio::use_awaitable);
                         co_return;
                     }
                     const std::string ce{req->header(field::content_encoding).value_or("none")};
                     const std::string cl{req->header(field::content_length).value_or("none")};
                     co_await res->status(status::ok).async_send(std::format("ce={};cl={};", ce, cl) + *body, asio::use_awaitable);
                 });

    // Slow handler for the raw Rapid Reset probe: enough concurrent streams
    // must fill h2_max_concurrent_streams + h2_max_queued_streams.
    server.route(any_methods, "/delay",
                 [](std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res) -> asio::awaitable<void> {
                     const auto ms = static_cast<int>(query_uint(req->query(), "ms", 100));
                     asio::steady_timer timer{co_await asio::this_coro::executor};
                     timer.expires_after(std::chrono::milliseconds(ms));
                     co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
                     co_await res->status(status::ok).async_send("delayed", asio::use_awaitable);
                 });

    server.route(
        any_methods, "/drain",
        [](std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res) -> asio::awaitable<void> {
            auto body = co_await body_read_all(req->body());
            co_await res->status(status::ok).async_send("received " + std::to_string(body ? body->size() : 0) + " bytes", asio::use_awaitable);
        });
    server.route(any_methods, "/stream",
                 [](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter> res) -> asio::awaitable<void> {
                     co_await res->status(status::ok).content_type("text/plain").async_begin(asio::use_awaitable);
                     for (int i = 0; i < 3; ++i) {
                         co_await res->async_write("part" + std::to_string(i) + " ", asio::use_awaitable);
                     }
                     co_await res->async_finish("end\n", asio::use_awaitable);
                 });
    server.route(any_methods, "/trailers",
                 [](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter> res) -> asio::awaitable<void> {
                     co_await res->status(status::ok).content_type("text/plain").async_begin(asio::use_awaitable);
                     co_await res->async_write("body", asio::use_awaitable);
                     res->trailer("x-checksum", "abc123");
                     co_await res->async_finish(asio::use_awaitable);
                 });
    server.route(any_methods, "/sse",
                 [](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter> res) -> asio::awaitable<void> {
                     co_await res->async_sse_begin(asio::use_awaitable);
                     for (int i = 1; i <= 3; ++i) {
                         co_await res->async_sse_event("value-" + std::to_string(i), "tick", std::to_string(i), "", asio::use_awaitable);
                     }
                     co_await res->async_finish(asio::use_awaitable);
                 });

    server.route(any_methods, "/trailer-echo",
                 [](std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res) -> asio::awaitable<void> {
                     (void)co_await body_read_all(req->body());
                     const auto check = req->trailers().get("x-check");
                     co_await res->status(status::ok).async_send(check ? std::string{*check} : std::string{"missing"}, asio::use_awaitable);
                 });
    server.route(any_methods, "/expect-ok",
                 [](std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res) -> asio::awaitable<void> {
                     co_await res->async_send_continue(asio::use_awaitable);
                     auto body = co_await body_read_all(req->body());
                     co_await res->status(status::ok).async_send("got " + std::to_string(body ? body->size() : 0), asio::use_awaitable);
                 });
    server.route(any_methods, "/expect-reject", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(status::expectation_failed).async_send("no", asio::use_awaitable);
    });

    // Host routing.
    server.route({Method::Get}, "/who", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(status::ok).async_send("root", asio::use_awaitable);
    });
    server.host("api.example.com", [](Router &api) {
        api.route({Method::Get}, "/who", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
            co_await res->status(status::ok).async_send("api", asio::use_awaitable);
        });
    });
    server.host("*.example.com", [](Router &sub) {
        sub.route({Method::Get}, "/who", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
            co_await res->status(status::ok).async_send("sub", asio::use_awaitable);
        });
    });
    server.host("api.example.com:8080", [](Router &port_router) {
        port_router.route({Method::Get}, "/who", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
            co_await res->status(status::ok).async_send("port", asio::use_awaitable);
        });
    });

    // Middleware surfaces.
    server.route(any_methods, "/gzip", {middleware::compress(CompressionConfig{.min_bytes = 1})},
                 [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                     co_await res->status(status::ok).content_type("text/plain").async_send(std::string(2048, 'c'), asio::use_awaitable);
                 });
    server.route(
        any_methods, "/cors",
        {make_cors_middleware(CorsConfig{.allow_origins = {"https://*.example.com"}, .allow_credentials = true})},
        [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(status::ok).async_send("cors", asio::use_awaitable); });
    server.route(
        any_methods, "/secure",
        {middleware::secure_headers(middleware::SecureHeadersConfig{.content_security_policy = "default-src 'self'",
                                                                    .hsts_max_age = std::chrono::seconds{60}})},
        [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(status::ok).async_send("secure", asio::use_awaitable); });
    server.route(
        any_methods, "/basic", {middleware::basic_auth("user", "pass")},
        [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(status::ok).async_send("ok", asio::use_awaitable); });
    server.route(
        any_methods, "/request-id", {middleware::request_id()},
        [](std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res) -> asio::awaitable<void> {
            co_await res->status(status::ok).async_send(std::string{req->header("x-request-id").value_or("missing")}, asio::use_awaitable);
        });
    auto limiter = std::make_shared<middleware::RateLimiter>(/*rate=*/0.001, /*burst=*/1, /*max_keys=*/16);
    server.route(
        any_methods, "/rate", {middleware::rate_limit(limiter)},
        [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(status::ok).async_send("ok", asio::use_awaitable); });

    // Session / CSRF / JWT.
    auto sessions = std::make_shared<MemorySessionStore>();
    auto session_mw = middleware::session(
        sessions, SessionConfig{.cookie_name = "sid", .same_site = "Lax", .secure = false, .http_only = true});
    server.route({Method::Post}, "/session/set", {session_mw},
                 [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
                     if (auto *s = session(*req)) {
                         s->set("uid", "42");
                     }
                     co_await res->status(status::no_content).async_send("", asio::use_awaitable);
                 });
    server.route({Method::Get}, "/session/get", {session_mw},
                 [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
                     auto *s = session(*req);
                     const auto uid = s ? s->get("uid") : std::nullopt;
                     co_await res->status(status::ok).async_send(uid ? std::string{*uid} : "none", asio::use_awaitable);
                 });
    auto csrf_mw = middleware::csrf(CsrfConfig{
        .cookie_name = "csrf", .header_name = "x-csrf-token", .secure = false, .exempt_paths = {"/csrf/exempt"}});
    server.route({Method::Get}, "/csrf/token", {csrf_mw}, [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(status::ok).async_send(std::string{csrf_token(*req)}, asio::use_awaitable);
    });
    server.route({Method::Post}, "/csrf/submit", {csrf_mw}, [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(status::no_content).async_send("", asio::use_awaitable);
    });
    server.route({Method::Post}, "/csrf/exempt", {csrf_mw}, [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(status::no_content).async_send("", asio::use_awaitable);
    });
    server.route(
        {Method::Get}, "/jwt/me",
        {middleware::jwt(JwtConfig{.algorithms = {"HS256"}, .secret = "secret", .issuer = "iss", .audience = "aud"})},
        [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
            const Principal *principal_value = principal(*req);
            co_await res->status(status::ok).async_send(principal_value ? principal_value->subject : std::string{"missing"}, asio::use_awaitable);
        });

    // WebSocket.
    // Origin-gated WebSocket route: proves per-route middleware runs on the
    // upgrade path (CSWSH defense). The Go reference uses gorilla's CheckOrigin.
    server.ws_route("/ws/guarded", {middleware::ws_origin({"https://app.example.com"})},
                    [](std::shared_ptr<Request>, std::shared_ptr<WebSocket> ws) -> asio::awaitable<void> {
                        for (;;) {
                            auto msg = co_await ws_read(*ws);
                            if (!msg) {
                                break;
                            }
                            if (co_await ws_write(*ws, msg->data, msg->text)) {
                                break;
                            }
                        }
                    });
    server.ws_route("/ws/echo", [](std::shared_ptr<Request>, std::shared_ptr<WebSocket> ws) -> asio::awaitable<void> {
        for (;;) {
            auto msg = co_await ws_read(*ws);
            if (!msg) {
                break;
            }
            if (auto ec = co_await ws_write(*ws, msg->data, msg->text)) {
                break;
            }
        }
    });

    if (site) {
        server.static_files(site);
    }
    server.fallback([](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(status::not_found).async_send("not found", asio::use_awaitable);
    });
}

} // namespace

int main(int argc, char **argv) {
    set_log_sink(make_stderr_sink(LogLevel::Warn));
    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);

    std::string static_root = "./test/diff/server/fixtures/static";
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--static-root" && i + 1 < argc) {
            static_root = argv[++i];
        }
    }
    auto site = load_static(static_root);

    ServerConfig sniff_cfg;
    sniff_cfg.listen = InetAddress{"0.0.0.0", 7820, false};
    sniff_cfg.worker_threads = 2;
    sniff_cfg.limits.max_body_bytes = 1024 * 1024;
    Server sniff{sniff_cfg};
    register_routes(sniff, site);

    ServerConfig h2c_cfg;
    h2c_cfg.listen = InetAddress{"0.0.0.0", 7821, false};
    h2c_cfg.worker_threads = 2;
    h2c_cfg.plaintext_protocols = PlaintextProtocols::Http2;
    h2c_cfg.limits.max_body_bytes = 1024 * 1024;
    // The raw Rapid Reset probe needs the dispatch-queue bound reachable. At the
    // default (queued == concurrent) a new stream hits the RFC 9113 §5.1.2
    // REFUSED_STREAM cap before the queue can overflow, so keep the queue small
    // on this fixture-only listener. No stock request opens enough streams to
    // notice; only the h2raw probe resets open streams to free table slots.
    h2c_cfg.limits.h2_max_queued_streams = 1;
    Server h2c{h2c_cfg};
    register_routes(h2c, site);

    // Pure HTTP/1.x listener for raw request-parsing cases: the sniffing
    // listener cannot decide whether "GARBAGE" is a bad h1 line or an h2
    // preface until it sees more bytes, while a declared h1 listener answers
    // 400 immediately (the same reason conformance uses single-protocol ports).
    ServerConfig h1_cfg;
    h1_cfg.listen = InetAddress{"0.0.0.0", 7824, false};
    h1_cfg.worker_threads = 2;
    h1_cfg.plaintext_protocols = PlaintextProtocols::Http1;
    h1_cfg.limits.max_body_bytes = 1024 * 1024;
    Server h1{h1_cfg};
    register_routes(h1, site);

    ServerConfig tls_cfg;
    tls_cfg.listen = InetAddress{"0.0.0.0", 7822, false};
    tls_cfg.worker_threads = 2;
    tls_cfg.limits.max_body_bytes = 1024 * 1024;
    tls_cfg.tls = TlsConfig{
        .cert_chain_file = "./test/tls_certificates/server_cert.pem",
        .private_key_file = "./test/tls_certificates/server_key.pem",
    };
    Server tls{tls_cfg};
    register_routes(tls, site);

    // Mutual TLS: the handshake fails unless the client presents a certificate
    // signed by the test CA. /client-cert reports the verified CN.
    ServerConfig mtls_cfg;
    mtls_cfg.listen = InetAddress{"0.0.0.0", 7825, false};
    mtls_cfg.worker_threads = 2;
    mtls_cfg.limits.max_body_bytes = 1024 * 1024;
    mtls_cfg.tls = TlsConfig{
        .cert_chain_file = "./test/tls_certificates/server_cert.pem",
        .private_key_file = "./test/tls_certificates/server_key.pem",
        .mutual = true,
        .ca_file = "./test/tls_certificates/ca_cert.pem",
    };
    Server mtls{mtls_cfg};
    register_routes(mtls, site);

    bool ok = sniff.start() && h2c.start() && h1.start() && tls.start() && mtls.start();
    std::cout << "diff server cpp: :7820 sniff :7821 h2c :7822 tls :7824 h1 :7825 mtls (ok=" << ok << ")\n";

#ifdef SIMPLE_HTTP_ENABLE_HTTP3
    ServerConfig h3_cfg;
    h3_cfg.listen = InetAddress{"0.0.0.0", 7823, false};
    h3_cfg.quic = InetAddress{"0.0.0.0", 7823, false};
    h3_cfg.worker_threads = 2;
    h3_cfg.reuse_port = true;
    h3_cfg.limits.max_body_bytes = 1024 * 1024;
    h3_cfg.tls = TlsConfig{
        .cert_chain_file = "./test/tls_certificates/server_cert.pem",
        .private_key_file = "./test/tls_certificates/server_key.pem",
    };
    Server h3{h3_cfg};
    register_routes(h3, site);
    const bool h3_ok = static_cast<bool>(h3.start());
    std::cout << "diff server cpp: :7823 tcp+udp h3 (ok=" << h3_ok << ")\n";
    ok = ok && h3_ok;
#endif

    if (!ok) {
        return 1;
    }
    std::fflush(stdout);
    std::cout << "READY diff-server-cpp\n";
    std::fflush(stdout);

    while (g_stop == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    sniff.stop();
    mtls.stop();
    h2c.stop();
    h1.stop();
    tls.stop();
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
    h3.stop();
#endif
    return 0;
}
