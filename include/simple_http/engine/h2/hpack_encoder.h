#pragma once

// HPACK response header-block encoder (RFC 7541), framework-free.
//
// This is a fresh, minimal encoder built on the two primitives that are
// byte-for-byte correct: the Huffman codec (`codec::http_huffman_encode`) and
// the HPACK static table (`codec::http2_header_static_table`). It does not
// reuse the ported `make_http2_headers_item*` builders that used to live beside
// that table — their varint continuation-byte math was wrong for values >= 127
// and they double-emitted the length prefix — and those have since been deleted
// (hpack_static_table.h is all that remains of that file).
//
// Strategy: every header field is encoded as "Literal Header Field without
// Indexing" (RFC 7541 §6.2.2), with the value Huffman-coded and the name taken
// from the static table when it is there (one or two octets instead of the
// ~6-13 a Huffman-coded name costs). This is always valid HPACK, keeps the
// encoder itself tiny and correct, and needs no encoder-side dynamic table (the
// peer's dynamic table is simply never populated by us, which HPACK permits —
// indexing is an optimization, not a requirement). `:status` additionally uses
// the indexed static table entries for the common codes
// (200/204/206/304/400/404/500) to keep typical responses a single byte for that
// field, matching what most HTTP/2 servers emit.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "hpack_huffman.h"
#include "hpack_static_table.h"

namespace simple_http::codec {

// Appends an HPACK integer (RFC 7541 §5.1) with the given prefix bits and
// leading pattern already set in `prefix_byte` (e.g. 0x00 for "without
// indexing", with the low `prefix_bits` bits left at 0 for the value).
inline void hpack_append_integer(std::string &out, unsigned char prefix_byte, unsigned int prefix_bits,
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
inline void hpack_append_string(std::string &out, std::string_view value) {
    // The Huffman buffer is scratch space, not output: encoding it into a fresh
    // local string cost one malloc/free per header *field* (Date, Content-Type,
    // Content-Length, Server, ...), which is several allocations per response.
    // A thread_local keeps its capacity across calls, and the encode itself
    // never calls back into this function, so one buffer per thread is enough.
    static thread_local std::string huff;
    huff.clear();
    http_huffman_encode(reinterpret_cast<unsigned char *>(const_cast<char *>(value.data())),
                        static_cast<unsigned int>(value.size()), huff);
    hpack_append_integer(out, 0x80, 7, huff.size());
    out.append(huff);
}

// Appends a literal field whose *name* comes from the table (RFC 7541 §6.2.2,
// index prefix 4 bits): the name costs one byte instead of a Huffman-coded
// string, which is what makes a head mostly table references.
inline void hpack_append_literal_indexed_name(std::string &out, uint64_t name_index, std::string_view value) {
    hpack_append_integer(out, 0x00, 4, name_index);
    hpack_append_string(out, value);
}

// Field names we can reference by static-table index instead of spelling out.
// A name costs one or two octets as a reference where the literal costs a
// Huffman-coded string (11 octets for "content-type"), and encoding it is work
// skipped. The static table is not fully sorted (RFC 7541 Appendix A lists
// "accept-ranges" before "accept"), so this is a linear scan over a short list
// rather than a search in the table itself.
inline constexpr std::pair<std::string_view, std::uint64_t> kHpackIndexedNames[] = {
    {":status", 8},
    {"accept-ranges", 18},
    {"allow", 22},
    {"cache-control", 24},
    {"content-disposition", 25},
    {"content-encoding", 26},
    {"content-language", 27},
    {"content-length", 28},
    {"content-range", 30},
    {"content-type", 31},
    {"date", 33},
    {"etag", 34},
    {"last-modified", 44},
    {"location", 46},
    {"retry-after", 53},
    {"server", 54},
    {"set-cookie", 55},
    {"strict-transport-security", 56},
    {"vary", 59},
    {"www-authenticate", 61},
};

// Compile-time guard. An index that does not name the field it claims would
// rename that header on the wire for every peer - a silent, total correctness
// failure - so the pairing with the RFC table is checked here instead of by
// review.
constexpr bool hpack_indexed_names_match_static_table() {
    for (const auto &[name, index] : kHpackIndexedNames) {
        if (index >= http2_header_static_table.size() || http2_header_static_table[index].key != name) {
            return false;
        }
    }
    return true;
}
static_assert(hpack_indexed_names_match_static_table(), "kHpackIndexedNames disagrees with the HPACK static table");

// The static-table index for `name`, or 0 when the name is not in that list.
inline std::uint64_t hpack_name_index(std::string_view name) {
    for (const auto &[n, index] : kHpackIndexedNames) {
        if (n == name) {
            return index;
        }
    }
    return 0;
}

// Appends one header field as "Literal Header Field without Indexing"
// (RFC 7541 §6.2.2), with the name taken from the static table when it is there
// and the value Huffman-coded.
inline void hpack_append_literal(std::string &out, std::string_view name, std::string_view value) {
    if (const std::uint64_t index = hpack_name_index(name); index != 0) {
        hpack_append_literal_indexed_name(out, index, value);
        return;
    }
    out.push_back('\x00'); // index=0 (literal name follows), "without indexing" pattern
    hpack_append_string(out, name);
    hpack_append_string(out, value);
}

// Appends an indexed header field (RFC 7541 §6.1): one byte for any entry in
// the static table whose exact name *and* value we are sending (e.g. index 2
// for
// `:method: GET`).
inline void hpack_append_indexed(std::string &out, uint64_t index) { hpack_append_integer(out, 0x80, 7, index); }

// Appends `:status` using the static-table index when the code is one of the
// entries HPACK reserves (RFC 7541 Appendix A), else falls back to a literal.
inline void hpack_append_status(std::string &out, int status) {
    switch (status) {
    case 200:
        out.push_back(static_cast<char>(0x88));
        return;
    case 204:
        out.push_back(static_cast<char>(0x89));
        return;
    case 206:
        out.push_back(static_cast<char>(0x8A));
        return;
    case 304:
        out.push_back(static_cast<char>(0x8B));
        return;
    case 400:
        out.push_back(static_cast<char>(0x8C));
        return;
    case 404:
        out.push_back(static_cast<char>(0x8D));
        return;
    case 500:
        out.push_back(static_cast<char>(0x8E));
        return;
    default:
        hpack_append_literal(out, ":status", std::to_string(status));
        return;
    }
}

} // namespace simple_http::codec
