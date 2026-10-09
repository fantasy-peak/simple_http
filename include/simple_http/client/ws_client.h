#pragma once

// Client-side WebSocket (RFC 6455): upgrade a dialed connection to WebSocket
// and hand it to the same WebSocket handle the server gives its handlers.
//
// The client side of the server's ws_route: the server receives the upgrade
// request and answers 101; here we *send* the upgrade request and verify the
// 101 + Sec-WebSocket-Accept. After the handshake the connection is a WebSocket
// stream, and the frame encoding/decoding, the write pump, ping/pong and close
// handling already exist in proto/websocket.h (WsBackendImpl) — reused as-is,
// since WsBackend is transport-agnostic.
//
// API (all on http::Client):
//   auto ws = co_await client.open_websocket("ws://host/chat", {.headers = ...});
//   ws->write_text("hi"); auto msg = co_await ws->read(); ...
//
// The resulting WebSocket behaves exactly like a server-side one: read() pulls
// whole messages (reassembling fragments, answering ping/close), write_text/
// write_binary serialize through one write pump, close() sends a Close frame.

#include <array>
#include <boost/asio.hpp>
#include <cctype> // std::tolower (accept-key header scan)
#include <cstddef>
#include <cstring>
#include <expected>
#include <memory>
#include <random> // std::random_device (Sec-WebSocket-Key nonce)
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "../core/base64.h"  // ws key generation
#include "../core/logging.h" // SIMPLE_HTTP_ERROR_LOG
#include "../core/types.h"   // contains_ctl, client_errc
#include "../core/version.h" // client_version
#include "../proto/headers.h"
#include "../proto/websocket.h" // WsBackendImpl / WebSocket / ws_accept_key
#include "client_config.h"
#include "http_client.h" // detail::ClientEngine::dial_transport

namespace simple_http {

namespace asio = boost::asio;

// Per-connection knobs for the WebSocket client. 0 keeps the ClientConfig
// default.
struct WebSocketSpec {
    Headers headers; // extra request headers (e.g. auth)
    // Origin to send. Empty = not sent (a browser would send one; a non-browser
    // client need not).
    std::string origin;
    // Max frame payload accepted from the peer. 0 = 16 MiB.
    std::size_t max_payload{0};
    // Inbound read-ahead cap in bytes (see WsBackendImpl). 0 = library default
    // (4 MiB). Bounds the memory a server can make the client buffer by sending
    // faster than the application reads.
    std::size_t read_ahead_bytes{0};
};

namespace detail {

// Generates the Sec-WebSocket-Key: base64 of 16 random bytes (RFC 6455
// §4.1: the key is a base64-encoded 16-byte random nonce). The bytes come from
// std::random_device (the OS entropy source) rather than std::rand(), which is
// unseeded (a fixed sequence per process), not thread-safe, and trivially
// predictable — a poor nonce even for a value that is not a secret.
inline std::string ws_client_key() {
    unsigned char random[16];
    std::random_device rd;
    for (auto &b : random)
        b = static_cast<unsigned char>(rd());
    return base64_encode(std::string_view{reinterpret_cast<const char *>(random), sizeof(random)});
}

// Performs the WebSocket handshake on an established transport: sends the
// upgrade request, reads the response head, verifies the 101 and the accept
// key. On success returns a WebSocket over the transport (pump already
// started). `path` is the origin-form target; extra headers arrived in `spec`.
template <typename Transport>
asio::awaitable<std::expected<std::shared_ptr<WebSocket>, error_code>>
ws_upgrade(std::shared_ptr<Transport> transport, std::string authority, std::string path, const WebSocketSpec &spec,
           std::chrono::milliseconds idle_timeout) {
    using asio::as_tuple;
    using asio::use_awaitable;

    const std::string key = ws_client_key();
    std::string request;
    request += "GET " + (path.empty() ? std::string{"/"} : path) + " HTTP/1.1\r\n";
    request += "host: " + authority + "\r\n";
    request += "upgrade: websocket\r\n";
    request += "connection: Upgrade\r\n";
    request += "sec-websocket-key: " + key + "\r\n";
    request += "sec-websocket-version: 13\r\n";
    if (!spec.origin.empty()) {
        request += "origin: " + spec.origin + "\r\n";
    }
    for (const auto &[name, value] : spec.headers) {
        if (contains_ctl(name) || contains_ctl(value)) {
            transport->close();
            co_return std::unexpected{make_error_code(asio::error::invalid_argument)};
        }
        request += name;
        request += ": ";
        request += value;
        request += "\r\n";
    }
    request += "user-agent: ";
    request += client_version;
    request += "\r\n\r\n";

    if (auto [ec, n] =
            co_await transport->async_write(std::as_bytes(std::span<const char>{request.data(), request.size()}));
        ec) {
        (void)n;
        transport->close();
        co_return std::unexpected{ec};
    }

    // Read the response head: up to the \r\n\r\n, bounded (a hostile peer could
    // stream headers forever).
    std::string response;
    std::array<std::byte, 512> buf{};
    std::size_t const kMaxHead = 16384;
    while (response.find("\r\n\r\n") == std::string::npos) {
        if (response.size() > kMaxHead) {
            transport->close();
            co_return std::unexpected{make_error_code(client_errc::header_too_large)};
        }
        auto [ec, n] = co_await transport->async_read_some(std::span<std::byte>{buf});
        if (ec) {
            transport->close();
            co_return std::unexpected{ec};
        }
        response.append(reinterpret_cast<const char *>(buf.data()), n);
    }
    const std::size_t head_end = response.find("\r\n\r\n") + 4;
    const std::string head = response.substr(0, head_end);
    const std::string_view extra = std::string_view{response}.substr(head_end); // first frame bytes

    // Status line: "HTTP/1.1 101 ...".
    if (!head.starts_with("HTTP/1.1 101")) {
        SIMPLE_HTTP_ERROR_LOG("ws client: upgrade to {} refused (not 101)", authority);
        transport->close();
        co_return std::unexpected{make_error_code(client_errc::protocol_error)};
    }
    // The Subprotocol / extension offer is the server's choice; nothing to
    // validate there. The accept key is the one mandatory check.
    std::string_view accept;
    for (std::size_t pos = 0;;) {
        const std::size_t nl = head.find("\r\n", pos);
        if (nl == std::string::npos || nl >= head_end)
            break;
        const std::string_view line = std::string_view{head}.substr(pos, nl - pos);
        if (line.empty())
            break;
        const std::size_t colon = line.find(':');
        if (colon != std::string_view::npos) {
            std::string_view name = line.substr(0, colon);
            // Compare case-insensitively, like the server's own header parser.
            std::string lower;
            lower.reserve(name.size());
            for (char c : name)
                lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            if (lower == "sec-websocket-accept") {
                std::string_view value = line.substr(colon + 1);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                    value.remove_prefix(1);
                accept = value;
            }
        }
        pos = nl + 2;
    }
    if (accept != ws_accept_key(key)) {
        transport->close();
        co_return std::unexpected{make_error_code(client_errc::protocol_error)};
    }

    auto backend = std::make_shared<WsBackendImpl<Transport>>(
        transport, spec.max_payload != 0 ? spec.max_payload : 16u * 1024 * 1024, idle_timeout,
        /*expect_masked=*/false, // server frames are unmasked
        spec.read_ahead_bytes);
    backend->feed(extra); // bytes already read past the head: the peer's first frame

    auto ws = std::make_shared<WebSocket>(std::move(backend));
    asio::co_spawn(transport->get_executor(), ws->run_writer(), asio::detached);
    co_return ws;
}

} // namespace detail

} // namespace simple_http