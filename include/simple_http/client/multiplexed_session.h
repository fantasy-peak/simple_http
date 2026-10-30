#pragma once

// The skeleton shared by the multiplexed client sessions (HTTP/2 and HTTP/3).
//
// The two protocols differ in everything below the stream abstraction — frames
// and HPACK vs nghttp3 — but their *client-side shape* is identical:
//
//   * a session keeps a table of in-flight exchanges and a count of them,
//     which is what `reusable()` is built from;
//   * each exchange is a ClientStream that gates on a shared StreamOutcome,
//     forwards write/finish/read to the session on its executor, and reports
//     the terminal state from a snapshot that outlives the session's table
//     entry;
//   * the session can multiplex, so it never takes the HTTP/1.1 one-exchange
//     gate.
//
// Keeping that shape in one place is not only line saving: the h2 and h3
// sessions used to hand-roll the same outcome checks, and a change to one
// (the interim-head reset, or the "a failed stream reports its error before
// anything else" rule) had to be rediscovered in the other.
//
// What stays in the concrete session: stream tables keyed by each protocol's
// stream id, the wait loops (they drive protocol-specific state), and the
// open/close paths.

#include <atomic>
#include <boost/asio.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "../core/types.h"
#include "client_config.h"
#include "client_stream.h"

namespace simple_http::detail {

namespace asio = boost::asio;

// Shared session state and accessors. `Derived` supplies the protocol-specific
// parts (open_stream, close, version, the stream table and its constants).
template <typename Derived>
class MultiplexedClientSession : public ClientSession, public std::enable_shared_from_this<Derived> {
  public:
    using Executor = asio::any_io_executor;

    MultiplexedClientSession(Executor ex, std::string authority)
        : m_executor(std::move(ex)), m_authority(std::move(authority)) {}

    MultiplexedClientSession(const MultiplexedClientSession &) = delete;
    MultiplexedClientSession &operator=(const MultiplexedClientSession &) = delete;

    bool alive() const override { return m_alive.load(std::memory_order_acquire); }

    bool reusable() const override {
        // The active count is read from foreign threads (the engine's reuse
        // check and the stream-destruction hook); the map is only touched on
        // the connection executor, hence the atomic counter rather than a table
        // walk.
        return m_alive.load(std::memory_order_acquire) && !m_goaway_received.load(std::memory_order_acquire) &&
               !m_close_requested.load(std::memory_order_acquire) &&
               m_active_streams.load(std::memory_order_acquire) == 0;
    }

    bool keepable() const override {
        return m_alive.load(std::memory_order_acquire) && !m_goaway_received.load(std::memory_order_acquire) &&
               !m_close_requested.load(std::memory_order_acquire);
    }

    bool multiplexes() const override { return true; }

    std::string_view authority() const override { return m_authority; }

    asio::any_io_executor executor() const override { return m_executor; }

    // The concrete executor type every operation hops onto. Internal callers
    // (the stream handle, the session's own coroutines) use this rather than
    // the type-erased `executor()`.
    [[nodiscard]] Executor get_executor() const { return m_executor; }

  protected:
    // Hops onto the pinned executor (model A); inline when already there.
    asio::awaitable<void> hop() const { co_await asio::dispatch(asio::bind_executor(m_executor, asio::use_awaitable)); }

    // In-flight exchange accounting for reusable(). Open and close must be
    // paired; the session's erase path is the only place that closes one.
    void stream_opened() noexcept { m_active_streams.fetch_add(1, std::memory_order_release); }
    void stream_closed() noexcept { m_active_streams.fetch_sub(1, std::memory_order_release); }

    Executor m_executor;
    std::string m_authority;
    std::atomic<std::size_t> m_active_streams{0};
    // Atomics because alive()/keepable()/reusable() are part of the session
    // interface and may be read off the connection's executor (see the base's
    // class comment in client_stream.h).
    std::atomic<bool> m_alive{false};
    std::atomic<bool> m_close_requested{false};
    std::atomic<bool> m_goaway_received{false};
};

// One exchange on a multiplexed session. Everything protocol-specific lives on
// the session: this class only gates on the outcome, forwards on the session's
// executor, and reports what the session published.
template <typename Session, typename Id> class MultiplexedClientStream final : public ClientStream {
  public:
    MultiplexedClientStream(std::shared_ptr<Session> session, Id id, std::shared_ptr<StreamOutcome> outcome)
        : m_session(std::move(session)), m_id(id), m_outcome(std::move(outcome)) {}

    asio::awaitable<error_code> write(std::string data) override {
        // Terminal state first: the session may have dropped its table entry
        // already, so the outcome is the only truthful source.
        if (m_outcome->get() != StreamState::Open)
            co_return m_outcome->to_error();
        co_return co_await m_session->stream_write(m_id, std::move(data), /*last=*/false);
    }

    asio::awaitable<error_code> finish(std::string data) override {
        if (m_outcome->get() != StreamState::Open)
            co_return m_outcome->to_error();
        co_return co_await m_session->stream_write(m_id, std::move(data), /*last=*/true);
    }

    asio::awaitable<std::expected<ReadResult, error_code>> read() override {
        if (m_outcome->get() != StreamState::Open)
            co_return outcome_result(*m_outcome);
        co_return co_await m_session->stream_read(m_id);
    }

    bool finished() const override { return m_outcome->get() != StreamState::Open; }

    Version version() const override { return m_session->version(); }

    std::uint32_t id() const override { return static_cast<std::uint32_t>(m_id); }

    asio::awaitable<void> cancel() override {
        co_await asio::dispatch(asio::bind_executor(m_session->get_executor(), asio::use_awaitable));
        m_session->stream_cancel(m_id);
    }

  private:
    // The session publishes the head/trailers here (and rearms an interim
    // 100-continue); those setters are protected on ClientStream.
    friend Session;

    asio::awaitable<error_code> await_head() override { co_return co_await m_session->await_stream_head(m_id); }

    // Strong: the caller's stream keeps the connection alive. The session refers
    // back to its streams weakly, so there is no cycle.
    std::shared_ptr<Session> m_session;
    Id m_id;
    std::shared_ptr<StreamOutcome> m_outcome;
};

} // namespace simple_http::detail
