#pragma once

#include <cstdint>

namespace simple_http {

// Minimum accepted TLS version, shared by the server (TlsConfig) and the client
// (TlsClientConfig).
//
//   * Server: defaults to 1.3. Set TlsConfig::min_version = Tls12 to also accept
//     TLS 1.2 (for clients that have not moved to 1.3 yet). 1.0/1.1 are never
//     accepted.
//   * Client: defaults to 1.2, because it must still reach peers that only
//     speak 1.2; set Tls13 to refuse them.
enum class TlsMinVersion : std::uint8_t {
    Tls12 = 0,
    Tls13 = 1,
};

} // namespace simple_http
