#pragma once

// HPACK header-block decoder (RFC 7541), framework-free.
//
// The decoding *algorithm* is adapted from paozhu's http2parse::headertype1..4
// (vendor/httpserver/http2_parse.cpp): the same prefix-bit dispatch, HPACK
// integer decoding, Huffman string decoding and the dynamic-table maintenance
// (push_front + a fixed 255-entry cap). paozhu wrote decoded fields straight
// into its httppeer god-object; this version instead returns a plain list of
// (name, value) pairs and owns a per-connection dynamic table, so it depends
// only on the standard library and the ported Huffman codec.
//
// One HpackDecoder instance lives per HTTP/2 connection (the dynamic table is
// connection-scoped and shared across streams, per the spec).

#include <cstdint>
#include <list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hpack_encode.h"   // http2_header_static_table
#include "hpack_huffman.h"  // http_huffman_decode, HUFFMAN_OK

namespace simple_http::codec {

// A decoded header field. Names are as received (lowercased by the peer for
// HTTP/2); pseudo-headers keep their leading ':'.
struct HpackHeader {
    std::string name;
    std::string value;
};

class HpackDecoder {
  public:
    HpackDecoder() = default;

    // Decodes one complete header block into `out`. Returns true on success;
    // on a malformed block returns false and sets last_error().
    bool decode(std::string_view block, std::vector<HpackHeader>& out);

    // Convenience: decode into a fresh vector.
    std::vector<HpackHeader> decode(std::string_view block) {
        std::vector<HpackHeader> out;
        decode(block, out);
        return out;
    }

    unsigned int last_error() const { return m_error; }

    // Dynamic-table maximum entry count (paozhu uses a fixed 255-entry cap
    // rather than the RFC byte-size accounting).
    static constexpr std::size_t kDynamicTableMaxEntries = 255;

  private:
    // HPACK integer decode with an N-bit prefix (RFC 7541 §5.1). `prefix` is the
    // already-masked prefix value; advances `pos`. Returns the full integer.
    bool decode_integer(std::string_view block, std::size_t& pos, unsigned int prefix_bits, uint64_t& out);

    // HPACK string decode (RFC 7541 §5.2): length-prefixed, optionally Huffman.
    bool decode_string(std::string_view block, std::size_t& pos, std::string& out);

    // Resolves a table index (1-based) to a name/value pair. Index into the
    // static table if < 62, otherwise into the dynamic table.
    bool resolve_index(uint64_t index, std::string& name, std::string& value) const;

    void dynamic_insert(std::string name, std::string value);

