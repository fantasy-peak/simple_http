#pragma once

// permessage-deflate (RFC 7692): the one WebSocket compression extension this
// library negotiates. Compiled in unconditionally — it needs only zlib, which
// is a dependency of this library like boost.asio and OpenSSL. Whether a server
// actually negotiates it is a runtime setting (EngineLimits::ws_compression,
// per-connection toggles on the WebSocket handle).
//
// The file is split into two halves:
//   * negotiation: parse the client's Sec-WebSocket-Extensions offer and decide
//     what to accept; render the response value. Pure string machinery.
//   * the wire codec: per-direction DEFLATE streams with the RFC's frame-
//     boundary rules.
//
// Wire semantics (RFC 7692 §7.2):
//   * A message is deflated with Z_SYNC_FLUSH; the trailing flush marker
//     (0x00 0x00 0xff 0xff) is stripped before the frame goes out.
//   * The receiver re-feeds those 4 octets into its inflater (the sender
//     removed them, and they are what lets the decoder's LZ77 window stay
//     bit-identical to the encoder's — the marker decodes to no output, it only
//     terminates the block). This matches what the reference implementations
//     (the `websockets` library, browsers) do byte for byte.
//   * RSV1 is set on the first frame of a compressed message, never on
//     continuation frames, never on control frames.
//   * Context takeover: with takeover disabled for a direction, the stream is
//     reset at each message boundary instead of carrying its sliding window
//     across messages.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include <zlib.h>

