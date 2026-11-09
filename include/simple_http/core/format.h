#pragma once

// A deliberately small `{}` formatter for the C++17 port.
//
// The C++23 implementation uses std::format. The only format strings this
// library writes are log messages and a couple of protocol strings, and they
// use the unannotated `{}` placeholder exclusively (no width, precision, index
// or type specifier). Reimplementing that subset keeps the call sites and the
// logging facade unchanged and avoids pulling in a third-party formatter.
//
// Supported: `{}` (next argument), `{{` / `}}` (literal braces). Arguments may
// be arithmetic, bool, char, any string-like type, or anything streamable via
// operator<<. Anything else renders as `<value>`.

#include <charconv>
#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace simple_http {
namespace detail {

inline void append_arg(std::string &out, std::string_view value) { out.append(value.data(), value.size()); }
inline void append_arg(std::string &out, const std::string &value) { out.append(value); }
inline void append_arg(std::string &out, const char *value) {
    if (value)
        out.append(value);
}
inline void append_arg(std::string &out, char value) { out.push_back(value); }
inline void append_arg(std::string &out, bool value) { out.append(value ? "true" : "false"); }

template <std::size_t N> void append_arg(std::string &out, const char (&value)[N]) { out.append(value, N - 1); }

template <typename T, typename = std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T, bool>>>
void append_arg(std::string &out, T value) {
    if constexpr (std::is_integral_v<T>) {
        char buffer[64];
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        out.append(buffer, static_cast<std::size_t>(result.ptr - buffer));
    } else {
        std::ostringstream stream;
        stream << value;
        out.append(stream.str());
    }
}

template <typename T> struct is_streamable {
    template <typename U>
    static auto test(int) -> decltype(std::declval<std::ostream &>() << std::declval<const U &>(), std::true_type{});
    template <typename> static std::false_type test(...);
    static constexpr bool value = decltype(test<T>(0))::value;
};

template <typename T, typename = std::enable_if_t<
                          !std::is_arithmetic_v<std::decay_t<T>> && !std::is_same_v<std::decay_t<T>, bool> &&
                          !std::is_same_v<std::decay_t<T>, std::string> &&
                          !std::is_convertible_v<const T &, std::string_view> && is_streamable<T>::value>>
void append_arg(std::string &out, const T &value) {
    std::ostringstream stream;
    stream << value;
    out.append(stream.str());
}

template <
    typename T,
    typename = std::enable_if_t<!std::is_arithmetic_v<std::decay_t<T>> && !std::is_same_v<std::decay_t<T>, bool> &&
                                !std::is_same_v<std::decay_t<T>, std::string> &&
                                !std::is_convertible_v<const T &, std::string_view> && !is_streamable<T>::value>,
    typename = void>
void append_arg(std::string &out, const T &) {
    out.append("<value>");
}

inline void format_into(std::string &out, std::string_view fmt) {
    for (std::size_t i = 0; i < fmt.size(); ++i) {
        const char c = fmt[i];
        if (c == '{' && i + 1 < fmt.size() && fmt[i + 1] == '{') {
            out.push_back('{');
            ++i;
        } else if (c == '}' && i + 1 < fmt.size() && fmt[i + 1] == '}') {
            out.push_back('}');
            ++i;
        } else {
            out.push_back(c);
        }
    }
}

template <typename T, typename... Rest>
void format_into(std::string &out, std::string_view fmt, T &&value, Rest &&...rest) {
    for (std::size_t i = 0; i < fmt.size(); ++i) {
        const char c = fmt[i];
        if (c == '{' && i + 1 < fmt.size() && fmt[i + 1] == '{') {
            out.push_back('{');
            ++i;
        } else if (c == '}' && i + 1 < fmt.size() && fmt[i + 1] == '}') {
            out.push_back('}');
            ++i;
        } else if (c == '{' && i + 1 < fmt.size() && fmt[i + 1] == '}') {
            append_arg(out, std::forward<T>(value));
            format_into(out, fmt.substr(i + 2), std::forward<Rest>(rest)...);
            return;
        } else {
            out.push_back(c);
        }
    }
    // More arguments than placeholders: the extras are intentionally ignored,
    // matching the lenient behaviour the old hand-written messages assumed.
}

} // namespace detail

template <typename... Args> std::string format(std::string_view fmt, Args &&...args) {
    std::string out;
    out.reserve(fmt.size() + 32);
    detail::format_into(out, fmt, std::forward<Args>(args)...);
    return out;
}

} // namespace simple_http
