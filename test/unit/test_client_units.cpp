// client/: the pure parts — URL parsing, targets and policy, the TLS knobs that
// can be checked without a handshake, the h2c settings payload, error codes.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include "simple_http.h"
#include "test_support.h"

using namespace simple_http;
using namespace simple_http::test;

// --- URL parsing -------------------------------------------------------------

TEST_CASE("client/url: scheme, authority and target", "[client]") {
    {
        auto url = parse_url("http://example.com/a/b?c=d");
        REQUIRE(url.has_value());
        CHECK(url->scheme == "http");
        CHECK(url->host == "example.com");
        CHECK(url->port == 0);
        CHECK(url->effective_port() == 80);
        CHECK(url->target == "/a/b?c=d");
        CHECK(url->path() == "/a/b");
        CHECK(url->query() == "c=d");
        CHECK_FALSE(url->use_tls());
    }
    {
        auto url = parse_url("https://example.com:8443");
        REQUIRE(url.has_value());
        CHECK(url->use_tls());
        CHECK(url->effective_port() == 8443);
        CHECK(url->target == "/");  // no path is still a valid origin-form target
        CHECK(url->path() == "/");
        CHECK(url->query().empty());
        CHECK(url->authority() == "example.com:8443");
    }
    {
        auto url = parse_url("HTTPS://Example.COM/");  // the scheme is case-insensitive
        REQUIRE(url.has_value());
        CHECK(url->scheme == "https");
        CHECK(url->host == "Example.COM");
        CHECK(url->effective_port() == 443);
    }
}

TEST_CASE("client/url: userinfo, fragments and odd targets", "[client]") {
    {
        auto url = parse_url("http://user:secret@example.com/x");  // userinfo is parsed, then dropped
        REQUIRE(url.has_value());
        CHECK(url->host == "example.com");
        CHECK(url->target == "/x");
    }
    {
        auto url = parse_url("http://example.com/x#frag");  // a fragment never goes on the wire
        REQUIRE(url.has_value());
        CHECK(url->target == "/x");
    }
    {
        auto url = parse_url("http://example.com?q=1");  // a bare query gets the "/" prefix
        REQUIRE(url.has_value());
        CHECK(url->target == "/?q=1");
        CHECK(url->path() == "/");
        CHECK(url->query() == "q=1");
    }
}

TEST_CASE("client/url: IPv6 literals", "[client]") {
    {
        auto url = parse_url("http://[::1]:8080/x");
        REQUIRE(url.has_value());
        CHECK(url->host == "::1");
        CHECK(url->port == 8080);
        CHECK(url->authority() == "[::1]:8080");  // bracketed on the wire
    }
    {
        auto url = parse_url("https://[2001:db8::1]/");  // no port: the scheme default
        REQUIRE(url.has_value());
        CHECK(url->host == "2001:db8::1");
        CHECK(url->effective_port() == 443);
    }
    {
        auto url = parse_url("http://[::1/x");  // unterminated bracket
        REQUIRE_FALSE(url.has_value());
    }
    {
        auto url = parse_url("http://[::1]x/y");  // junk after the bracket
        REQUIRE_FALSE(url.has_value());
    }
}

TEST_CASE("client/url: rejected inputs", "[client]") {
    // The error side of an expected may only be read when it holds one.
    auto rejection = [](std::string_view url) {
        auto parsed = parse_url(url);
        REQUIRE_FALSE(parsed.has_value());
        return parsed.error();
    };

    CHECK(rejection("") == make_error_code(client_errc::bad_url));
    CHECK(rejection("example.com/x") == make_error_code(client_errc::bad_url));  // no scheme
    CHECK(rejection("://example.com") == make_error_code(client_errc::bad_url));
    CHECK(rejection("http://") == make_error_code(client_errc::bad_url));  // no host
    CHECK(rejection("ftp://example.com/x") == make_error_code(client_errc::unsupported_scheme));
    CHECK(rejection("http://example.com:0/x") == make_error_code(client_errc::bad_url));
    CHECK(rejection("http://example.com:70000/x") == make_error_code(client_errc::bad_url));
    CHECK(rejection("http://example.com:abc/x") == make_error_code(client_errc::bad_url));
    CHECK(rejection("http://example.com:/x") == make_error_code(client_errc::bad_url));
    // Control bytes would split the request line downstream: refused at the source.
    CHECK(rejection("http://example.com/a\r\nHost: evil") == make_error_code(client_errc::bad_url));
    // A NUL is only visible with an explicit length (a const char* would stop at it).
    CHECK(rejection(std::string_view{"http://exa\0mple.com/", 19}) == make_error_code(client_errc::bad_url));
    CHECK(rejection(std::string_view{"http://example.com/a\0b", 21}) == make_error_code(client_errc::bad_url));
}