namespace simple_http {

// The accepted permessage-deflate parameters, named from the *server's* point
// of view: server_* govern the outbound encoder, client_* the decoder of the
// peer's inbound messages. `enabled` is false when nothing was negotiated.
struct WsDeflateConfig {
    bool enabled = false;
    bool server_no_context_takeover = false; // we reset our encoder per message
    bool client_no_context_takeover = false; // we reset our decoder per message
    int server_window_bits = 15;             // our encoder's LZ77 window (8..15)
    int client_window_bits = 15;             // our decoder's window (8..15)
    // Negotiation bookkeeping, for rendering the response (RFC 7692 §7.1.2):
    // a client that offered server_max_window_bits must get an echoed value in
    // the response, and a client_max_window_bits that carried a value should.
    bool client_offered_server_window_bits = false;
    bool echo_client_window_bits = false;
};

// --- negotiation -------------------------------------------------------------

namespace detail {

// The one search within a Sec-WebSocket-Extensions offer we back: nothing is
// case-sensitive in there except the parameter values.
inline bool ws_ieq(std::string_view a, std::string_view name) {
    if (a.size() != name.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char x = a[i];
        const char y = name[i];
        if (x >= 'A' && x <= 'Z') {
            if (x - 'A' + 'a' != y) {
                return false;
            }
        } else if (x != y) {
            return false;
        }
    }
    return true;
}

// Trims RFC 7230 optional whitespace (SP / HTAB).
inline std::string_view trim_ows(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

// Whether one comma-separated part of Sec-WebSocket-Extensions names
// permessage-deflate (its parameters, if any, follow the first ';').
inline bool ws_part_is_permessage_deflate(std::string_view part) {
    const std::size_t semi = part.find(';');
    return ws_ieq(trim_ows(part.substr(0, semi)), "permessage-deflate");
}

} // namespace detail

// Parses the parameters of a single permessage-deflate offer part; see the
// definition for the param rules. Declared before ws_parse_deflate_offer so
// the multi-offer scan can call it.
std::optional<WsDeflateConfig> ws_parse_one_deflate_part(std::string_view part);

// Parses a client's Sec-WebSocket-Extensions value. Returns the config to
// enable when a valid permessage-deflate offer is present; std::nullopt when the
// extension must be declined — not offered, or no offer can be honoured (an
// unknown parameter, a repeated parameter, an out-of-range value, all of them
// malformed). RFC 7692 makes a declined extension a non-negotiation rather than
// a handshake failure: the upgrade proceeds, simply uncompressed.
//
// The header may list several *alternative* permessage-deflate offers,
// comma-separated, in the client's preference order (RFC 6455 §9.1); the
// server accepts at most one. This scans for the first offer it can process
// and accepts it — later alternatives are only consulted if the earlier ones
// are malformed.
inline std::optional<WsDeflateConfig> ws_parse_deflate_offer(std::string_view extensions) {
    std::size_t pos = 0;
    while (pos <= extensions.size()) {
        const std::size_t comma = extensions.find(',', pos);
        const std::string_view part = detail::trim_ows(
            extensions.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos));
        if (comma == std::string_view::npos) {
            pos = extensions.size() + 1;
        } else {
            pos = comma + 1;
        }
        if (part.empty()) {
            continue;
        }
        if (!detail::ws_part_is_permessage_deflate(part)) {
            continue; // some other extension; not one we process (RFC 6455 §9.1)
        }
        if (auto cfg = ws_parse_one_deflate_part(part)) {
            return cfg; // first acceptable offer wins (client preference order)
        }
        // A malformed alternative: keep scanning, a later one may be valid.
    }
    return std::nullopt; // no acceptable permessage-deflate offer
}

// Parses the parameters of one permessage-deflate offer. std::nullopt when
// that offer cannot be honoured (an unknown parameter, a value on a valueless
// parameter, an out-of-range or duplicated window-bits value, a duplicated
// parameter).
inline std::optional<WsDeflateConfig> ws_parse_one_deflate_part(std::string_view part) {
    WsDeflateConfig cfg;
    std::optional<bool> offered_snct, offered_cnct; // seen, to reject duplicates
    std::optional<int> offered_server_bits, offered_client_bits;
    bool client_bits_bare = false;

    const std::size_t semi = part.find(';');
    std::size_t p = semi == std::string_view::npos ? part.size() : semi + 1;
    while (p <= part.size()) {
        const std::size_t next_semi = part.find(';', p);
        const std::string_view token = detail::trim_ows(
            part.substr(p, next_semi == std::string_view::npos ? std::string_view::npos : next_semi - p));
        if (next_semi == std::string_view::npos) {
            p = part.size() + 1;
        } else {
            p = next_semi + 1;
        }
        if (token.empty()) {
            continue;
        }
        const std::size_t eq = token.find('=');
        const std::string_view pname = detail::trim_ows(token.substr(0, eq));
        std::string_view pvalue{};
        if (eq != std::string_view::npos) {
            pvalue = detail::trim_ows(token.substr(eq + 1));
        }

        const auto bits8_15 = [](std::string_view v) -> std::optional<int> {
            if (v.size() == 1 && v[0] >= '8' && v[0] <= '9') {
                return v[0] - '0'; // 8, 9
            }
            if (v.size() == 2 && v[0] == '1' && v[1] >= '0' && v[1] <= '5') {
                return 10 + (v[1] - '0'); // 10..15
            }
            return std::nullopt;
        };

        if (detail::ws_ieq(pname, "server_no_context_takeover")) {
            if (offered_snct) {
                return std::nullopt; // duplicate
            }
            if (!pvalue.empty()) {
                return std::nullopt; // a value is not allowed here
            }
            offered_snct = true;
        } else if (detail::ws_ieq(pname, "client_no_context_takeover")) {
            if (offered_cnct) {
                return std::nullopt;
            }
            if (!pvalue.empty()) {
                return std::nullopt;
            }
            offered_cnct = true;
        } else if (detail::ws_ieq(pname, "server_max_window_bits")) {
            if (offered_server_bits) {
                return std::nullopt;
            }
            auto bits = bits8_15(pvalue);
            if (!bits) {
                return std::nullopt;
            }
            offered_server_bits = bits;
        } else if (detail::ws_ieq(pname, "client_max_window_bits")) {
            if (offered_client_bits || client_bits_bare) {
                return std::nullopt;
            }
            if (pvalue.empty()) {
                client_bits_bare = true; // offer without a limit
            } else {
                auto bits = bits8_15(pvalue);
                if (!bits) {
                    return std::nullopt;
                }
                offered_client_bits = bits;
            }
        } else {
            // An unknown parameter: the endpoint cannot honour the offer
            // (RFC 7692 §6.1) — decline rather than guess.
            return std::nullopt;
        }
    }

    cfg.enabled = true;
    cfg.server_no_context_takeover = offered_snct.value_or(false);
    cfg.client_no_context_takeover = offered_cnct.value_or(false);
    if (offered_server_bits) {
        cfg.server_window_bits = *offered_server_bits;
        cfg.client_offered_server_window_bits = true;
    }
    if (client_bits_bare) {
        cfg.client_window_bits = 15; // no limit, accept the default
    } else if (offered_client_bits) {
        cfg.client_window_bits = *offered_client_bits;
        cfg.echo_client_window_bits = true;
    }
    return cfg;
}

// The Sec-WebSocket-Extensions response value for an accepted negotiation, or
// empty when `cfg.enabled` is false (nothing to advertise). Only parameters
// that changed behaviour are rendered, so a plain offer answers with the bare
// extension name.
[[nodiscard]] inline std::string ws_deflate_response_value(const WsDeflateConfig &cfg) {
    if (!cfg.enabled) {
        return {};
    }
    std::string out{"permessage-deflate"};
    if (cfg.server_no_context_takeover) {
        out += "; server_no_context_takeover";
    }
    if (cfg.client_no_context_takeover) {
        out += "; client_no_context_takeover";
    }
    // A client that tied our encoder window down must receive the echo (§7.1.2).
    if (cfg.client_offered_server_window_bits) {
        out += "; server_max_window_bits=";
        out += std::to_string(cfg.server_window_bits);
    }
    if (cfg.echo_client_window_bits) {
        out += "; client_max_window_bits=";
        out += std::to_string(cfg.client_window_bits);
    }
    return out;
}

// The flushing DEFLATE encoding that permessage-deflate frames with.
//
// One stream per direction, so the LZ77 window carries across messages while
// context takeover is on; a no-context-takeover peer resets the stream at every
// message boundary instead.

// Outbound encoder. Compresses one whole message with Z_SYNC_FLUSH and drops
// the trailing flush marker, per RFC 7692 §7.2.1.
class WsDeflater {
  public:
    // `level`: 0 = zlib default, or 1..9 (fastest..most). The encoder holds the
    // stream so the LZ77 window survives across messages (context takeover);
    // set_level() re-initializes it, which is safe for the peer — a fresh
    // window simply never references history the decoder does not have.
    explicit WsDeflater(int window_bits, int level = 0) {
        std::memset(&m_strm, 0, sizeof(m_strm));
        m_ok = ::deflateInit2(&m_strm, level <= 0 ? Z_DEFAULT_COMPRESSION : level, Z_DEFLATED, -window_bits, 8,
                              Z_DEFAULT_STRATEGY) == Z_OK;
        if (m_ok) {
            m_level = level;
        }
    }
    ~WsDeflater() {
        if (m_ok) {
            ::deflateEnd(&m_strm);
        }
    }
    WsDeflater(const WsDeflater &) = delete;
    WsDeflater &operator=(const WsDeflater &) = delete;

