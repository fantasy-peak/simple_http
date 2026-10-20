#pragma once

// Shared WebSocket reverse-proxy handshake helpers.
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

#include <algorithm>
#include <array>
#include <boost/asio.hpp>
#include <boost/asio/cancel_after.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "../core/base64.h"
#include "../core/http_status.h"
#include "../core/logging.h"
#include "../core/types.h"
#include "../proto/headers.h"
#include "../proto/ws_frame.h" // ws_accept_key
#include "dispatcher.h"        // WsProxyTarget
#include "h1/h1_parser.h"      // H1ResponseParser / ParsedResponseHead

namespace simple_http {

namespace asio = boost::asio;

namespace detail {

inline std::string ws_lower(std::string_view s) {
    std::string out{s};
    for (auto &c : out)
        c = ascii_lower(c);
    return out;
}

inline bool ws_ci_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i]))
            return false;
    }
    return true;
}

// Whether a comma-separated field value contains `token` (case-insensitive).
inline bool ws_value_has_token(std::string_view value, std::string_view token) {
    std::size_t pos = 0;
    while (pos <= value.size()) {
        const std::size_t comma = value.find(',', pos);
        std::string_view item =
            value.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t'))
            item.remove_prefix(1);
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t'))
            item.remove_suffix(1);
        if (ws_ci_equal(item, token))
            return true;
        if (comma == std::string_view::npos)
            break;
        pos = comma + 1;
    }
    return false;
}

inline std::vector<std::string> ws_connection_tokens(const Headers &headers) {
    std::vector<std::string> tokens;
    for (auto conn : headers.get_all("connection")) {
        std::size_t pos = 0;
        while (pos < conn.size()) {
            const std::size_t comma = conn.find(',', pos);
            std::string_view item =
                conn.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
            while (!item.empty() && (item.front() == ' ' || item.front() == '\t'))
                item.remove_prefix(1);
            while (!item.empty() && (item.back() == ' ' || item.back() == '\t'))
                item.remove_suffix(1);
            if (!item.empty())
                tokens.push_back(ws_lower(item));
            if (comma == std::string_view::npos)
                break;
            pos = comma + 1;
        }
    }
    return tokens;
}

