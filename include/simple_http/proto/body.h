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

#include <boost/asio.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <cstddef>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include "../core/content_encoding.h"
#include "../core/types.h"

namespace simple_http {

namespace asio = boost::asio;

// Result of a single Body::read(). When `eof` is true the body is complete and
// `data` is empty; otherwise `data` holds the chunk (which may itself be
// empty).
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
    explicit Body(const Executor &exec, std::size_t capacity = 1024) : m_exec(exec), m_capacity(capacity) {}

    Body(const Body &) = delete;
    Body &operator=(const Body &) = delete;
    Body(Body &&) = default;
    Body &operator=(Body &&) = default;

    // --- consumer API (handler side) ---

    // Reads the next body chunk (data or end-of-body), or an error_code.
    asio::awaitable<std::expected<ReadResult, error_code>> read() {
        if (m_eof) {
            co_return ReadResult::end();
        }
        // Exactly once, before the first read. HTTP/2 uses this to answer
        // `Expect: 100-continue` lazily (only if the handler actually reads the
        // body, so a handler that rejects the upload can still send a final
        // status). HTTP/1.x emits it from its pull provider instead.
        if (m_first_read_hook) {
            auto hook = std::move(m_first_read_hook);
            m_first_read_hook = nullptr;
            if (auto ec = co_await hook(); ec) {
                co_return std::unexpected(ec);
            }
        }
        m_read_started = true;
        // Pull mode (HTTP/1.x): fetch the next chunk on demand from the provider.
        if (m_pull) {
            for (;;) {
                auto r = co_await m_pull();
                if (!r) {
                    m_eof = true;
                    co_return r;
                }
                if (r->eof) {
                    if (!m_decoder) {
                        m_eof = true;
                        co_return ReadResult::end();
                    }
                    auto tail = finish_decoder();
                    m_eof = true;
                    if (!tail) {
                        co_return std::unexpected(tail.error());
                    }
                    if (!tail->empty()) {
                        co_return ReadResult::chunk(std::move(*tail));
                    }
                    co_return ReadResult::end();
                }
                if (!m_decoder) {
                    co_return r;
                }
                auto out = decode_chunk(r->data);
                if (!out) {
                    m_eof = true;
                    co_return std::unexpected(out.error());
                }
                if (out->empty()) {
                    continue; // the codec is buffering; pull more wire bytes
                }
                co_return ReadResult::chunk(std::move(*out));
            }
        }
        // Push mode (HTTP/2, HTTP/3): consume raw frames from the channel. When
        // decoding is enabled, the decoder runs here, on the consumer side;
        // flow-control credit is still reported for the raw (wire) frame size.
        for (;;) {
            // A channel that was never created means no byte was ever fed, so a
            // terminator recorded before the first read can be answered without
            // building one (the common GET/response path). Only a stream that is
            // still waiting for data needs the channel as its wait point.
            if (!m_channel) {
                if (m_pending_error) {
                    m_eof = true;
                    co_return std::unexpected(*m_pending_error);
                }
                if (m_pending_eof) {
                    co_return co_await finish_push_decoder();
                }
            }
            error_code ec;
            Frame frame;
            // Fast path: a frame is already queued.
            Channel &ch = channel();
            bool got = ch.try_receive([&](error_code e, Frame f) {
                ec = e;
                frame = std::move(f);
            });
            if (!got) {
                // A finish()/fail() that could not fit when it arrived. Reaching
                // here means the queue is drained (try_receive failed), so
                // delivering the recorded terminator now is exactly what a queued
                // one would have done.
                if (m_pending_error) {
                    m_eof = true;
                    co_return std::unexpected(*m_pending_error);
                }
                if (m_pending_eof) {
                    co_return co_await finish_push_decoder();
                }
                std::tie(ec, frame) = co_await ch.async_receive(asio::as_tuple(asio::use_awaitable));
            }
            if (ec) {
                m_eof = true;
                co_return std::unexpected(ec);
            }
            if (std::holds_alternative<Eof>(frame)) {
                co_return co_await finish_push_decoder();
            }
            std::string data = std::move(std::get<std::string>(frame));
            // Notify the producer (HTTP/2/3 engine) that these wire bytes were
            // consumed, so it can replenish flow-control credit. Decoding does
            // not change this: the engine's windows count compressed bytes.
            if (m_on_consumed && !data.empty()) {
                m_on_consumed(data.size());
            }
            if (!m_decoder) {
                co_return ReadResult::chunk(std::move(data));
            }
            auto out = decode_chunk(data);
            if (!out) {
                m_eof = true;
                co_return std::unexpected(out.error());
            }
            if (out->empty()) {
                continue; // the codec is buffering; wait for the next frame
            }
            co_return ReadResult::chunk(std::move(*out));
        }
    }