    [[nodiscard]] bool ok() const { return m_ok; }

    // Changes the write compression level (0 = zlib default, 1..9), taking
    // effect from the next message. True on success; false on a bad level or a
    // zlib failure. deflateParams keeps the LZ77 window, so context takeover
    // survives the change, and the level persists across per-message resets
    // (no-context-takeover mode).
    bool set_level(int level) {
        if (!m_ok) {
            return false;
        }
        if (level != 0 && (level < 1 || level > 9)) {
            return false;
        }
        if (level == m_level) {
            return true;
        }
        if (::deflateParams(&m_strm, level <= 0 ? Z_DEFAULT_COMPRESSION : level, Z_DEFAULT_STRATEGY) != Z_OK) {
            return false;
        }
        m_level = level;
        return true;
    }

    // Deflates `plain` into `out` (the 4-octet sync-flush marker removed) and sets
    // `compressed` to whether the peer should see RSV1 set on the frame. When
    // `reset` (server-side no context takeover) the stream is re-initialized
    // after the message, so no window survives to the next one. False on a zlib
    // failure; the stream is then unusable.
    //
    // An empty message is deliberately NOT compressed (compressed = false, out
    // stays empty): its deflate is only the sync-flush marker, and RFC 7692
    // §7.2.3.5 singles out exactly this case for uncompressed handling — a
    // bare marker on a context-taken-over stream is where reference decoders
    // differ. Every receiver accepts an uncompressed message once the extension
    // is negotiated.
    bool compress(std::string_view plain, std::string &out, bool reset, bool &compressed) {
        out.clear();
        if (!m_ok) {
            return false;
        }
        if (plain.empty()) {
            compressed = false;
            if (reset) {
                ::deflateReset(&m_strm);
            }
            return true;
        }
        m_strm.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(plain.data()));
        m_strm.avail_in = static_cast<uInt>(plain.size());
        std::array<char, 16384> tmp{};
        int rc = Z_OK;
        for (;;) {
            m_strm.next_out = reinterpret_cast<Bytef *>(tmp.data());
            m_strm.avail_out = static_cast<uInt>(tmp.size());
            rc = ::deflate(&m_strm, Z_SYNC_FLUSH);
            if (rc != Z_OK && rc != Z_BUF_ERROR) {
                return false;
            }
            out.append(tmp.data(), tmp.size() - m_strm.avail_out);
            if (m_strm.avail_out != 0) {
                break;
            }
        }
        if (out.size() < 4) {
            return false; // a deflated message always carries at least the flush marker
        }
        out.resize(out.size() - 4);
        compressed = true;
        if (reset) {
            ::deflateReset(&m_strm);
        }
        return true;
    }