TEST_CASE("client/url: to_target carries the destination", "[client]") {
    auto url = parse_url("https://example.com:8443/x");
    REQUIRE(url.has_value());
    ClientTarget target = url->to_target();
    CHECK(target.host == "example.com");
    CHECK(target.port == 8443);
    CHECK(target.use_tls);
    CHECK(target.authority() == "example.com:8443");
    CHECK(target.sni_host() == "example.com");
    CHECK_FALSE(target.h2c_enabled());  // TLS targets never use h2c
}

// --- targets and policy ------------------------------------------------------

TEST_CASE("client/target: defaults, ports and SNI", "[client]") {
    ClientTarget plain;  // defaults to loopback
    CHECK(plain.host == "127.0.0.1");
    CHECK(plain.effective_port() == 80);
    CHECK_FALSE(plain.use_tls);

    plain.use_tls = true;
    CHECK(plain.effective_port() == 443);

    plain.sni = "backend.internal";
    CHECK(plain.sni_host() == "backend.internal");  // the override wins over the host

    ClientTarget v6;
    v6.host = "2001:db8::1";
    v6.port = 443;
    CHECK(v6.authority() == "[2001:db8::1]:443");

    ClientTarget h2c;
    CHECK(h2c.h2c_enabled());  // plaintext: h2c is on by default (the safe Upgrade mode)
    h2c.h2c = H2cMode::Off;
    CHECK_FALSE(h2c.h2c_enabled());
}

TEST_CASE("client/tls config: the ALPN list follows the version policy", "[client]") {
    const std::string http11 = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    const std::string h2 = {2, 'h', '2'};

    TlsClientConfig tls;
    ClientTarget target;
    auto wire_to_string = [](const std::vector<unsigned char>& wire) {
        return std::string{reinterpret_cast<const char*>(wire.data()), wire.size()};
    };

    target.version = HttpVersionPolicy::Auto;
    CHECK(wire_to_string(alpn_wire_list(target, tls)) == h2 + http11);  // h2 preferred
    target.version = HttpVersionPolicy::Http11;
    CHECK(wire_to_string(alpn_wire_list(target, tls)) == http11);
    target.version = HttpVersionPolicy::Http2;
    CHECK(wire_to_string(alpn_wire_list(target, tls)) == h2);

    tls.alpn = {"h3", "h2"};
    target.version = HttpVersionPolicy::Auto;
    CHECK(wire_to_string(alpn_wire_list(target, tls)) == std::string{2, 'h', '3'} + std::string{2, 'h', '2'});

    tls.alpn = {std::string(256, 'x')};  // a protocol name is a one-byte-length field
    CHECK_THROWS_AS(alpn_wire_list(target, tls), std::runtime_error);
}

TEST_CASE("client/tls config: IP literals are told apart from names", "[client]") {
    CHECK(is_ip_literal("127.0.0.1"));
    CHECK(is_ip_literal("::1"));
    CHECK(is_ip_literal("2001:db8::1"));
    CHECK_FALSE(is_ip_literal("example.com"));
    CHECK_FALSE(is_ip_literal("1.2.3"));  // not a dotted quad
    CHECK_FALSE(is_ip_literal(""));
}

