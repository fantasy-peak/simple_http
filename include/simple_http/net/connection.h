#pragma once

// Connection protocol detection.
//
// Given a ready transport, decide which engine to run:
//   * TLS       -> ALPN result selects h2 or http/1.1.
//   * plaintext -> peek the first bytes: the HTTP/2 connection preface means
//                  prior-knowledge h2; otherwise HTTP/1.x (the HTTP/1.x engine
//                  additionally handles an Upgrade: h2c to switch to h2c).
//
// These are free coroutine templates parameterized on the transport, so no
// concrete socket type leaks into the detection logic.

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <boost/asio.hpp>

#include "../core/limits.h"
#include "../engine/dispatcher.h"
#include "../engine/h1/h1_engine.h"
#include "../engine/h2/h2_engine.h"

namespace simple_http {

namespace asio = boost::asio;
namespace beast = boost::beast;

// The HTTP/2 connection preface prefix (enough to identify prior-knowledge h2).
inline constexpr std::string_view h2_preface_prefix = "PRI * HTTP/2.0";

// Serve a plaintext transport: detect prior-knowledge h2, else HTTP/1.x.
template <typename Transport>
inline asio::awaitable<void> serve_plaintext(std::shared_ptr<Transport> transport, Dispatcher dispatch,
                                             WsLookup ws_lookup = {}, EngineLimits limits = {},
                                             WsProxyLookup ws_proxy_lookup = {}) {
    std::array<std::byte, 4096> buf{};
    auto [ec, n] = co_await transport->async_read_some(std::span<std::byte>{buf});
    if (ec) {
        transport->close();
        co_return;
    }
    std::string_view head{reinterpret_cast<const char*>(buf.data()), n};

    if (head.starts_with(h2_preface_prefix)) {
        // HTTP/2 prior-knowledge: replay the bytes we consumed into the engine.
        // The engine is shared so in-flight handlers keep it alive.
        auto engine = std::make_shared<Http2Engine<Transport>>(transport, limits);
        co_await engine->run(std::move(dispatch), std::string{head});
        co_return;
    }

    // HTTP/1.x: hand the already-read bytes to the engine via its initial buffer.
    // A registered WebSocket route (ws_lookup) enables the Upgrade: websocket
    // handshake inside the h1 engine.
    Http1Engine<Transport> engine{transport, limits};
    co_await engine.run(dispatch, std::string{head}, std::move(ws_lookup), std::move(ws_proxy_lookup));
    co_return;
}

// Serve a TLS transport: handshake, then ALPN selects the engine.
template <typename TlsTransportT>
inline asio::awaitable<void> serve_tls(std::shared_ptr<TlsTransportT> transport, Dispatcher dispatch,
                                       WsLookup ws_lookup = {}, EngineLimits limits = {},
                                       WsProxyLookup ws_proxy_lookup = {}) {
    if (auto ec = co_await transport->handshake(); ec) {
        transport->close();
        co_return;
    }
    if (transport->alpn_selected() == "h2") {
        auto engine = std::make_shared<Http2Engine<TlsTransportT>>(transport, limits);
        co_await engine->run(std::move(dispatch));
    } else {
        Http1Engine<TlsTransportT> engine{transport, limits};
        co_await engine.run(dispatch, {}, std::move(ws_lookup), std::move(ws_proxy_lookup));
    }
    co_return;
}

}  // namespace simple_http
