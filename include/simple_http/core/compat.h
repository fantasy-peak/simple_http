#pragma once

// C++17 compatibility shims used across the cpp17 port.
//
// The C++23 implementation relies on a handful of library facilities that only
// became standard later. They are collected here so the ported headers do not
// each grow their own copy:
//
//   * std::span            -> simple_http::span (std::span under C++20,
//                             a minimal view in core/span.h under C++17)
//   * string_view::starts_with / ends_with / contains
//                          -> free functions below
//   * std::remove_cvref_t  -> detail::remove_cvref_t
//
// Nothing here owns state; these are header-only helpers.

#include <boost/asio/any_io_executor.hpp>
#include <cstddef>
#include <string_view>
#include <type_traits>
#include <utility>

#include "span.h"

namespace simple_http {

namespace detail {

// Wraps a hand-written initiation so it exposes the connection executor.
// Cancellation tokens such as asio::cancel_after ask the initiation for
// `executor_type`/`get_executor()` (they construct their timer on it), which a
// plain lambda does not provide.
template <typename F> struct executor_initiation {
    using executor_type = boost::asio::any_io_executor;
    F fn;
    executor_type ex;

    executor_type get_executor() const noexcept { return ex; }
    template <typename Handler> void operator()(Handler &&handler) { fn(std::forward<Handler>(handler)); }
};

template <typename F>
executor_initiation<std::decay_t<F>> make_executor_initiation(F &&fn, boost::asio::any_io_executor ex) {
    return {std::forward<F>(fn), std::move(ex)};
}

} // namespace detail

// Byte views are the transport layer's public currency. `span` is std::span on
// C++20 and the minimal view from core/span.h on C++17; asio buffers are built
// from data()/size() explicitly rather than relying on asio::buffer(span).
using ByteSpan = span<std::byte>;
using ConstByteSpan = span<const std::byte>;

inline bool starts_with(std::string_view text, std::string_view prefix) noexcept {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

inline bool ends_with(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Generic membership test for associative containers. The expression is only
// viable for containers that expose find(), so it cannot silently accept a
// type that has no membership notion.
template <typename Container, typename Key>
auto contains(const Container &container, const Key &key) -> decltype(container.find(key) != container.end()) {
    return container.find(key) != container.end();
}

namespace detail {

template <typename T> struct remove_cvref {
    using type = std::remove_cv_t<std::remove_reference_t<T>>;
};
template <typename T> using remove_cvref_t = typename remove_cvref<T>::type;

} // namespace detail

} // namespace simple_http
