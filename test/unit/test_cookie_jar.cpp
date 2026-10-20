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
