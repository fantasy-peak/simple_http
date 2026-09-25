#pragma once

// HPACK response header-block encoder (RFC 7541), framework-free.
//
// This is a fresh, minimal encoder — it does not reuse the ported
// `make_http2_headers_item*` helpers in hpack_encode.h (their varint
// continuation-byte math is wrong for values >= 127 and they double-emit the
// length prefix), only the two primitives that are byte-for-byte correct:
// the Huffman codec (`codec::http_huffman_encode`) and the HPACK static table
// (`codec::http2_header_static_table`).
//
// Strategy: every header field is encoded as "Literal Header Field without
// Indexing" (RFC 7541 §6.2.2), with both name and value Huffman-coded. This is
// always valid HPACK, keeps the encoder itself tiny and correct, and needs no
// encoder-side dynamic table (the peer's dynamic table is simply never
// populated by us, which HPACK permits — indexing is an optimization, not a
// requirement). The one exception is `:status`, which uses the indexed static
// table entries for the common codes (200/204/206/304/400/404/500) to keep
// typical responses a single byte for that field, matching what most HTTP/2
// servers emit.

#include <cstdint>
#include <string>
#include <string_view>

#include "hpack_huffman.h"

namespace simple_http::codec {

// Appends an HPACK integer (RFC 7541 §5.1) with the given prefix bits and
// leading pattern already set in `prefix_byte` (e.g. 0x00 for "without
// indexing", with the low `prefix_bits` bits left at 0 for the value).
inline void hpack_append_integer(std::string& out, unsigned char prefix_byte, unsigned int prefix_bits,
                                 uint64_t value) {
    const uint64_t max_prefix = (1ull << prefix_bits) - 1ull;
    if (value < max_prefix) {
        out.push_back(static_cast<char>(prefix_byte | static_cast<unsigned char>(value)));
        return;
    }
    out.push_back(static_cast<char>(prefix_byte | static_cast<unsigned char>(max_prefix)));
    uint64_t remaining = value - max_prefix;
    while (remaining >= 0x80) {
        out.push_back(static_cast<char>((remaining & 0x7F) | 0x80));
        remaining >>= 7;
    }
    out.push_back(static_cast<char>(remaining));
}

// Appends an HPACK string literal (RFC 7541 §5.2), always Huffman-coded (the
// Huffman table never expands ASCII header text, so this is never larger than
// the raw encoding and is simpler to always take).
inline void hpack_append_string(std::string& out, std::string_view value) {
    std::string huff;
    http_huffman_encode(reinterpret_cast<unsigned char*>(const_cast<char*>(value.data())),
                        static_cast<unsigned int>(value.size()), huff);
    hpack_append_integer(out, 0x80, 7, huff.size());
    out.append(huff);
}

// Appends one header field as "Literal Header Field without Indexing"
// (RFC 7541 §6.2.2: 0000 0000 prefix byte since the name is given as a
// literal, not a table index), with both name and value Huffman-coded.
inline void hpack_append_literal(std::string& out, std::string_view name, std::string_view value) {
    out.push_back('\x00');  // index=0 (literal name follows), "without indexing" pattern
    hpack_append_string(out, name);
    hpack_append_string(out, value);
}

// Appends an indexed header field (RFC 7541 §6.1): one byte for any entry in
// the static table whose exact name *and* value we are sending (e.g. index 2 for
// `:method: GET`).
inline void hpack_append_indexed(std::string& out, uint64_t index) {
    hpack_append_integer(out, 0x80, 7, index);
}

// Appends a literal field whose *name* comes from the table (RFC 7541 §6.2.2,
// index prefix 4 bits): the name costs one byte instead of a Huffman-coded
// string, which is what makes a request head mostly table references.
inline void hpack_append_literal_indexed_name(std::string& out, uint64_t name_index, std::string_view value) {
    hpack_append_integer(out, 0x00, 4, name_index);
    hpack_append_string(out, value);
}

// Appends `:status` using the static-table index when the code is one of the
// entries HPACK reserves (RFC 7541 Appendix A), else falls back to a literal.
inline void hpack_append_status(std::string& out, int status) {
    switch (status) {
        case 200: out.push_back(static_cast<char>(0x88)); return;
        case 204: out.push_back(static_cast<char>(0x89)); return;
        case 206: out.push_back(static_cast<char>(0x8A)); return;
        case 304: out.push_back(static_cast<char>(0x8B)); return;
        case 400: out.push_back(static_cast<char>(0x8C)); return;
        case 404: out.push_back(static_cast<char>(0x8D)); return;
        case 500: out.push_back(static_cast<char>(0x8E)); return;
        default:
            hpack_append_literal(out, ":status", std::to_string(status));
            return;
    }
}

}  // namespace simple_http::codec
