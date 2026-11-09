#pragma once

// Protocol selection for a plaintext listener (C++17 port).
//
// The C++23 implementation sniffs the HTTP/2 connection preface and dispatches
// to the h2 engine. HTTP/2 is not ported yet, so this first version serves
// HTTP/1.x on every plaintext listener; the enum is kept so listener
// configuration and fixtures keep compiling, and Http2-only is rejected at
// startup rather than silently answered in the wrong protocol.

namespace simple_http {

enum class PlaintextProtocols {
    Http1, // parse every connection as HTTP/1.x
    Http2, // require the HTTP/2 preface (not available in this port yet)
    Both,  // the sniffing default: HTTP/1.x until the h2 phase lands
};

} // namespace simple_http
