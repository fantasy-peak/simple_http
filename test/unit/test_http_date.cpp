// core/http_date.h: IMF-fixdate formatting and parsing.
//
// The round trip is the guard that matters here. A date layer can look right and
// still be wrong in a way nothing else notices — the failure mode is a validator
// the sender believes and the receiver refuses, which silently disables caching
// rather than raising anything. (The original implementation of this had
// exactly that bug: the file-time conversion produced a plausible-looking but
// completely wrong year, and the only symptom was ETags that never matched.)

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include "simple_http.h"

using namespace simple_http;

TEST_CASE("http_date: the format is exactly what the RFC asks for", "[http_date]") {
    const std::string s = http_date(784111777);  // Sun, 06 Nov 1994 08:49:37 GMT
    CHECK(s == "Sun, 06 Nov 1994 08:49:37 GMT");
    CHECK(s.size() == 29);
    CHECK(s.substr(26) == "GMT");
    CHECK(s.substr(25) == " GMT");
    CHECK(s[3] == ',');
    CHECK(s[4] == ' ');
}

TEST_CASE("http_date: parsing round-trips, across the awkward values", "[http_date]") {
    // The epoch, a leap day, the 32-bit boundary and a far-future date — the
    // set that catches an epoch mismatch, a leap-year bug and an overflow.
    const std::int64_t values[] = {
        0,           // Thu, 01 Jan 1970
        951782400,   // Tue, 29 Feb 2000 — a leap day in a century year
        2147483647,  // Tue, 19 Jan 2038 — the 32-bit time_t boundary
        253402300799 // Fri, 31 Dec 9999 — the year limit of a four-digit date
    };
    for (std::int64_t t : values) {
        INFO("unix: " << t);
        const std::string formatted = http_date(t);
        auto parsed = parse_http_date(formatted);
        REQUIRE(parsed.has_value());
        CHECK(*parsed == t);
    }
}

TEST_CASE("http_date: parsing rejects what it should not accept", "[http_date]") {
    // The two obsolete formats. They are the historical vehicle for cache
    // poisoning and request smuggling, and no client worth supporting sends them.
    CHECK_FALSE(parse_http_date("Sunday, 06-Nov-94 08:49:37 GMT").has_value());  // RFC 850
    CHECK_FALSE(parse_http_date("Sun Nov  6 08:49:37 1994").has_value());        // asctime

    // Truncated and malformed inputs.
    CHECK_FALSE(parse_http_date("").has_value());
    CHECK_FALSE(parse_http_date("Sun, 06 Nov 1994 08:49:37").has_value());  // under 29 bytes
    CHECK_FALSE(parse_http_date("Sun, XX Nov 1994 08:49:37 GMT").has_value());
    CHECK_FALSE(parse_http_date("Sun, 06 Xxx 1994 08:49:37 GMT").has_value());
    CHECK_FALSE(parse_http_date("Sun 06 Nov 1994 08:49:37 GMT").has_value());  // comma dropped

    // The parser reads the first 29 bytes and does not look at what follows, nor
    // at the "GMT" literal itself. Recorded rather than endorsed: it is not a
    // smuggling vector here because the result is a number rather than a string
    // that gets echoed back, and tightening it would reject dates some clients
    // length-pad. Pinned so a change is deliberate.
    CHECK(parse_http_date("Sun, 06 Nov 1994 08:49:37 UTC").has_value());
    CHECK(parse_http_date("Sun, 06 Nov 1994 08:49:37 GMTX").has_value());
    CHECK(parse_http_date("Sun, 06 Nov 1994 08:49:37 XXX").has_value());

    // A real value with the day-of-week wrong is still accepted: the weekday is
    // redundant, and refusing it would reject dates from clients that compute it
    // differently.
    CHECK(parse_http_date("Mon, 06 Nov 1994 08:49:37 GMT").has_value());
}

TEST_CASE("http_date: now_unix is in the right epoch", "[http_date]") {
    // Sanity rather than precision: this pins that now_unix() is seconds since
    // the Unix epoch and not, say, a steady-clock tick or a file-time value.
    const std::int64_t now = now_unix();
    CHECK(now > 1700000000);   // after Nov 2023
    CHECK(now < 4102444800);   // before 2100
}
