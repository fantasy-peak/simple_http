// core/content_encoding.h: Accept-Encoding negotiation, the content-type rule,
// and the gzip/brotli codecs (behind SIMPLE_HTTP_ENABLE_COMPRESSION, which the
// unittest target defines).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "simple_http.h"

using namespace simple_http;

namespace {

CompressionConfig compression_on() {
    CompressionConfig config;
    config.enabled = true;
    return config;
}

// Negotiation result as a string, so the assertions read as one line.
std::string pick(std::string_view accept_encoding, const CompressionConfig& config) {
    return negotiate_encoding(accept_encoding, config).value_or("<none>");
}

}  // namespace

TEST_CASE("content_encoding: negotiation honouring q-values", "[compression]") {
    const CompressionConfig config = compression_on();

    CHECK(pick("gzip", config) == "gzip");
    CHECK(pick("br", config) == "br");
    CHECK(pick("GZIP", config) == "gzip");  // case-insensitive
    CHECK(pick("gzip, br", config) == "br");  // tie goes to brotli
    CHECK(pick("br, gzip", config) == "br");

    CHECK(pick("br;q=0.5, gzip;q=0.9", config) == "gzip");
    CHECK(pick("br;q=0.9, gzip;q=0.5", config) == "br");
    CHECK(pick(" br ; gzip ", config) == "br");  // surrounding whitespace
}

TEST_CASE("content_encoding: negotiation refuses what the client refuses", "[compression]") {
    const CompressionConfig config = compression_on();

    CHECK(pick("", config) == "<none>");
    CHECK(pick("identity", config) == "<none>");
    CHECK(pick("deflate", config) == "<none>");  // valid, but not one we produce
    CHECK(pick("gzip;q=0", config) == "<none>");
    CHECK(pick("br;q=0, gzip;q=0", config) == "<none>");
    CHECK(pick("*;q=0", config) == "<none>");
    CHECK(pick("gzip;q=0.001", config) == "gzip");  // a hair above zero still counts
}

TEST_CASE("content_encoding: the wildcard covers the unmentioned", "[compression]") {
    const CompressionConfig config = compression_on();

    CHECK(pick("*", config) == "br");
    CHECK(pick("*;q=0, gzip", config) == "gzip");  // explicit beats the wildcard
    CHECK(pick("gzip;q=0, *", config) == "br");    // gzip stays refused, brotli is covered
    CHECK(pick("gzip;q=0.4, *", config) == "br");
}

TEST_CASE("content_encoding: disabled means no negotiation at all", "[compression]") {
    const CompressionConfig off{};
    CHECK_FALSE(negotiate_encoding("gzip, br", off).has_value());
    CHECK_FALSE(negotiate_encoding("*", off).has_value());
}

TEST_CASE("content_encoding: which types are worth compressing", "[compression]") {
    const CompressionConfig config = compression_on();

    CHECK(is_compressible_type("text/html", config));
    CHECK(is_compressible_type("text/plain; charset=utf-8", config));  // parameters stripped
    CHECK(is_compressible_type("APPLICATION/JSON", config));
    CHECK(is_compressible_type("application/ld+json", config));  // +json suffix
    CHECK(is_compressible_type("image/svg+xml", config));        // svg is text, not a bitmap

    CHECK_FALSE(is_compressible_type("image/png", config));
    CHECK_FALSE(is_compressible_type("video/mp4", config));
    CHECK_FALSE(is_compressible_type("audio/ogg", config));
    CHECK_FALSE(is_compressible_type("font/woff2", config));
    CHECK_FALSE(is_compressible_type("application/octet-stream", config));
    CHECK_FALSE(is_compressible_type("application/zip", config));
    CHECK_FALSE(is_compressible_type("", config));
}

TEST_CASE("content_encoding: an explicit type list replaces the built-in rule", "[compression]") {
    CompressionConfig config = compression_on();
    config.types = {"application/octet-stream"};

    CHECK(is_compressible_type("application/octet-stream", config));
    CHECK_FALSE(is_compressible_type("text/html", config));  // no longer implicit
}

TEST_CASE("content_encoding: q-value and mime parsing", "[compression]") {
    CHECK(parse_qvalue("1") == 1.0);
    CHECK(parse_qvalue("0") == 0.0);
    CHECK(parse_qvalue("0.8") == Catch::Approx(0.8));
    CHECK(parse_qvalue(".5") == Catch::Approx(0.5));
    CHECK(parse_qvalue("bogus") == 0.0);  // unparseable reads as "not acceptable"

    CHECK(mime_essence("text/html; charset=utf-8") == "text/html");
    CHECK(mime_essence("text/html") == "text/html");
    CHECK(mime_essence("  text/html  ") == "text/html");
    CHECK(mime_essence("") == "");
}

