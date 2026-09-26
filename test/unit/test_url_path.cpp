// core/url_path.h: decoding a request target into a canonical lookup key, and
// the segment-aware prefix test that the key is designed to be used with.
//
// This is the security-relevant half of the static-file design, so the cases
// below are mostly adversary-shaped. The property under test is not "these known
// bad inputs are rejected" but "no input can produce a key that escapes" — which
// is why the hostile list is checked as a whole rather than one assertion per
// string.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

#include "simple_http.h"

using namespace simple_http;

namespace {

// Decodes and returns the key, or an empty optional with `err` filled in.
std::optional<std::string> decode(std::string_view raw, PathError& err) { return decode_and_normalize(raw, err); }

}  // namespace

TEST_CASE("url_path: normalization folds the forms that mean the same file", "[url_path]") {
    PathError err{};
    auto ok = [&](std::string_view raw) {
        auto out = decode(raw, err);
        REQUIRE(out.has_value());
        return *out;
    };

    CHECK(ok("/") == "/");
    CHECK(ok("/a") == "/a");
    CHECK(ok("/a/b") == "/a/b");
    CHECK(ok("/a/b/") == "/a/b");      // trailing slash is not a different resource
    CHECK(ok("//a//b") == "/a/b");     // empty segments collapse
    CHECK(ok("/a/./b") == "/a/b");     // "." segments collapse
    // ".." is NOT folded here: a surviving one is refused outright rather than
    // resolved, which is the next test case.
}

TEST_CASE("url_path: a surviving .. is a traversal, wherever it hides", "[url_path]") {
    PathError err{};

    // Refused outright rather than resolved: resolving is only safe if the
    // concatenation it feeds is safe too, and refusing has a smaller proof
    // surface.
    for (std::string_view raw : {"/..", "/../x", "/a/../b", "/a/..", "/%2e%2e/x", "/%2E%2E/x", "/%2e./x"}) {
        INFO("raw: " << raw);
        auto out = decode(raw, err);
        // Either refused as traversal, or refused some other way — never a key.
        CHECK_FALSE(out.has_value());
        CHECK((err == PathError::Traversal || err == PathError::Malformed));
    }
}

TEST_CASE("url_path: an escaped separator is refused, not decoded", "[url_path]") {
    PathError err{};

    // The family this exists for: `%2e%2e%2f` decodes to "../", so a decoder
    // that decodes first and splits later sees one segment where the raw text
    // had none. Refusing the separator kills the whole family.
    struct Case {
        std::string_view raw;
        PathError expected;
    };
    const Case cases[] = {
        {"/..%2fb", PathError::Malformed},
        {"/%2f", PathError::Malformed},
        {"/a%2Fb", PathError::Malformed},
        {"/a%5cb", PathError::Malformed},   // escaped backslash
        {"/a%00b", PathError::Malformed},   // escaped NUL
        {"/a%09b", PathError::Malformed},   // escaped tab
        {"/a%1fb", PathError::Malformed},   // escaped control byte
    };
    for (const auto& c : cases) {
        INFO("raw: " << c.raw);
        auto out = decode(c.raw, err);
        CHECK_FALSE(out.has_value());
        CHECK(err == c.expected);
    }
}

TEST_CASE("url_path: only origin-form is accepted, and only in clean bytes", "[url_path]") {
    PathError err{};

    // No leading '/': rejects a relative path and, with it, absolute-form
    // ("GET http://host/x"), which would otherwise be a different parse of the
    // same target.
    CHECK_FALSE(decode("", err).has_value());
    CHECK(err == PathError::Malformed);
    CHECK_FALSE(decode("a/b", err).has_value());
    CHECK_FALSE(decode("http://host/x", err).has_value());

    // Bytes that must arrive escaped to mean themselves.
    CHECK_FALSE(decode("/a b", err).has_value());
    CHECK(err == PathError::Malformed);
    CHECK_FALSE(decode("/a\\b", err).has_value());
    CHECK(err == PathError::Malformed);
    CHECK_FALSE(decode(std::string_view{"/a\0b", 4}, err).has_value());
    CHECK(err == PathError::Malformed);

    // A truncated or malformed escape.
    CHECK_FALSE(decode("/a%", err).has_value());
    CHECK_FALSE(decode("/a%2", err).has_value());
    CHECK_FALSE(decode("/a%zz", err).has_value());
    CHECK(err == PathError::Malformed);
}

TEST_CASE("url_path: high bytes pass through unexamined", "[url_path]") {
    PathError err{};

    // UTF-8 path components are legitimate. Because the result is compared
    // against a table of real names, an overlong encoding of ".." is harmless:
    // it is simply a key that does not exist. Refusing it here would also refuse
    // every legitimate non-ASCII name.
    auto utf8 = decode("/caf\xc3\xa9", err);
    REQUIRE(utf8.has_value());
    CHECK(*utf8 == "/caf\xc3\xa9");

    auto overlong = decode("/%c0%ae%c0%ae", err);
    REQUIRE(overlong.has_value());
    CHECK(*overlong == "/\xc0\xae\xc0\xae");  // a key, not a traversal

    auto percent = decode("/a%25b", err);  // an escaped '%' is just a '%'
    REQUIRE(percent.has_value());
    CHECK(*percent == "/a%b");
}

TEST_CASE("url_path: the bounds are enforced before anything is built", "[url_path]") {
    PathError err{};

    CHECK_FALSE(decode("/" + std::string(kMaxPathBytes, 'a'), err).has_value());
    CHECK(err == PathError::TooLong);

    // One segment past the filesystem's own per-component limit.
    CHECK_FALSE(decode("/" + std::string(kMaxSegmentBytes + 1, 'a'), err).has_value());
    CHECK(err == PathError::TooLong);

    std::string many;
    for (std::size_t i = 0; i <= kMaxSegments; ++i) many += "/a";
    CHECK_FALSE(decode(many, err).has_value());
    CHECK(err == PathError::TooLong);

    // Exactly at the segment limit is still fine.
    std::string at_limit;
    for (std::size_t i = 0; i < kMaxSegments; ++i) at_limit += "/a";
    CHECK(decode(at_limit, err).has_value());
}

TEST_CASE("url_path: under_prefix compares whole segments", "[url_path]") {
    // Equal, or continuing on a boundary.
    CHECK(under_prefix("/api", "/api"));
    CHECK(under_prefix("/api/x", "/api"));
    CHECK(under_prefix("/api/x/y", "/api"));

    // The reason this function exists: a plain starts_with would match these.
    CHECK_FALSE(under_prefix("/apixyz", "/api"));
    CHECK_FALSE(under_prefix("/ap", "/api"));

    // A prefix that already ends in '/' is a boundary by itself — without this
    // case "/assets/" would never match "/assets/app.js", whose next character
    // is 'a' rather than '/'.
    CHECK(under_prefix("/assets/app.js", "/assets/"));
    CHECK(under_prefix("/assets/", "/assets/"));

    // ...and the case that falls out of the size comparison: "/assets" is
    // shorter than "/assets/", so it does not match. Pinned because it is not
    // obvious at a glance.
    CHECK_FALSE(under_prefix("/assets", "/assets/"));

    // An empty prefix matches nothing rather than everything.
    CHECK_FALSE(under_prefix("/anything", ""));
}
