// client/proxy.h: proxy URL parsing, NO_PROXY matching, environment selection
// and the CONNECT request/status wire format. The tunnel handshakes themselves
// are exercised end-to-end in test/client.cpp against in-process proxies.

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <string>
#include <vector>

#include "simple_http.h"

using namespace simple_http;
using namespace simple_http::detail;

// --- proxy URL ---------------------------------------------------------------

TEST_CASE("client/proxy: URL parsing", "[client][proxy]") {
    {
        auto p = parse_proxy_url("http://proxy.example:8080");
        REQUIRE(p.has_value());
        CHECK(p->kind == ProxyKind::Http);
        CHECK(p->host == "proxy.example");
        CHECK(p->effective_port() == 8080);
        CHECK(p->username.empty());
        CHECK(p->password.empty());
    }
    {
        auto p = parse_proxy_url("https://proxy.example");
        REQUIRE(p.has_value());
        CHECK(p->kind == ProxyKind::Https);
        CHECK(p->effective_port() == 443);
    }
    {
        auto p = parse_proxy_url("socks5://127.0.0.1");
        REQUIRE(p.has_value());
        CHECK(p->kind == ProxyKind::Socks5);
        CHECK(p->effective_port() == 1080);
    }
    {
        // socks5h is the same code path: the name always goes to the proxy.
        auto p = parse_proxy_url("socks5h://user:pass@[::1]:9999");
        REQUIRE(p.has_value());
        CHECK(p->kind == ProxyKind::Socks5);
        CHECK(p->host == "::1");
        CHECK(p->effective_port() == 9999);
        CHECK(p->username == "user");
        CHECK(p->password == "pass");
        CHECK(p->authority() == "[::1]:9999");
    }
    {
        // No scheme is HTTP, like curl.
        auto p = parse_proxy_url("proxy.internal:3128");
        REQUIRE(p.has_value());
        CHECK(p->kind == ProxyKind::Http);
        CHECK(p->host == "proxy.internal");
        CHECK(p->effective_port() == 3128);
    }
    {
        // A path/query is ignored: proxies are authority-only.
        auto p = parse_proxy_url("http://proxy.example:8080/ignored?x=1");
        REQUIRE(p.has_value());
        CHECK(p->host == "proxy.example");
    }
    {
        // RFC 3986 userinfo decoding; '+' stays a plus.
        auto p = parse_proxy_url("http://u%40x:p%3Ass%2Bword@h:1");
        REQUIRE(p.has_value());
        CHECK(p->username == "u@x");
        CHECK(p->password == "p:ss+word");
    }
}

TEST_CASE("client/proxy: malformed URLs fail closed", "[client][proxy]") {
    for (const char *bad : {"", "   ", "ftp://host", "http://", "http://host:0", "http://host:99999", "http://host:abc",
                            "http://user%zz@host", "http://host:", "http://u%3Ap:pw@host"}) {
        INFO("input: '" << bad << "'");
        CHECK_FALSE(parse_proxy_url(bad).has_value());
    }
}

// --- NO_PROXY ----------------------------------------------------------------

TEST_CASE("client/proxy: NO_PROXY matching", "[client][proxy]") {
    CHECK(no_proxy_matches(parse_no_proxy("*"), "anything.example", 443));

    const auto domain = parse_no_proxy("example.com");
    CHECK(no_proxy_matches(domain, "example.com", 443));
    CHECK(no_proxy_matches(domain, "api.example.com", 443));
    CHECK(no_proxy_matches(domain, "EXAMPLE.COM", 443));
    CHECK_FALSE(no_proxy_matches(domain, "badexample.com", 443));
    CHECK_FALSE(no_proxy_matches(domain, "example.com.evil", 443));

    const auto dotted = parse_no_proxy(".example.com");
    CHECK(no_proxy_matches(dotted, "api.example.com", 80));

    const auto with_port = parse_no_proxy("example.com:8080");
    CHECK(no_proxy_matches(with_port, "example.com", 8080));
    CHECK_FALSE(no_proxy_matches(with_port, "example.com", 443));

    const auto v4 = parse_no_proxy("127.0.0.1");
    CHECK(no_proxy_matches(v4, "127.0.0.1", 80));
    CHECK_FALSE(no_proxy_matches(v4, "127.0.0.2", 80));

    const auto v6 = parse_no_proxy("[::1]:9000");
    CHECK(no_proxy_matches(v6, "::1", 9000));
    CHECK_FALSE(no_proxy_matches(v6, "::1", 9001));
    CHECK(no_proxy_matches(parse_no_proxy("::1"), "::1", 443));

    // Whitespace, empty entries, and an unparseable one are ignored.
    const auto messy = parse_no_proxy("  , example.com ,, not a host ");
    CHECK(no_proxy_matches(messy, "example.com", 80));

    // A name rule never matches an IP literal, and vice versa.
    CHECK_FALSE(no_proxy_matches(parse_no_proxy("example.com"), "127.0.0.1", 80));
}

