#pragma once

// Body: the async input stream for a request body (C++17 token-only port).
//
// A protocol engine feeds bytes into the Body (feed/finish/fail); the handler
// consumes them with `async_read(token)`. Internally a channel carries frames
// in push mode (HTTP/2, HTTP/3), while HTTP/1.x installs a pull provider that
// reads on demand from the socket.
//
// Completion signature: void(error_code, ReadResult). ReadResult's explicit
// `eof` flag distinguishes end-of-body from an empty chunk; the error channel is
// the error_code. The caller's token decides whether an error is a value
// (callback / as_tuple) or an exception (use_awaitable), exactly like every
// other Asio operation.
//
// The operation is written as an asio::async_compose state machine so the
// caller's associated executor, allocator and cancellation slot travel through
// the hook, the pull provider and the channel receive without being erased.

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/compose.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/system/result.hpp>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "../core/content_encoding.h"
#include "../core/result.h"
#include "../core/types.h"

namespace simple_http {

namespace asio = boost::asio;

// Result of a single async_read(). When `eof` is true the body is complete and
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
    using Channel = asio::experimental::channel<void(error_code, Frame)>;

    using ReadCompletion = asio::any_completion_handler<void(error_code, ReadResult)>;

    template <typename Executor>
    explicit Body(const Executor &exec, std::size_t capacity = 1024) : m_exec(exec), m_capacity(capacity) {}

    Body(const Body &) = delete;
    Body &operator=(const Body &) = delete;
    Body(Body &&) = default;
    Body &operator=(Body &&) = default;

    // --- consumer API (handler side) ---

    // Reads the next body chunk (data or end-of-body), or an error_code.
    template <typename Token> auto async_read(Token &&token) {
        return asio::async_compose<Token, void(error_code, ReadResult)>(ReadOp{*this}, token, m_exec);
    }

    // Reads and concatenates the entire remaining body.
    template <typename Token> auto async_read_all(Token &&token) {
        return asio::async_compose<Token, void(error_code, std::string)>(ReadAllOp{*this}, token, m_exec);
    }

    // The executor this body's channel is bound to (model A: the connection's
    // single-threaded io_context). Exposed so token-based composed operations
    // can use it as their I/O object.
    [[nodiscard]] asio::any_io_executor get_executor() const noexcept { return m_exec; }

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
    [[nodiscard]] bool feed(std::string &&data) {
        if (!channel().try_send(error_code{}, std::move(data)))
            return false;
        m_fed = true;
        return true;
    }

    // Signals normal end-of-body. Unlike feed(), this must never be droppable:
    // the consumer *waits* for it, so it is recorded out of band and read()
    // reports it once the queued frames have drained.
    void finish() {
        if (m_terminated)
            return; // first terminator wins (see fail())
        m_terminated = true;
        m_pending_eof = true;
        if (m_channel)
            (void)m_channel->try_send(error_code{}, Frame{Eof{}});
    }

    // Signals a stream failure (reset/disconnect) to the consumer. Same
    // out-of-band guarantee as finish(), and for the same reason.
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
    // to drive consumption-based flow-control replenishment.
    void set_on_consumed(std::function<void(std::size_t)> cb) { m_on_consumed = std::move(cb); }

    // Invoked exactly once, before the first read(), from the consumer's
    // context. A non-zero error aborts the read.
    using FirstReadHook = std::function<void(asio::any_completion_handler<void(error_code)>)>;
    void set_first_read_hook(FirstReadHook hook) { m_first_read_hook = std::move(hook); }

    // --- pull mode (HTTP/1.x) ---
    //
    // A pull provider turns Body into an on-demand stream: each read pulls the
    // next chunk directly from the provider (which reads/frames bytes off the
    // socket), instead of receiving pushed frames from the channel. The HTTP/1
    // engine installs this so a handler that never reads the body causes zero
    // socket reads (natural backpressure + no wasted work).
    using PullProvider = std::function<void(ReadCompletion)>;
    void set_pull_provider(PullProvider provider) { m_pull = std::move(provider); }
    bool is_pull() const { return static_cast<bool>(m_pull); }

    // --- transparent decoding (server request decompression) ---

    // Installed by middleware::decompress_request before the handler runs. The
    // engine may already have queued raw (compressed) frames - the decoder runs
    // in async_read(), on the consumer side - so this only refuses once a read
    // has started. `max_decoded` caps the decoded bytes; 0 means no extra cap.
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

    result<std::string> decode_chunk(std::string_view data) {
        bool exceeded = false;
        auto out = m_decoder->write_bounded(data, decode_room(), exceeded);
        if (m_decoder->failed()) {
            return boost::system::errc::make_error_code(boost::system::errc::invalid_argument);
        }
        if (exceeded || over_decoded_limit(out.size())) {
            return make_error_code(asio::error::message_size);
        }
        return out;
    }

    result<std::string> finish_decoder() {
        if (!m_decoder || m_decoder_finished) {
            return std::string{};
        }
        m_decoder_finished = true;
        bool exceeded = false;
        auto tail = m_decoder->finish_bounded(decode_room(), exceeded);
        if (m_decoder->failed()) {
            return boost::system::errc::make_error_code(boost::system::errc::invalid_argument);
        }
        if (exceeded || over_decoded_limit(tail.size())) {
            return make_error_code(asio::error::message_size);
        }
        return tail;
    }

    // State machine behind async_read(). One instance per operation; `body` is
    // the Body the operation was started on (it outlives the operation because
    // the caller owns the Request that owns this Body).
    struct ReadOp {
        enum class State { Start, Hook, Pull, Receive };
        Body &body;
        State state{State::Start};

