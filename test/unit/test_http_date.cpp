// core/http_date.h: IMF-fixdate formatting and parsing.
//
// The round trip is the guard that matters here. A date layer can look right
// and still be wrong in a way nothing else notices — the failure mode is a
// validator the sender believes and the receiver refuses, which silently
// disables caching rather than raising anything. (The original implementation
// of this had exactly that bug: the file-time conversion produced a
// plausible-looking but completely wrong year, and the only symptom was ETags
// that never matched.)

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "simple_http.h"

using namespace simple_http;

TEST_CASE("http_date: the format is exactly what the RFC asks for", "[http_date]") {
    const std::string s = http_date(784111777); // Sun, 06 Nov 1994 08:49:37 GMT
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
    CHECK_FALSE(parse_http_date("Sunday, 06-Nov-94 08:49:37 GMT").has_value()); // RFC 850
    CHECK_FALSE(parse_http_date("Sun Nov  6 08:49:37 1994").has_value());       // asctime

    // Truncated and malformed inputs.
    CHECK_FALSE(parse_http_date("").has_value());
    CHECK_FALSE(parse_http_date("Sun, 06 Nov 1994 08:49:37").has_value()); // under 29 bytes
    CHECK_FALSE(parse_http_date("Sun, XX Nov 1994 08:49:37 GMT").has_value());
    CHECK_FALSE(parse_http_date("Sun, 06 Xxx 1994 08:49:37 GMT").has_value());
    CHECK_FALSE(parse_http_date("Sun 06 Nov 1994 08:49:37 GMT").has_value()); // comma dropped

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

TEST_CASE("http_date: pre-epoch values and the four-digit-year range", "[http_date]") {
    // The civil-date arithmetic floors negative timestamps; truncating division
    // would put -1s on 01 Jan 1970 and get the weekday wrong too.
    CHECK(http_date(-1) == "Wed, 31 Dec 1969 23:59:59 GMT");
    CHECK(http_date(-86400) == "Wed, 31 Dec 1969 00:00:00 GMT");
    CHECK(http_date(951782400) == "Tue, 29 Feb 2000 00:00:00 GMT");
    CHECK(http_date(253402300799) == "Fri, 31 Dec 9999 23:59:59 GMT");

    // IMF-fixdate is 4DIGIT (RFC 9110 §5.6.7), so values outside that range
    // saturate instead of growing past 29 bytes.
    CHECK(http_date(std::numeric_limits<std::int64_t>::min()) == "Sat, 01 Jan 0000 00:00:00 GMT");
    CHECK(http_date(std::numeric_limits<std::int64_t>::max()) == "Fri, 31 Dec 9999 23:59:59 GMT");

    // The allocation-free entry points agree with the owning one.
    char buf[29];
    http_date_to(buf, 784111777);
    CHECK(std::string_view{buf, sizeof(buf)} == "Sun, 06 Nov 1994 08:49:37 GMT");
    CHECK(http_date_fmt(784111777).view() == "Sun, 06 Nov 1994 08:49:37 GMT");
    CHECK(std::string_view{http_date_fmt(784111777)} == "Sun, 06 Nov 1994 08:49:37 GMT");
}

TEST_CASE("http_date: now_unix is in the right epoch", "[http_date]") {
    // Sanity rather than precision: this pins that now_unix() is seconds since
    // the Unix epoch and not, say, a steady-clock tick or a file-time value.
    const std::int64_t now = now_unix();
    CHECK(now > 1700000000); // after Nov 2023
    CHECK(now < 4102444800); // before 2100
}

TEST_CASE("http_date: impossible dates are refused, not normalised by timegm", "[http_date]") {
    CHECK_FALSE(parse_http_date("Sun, 31 Feb 2020 00:00:00 GMT").has_value());
    CHECK_FALSE(parse_http_date("Sun, 06 Nov 1994 99:99:99 GMT").has_value());
    CHECK_FALSE(parse_http_date("Sun, 06 Nov 1994 24:00:00 GMT").has_value());

    // -1 is a real timestamp (1969-12-31T23:59:59Z), not only timegm's error.
    auto epoch = parse_http_date("Wed, 31 Dec 1969 23:59:59 GMT");
    REQUIRE(epoch.has_value());
    CHECK(*epoch == -1);
}
