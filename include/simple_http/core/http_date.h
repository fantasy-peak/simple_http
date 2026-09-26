#pragma once

// HTTP dates (IMF-fixdate, RFC 9110 §5.6.7): formatting, parsing, and the
// current time as Unix seconds.
//
// This is here rather than left to each caller because the failure mode is
// silent. A `Last-Modified` that the sender believes and the receiver refuses
// looks like a missing validator, not like a bug: nothing errors, the cache just
// stops working. Both directions are therefore hand-written, with no locale and
// no library that could disagree about what a month is called.

#include <chrono>
#include <cstdint>
#include <cstdio>
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

}  // namespace detail

// The current time in Unix seconds.
inline std::int64_t now_unix() {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

// Formats `unix_seconds` as IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT".
// Always 29 bytes; nothing here varies with locale.
inline std::string http_date(std::int64_t unix_seconds) {
    const std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    const int wday = tm.tm_wday < 0 || tm.tm_wday > 6 ? 0 : tm.tm_wday;
    const int mon = tm.tm_mon < 0 || tm.tm_mon > 11 ? 0 : tm.tm_mon;

    // 96, not 32: the compiler cannot prove the %.3s fields are short (they come
    // from string_views), so it assumes a worst case far larger than the real
    // 29-byte output and warns under -Wformat-truncation.
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%.3s, %02d %.3s %04d %02d:%02d:%02d GMT", detail::kDays[wday].data(),
                  tm.tm_mday, detail::kMonths[mon].data(), tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string{buf};
}

// Parses an IMF-fixdate, returning Unix seconds, or nullopt if it is not one.
//
// The two obsolete formats — RFC 850's "Sunday, 06-Nov-94 08:49:37 GMT" and
// asctime's "Sun Nov  6 08:49:37 1994" — are deliberately refused rather than
// tolerated. They are the historical vehicle for cache-poisoning and
// request-smuggling tricks, and no client worth supporting sends them.
inline std::optional<std::int64_t> parse_http_date(std::string_view s) {
    // "Sun, 06 Nov 1994 08:49:37 GMT"
    if (s.size() < 29) return std::nullopt;
    auto digits = [&](std::size_t pos, std::size_t n) -> std::optional<int> {
        if (pos + n > s.size()) return std::nullopt;
        int v = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const char c = s[pos + i];
            if (c < '0' || c > '9') return std::nullopt;
            v = v * 10 + (c - '0');
        }
        return v;
    };
    if (s[3] != ',' || s[4] != ' ') return std::nullopt;

    auto day = digits(5, 2);
    auto year = digits(12, 4);
    auto hour = digits(17, 2);
    auto minute = digits(20, 2);
    auto second = digits(23, 2);
    if (!day || !year || !hour || !minute || !second) return std::nullopt;
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
    if (mon < 0) return std::nullopt;

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
    if (t == static_cast<std::time_t>(-1)) return std::nullopt;
    return static_cast<std::int64_t>(t);
}

}  // namespace simple_http
