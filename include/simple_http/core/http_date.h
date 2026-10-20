#pragma once

// HTTP dates (IMF-fixdate, RFC 9110 §5.6.7): formatting, parsing, and the
// current time as Unix seconds.
//
// This is here rather than left to each caller because the failure mode is
// silent. A `Last-Modified` that the sender believes and the receiver refuses
// looks like a missing validator, not like a bug: nothing errors, the cache
// just stops working. Both directions are therefore hand-written, with no
// locale and no library that could disagree about what a month is called.

#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

namespace simple_http {

namespace detail {

// Rendered from these rather than through strftime: strftime formats the names
// in the process locale, and a locale that spells "Nov" differently produces an
// invalid HTTP date. Every real server sends this header, so a malformed one
// stands out more than a missing one.
inline constexpr std::string_view kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
inline constexpr std::string_view kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// kTwoDigits[2 * n] is the tens digit of n, kTwoDigits[2 * n + 1] the units.
// One 2-byte memcpy replaces the two `v / 10` / `v % 10` pairs that a
// hand-rolled digits loop would need.
inline constexpr char kTwoDigits[201] = "00010203040506070809"
                                        "10111213141516171819"
                                        "20212223242526272829"
                                        "30313233343536373839"
                                        "40414243444546474849"
                                        "50515253545556575859"
                                        "60616263646566676869"
                                        "70717273747576777879"
                                        "80818283848586878889"
                                        "90919293949596979899";

inline void write_two_digits(char *out, unsigned v) {
    // Precondition: v < 100.
    std::memcpy(out, kTwoDigits + v * 2, 2);
}

struct CivilDate {
    std::int64_t year;
    unsigned month; // 1..12
    unsigned day;   // 1..31
};

// Days since 1970-01-01 to a civil date, without gmtime_r or time_t. This is
// Howard Hinnant's `civil_from_days`; all divisions are by compile-time
// constants, so they become multiply-shift sequences, and the result does not
// depend on the platform's time_t width (Windows' 32-bit time_t would otherwise
// cap dates at 2038).
inline CivilDate civil_from_days(std::int64_t z) {
    z += 719468; // shift the epoch to 0000-03-01
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);               // [0, 146096]
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100); // [0, 365]
    const unsigned mp = (5 * doy + 2) / 153;                      // [0, 11]
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;              // [1, 31]
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;                 // [1, 12]
    return {y + (m <= 2), m, d};
}

// Locale-independent equivalent of gmtime_r + snprintf for an IMF-fixdate.
inline constexpr std::int64_t kMinHttpDate = -62167219200LL; // 0000-01-01T00:00:00Z
inline constexpr std::int64_t kMaxHttpDate = 253402300799LL; // 9999-12-31T23:59:59Z

} // namespace detail

