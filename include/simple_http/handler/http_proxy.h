#pragma once

// HTTP reverse proxy — request-level, modelled on Go's
// net/http/httputil.ReverseProxy, driven by the client layer.
//
// Unlike the byte-level WebSocket tunnel (engine/h1/ws_proxy.h), this operates
// on the parsed Request/Response abstraction, one request at a time, and the
// upstream leg is an ordinary client exchange: the request is re-framed there
// (chunked body on HTTP/1.1, DATA frames on HTTP/2), and the response arrives
// as a head plus body chunks. So the backend may be plaintext or TLS,
// HTTP/1.1 or HTTP/2 — see HttpProxyTarget.
//
// The shape follows Go's ReverseProxy:
//   * hop-by-hop fields are removed on both legs, including every field named
//     by any Connection header (RFC 9110 §7.6.1), and HTTP2-Settings;
//   * X-Forwarded-For / -Proto / -Host are authored by the proxy (the inbound
//     forwarding fields are stripped first, Go's Rewrite mode; preserve the
//     inbound X-Forwarded-For chain with preserve_inbound_forwarding);
//   * TE: trailers is kept alive (gRPC), and upstream trailers are relayed;
//   * Rewrite / ModifyResponse / ErrorHandler hooks are available per route;
//   * CONNECT is rejected (Go answers 405), never forwarded;
//   * only replay-safe requests are retried after anything reached the wire,
//     and request-body failures abort the upstream leg instead of sending a
//     truncated body as if it were complete.
//
// Connection shape: per request. The handler runs on the connection executor,
// so the client it builds is pinned to that executor and all I/O for the
// request is single-threaded; the client — and its one upstream connection —
// is released when the exchange ends. This is nginx's default upstream
// behaviour (no keep-alive pool): a fresh TCP/TLS connection per proxied
// request, trading a handshake per request for a cache-free, lock-free,
// head-of-line-free proxy. A backend behind a tunnel that needs reuse would
// want a pool instead.
//
// Deliberate limits relative to Go (documented rather than hidden):
//   * HTTP/1.1 request bodies are streamed as chunked; a known Content-Length
//     is not preserved (the proxy never buffers the body);
//   * generic 101 Switching Protocols is not tunnelled here — WebSocket has its
//     own byte-level route (ws_proxy), and HTTP/1.1 Upgrade requests that reach
//     this handler are rejected with 501 rather than silently downgraded;
//   * informational responses other than the local 100-continue handshake are
//     not relayed by the client layer; request trailers have no upstream
//     client API yet. `Expect: 100-continue` is answered locally (RFC 9110
//     §10.1.1 permits an intermediary to), so a backend cannot reject the body
//     before it arrives.

#include <array>
#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "../client/http.h" // simple_http::Client — the upstream leg lives on the new API
#include "../core/http_status.h"
#include "../core/logging.h"
#include "../core/types.h"
#include "../engine/dispatcher.h" // HttpProxyTarget / HttpProxyHooks
#include "../proto/headers.h"
#include "../proto/request.h"
#include "../proto/response.h"

