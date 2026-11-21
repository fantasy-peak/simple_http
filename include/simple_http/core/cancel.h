#pragma once

// Cancellation helpers shared by the server and client.
//
// The library follows Asio's per-operation cancellation contract: a pending
// operation that observes cancellation completes with operation_aborted.
// simple_http::asio::experimental::channel reports its own channel_cancelled instead,
// so composed operations normalize it at the completion boundary rather than
// leaking a second cancellation vocabulary to callers.

#include "asio_compat.h"
#include <utility>

#include "types.h"

namespace simple_http {

// Request::cancel() and asio::cancel_after() both emit terminal by default.
inline bool is_terminal_cancel(asio::cancellation_type_t type) noexcept {
    return (type & asio::cancellation_type::terminal) != asio::cancellation_type::none;
}

// Map the channel's per-operation cancellation code onto Asio's conventional
// operation_aborted. Every other code (connection_reset, eof, ...) is preserved.
inline error_code normalize_cancellation(error_code ec) noexcept {
    if (ec == asio::experimental::error::channel_cancelled) {
        return make_error_code(asio::error::operation_aborted);
    }
    return ec;
}

// For code that cannot normalize at the boundary: both codes mean "cancelled".
inline bool is_cancellation_error(const error_code &ec) noexcept {
    return ec == asio::error::operation_aborted || ec == asio::experimental::error::channel_cancelled;
}

// The one code the library reports for request/per-operation cancellation.
inline error_code cancelled_error() noexcept { return make_error_code(asio::error::operation_aborted); }

// Read the caller's slot before a wrapper moves the completion handler into
// itself. Argument evaluation order is unspecified in C++17, so
// `f(done, [done = std::move(done)] {...})` may sample a moved-from handler and
// silently lose the slot. Call this first, then `forward_cancel_slot`.
template <typename Handler> asio::cancellation_slot cancel_slot_of(const Handler &handler) noexcept {
    return asio::get_associated_cancellation_slot(handler);
}

// Bind a previously sampled slot to the wrapper completion that replaces a
// caller's handler. This is the only supported way to change the shape of an
// any_completion_handler: a bare lambda drops executor/allocator/slot
// associators, and a per-operation timer can then never reach the operation.
template <typename Slot, typename F> auto forward_cancel_slot(Slot slot, F &&fn) {
    return asio::bind_cancellation_slot(slot, std::forward<F>(fn));
}

// Install a cancellation handler into a slot the operation owns. Returns false
// when the slot is not connected (the caller bound no token).
template <typename Handler, typename Slot> bool install_cancel_handler(Slot slot, Handler handler) {
    if (!slot.is_connected()) {
        return false;
    }
    slot.template emplace<Handler>(std::move(handler));
    return true;
}

// Complete a caller from a cancellation handler without destroying its
// cancellation_state while the signal is still emitting. The completion must
// always be posted to the owner executor, never invoked inline.
template <typename Executor, typename Done> void post_cancelled(Executor executor, Done done) {
    asio::post(std::move(executor), [done = std::move(done)]() mutable { std::move(done)(cancelled_error()); });
}

template <typename Executor, typename Done, typename Arg> void post_cancelled(Executor executor, Done done, Arg arg) {
    asio::post(std::move(executor), [done = std::move(done), arg = std::move(arg)]() mutable {
        std::move(done)(cancelled_error(), std::move(arg));
    });
}

// RAII owner for the single handler installed in a slot that belongs to one
// operation (not a caller-owned associator). `clear()` before completing, and
// the destructor also clears it so a dropped operation cannot leave a stale
// handler behind.
class SlotGuard {
  public:
    SlotGuard() = default;
    explicit SlotGuard(asio::cancellation_slot slot) noexcept : m_slot(std::move(slot)) {}
    SlotGuard(const SlotGuard &) = delete;
    SlotGuard &operator=(const SlotGuard &) = delete;

    SlotGuard(SlotGuard &&other) noexcept : m_slot(other.m_slot), m_armed(other.m_armed) { other.m_armed = false; }
    SlotGuard &operator=(SlotGuard &&other) noexcept {
        if (this != &other) {
            clear();
            m_slot = other.m_slot;
            m_armed = other.m_armed;
            other.m_armed = false;
        }
        return *this;
    }
    ~SlotGuard() { clear(); }

    [[nodiscard]] bool connected() const noexcept { return m_slot.is_connected(); }

    template <typename Handler> bool install(Handler handler) {
        if (!m_slot.is_connected()) {
            return false;
        }
        m_slot.template emplace<Handler>(std::move(handler));
        m_armed = true;
        return true;
    }

    void clear() noexcept {
        if (m_armed) {
            m_slot.clear();
            m_armed = false;
        }
    }

  private:
    asio::cancellation_slot m_slot;
    bool m_armed{false};
};

} // namespace simple_http
