#pragma once

// Client-side configuration, targets and error codes for the outbound HTTP
// client (client/ layer).
//
// The split mirrors the server side: ClientTarget says *where* to connect and
// which HTTP version to speak there (so one HttpClient can serve many origins),
// ClientConfig carries the policy that applies to every connection it opens
// (TLS knobs, timeouts, protocol limits, connection-pool sizing, hooks).
//
// Fallible client operations report failure as std::expected<…, error_code>
// (never exceptions), like the rest of the library. Transport failures surface
// as the underlying asio error_code; failures the client itself decides on use
// client_errc below.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/system/error_code.hpp>

#include "../core/limits.h"
#include "../core/types.h"

namespace simple_http {

namespace asio = boost::asio;

// Which HTTP version to use on the wire.
enum class HttpVersionPolicy : std::uint8_t {
    // https: ALPN decides — h2 when the peer selects it, HTTP/1.1 otherwise.
    // http:  h2c per H2cMode (which may also end up as HTTP/1.1).
    Auto = 0,
    // Force HTTP/1.1: no ALPN h2 offer, no h2c attempt.
    Http11 = 1,
    // Require HTTP/2. TLS needs ALPN to select "h2"; plaintext uses h2c per
    // H2cMode, and a peer that will not speak h2 fails with
    // client_errc::version_not_negotiated.
    Http2 = 2,
};

// How plaintext HTTP/2 (h2c) is established.
enum class H2cMode : std::uint8_t {
    // Start with an HTTP/1.1 `Upgrade: h2c` request (RFC 9113 §3.2) and continue
    // as h2 only if the peer answers 101. Costs one round trip, but interops with
    // servers that ignore the upgrade (they simply answer the request as
    // HTTP/1.1) as well as with servers that support it — the safe default.
    Upgrade = 0,
    // Send the connection preface straight away (RFC 9113 §3.4): no extra round
    // trip, but a peer that does not speak h2 sees a malformed request line
    // ("PRI * HTTP/2.0"). Only sensible when the peer is known to speak h2c.
    PriorKnowledge = 1,
    // Never speak h2 over plaintext: http:// is always HTTP/1.1.
    Off = 2,
};

// Minimum accepted TLS version. Defaults to 1.2 — unlike the server, which pins
// 1.3, a client still has to reach peers that have not moved to 1.3.
enum class TlsMinVersion : std::uint8_t {
    Tls12 = 0,
    Tls13 = 1,
};

// TLS knobs for a client connection.
struct TlsClientConfig {
    // Verify the peer's certificate chain.
    bool verify_peer{true};
    // Additionally verify that the certificate matches the host name asked for
    // (SAN, falling back to CN). Turn off for certificates without a usable
    // name (the library's own test certificate, for instance).
    bool verify_host{true};
    // CA bundle used for verification; empty = OpenSSL's system defaults.
    std::string ca_file;
    // Optional client certificate + key for mutual TLS. Both must be set.
    std::string cert_chain_file;
    std::string private_key_file;
    TlsMinVersion min_version{TlsMinVersion::Tls12};
    // Name sent as SNI and verified against when the target does not name one.
    // Needed when a target is reached by address but its certificate is issued
    // for a name (a load-balancer VIP, a tunnelled origin, …).
    std::string sni_override;
    // ALPN protocol list override. Empty = derived from the HTTP version policy
    // (Auto: {"h2", "http/1.1"}; Http11: {"http/1.1"}; Http2: {"h2"}).
    std::vector<std::string> alpn;
    // Customization hook, invoked on the fresh ssl::context before the library
    // applies the settings above — so those fields win over anything the hook
    // sets for the same knob. (`verify_host` is ignored unless `verify_peer` is
    // set: without chain verification a name check proves nothing.)
    std::function<void(asio::ssl::context&)> setup;
};

// Where to connect and how to speak there. One HttpClient can serve many of
// these; pooled connections are keyed by the whole target.
struct ClientTarget {
    std::string host{"127.0.0.1"};
    std::uint16_t port{0};  // 0 = the scheme default (80 for http, 443 for https)
    bool use_tls{false};
    HttpVersionPolicy version{HttpVersionPolicy::Auto};
    H2cMode h2c{H2cMode::Upgrade};
    // TLS SNI / host-name verification override; empty = `host`.
    std::string sni;
    // Extra connection-pool discriminator, for cases the other fields cannot
    // express (different credentials for the same origin, a different TLS
    // customization hook, …).
    std::string pool_tag;

