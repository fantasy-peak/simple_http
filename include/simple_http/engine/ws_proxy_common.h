#pragma once

// Shared WebSocket reverse-proxy handshake helpers (C++17 token-only port).
//
// Both frontends use the same upstream leg:
//   * HTTP/1.1 Upgrade (h1 frontend, and h2 frontend translated by the engine);
//   * byte-level pass-through after 101.
//
// This header owns the Go httputil.ReverseProxy-shaped part: build the
// outbound Upgrade request from the inbound fields, strip hop-by-hop headers,
// author X-Forwarded-*, read/validate the backend's response head, and impose a
// connect budget. The two engines keep their own tunnel loops because one side
// is a Transport and the other is an h2 stream, but the handshake must not
// drift between them.
//
// Every asynchronous helper is an Asio initiation with an explicit token.

#include <algorithm>
#include <array>
#include <boost/asio.hpp>
#include <boost/asio/cancel_after.hpp>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../core/base64.h"
#include "../core/compat.h"
#include "../core/http_field.h"
#include "../core/http_status.h"
#include "../core/logging.h"
#include "../core/resolve.h"
#include "../core/types.h"
#include "../proto/headers.h"
#include "../proto/ws_frame.h" // ws_accept_key
#include "dispatcher.h"        // WsProxyTarget
#include "h1/h1_parser.h"      // H1ResponseParser / ParsedResponseHead

