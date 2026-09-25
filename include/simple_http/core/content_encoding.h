#pragma once

// Response body compression: gzip (zlib) and brotli, plus the Accept-Encoding
// negotiation and content-type rules that decide whether to use them.
//
// The codecs sit behind SIMPLE_HTTP_ENABLE_COMPRESSION. This is a header-only
// INTERFACE library, so an unconditional #include <zlib.h> here would force
// every downstream to link zlib and brotli whether it wants compression or not.
// With the macro undefined every function below still exists but degrades to
// "no encoding available" - negotiate_encoding returns nullopt and
// make_encoder returns nullptr - which keeps call sites free of #ifdefs.
//
// Only the encoders live here. Decoding is provided for tests and for callers
// that explicitly want it; the server never decodes (it does not accept
// compressed request bodies) and the HTTP client does not decode responses.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "compression.h"

#ifdef SIMPLE_HTTP_ENABLE_COMPRESSION
#include <brotli/decode.h>
#include <brotli/encode.h>
#include <zlib.h>
#endif

namespace simple_http {

inline constexpr std::string_view kEncodingGzip = "gzip";
inline constexpr std::string_view kEncodingBrotli = "br";
inline constexpr std::string_view kEncodingIdentity = "identity";

// --- small text helpers ---------------------------------------------------

inline char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool iequals_ci(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) {
            return false;
        }
    }
    return true;
}

inline bool starts_with_ci(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && iequals_ci(s.substr(0, prefix.size()), prefix);
}

inline bool ends_with_ci(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && iequals_ci(s.substr(s.size() - suffix.size()), suffix);
}

inline std::string_view trim_ascii(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

// True when a comma-separated header value contains `token` (case-insensitive).
// Used for `Vary` and `Cache-Control` membership tests.
inline bool header_has_token(std::string_view value, std::string_view token) {
    std::size_t pos = 0;
    for (;;) {
        const auto comma = value.find(',', pos);
        const std::string_view item =
            trim_ascii(value.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos));
        if (iequals_ci(item, token)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            return false;
        }
        pos = comma + 1;
    }
}

// "text/html; charset=utf-8" -> "text/html"
inline std::string_view mime_essence(std::string_view content_type) {
    const auto semi = content_type.find(';');
    if (semi != std::string_view::npos) {
        content_type = content_type.substr(0, semi);
    }
    return trim_ascii(content_type);
}

// Parses the value of an Accept-Encoding q parameter ("0", "1", "0.8", ".5").
// Anything unparseable yields 0, which reads as "not acceptable" - the safe
// direction, since it only ever suppresses compression.
inline double parse_qvalue(std::string_view s) {
    s = trim_ascii(s);
    double value = 0.0;
    bool seen_dot = false;
    double scale = 0.1;
    for (char c : s) {
        if (c >= '0' && c <= '9') {
            if (!seen_dot) {
                value = value * 10.0 + (c - '0');
            } else {
                value += (c - '0') * scale;
                scale *= 0.1;
            }
        } else if (c == '.') {
            if (seen_dot) {
                break;
            }
            seen_dot = true;
        } else {
            break;
        }
    }
    return value;
}

// --- encoders -------------------------------------------------------------

// A streaming encoder. `write` may return an empty string (the codec is
// buffering) but never loses bytes; `finish` emits the tail and terminates the
// stream. One instance encodes exactly one response body.
class ContentEncoder {
  public:
    virtual ~ContentEncoder() = default;
    virtual std::string write(std::string_view data) = 0;
    virtual std::string finish() = 0;
};

// The decoding counterpart, used by the client. `write` returns whatever could
// be decoded so far - empty is normal, since the codec may be waiting for more
// input - and never loses bytes. `finish` is called once at end-of-body and
// marks a truncated stream as failed; `failed` reports malformed input, so a
// caller can refuse half a body instead of passing it off as whole. One
// instance decodes exactly one body.
class ContentDecoder {
  public:
    virtual ~ContentDecoder() = default;
    virtual std::string write(std::string_view data) = 0;
    virtual std::string finish() = 0;
    virtual bool failed() const = 0;
};

#ifdef SIMPLE_HTTP_ENABLE_COMPRESSION

