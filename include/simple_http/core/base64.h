#pragma once

// base64url (RFC 4648 §5, URL/filename-safe alphabet, no padding) codec.
// Used to decode the HTTP2-Settings header carried by an h2c upgrade request.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace simple_http {

inline constexpr char base64_url_alphabet[] = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V',
    'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r',
    's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '-', '_'};

inline std::string base64_url_encode(std::string_view in) {
    std::string out;
    int val = 0;
    int bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out.push_back(base64_url_alphabet[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) {
        out.push_back(base64_url_alphabet[((val << 8) >> (bits + 8)) & 0x3F]);
    }
    return out;
}

// Standard base64 alphabet (RFC 4648 §4). Used for the WebSocket handshake
// Sec-WebSocket-Accept value (SHA1 digest -> base64 with padding).
inline constexpr char base64_std_alphabet[] = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V',
    'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r',
    's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/'};

// Standard base64 encode with '=' padding (RFC 4648 §4). Accepts arbitrary
// bytes (e.g. a SHA1 digest), so the input is a byte view.
inline std::string base64_encode(std::string_view in) {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= in.size()) {
        std::uint32_t n = (static_cast<unsigned char>(in[i]) << 16) |
                          (static_cast<unsigned char>(in[i + 1]) << 8) |
                          static_cast<unsigned char>(in[i + 2]);
        out.push_back(base64_std_alphabet[(n >> 18) & 0x3F]);
        out.push_back(base64_std_alphabet[(n >> 12) & 0x3F]);
        out.push_back(base64_std_alphabet[(n >> 6) & 0x3F]);
        out.push_back(base64_std_alphabet[n & 0x3F]);
        i += 3;
    }
    std::size_t rem = in.size() - i;
    if (rem == 1) {
        std::uint32_t n = static_cast<unsigned char>(in[i]) << 16;
        out.push_back(base64_std_alphabet[(n >> 18) & 0x3F]);
        out.push_back(base64_std_alphabet[(n >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        std::uint32_t n = (static_cast<unsigned char>(in[i]) << 16) |
                          (static_cast<unsigned char>(in[i + 1]) << 8);
        out.push_back(base64_std_alphabet[(n >> 18) & 0x3F]);
        out.push_back(base64_std_alphabet[(n >> 12) & 0x3F]);
        out.push_back(base64_std_alphabet[(n >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

inline std::string base64_url_decode(std::string_view in) {
    // Reverse lookup table: character -> 6-bit value, or -1 if not in alphabet.
    std::array<int, 256> rev;
    rev.fill(-1);
    for (int i = 0; i < 64; ++i) {
        rev[static_cast<unsigned char>(base64_url_alphabet[i])] = i;
    }

    std::string out;
    int val = 0;
    int bits = -8;
    for (unsigned char c : in) {
        int mapped = rev[c];
        if (mapped == -1) {
            break;  // stop at the first character outside the alphabet
        }
        val = (val << 6) + mapped;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

}  // namespace simple_http
