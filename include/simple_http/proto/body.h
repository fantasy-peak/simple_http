#pragma once

// Body: the async input stream for a request body.
//
// A protocol engine feeds bytes into the Body (feed/finish/fail); the handler
// consumes them with `co_await body.read()`. Internally a concurrent_channel
// carries frames, so producer and consumer may run on different executors while
// remaining safe.
//
// read() returns std::expected<ReadResult, error_code>: the error channel is
// the expected's unexpected side, while the success side carries an explicit
// three-state ReadResult (data / end-of-body). This avoids the ambiguity of a
// nested expected<optional<string>> and cleanly represents empty DATA frames.

#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include <boost/asio.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>

#include "../core/types.h"

namespace simple_http {

namespace asio = boost::asio;

// Result of a single Body::read(). When `eof` is true the body is complete and
// `data` is empty; otherwise `data` holds the chunk (which may itself be empty).
struct ReadResult {
    std::string data;
    bool eof = false;

    static ReadResult chunk(std::string d) { return ReadResult{std::move(d), false}; }
    static ReadResult end() { return ReadResult{{}, true}; }
};

class Body {
  public:
    // Frame carried on the internal channel: a data chunk or end-of-body.
    using Frame = std::variant<std::string, Eof>;
    using Channel = asio::experimental::concurrent_channel<void(error_code, Frame)>;

    template <typename Executor>
    explicit Body(const Executor& exec, std::size_t capacity = 1024)
        : m_channel(std::make_shared<Channel>(exec, capacity)) {}

    Body(const Body&) = delete;
    Body& operator=(const Body&) = delete;
    Body(Body&&) = default;
    Body& operator=(Body&&) = default;

    // --- consumer API (handler side) ---

    // Reads the next body chunk (data or end-of-body), or an error_code.
    asio::awaitable<std::expected<ReadResult, error_code>> read() {
        if (m_eof) {
            co_return ReadResult::end();
        }
        // Pull mode (HTTP/1.x): fetch the next chunk on demand from the provider.
        if (m_pull) {
            auto r = co_await m_pull();
            if (!r || r->eof) m_eof = true;
            co_return r;
        }
        error_code ec;
        Frame frame;
        // Fast path: a frame is already queued.
        bool got = m_channel->try_receive([&](error_code e, Frame f) {
            ec = e;
            frame = std::move(f);
        });
        if (!got) {
            // A finish()/fail() that could not fit when it arrived. Reaching here
            // means the queue is drained (try_receive failed), so delivering the
            // recorded terminator now is exactly what a queued one would have done.
            if (m_pending_error) {
                m_eof = true;
                co_return std::unexpected(*m_pending_error);
            }
            if (m_pending_eof) {
                m_eof = true;
                co_return ReadResult::end();
            }
            std::tie(ec, frame) = co_await m_channel->async_receive(asio::as_tuple(asio::use_awaitable));
        }
        if (ec) {
            m_eof = true;
            co_return std::unexpected(ec);
        }
        if (std::holds_alternative<Eof>(frame)) {
            m_eof = true;
            co_return ReadResult::end();
        }
        std::string data = std::move(std::get<std::string>(frame));
        // Notify the producer (HTTP/2 engine) that these bytes were consumed, so
        // it can replenish flow-control credit only for data the handler has
        // actually read — this bounds in-flight memory (no un-consumed backlog).
        if (m_on_consumed && !data.empty()) {
            m_on_consumed(data.size());
        }
        co_return ReadResult::chunk(std::move(data));
    }

    // Reads and concatenates the entire remaining body.
    asio::awaitable<std::expected<std::string, error_code>> read_all() {
        std::string out;
        for (;;) {
            auto r = co_await read();
            if (!r) {
                co_return std::unexpected(r.error());
            }
            if (r->eof) {
                co_return out;
            }
            out.append(r->data);
        }
    }

    bool eof() const { return m_eof; }

    // --- producer API (protocol engine side) ---

    // Enqueues a body chunk. Returns false if the channel is full or closed.
    //
    // There is no way to ask "is there room?" first: asio's concurrent_channel
    // reports readiness for *sending or receiving*, which stays true while a
    // reader is parked, not whether the buffer has space. So the caller must
    // check this result and keep the frame — a dropped frame is silent data loss,
    // and on HTTP/2 it also strands the connection, because those bytes were
    // already debited against the connection window and the credit is only
    // returned when the handler consumes.
    [[nodiscard]] bool feed(std::string data) { return m_channel->try_send(error_code{}, Frame{std::move(data)}); }

    // Signals normal end-of-body.
    //
    // Unlike feed(), this must never be droppable: the consumer *waits* for it, so
    // losing it hangs the reader rather than truncating it. It is therefore also
    // recorded out of band, and read() reports it once the queued frames have
    // drained — whether or not the channel had room when it arrived.
    void finish() {
        m_pending_eof = true;
        (void)m_channel->try_send(error_code{}, Frame{Eof{}});
    }

    // Signals a stream failure (reset/disconnect) to the consumer. Same
    // out-of-band guarantee as finish(), and for the same reason.
    void fail(error_code ec) {
        m_pending_error = ec;
        (void)m_channel->try_send(ec, Frame{Eof{}});
    }

    // Installs a consumption hook invoked (from the consumer's context) with the
    // byte count of each data chunk the handler reads. The HTTP/2 engine uses it
    // to drive consumption-based flow-control replenishment. Must be cheap and
    // thread-safe; it typically just posts onto the connection executor.
    void set_on_consumed(std::function<void(std::size_t)> cb) { m_on_consumed = std::move(cb); }

    // --- pull mode (HTTP/1.x) ---
    //
    // A pull provider turns Body into an on-demand stream: each read() pulls the
    // next chunk directly from the provider (which reads/frames bytes off the
    // socket), instead of receiving pushed frames from the channel. The HTTP/1
    // engine installs this so a handler that never reads the body causes zero
    // socket reads (natural backpressure + no wasted work). HTTP/2 leaves it
    // unset and keeps using the pushed channel.
    using PullProvider = std::function<asio::awaitable<std::expected<ReadResult, error_code>>()>;
    void set_pull_provider(PullProvider provider) { m_pull = std::move(provider); }
    bool is_pull() const { return static_cast<bool>(m_pull); }

  private:
    std::shared_ptr<Channel> m_channel;
    bool m_eof{false};
    // Terminators that did not fit on the channel when they arrived; see finish().
    bool m_pending_eof{false};
    std::optional<error_code> m_pending_error;
    std::function<void(std::size_t)> m_on_consumed;
    PullProvider m_pull;  // set in pull mode (HTTP/1.x); unset = pushed channel (HTTP/2)
};

}  // namespace simple_http
