#pragma once

// The skeleton shared by the multiplexed client sessions (HTTP/2 and HTTP/3),
// C++17 token-only port.
//
// The protocols differ below the stream abstraction, but their client-side
// shape is identical: a session keeps a table of in-flight exchanges (the basis
// of reusable()), and each exchange is a ClientStream that gates on a shared
// StreamOutcome, forwards writes/reads to the session on its executor, and
// reports the terminal state from a snapshot that outlives the table entry.

#include <atomic>
#include <boost/asio.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "../core/result.h"
#include "../core/types.h"
#include "client_config.h"
#include "client_stream.h"

namespace simple_http::detail {

namespace asio = boost::asio;

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
    [[nodiscard]] Executor get_executor() const { return m_executor; }

  protected:
    // Runs `fn` on the pinned executor (model A); inline when already there.
    template <typename Fn> void hop(Fn &&fn) const {
        asio::dispatch(asio::bind_executor(asio::any_io_executor{m_executor}, std::forward<Fn>(fn)));
    }

    void stream_opened() noexcept { m_active_streams.fetch_add(1, std::memory_order_release); }
    void stream_closed() noexcept { m_active_streams.fetch_sub(1, std::memory_order_release); }

    Executor m_executor;
    std::string m_authority;
    std::atomic<std::size_t> m_active_streams{0};
    std::atomic<bool> m_alive{false};
    std::atomic<bool> m_close_requested{false};
    std::atomic<bool> m_goaway_received{false};
};

template <typename Session, typename Id> class MultiplexedClientStream final : public ClientStream {
  public:
    MultiplexedClientStream(std::shared_ptr<Session> session, Id id, std::shared_ptr<StreamOutcome> outcome)
        : m_session(std::move(session)), m_id(id), m_outcome(std::move(outcome)) {}

    void write(std::string data, WriteCompletion done) override {
        if (m_outcome->get() != StreamState::Open) {
            std::move(done)(m_outcome->to_error());
            return;
        }
        m_session->stream_write(m_id, std::move(data), /*last=*/false, std::move(done));
    }

    void finish(std::string data, WriteCompletion done) override {
        if (m_outcome->get() != StreamState::Open) {
            std::move(done)(m_outcome->to_error());
            return;
        }
        m_session->stream_write(m_id, std::move(data), /*last=*/true, std::move(done));
    }

    void finish_trailers(Headers trailers, WriteCompletion done) override {
        if (m_outcome->get() != StreamState::Open) {
            std::move(done)(m_outcome->to_error());
            return;
        }
        m_session->stream_finish_trailers(m_id, std::move(trailers), std::move(done));
    }

    void read(ReadCompletion done) override {
        if (m_outcome->get() != StreamState::Open) {
            auto result = outcome_result(*m_outcome);
            if (result) {
                std::move(done)(error_code{}, std::move(*result));
            } else {
                std::move(done)(result.error(), ReadResult{});
            }
            return;
        }
        m_session->stream_read(m_id, std::move(done));
    }

    bool finished() const override { return m_outcome->get() != StreamState::Open; }
    Version version() const override { return m_session->version(); }
    std::uint32_t id() const override { return static_cast<std::uint32_t>(m_id); }

    void cancel(WriteCompletion done) override {
        auto session = m_session;
        auto id = m_id;
        asio::dispatch(asio::bind_executor(asio::any_io_executor{session->get_executor()},
                                           [session, id, done = std::move(done)]() mutable {
                                               session->stream_cancel(id);
                                               std::move(done)(error_code{});
                                           }));
    }

    friend Session;

    void await_head(WriteCompletion done) override { m_session->await_stream_head(m_id, std::move(done)); }
    asio::any_io_executor head_executor() const override { return m_session->executor(); }

  private:
    std::shared_ptr<Session> m_session;
    Id m_id;
    std::shared_ptr<StreamOutcome> m_outcome;
};

} // namespace simple_http::detail
