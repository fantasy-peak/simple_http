// Pure-logic pieces of the 0-RTT client path: the wire methods, the Request
// flags a replay must preserve, and the single-use session cache.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>

#include <openssl/ssl.h>

#include "simple_http.h"
#include <simple_http/client/h3_session_cache.h>
#include <simple_http/quic/tls.h>

#ifdef SIMPLE_HTTP_ENABLE_HTTP3

using namespace simple_http;

TEST_CASE("0-RTT method sentinels serialize as GET/HEAD", "[h3][0rtt]") {
    CHECK(to_string(Method::Get0RTT) == "GET");
    CHECK(to_string(Method::Head0RTT) == "HEAD");
    CHECK(wire_method(Method::Get0RTT) == Method::Get);
    CHECK(wire_method(Method::Head0RTT) == Method::Head);
    CHECK(is_0rtt(Method::Get0RTT));
    CHECK(is_0rtt(Method::Head0RTT));
    CHECK_FALSE(is_0rtt(Method::Get));
    CHECK(is_idempotent(Method::Get0RTT));
    CHECK(is_idempotent(Method::Head0RTT));

    // The sentinels are API-only: parsing can never produce one.
    CHECK(method_from_string("GET_0RTT") == Method::Unknown);
    CHECK(method_from_string("GET") == Method::Get);
    CHECK(method_from_string("HEAD") == Method::Head);
}

TEST_CASE("Request normalizes 0-RTT sentinels and preserves the opt-in", "[h3][0rtt]") {
    asio::io_context io;
    Request req(Version::Http11, io.get_executor());
    req.set_method(Method::Head0RTT);
    CHECK(req.method() == Method::Head);
    CHECK(req.method_token() == "HEAD");
    req.set_allow_0rtt(true);
    CHECK(req.allow_0rtt());

    // Moving preserves both flags; a replay keeps the opt-in but is not itself
    // early data.
    Request moved(std::move(req));
    CHECK(moved.method() == Method::Head);
    CHECK(moved.allow_0rtt());

    auto replay = moved.clone_for_replay(io.get_executor());
    CHECK(replay->method() == Method::Head);
    CHECK(replay->allow_0rtt());
    CHECK_FALSE(replay->used_0rtt());
}

TEST_CASE("Request used_0rtt survives a move", "[h3][0rtt]") {
    asio::io_context io;
    Request req(Version::Http11, io.get_executor());
    req.set_used_0rtt(true);
    Request moved(std::move(req));
    CHECK(moved.used_0rtt());
}

TEST_CASE("incompatible ticket appdata still asks for a fresh ticket", "[h3][0rtt][tls]") {
    using quic::ticket_return_code;

    // No ticket, or one this process cannot decrypt: full handshake, and the
    // client still needs a new ticket so it can try 0-RTT again.
    CHECK(ticket_return_code(SSL_TICKET_EMPTY, false) == SSL_TICKET_RETURN_IGNORE_RENEW);
    CHECK(ticket_return_code(SSL_TICKET_NO_DECRYPT, false) == SSL_TICKET_RETURN_IGNORE_RENEW);

    // Decryptable but with a stale HTTP/3 SETTINGS snapshot: resumption is
    // refused, yet the return code must renew. A bare IGNORE would leave the
    // client replaying the stale ticket until it expires.
    CHECK(ticket_return_code(SSL_TICKET_SUCCESS, false) == SSL_TICKET_RETURN_IGNORE_RENEW);
    CHECK(ticket_return_code(SSL_TICKET_SUCCESS_RENEW, false) == SSL_TICKET_RETURN_IGNORE_RENEW);

    // Compatible appdata: resume normally.
    CHECK(ticket_return_code(SSL_TICKET_SUCCESS, true) == SSL_TICKET_RETURN_USE);
    CHECK(ticket_return_code(SSL_TICKET_SUCCESS_RENEW, true) == SSL_TICKET_RETURN_USE_RENEW);
}

TEST_CASE("H3 session cache is single-use and expires", "[h3][0rtt]") {
    detail::H3SessionCache cache;

    auto make_entry = [](std::chrono::seconds ttl) {
        detail::H3SessionEntry entry;
        entry.session = std::shared_ptr<SSL_SESSION>(SSL_SESSION_new(), SSL_SESSION_free);
        entry.transport_params = {1, 2, 3};
        entry.expires = std::chrono::steady_clock::now() + ttl;
        return entry;
    };

    cache.put("https://example.test:443", make_entry(std::chrono::seconds(60)));
    CHECK(cache.size() == 1);

    auto first = cache.take("https://example.test:443");
    REQUIRE(first.has_value());
    REQUIRE(first->session != nullptr);
    CHECK(first->transport_params == std::vector<std::uint8_t>{1, 2, 3});
    CHECK(cache.size() == 0);
    CHECK_FALSE(cache.take("https://example.test:443").has_value());

    cache.put("expired", make_entry(std::chrono::seconds(-1)));
    CHECK_FALSE(cache.take("expired").has_value());
    CHECK_FALSE(cache.take("missing").has_value());
}

#endif
