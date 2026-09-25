#pragma once

// HPACK header-block decoder (RFC 7541), framework-free.
//
// The decoding algorithm: the RFC 7541 prefix-bit dispatch (§6), HPACK integer
// decoding, Huffman string decoding and the dynamic-table maintenance (newest
// entry first). It returns a plain list of (name, value) pairs and owns a
// per-connection dynamic table, so it depends only on the standard library and
// the Huffman codec.
//
// The dynamic table is accounted in *bytes* (RFC 7541 §4.1: each entry costs
// name + value + 32) against the size this decoder advertised in
// SETTINGS_HEADER_TABLE_SIZE, and it honours a Dynamic Table Size Update (§6.3).
// That matters beyond tidiness: an entry larger than the table must empty the
// table and *not* be inserted (§4.4), so a peer that sends a big cookie and we
// insert it anyway would shift every later dynamic index and silently hand back
// the wrong header values. (Byte accounting also makes the previous fixed
// 255-entry cap unnecessary; it is kept only as a belt-and-braces bound.)
//
// One HpackDecoder instance lives per HTTP/2 connection (the dynamic table is
// connection-scoped and shared across streams, per the spec).

#include <algorithm>
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

    // The size advertised in SETTINGS_HEADER_TABLE_SIZE: the largest dynamic
    // table the peer may keep. A size update above it is a compression error
    // (RFC 7541 §4.2), so the value must be set before decoding begins.
    void set_max_table_size(std::size_t bytes) {
        m_advertised_max = bytes;
        m_max_size = bytes;
    }

    std::size_t max_table_size() const { return m_advertised_max; }
    // Bytes currently occupied by the dynamic table.
    std::size_t dynamic_table_size() const { return m_size; }

    // Dynamic-table entry cap, a belt-and-braces bound on top of the RFC byte
    // accounting (an entry can never be smaller than 32 bytes, so a legitimate
    // table cannot exceed the size limit / 32 entries).
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

    // Drops the oldest entries until the table fits in `m_max_size` (§4.3).
    void evict_to_fit();

    std::list<std::pair<std::string, std::string>> m_dynamic;  // most-recent at front
    std::size_t m_max_size = 4096;         // current table budget (may be lowered by a size update)
    std::size_t m_advertised_max = 4096;   // what we advertised; a size update may not exceed it
    std::size_t m_size = 0;                // bytes occupied (name + value + 32 per entry)
    unsigned int m_error = 0;
};

// --- inline definitions ---

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
    // RFC 7541 §4.1: an entry costs 32 bytes plus the name and value lengths.
    const std::size_t entry_size = name.size() + value.size() + 32;
    // §4.4: an entry larger than the whole table empties the table and is *not*
    // inserted. Skipping this is what makes a later dynamic index point at the
    // wrong entry, so the peer and we would disagree silently.
    if (entry_size > m_max_size) {
        m_dynamic.clear();
        m_size = 0;
        return;
    }
    m_size += entry_size;
    m_dynamic.push_front({std::move(name), std::move(value)});
    evict_to_fit();
}

inline void HpackDecoder::evict_to_fit() {
    while ((!m_dynamic.empty() && m_size > m_max_size) || m_dynamic.size() > kDynamicTableMaxEntries) {
        const auto& oldest = m_dynamic.back();
        const std::size_t entry_size = oldest.first.size() + oldest.second.size() + 32;
        m_size -= std::min(m_size, entry_size);
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
            // 6.3 Dynamic Table Size Update: 001xxxxx. The encoder tells us how
            // much of the table it will use; anything above what we advertised
            // is a compression error (§4.2), and lowering it evicts entries
            // immediately (§4.3).
            pos += 1;
            uint64_t size = 0;
            if (!decode_integer(block, pos, 5, size)) return false;
            if (size > m_advertised_max) {
                m_error = 40160;
                return false;
            }
            m_max_size = static_cast<std::size_t>(size);
            evict_to_fit();
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