namespace simple_http {

namespace asio = boost::asio;

namespace detail {

inline std::string ws_authority(const WsProxyTarget &target) {
    std::string out;
    if (target.host.find(':') != std::string::npos) {
        out.push_back('[');
        out.append(target.host);
        out.push_back(']');
    } else {
        out.append(target.host);
    }
    out.push_back(':');
    out.append(std::to_string(target.port));
    return out;
}

// A fresh Sec-WebSocket-Key for a backend leg whose frontend used HTTP/2
// extended CONNECT (and therefore had no key of its own).
inline std::string make_ws_client_key() {
    std::array<unsigned char, 16> nonce{};
    std::random_device rd;
    for (auto &b : nonce)
        b = static_cast<unsigned char>(rd());
    return base64_encode(std::string_view{reinterpret_cast<const char *>(nonce.data()), nonce.size()});
}

// Builds the HTTP/1.1 Upgrade request the backend sees. `in` is the inbound
// header section (Host included), `request_target` already contains the query,
// and `key` is either the frontend's key (h1 replay) or a synthesized one (h2).
//
// Header policy matches the request-level proxy (and Go's Rewrite mode):
// hop-by-hop fields are removed, inbound Forwarded/X-Forwarded-* are discarded,
// X-Forwarded-For/-Proto/-Host are authored here, and the Host policy is the
// route's (preserve the frontend Host by default, or an explicit override).
inline std::string build_ws_upgrade_head(const Headers &in, std::string_view inbound_host,
                                         std::string_view request_target, const WsProxyTarget &target,
                                         std::string_view peer_address, bool client_is_tls, std::string_view key) {
    std::string out;
    out.reserve(256 + request_target.size());
    out.append("GET ");
    out.append(request_target.empty() ? std::string_view{"/"} : request_target);
    out.append(" HTTP/1.1\r\n");

    std::string host = target.host_header;
    if (host.empty() && target.preserve_host)
        host.assign(inbound_host);
    if (host.empty())
        host = ws_authority(target);
    if (contains_ctl(host))
        host = ws_authority(target);
    out.append("host: ");
    out.append(host);
    out.append("\r\n");

    out.append("upgrade: websocket\r\n");
    out.append("connection: Upgrade\r\n");
    out.append("sec-websocket-key: ");
    out.append(key.empty() ? make_ws_client_key() : std::string{key});
    out.append("\r\n");

    // Preserve the client's version if it sent one; every conforming handshake
    // carries 13, and the synthesized h2 leg has no version at all.
    if (auto version = in.get("sec-websocket-version")) {
        if (!contains_ctl(*version)) {
            out.append("sec-websocket-version: ");
            out.append(*version);
            out.append("\r\n");
        }
    } else {
        out.append("sec-websocket-version: 13\r\n");
    }

    const auto conn_tokens = connection_tokens(in);
    for (const auto &[name, value] : in) {
        if (is_hop_by_hop_field(name) || contains_ctl(name) || contains_ctl(value))
            continue;
        if (name == "host" || name == "content-length" || name == "expect" || name == "sec-websocket-key" ||
            name == "sec-websocket-version")
            continue;
        if (name == "forwarded" || name == "x-forwarded-for" || name == "x-forwarded-host" ||
            name == "x-forwarded-proto")
            continue;
        bool nominated = false;
        for (const auto &token : conn_tokens) {
            if (token == name) {
                nominated = true;
                break;
            }
        }
        if (nominated)
            continue;
        out.append(name);
        out.append(": ");
        out.append(value);
        out.append("\r\n");
    }

    // X-Forwarded-For: only the real peer by default (Go Rewrite mode), or the
    // inbound chain appended to it when the route asks for legacy behaviour.
    std::string xff;
    if (target.preserve_inbound_forwarding) {
        for (auto prior : in.get_all("x-forwarded-for")) {
            if (!xff.empty())
                xff.append(", ");
            xff.append(prior);
        }
    }
    if (!peer_address.empty()) {
        if (!xff.empty())
            xff.append(", ");
        xff.append(peer_address);
    }
    if (!xff.empty()) {
        out.append("x-forwarded-for: ");
        out.append(xff);
        out.append("\r\n");
    }
    out.append("x-forwarded-proto: ");
    out.append(client_is_tls ? "https" : "http");
    out.append("\r\n");
    if (!inbound_host.empty() && !contains_ctl(inbound_host)) {
        out.append("x-forwarded-host: ");
        out.append(inbound_host);
        out.append("\r\n");
    }
    out.append("\r\n");
    return out;
}

struct WsProxyReplyHead {
    ParsedResponseHead head;
    std::string raw;     // the raw head, including the terminating CRLFCRLF
    std::string initial; // bytes the backend sent after the head
};

// Best-effort teardown of the backend socket, shared by both tunnel frontends.
template <typename Socket> void ws_proxy_close(Socket &backend) {
    error_code ec;
    backend.shutdown(asio::socket_base::shutdown_both, ec);
    backend.close(ec);
}

// The target the backend sees: the route's rewrite path when configured,
// otherwise the frontend's path, always carrying the frontend's query string
// (session tokens and code-server's reconnectionToken live there). `query` is
// nullopt when the inbound target had no '?' at all; an engaged empty view means
// the target ended in a bare '?', which is replayed verbatim. Returns nullopt
// when the result contains CR/LF/NUL -- the caller answers 400 and must not
// splice it into a request line.
inline std::optional<std::string> ws_proxy_replay_target(std::string_view rewrite_path, std::string_view path,
                                                         std::optional<std::string_view> query) {
    std::string target{rewrite_path.empty() ? path : rewrite_path};
    if (target.empty())
        target = "/";
    if (query) {
        target.push_back('?');
        target.append(*query);
    }
    if (contains_ctl(target))
        return std::nullopt;
    return target;
}

// Tracks where an HTTP/1.1 body ends without buffering or decoding it. Only the
// refusal path uses this: the body is relayed verbatim, but the proxy must know
// when to stop instead of waiting for a close-delimited EOF (a keep-alive
// backend never sends one) or truncating at a Content-Length that is not there.
class H1ChunkedEndScanner {
  public:
    void feed(std::string_view bytes) {
        for (char c : bytes) {
            if (m_state == State::Done)
                return;
            switch (m_state) {
            case State::Size:
                if (c == '\r') {
                    m_state = State::SizeLf;
                } else if (c == ';') {
                    m_state = State::SizeExt;
                } else if (int digit = hex_digit_value(c); digit >= 0) {
                    m_size = m_size * 16 + static_cast<std::uint64_t>(digit);
                } else {
                    m_state = State::Done; // malformed: stop relaying, never spin
                }
                break;
            case State::SizeExt:
                if (c == '\r')
                    m_state = State::SizeLf;
                break;
            case State::SizeLf:
                m_state = c == '\n' ? (m_size == 0 ? State::TrailerStart : State::Data) : State::Done;
                m_remaining = m_size;
                break;
            case State::Data:
                if (m_remaining > 0)
                    --m_remaining;
                if (m_remaining == 0)
                    m_state = State::DataCr;
                break;
            case State::DataCr:
                m_state = c == '\r' ? State::DataLf : State::Done;
                break;
            case State::DataLf:
                if (c != '\n') {
                    m_state = State::Done;
                } else {
                    m_size = 0;
                    m_state = State::Size;
                }
                break;
            case State::TrailerStart:
                m_state = c == '\r' ? State::TrailerEndLf : State::TrailerRest;
                break;
            case State::TrailerRest:
                if (c == '\r')
                    m_state = State::TrailerLf;
                break;
            case State::TrailerLf:
                m_state = c == '\n' ? State::TrailerStart : State::Done;
                break;
            case State::TrailerEndLf:
                m_state = State::Done;
                break;
            case State::Done:
                break;
            }
        }
    }

