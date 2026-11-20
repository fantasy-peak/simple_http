#pragma once

// Locale-independent parsing of HTTP qvalues (RFC 9110 §12.4.2).
//
// strtod/std::stod read the decimal point through LC_NUMERIC: in a de_DE or
// fr_FR process "0.8" parses as 0, so an Accept-Encoding client that accepts
// gzip at q=0.8 looks like it refused it and compression is silently disabled.
// This parser owns the loop, so it behaves identically under every locale.

#include <string_view>

namespace simple_http {

// Parses "0", "1", "0.8" or ".5". Stops at the first byte that is not a digit
// or '.', ignores leading SP/HTAB, and returns 0 for anything unparseable. The
// caller decides whether 0 means "refused" (it does for Accept-Encoding).
inline double parse_qvalue(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    double value = 0.0;
    bool seen_dot = false;
    double scale = 0.1;
    for (char c : s) {
        if (c >= '0' && c <= '9') {
            if (!seen_dot) {
                value = value * 10.0 + (c - '0');
            } else {
                value += (c - '0') * scale;
                scale *= 0.1;
            }
        } else if (c == '.') {
            if (seen_dot)
                break;
            seen_dot = true;
        } else {
            break;
        }
    }
    return value;
}

} // namespace simple_http
