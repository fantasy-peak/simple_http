#pragma once

// Byte/object views. std::span exists only since C++20, while the library keeps
// a C++17 baseline, so this header either re-exports it or supplies the subset
// the library actually uses: default construction, (data, size) construction,
// data()/size()/empty()/begin()/end()/operator[]/front()/back(), and the
// qualification conversion span<T> -> span<const T>.
//
// Nothing here owns state; both forms are non-owning views.

#include <cstddef>

#if defined(__cpp_lib_span) && __cpp_lib_span >= 202002L
#include <span>
#else
#include <type_traits>
#endif

namespace simple_http {

#if defined(__cpp_lib_span) && __cpp_lib_span >= 202002L

template <typename T> using span = std::span<T>;

#else

// Dynamic-extent stand-in for std::span. Deliberately minimal: adding members
// (subspan, extent-tracked constructors, ...) here would invite code that only
// compiles on one of the two paths.
template <typename T> class span {
  public:
    using element_type = T;
    using value_type = std::remove_cv_t<T>;
    using size_type = std::size_t;
    using pointer = T *;
    using reference = T &;
    using iterator = pointer;

    constexpr span() noexcept = default;
    constexpr span(pointer data, size_type count) noexcept : m_data(data), m_size(count) {}

    // Qualification conversion, e.g. span<std::byte> -> span<const std::byte>.
    template <typename U, std::enable_if_t<std::is_convertible_v<U (*)[], T (*)[]>, int> = 0>
    constexpr span(const span<U> &other) noexcept : m_data(other.data()), m_size(other.size()) {}

    constexpr pointer data() const noexcept { return m_data; }
    constexpr size_type size() const noexcept { return m_size; }
    constexpr size_type size_bytes() const noexcept { return m_size * sizeof(T); }
    constexpr bool empty() const noexcept { return m_size == 0; }
    constexpr pointer begin() const noexcept { return m_data; }
    constexpr pointer end() const noexcept { return m_data + m_size; }
    constexpr reference operator[](size_type i) const noexcept { return m_data[i]; }
    constexpr reference front() const noexcept { return m_data[0]; }
    constexpr reference back() const noexcept { return m_data[m_size - 1]; }

  private:
    pointer m_data{nullptr};
    size_type m_size{0};
};

#endif

} // namespace simple_http