    [[nodiscard]] bool done() const { return m_state == State::Done; }

  private:
    enum class State {
        Size,
        SizeExt,
        SizeLf,
        Data,
        DataCr,
        DataLf,
        TrailerStart,
        TrailerRest,
        TrailerLf,
        TrailerEndLf,
        Done
    };

    State m_state{State::Size};
    std::uint64_t m_size{0};
    std::uint64_t m_remaining{0};
};

// A 101 is only usable if it actually switched to WebSocket (Go checks the
// Upgrade type; the accept key check protects the h2 leg, where the proxy
// synthesized the key and the frontend cannot see it).
inline bool ws_validate_101(const ParsedResponseHead &head, std::string_view expected_accept) {
    if (head.status != status::switching_protocols)
        return false;
    bool connection_upgrade = false;
    for (auto conn : head.headers.get_all("connection")) {
        if (value_has_token(conn, "upgrade")) {
            connection_upgrade = true;
            break;
        }
    }
    if (!connection_upgrade)
        return false;
    const auto upgrade = head.headers.get("upgrade");
    if (!upgrade || !iequals_ci(*upgrade, "websocket"))
        return false;
    if (!expected_accept.empty()) {
        const auto accept = head.headers.get("sec-websocket-accept");
        if (!accept || *accept != expected_accept)
            return false;
    }
    return true;
}

// --- asynchronous helpers ----------------------------------------------------

// Reads and parses the backend's response head. Bounded by `max_header_bytes`;
// anything after the head is returned as `initial` so no tunnel byte is lost.
// Completion: void(error_code, WsProxyReplyHead).
//
// The mutable state lives behind a shared_ptr: passing `std::move(self)` to an
// Asio operation moves the composed operation, and a buffer bound to a member
// array would point into the moved-from copy. Sharing the state keeps the
// read buffer, parser and accumulated bytes in one heap object that every copy
// of the operation refers to.
template <typename Socket> struct WsReadReplyState {
    explicit WsReadReplyState(Socket &s, std::size_t cap, std::chrono::steady_clock::duration t)
        : socket(s), max_header_bytes(cap), timeout(t) {}

    Socket &socket;
    std::size_t max_header_bytes;
    std::chrono::steady_clock::duration timeout;
    H1ResponseParser parser;
    std::string raw;
    std::array<std::byte, 4096> buf{};
    bool started{false};
};

template <typename Socket> struct WsReadReplyOp {
    std::shared_ptr<WsReadReplyState<Socket>> state;

    asio::any_io_executor get_executor() const noexcept { return state->socket.get_executor(); }

    template <typename Self> void operator()(Self &self, error_code ec = {}, std::size_t n = 0) {
        auto &st = *state;
        if (!st.started) {
            st.started = true;
        } else {
            if (ec || n == 0) {
                self.complete(ec ? ec : make_error_code(asio::error::eof), WsProxyReplyHead{});
                return;
            }
            st.raw.append(reinterpret_cast<const char *>(st.buf.data()), n);
            st.parser.feed(std::string_view{reinterpret_cast<const char *>(st.buf.data()), n});
        }
        const auto state_now = st.parser.parse_head();
        if (state_now == H1ResponseParser::State::Done) {
            const std::size_t consumed = st.parser.consumed();
            // The head may complete in the same read that pushes it over the
            // bound; the NeedMore check below would never run. Same rule as the
            // h1 request parser (AGENTS §9.28).
            if (consumed > st.max_header_bytes) {
                self.complete(make_error_code(asio::error::no_buffer_space), WsProxyReplyHead{});
                return;
            }
            WsProxyReplyHead out;
            out.head = st.parser.head();
            out.raw = st.raw.substr(0, consumed);
            out.initial = st.raw.substr(consumed);
            self.complete(error_code{}, std::move(out));
            return;
        }
        if (state_now == H1ResponseParser::State::Error) {
            self.complete(make_error_code(asio::error::invalid_argument), WsProxyReplyHead{});
            return;
        }
        if (st.raw.size() > st.max_header_bytes) {
            self.complete(make_error_code(asio::error::no_buffer_space), WsProxyReplyHead{});
            return;
        }
        if (st.timeout.count() > 0) {
            st.socket.async_read_some(asio::buffer(st.buf.data(), st.buf.size()),
                                      asio::cancel_after(st.timeout, std::move(self)));
        } else {
            st.socket.async_read_some(asio::buffer(st.buf.data(), st.buf.size()), std::move(self));
        }
    }
};

template <typename Socket, typename Token>
auto async_ws_proxy_read_reply(Socket &socket, std::size_t max_header_bytes,
                               std::chrono::steady_clock::duration timeout, Token &&token) {
    return asio::async_compose<Token, void(error_code, WsProxyReplyHead)>(
        WsReadReplyOp<Socket>{std::make_shared<WsReadReplyState<Socket>>(socket, max_header_bytes, timeout)}, token,
        socket.get_executor());
}

// Appends a non-101 refusal body to `out` (which already holds the head and the
// bytes returned as `initial`) until the response ends. Content-Length is
// honored exactly; chunked runs to its terminating chunk; anything else is
// close-delimited and stops at EOF. Bounded in bytes and in time.
template <typename Socket> struct WsRelayRefusalState {
    WsRelayRefusalState(Socket &s, std::string &o, std::size_t initial, Headers h,
                        std::chrono::steady_clock::duration t)
        : socket(s), out(o), initial_size(initial), headers(std::move(h)), timeout(t) {}

    Socket &socket;
    std::string &out;
    std::size_t initial_size;
    Headers headers;
    std::chrono::steady_clock::duration timeout;

    std::uint64_t want{0};
    bool has_length{false};
    bool chunked{false};
    H1ChunkedEndScanner scanner;
    std::uint64_t left{0};
    std::size_t body_start{0};
    std::array<std::byte, 8192> buf{};
    bool started{false};

    static constexpr std::size_t kMaxRefusalBodyBytes = 4 * 1024 * 1024;

    void prepare() {
        body_start = out.size() - initial_size;
        if (auto cl = headers.get("content-length")) {
            auto [ptr, e] = std::from_chars(cl->data(), cl->data() + cl->size(), want);
            has_length = e == std::errc{} && ptr == cl->data() + cl->size();
        }
        for (auto value : headers.get_all("transfer-encoding")) {
            if (value_has_token(value, "chunked"))
                chunked = true;
        }
        // RFC 9112 §6.3: Transfer-Encoding overrides Content-Length.
        if (chunked)
            has_length = false;
        if (chunked)
            scanner.feed(out.substr(out.size() - initial_size));
        left = has_length ? want - std::min<std::uint64_t>(want, initial_size) : 0;
    }

    std::size_t body_size() const { return out.size() - body_start; }
};

template <typename Socket> struct WsRelayRefusalOp {
    std::shared_ptr<WsRelayRefusalState<Socket>> state;

    asio::any_io_executor get_executor() const noexcept { return state->socket.get_executor(); }

    template <typename Self> void operator()(Self &self, error_code ec = {}, std::size_t n = 0) {
        auto &st = *state;
        if (!st.started) {
            st.started = true;
            st.prepare();
            if ((st.has_length && st.initial_size >= st.want) || (st.has_length && st.left == 0) ||
                (st.chunked && st.scanner.done())) {
                self.complete(error_code{});
                return;
            }
        } else {
            if (ec || n == 0) {
                self.complete(error_code{});
                return;
            }
            st.out.append(reinterpret_cast<const char *>(st.buf.data()), n);
            if (st.chunked)
                st.scanner.feed(std::string_view{reinterpret_cast<const char *>(st.buf.data()), n});
            if (st.has_length)
                st.left -= std::min<std::uint64_t>(st.left, n);
        }
        if (st.body_size() >= WsRelayRefusalState<Socket>::kMaxRefusalBodyBytes || (st.has_length && st.left == 0) ||
            (st.chunked && st.scanner.done())) {
            self.complete(error_code{});
            return;
        }
        const std::size_t ask =
            st.has_length ? static_cast<std::size_t>(std::min<std::uint64_t>(st.buf.size(), st.left)) : st.buf.size();
        if (st.timeout.count() > 0) {
            st.socket.async_read_some(asio::buffer(st.buf.data(), ask),
                                      asio::cancel_after(st.timeout, std::move(self)));
        } else {
            st.socket.async_read_some(asio::buffer(st.buf.data(), ask), std::move(self));
        }
    }
};

template <typename Socket, typename Token>
auto async_ws_proxy_relay_refusal_body(Socket &socket, std::string &out, std::string_view initial, Headers headers,
                                       std::chrono::steady_clock::duration timeout, Token &&token) {
    return asio::async_compose<Token, void(error_code)>(
        WsRelayRefusalOp<Socket>{
            std::make_shared<WsRelayRefusalState<Socket>>(socket, out, initial.size(), std::move(headers), timeout)},
        token, socket.get_executor());
}

// Resolves, connects and applies the route's connect budget on the connection
// executor. Completion: void(error_code, shared_ptr<tcp::socket>).
//
// Implemented with async_initiate + an explicitly shared state rather than a
// composed operation: the two steps complete with different argument types
// (vector<endpoint> then endpoint), and a single composed operator() overload
// set cannot be both the token for the resolver and for the connect.
template <typename Executor, typename Token>
auto async_ws_proxy_connect(Executor executor, WsProxyTarget target, Token &&token) {
    return asio::async_initiate<Token, void(error_code, std::shared_ptr<asio::ip::tcp::socket>)>(
        [executor, target = std::move(target)](auto handler) mutable {
            struct State {
                State(Executor ex, WsProxyTarget t,
                      asio::any_completion_handler<void(error_code, std::shared_ptr<asio::ip::tcp::socket>)> h)
                    : executor(ex), target(std::move(t)), done(std::move(h)) {}
                Executor executor;
                WsProxyTarget target;
                std::shared_ptr<asio::ip::tcp::socket> socket;
                asio::any_completion_handler<void(error_code, std::shared_ptr<asio::ip::tcp::socket>)> done;
            };
            auto state = std::make_shared<State>(executor, std::move(target), std::move(handler));
            state->socket = std::make_shared<asio::ip::tcp::socket>(state->executor);
            // Same policy as the client layer (ClientConfig::tcp_nodelay): on by
            // default, best-effort, applied before the dial. A WebSocket tunnel
            // is latency-sensitive and often writes small frames, so Nagle would
            // add a delayed-ACK stall per message.
            if (state->target.tcp_nodelay) {
                error_code ne;
                state->socket->set_option(asio::ip::tcp::no_delay(true), ne);
            }
            async_resolve_endpoints(
                state->target.resolve, state->target.host, state->target.port, state->executor,
                [state](error_code ec, std::vector<asio::ip::tcp::endpoint> endpoints) {
                    if (ec) {
                        std::move(state->done)(ec, nullptr);
                        return;
                    }
                    if (endpoints.empty()) {
                        std::move(state->done)(make_error_code(asio::error::host_not_found), nullptr);
                        return;
                    }
                    auto on_connect = [state](error_code cec, asio::ip::tcp::endpoint) {
                        if (cec) {
                            error_code ignored;
                            state->socket->close(ignored);
                            std::move(state->done)(
                                cec == asio::error::operation_aborted ? make_error_code(asio::error::timed_out) : cec,
                                nullptr);
                            return;
                        }
                        std::move(state->done)(error_code{}, state->socket);
                    };
                    if (state->target.connect_timeout.count() > 0) {
                        asio::async_connect(*state->socket, endpoints,
                                            asio::cancel_after(state->target.connect_timeout, std::move(on_connect)));
                    } else {
                        asio::async_connect(*state->socket, endpoints, std::move(on_connect));
                    }
                });
        },
        token);
}

} // namespace detail

} // namespace simple_http