TEST_CASE("client/tls config: the client context is built and validated", "[client]") {
    {
        TlsClientConfig tls;
        tls.verify_peer = false;
        CHECK(make_client_ssl_context(tls) != nullptr);  // no CA needed when not verifying
    }
    {
        TlsClientConfig tls;  // the library's own test CA and client certificate
        tls.ca_file = "./test/tls_certificates/ca_cert.pem";
        tls.cert_chain_file = "./test/tls_certificates/client_cert.pem";
        tls.private_key_file = "./test/tls_certificates/client_key.pem";
        CHECK(make_client_ssl_context(tls) != nullptr);
    }
    {
        TlsClientConfig tls;
        tls.ca_file = "./test/tls_certificates/does_not_exist.pem";
        CHECK_THROWS_AS(make_client_ssl_context(tls), std::runtime_error);
    }
    {
        TlsClientConfig tls;  // a certificate without its key is a configuration error
        tls.cert_chain_file = "./test/tls_certificates/client_cert.pem";
        CHECK_THROWS_AS(make_client_ssl_context(tls), std::runtime_error);
    }
}

// --- h2c settings ------------------------------------------------------------

TEST_CASE("client/h2c: the settings payload and its base64url form", "[client]") {
    EngineLimits limits;
    const std::string payload = h2_settings_payload(limits);

    REQUIRE(payload.size() % 6 == 0);
    auto value_of = [&](std::uint16_t id) -> std::optional<std::uint32_t> {
        for (std::size_t i = 0; i + 6 <= payload.size(); i += 6) {
            const auto got = static_cast<std::uint16_t>((static_cast<unsigned char>(payload[i]) << 8) |
                                                        static_cast<unsigned char>(payload[i + 1]));
            if (got == id)
                return codec::read_u32(payload, i + 2);
        }
        return std::nullopt;
    };
    CHECK(value_of(codec::H2_SETTINGS_MAX_CONCURRENT_STREAMS) == limits.h2_max_concurrent_streams);
    CHECK(value_of(codec::H2_SETTINGS_INITIAL_WINDOW_SIZE) == static_cast<std::uint32_t>(limits.h2_initial_window));
    CHECK(value_of(codec::H2_SETTINGS_MAX_FRAME_SIZE) == limits.h2_max_frame_size);
    CHECK(value_of(codec::H2_SETTINGS_ENABLE_PUSH) == 0u);  // we never accept server push

    // The upgrade header carries exactly these bytes, base64url (no padding).
    const std::string b64 = h2_settings_base64url(limits);
    CHECK(b64.find('=') == std::string::npos);
    CHECK(base64_url_decode(b64) == payload);
}

// --- errors and response helpers ---------------------------------------------

TEST_CASE("client/errors: codes, messages and retryability", "[client]") {
    const error_code refused = make_error_code(client_errc::stream_refused);
    CHECK(refused.category() == client_category());
    CHECK(refused.message() == "the peer did not process the request");
    CHECK(is_retryable(refused));  // the peer provably never handled it

    CHECK_FALSE(is_retryable(make_error_code(client_errc::stream_reset)));
    CHECK_FALSE(is_retryable(make_error_code(client_errc::request_timeout)));
    CHECK_FALSE(is_retryable(make_error_code(asio::error::eof)));  // transport errors are not "retryable" per se
    CHECK_FALSE(is_retryable(error_code{}));                       // success is not a retry case

    // The category is distinct from the transport's, so the two never get mixed up.
    CHECK(make_error_code(client_errc::protocol_error).category() != asio::error::get_ssl_category());
    CHECK(error_code{client_errc::bad_url} == make_error_code(client_errc::bad_url));  // enum conversion works

    CHECK(std::string{make_error_code(client_errc::unsupported_scheme).message()}.find("http") != std::string::npos);
}

TEST_CASE("client/response: ok() and header lookup", "[client]") {
    ClientResponse response;
    response.status = 204;
    CHECK(response.ok());  // 2xx: 204 is a success, it just has no body
    response.status = 200;
    CHECK(response.ok());
    response.status = 299;
    CHECK(response.ok());
    response.status = 301;
    CHECK_FALSE(response.ok());
    response.status = 404;
    CHECK_FALSE(response.ok());
    response.status = 500;
    CHECK_FALSE(response.ok());

    response.headers.add("Content-Type", "text/plain");
    CHECK(response.header("content-type") == "text/plain");
    CHECK_FALSE(response.header("missing").has_value());

    ResponseHead head;
    head.status = 200;
    head.headers.add("x-a", "1");
    CHECK(head.header("X-A") == "1");
    CHECK(head.status == 200);
    CHECK_FALSE(head.bodyless);
}
