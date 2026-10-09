#pragma once

// EngineLimits: all runtime-tunable protocol-engine parameters in one place.
//
// These used to be hard-coded constants inside the h1/h2 engines. Collecting
// them here lets ServerConfig expose them as a single knob that flows unchanged
// down to every engine (via net/connection's serve_* helpers), and lets new
// tunables be added without touching engine or serve_* signatures.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

#include "compression.h"

namespace simple_http {

struct EngineLimits {
    // --- shared ---
    // Idle timeout: a connection with no bytes for this long is closed (Slowloris
    // defense). Applies to both HTTP/1.x and HTTP/2.
    std::chrono::seconds idle_timeout{120};

    // --- HTTP/1.x ---
    // Maximum size of a request head (request line + all header fields).
    // Exceeding it is answered with 431 Request Header Fields Too Large.
    std::size_t max_header_bytes{64 * 1024};
    // Maximum request body size (Content-Length or accumulated chunked).
    // Exceeding it (or a Content-Length that overflows) is answered with 413
    // Payload Too Large.
    std::size_t max_body_bytes{64ull * 1024 * 1024};

    // --- WebSocket ---
    // Per-connection cap on inbound messages the engine read-aheads while the
    // handler is busy (typically blocked writing because the peer stopped
    // reading). The reader keeps draining the socket into a bounded queue up to
    // this many bytes — payload plus a per-message overhead — then stops; read()
    // making room resumes it. This is what keeps a peer that pipelines a burst
    // without reading from deadlocking the connection, so it should comfortably
    // exceed the largest burst a client sends while it is not reading. 0 keeps
    // the library default (4 MiB). Memory cost is bounded at roughly this per
    // connection only while the application is not consuming.
    std::size_t ws_read_ahead_bytes{4 * 1024 * 1024};
    // Negotiate permessage-deflate (RFC 7692) for WebSocket, compressing frames
    // in both directions. Off by default: it adds per-connection CPU cost, so a
    // server opts in. A WebSocket handle can further tune a single connection
    // at runtime (WebSocket::enable_write_compression / set_compression_level).
    bool ws_compression{false};

    // --- HTTP/2 ---
    // SETTINGS_MAX_CONCURRENT_STREAMS advertised to the peer.
    std::uint32_t h2_max_concurrent_streams{200};
    // Largest DATA payload we emit per frame, and our advertised
    // SETTINGS_MAX_FRAME_SIZE (RFC 7540 §6.5.2 floor is 16384).
    std::uint32_t h2_max_frame_size{16384};
    // Our advertised initial receive window (SETTINGS_INITIAL_WINDOW_SIZE), per
    // stream and connection. Larger trades memory for throughput on fast links.
    std::int32_t h2_initial_window{65535};
    // Advertise SETTINGS_ENABLE_CONNECT_PROTOCOL (RFC 8441), which lets a client
    // open a WebSocket over HTTP/2 with an extended CONNECT (`:protocol:
    // websocket`). On by default; a route still has to match, and a stream that
    // asks for a path with no ws route is answered 404. Set false to keep the
    // connection on plain HTTP/2 requests only.
    bool h2_enable_connect_protocol{true};

    // Decoded header-list size we accept and advertise as
    // SETTINGS_MAX_HEADER_LIST_SIZE (name + value + 32 per field, RFC 9113
    // §6.5.2). Independent of max_header_bytes, which bounds the *compressed*
    // block: compression means a tiny block can decode to a much larger list, so
    // the two are different questions. 0 (the default) means "follow
    // max_header_bytes", so tuning the header-size limit scales both instead of
    // leaving a second hard-coded number behind; set an explicit value to
    // decouple them. Exceeding it is a stream error (ENHANCE_YOUR_CALM).
    std::size_t h2_max_header_list_size{0};

    // The decoded header-list bound actually applied (see above).
    [[nodiscard]] std::size_t effective_max_header_list_size() const {
        return h2_max_header_list_size != 0 ? h2_max_header_list_size : max_header_bytes;
    }

    // --- HTTP/3 advertisement ---
    // Whether to advertise this origin's HTTP/3 endpoint in its HTTP/1.x,
    // HTTP/2 and HTTP/3 responses (RFC 7838). Alt-Svc is the only way a
    // browser learns the QUIC endpoint exists — it never guesses, so the first
    // visit to an origin always arrives over TCP — and leaving this off keeps
    // browsers on TCP while clients that ask for HTTP/3 explicitly (curl, a
    // conformance suite) still reach it.
    //
    // The port is deliberately not a setting: it is always the QUIC listener's
    // own (ServerConfig::quic), which is what the Server renders below.
    // Advertising a port nobody listens on is a configuration mistake, not a
    // feature, and requiring the number to be repeated here only invited the
    // two to drift — including the silent failure of a QUIC listener that no
    // response ever mentions.
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
    bool h3_alt_svc{true};
    std::uint32_t h3_alt_svc_max_age{86400};

    // Render the Alt-Svc value for a QUIC listener on `quic_port`, or empty when
    // there is nothing to advertise. Called by the Server while it assembles its
    // configuration. The macro is read here and nowhere else, so a build that
    // cannot serve HTTP/3 has nothing to render.
    [[nodiscard]] std::string render_alt_svc(std::uint16_t quic_port) const {
        if (!h3_alt_svc || quic_port == 0)
            return {};
        return std::format("h3=\":{}\"; ma={}", quic_port, h3_alt_svc_max_age);
    }

    // The rendered field, written verbatim into every HTTP/1.x and HTTP/2
    // response head. Filled in by the Server, and only when there is a QUIC
    // listener — a build without HTTP/3 never sets it.
    std::string alt_svc;
#endif

    // The Alt-Svc field value, or empty when there is nothing to advertise.
    //
    // The engines that send the header call this unconditionally and never
    // branch on how the value was produced. HTTP/3 sends it too — on a port
    // that differs from the one the connection arrived on it is real
    // information, and on the listener's own port it is harmless — so all three
    // engines ask.
    [[nodiscard]] const std::string &alt_svc_value() const {
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
        return alt_svc;
#else
        static const std::string none{};
        return none;
#endif
    }
};

} // namespace simple_http
