#pragma once

// A C++17 stand-in for std::source_location (C++20).
//
// The important property is that `current()` is a constant-expression function
// whose default arguments are evaluated at the *call site*, so a logging macro
// expanding to `source_location::current()` records the caller's file/line even
// though current() itself lives here. GCC and Clang both provide the required
// builtins; MSVC has __FILE__/__LINE__ but no __builtin_FUNCTION, hence the
// guarded fallback.

#include <cstdint>

namespace simple_http {

struct source_location {
    const char *file_{""};
    const char *function_{""};
    std::uint_least32_t line_{0};

    static constexpr source_location current(
#if defined(__GNUC__) || defined(__clang__)
        const char *file = __builtin_FILE(), const char *function = __builtin_FUNCTION(),
        std::uint_least32_t line = __builtin_LINE()
#else
        const char *file = __FILE__, const char *function = "", std::uint_least32_t line = __LINE__
#endif
            ) noexcept {
        return source_location{file, function, line};
    }

    constexpr const char *file_name() const noexcept { return file_; }
    constexpr const char *function_name() const noexcept { return function_; }
    constexpr std::uint_least32_t line() const noexcept { return line_; }
};

} // namespace simple_http