// The current time in Unix seconds.
inline std::int64_t now_unix() {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

// Writes exactly 29 bytes ("Sun, 06 Nov 1994 08:49:37 GMT") to `out`; no
// allocation, no locale, no stdio. Values outside the IMF-fixdate four-digit
// year range are clamped to 0000-01-01 / 9999-12-31 so the output length is
// always exactly 29 bytes.
inline void http_date_to(char *out, std::int64_t unix_seconds) {
    if (unix_seconds < detail::kMinHttpDate) {
        unix_seconds = detail::kMinHttpDate;
    } else if (unix_seconds > detail::kMaxHttpDate) {
        unix_seconds = detail::kMaxHttpDate;
    }

    // Floor division: C++ truncates toward zero, which would put pre-1970
    // timestamps on the wrong day and give the wrong time-of-day as well.
    std::int64_t days = unix_seconds / 86400;
    std::int64_t second_of_day = unix_seconds % 86400;
    if (second_of_day < 0) {
        second_of_day += 86400;
        --days;
    }

    // 1970-01-01 was a Thursday, and kDays is Sunday-first.
    std::int64_t wday = (days + 4) % 7;
    if (wday < 0) {
        wday += 7;
    }

    const detail::CivilDate date = detail::civil_from_days(days);
    const unsigned hour = static_cast<unsigned>(second_of_day / 3600);
    const unsigned minute = static_cast<unsigned>((second_of_day % 3600) / 60);
    const unsigned second = static_cast<unsigned>(second_of_day % 60);
    const unsigned year = static_cast<unsigned>(date.year);

    const std::string_view day_name = detail::kDays[wday];
    const std::string_view month_name = detail::kMonths[date.month - 1];
    std::memcpy(out, day_name.data(), 3);
    out[3] = ',';
    out[4] = ' ';
    detail::write_two_digits(out + 5, date.day);
    out[7] = ' ';
    std::memcpy(out + 8, month_name.data(), 3);
    out[11] = ' ';
    detail::write_two_digits(out + 12, year / 100);
    detail::write_two_digits(out + 14, year % 100);
    out[16] = ' ';
    detail::write_two_digits(out + 17, hour);
    out[19] = ':';
    detail::write_two_digits(out + 20, minute);
    out[22] = ':';
    detail::write_two_digits(out + 23, second);
    out[25] = ' ';
    std::memcpy(out + 26, "GMT", 3);
}

// A 29-byte IMF-fixdate that owns its bytes, so a caller that only needs a view
// (writing it into a header block, logging it) pays no heap allocation.
struct HttpDate {
    char bytes[29];

    std::string_view view() const { return {bytes, sizeof(bytes)}; }
    operator std::string_view() const { return view(); }
};

// Formats `unix_seconds` as IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT".
// Always 29 bytes; nothing here varies with locale.
inline HttpDate http_date_fmt(std::int64_t unix_seconds) {
    HttpDate date;
    http_date_to(date.bytes, unix_seconds);
    return date;
}

// Owning convenience overload. Prefer http_date_to / http_date_fmt on a hot
// path: 29 bytes do not fit in libstdc++'s 15-byte SSO buffer, so this
// allocates.
inline std::string http_date(std::int64_t unix_seconds) {
    std::string out(29, '\0');
    http_date_to(out.data(), unix_seconds);
    return out;
}

// The current time formatted as an IMF-fixdate, cached per thread and per
// second. Date has one-second resolution, so formatting it on every response
// repeats the same work tens of thousands of times; the cached view is valid
// until the next second. Callers that store the value own a copy (Headers
// does), but the expensive formatting is shared.
inline std::string_view http_date_now() {
    thread_local std::int64_t cached_second = -1;
    thread_local std::string cached;
    const std::int64_t now = now_unix();
    if (now != cached_second) {
        // resize() reuses the existing capacity after the first call, so the
        // once-per-second refresh does not allocate either.
        cached.resize(29);
        http_date_to(cached.data(), now);
        cached_second = now;
    }
    return cached;
}

// Parses an IMF-fixdate, returning Unix seconds, or nullopt if it is not one.
//
// The two obsolete formats — RFC 850's "Sunday, 06-Nov-94 08:49:37 GMT" and
// asctime's "Sun Nov  6 08:49:37 1994" — are deliberately refused rather than
// tolerated. They are the historical vehicle for cache-poisoning and
// request-smuggling tricks, and no client worth supporting sends them.
inline std::optional<std::int64_t> parse_http_date(std::string_view s) {
    // "Sun, 06 Nov 1994 08:49:37 GMT"
    if (s.size() < 29)
        return std::nullopt;
    auto digits = [&](std::size_t pos, std::size_t n) -> std::optional<int> {
        if (pos + n > s.size())
            return std::nullopt;
        int v = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const char c = s[pos + i];
            if (c < '0' || c > '9')
                return std::nullopt;
            v = v * 10 + (c - '0');
        }
        return v;
    };
    if (s[3] != ',' || s[4] != ' ')
        return std::nullopt;

    auto day = digits(5, 2);
    auto year = digits(12, 4);
    auto hour = digits(17, 2);
    auto minute = digits(20, 2);
    auto second = digits(23, 2);
    if (!day || !year || !hour || !minute || !second)
        return std::nullopt;
    if (s[7] != ' ' || s[11] != ' ' || s[16] != ' ' || s[19] != ':' || s[22] != ':' || s[25] != ' ') {
        return std::nullopt;
    }

    int mon = -1;
    for (int i = 0; i < 12; ++i) {
        if (s.compare(8, 3, detail::kMonths[i]) == 0) {
            mon = i;
            break;
        }
    }
    if (mon < 0)
        return std::nullopt;

    std::tm tm{};
    tm.tm_mday = *day;
    tm.tm_mon = mon;
    tm.tm_year = *year - 1900;
    tm.tm_hour = *hour;
    tm.tm_min = *minute;
    tm.tm_sec = *second;
    // timegm is not ISO C++ (it is POSIX and glibc); the Windows spelling is
    // _mkgmtime. Both treat the fields as UTC, which is what the "GMT" in an
    // IMF-fixdate means -- mktime would apply the process timezone and shift
    // every validator by the local offset.
#if defined(_WIN32)
    const std::time_t t = _mkgmtime(&tm);
#else
    const std::time_t t = timegm(&tm);
#endif
    if (t == static_cast<std::time_t>(-1))
        return std::nullopt;
    return static_cast<std::int64_t>(t);
}

} // namespace simple_http
