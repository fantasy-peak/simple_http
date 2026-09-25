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

#include "compression.h"

namespace simple_http {

struct EngineLimits {
    // --- shared ---
    // Idle timeout: a connection with no bytes for this long is closed (Slowloris
    // defense). Applies to both HTTP/1.x and HTTP/2.
    std::chrono::seconds idle_timeout{120};

    // --- HTTP/1.x ---
    // Maximum size of a request head (request line + all header fields). Exceeding
    // it is answered with 431 Request Header Fields Too Large.
    std::size_t max_header_bytes{64 * 1024};
    // Maximum request body size (Content-Length or accumulated chunked). Exceeding
    // it (or a Content-Length that overflows) is answered with 413 Payload Too Large.
    std::size_t max_body_bytes{64ull * 1024 * 1024};

    // --- HTTP/2 ---
    // SETTINGS_MAX_CONCURRENT_STREAMS advertised to the peer.
    std::uint32_t h2_max_concurrent_streams{200};
    // Largest DATA payload we emit per frame, and our advertised SETTINGS_MAX_FRAME_SIZE
    // (RFC 7540 §6.5.2 floor is 16384).
    std::uint32_t h2_max_frame_size{16384};
    // Our advertised initial receive window (SETTINGS_INITIAL_WINDOW_SIZE), per
    // stream and connection. Larger trades memory for throughput on fast links.
    std::int32_t h2_initial_window{65535};

    // --- response compression (off by default) ---
    // Lives here rather than on ServerConfig so that it reaches the engines by
    // the same path every other tunable takes; see the note at the top of this
    // file.
    CompressionConfig compression{};
};

}  // namespace simple_http