    // The port actually dialed.
    std::uint16_t effective_port() const {
        if (port != 0)
            return port;
        return use_tls ? 443 : 80;
    }

    // The name used for SNI and host-name verification.
    std::string_view sni_host() const {
        return sni.empty() ? std::string_view{host} : std::string_view{sni};
    }

    // "host:port" as it appears in the Host header / :authority. IPv6 literals
    // are bracketed, as the authority grammar requires.
    std::string authority() const {
        std::string out;
        if (host.find(':') != std::string::npos) {
            out.push_back('[');
            out.append(host);
            out.push_back(']');
        } else {
            out.append(host);
        }
        out.push_back(':');
        out.append(std::to_string(effective_port()));
        return out;
    }

    bool h2c_enabled() const {
        return !use_tls && h2c != H2cMode::Off;
    }
};

// Policy shared by every connection an HttpClient opens.
struct ClientConfig {
    TlsClientConfig tls{};

    // Defaults for requests that name only a URL (which cannot express a version
    // policy). A ClientTarget passed explicitly overrides them.
    HttpVersionPolicy default_version{HttpVersionPolicy::Auto};
    H2cMode default_h2c{H2cMode::Upgrade};

    // Whole-connection connect budget (DNS + TCP + TLS handshake).
    std::chrono::milliseconds connect_timeout{10000};
    // How long a session may sit idle before it is closed: a stalled read on a
    // live exchange, and an idle pooled connection that nothing ever comes back
    // for (which would otherwise hold its socket for the process's lifetime).
    // This is the client's own knob — EngineLimits::idle_timeout is the server's
    // and is not consulted here.
    std::chrono::milliseconds idle_timeout{120000};
    // Budget for one convenience request (head + body, both directions).
    // 0 = no limit. The session layer leaves the budget to its caller.
    std::chrono::milliseconds request_timeout{60000};

    // Protocol limits. The HTTP/2 receive window is larger than the server's
    // default: a client is usually the one waiting on data, so a bigger window
    // keeps a fast link busy while still bounding per-stream memory (credit is
    // returned only as the application consumes, see h2_client.h).
    EngineLimits limits{.h2_initial_window = 1 << 20};

    // Idle pooled connections kept per target, and how long an idle one stays
    // usable (a tunnelled or lightly loaded peer closes idle connections on its
    // own schedule; handing out a half-closed one only costs a failed request).
    std::size_t max_idle_per_target{8};
    std::chrono::milliseconds idle_pool_ttl{20000};

    bool tcp_nodelay{true};
    bool tcp_keepalive{false};

    // Automatic response decompression. When on, the client advertises the
    // encodings below and decodes whatever comes back, so a caller always sees
    // the original bytes. Off by default: it changes what goes out on the wire,
    // so it is opt-in. Requires SIMPLE_HTTP_ENABLE_COMPRESSION for the codecs -
    // without it this is a no-op and no Accept-Encoding is sent.
    bool auto_decompress{false};
    // Encodings advertised when auto_decompress is on, most preferred first.
    // Only the ones this build can actually decode are sent; a caller that sets
    // accept-encoding itself keeps full control (nothing is injected).
    std::vector<std::string> accept_encodings{"br", "gzip"};

    // Optional name-resolution override: return the endpoints to try, in order.
    // Lets an application plug in its own DNS (cache, DoH, …) instead of the
    // built-in asio resolver. Empty = asio::ip::tcp::resolver.
    std::function<asio::awaitable<std::pair<error_code, std::vector<asio::ip::tcp::endpoint>>>(std::string host,
                                                                                               std::string port)>
        resolve;