// Hop-by-hop fields that must not be copied to the backend. Connection and
// Upgrade are written by the builder itself; the rest are dropped entirely.
inline bool ws_is_hop_by_hop(std::string_view name) {
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
        "http2-settings",
    };
    for (auto h : kHop) {
        if (name == h)
            return true;
    }
    return false;
}

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

    const auto conn_tokens = ws_connection_tokens(in);
    for (const auto &[name, value] : in) {
        if (ws_is_hop_by_hop(name) || contains_ctl(name) || contains_ctl(value))
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

// One bounded read from the backend. `timeout == 0` means no deadline (the
// route's connect budget disabled); otherwise the operation is cancelled with
// asio::error::timed_out. Every handshake/refusal read goes through this so a
// backend that accepts and then says nothing cannot pin the frontend forever.
template <typename Socket>
asio::awaitable<std::pair<error_code, std::size_t>> ws_proxy_read_some(Socket &socket, std::span<std::byte> buf,
                                                                       std::chrono::steady_clock::duration timeout) {
    if (timeout.count() <= 0) {
        auto [ec, n] =
            co_await socket.async_read_some(asio::buffer(buf.data(), buf.size()), asio::as_tuple(asio::use_awaitable));
        co_return std::pair{ec, n};
    }
    auto [ec, n] = co_await socket.async_read_some(asio::buffer(buf.data(), buf.size()),
                                                   asio::cancel_after(timeout, asio::as_tuple(asio::use_awaitable)));
    co_return std::pair{ec, n};
}

// Reads and parses the backend's response head. Bounded by `max_header_bytes`;
// anything after the head is returned as `initial` so no tunnel byte is lost.
template <typename Socket>
asio::awaitable<std::expected<WsProxyReplyHead, error_code>>
ws_proxy_read_reply(Socket &socket, std::size_t max_header_bytes, std::chrono::steady_clock::duration timeout) {
    H1ResponseParser parser;
    std::string raw;
    std::array<std::byte, 4096> buf{};
    for (;;) {
        const auto state = parser.parse_head();
        if (state == H1ResponseParser::State::Done) {
            const std::size_t consumed = parser.consumed();
            WsProxyReplyHead out;
            out.head = parser.head();
            out.raw = raw.substr(0, consumed);
            out.initial = raw.substr(consumed);
            co_return out;
        }
        if (state == H1ResponseParser::State::Error) {
            co_return std::unexpected{make_error_code(asio::error::invalid_argument)};
        }
        if (raw.size() > max_header_bytes) {
            co_return std::unexpected{make_error_code(asio::error::no_buffer_space)};
        }
        auto [ec, n] = co_await ws_proxy_read_some(socket, std::span<std::byte>{buf}, timeout);
        if (ec || n == 0) {
            co_return std::unexpected{ec ? ec : make_error_code(asio::error::eof)};
        }
        raw.append(reinterpret_cast<const char *>(buf.data()), n);
        parser.feed(std::string_view{reinterpret_cast<const char *>(buf.data()), n});
    }
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
                } else if (int digit = hex_value(c); digit >= 0) {
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
                // The terminating blank line's LF; anything else is malformed,
                // and either way the relay stops here.
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

    static int hex_value(char c) {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    }

    State m_state{State::Size};
    std::uint64_t m_size{0};
    std::uint64_t m_remaining{0};
};

// Appends a non-101 refusal body to `out` (which already holds the head and the
// bytes returned as `initial`) until the response ends. Content-Length is
// honored exactly; chunked runs to its terminating chunk; anything else is
// close-delimited and stops at EOF. Bounded in bytes and in time.
template <typename Socket>
asio::awaitable<void> ws_proxy_relay_refusal_body(Socket &backend, std::string &out, std::string_view initial,
                                                  const Headers &headers, std::chrono::steady_clock::duration timeout) {
    constexpr std::size_t kMaxRefusalBodyBytes = 4 * 1024 * 1024;
    const std::size_t body_start = out.size() - initial.size();
    std::array<std::byte, 8192> buf{};

    std::uint64_t want = 0;
    bool has_length = false;
    if (auto cl = headers.get("content-length")) {
        auto [ptr, ec] = std::from_chars(cl->data(), cl->data() + cl->size(), want);
        has_length = ec == std::errc{} && ptr == cl->data() + cl->size();
    }
    bool chunked = false;
    for (auto value : headers.get_all("transfer-encoding")) {
        if (ws_value_has_token(value, "chunked"))
            chunked = true;
    }
    // RFC 9112 §6.3: Transfer-Encoding overrides Content-Length. Honoring both
    // would stop at a byte count the sender did not frame the body with.
    if (chunked)
        has_length = false;

    auto body_size = [&] { return out.size() - body_start; };
    auto over_cap = [&] { return body_size() >= kMaxRefusalBodyBytes; };

    if (has_length && initial.size() >= want) {
        co_return; // the head read already carried the whole body
    }

    H1ChunkedEndScanner scanner;
    if (chunked)
        scanner.feed(initial);

    std::uint64_t left = has_length ? want - std::min<std::uint64_t>(want, initial.size()) : 0;
    for (;;) {
        if (over_cap())
            co_return;
        if (has_length && left == 0)
            co_return;
        if (chunked && scanner.done())
            co_return;
        const std::size_t ask =
            has_length ? static_cast<std::size_t>(std::min<std::uint64_t>(buf.size(), left)) : buf.size();
        auto [ec, n] = co_await ws_proxy_read_some(backend, std::span<std::byte>{buf.data(), ask}, timeout);
        if (ec || n == 0)
            co_return; // EOF is the end for a close-delimited body, and an error ends the relay
        out.append(reinterpret_cast<const char *>(buf.data()), n);
        if (chunked)
            scanner.feed(std::string_view{reinterpret_cast<const char *>(buf.data()), n});
        if (has_length)
            left -= std::min<std::uint64_t>(left, n);
    }
}

// A 101 is only usable if it actually switched to WebSocket (Go checks the
// Upgrade type; the accept key check protects the h2 leg, where the proxy
// synthesized the key and the frontend cannot see it).
inline bool ws_validate_101(const ParsedResponseHead &head, std::string_view expected_accept) {
    if (head.status != 101)
        return false;
    bool connection_upgrade = false;
    for (auto conn : head.headers.get_all("connection")) {
        if (ws_value_has_token(conn, "upgrade")) {
            connection_upgrade = true;
            break;
        }
    }
    if (!connection_upgrade)
        return false;
    const auto upgrade = head.headers.get("upgrade");
    if (!upgrade || !ws_ci_equal(*upgrade, "websocket"))
        return false;
    if (!expected_accept.empty()) {
        const auto accept = head.headers.get("sec-websocket-accept");
        if (!accept || *accept != expected_accept)
            return false;
    }
    return true;
}

// Resolves, connects and applies the route's connect budget on the connection
// executor. The socket is returned alive; the caller owns it.
template <typename Executor>
asio::awaitable<std::expected<std::shared_ptr<asio::ip::tcp::socket>, error_code>>
ws_proxy_connect(const Executor &executor, const WsProxyTarget &target) {
    using namespace asio::experimental::awaitable_operators;
    auto socket = std::make_shared<asio::ip::tcp::socket>(executor);

    auto connect_op = [&]() -> asio::awaitable<error_code> {
        auto [rec, endpoints] = co_await resolve_endpoints(target.resolve, target.host, target.port, executor);
        if (rec)
            co_return rec;
        if (endpoints.empty())
            co_return make_error_code(asio::error::host_not_found);
        auto [cec, _] = co_await asio::async_connect(*socket, endpoints, asio::as_tuple(asio::use_awaitable));
        (void)_;
        co_return cec;
    };

    error_code ec;
    if (target.connect_timeout.count() > 0) {
        auto timer_op = [&]() -> asio::awaitable<void> {
            asio::steady_timer timer{executor};
            timer.expires_after(target.connect_timeout);
            auto [tec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
            (void)tec;
            co_return;
        };
        auto outcome = co_await (connect_op() || timer_op());
        if (auto *value = std::get_if<error_code>(&outcome)) {
            ec = *value;
        } else {
            ec = make_error_code(asio::error::timed_out);
        }
    } else {
        ec = co_await connect_op();
    }
    if (ec) {
        error_code ignored;
        socket->close(ignored);
        co_return std::unexpected{ec};
    }
    co_return socket;
}

} // namespace detail

} // namespace simple_http
