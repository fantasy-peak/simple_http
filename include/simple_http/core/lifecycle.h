#pragma once

// Lifecycle: the single source of truth for whether a connection, exchange or
// stream may still be operated on (model A).
//
// A Lifecycle forms a parent chain: a stream/exchange lifecycle points at the
// connection lifecycle. can() walks the chain, so closing the root makes every
// descendant unusable without visiting each child. The engine holds the only
// strong reference to the root and each container holds the only strong
// reference to its child; objects (Body, ResponseWriter, WebSocket) hold a
// weak reference. Teardown closes the root and then resets it, which expires
// every weak reference immediately instead of waiting for object destruction.
//
// Thread affinity: phase transitions and can() run only on the owning
// connection executor. The only cross-thread entry point remains
// Request::cancel(), which is atomic and deliberately not modelled here.
//
// The helpers below keep the per-operation boilerplate to three lines:
//   * hop_to_owner() reposts a composed operation's `self` onto its own
//     executor. It is a no-op when the caller is already there, and it carries
//     the composed operation's cancellation state, so a cancellation delivered
//     while the operation is queued is not lost.
//   * try_acquire() locks the object state (Level 2) and checks the lifecycle
//     (Level 1) in one step. A non-empty Guard means "usable right now" and
//     also keeps the object alive for the duration of the operation.

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/dispatch.hpp>
#include <cstdint>
#include <memory>
#include <utility>

namespace simple_http {

namespace asio = boost::asio;

class Lifecycle {
  public:
    enum class Phase : std::uint8_t { Open, Closed };
    // Control covers operations that must still run after one direction has
    // closed (asking whether the connection is reusable, closing a response
    // that already ended). Any is for operations whose sink is itself safe on a
    // dead stream and whose historical contract is "no-op after the response
    // ended" (write/finish/trailers/continue/close/connected).
    enum class Access : std::uint8_t { Read, Write, Control, Any };

    explicit Lifecycle(asio::any_io_executor exec) : m_exec(std::move(exec)) {}

    Lifecycle(asio::any_io_executor exec, const std::shared_ptr<Lifecycle> &parent)
        : m_exec(std::move(exec)), m_parent(parent), m_has_parent(true) {}

    // Whether `access` is allowed right now. An expired parent counts as
    // closed: resetting the root invalidates every descendant. The chain is at
    // most two nodes deep (connection -> stream/exchange); the loop keeps the
    // parent alive across iterations instead of recursing.
    [[nodiscard]] bool can(Access access) const noexcept {
        if (access == Access::Any) {
            return true;
        }
        const Lifecycle *node = this;
        std::shared_ptr<Lifecycle> parent;
        for (;;) {
            if (node->m_phase != Phase::Open) {
                return false;
            }
            if (access == Access::Read ? !node->m_read_open
                                       : access == Access::Write ? !node->m_write_open : false) {
                return false;
            }
            if (!node->m_has_parent) {
                return true;
            }
            auto next = node->m_parent.lock();
            if (!next) {
                return false;
            }
            parent = std::move(next);
            node = parent.get();
        }
    }

    void close_read() noexcept { m_read_open = false; }
    void close_write() noexcept { m_write_open = false; }
    void close() noexcept {
        m_phase = Phase::Closed;
        m_read_open = false;
        m_write_open = false;
    }

    [[nodiscard]] bool closed() const noexcept { return m_phase == Phase::Closed; }
    [[nodiscard]] const asio::any_io_executor &executor() const noexcept { return m_exec; }

  private:
    asio::any_io_executor m_exec;
    std::weak_ptr<Lifecycle> m_parent;
    Phase m_phase{Phase::Open};
    bool m_read_open{true};
    bool m_write_open{true};
    bool m_has_parent{false};
};

// Move-only handle to an object's operation state. Holding it keeps the state
// alive until the operation completes, and its move-only nature prevents
// callers from caching it and accidentally extending the object's lifetime.
template <typename State> class Guard {
  public:
    Guard() = default;
    explicit Guard(std::shared_ptr<State> state) : m_state(std::move(state)) {}
    ~Guard() = default;

    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;
    Guard(Guard &&) noexcept = default;
    Guard &operator=(Guard &&) noexcept = default;

    [[nodiscard]] State *operator->() const noexcept { return m_state.get(); }
    [[nodiscard]] State &operator*() const noexcept { return *m_state; }
    explicit operator bool() const noexcept { return static_cast<bool>(m_state); }
    [[nodiscard]] const std::shared_ptr<State> &shared() const noexcept { return m_state; }

  private:
    std::shared_ptr<State> m_state;
};

// One-step "usable right now" check. `managed` is false for objects that are
// not attached to a lifecycle (client-heavy paths and standalone tests): they
// still need the Level-2 state, but have no connection to gate on.
template <typename State>
[[nodiscard]] Guard<State> try_acquire(const std::weak_ptr<State> &state, const std::weak_ptr<Lifecycle> &life,
                                       Lifecycle::Access access, bool managed = true) {
    auto acquired = state.lock();
    if (!acquired) {
        return {};
    }
    if (managed) {
        auto l = life.lock();
        if (!l || !l->can(access)) {
            return {};
        }
    }
    return Guard<State>{std::move(acquired)};
}

// Reposts a composed operation onto its own executor. Returns true when the
// caller must return immediately (the operation was moved into the dispatch).
// The composed operation's cancellation state travels with `self`, so a
// cancellation delivered while it is queued is observed after re-entry.
template <typename Self> bool hop_to_owner(Self &self, bool &hopped) {
    if (hopped) {
        return false;
    }
    hopped = true;
    asio::dispatch(self.get_executor(), std::move(self));
    return true;
}

} // namespace simple_http