namespace simple_http {

namespace asio = boost::asio;

namespace detail {

// Hop-by-hop headers that must not be forwarded (RFC 9110 §7.6.1). Field names
// are compared lowercased (Headers stores them lowercased already).
inline bool is_hop_by_hop(std::string_view name) {
    static constexpr std::string_view kHop[] = {
        "connection",
        "keep-alive",
        "proxy-authenticate",
        "proxy-authorization",
        "proxy-connection",
        "te",
        "trailer",
        "transfer-encoding",
        "upgrade",
        // RFC 7540 §3.2.2.2: only meaningful for the h2c Upgrade request.
        "http2-settings",
    };
    for (auto h : kHop) {
        if (name == h)
            return true;
    }
    return false;
}

inline std::string to_lower(std::string_view s) {
    std::string out{s};
    for (auto &c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Every value of the Connection header names a field that is hop-by-hop for
// this connection. Multiple Connection fields are legal in HTTP/1.x (Go's
// removeHopByHopHeaders iterates all of them), so reading only the first one
// would leave a second field's nominated headers attached to the upstream
// message.
inline std::vector<std::string> connection_tokens(const Headers &headers) {
    std::vector<std::string> tokens;
    for (auto conn : headers.get_all("connection")) {
        std::string_view v{conn};
        std::size_t pos = 0;
        while (pos < v.size()) {
            std::size_t comma = v.find(',', pos);
            std::string_view tok =
                v.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
            // trim OWS
            while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t'))
                tok.remove_prefix(1);
            while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t'))
                tok.remove_suffix(1);
            if (!tok.empty())
                tokens.push_back(to_lower(tok));
            if (comma == std::string_view::npos)
                break;
            pos = comma + 1;
        }
    }
    return tokens;
}

inline bool contains_token(const std::vector<std::string> &tokens, std::string_view name) {
    for (const auto &t : tokens) {
        if (t == name)
            return true;
    }
    return false;
}

// Whether a single comma-separated field value contains `lower_token`
// (case-insensitively). Go's httpguts.HeaderValuesContainsToken, used to keep
// `TE: trailers` alive across the proxy.
inline bool value_has_token(std::string_view value, std::string_view lower_token) {
    std::size_t pos = 0;
    while (pos <= value.size()) {
        std::size_t comma = value.find(',', pos);
        std::string_view tok =
            value.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
        while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t'))
            tok.remove_prefix(1);
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t'))
            tok.remove_suffix(1);
        if (to_lower(tok) == lower_token)
            return true;
        if (comma == std::string_view::npos)
            break;
        pos = comma + 1;
    }
    return false;
}

// Fences one client awaitable against the request-scoped cancellation token.
// operator|| waits for the loser to finish before returning, so the caller can
// safely abort the upstream stream afterwards. Returns operation_aborted when
// the request was cancelled (the caller then returns without touching the
// frontend response).
template <typename T>
asio::awaitable<std::expected<T, error_code>> await_cancelable(const std::shared_ptr<Request> &req,
                                                               asio::awaitable<std::expected<T, error_code>> op) {
    if (req->cancelled())
        co_return std::unexpected{make_error_code(asio::error::operation_aborted)};
    using namespace asio::experimental::awaitable_operators;
    auto outcome = co_await (std::move(op) || req->wait_cancelled());
    if (auto *value = std::get_if<std::expected<T, error_code>>(&outcome))
        co_return std::move(*value);
    co_return std::unexpected{make_error_code(asio::error::operation_aborted)};
}

inline asio::awaitable<error_code> await_cancelable(const std::shared_ptr<Request> &req,
                                                    asio::awaitable<error_code> op) {
    if (req->cancelled())
        co_return make_error_code(asio::error::operation_aborted);
    using namespace asio::experimental::awaitable_operators;
    auto outcome = co_await (std::move(op) || req->wait_cancelled());
    if (auto *value = std::get_if<error_code>(&outcome))
        co_return *value;
    co_return make_error_code(asio::error::operation_aborted);
}

} // namespace detail

// Runs one request-level HTTP reverse-proxy exchange: forwards `req` to the
// backend `target` through `client` and streams the response back through
// `res`. On any upstream failure a 502 Bad Gateway is sent while the response
// has not started yet; a failure once the body is flowing aborts the frontend
// response instead (a truncated body must never look complete).
inline asio::awaitable<void> run_http_proxy(std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res,
                                            bool client_is_tls, HttpProxyTarget target, simple_http::Client &client) {
    auto send_error = [&](int status, error_code ec) -> asio::awaitable<void> {
        if (req->cancelled() || res->started())
            co_return;
        if (!co_await res->connected())
            co_return;
        if (target.hooks.error_handler) {
            try {
                co_await target.hooks.error_handler(res, *req, ec);
            } catch (const std::exception &e) {
                SIMPLE_HTTP_ERROR_LOG("http-proxy: error handler threw: {}", e.what());
            } catch (...) {
                SIMPLE_HTTP_ERROR_LOG("http-proxy: error handler threw an unknown exception");
            }
            if (res->started())
                co_return;
        }
        (void)co_await res->status(status).content_type("text/plain").send(std::string{reason_phrase(status)});
        co_return;
    };

    // RFC 9110 §9.3.6: CONNECT turns the connection into a tunnel; this
    // request-level proxy cannot honour that. Go answers 405 rather than
    // forwarding a half-understood request.
    if (req->method() == Method::Connect) {
        SIMPLE_HTTP_ERROR_LOG("http-proxy: refusing CONNECT (not a tunnel proxy)");
        co_await send_error(status::method_not_allowed, make_error_code(asio::error::operation_not_supported));
        co_return;
    }

    // RFC 9110 §7.8: an Upgrade reaches this handler only when no WebSocket
    // route claimed it. A generic 101 tunnel is not implemented here (the
    // byte-level ws_proxy is the WebSocket path), so reject it explicitly
    // instead of stripping Upgrade and letting the backend answer as 200.
    // h2c is the exception Go makes too: a frontend may use the h2c Upgrade
    // handshake on its leg to *this* proxy; the proxy answers it as an ordinary
    // HTTP/1.1 request and forwards a plain request, never the upgrade itself.
    {
        bool connection_upgrade = false;
        for (auto conn : req->headers().get_all("connection")) {
            if (detail::value_has_token(conn, "upgrade")) {
                connection_upgrade = true;
                break;
            }
        }
        if (connection_upgrade) {
            if (auto upgrade = req->header("upgrade"); upgrade && !detail::value_has_token(*upgrade, "h2c")) {
                SIMPLE_HTTP_ERROR_LOG("http-proxy: refusing unsupported Upgrade '{}'", *upgrade);
                co_await send_error(status::not_implemented, make_error_code(asio::error::operation_not_supported));
                co_return;
            }
        }
    }

    // --- the request to forward ---
    // The rewrite template operates on the path only (Router matches req->path(),
    // which excludes the query). Preserve the original query string so it is not
    // lost when a rewrite is applied; if the rewrite itself already contains a
    // '?', respect it and do not append the original query.
    std::string forwarded_target;
    if (target.rewrite_path.empty()) {
        forwarded_target = std::string{req->target()};
    } else {
        forwarded_target = target.rewrite_path;
        if (forwarded_target.find('?') == std::string::npos && !req->query().empty()) {
            forwarded_target.push_back('?');
            forwarded_target.append(req->query());
        }
    }

    // Defence in depth: never splice a control byte into the request line or a
    // header we synthesize. The engines reject such bytes at parse time (h1
    // cannot produce them, h2 validates them), but a proxy must not rely on its
    // caller.
    const std::string_view method_token = req->method_token();
    if (method_token.empty() || contains_ctl(method_token) || contains_ctl(forwarded_target)) {
        SIMPLE_HTTP_ERROR_LOG("http-proxy: refusing request line with an empty method or CR/LF/NUL");
        co_await send_error(status::bad_request, make_error_code(asio::error::invalid_argument));
        co_return;
    }

    auto spec =
        std::make_shared<simple_http::Request>(simple_http::Version::Http11, co_await asio::this_coro::executor);
    spec->set_method_token(std::string{method_token});
    spec->set_target(std::move(forwarded_target));

    // Copy the frontend's fields, minus what the client layer owns (Host,
    // Content-Length, the connection's own headers), minus anything the
    // Connection header lists, and minus the forwarding fields this proxy
    // authors itself. Go's Rewrite mode strips the latter before the hook runs;
    // the hook can copy them back from `in` when a deployment needs that.
    auto req_conn_tokens = detail::connection_tokens(req->headers());
    std::vector<std::string_view> prior_xff;
    for (const auto &[name, value] : req->headers()) {
        if (detail::is_hop_by_hop(name) || detail::contains_token(req_conn_tokens, name))
            continue;
        if (name == "host" || name == "expect" || name == "content-length")
            continue;
        if (name == "forwarded" || name == "x-forwarded-host" || name == "x-forwarded-proto")
            continue;
        if (name == "x-forwarded-for") {
            if (target.preserve_inbound_forwarding)
                prior_xff.push_back(value);
            continue;
        }
        if (contains_ctl(name) || contains_ctl(value)) {
            SIMPLE_HTTP_ERROR_LOG("http-proxy: refusing header with CR/LF/NUL: {}", name);
            co_await send_error(status::bad_request, make_error_code(asio::error::invalid_argument));
            co_return;
        }
        spec->mutable_headers().add_lower(name, value);
    }

    // X-Forwarded-For: by default only the real peer (Go's Rewrite mode); the
    // legacy Director-style chain is available explicitly.
    std::string xff;
    if (target.preserve_inbound_forwarding) {
        for (const auto &prior : prior_xff) {
            if (!xff.empty())
                xff.append(", ");
            xff.append(prior);
        }
    }
    if (!xff.empty())
        xff.append(", ");
    xff.append(req->peer_address());
    spec->mutable_headers().add_lower("x-forwarded-for", std::move(xff));
    spec->mutable_headers().add_lower("x-forwarded-proto", client_is_tls ? "https" : "http");
    if (auto host = req->header("host"))
        spec->mutable_headers().add_lower("x-forwarded-host", std::string{*host});

    // Host policy: the client layer writes the backend authority unless the
    // Request carries an explicit `host` field (h1 and h2 honour it).
    if (!target.host_header.empty()) {
        if (contains_ctl(target.host_header)) {
            SIMPLE_HTTP_ERROR_LOG("http-proxy: refusing configured host header with CR/LF/NUL");
            co_await send_error(status::bad_gateway, make_error_code(asio::error::invalid_argument));
            co_return;
        }
        spec->mutable_headers().add_lower("host", target.host_header);
    } else if (target.preserve_host) {
        if (auto host = req->header("host"))
            spec->mutable_headers().add_lower("host", std::string{*host});
    }

    // `TE` is hop-by-hop, but RFC 9110 §6.1 / Go's ReverseProxy keep exactly
    // `TE: trailers` alive: it is how an HTTP/2-capable peer (gRPC in particular)
    // is told the response may end in a trailer section, which the proxy then
    // forwards. Dropping it silently breaks grpc-status trailers.
    for (auto te : req->headers().get_all("te")) {
        if (detail::value_has_token(te, "trailers")) {
            spec->mutable_headers().add_lower("te", "trailers");
            break;
        }
    }

    // Go suppresses net/http's default User-Agent by setting an empty value.
    // The client engines here have their own default, so an empty field is the
    // equivalent: a transparent proxy must not invent an identity.
    if (!spec->headers().contains("user-agent"))
        spec->mutable_headers().add_lower("user-agent", "");

    // Does the request carry a body? Header hints work for HTTP/1.x but NOT for
    // HTTP/2, which frames the body with END_STREAM and carries neither header,
    // so the body is probed: read the first frame now and forward it once the
    // upstream exchange is open. A client that sent `Expect: 100-continue` waits
    // for a go-ahead before sending its body: answer it here (RFC 9110 §10.1.1
    // allows an intermediary to), otherwise this read would block until the
    // client gives up.
    if (auto expect = req->header("expect");
        expect && detail::to_lower(*expect).find("100-continue") != std::string::npos) {
        (void)co_await res->writer().send_continue();
    }
    std::string first_chunk;
    bool has_body = false;
    {
        auto frame = co_await detail::await_cancelable(req, req->body().read());
        if (req->cancelled())
            co_return;
        if (!frame) {
            // The frontend's body stream failed (disconnect/reset). It must not
            // become a bodyless or truncated request at the backend: nothing has
            // been sent there yet, so answer the frontend and stop.
            SIMPLE_HTTP_ERROR_LOG("http-proxy: reading the request body failed: {}", frame.error().message());
            co_await send_error(status::bad_request, frame.error());
            co_return;
        }
        if (!frame->eof) {
            has_body = true;
            first_chunk = std::move(frame->data);
        }
    }
    spec->set_stream_body(has_body);

    // Rewrite hook runs after the standard rewrite and before the body is sent,
    // like Go's ProxyRequest.Rewrite. A throwing hook is a proxy fault (500).
    if (target.hooks.rewrite) {
        std::string hook_error;
        try {
            target.hooks.rewrite(*req, *spec);
        } catch (const std::exception &e) {
            hook_error = e.what();
        } catch (...) {
            hook_error = "unknown exception";
        }
        if (!hook_error.empty()) {
            SIMPLE_HTTP_ERROR_LOG("http-proxy: rewrite hook threw: {}", hook_error);
            co_await send_error(status::internal_server_error, make_error_code(asio::error::invalid_argument));
            co_return;
        }
    }

    // --- the backend this target names ---
    simple_http::detail::ClientTarget upstream;
    upstream.host = target.host;
    upstream.port = target.port;
    upstream.use_tls = target.tls;
    if (target.tls) {
        upstream.version = simple_http::HttpVersionPolicy::Auto; // ALPN: h2 when the backend offers it
    } else if (target.h2c || target.h2c_prior_knowledge) {
        upstream.version = simple_http::HttpVersionPolicy::Http2;
        upstream.h2c =
            target.h2c_prior_knowledge ? simple_http::H2cMode::PriorKnowledge : simple_http::H2cMode::Upgrade;
    } else {
        upstream.version = simple_http::HttpVersionPolicy::Http11; // the historical default: plain HTTP/1.1
        upstream.h2c = simple_http::H2cMode::Off;
    }

    // A request may be replayed only when nothing it says can have been acted
    // on twice. A body that has been streamed cannot be rebuilt; a POST/PATCH —
    // even a bodyless one — is not idempotent. This is the same rule the client
    // layer uses (detail::retry_allowed), applied to the proxy's own retry loop.
    auto transport_drop = [](const error_code &ec) {
        return simple_http::detail::transport_failure(ec) || simple_http::is_retryable(ec);
    };
    const bool replayable = !has_body && is_idempotent(req->method());
    const std::size_t max_attempts = target.max_attempts == 0 ? 1 : target.max_attempts;
    for (std::size_t attempt = 0; attempt < max_attempts; ++attempt) {
        const bool last_attempt = attempt + 1 == max_attempts;
        auto opened = co_await detail::await_cancelable(req, client.open_target(upstream, spec));
        if (req->cancelled())
            co_return;
        if (!opened) {
            const error_code ec = opened.error();
            // Nothing of the request has reached the wire when the open itself
            // failed, but a bodyless POST is still not safe to replay: the
            // client may have sent the head before the failure (h2c upgrade).
            if (!last_attempt && replayable && transport_drop(ec)) {
                SIMPLE_HTTP_ERROR_LOG("http-proxy: opening {}:{} failed ({}), retrying", target.host, target.port,
                                      ec.message());
                continue;
            }
            SIMPLE_HTTP_ERROR_LOG("http-proxy: no upstream connection to {}:{} ({})", target.host, target.port,
                                  ec.message());
            co_await send_error(status::bad_gateway, ec);
            co_return;
        }
        auto &stream = opened->stream;

        // --- request body ---
        if (has_body) {
            error_code body_ec;
            if (!first_chunk.empty())
                body_ec = co_await detail::await_cancelable(req, stream->write(std::move(first_chunk)));
            for (; !body_ec;) {
                auto frame = co_await detail::await_cancelable(req, req->body().read());
                if (req->cancelled()) {
                    (void)co_await stream->abort();
                    co_return;
                }
                if (!frame) {
                    // A failed inbound body cannot be presented upstream as a
                    // complete body: abort instead of finish(), otherwise the
                    // backend acts on a silently shortened request.
                    SIMPLE_HTTP_ERROR_LOG("http-proxy: request body failed mid-stream: {}", frame.error().message());
                    (void)co_await stream->abort();
                    co_await send_error(status::bad_request, frame.error());
                    co_return;
                }
                if (frame->eof)
                    break;
                if (frame->data.empty())
                    continue;
                body_ec = co_await detail::await_cancelable(req, stream->write(std::move(frame->data)));
            }
            if (req->cancelled()) {
                (void)co_await stream->abort();
                co_return;
            }
            if (!body_ec)
                body_ec = co_await detail::await_cancelable(req, stream->finish(std::string{}));
            if (req->cancelled()) {
                (void)co_await stream->abort();
                co_return;
            }
            if (body_ec) {
                // The request is half-sent: it cannot be replayed, and the
                // frontend has not seen a response yet.
                SIMPLE_HTTP_ERROR_LOG("http-proxy: upstream {}:{} rejected the body ({})", target.host, target.port,
                                      body_ec.message());
                (void)co_await stream->abort();
                co_await send_error(status::bad_gateway, body_ec);
                co_return;
            }
        }

        // --- the backend's response head ---
        auto head = co_await detail::await_cancelable(req, stream->read_head());
        if (req->cancelled()) {
            (void)co_await stream->abort();
            co_return;
        }
        if (!head) {
            const error_code ec = head.error();
            if (!last_attempt && replayable && transport_drop(ec)) {
                SIMPLE_HTTP_ERROR_LOG("http-proxy: {}:{} dropped the connection before answering ({}), retrying",
                                      target.host, target.port, ec.message());
                (void)co_await stream->abort();
                continue;
            }
            SIMPLE_HTTP_ERROR_LOG("http-proxy: no response head from {}:{} ({})", target.host, target.port,
                                  ec.message());
            (void)co_await stream->abort();
            co_await send_error(status::bad_gateway, ec);
            co_return;
        }

        // --- response head: strip hop-by-hop fields, then let the hook adjust ---
        Headers out_headers;
        const auto resp_conn_tokens = detail::connection_tokens(head->headers);
        for (const auto &[name, value] : head->headers) {
            if (detail::is_hop_by_hop(name) || detail::contains_token(resp_conn_tokens, name))
                continue;
            // We stream the body, so the upstream Content-Length is replaced by
            // our own framing — except for HEAD (and 304, where it describes the
            // selected representation), whose response carries the headers a GET
            // would have produced and no body at all.
            if (name == "content-length" && (head->status == status::no_content ||
                                             (req->method() != Method::Head && head->status != status::not_modified))) {
                continue;
            }
            if (contains_ctl(name) || contains_ctl(value)) {
                SIMPLE_HTTP_ERROR_LOG("http-proxy: dropping upstream header with CR/LF/NUL: {}", name);
                continue;
            }
            out_headers.add_lower(name, value);
        }

        int out_status = head->status;
        if (target.hooks.modify_response) {
            error_code ec;
            try {
                ec = target.hooks.modify_response(out_status, out_headers, *req);
            } catch (const std::exception &e) {
                SIMPLE_HTTP_ERROR_LOG("http-proxy: modify-response hook threw: {}", e.what());
                ec = make_error_code(asio::error::invalid_argument);
            } catch (...) {
                SIMPLE_HTTP_ERROR_LOG("http-proxy: modify-response hook threw an unknown exception");
                ec = make_error_code(asio::error::invalid_argument);
            }
            if (ec) {
                (void)co_await stream->abort();
                co_await send_error(status::bad_gateway, ec);
                co_return;
            }
        }
        if (out_status < 200 || out_status > 599) {
            SIMPLE_HTTP_ERROR_LOG("http-proxy: invalid final status {} from {}:{}", out_status, target.host,
                                  target.port);
            (void)co_await stream->abort();
            co_await send_error(status::bad_gateway, make_error_code(asio::error::invalid_argument));
            co_return;
        }

        res->status(out_status);
        for (const auto &[name, value] : out_headers)
            res->header(name, value);

        // HTTP/1.1 chunked framing: announce the trailer names the backend
        // promised, as Go's ReverseProxy does. HTTP/2/3 do not need the field.
        if (res->version() == Version::Http11) {
            for (auto trailer : head->headers.get_all("trailer"))
                res->header("trailer", std::string{trailer});
        }

        const bool bodyless =
            req->method() == Method::Head || out_status == status::no_content || out_status == status::not_modified;
        if (bodyless) {
            // Nothing follows the head (HEAD, 204/304): no framing, no terminator.
            (void)co_await res->send_bodyless();
            // If a ModifyResponse hook turned a bodied response into a bodyless
            // one, the upstream body is now unread; abort it rather than leave
            // the exchange half-finished.
            if (!head->bodyless)
                (void)co_await stream->abort();
            co_return;
        }
        if (auto ec = co_await res->begin(); ec) {
            (void)co_await stream->abort(); // the frontend went away; stop paying for the upstream
            co_return;
        }

        for (;;) {
            auto chunk = co_await detail::await_cancelable(req, stream->read());
            if (req->cancelled()) {
                (void)co_await stream->abort();
                co_return;
            }
            if (!chunk) {
                // An upstream that stops mid-body must not be reported to the
                // frontend as a complete response: abort instead of finishing
                // cleanly (HTTP/1.x closes, HTTP/2 resets the stream).
                SIMPLE_HTTP_ERROR_LOG("http-proxy: {}:{} failed mid-body ({}), aborting the response", target.host,
                                      target.port, chunk.error().message());
                (void)co_await res->close();
                co_return;
            }
            if (chunk->eof)
                break;
            // No cancellation race here: a cancelled/reset frontend makes the
            // writer itself fail, and each extra wait_cancelled() branch would
            // allocate a channel per body chunk on the hot path.
            if (auto ec = co_await res->write(std::move(chunk->data))) {
                (void)co_await stream->abort();
                co_return;
            }
        }
        // Forward the backend's trailer fields (RFC 9110 §6.5; grpc-status is
        // the canonical user). Whether the frontend can carry them is its
        // engine's decision: chunked HTTP/1.1, HTTP/2 and HTTP/3 do; a
        // close-delimited HTTP/1.0 response or a response to HEAD drops them.
        if (const Headers &trailers = stream->trailers(); !trailers.empty()) {
            (void)co_await res->finish_trailers(trailers);
        } else {
            (void)co_await res->finish();
        }
        co_return;
    }
    co_return;
}

} // namespace simple_http