    // Applied to the freshly-connected socket, before TLS starts.
    std::function<void(asio::ip::tcp::socket&)> socket_setup;
};

// Failures the client itself decides on. Everything the transport reports stays
// an asio error_code.
enum class client_errc : int {
    bad_url = 1,             // malformed URL (parse_url)
    unsupported_scheme,      // scheme is neither http nor https
    protocol_error,          // peer violated the protocol (malformed head/frame)
    version_not_negotiated,  // the required HTTP version was not available
    header_too_large,        // head exceeded EngineLimits::max_header_bytes
    body_too_large,          // body exceeded the caller's cap
    body_decode_failed,      // the body was compressed but did not decode
    session_busy,            // HTTP/1.1 session already has an exchange in flight
    session_closed,          // the session/connection is gone
    body_not_streaming,      // write() on a request whose body was sent up front
    too_many_streams,        // the peer's MAX_CONCURRENT_STREAMS is exhausted
    stream_reset,            // the peer reset the stream (RST_STREAM)
    stream_refused,          // the peer did not process the request (REFUSED_STREAM, or a GOAWAY covering it)
    goaway,                  // the peer is draining the connection (GOAWAY)
    connect_timeout,
    request_timeout,
    invalid_spec,  // the caller's RequestSpec contradicts itself (body + stream_body)
};

}  // namespace simple_http

// Lets `error_code ec = client_errc::bad_url;` work through the library's
// boost::system::error_code. This must be visible before the first conversion —
// boost's converting constructor is gated on the trait, and a non-template
// function's body is compiled where it is defined — so it sits here rather than
// at the end of the header.
namespace boost::system {

template <>
struct is_error_code_enum<simple_http::client_errc> {
    static const bool value = true;
};

}  // namespace boost::system

namespace simple_http {

class ClientErrorCategory : public boost::system::error_category {
  public:
    const char* name() const noexcept override {
        return "simple_http.client";
    }

    std::string message(int ev) const override {
        switch (static_cast<client_errc>(ev)) {
            case client_errc::bad_url:
                return "malformed URL";
            case client_errc::unsupported_scheme:
                return "unsupported URL scheme (expected http or https)";
            case client_errc::protocol_error:
                return "peer violated the HTTP protocol";
            case client_errc::version_not_negotiated:
                return "the required HTTP version was not negotiated";
            case client_errc::header_too_large:
                return "response head exceeded the configured limit";
            case client_errc::body_too_large:
                return "response body exceeded the configured limit";
            case client_errc::session_busy:
                return "HTTP/1.1 session already has an exchange in flight";
            case client_errc::session_closed:
                return "the session is closed";
            case client_errc::body_not_streaming:
                return "the request body is not streamable here (already sent or sent up front)";
            case client_errc::too_many_streams:
                return "the peer's concurrent-stream limit is exhausted";
            case client_errc::stream_reset:
                return "the peer reset the stream";
            case client_errc::stream_refused:
                return "the peer did not process the request";
            case client_errc::goaway:
                return "the peer is draining the connection (GOAWAY)";
            case client_errc::connect_timeout:
                return "connection attempt timed out";
            case client_errc::request_timeout:
                return "request timed out";
            case client_errc::invalid_spec:
                return "the request spec sets both body and stream_body";
            default:
                return "unknown client error";
        }
    }
};

inline const boost::system::error_category& client_category() {
    static const ClientErrorCategory instance;
    return instance;
}

inline error_code make_error_code(client_errc e) noexcept {
    return error_code{static_cast<int>(e), client_category()};
}

// Whether a failed request may be sent again on a fresh connection without
// risking a second side effect. The peer provably did not process it: the
// stream was refused before any response, or a GOAWAY covered it. Everything
// else — a truncation, a reset after the response started, a timeout — might
// have been acted on, so retrying is the caller's decision, not ours.
inline bool is_retryable(const error_code& ec) noexcept {
    return ec == make_error_code(client_errc::stream_refused);
}



}  // namespace simple_http