// --- selection ---------------------------------------------------------------

namespace {

ClientTarget target(std::string host, bool tls, std::uint16_t port = 0) {
    ClientTarget t;
    t.host = std::move(host);
    t.use_tls = tls;
    t.port = port;
    return t;
}

ProxyEnvironment env_with(std::string http, std::string https, std::string all, std::string no = {}) {
    ProxyEnvironment env;
    env.http_proxy = std::move(http);
    env.https_proxy = std::move(https);
    env.all_proxy = std::move(all);
    env.no_proxy = std::move(no);
    return env;
}

} // namespace

TEST_CASE("client/proxy: explicit config wins over the environment", "[client][proxy]") {
    ClientConfig cfg;
    cfg.proxy = ProxyConfig{.kind = ProxyKind::Socks5, .host = "explicit", .port = 1080};
    auto out = select_proxy(cfg, target("example.com", true), env_with("http://env:1", "http://env:2", ""));
    REQUIRE(out.has_value());
    REQUIRE(out->has_value());
    CHECK((*out)->host == "explicit");
}

TEST_CASE("client/proxy: config no_proxy bypasses an explicit proxy", "[client][proxy]") {
    ClientConfig cfg;
    cfg.proxy = ProxyConfig{.host = "explicit"};
    cfg.no_proxy = {"example.com"};
    auto out = select_proxy(cfg, target("api.example.com", true), ProxyEnvironment{});
    REQUIRE(out.has_value());
    CHECK_FALSE(out->has_value());
}

TEST_CASE("client/proxy: scheme-specific environment, then ALL_PROXY", "[client][proxy]") {
    ClientConfig cfg;
    const auto env = env_with("http://http-proxy:1", "http://https-proxy:2", "http://all-proxy:3");

    auto http_target = select_proxy(cfg, target("example.com", false), env);
    REQUIRE(http_target.has_value());
    REQUIRE(http_target->has_value());
    CHECK((*http_target)->host == "http-proxy");

    auto https_target = select_proxy(cfg, target("example.com", true), env);
    REQUIRE(https_target.has_value());
    REQUIRE(https_target->has_value());
    CHECK((*https_target)->host == "https-proxy");

    // A missing scheme-specific value falls back to ALL_PROXY.
    const auto only_all = env_with({}, {}, "http://all-proxy:3");
    auto fallback = select_proxy(cfg, target("example.com", true), only_all);
    REQUIRE(fallback.has_value());
    REQUIRE(fallback->has_value());
    CHECK((*fallback)->host == "all-proxy");

    // Nothing configured means a direct connection.
    auto direct = select_proxy(cfg, target("example.com", true), ProxyEnvironment{});
    REQUIRE(direct.has_value());
    CHECK_FALSE(direct->has_value());

    // proxy_from_env=false never consults the environment.
    ClientConfig no_env;
    no_env.proxy_from_env = false;
    auto off = select_proxy(no_env, target("example.com", true), env);
    REQUIRE(off.has_value());
    CHECK_FALSE(off->has_value());
}

