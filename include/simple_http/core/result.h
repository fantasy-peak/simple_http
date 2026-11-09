#pragma once

// Value-or-error for synchronous APIs, C++17 style.
//
// Async operations do not use this: their completion signature is the usual
// `void(error_code, T...)`, and the caller's token decides whether an error is
// a value (callback / as_tuple) or an exception (use_awaitable). A few
// synchronous parsers still need to return either a value or an error, and
// Boost.System already provides exactly that shape -- result<T, E>, which is
// C++11-compatible and shares error_code with the rest of the library.
//
// Usage mirrors std::expected closely enough for a mechanical port:
//   result<Url> r = parse_url(text);
//   if (!r) return r.error();
//   use(*r);
// Note that result::value() throws bad_result_access when empty; callers in
// this library always check operator bool first (the port must stay
// exception-free internally).

#include <boost/system/result.hpp>

#include "types.h"

namespace simple_http {

template <typename T, typename E = error_code> using result = boost::system::result<T, E>;

} // namespace simple_http