    std::list<std::pair<std::string, std::string>> m_dynamic;  // most-recent at front
    unsigned int m_error = 0;
};

// --- inline definitions (algorithm adapted from paozhu http2parse::headertype1..4) ---

namespace detail {
// Number of entries in the static table (indices 1..61; slot 0 is a sentinel).
inline constexpr uint64_t kStaticTableCount = 61;
}  // namespace detail

inline bool HpackDecoder::decode_integer(std::string_view block, std::size_t& pos, unsigned int prefix_bits,
                                         uint64_t& out) {
    const unsigned int max_prefix = (1u << prefix_bits) - 1u;
    // The prefix byte was already read by the caller at pos-1; recompute it.
    unsigned char first = static_cast<unsigned char>(block[pos - 1]);
    uint64_t value = first & max_prefix;
    if (value < max_prefix) {
        out = value;
        return true;
    }
    // Continuation bytes: 7 bits each, little-endian, high bit = "more".
    unsigned int shift = 0;
    for (;;) {
        if (pos >= block.size()) {
            m_error = 40140;
            return false;
        }
        unsigned char b = static_cast<unsigned char>(block[pos++]);
        value += static_cast<uint64_t>(b & 0x7F) << shift;
        shift += 7;
        if ((b & 0x80) == 0) break;
        if (shift > 62) {  // guard against overflow / malformed input
            m_error = 40141;
            return false;
        }
    }
    out = value;
    return true;
}

inline bool HpackDecoder::decode_string(std::string_view block, std::size_t& pos, std::string& out) {
    if (pos >= block.size()) {
        m_error = 40142;
        return false;
    }
    unsigned char first = static_cast<unsigned char>(block[pos]);
    bool huffman = (first & 0x80) != 0;
    pos += 1;  // consume prefix byte so decode_integer can re-read it at pos-1
    uint64_t len = 0;
    if (!decode_integer(block, pos, 7, len)) {
        return false;
    }
    if (pos + len > block.size()) {
        m_error = 40143;
        return false;
    }
    if (huffman) {
        unsigned char state = 0;
        if (http_huffman_decode(&state, reinterpret_cast<unsigned char*>(const_cast<char*>(block.data() + pos)),
                                static_cast<std::size_t>(len), out, 1) != HUFFMAN_OK) {
            m_error = 40144;
            return false;
        }
    } else {
        out.append(block.data() + pos, static_cast<std::size_t>(len));
    }
    pos += static_cast<std::size_t>(len);
    return true;
}

inline bool HpackDecoder::resolve_index(uint64_t index, std::string& name, std::string& value) const {
    if (index == 0) {
        return false;
    }
    if (index <= detail::kStaticTableCount) {
        name = http2_header_static_table[index].key;
        value = http2_header_static_table[index].value;
        return true;
    }
    uint64_t dyn = index - detail::kStaticTableCount - 1;  // 0-based into dynamic table
    uint64_t j = 0;
    for (const auto& entry : m_dynamic) {
        if (j == dyn) {
            name = entry.first;
            value = entry.second;
            return true;
        }
        j++;
    }
    return false;
}

inline void HpackDecoder::dynamic_insert(std::string name, std::string value) {
    m_dynamic.push_front({std::move(name), std::move(value)});
    if (m_dynamic.size() > kDynamicTableMaxEntries) {
        m_dynamic.pop_back();
    }
}

inline bool HpackDecoder::decode(std::string_view block, std::vector<HpackHeader>& out) {
    m_error = 0;
    std::size_t pos = 0;
    while (pos < block.size()) {
        unsigned char c = static_cast<unsigned char>(block[pos]);

        if (c & 0x80) {
            // 6.1 Indexed Header Field: 1xxxxxxx
            pos += 1;
            uint64_t index = 0;
            if (!decode_integer(block, pos, 7, index)) return false;
            std::string name, value;
            if (!resolve_index(index, name, value)) {
                m_error = 40150;
                return false;
            }
            out.push_back({std::move(name), std::move(value)});
        } else if (c & 0x40) {
            // 6.2.1 Literal Header Field with Incremental Indexing: 01xxxxxx
            pos += 1;
            uint64_t index = 0;
            if (!decode_integer(block, pos, 6, index)) return false;
            std::string name, value;
            if (index != 0) {
                std::string dummy;
                if (!resolve_index(index, name, dummy)) {
                    m_error = 40151;
                    return false;
                }
            } else {
                if (!decode_string(block, pos, name)) return false;
            }
            if (!decode_string(block, pos, value)) return false;
            out.push_back({name, value});
            dynamic_insert(std::move(name), std::move(value));
        } else if (c & 0x20) {
            // 6.3 Dynamic Table Size Update: 001xxxxx — read and ignore the size
            // (paozhu uses a fixed entry-count cap and does not honor byte sizes).
            pos += 1;
            uint64_t size = 0;
            if (!decode_integer(block, pos, 5, size)) return false;
        } else {
            // 6.2.2 (0000xxxx) without indexing / 6.2.3 (0001xxxx) never indexed:
            // both use a 4-bit index prefix and are not added to the dynamic table.
            pos += 1;
            uint64_t index = 0;
            if (!decode_integer(block, pos, 4, index)) return false;
            std::string name, value;
            if (index != 0) {
                std::string dummy;
                if (!resolve_index(index, name, dummy)) {
                    m_error = 40152;
                    return false;
                }
            } else {
                if (!decode_string(block, pos, name)) return false;
            }
            if (!decode_string(block, pos, value)) return false;
            out.push_back({std::move(name), std::move(value)});
        }
    }
    return true;
}

}  // namespace simple_http::codec