        asio::any_io_executor get_executor() noexcept { return body.m_exec; }

        // Start (no arguments) and first-read-hook completion (one error_code).
        template <typename Self> void operator()(Self &self, error_code ec = {}) {
            if (state == State::Start) {
                start(self);
                return;
            }
            // Hook completion.
            if (ec) {
                body.m_eof = true;
                self.complete(ec, ReadResult{});
                return;
            }
            after_hook(self);
        }

        // Pull-provider completion.
        template <typename Self> void operator()(Self &self, error_code ec, ReadResult result) {
            if (ec) {
                body.m_eof = true;
                self.complete(ec, ReadResult{});
                return;
            }
            if (result.eof) {
                finish_for_end(self);
                return;
            }
            if (!body.m_decoder) {
                self.complete(error_code{}, std::move(result));
                return;
            }
            auto out = body.decode_chunk(result.data);
            if (!out) {
                body.m_eof = true;
                self.complete(out.error(), ReadResult{});
                return;
            }
            if (out->empty()) {
                state = State::Pull;
                body.m_pull(std::move(self)); // codec buffering: pull more wire bytes
                return;
            }
            self.complete(error_code{}, ReadResult::chunk(std::move(*out)));
        }

        // Channel receive completion.
        template <typename Self> void operator()(Self &self, error_code ec, Frame frame) {
            if (ec) {
                body.m_eof = true;
                self.complete(ec, ReadResult{});
                return;
            }
            if (std::holds_alternative<Eof>(frame)) {
                finish_for_end(self);
                return;
            }
            std::string data = std::move(std::get<std::string>(frame));
            if (body.m_on_consumed && !data.empty()) {
                body.m_on_consumed(data.size());
            }
            if (!body.m_decoder) {
                self.complete(error_code{}, ReadResult::chunk(std::move(data)));
                return;
            }
            auto out = body.decode_chunk(data);
            if (!out) {
                body.m_eof = true;
                self.complete(out.error(), ReadResult{});
                return;
            }
            if (out->empty()) {
                start_receive(self); // codec buffering: wait for the next frame
                return;
            }
            self.complete(error_code{}, ReadResult::chunk(std::move(*out)));
        }

      private:
        template <typename Self> void start(Self &self) {
            if (body.m_eof) {
                self.complete(error_code{}, ReadResult::end());
                return;
            }
            if (body.m_first_read_hook) {
                auto hook = std::move(body.m_first_read_hook);
                body.m_first_read_hook = nullptr;
                state = State::Hook;
                hook(std::move(self));
                return;
            }
            after_hook(self);
        }

        template <typename Self> void after_hook(Self &self) {
            body.m_read_started = true;
            if (body.m_pull) {
                state = State::Pull;
                body.m_pull(std::move(self));
                return;
            }
            start_receive(self);
        }

        template <typename Self> void start_receive(Self &self) {
            state = State::Receive;
            if (!body.m_channel) {
                if (body.m_pending_error) {
                    body.m_eof = true;
                    self.complete(*body.m_pending_error, ReadResult{});
                    return;
                }
                if (body.m_pending_eof) {
                    finish_for_end(self);
                    return;
                }
            }
            Channel &ch = body.channel();
            Frame frame;
            error_code ec;
            const bool got = ch.try_receive([&](error_code e, Frame f) {
                ec = e;
                frame = std::move(f);
            });
            if (got) {
                (*this)(self, ec, std::move(frame));
                return;
            }
            // A finish()/fail() that could not fit when it arrived. Reaching
            // here means the queue is drained (try_receive failed), so
            // delivering the recorded terminator now is exactly what a queued
            // one would have done.
            if (body.m_pending_error) {
                body.m_eof = true;
                self.complete(*body.m_pending_error, ReadResult{});
                return;
            }
            if (body.m_pending_eof) {
                finish_for_end(self);
                return;
            }
            ch.async_receive(std::move(self));
        }

        template <typename Self> void finish_for_end(Self &self) {
            if (!body.m_decoder) {
                body.m_eof = true;
                self.complete(error_code{}, ReadResult::end());
                return;
            }
            auto tail = body.finish_decoder();
            body.m_eof = true;
            if (!tail) {
                self.complete(tail.error(), ReadResult{});
                return;
            }
            if (!tail->empty()) {
                self.complete(error_code{}, ReadResult::chunk(std::move(*tail)));
                return;
            }
            self.complete(error_code{}, ReadResult::end());
        }
    };

    struct ReadAllOp {
        Body &body;
        std::string out;
        bool first{true};
        bool started{false};

        asio::any_io_executor get_executor() noexcept { return body.m_exec; }

        template <typename Self> void operator()(Self &self, error_code ec = {}, ReadResult result = {}) {
            if (!started) {
                started = true;
                body.async_read(std::move(self));
                return;
            }
            if (ec) {
                self.complete(ec, std::string{});
                return;
            }
            if (result.eof) {
                self.complete(error_code{}, std::move(out));
                return;
            }
            // The first chunk is moved, not appended: async_read() already hands
            // its buffer over, so appending it into an empty string would be a
            // second allocation plus a copy of the whole (usually the only) chunk.
            if (first) {
                out = std::move(result.data);
                first = false;
            } else {
                out.append(result.data);
            }
            body.async_read(std::move(self));
        }
    };

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
    // movable; the decoder is only touched from the consumer (async_read()).
    std::unique_ptr<ContentDecoder> m_decoder;
    std::size_t m_max_decoded{0};
    std::size_t m_decoded_total{0};
    bool m_read_started{false};
    bool m_decoder_finished{false};
};

} // namespace simple_http