    // Reads and concatenates the entire remaining body.
    asio::awaitable<std::expected<std::string, error_code>> read_all() {
        std::string out;
        bool first = true;
        for (;;) {
            auto r = co_await read();
            if (!r) {
                co_return std::unexpected(r.error());
            }
            if (r->eof) {
                co_return out;
            }
            // The first chunk is moved, not appended: read() already hands its
            // buffer over, so appending it into an empty string was a second
            // allocation plus a copy of the whole (usually the only) chunk.
            if (first) {
                out = std::move(r->data);
                first = false;
            } else {
                out.append(r->data);
            }
        }
    }

    bool eof() const { return m_eof; }
    // Whether any chunk was handed to feed() (set_body included). Lets a client
    // tell "no body was ever supplied" from "a body was fed but not finished".
    bool fed() const { return m_fed; }
    // Whether finish()/fail() has been called. A directly-fed body must be
    // finished before it can be handed to the buffered client send().
    bool finished() const { return m_terminated; }

    // --- producer API (protocol engine side) ---

    // Enqueues a body chunk. Returns false if the channel is full or closed, and
    // on failure `data` is left untouched so the caller can retry it.
    //
    // There is no way to ask "is there room?" first: asio's concurrent_channel
    // reports readiness for *sending or receiving*, which stays true while a
    // reader is parked, not whether the buffer has space. So the caller must
    // check this result and keep the frame — a dropped frame is silent data loss,
    // and on HTTP/2 it also strands the connection, because those bytes were
    // already debited against the connection window and the credit is only
    // returned when the handler consumes.
    //
    // `data` is handed straight to try_send rather than wrapped in a Frame here:
    // asio moves its arguments into the queued frame only on success, so a refused
    // send leaves `data` intact. Wrapping it first would move it unconditionally
    // and lose it on failure.
    [[nodiscard]] bool feed(std::string &&data) {
        if (!channel().try_send(error_code{}, std::move(data)))
            return false;
        m_fed = true;
        return true;
    }

    // Signals normal end-of-body.
    //
    // Unlike feed(), this must never be droppable: the consumer *waits* for it,
    // so losing it hangs the reader rather than truncating it. It is therefore
    // also recorded out of band, and read() reports it once the queued frames
    // have drained — whether or not the channel had room when it arrived.
    void finish() {
        if (m_terminated)
            return; // first terminator wins (see fail())
        m_terminated = true;
        m_pending_eof = true;
        // A body-less request (the common GET) never touches the channel:
        // creating it here only to enqueue the terminator cost a shared_ptr
        // allocation plus the channel object for every such request. read()
        // reports m_pending_eof as soon as it finds the queue empty, so the
        // terminator does not need to be on the channel at all.
        if (m_channel)
            (void)m_channel->try_send(error_code{}, Frame{Eof{}});
    }

    // Signals a stream failure (reset/disconnect) to the consumer. Same
    // out-of-band guarantee as finish(), and for the same reason.
    //
    // First terminator wins: a body that already ended normally must not be
    // rewritten into a failure by a later teardown (an engine now fails every
    // in-flight body on connection shutdown and on its own stream resets, which
    // can run after a normal finish()), and repeated fails must not queue a
    // second terminator.
    void fail(error_code ec) {
        if (m_terminated)
            return;
        m_terminated = true;
        m_pending_error = ec;
        if (m_channel)
            (void)m_channel->try_send(ec, Frame{Eof{}});
    }

    // Installs a consumption hook invoked (from the consumer's context) with the
    // byte count of each data chunk the handler reads. The HTTP/2 engine uses it
    // to drive consumption-based flow-control replenishment. Must be cheap and
    // thread-safe; it typically just posts onto the connection executor.
    void set_on_consumed(std::function<void(std::size_t)> cb) { m_on_consumed = std::move(cb); }

