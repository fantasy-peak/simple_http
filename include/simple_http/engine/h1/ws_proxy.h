#pragma once

// WsProxyTunnel — byte-level WebSocket reverse-proxy pass-through.
//
// When an Upgrade: websocket request matches a registered proxy route, the H1
// engine does NOT enter the WebSocket message abstraction. Instead it hands the
// connection here: we open a plain TCP connection to the backend, build the
// backend Upgrade request from the inbound fields (same header policy as the
// request-level proxy), wait for and validate the backend's 101, then splice
// the two byte streams verbatim in both directions until either side closes.
//
// Because nothing is decoded, everything passes through untouched: WebSocket
// frame boundaries, fragmentation, client-side masking, control frames and
// permessage-deflate. There is no per-message size cap and no re-framing — this
// is a transparent tunnel, not a message-level proxy.
//
// The handshake half is shared with the HTTP/2 extended-CONNECT path
// (engine/ws_proxy_common.h); only the tunnel loops differ, because one side is
// a Transport and the other is an h2 stream.
//
// Concurrency (model A): the tunnel runs on the connection's own executor. The
// backend socket is created on that same executor, so both halves of the splice
// are single-threaded and need no locks.

#include <algorithm>
#include <array>
#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "../../core/address.h"
#include "../../core/http_status.h" // reason_phrase (the 502 body/status line)
#include "../../core/logging.h"
#include "../../core/types.h"
#include "../../proto/headers.h"
#include "../../proto/ws_frame.h" // ws_accept_key
#include "../../transport/transport.h"
#include "../dispatcher.h"      // WsProxyTarget
#include "../ws_proxy_common.h" // shared handshake / header policy
#include "h1_parser.h"          // ParsedHead

namespace simple_http {

namespace asio = boost::asio;

// Reconstructs the raw HTTP/1.x request head (request line + headers + blank
// line) from a ParsedHead. Kept as the low-level spelling used by the parser
// tests; the live proxy uses build_ws_upgrade_head(), which also applies the
// Go-style forwarding-header policy.
inline std::string rebuild_request_head(const ParsedHead &head, std::string_view target_override = {}) {
    std::string out;
    out.append(head.method_token);
    out.push_back(' ');
    out.append(target_override.empty() ? std::string_view{head.target} : target_override);
    out.push_back(' ');
    out.append(head.version == Version::Http1 ? "HTTP/1.0" : "HTTP/1.1");
    out.append("\r\n");
    for (const auto &[name, value] : head.headers) {
        out.append(name);
        out.append(": ");
        out.append(value);
        out.append("\r\n");
    }
    out.append("\r\n");
    return out;
}

// Answers the client when the tunnel could not be established, then closes the
// client side. Without this the peer sees a bare connection close with no HTTP
// status at all — measured: an upgrade to a route whose backend refused the
// connection (ECONNREFUSED) delivered 0 bytes and an immediate EOF, so a browser
// reports "connection closed before the handshake completed" and nothing ties it
// to the unreachable backend. A backend that is merely unreachable is a 502,
// which is what the HTTP/2 proxy path has always answered.
//
// Called only before any byte of the tunnel has been written to the client, so
// the 502 is the response to its upgrade request and nothing else.
template <typename Transport>
inline asio::awaitable<void> ws_proxy_fail(const std::shared_ptr<Transport> &client, int status) {
    const std::string resp = "HTTP/1.1 " + std::to_string(status) + " " + std::string{reason_phrase(status)} +
                             "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    (void)co_await client->async_write(
        std::span<const std::byte>{reinterpret_cast<const std::byte *>(resp.data()), resp.size()});
    client->close();
    co_return;
}

