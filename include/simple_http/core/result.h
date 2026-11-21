#pragma once

// Value-or-error for synchronous APIs.
//
// Async operations do not use this: their completion signature is the usual
// `void(error_code, T...)`, and the caller's token decides whether an error is
// a value (callback / as_tuple) or an exception (use_awaitable). A few
// synchronous parsers still need to return either a value or an error, and this
// is the project's std::expected-like type for them.
//
// It is deliberately hand-rolled rather than boost::system::result: the same
// type is then available to Boost.Asio and standalone Asio builds, and the
// public API does not drag Boost.System in. Usage mirrors std::expected:
//   result<Url> r = parse_url(text);
//   if (!r) return r.error();
//   use(*r);
// value() and error() throw bad_result_access when the result is on the wrong
// side; callers in this library always check operator bool first.

#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include "types.h"

namespace simple_http {

class bad_result_access : public std::logic_error {
  public:
    bad_result_access() : std::logic_error("bad result access") {}
};

template <typename T, typename E = error_code> class result {
    static_assert(!std::is_same_v<T, E>, "result<T, E> requires distinct value and error types");

  public:
    using value_type = T;
    using error_type = E;

    // Like std::expected and boost::system::result, a default-constructed
    // result holds a value-initialized T.
    result() : m_storage(std::in_place_index<0>) {}
    result(T value) : m_storage(std::in_place_index<0>, std::move(value)) {}
    result(E error) : m_storage(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool has_value() const noexcept { return m_storage.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    T &value() & {
        require_value();
        return std::get<0>(m_storage);
    }
    const T &value() const & {
        require_value();
        return std::get<0>(m_storage);
    }
    T &&value() && {
        require_value();
        return std::move(std::get<0>(m_storage));
    }

    E &error() & {
        require_error();
        return std::get<1>(m_storage);
    }
    const E &error() const & {
        require_error();
        return std::get<1>(m_storage);
    }
    E &&error() && {
        require_error();
        return std::move(std::get<1>(m_storage));
    }

    T &operator*() & { return value(); }
    const T &operator*() const & { return value(); }
    T &&operator*() && { return std::move(*this).value(); }
    T *operator->() { return &value(); }
    const T *operator->() const { return &value(); }

    template <typename U> T value_or(U &&fallback) const & {
        return has_value() ? std::get<0>(m_storage) : static_cast<T>(std::forward<U>(fallback));
    }

  private:
    void require_value() const {
        if (!has_value()) {
            throw bad_result_access();
        }
    }
    void require_error() const {
        if (has_value()) {
            throw bad_result_access();
        }
    }

    std::variant<T, E> m_storage;
};

template <typename E> class result<void, E> {
  public:
    using value_type = void;
    using error_type = E;

    result() noexcept = default;
    result(E error) : m_error(std::move(error)) {}

    [[nodiscard]] bool has_value() const noexcept { return !m_error.has_value(); }
    explicit operator bool() const noexcept { return has_value(); }

    void value() const {
        if (!has_value()) {
            throw bad_result_access();
        }
    }

    E &error() & {
        require_error();
        return *m_error;
    }
    const E &error() const & {
        require_error();
        return *m_error;
    }
    E &&error() && {
        require_error();
        return std::move(*m_error);
    }

  private:
    void require_error() const {
        if (has_value()) {
            throw bad_result_access();
        }
    }

    std::optional<E> m_error;
};

} // namespace simple_http
