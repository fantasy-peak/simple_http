#pragma once

// CompressionConfig: whether and how the server compresses response bodies.
//
// This lives on its own (rather than inside content_encoding.h) so that
// EngineLimits can carry one without pulling in zlib/brotli headers: the codecs
// themselves sit behind SIMPLE_HTTP_ENABLE_COMPRESSION.

#include <cstdint>
#include <string>
#include <vector>

namespace simple_http {

struct CompressionConfig {
    // Off by default. Turning it on changes what every handler puts on the
    // wire, so it is opt-in rather than opt-out; with it off, nothing in the
    // compression path is even consulted.
    bool enabled{false};

    // Responses smaller than this are sent as-is, because compression overhead
    // can make a small body bigger. Only the one-shot send() path can honour
    // this: a streamed response's length is not known when it starts.
    std::uint64_t min_bytes{1024};

    int gzip_level{6};      // zlib's Z_DEFAULT_COMPRESSION
    int brotli_quality{5};  // 0..11; 5 is the usual speed/ratio knee

    // Content types to compress. Empty means "use the built-in rule" (see
    // is_compressible_type); non-empty *replaces* that rule, so an application
    // can compress exactly what it means to and nothing else.
    std::vector<std::string> types{};

    // Emit (and merge into any existing) `Vary: Accept-Encoding`. Leave this on
    // unless something downstream already handles it: without it a shared cache
    // can hand a gzipped body to a client that only understands identity.
    bool vary{true};

    // Demote a strong ETag to a weak one on a compressed response. The body
    // changed, so the original validator no longer identifies this
    // representation; `W/` keeps conditional requests working under the weaker
    // comparison rather than lying.
    bool weaken_etag{true};

    // Honour `Cache-Control: no-transform` (RFC 9110 §7.7), which forbids
    // intermediaries and servers from changing the representation.
    bool respect_no_transform{true};

    // Compress streamed responses (begin/write/finish) too. Their length is not
    // known up front, so min_bytes cannot apply and even a short stream gets
    // compressed - which can make it a few bytes larger. Turn this off to have
    // only responses whose full length is known (send()) compressed.
    bool compress_streamed{true};
};

}  // namespace simple_http