    // Convenience overload for callers that always send the frame compressed
    // (the empty-message special case then yields an empty, uncompressed frame).
    bool compress(std::string_view plain, std::string &out, bool reset) {
        bool flag = false;
        return compress(plain, out, reset, flag);
    }

  private:
    z_stream m_strm{};
    bool m_ok = false;
    int m_level = 0;
};

// Result of decoding a compressed chunk.
enum class WsInflateStatus { Ok, Overflow, Error };

// Inbound decoder. `feed` consumes compressed octets (fragment payloads),
// `finish` terminates the message by re-feeding the 4-octet marker the sender
// stripped (RFC 7692 §7.2.2), and — with no context takeover — resets the
// stream so no window survives to the next message.
class WsInflater {
  public:
    explicit WsInflater(int window_bits) {
        std::memset(&m_strm, 0, sizeof(m_strm));
        m_ok = ::inflateInit2(&m_strm, -window_bits) == Z_OK;
    }
    ~WsInflater() {
        if (m_ok) {
            ::inflateEnd(&m_strm);
        }
    }
    WsInflater(const WsInflater &) = delete;
    WsInflater &operator=(const WsInflater &) = delete;

    [[nodiscard]] bool ok() const { return m_ok; }

    WsInflateStatus feed(std::string_view data, std::string &out, std::size_t budget) {
        return pump(data, Z_NO_FLUSH, out, budget);
    }

    // Terminates the current message: flushes the decoder and resets the stream
    // when `reset` was negotiated. Called once per message, after its last
    // fragment.
    WsInflateStatus finish(std::string &out, std::size_t budget, bool reset) {
        // The 4 octets the sender removed, fed back so the decoder's window
        // lands exactly where the encoder's did. They decode to no output.
        static constexpr char kTail[4] = {0x00, 0x00, static_cast<char>(0xFF), static_cast<char>(0xFF)};
        const WsInflateStatus st = pump(std::string_view{kTail, sizeof(kTail)}, Z_SYNC_FLUSH, out, budget);
        if (st == WsInflateStatus::Ok && reset) {
            ::inflateReset(&m_strm);
        }
        return st;
    }

  private:
    WsInflateStatus pump(std::string_view in, int flush, std::string &out, std::size_t budget) {
        if (!m_ok) {
            return WsInflateStatus::Error;
        }
        m_strm.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.data()));
        m_strm.avail_in = static_cast<uInt>(in.size());
        std::array<char, 16384> tmp{};
        for (;;) {
            m_strm.next_out = reinterpret_cast<Bytef *>(tmp.data());
            m_strm.avail_out = static_cast<uInt>(tmp.size());
            const int rc = ::inflate(&m_strm, flush);
            if (rc != Z_OK && rc != Z_BUF_ERROR) {
                // Z_STREAM_END should never occur: a permessage-deflate stream
                // never carries a deflate terminator, only sync flushes.
                return WsInflateStatus::Error;
            }
            const std::size_t produced = tmp.size() - m_strm.avail_out;
            if (produced > budget - out.size()) {
                return WsInflateStatus::Overflow; // zip bomb: bounded, like the uncompressed path
            }
            out.append(tmp.data(), produced);
            if (m_strm.avail_out != 0) {
                break;
            }
        }
        return WsInflateStatus::Ok;
    }

    z_stream m_strm{};
    bool m_ok = false;
};

} // namespace simple_http