namespace detail {

inline std::string zlib_run(z_stream& zs, bool& ok, std::string_view data, int flush) {
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    zs.avail_in = static_cast<uInt>(data.size());
    std::string out;
    for (;;) {
        char buf[16 * 1024];
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        const int rc = ::deflate(&zs, flush);
        out.append(buf, sizeof(buf) - zs.avail_out);
        if (rc == Z_STREAM_END) {
            break;
        }
        // Z_BUF_ERROR is not a failure: with Z_NO_FLUSH it means "not enough
        // input to produce output yet", which is the normal state after a small
        // write. Treating it as fatal silently swallowed everything that
        // followed.
        if (rc == Z_BUF_ERROR) {
            break;
        }
        if (rc != Z_OK) {
            ok = false;
            break;
        }
        // Z_OK: keep going while input remains, or the output buffer filled up
        // (which means the codec still has bytes to give us).
        if (zs.avail_in == 0 && zs.avail_out != 0) {
            break;
        }
    }
    return out;
}

inline std::string brotli_run(BrotliEncoderState* st, bool& ok, std::string_view data,
                              BrotliEncoderOperation op) {
    std::size_t avail_in = data.size();
    const std::uint8_t* next_in = reinterpret_cast<const std::uint8_t*>(data.data());
    std::string out;
    for (;;) {
        std::uint8_t buf[16 * 1024];
        std::size_t avail_out = sizeof(buf);
        std::uint8_t* next_out = buf;
        std::size_t total_out = 0;
        if (!::BrotliEncoderCompressStream(st, op, &avail_in, &next_in, &avail_out, &next_out, &total_out)) {
            ok = false;
            break;
        }
        out.append(reinterpret_cast<const char*>(buf), sizeof(buf) - avail_out);
        if (op == BROTLI_OPERATION_FINISH) {
            if (::BrotliEncoderIsFinished(st)) {
                break;
            }
        } else if (!::BrotliEncoderHasMoreOutput(st) && avail_in == 0) {
            break;
        }
    }
    return out;
}

class GzipEncoder final : public ContentEncoder {
  public:
    explicit GzipEncoder(int level) {
        // windowBits 15 + 16 selects the gzip container (RFC 1952) over a raw
        // deflate stream, which is what `Content-Encoding: gzip` promises.
        m_ok = ::deflateInit2(&m_zs, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK;
    }
    ~GzipEncoder() override {
        if (m_ok) {
            ::deflateEnd(&m_zs);
        }
    }

    std::string write(std::string_view data) override {
        if (!m_ok || data.empty()) {
            return {};
        }
        return zlib_run(m_zs, m_ok, data, Z_NO_FLUSH);
    }
    std::string finish() override {
        if (!m_ok) {
            return {};
        }
        return zlib_run(m_zs, m_ok, {}, Z_FINISH);
    }

  private:
    z_stream m_zs{};
    bool m_ok{false};
};

class BrotliEncoderImpl final : public ContentEncoder {
  public:
    explicit BrotliEncoderImpl(int quality) {
        m_st = ::BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
        if (m_st) {
            ::BrotliEncoderSetParameter(m_st, BROTLI_PARAM_QUALITY, static_cast<std::uint32_t>(quality));
        }
    }
    ~BrotliEncoderImpl() override {
        if (m_st) {
            ::BrotliEncoderDestroyInstance(m_st);
        }
    }

    std::string write(std::string_view data) override {
        if (!m_st || data.empty()) {
            return {};
        }
        return brotli_run(m_st, m_ok, data, BROTLI_OPERATION_PROCESS);
    }
    std::string finish() override {
        if (!m_st) {
            return {};
        }
        return brotli_run(m_st, m_ok, {}, BROTLI_OPERATION_FINISH);
    }

  private:
    BrotliEncoderState* m_st{nullptr};
    bool m_ok{true};
};

// --- decoders -------------------------------------------------------------

class GzipDecoder final : public ContentDecoder {
  public:
    GzipDecoder() {
        // windowBits 15 + 16: the gzip container, matching the encoder.
        if (::inflateInit2(&m_zs, 15 + 16) != Z_OK) {
            m_failed = true;
        }
    }
    ~GzipDecoder() override {
        if (m_zs.state != nullptr) {  // only set once inflateInit2 succeeded
            ::inflateEnd(&m_zs);
        }
    }

    std::string write(std::string_view data) override { return run(data); }

    // inflate has no flush step: the stream ends where the data says it does,
    // so reaching here without Z_STREAM_END means the body was truncated.
    std::string finish() override {
        if (!m_stream_end) {
            m_failed = true;
        }
        return {};
    }

    bool failed() const override { return m_failed; }

