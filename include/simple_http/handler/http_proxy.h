#pragma once

// HTTP reverse proxy — request-level, standard forward-proxy behaviour, driven
// by the client layer.
//
// Unlike the byte-level WebSocket tunnel (engine/h1/ws_proxy.h), this operates on
// the parsed Request/Response abstraction, one request at a time, and the
// upstream leg is an ordinary client exchange: the connection comes from the
// client layer's pool, the request is re-framed there (chunked body on HTTP/1.1,
// DATA frames on HTTP/2), and the response arrives as a head plus body chunks. So
// the backend may be plaintext or TLS, HTTP/1.1 or HTTP/2 — see HttpProxyTarget.
//
// Standard reverse-proxy header handling (RFC 7230 §5.7.1 / §6.1):
//   * append the client address to X-Forwarded-For, set X-Forwarded-Proto and
//     X-Forwarded-Host;
//   * the Host header (and, on HTTP/2, :authority) is the backend's — the client
//     layer writes the origin's authority, so a `host` field from the frontend is
//     dropped rather than forwarded;
//   * strip hop-by-hop headers (Connection, Keep-Alive, Proxy-*, TE, Trailer,
//     Transfer-Encoding, Upgrade) plus any field named in the Connection header,
//     on both the request and the response.
//
// Concurrency (model A): the handler runs on the connection executor, and the
// client session it takes is bound to that same executor, so all I/O for one
// request is single-threaded. Idle upstream connections are pooled and reused by
// the client — a proxy that reconnects per request falls over when the backend
// sits behind a tunnel and the client issues requests in bursts.

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio.hpp>

#include "../client/http_client.h"
#include "../core/logging.h"
#include "../core/types.h"
#include "../engine/dispatcher.h"  // HttpProxyTarget
#include "../proto/headers.h"
#include "../proto/request.h"
#include "../proto/response.h"