#ifdef SIMPLE_HTTP_ENABLE_COMPRESSION

namespace {

std::string repetitive_body() {
    std::string body;
    for (int i = 0; i < 500; ++i) {
        body += "chunk-";
        body += std::to_string(i);
        body += ";";
    }
    return body;
}

}  // namespace

TEST_CASE("content_encoding: round-trips shrink and restore", "[compression]") {
    const CompressionConfig config = compression_on();
    const std::string body = repetitive_body();
    REQUIRE(body.size() > 2000);

    for (std::string_view encoding : {kEncodingGzip, kEncodingBrotli}) {
        const std::string packed = compress_all(encoding, body, config);
        CHECK(packed.size() < body.size());
        CHECK(decompress_all(encoding, packed) == body);
    }
}

TEST_CASE("content_encoding: streaming equals one-shot", "[compression]") {
    const CompressionConfig config = compression_on();
    const std::string body = repetitive_body();

    for (std::string_view encoding : {kEncodingGzip, kEncodingBrotli}) {
        auto encoder = make_encoder(encoding, config);
        REQUIRE(encoder != nullptr);

        // Awkward chunk sizes on purpose: exercise partial input and output.
        std::string streamed;
        for (std::size_t off = 0; off < body.size(); off += 7) {
            streamed += encoder->write(std::string_view{body}.substr(off, 7));
        }
        streamed += encoder->finish();

        CHECK(decompress_all(encoding, streamed) == body);
    }
}

TEST_CASE("content_encoding: empty and tiny bodies", "[compression]") {
    const CompressionConfig config = compression_on();

    for (std::string_view encoding : {kEncodingGzip, kEncodingBrotli}) {
        CHECK(decompress_all(encoding, compress_all(encoding, "", config)).empty());
        CHECK(decompress_all(encoding, compress_all(encoding, "x", config)) == "x");
    }
}

TEST_CASE("content_encoding: the streaming decoder matches the one-shot path", "[compression]") {
    const CompressionConfig config = compression_on();
    const std::string body = repetitive_body();

    for (std::string_view encoding : {kEncodingGzip, kEncodingBrotli}) {
        const std::string packed = compress_all(encoding, body, config);

        // Chunk boundaries never line up with codec frames in the wild, so walk
        // a few awkward step sizes rather than one comfortable one.
        for (const std::size_t step : {1u, 3u, 7u, 1024u}) {
            auto decoder = make_decoder(encoding);
            REQUIRE(decoder != nullptr);

            std::string decoded;
            for (std::size_t off = 0; off < packed.size(); off += step) {
                decoded += decoder->write(std::string_view{packed}.substr(off, step));
            }
            decoded += decoder->finish();
            CHECK_FALSE(decoder->failed());
            CHECK(decoded == body);
        }
    }
}

TEST_CASE("content_encoding: a truncated stream reports failure", "[compression]") {
    for (std::string_view encoding : {kEncodingGzip, kEncodingBrotli}) {
        const std::string full = compress_all(encoding, repetitive_body(), compression_on());
        REQUIRE(full.size() > 8);

        auto decoder = make_decoder(encoding);
        REQUIRE(decoder != nullptr);
        (void)decoder->write(std::string_view{full}.substr(0, full.size() - 4));
        (void)decoder->finish();  // the stream never reached its end marker
        CHECK(decoder->failed());
    }
}

TEST_CASE("content_encoding: malformed input reports failure", "[compression]") {
    auto decoder = make_decoder("gzip");
    REQUIRE(decoder != nullptr);
    (void)decoder->write("this is definitely not a gzip stream");
    CHECK(decoder->failed());
}

TEST_CASE("content_encoding: make_decoder knows what it can decode", "[compression]") {
    CHECK(make_decoder("gzip") != nullptr);
    CHECK(make_decoder("br") != nullptr);
    CHECK(make_decoder("identity") == nullptr);
    CHECK(make_decoder("deflate") == nullptr);
    CHECK(make_decoder("") == nullptr);
}

TEST_CASE("content_encoding: unknown encodings and codecs", "[compression]") {
    const CompressionConfig config = compression_on();

    CHECK(make_encoder("deflate", config) == nullptr);
    CHECK(make_encoder("identity", config) == nullptr);
    // compress_all falls back to a copy rather than failing.
    CHECK(compress_all("deflate", "payload", config) == "payload");
    // decompress_all passes unknown encodings through.
    CHECK(decompress_all("deflate", "payload") == "payload");
}

#endif  // SIMPLE_HTTP_ENABLE_COMPRESSION