    // Invoked exactly once, before the first read(), from the consumer's
    // context. A non-zero error aborts the read. See read() for the HTTP/2
    // 100-continue use.
    using FirstReadHook = std::function<asio::awaitable<error_code>()>;
    void set_first_read_hook(FirstReadHook hook) { m_first_read_hook = std::move(hook); }

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

    // --- transparent decoding (server request decompression) ---
    //
    // Installed by middleware::decompress_request before the handler runs. The
    // engine may already have queued raw (compressed) frames - the decoder runs
    // in read(), on the consumer side - so this only refuses once a read has
    // started. `max_decoded` caps the decoded bytes; 0 means no extra cap.
    [[nodiscard]] bool enable_decoding(std::unique_ptr<ContentDecoder> decoder, std::size_t max_decoded = 0) {
        if (!decoder || m_decoder || m_read_started) {
            return false;
        }
        m_decoder = std::move(decoder);
        m_max_decoded = max_decoded;
        return true;
    }
    bool decoding() const { return static_cast<bool>(m_decoder); }

  private:
    // The frame channel is created on first use. HTTP/1.x runs the body in pull
    // mode and never feeds or reads the channel, so building it in the
    // constructor would allocate a channel object plus its deque for every
    // request and use neither.
    Channel &channel() {
        if (!m_channel)
            m_channel = std::make_shared<Channel>(m_exec, m_capacity);
        return *m_channel;
    }

    std::size_t decode_room() const {
        return m_max_decoded == 0 ? std::numeric_limits<std::size_t>::max() : m_max_decoded - m_decoded_total;
    }

    bool over_decoded_limit(std::size_t produced) {
        if (produced == 0 || m_max_decoded == 0) {
            return false;
        }
        if (produced > m_max_decoded - m_decoded_total) {
            return true;
        }
        m_decoded_total += produced;
        return false;
    }

    std::expected<std::string, error_code> decode_chunk(std::string_view data) {
        bool exceeded = false;
        auto out = m_decoder->write_bounded(data, decode_room(), exceeded);
        if (m_decoder->failed()) {
            return std::unexpected{make_error_code(asio::error::invalid_argument)};
        }
        if (exceeded || over_decoded_limit(out.size())) {
            return std::unexpected{make_error_code(asio::error::message_size)};
        }
        return out;
    }

    std::expected<std::string, error_code> finish_decoder() {
        if (!m_decoder || m_decoder_finished) {
            return std::string{};
        }
        m_decoder_finished = true;
        bool exceeded = false;
        auto tail = m_decoder->finish_bounded(decode_room(), exceeded);
        if (m_decoder->failed()) {
            return std::unexpected{make_error_code(asio::error::invalid_argument)};
        }
        if (exceeded || over_decoded_limit(tail.size())) {
            return std::unexpected{make_error_code(asio::error::message_size)};
        }
        return tail;
    }

    // Shared EOF path for the pushed (h2/h3) side. Returns a final decoded tail
    // if the codec buffered one, otherwise end-of-body.
    asio::awaitable<std::expected<ReadResult, error_code>> finish_push_decoder() {
        if (!m_decoder) {
            m_eof = true;
            co_return ReadResult::end();
        }
        auto tail = finish_decoder();
        m_eof = true;
        if (!tail) {
            co_return std::unexpected(tail.error());
        }
        if (!tail->empty()) {
            co_return ReadResult::chunk(std::move(*tail));
        }
        co_return ReadResult::end();
    }

    asio::any_io_executor m_exec;
    std::size_t m_capacity{1024};
    std::shared_ptr<Channel> m_channel;
    bool m_eof{false};
    // Terminators that did not fit on the channel when they arrived; see
    // finish().
    bool m_pending_eof{false};
    // A terminator (finish or fail) has been recorded; only the first one wins.
    bool m_terminated{false};
    bool m_fed{false};
    std::optional<error_code> m_pending_error;
    std::function<void(std::size_t)> m_on_consumed;
    FirstReadHook m_first_read_hook;
    PullProvider m_pull; // set in pull mode (HTTP/1.x); unset = pushed channel (HTTP/2)
    // Server request decompression (see enable_decoding). Unique-ptr keeps Body
    // movable; the decoder is only touched from the consumer (read()).
    std::unique_ptr<ContentDecoder> m_decoder;
    std::size_t m_max_decoded{0};
    std::size_t m_decoded_total{0};
    bool m_read_started{false};
    bool m_decoder_finished{false};
};

} // namespace simple_http