// Runs a byte-level WebSocket proxy tunnel between the already-upgraded client
// transport and a freshly-opened backend TCP connection. Returns after both
// directions have finished (the caller owns lifetime of the client transport
// and must not close it before this returns).
//
// `initial` holds bytes the H1 engine read past the request head (the start of
// the client->backend WebSocket stream); they are forwarded to the backend
// right after the replayed request head.
template <typename Transport>
inline asio::awaitable<bool>
run_ws_proxy(std::shared_ptr<Transport> client, const ParsedHead &head, std::string initial, WsProxyTarget target,
             std::chrono::steady_clock::duration idle_timeout = std::chrono::seconds(120),
             std::size_t max_header_bytes = 64 * 1024, std::string peer_address = {}, bool client_is_tls = false) {
    using namespace asio::experimental::awaitable_operators;

    auto executor = client->get_executor();
    auto connected = co_await detail::ws_proxy_connect(executor, target);
    if (!connected) {
        const error_code ec = connected.error();
        SIMPLE_HTTP_ERROR_LOG("ws-proxy connect {}:{} failed (client {}): {}", target.host, target.port,
                              address_string(client->peer().address()), ec.message());
        co_await ws_proxy_fail(client, 502);
        co_return false;
    }
    auto backend = std::move(*connected);

    auto close_backend = [backend]() { detail::ws_proxy_close(*backend); };

    // Replay target: rewrite_path replaces the path, but the query string from
    // the inbound target is preserved (session tokens and code-server's
    // reconnectionToken live there). If no rewrite is configured, keep the
    // inbound target as-is.
    const auto qmark = head.target.find('?');
    const std::string_view inbound_path = qmark == std::string_view::npos
                                              ? std::string_view{head.target}
                                              : std::string_view{head.target}.substr(0, qmark);
    // An empty view (target ends in '?') is distinct from no '?' at all; the h1
    // proxy replayed the bare '?' verbatim before this was factored out.
    const std::optional<std::string_view> inbound_query =
        qmark == std::string_view::npos
            ? std::nullopt
            : std::optional<std::string_view>{std::string_view{head.target}.substr(qmark + 1)};
    auto replay = detail::ws_proxy_replay_target(target.rewrite_path, inbound_path, inbound_query);
    if (!replay) {
        SIMPLE_HTTP_ERROR_LOG("ws-proxy: refusing a request target with CR/LF/NUL (client {})",
                              address_string(client->peer().address()));
        close_backend();
        co_await ws_proxy_fail(client, status::bad_request);
        co_return false;
    }
    std::string replay_target = std::move(*replay);

    const auto inbound_host = head.headers.get("host");
    std::string key;
    if (auto inbound_key = head.headers.get("sec-websocket-key"); inbound_key && !inbound_key->empty()) {
        key.assign(*inbound_key);
    } else {
        key = detail::make_ws_client_key();
    }

    std::string preamble =
        detail::build_ws_upgrade_head(head.headers, inbound_host ? *inbound_host : std::string_view{}, replay_target,
                                      target, peer_address, client_is_tls, key);
    // Bytes the client already sent after its handshake travel behind the
    // upstream request, exactly as before.
    preamble.append(initial);
    {
        auto [ec, n] = co_await asio::async_write(*backend, asio::buffer(preamble.data(), preamble.size()),
                                                  asio::as_tuple(asio::use_awaitable));
        (void)n;
        if (ec) {
            SIMPLE_HTTP_ERROR_LOG("ws-proxy write preamble failed (client {}): {}",
                                  address_string(client->peer().address()), ec.message());
            close_backend();
            co_await ws_proxy_fail(client, 502);
            co_return false;
        }
    }

    auto reply = co_await detail::ws_proxy_read_reply(*backend, max_header_bytes, target.connect_timeout);
    if (!reply) {
        SIMPLE_HTTP_ERROR_LOG("ws-proxy backend handshake failed (client {}): {}",
                              address_string(client->peer().address()), reply.error().message());
        close_backend();
        co_await ws_proxy_fail(client, 502);
        co_return false;
    }

    if (reply->head.status != 101) {
        // The backend refused the upgrade. Relay its actual response (head plus
        // whatever body bytes already arrived) instead of inventing a 502; the
        // h1 client sees the same 401/403/302 a direct connection would have
        // produced. The body is framed like any HTTP/1.1 response: Content-Length,
        // chunked, or close-delimited — the relay must not stop at the first
        // `initial` bytes.
        std::string out = reply->raw;
        out.append(reply->initial);
        co_await detail::ws_proxy_relay_refusal_body(*backend, out, reply->initial, reply->head.headers,
                                                     target.connect_timeout);
        (void)co_await client->async_write(
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(out.data()), out.size()});
        close_backend();
        client->close();
        co_return false;
    }

    if (!detail::ws_validate_101(reply->head, ws_accept_key(key))) {
        SIMPLE_HTTP_ERROR_LOG("ws-proxy backend answered 101 with a mismatched Upgrade/accept (client {})",
                              address_string(client->peer().address()));
        close_backend();
        co_await ws_proxy_fail(client, 502);
        co_return false;
    }

    // Relay the backend's 101 head verbatim (Sec-WebSocket-Accept, negotiated
    // extensions/subprotocol, any Set-Cookie) and any tunnel bytes that arrived
    // in the same read.
    std::string opening = reply->raw;
    opening.append(reply->initial);
    {
        auto [ec, n] = co_await client->async_write(
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(opening.data()), opening.size()});
        (void)n;
        if (ec) {
            close_backend();
            co_return false;
        }
    }

    // Idle deadline, refreshed by every successful read or write in either
    // direction. The tunnel has no framing of its own, so without this a peer
    // that vanishes without a FIN/RST (NAT timeout, power loss, pulled cable)
    // leaves both directions blocked forever - holding two sockets, the 16 KiB
    // buffers and this coroutine. A long one-way transfer keeps refreshing it, so
    // only a genuinely idle tunnel is reaped.
    auto deadline = std::chrono::steady_clock::now() + idle_timeout;

    auto teardown = [client, backend]() {
        client->close();
        detail::ws_proxy_close(*backend);
    };

    // client -> backend: raw bytes off the transport, straight to the backend.
    auto client_to_backend = [&]() -> asio::awaitable<void> {
        std::array<std::byte, 16384> buf; // no init: read_some fills [0,n)
        for (;;) {
            auto [rec2, n] = co_await client->async_read_some(std::span<std::byte>{buf});
            if (rec2 || n == 0) {
                // Half-close: the client will send no more, but the backend may
                // still have its Close frame to deliver.
                error_code sec;
                backend->shutdown(asio::ip::tcp::socket::shutdown_send, sec);
                break;
            }
            deadline = std::chrono::steady_clock::now() + idle_timeout;
            auto [wec, w] =
                co_await asio::async_write(*backend, asio::buffer(buf.data(), n), asio::as_tuple(asio::use_awaitable));
            (void)w;
            if (wec)
                break;
            deadline = std::chrono::steady_clock::now() + idle_timeout;
        }
        co_return;
    };

    // backend -> client: raw bytes off the backend, straight to the transport.
    auto backend_to_client = [&]() -> asio::awaitable<void> {
        std::array<std::byte, 16384> buf; // no init: read_some fills [0,n)
        for (;;) {
            auto [rec2, n] = co_await backend->async_read_some(asio::buffer(buf.data(), buf.size()),
                                                               asio::as_tuple(asio::use_awaitable));
            if (rec2 || n == 0) {
                // Backend is done: closing the client also unblocks the other
                // direction (there is nothing left for it to deliver to).
                client->close();
                break;
            }
            deadline = std::chrono::steady_clock::now() + idle_timeout;
            auto [wec, w] = co_await client->async_write(std::span<const std::byte>{buf.data(), n});
            (void)w;
            if (wec)
                break;
            deadline = std::chrono::steady_clock::now() + idle_timeout;
        }
        co_return;
    };

    // Wait for both directions (Go's io.Copy pair), with the idle watchdog as
    // the only early exit. When one direction half-closes, it shuts down only
    // the opposite write side and lets the other finish.
    if (idle_timeout.count() > 0) {
        auto outcome =
            co_await ((client_to_backend() && backend_to_client()) || detail::ws_tunnel_idle_guard(executor, deadline));
        if (const auto *timed_out = std::get_if<bool>(&outcome); timed_out && *timed_out) {
            SIMPLE_HTTP_ERROR_LOG("ws-proxy tunnel idle for {}s (client {}); closing",
                                  std::chrono::duration_cast<std::chrono::seconds>(idle_timeout).count(),
                                  address_string(client->peer().address()));
        }
    } else {
        co_await (client_to_backend() && backend_to_client());
    }
    teardown();
    co_return true;
}

} // namespace simple_http
