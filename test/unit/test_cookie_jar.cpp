// client/cookie_jar.h: RFC 6265 scoping and lifetime rules.

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>

#include "simple_http.h"

using namespace simple_http;

namespace {

void set_cookie(CookieJar &jar, std::string_view url, std::string_view value) {
    Headers headers;
    headers.add("set-cookie", std::string{value});
    jar.store(url, headers);
}

} // namespace

TEST_CASE("client/cookie_jar: a cookie without Domain is host-only", "[client][cookie]") {
    CookieJar jar;
    set_cookie(jar, "https://example.com/", "sid=secret; Path=/");

    CHECK(jar.cookie_header("https://example.com/") == "sid=secret");
    CHECK(jar.cookie_header("https://sub.example.com/").empty());
    CHECK(jar.cookie_header("https://notexample.com/").empty());
}

TEST_CASE("client/cookie_jar: a Domain cookie reaches subdomains only within it", "[client][cookie]") {
    CookieJar jar;
    set_cookie(jar, "https://example.com/", "shared=1; Domain=example.com; Path=/");

    CHECK(jar.cookie_header("https://example.com/") == "shared=1");
    CHECK(jar.cookie_header("https://app.example.com/") == "shared=1");
    CHECK(jar.cookie_header("https://notexample.com/").empty());
}

TEST_CASE("client/cookie_jar: a Domain that does not cover the request host is rejected", "[client][cookie]") {
    CookieJar jar;
    set_cookie(jar, "https://evil.example/", "bad=1; Domain=bank.example; Path=/");

    CHECK(jar.size() == 0);
    CHECK(jar.cookie_header("https://bank.example/").empty());
}

TEST_CASE("client/cookie_jar: same name/domain/path replaces the old cookie", "[client][cookie]") {
    CookieJar jar;
    set_cookie(jar, "https://example.com/", "sid=one; Path=/");
    set_cookie(jar, "https://example.com/", "sid=two; Path=/");

    CHECK(jar.size() == 1);
    CHECK(jar.cookie_header("https://example.com/") == "sid=two");
}

TEST_CASE("client/cookie_jar: Max-Age=0 and a past Expires delete the cookie", "[client][cookie]") {
    CookieJar jar;
    set_cookie(jar, "https://example.com/", "sid=one; Path=/");
    set_cookie(jar, "https://example.com/", "sid=gone; Path=/; Max-Age=0");
    CHECK(jar.size() == 0);
    CHECK(jar.cookie_header("https://example.com/").empty());

    set_cookie(jar, "https://example.com/", "old=1; Path=/");
    set_cookie(jar, "https://example.com/", "old=gone; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT");
    CHECK(jar.size() == 0);
    CHECK(jar.cookie_header("https://example.com/").empty());
}

TEST_CASE("client/cookie_jar: Max-Age wins over Expires regardless of order", "[client][cookie]") {
    CookieJar jar;
    set_cookie(jar, "https://example.com/", "m=1; Expires=Thu, 01 Jan 1970 00:00:00 GMT; Max-Age=3600; Path=/");
    CHECK(jar.cookie_header("https://example.com/") == "m=1");

    set_cookie(jar, "https://example.com/", "m=1; Max-Age=0; Expires=Wed, 01 Jan 2031 00:00:00 GMT; Path=/");
    CHECK(jar.cookie_header("https://example.com/").empty());
}

TEST_CASE("client/cookie_jar: Path defaults to the request directory and rejects relative paths", "[client][cookie]") {
    CookieJar jar;
    set_cookie(jar, "https://example.com/a/b/c", "p=1");

    CHECK(jar.cookie_header("https://example.com/a/b/x") == "p=1");
    CHECK(jar.cookie_header("https://example.com/a/x").empty());

    CookieJar relative;
    set_cookie(relative, "https://example.com/a/b/c", "p=1; Path=not-absolute");
    CHECK(relative.cookie_header("https://example.com/a/b/x") == "p=1");
    CHECK(relative.cookie_header("https://example.com/a/x").empty());
}

TEST_CASE("client/cookie_jar: Secure cookies only go over https", "[client][cookie]") {
    CookieJar jar;
    set_cookie(jar, "https://example.com/", "s=1; Secure; Path=/");

    CHECK(jar.cookie_header("https://example.com/") == "s=1");
    CHECK(jar.cookie_header("http://example.com/").empty());
}

// A single-label Domain is a public suffix: accepting it lets a hostile origin
// (evil.com) plant a cookie that this jar then sends to every .com host it
// talks to.
TEST_CASE("client/cookie_jar: a single-label Domain is rejected", "[client][cookie][security]") {
    CookieJar jar;
    set_cookie(jar, "http://evil.com/", "a=b; Domain=com; Path=/");

    CHECK(jar.size() == 0);
    CHECK(jar.cookie_header("http://victim.com/").empty());
    CHECK(jar.cookie_header("http://evil.com/").empty());
}

TEST_CASE("client/cookie_jar: the optional public-suffix predicate is honoured", "[client][cookie][security]") {
    CookieJar jar{[](std::string_view domain) { return domain == "co.uk"; }};
    set_cookie(jar, "http://evil.co.uk/", "a=b; Domain=co.uk; Path=/");
    CHECK(jar.size() == 0);
    CHECK(jar.cookie_header("http://victim.co.uk/").empty());

    set_cookie(jar, "http://evil.co.uk/", "a=b; Domain=evil.co.uk; Path=/");
    CHECK(jar.cookie_header("http://evil.co.uk/") == "a=b");
}

TEST_CASE("client/cookie_jar: 127.evil.com is a DNS name, not loopback", "[client][cookie][security]") {
    CookieJar jar;
    set_cookie(jar, "https://127.evil.com/", "s=1; Secure; Path=/");

    CHECK(jar.cookie_header("https://127.evil.com/") == "s=1");
    CHECK(jar.cookie_header("http://127.evil.com/").empty());
}

TEST_CASE("client/cookie_jar: __Host-/__Secure- prefixes are enforced", "[client][cookie][security]") {
    CookieJar jar;
    set_cookie(jar, "https://example.com/", "__Host-a=1; Path=/; Secure");
    CHECK(jar.cookie_header("https://example.com/") == "__Host-a=1");

    // __Host- forbids Domain, requires Secure and Path=/; __Secure- requires
    // Secure. Violations are discarded, not downgraded.
    set_cookie(jar, "https://example.com/", "__Host-b=1; Domain=example.com; Path=/; Secure");
    set_cookie(jar, "https://example.com/", "__Host-c=1; Path=/");
    set_cookie(jar, "https://example.com/", "__Secure-d=1; Path=/");
    CHECK(jar.size() == 1);

    set_cookie(jar, "https://example.com/", "__Secure-e=1; Path=/; Secure");
    CHECK(jar.cookie_header("https://example.com/").find("__Secure-e=1") != std::string::npos);
}

TEST_CASE("client/cookie_jar: a malformed Set-Cookie is ignored", "[client][cookie][security]") {
    CookieJar jar;
    // No '=' in the first ';'-separated segment: the attribute's '=' must not
    // be used to split the name/value.
    set_cookie(jar, "https://example.com/", "novalue; Path=/");
    CHECK(jar.size() == 0);
    CHECK(jar.cookie_header("https://example.com/").empty());

    set_cookie(jar, "https://example.com/", "bad name=v; Path=/");
    CHECK(jar.size() == 0);
}
