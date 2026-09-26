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

// The HTTP/2 connection preface prefix (enough to identify prior-knowledge h2).
inline constexpr std::string_view h2_preface_prefix = "PRI * HTTP/2.0";

// Which protocols a plaintext listener serves. A TLS listener negotiates this
// with ALPN instead and ignores it.
//
// Both is the sniffing default; the two single-protocol values exist because
// sniffing cannot answer a peer that opens with neither. A malformed HTTP/2
// preface and an HTTP/1.x request line are the same bytes until you have decided
// which protocol you are speaking, so one of the two readings is always wrong —
// and the two conformance suites want opposite ones.
enum class PlaintextProtocols {
    Http1,  // parse every connection as HTTP/1.x
    Http2,  // require the HTTP/2 connection preface on every connection
    Both,   // sniff (the default)
};

// Serve a plaintext transport: detect prior-knowledge h2, else HTTP/1.x.
//
// HTTP/2 prior knowledge is recognised by the start of the client preface, so we
// read exactly that many bytes and compare. Reading exactly (rather than once)
// matters: a single async_read_some may return fewer bytes - a TCP segment can
// split the preface - and a partial match must not be read as "not h2". A
// well-formed HTTP/1.x request line is longer than the prefix, so this never
// delays HTTP/1.x handling. The read is bounded by the idle timeout, otherwise a
// client that sends a few bytes and stalls would hold the connection open (the
// engines' own watchdogs only start once a protocol has been chosen).
template <typename Transport>
inline asio::awaitable<void> serve_plaintext(std::shared_ptr<Transport> transport, Dispatcher dispatch,
                                             WsLookup ws_lookup = {}, EngineLimits limits = {},
                                             WsProxyLookup ws_proxy_lookup = {},
                                             PlaintextProtocols protocols = PlaintextProtocols::Both) {
    using namespace asio::experimental::awaitable_operators;

    // A listener that serves one protocol does not sniff — that is the whole
    // point of declaring it (see PlaintextProtocols).
    if (protocols == PlaintextProtocols::Http2) {
        // No pre-read: the engine reads and verifies all 24 octets of the preface
        // itself (RFC 9113 §3.4), so a peer that opened with anything else gets the
        // connection error it earned rather than being answered as HTTP/1.x.
        auto engine = std::make_shared<Http2Engine<Transport>>(transport, limits);
        co_await engine->run(std::move(dispatch));
        co_return;
    }
    if (protocols == PlaintextProtocols::Http1) {
        Http1Engine<Transport> engine{transport, limits};
        co_await engine.run(dispatch, {}, std::move(ws_lookup), std::move(ws_proxy_lookup));
        co_return;
    }

    std::array<std::byte, h2_preface_prefix.size()> head{};
    std::size_t n = 0;

    auto read_head = [&]() -> asio::awaitable<void> {
        auto [ec, got] = co_await transport->async_read(std::span<std::byte>{head});
        n = got;
        if (ec && got == 0) {
            co_return;  // EOF/error before a single byte: nothing to classify
        }
        co_return;
    };
    bool timed_out = false;
    auto detection_deadline = [&]() -> asio::awaitable<void> {
        asio::steady_timer timer{co_await asio::this_coro::executor};
        timer.expires_after(limits.idle_timeout);
        co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        timed_out = true;
    };

    // Race the read against the deadline only when there is a deadline. Racing a
    // no-op against it is not a harmless simplification: `a || b` completes as
    // soon as either side does, so a "disabled" branch that returns without
    // suspending wins instantly, cancels the read before a byte arrives, and
    // every connection is accepted and dropped.
    if (limits.idle_timeout.count() <= 0) {
        co_await read_head();
    } else {
        co_await (read_head() || detection_deadline());
    }
    if (n == 0) {
        transport->close();
        co_return;
    }

    std::string_view header{reinterpret_cast<const char*>(head.data()), n};
    if (n == h2_preface_prefix.size() && header == h2_preface_prefix) {
        // HTTP/2 prior-knowledge: replay the consumed prefix into the engine, which
        // reads the rest of the 24-octet preface itself (RFC 7540 §3.5). The
        // engine is shared so in-flight handlers keep it alive.
        auto engine = std::make_shared<Http2Engine<Transport>>(transport, limits);
        co_await engine->run(std::move(dispatch), std::string{header});
        co_return;
    }

    // HTTP/1.x: hand the already-read bytes to the engine via its initial buffer.
    // A registered WebSocket route (ws_lookup) enables the Upgrade: websocket
    // handshake inside the h1 engine.
    Http1Engine<Transport> engine{transport, limits};
    co_await engine.run(dispatch, std::string{header}, std::move(ws_lookup), std::move(ws_proxy_lookup));
    co_return;
}

// Serve a TLS transport: handshake, then ALPN selects the engine.
template <typename TlsTransportT>
inline asio::awaitable<void> serve_tls(std::shared_ptr<TlsTransportT> transport, Dispatcher dispatch,
                                       WsLookup ws_lookup = {}, EngineLimits limits = {},
                                       WsProxyLookup ws_proxy_lookup = {}) {
    using namespace asio::experimental::awaitable_operators;

    // The handshake is bounded for exactly the reason the plaintext detection
    // read is: a peer that completes the TCP handshake and then sends nothing
    // would otherwise pin a socket, a coroutine frame and executor capacity
    // forever — the engines' watchdogs only start once a protocol is chosen.
    error_code ec;
    if (limits.idle_timeout.count() <= 0) {
        ec = co_await transport->handshake();
    } else {
        auto deadline = [&]() -> asio::awaitable<error_code> {
            asio::steady_timer timer{co_await asio::this_coro::executor};
            timer.expires_after(limits.idle_timeout);
            co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
            co_return make_error_code(asio::error::timed_out);
        };
        // `||` yields a variant, and both arms are error_code — the index is
        // what says which one finished, not the value.
        auto raced = co_await (transport->handshake() || deadline());
        ec = raced.index() == 1 ? std::get<1>(raced) : std::get<0>(raced);
    }
    if (ec) {
        SIMPLE_HTTP_ERROR_LOG("TLS handshake failed: {}", ec.message());
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
