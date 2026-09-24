#pragma once

// WsProxyTunnel — byte-level WebSocket reverse-proxy pass-through.
//
// When an Upgrade: websocket request matches a registered proxy route, the H1
// engine does NOT enter the WebSocket message abstraction. Instead it hands the
// connection here: we open a plain TCP connection to the backend, replay the
// client's *raw* upgrade request onto it (reconstructed request line + headers,
// plus any bytes already read past the head), and then splice the two byte
// streams verbatim in both directions until either side closes.
//
// Because nothing is decoded, everything passes through untouched: the 101
// handshake response the backend produces, WebSocket frame boundaries,
// fragmentation, client-side masking, and control frames (Ping/Pong/Close).
// There is no per-message size cap and no re-framing — this is a transparent
// tunnel, not a message-level proxy.
//
// Concurrency (model A): the tunnel runs on the connection's own executor. The
// backend socket is created on that same executor, so both halves of the splice
// are single-threaded and need no locks. The two copy directions run
// concurrently via awaitable_operators (||); when either finishes (EOF/error)
// both sockets are shut down, which unblocks the other direction.

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

#include "../../core/logging.h"
#include "../../core/types.h"
#include "../../proto/headers.h"
#include "../../transport/transport.h"
#include "../dispatcher.h"  // WsProxyTarget
#include "h1_parser.h"      // ParsedHead

namespace simple_http {

namespace asio = boost::asio;

// Reconstructs the raw HTTP/1.x request head (request line + headers + blank
// line) from a ParsedHead, so it can be replayed to the backend verbatim. Header
// field names were lowercased by the parser; that is wire-legal and preserves
// the WebSocket upgrade semantics.
inline std::string rebuild_request_head(const ParsedHead& head, std::string_view target_override = {}) {
    std::string out;
    out.append(head.method_token);
    out.push_back(' ');
    out.append(target_override.empty() ? std::string_view{head.target} : target_override);
    out.push_back(' ');
    out.append(head.version == Version::Http1 ? "HTTP/1.0" : "HTTP/1.1");
    out.append("\r\n");
    for (const auto& [name, value] : head.headers) {
        out.append(name);
        out.append(": ");
        out.append(value);
        out.append("\r\n");
    }
    out.append("\r\n");
    return out;
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
inline asio::awaitable<bool> run_ws_proxy(std::shared_ptr<Transport> client, const ParsedHead& head,
                                          std::string initial, WsProxyTarget target,
                                          std::chrono::steady_clock::duration idle_timeout =
                                              std::chrono::seconds(120)) {
    using namespace asio::experimental::awaitable_operators;

    auto executor = client->get_executor();
    auto backend = std::make_shared<asio::ip::tcp::socket>(executor);

    // Resolve + connect the backend on this connection's executor.
    asio::ip::tcp::resolver resolver{executor};
    auto [rec, endpoints] = co_await resolver.async_resolve(
        target.host, std::to_string(target.port), asio::as_tuple(asio::use_awaitable));
    if (rec) {
        SIMPLE_HTTP_ERROR_LOG("ws-proxy resolve {}:{} failed: {}", target.host, target.port, rec.message());
        co_return false;
    }
    auto [cec, _] = co_await asio::async_connect(*backend, endpoints, asio::as_tuple(asio::use_awaitable));
    if (cec) {
        SIMPLE_HTTP_ERROR_LOG("ws-proxy connect {}:{} failed: {}", target.host, target.port, cec.message());
        co_return false;
    }

    // Replay the client's raw upgrade request (rebuilt head + any buffered body
    // bytes) to the backend. The backend answers with its own 101, which we
    // splice straight back to the client below.
    std::string preamble = rebuild_request_head(head, target.rewrite_path);
    preamble.append(initial);
    {
        // Composed async_write: writes the whole preamble or returns an error.
        auto [ec, n] = co_await asio::async_write(
            *backend, asio::buffer(preamble.data(), preamble.size()), asio::as_tuple(asio::use_awaitable));
        (void)n;
        if (ec) {
            SIMPLE_HTTP_ERROR_LOG("ws-proxy write preamble failed: {}", ec.message());
            error_code sec;
            backend->shutdown(asio::ip::tcp::socket::shutdown_both, sec);
            backend->close(sec);
            co_return false;
        }
    }

    // Idle deadline, refreshed by every successful read or write in either
    // direction. The tunnel has no framing of its own, so without this a peer that
    // vanishes without a FIN/RST (NAT timeout, power loss, pulled cable) leaves both
    // directions blocked forever - holding two sockets, the 16 KiB buffers and this
    // coroutine. A long one-way transfer keeps refreshing it, so only a genuinely
    // idle tunnel is reaped.
    auto deadline = std::chrono::steady_clock::now() + idle_timeout;

    // Shut both sockets down so a finished direction unblocks the other.
    auto teardown = [client, backend]() {
        client->close();
        error_code sec;
        backend->shutdown(asio::ip::tcp::socket::shutdown_both, sec);
        backend->close(sec);
    };

    // client -> backend: raw bytes off the transport, straight to the backend.
    auto client_to_backend = [&]() -> asio::awaitable<void> {
        std::array<std::byte, 16384> buf{};
        for (;;) {
            auto [rec2, n] = co_await client->async_read_some(std::span<std::byte>{buf});
            if (rec2 || n == 0) break;
            deadline = std::chrono::steady_clock::now() + idle_timeout;
            // Composed async_write: writes all n bytes or errors, no inner loop.
            auto [wec, w] =
                co_await asio::async_write(*backend, asio::buffer(buf.data(), n), asio::as_tuple(asio::use_awaitable));
            (void)w;
            if (wec) break;
            deadline = std::chrono::steady_clock::now() + idle_timeout;
        }
        co_return;
    };

    // backend -> client: raw bytes off the backend, straight to the transport.
    auto backend_to_client = [&]() -> asio::awaitable<void> {
        std::array<std::byte, 16384> buf{};
        for (;;) {
            auto [rec2, n] = co_await backend->async_read_some(
                asio::buffer(buf.data(), buf.size()), asio::as_tuple(asio::use_awaitable));
            if (rec2 || n == 0) break;
            deadline = std::chrono::steady_clock::now() + idle_timeout;
            // Transport::async_write is composed: writes all n bytes or errors.
            auto [wec, w] = co_await client->async_write(std::span<const std::byte>{buf.data(), n});
            (void)w;
            if (wec) break;
            deadline = std::chrono::steady_clock::now() + idle_timeout;
        }
        co_return;
    };

    // Reaps a tunnel that has seen no traffic in either direction for idle_timeout.
    auto idle_check = [&]() -> asio::awaitable<void> {
        if (idle_timeout.count() <= 0) {
            co_return;  // disabled: do not take part in the race
        }
        for (;;) {
            if (std::chrono::steady_clock::now() >= deadline) {
                SIMPLE_HTTP_ERROR_LOG("ws-proxy tunnel idle for {}s; closing",
                                      std::chrono::duration_cast<std::chrono::seconds>(idle_timeout).count());
                co_return;
            }
            asio::steady_timer timer{executor};
            timer.expires_at(deadline);
            auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
            if (ec) co_return;  // cancelled: the tunnel already finished
        }
    };

    // Run both directions plus the idle check concurrently; the first to finish
    // (EOF/error/idle) tears both sockets down, which unblocks the pending read in
    // the other direction so it also completes.
    co_await (client_to_backend() || backend_to_client() || idle_check());
    teardown();
    co_return true;
}

}  // namespace simple_http