  private:
    std::string run(std::string_view data) {
        if (m_failed) {
            return {};
        }
        m_zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
        m_zs.avail_in = static_cast<uInt>(data.size());
        std::string out;
        for (;;) {
            char buf[16 * 1024];
            m_zs.next_out = reinterpret_cast<Bytef*>(buf);
            m_zs.avail_out = sizeof(buf);
            const int rc = ::inflate(&m_zs, Z_NO_FLUSH);
            out.append(buf, sizeof(buf) - m_zs.avail_out);
            if (rc == Z_STREAM_END) {
                m_stream_end = true;
                break;
            }
            if (rc == Z_OK) {
                if (m_zs.avail_in == 0 && m_zs.avail_out != 0) {
                    break;  // consumed everything it could; more input will come
                }
                continue;  // output buffer filled: there is more to collect
            }
            if (rc == Z_BUF_ERROR) {
                break;  // not enough input to make progress - not an error
            }
            m_failed = true;
            break;
        }
        return out;
    }

    z_stream m_zs{};
    bool m_stream_end{false};
    bool m_failed{false};
};

class BrotliDecoderImpl final : public ContentDecoder {
  public:
    BrotliDecoderImpl() {
        m_st = ::BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
        if (!m_st) {
            m_failed = true;
        }
    }
    ~BrotliDecoderImpl() override {
        if (m_st) {
            ::BrotliDecoderDestroyInstance(m_st);
        }
    }

    std::string write(std::string_view data) override {
        if (!m_st || m_failed) {
            return {};
        }
        std::size_t avail_in = data.size();
        const std::uint8_t* next_in = reinterpret_cast<const std::uint8_t*>(data.data());
        std::string out;
        for (;;) {
            std::uint8_t buf[16 * 1024];
            std::size_t avail_out = sizeof(buf);
            std::uint8_t* next_out = buf;
            const BrotliDecoderResult rc =
                ::BrotliDecoderDecompressStream(m_st, &avail_in, &next_in, &avail_out, &next_out, nullptr);
            out.append(reinterpret_cast<const char*>(buf), sizeof(buf) - avail_out);
            if (rc == BROTLI_DECODER_RESULT_SUCCESS) {
                m_stream_end = true;
                break;
            }
            if (rc == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
                continue;
            }
            if (rc == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) {
                break;
            }
            m_failed = true;  // ERROR
            break;
        }
        return out;
    }

    std::string finish() override {
        if (!m_stream_end) {
            m_failed = true;
        }
        return {};
    }

    bool failed() const override { return m_failed; }

