// core/accept_encoding.h: which content codings a client will take, and at what
// quality.
//
// The properties worth pinning here are the two that a substring search gets
// wrong: an explicit q=0 is a *refusal* rather than an absence, and identity has
// a default that only "*;q=0" overrides.

#include <catch2/catch_test_macros.hpp>

#include <string_view>

#include "simple_http.h"

using namespace simple_http;

TEST_CASE("accept_encoding: an absent header accepts everything by default", "[accept_encoding]") {
    const AcceptEncoding ae = parse_accept_encoding("");

    // "Not mentioned" is -1, not 0: the caller resolves the default, and for
    // identity that default is "acceptable" (RFC 9110 §12.5.3).
    CHECK(ae.br == -1.0);
    CHECK(ae.gzip == -1.0);
    CHECK(ae.wildcard == -1.0);
    CHECK(ae.identity == -1.0);

    CHECK(identity_q(ae) == 1.0);
    CHECK(coding_q(ae.br, ae) == -1.0);  // nothing to say about brotli
}

TEST_CASE("accept_encoding: a listed coding is accepted at q=1 by default", "[accept_encoding]") {
    const AcceptEncoding ae = parse_accept_encoding("gzip, deflate, br");
    CHECK(coding_q(ae.gzip, ae) == 1.0);
    CHECK(coding_q(ae.br, ae) == 1.0);
    CHECK(identity_q(ae) == 1.0);
}

TEST_CASE("accept_encoding: q=0 is a refusal, not an absence", "[accept_encoding]") {
    // The bug this guards: a substring search for "br" finds it and hands the
    // client a brotli body it just said it cannot decode.
    const AcceptEncoding ae = parse_accept_encoding("gzip, br;q=0");
    CHECK(coding_q(ae.br, ae) == 0.0);
    CHECK(coding_q(ae.gzip, ae) == 1.0);

    // Explicit values win over the wildcard.
    const AcceptEncoding mixed = parse_accept_encoding("br;q=0.2, gzip;q=0.8, *;q=0.1");
    CHECK(coding_q(mixed.br, mixed) == 0.2);
    CHECK(coding_q(mixed.gzip, mixed) == 0.8);
}

TEST_CASE("accept_encoding: identity has a default that only a wildcard can refuse", "[accept_encoding]") {
    // Unmentioned: acceptable.
    CHECK(identity_q(parse_accept_encoding("gzip")) == 1.0);

    // "*;q=0" refuses everything not named, identity included.
    CHECK(identity_q(parse_accept_encoding("*;q=0")) == 0.0);

    // An explicit value wins over the wildcard, in both directions.
    CHECK(identity_q(parse_accept_encoding("identity;q=0")) == 0.0);
    CHECK(identity_q(parse_accept_encoding("identity;q=0, *;q=1")) == 0.0);
    CHECK(identity_q(parse_accept_encoding("identity;q=0.5, *;q=0")) == 0.5);

    // A lone "*" is not a refusal of identity.
    CHECK(identity_q(parse_accept_encoding("*")) == 1.0);
    CHECK(coding_q(parse_accept_encoding("*").br, parse_accept_encoding("*")) == 1.0);
}

TEST_CASE("accept_encoding: coding names are matched case-insensitively", "[accept_encoding]") {
    // RFC 9110 §8.4.1: content-coding names are case-insensitive.
    const AcceptEncoding upper = parse_accept_encoding("GZIP, BR");
    CHECK(coding_q(upper.gzip, upper) == 1.0);
    CHECK(coding_q(upper.br, upper) == 1.0);

    // x-gzip is the historical spelling of gzip.
    const AcceptEncoding legacy = parse_accept_encoding("x-gzip");
    CHECK(coding_q(legacy.gzip, legacy) == 1.0);
}

TEST_CASE("accept_encoding: a malformed qvalue", "[accept_encoding]") {
    // Several shapes, three different outcomes. Recorded rather than endorsed:
    // the port kept the behaviour it had, and pinning it means any future change
    // has to be deliberate.
    //
    // Non-numeric from the first character: the scan stops immediately and `q`
    // keeps its default of 1.0.
    CHECK(parse_accept_encoding("br;q=abc").br == 1.0);

    // std::stod parses as much as it can and stops rather than failing, so
    // trailing garbage yields the numeric prefix.
    CHECK(parse_accept_encoding("br;q=1.2.3").br == 1.2);

    // A bare "q" with no '=' is ignored entirely, leaving the default.
    CHECK(parse_accept_encoding("br;q").br == 1.0);

    // Digits-and-dots that std::stod still rejects — the only shape that
    // reaches the catch and produces 0.
    CHECK(parse_accept_encoding("br;q=.").br == 0.0);
}
