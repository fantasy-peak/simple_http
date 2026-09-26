// core/validators.h: cache validators and byte ranges.
//
// The two are tested together because they share one input in practice: a
// request carries the validator and the range, and If-Range is the point where
// the two decisions meet.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include "simple_http.h"

using namespace simple_http;

TEST_CASE("validators: etags are quoted, deterministic and per-representation", "[validators]") {
    // nginx's shape: quoted hex mtime and size.
    CHECK(make_etag(100, 200, "") == "\"64-c8\"");

    // Deterministic: the same metadata yields the same validator, which is what
    // lets a 304 be answered without touching the file.
    CHECK(make_etag(100, 200, "") == make_etag(100, 200, ""));

    // A different size is a different representation.
    CHECK(make_etag(100, 201, "") != make_etag(100, 200, ""));

    // The three encodings of one file must never share a validator: a client
    // that saw the brotli form would otherwise revalidate the identity one as
    // unchanged and get a body it cannot decode.
    const std::string id = make_etag(100, 200, "");
    const std::string br = make_etag(100, 200, "br");
    const std::string gz = make_etag(100, 200, "gz");
    CHECK(id != br);
    CHECK(id != gz);
    CHECK(br != gz);

    // Strong, not weak: If-Range only honours a strong validator, so a "W/"
    // prefix here would silently disable range resumption.
    CHECK_FALSE(id.starts_with("W/"));
}

TEST_CASE("validators: etag comparison is weak and list-aware", "[validators]") {
    const std::string etag = "\"abc-123\"";

    CHECK(etag_matches("*", etag));
    CHECK(etag_matches(etag, etag));
    CHECK(etag_matches("\"other\", " + etag, etag));
    CHECK(etag_matches(etag + ", \"other\"", etag));

    // RFC 9110 §8.8.3.2: W/ is ignored on both sides.
    CHECK(etag_matches("W/" + etag, etag));
    CHECK(etag_matches(etag, etag));
    CHECK(etag_matches("W/\"x\"", "\"x\""));

    // Whitespace around list members is not significant.
    CHECK(etag_matches("  \"a\" ,  " + etag + "  ", etag));

    CHECK_FALSE(etag_matches("\"different\"", etag));
    CHECK_FALSE(etag_matches("", etag));
    CHECK_FALSE(etag_matches("  ", etag));
}

TEST_CASE("validators: parse_range handles the forms a browser actually sends", "[validators]") {
    bool bad = false;

    auto r = parse_range("bytes=0-4", 10, bad);
    REQUIRE(r.has_value());
    CHECK(r->first == 0);
    CHECK(r->last == 4);
    CHECK(r->length() == 5);
    CHECK_FALSE(bad);

    // "-N": the last N bytes.
    r = parse_range("bytes=-3", 10, bad);
    REQUIRE(r.has_value());
    CHECK(r->first == 7);
    CHECK(r->last == 9);

    // "N-": from N to the end.
    r = parse_range("bytes=5-", 10, bad);
    REQUIRE(r.has_value());
    CHECK(r->first == 5);
    CHECK(r->last == 9);

    // An end past the file is clamped, not refused (RFC 9110 §14.1.2).
    r = parse_range("bytes=0-100", 10, bad);
    REQUIRE(r.has_value());
    CHECK(r->last == 9);

    // "-N" with N larger than the file is the whole file.
    r = parse_range("bytes=-100", 10, bad);
    REQUIRE(r.has_value());
    CHECK(r->first == 0);
    CHECK(r->last == 9);

    // Whitespace around the spec is tolerated.
    CHECK(parse_range("bytes= 0-4 ", 10, bad).has_value());
}

TEST_CASE("validators: unsatisfiable and unparseable are different answers", "[validators]") {
    bool bad = false;

    // A syntactically valid range entirely past the end: 416.
    CHECK_FALSE(parse_range("bytes=10-", 10, bad).has_value());
    CHECK(bad);

    // Size 0 with a suffix range means there is nothing to satisfy.
    CHECK_FALSE(parse_range("bytes=-1", 0, bad).has_value());
    CHECK(bad);

    // Everything below is "cannot parse this", which the caller answers with a
    // full 200 (RFC 9110 permits ignoring Range) rather than a 416.
    struct Case {
        const char* header;
    };
    const Case cases[] = {
        {"bytes=-0"},        // a zero-length suffix names nothing
        {"bytes=0-1,3-4"},   // multi-range: multipart/byteranges is two shapes
        {"items=0-1"},       // a unit we do not speak
        {"bytes=5-2"},       // last before first
        {"bytes="},          // no spec at all
        {"0-4"},             // missing the unit
        {"bytes=abc-def"},   // not numbers
    };
    for (const auto& c : cases) {
        INFO("header: " << c.header);
        bad = false;
        CHECK_FALSE(parse_range(c.header, 10, bad).has_value());
        CHECK_FALSE(bad);  // "no range", not "unsatisfiable"
    }
}
