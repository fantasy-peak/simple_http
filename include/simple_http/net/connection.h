#pragma once

// Protocol selection for a plaintext listener.
//
// Http1 serves HTTP/1.x only, Http2 requires the HTTP/2 connection preface
// (prior knowledge), and Both sniffs the preface: a matching prefix goes to
// the h2 engine, anything else starts as HTTP/1.x and may still upgrade to
// h2c.

namespace simple_http {

enum class PlaintextProtocols {
    Http1, // parse every connection as HTTP/1.x
    Http2, // require the HTTP/2 preface (prior knowledge)
    Both,  // sniff the preface; h1 otherwise, with h2c Upgrade allowed
};

} // namespace simple_http