TEST_CASE("client/proxy: NO_PROXY and malformed values", "[client][proxy]") {
    ClientConfig cfg;
    // The environment's NO_PROXY bypasses an environment-selected proxy; a
    // malformed proxy URL behind the bypass is never parsed.
    auto bypass = select_proxy(cfg, target("127.0.0.1", true), env_with({}, "not a url", "", "127.0.0.1"));
    REQUIRE(bypass.has_value());
    CHECK_FALSE(bypass->has_value());

    // Without a bypass, a malformed value fails closed.
    auto malformed = select_proxy(cfg, target("example.com", true), env_with({}, "not a url", ""));
    REQUIRE_FALSE(malformed.has_value());
    CHECK(malformed.error() == make_error_code(client_errc::proxy_error));

    // The config list overrides the environment's NO_PROXY for env proxies.
    ClientConfig with_config_rules;
    with_config_rules.no_proxy = {"example.com"};
    auto config_bypass =
        select_proxy(with_config_rules, target("example.com", true), env_with({}, "http://proxy:1", "", "other.com"));
    REQUIRE(config_bypass.has_value());
    CHECK_FALSE(config_bypass->has_value());
}

TEST_CASE("client/proxy: httpoxy guard ignores uppercase HTTP_PROXY under CGI", "[client][proxy]") {
    // Save and restore the two variables this test touches, so the rest of the
    // suite sees the same environment it started with.
    const char *saved_method = std::getenv("REQUEST_METHOD");
    const char *saved_upper = std::getenv("HTTP_PROXY");
    const char *saved_lower = std::getenv("http_proxy");
    const std::string method = saved_method ? saved_method : "";
    const std::string upper = saved_upper ? saved_upper : "";
    const std::string lower = saved_lower ? saved_lower : "";
    const bool had_method = saved_method != nullptr;
    const bool had_upper = saved_upper != nullptr;
    const bool had_lower = saved_lower != nullptr;

    ::setenv("REQUEST_METHOD", "GET", 1);
    ::setenv("HTTP_PROXY", "http://upper:1", 1);
    ::unsetenv("http_proxy");
    auto env = ProxyEnvironment::from_environment();
    CHECK(env.http_proxy.empty());

    ::setenv("http_proxy", "http://lower:2", 1);
    env = ProxyEnvironment::from_environment();
    CHECK(env.http_proxy == "http://lower:2");

    // Restore.
    if (had_method)
        ::setenv("REQUEST_METHOD", method.c_str(), 1);
    else
        ::unsetenv("REQUEST_METHOD");
    if (had_upper)
        ::setenv("HTTP_PROXY", upper.c_str(), 1);
    else
        ::unsetenv("HTTP_PROXY");
    if (had_lower)
        ::setenv("http_proxy", lower.c_str(), 1);
    else
        ::unsetenv("http_proxy");
}

// --- CONNECT wire format -----------------------------------------------------

TEST_CASE("client/proxy: CONNECT request and status parsing", "[client][proxy]") {
    ProxyConfig plain{.host = "proxy", .port = 8080};
    CHECK(build_connect_request(plain, "example.com:443") ==
          "CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n\r\n");

    ProxyConfig auth{.host = "proxy", .username = "user", .password = "pass"};
    CHECK(build_connect_request(auth, "example.com:443") ==
          "CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n"
          "Proxy-Authorization: Basic dXNlcjpwYXNz\r\n\r\n");

    auto ok = parse_connect_status("HTTP/1.1 200 Connection established\r\n\r\n");
    REQUIRE(ok.has_value());
    CHECK(*ok == 200);

    auto auth_required = parse_connect_status("HTTP/1.1 407 Proxy Authentication Required\r\n\r\n");
    REQUIRE(auth_required.has_value());
    CHECK(*auth_required == 407);

    CHECK_FALSE(parse_connect_status("garbage\r\n\r\n").has_value());
    CHECK_FALSE(parse_connect_status("HTTP/1.1 20\r\n\r\n").has_value());
    CHECK_FALSE(parse_connect_status("HTTP/1.1 2000 nope\r\n\r\n").has_value());
}

// --- error codes -------------------------------------------------------------

TEST_CASE("client/proxy: error codes carry proxy-specific messages", "[client][proxy]") {
    CHECK(make_error_code(client_errc::proxy_error).message() ==
          "the proxy refused the tunnel or its reply was malformed");
    CHECK(make_error_code(client_errc::proxy_auth_failed).message() == "the proxy rejected the configured credentials");
}