namespace simple_http {

namespace asio = boost::asio;

namespace detail {

// Hop-by-hop headers that must not be forwarded (RFC 7230 §6.1). Field names are
// compared lowercased (Headers stores them lowercased already).
inline bool is_hop_by_hop(std::string_view name) {
    static constexpr std::string_view kHop[] = {
        "connection", "keep-alive",         "proxy-authenticate", "proxy-authorization",
        "te",         "trailer",            "transfer-encoding",  "upgrade",
    };
    for (auto h : kHop) {
        if (name == h) return true;
    }
    return false;
}

inline std::string to_lower(std::string_view s) {
    std::string out{s};
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Splits a comma-separated Connection header value into lowercased tokens, which
// name additional per-connection headers that must also be stripped.
inline std::vector<std::string> connection_tokens(const Headers& headers) {
    std::vector<std::string> tokens;
    auto conn = headers.get("connection");
    if (!conn) return tokens;
    std::string_view v{*conn};
    std::size_t pos = 0;
    while (pos < v.size()) {
        std::size_t comma = v.find(',', pos);
        std::string_view tok = v.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
        // trim OWS
        while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) tok.remove_prefix(1);
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) tok.remove_suffix(1);
        if (!tok.empty()) tokens.push_back(to_lower(tok));
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    return tokens;
}

inline bool contains_token(const std::vector<std::string>& tokens, std::string_view name) {
    for (const auto& t : tokens) {
        if (t == name) return true;
    }
    return false;
}

}  // namespace detail

// Runs one request-level HTTP reverse-proxy exchange: forwards `req` to the
// backend `target` through `client` and streams the response back through `res`.
// On any upstream failure a 502 Bad Gateway is sent while the response has not
// started yet; a failure once the body is flowing aborts the frontend response
// instead (a truncated body must never look complete).
inline asio::awaitable<void> run_http_proxy(std::shared_ptr<Request> req,
                                            std::shared_ptr<Response> res,
                                            bool client_is_tls,
                                            HttpProxyTarget target,
                                            HttpClient& client) {
    auto fail_502 = [&res]() -> asio::awaitable<void> {
        if (co_await res->connected()) {
            (void)co_await res->status(502).content_type("text/plain").send("Bad Gateway");
        }
        co_return;
    };

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
    // header we synthesize. The engines reject such bytes at parse time (h1 cannot
    // produce them, h2 validates them), but a proxy must not rely on its caller.
    if (contains_ctl(req->method_token()) || contains_ctl(forwarded_target)) {
        SIMPLE_HTTP_ERROR_LOG("http-proxy: refusing request line with CR/LF/NUL");
        co_await fail_502();
        co_return;
    }

    RequestSpec spec;
    spec.method = req->method();
    spec.target = std::move(forwarded_target);

    // Copy the frontend's fields, minus what the client layer owns (Host,
    // Content-Length, the connection's own headers) and minus anything the
    // Connection header lists.
    auto req_conn_tokens = detail::connection_tokens(req->headers());
    std::string xff;
    bool saw_xff = false;
    for (const auto& [name, value] : req->headers()) {
        if (detail::is_hop_by_hop(name) || detail::contains_token(req_conn_tokens, name)) continue;
        if (name == "host" || name == "expect" || name == "content-length") continue;
        if (name == "x-forwarded-for") {
            saw_xff = true;
            xff = value;  // will append the client address below
            continue;
        }
        if (contains_ctl(name) || contains_ctl(value)) {
            SIMPLE_HTTP_ERROR_LOG("http-proxy: refusing header with CR/LF/NUL: {}", name);
            co_await fail_502();
            co_return;
        }
        spec.headers.add_lower(name, value);
    }
    spec.headers.add_lower("x-forwarded-for", saw_xff && !xff.empty() ? xff + ", " + req->peer_address()
                                                                      : req->peer_address());
    spec.headers.add_lower("x-forwarded-proto", client_is_tls ? "https" : "http");
    if (auto host = req->header("host")) spec.headers.add_lower("x-forwarded-host", std::string{*host});

    // Does the request carry a body? Header hints work for HTTP/1.x but NOT for
    // HTTP/2, which frames the body with END_STREAM and carries neither header, so
    // the body is probed: read the first frame now and forward it once the
    // upstream exchange is open. A client that sent `Expect: 100-continue` waits
    // for a go-ahead before sending its body: answer it here (RFC 9110 §10.1.1
    // allows an intermediary to), otherwise this read would block until the client
    // gives up.
    if (auto expect = req->header("expect");
        expect && detail::to_lower(*expect).find("100-continue") != std::string::npos) {
        (void)co_await res->writer().send_continue();
    }
    std::string first_chunk;
    bool has_body = false;
    {
        auto frame = co_await req->body().read();
        if (frame && !frame->eof) {
            has_body = true;
            first_chunk = std::move(frame->data);
        }
        // else: no body (immediate EOF), or a read error — forward with no body.
    }
    spec.stream_body = has_body;

    // --- the backend this target names ---
    ClientTarget upstream;
    upstream.host = target.host;
    upstream.port = target.port;
    upstream.use_tls = target.tls;
    if (target.tls) {
        upstream.version = HttpVersionPolicy::Auto;  // ALPN: h2 when the backend offers it
    } else if (target.h2c) {
        upstream.version = HttpVersionPolicy::Http2;
        upstream.h2c = H2cMode::Upgrade;
    } else {
        upstream.version = HttpVersionPolicy::Http11;  // the historical default: plain HTTP/1.1
        upstream.h2c = H2cMode::Off;
    }

    // One retry, under the same rule the client's convenience layer uses: only a
    // connection that came from the pool (so it sat idle and the peer may have
    // closed it unnoticed) failing at the transport level, and only when the
    // request can be sent again — a body already taken from the frontend cannot.
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto opened = co_await client.open_stream(upstream, spec);
        if (!opened) {
            SIMPLE_HTTP_ERROR_LOG("http-proxy: no upstream connection to {}:{} ({})",
                                  target.host,
                                  target.port,
                                  opened.error().message());
            co_await fail_502();
            co_return;
        }
        auto& stream = opened->stream;
        const bool may_retry = attempt == 0 && opened->pooled && !has_body;

        // --- request body ---
        if (has_body) {
            bool body_sent = true;
            if (!first_chunk.empty()) {
                if (auto ec = co_await stream->write(first_chunk); ec) body_sent = false;
            }
            for (; body_sent;) {
                auto frame = co_await req->body().read();
                if (!frame || frame->eof) break;  // a body read error just ends what we forward
                if (frame->data.empty()) continue;
                if (auto ec = co_await stream->write(std::move(frame->data)); ec) {
                    SIMPLE_HTTP_ERROR_LOG("http-proxy: upstream {}:{} rejected the body ({})",
                                          target.host,
                                          target.port,
                                          ec.message());
                    body_sent = false;
                    break;
                }
            }
            if (body_sent) {
                if (auto ec = co_await stream->finish(std::string{}); ec) body_sent = false;
            }
            if (!body_sent) {
                // The request is half-sent: it cannot be replayed, and the
                // frontend has not seen a response yet.
                (void)co_await stream->cancel();
                co_await fail_502();
                co_return;
            }
        }

        // --- the backend's response head ---
        auto head = co_await stream->read_head();
        if (!head) {
            if (may_retry && transport_failure(head.error())) {
                SIMPLE_HTTP_ERROR_LOG("http-proxy: {}:{} dropped a pooled connection ({}), retrying",
                                      target.host,
                                      target.port,
                                      head.error().message());
                (void)co_await stream->cancel();
                continue;
            }
            SIMPLE_HTTP_ERROR_LOG("http-proxy: no response head from {}:{} ({})",
                                  target.host,
                                  target.port,
                                  head.error().message());
            (void)co_await stream->cancel();
            co_await fail_502();
            co_return;
        }

        // --- stream it back through the version-agnostic Response ---
        res->status(head->status);
        auto resp_conn_tokens = detail::connection_tokens(head->headers);
        for (const auto& [name, value] : head->headers) {
            if (detail::is_hop_by_hop(name) || detail::contains_token(resp_conn_tokens, name)) continue;
            // We stream the body, so the upstream Content-Length is replaced by our
            // own framing — except for HEAD, whose response carries the headers a
            // GET would have produced and no body at all.
            if (name == "content-length" && req->method() != Method::Head) continue;
            res->header(name, value);
        }

        if (head->bodyless) {
            // Nothing follows the head (HEAD, 204/304): no framing, no terminator.
            (void)co_await res->send_bodyless();
            co_return;
        }
        if (auto ec = co_await res->begin(); ec) {
            (void)co_await stream->cancel();  // the frontend went away; stop paying for the upstream
            co_return;
        }

        for (;;) {
            auto chunk = co_await stream->read();
            if (!chunk) {
                // An upstream that stops mid-body must not be reported to the
                // frontend as a complete response: abort instead of finishing
                // cleanly (HTTP/1.x closes, HTTP/2 resets the stream).
                SIMPLE_HTTP_ERROR_LOG("http-proxy: {}:{} failed mid-body ({}), aborting the response",
                                      target.host,
                                      target.port,
                                      chunk.error().message());
                (void)co_await res->close();
                co_return;
            }
            if (chunk->eof) break;
            if (auto ec = co_await res->write(std::move(chunk->data)); ec) {
                (void)co_await stream->cancel();
                co_return;
            }
        }
        (void)co_await res->finish();
        co_return;
    }
    co_return;
}

}  // namespace simple_http