  private:
    BrotliDecoderState* m_st{nullptr};
    bool m_stream_end{false};
    bool m_failed{false};
};

}  // namespace detail

#endif  // SIMPLE_HTTP_ENABLE_COMPRESSION

// Returns nullptr when `encoding` is unknown or no codec is compiled in.
[[nodiscard]] inline std::unique_ptr<ContentEncoder> make_encoder(std::string_view encoding,
                                                                 const CompressionConfig& config) {
#ifdef SIMPLE_HTTP_ENABLE_COMPRESSION
    if (encoding == kEncodingGzip) {
        return std::make_unique<detail::GzipEncoder>(config.gzip_level);
    }
    if (encoding == kEncodingBrotli) {
        return std::make_unique<detail::BrotliEncoderImpl>(config.brotli_quality);
    }
#else
    (void)encoding;
    (void)config;
#endif
    return nullptr;
}

// Returns nullptr when `encoding` is unknown, is `identity`, or no codec is
// compiled in - the caller then leaves the body alone.
[[nodiscard]] inline std::unique_ptr<ContentDecoder> make_decoder(std::string_view encoding) {
#ifdef SIMPLE_HTTP_ENABLE_COMPRESSION
    if (encoding == kEncodingGzip) {
        return std::make_unique<detail::GzipDecoder>();
    }
    if (encoding == kEncodingBrotli) {
        return std::make_unique<detail::BrotliDecoderImpl>();
    }
#else
    (void)encoding;
#endif
    return nullptr;
}

// Compresses a whole buffer with one call. Falls back to a copy when the
// encoding is unavailable, so callers never have to check.
inline std::string compress_all(std::string_view encoding, std::string_view in,
                                const CompressionConfig& config) {
    auto encoder = make_encoder(encoding, config);
    if (!encoder) {
        return std::string{in};
    }
    std::string out = encoder->write(in);
    out += encoder->finish();
    return out;
}

// Decodes a whole buffer in one call, for callers that already hold the bytes.
// Unknown encodings and malformed streams both come back unchanged: this never
// throws and never loses data. A caller that must tell those apart - the
// streaming client does - uses make_decoder directly.
inline std::string decompress_all(std::string_view encoding, std::string_view in) {
    auto decoder = make_decoder(encoding);
    if (!decoder) {
        return std::string{in};
    }
    std::string out = decoder->write(in);
    out += decoder->finish();
    if (decoder->failed()) {
        return std::string{in};
    }
    return out;
}

// --- negotiation ----------------------------------------------------------

// Picks the best encoding the client accepts and we can produce, honouring
// q-values and the `*` wildcard. Returns nullopt to send the body unchanged,
// which is also the answer whenever compression is disabled or unavailable.
//
// Ties go to brotli: at equal quality it produces a smaller body, and the extra
// CPU is the caller's choice (they enabled compression).
inline std::optional<std::string> negotiate_encoding(std::string_view accept_encoding,
                                                     const CompressionConfig& config) {
    if (!config.enabled || accept_encoding.empty()) {
        return std::nullopt;
    }

    // -1 means "not mentioned"; the `*` entry fills the gaps.
    double q_brotli = -1.0;
    double q_gzip = -1.0;
    double q_any = 0.0;  // per RFC 9110, an absent `*` means "not acceptable"

    std::size_t pos = 0;
    while (pos <= accept_encoding.size()) {
        const auto comma = accept_encoding.find(',', pos);
        std::string_view token = accept_encoding.substr(
            pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
        pos = (comma == std::string_view::npos) ? accept_encoding.size() + 1 : comma + 1;

        token = trim_ascii(token);
        if (token.empty()) {
            continue;
        }
        const auto semi = token.find(';');
        const std::string_view name = trim_ascii(semi == std::string_view::npos ? token : token.substr(0, semi));
        double q = 1.0;
        if (semi != std::string_view::npos) {
            std::string_view params = token.substr(semi + 1);
            const auto qpos = params.find("q=");
            if (qpos != std::string_view::npos) {
                q = parse_qvalue(params.substr(qpos + 2));
            }
        }

        if (iequals_ci(name, kEncodingBrotli)) {
            q_brotli = q;
        } else if (iequals_ci(name, kEncodingGzip)) {
            q_gzip = q;
        } else if (name == "*") {
            q_any = q;
        }
    }

    if (q_brotli < 0) {
        q_brotli = q_any;
    }
    if (q_gzip < 0) {
        q_gzip = q_any;
    }

    if (q_brotli > 0 && q_brotli >= q_gzip) {
        return std::string{kEncodingBrotli};
    }
    if (q_gzip > 0) {
        return std::string{kEncodingGzip};
    }
    return std::nullopt;
}

// --- content-type rule ----------------------------------------------------

// The built-in rule. Textual types compress well; containers that are already
// entropy-coded (images, media, fonts, archives) do not - compressing them
// costs CPU and can make the body *larger*. Unknown types are left alone: a
// wrong "no" ships a few extra bytes, a wrong "yes" burns CPU on every response
// for nothing.
inline bool default_compressible_type(std::string_view essence) {
    // Media types are case-insensitive (RFC 9110 §8.3), so every comparison here
    // is too - a client that sends "APPLICATION/JSON" means the same thing.
    if (starts_with_ci(essence, "text/")) {
        return true;
    }
    if (iequals_ci(essence, "image/svg+xml")) {
        return true;  // XML, despite the image/ prefix
    }
    if (starts_with_ci(essence, "image/") || starts_with_ci(essence, "video/") ||
        starts_with_ci(essence, "audio/") || starts_with_ci(essence, "font/")) {
        return false;
    }
    if (ends_with_ci(essence, "+json") || ends_with_ci(essence, "+xml")) {
        return true;
    }
    return iequals_ci(essence, "application/json") || iequals_ci(essence, "application/javascript") ||
           iequals_ci(essence, "application/x-javascript") || iequals_ci(essence, "application/xml") ||
           iequals_ci(essence, "application/xhtml+xml") || iequals_ci(essence, "application/rss+xml") ||
           iequals_ci(essence, "application/atom+xml");
}

// A non-empty config.types replaces the built-in rule entirely.
inline bool is_compressible_type(std::string_view content_type, const CompressionConfig& config) {
    const std::string_view essence = mime_essence(content_type);
    if (essence.empty()) {
        return false;
    }
    if (!config.types.empty()) {
        for (const std::string& allowed : config.types) {
            if (iequals_ci(allowed, essence)) {
                return true;
            }
        }
        return false;
    }
    return default_compressible_type(essence);
}

}  // namespace simple_http
