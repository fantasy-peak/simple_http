#pragma once

// HTTP/2 client session (RFC 9113).
//
// Where the server's h2 engine reads requests and writes responses, this one
// does the reverse on the same wire format: it sends the client preface + our
// SETTINGS, opens streams with HEADERS, frames the request body into DATA under
// flow control, and turns the peer's HEADERS/DATA back into a response head and
// body chunks. The frame codec, HPACK and the constants come from the sibling
// engine/h2 headers, so both roles share one implementation of each.
//
// The structure mirrors the server engine — read_loop / write_loop / watchdog
// racing under awaitable_operators, with all connection state owned by the
// single connection executor:
//   * read_loop   transport bytes -> frame parser -> per-stream inbox
//   * write_loop  control frames + per-stream DATA (flow-controlled) ->
//   transport
//   * watchdog    closes the connection after idle_timeout of nothing at all
//
// What differs is where the data goes. A server engine *pushes* a request body
// into a Body channel; a client engine must *pull*: a response body lands in a
// per-stream deque, the application pops it through ClientStream::read(), and
// only what it took is credited back — so a consumer that stops reading leaves
// the peer's window closed and memory bounded by the window, and no frame is
// ever dropped (a fixed-capacity channel can drop when a peer sends many tiny
// DATA frames; a deque cannot).
//
// Stream-level errors leave the connection usable — that is the point of
// multiplexing. Connection-level errors send GOAWAY and fail every stream. And
// never write to a stream after RST_STREAM or after our own END_STREAM:
// RFC 9113 §5.1 makes that a connection-level protocol error, so one careless
// write would cost every other stream on the connection.

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../core/base64.h"
#include "../core/http_field.h" // is_connection_specific_field
#include "../core/http_method.h"
#include "../core/http_status.h"
#include "../core/limits.h"
#include "../core/logging.h"
#include "../core/types.h"
#include "../core/version.h"
#include "../engine/h2/h2_frame.h"
#include "../engine/h2/hpack_decode.h"
#include "../engine/h2/hpack_encoder.h"
#include "../proto/headers.h"
#include "../transport/transport.h"
#include "client_config.h"
#include "client_stream.h"
#include "multiplexed_session.h"

namespace simple_http::detail {

namespace asio = boost::asio;

// Protocol defaults (RFC 9113 §6.5.2, §6.9.2): what a peer may assume about us
// until our SETTINGS say otherwise. The server engine has its own copies; these
// are the client's so the two session types stay independent headers.
inline constexpr std::int32_t kClientInitialWindow = 65535;
inline constexpr std::size_t kClientMaxFrameSize = 16384;

// Outbound backpressure watermarks for one stream's request-body queue: a
// writer is parked once it has this much queued but unframed, and released when
// the write loop has drained the queue back to the low mark. Without it a
// writer outruns the peer's flow-control window at full speed and queues the
// whole body in memory (the lesson the server engine learned for responses).
inline constexpr std::size_t kClientOutHighWatermark = 1u << 20;  // 1 MiB
inline constexpr std::size_t kClientOutLowWatermark = 256u << 10; // 256 KiB

// The SETTINGS payload we advertise, raw (6 bytes per entry, for the SETTINGS
// frame) and base64url (for an h2c upgrade's HTTP2-Settings header).
inline std::string h2_settings_payload(const EngineLimits &limits) {
    std::string payload;
    auto add = [&](std::uint16_t id, std::uint32_t value) {
        payload.push_back(static_cast<char>((id >> 8) & 0xFF));
        payload.push_back(static_cast<char>(id & 0xFF));
        payload.push_back(static_cast<char>((value >> 24) & 0xFF));
        payload.push_back(static_cast<char>((value >> 16) & 0xFF));
        payload.push_back(static_cast<char>((value >> 8) & 0xFF));
        payload.push_back(static_cast<char>(value & 0xFF));
    };
    add(codec::H2_SETTINGS_MAX_CONCURRENT_STREAMS, limits.h2_max_concurrent_streams);
    add(codec::H2_SETTINGS_INITIAL_WINDOW_SIZE, static_cast<std::uint32_t>(limits.h2_initial_window));
    add(codec::H2_SETTINGS_MAX_FRAME_SIZE, limits.h2_max_frame_size);
    add(codec::H2_SETTINGS_MAX_HEADER_LIST_SIZE, static_cast<std::uint32_t>(limits.effective_max_header_list_size()));
    add(codec::H2_SETTINGS_ENABLE_PUSH, 0); // server push is not supported here
    return payload;
}

inline std::string h2_settings_base64url(const EngineLimits &limits) {
    return base64_url_encode(h2_settings_payload(limits));
}

template <typename Transport> class Http2ClientSession;

// One HTTP/2 stream, from the caller's side. The shared multiplexed-stream
// skeleton (outcome gating, executor hop, forwarders) lives in
// multiplexed_session.h; the session supplies the protocol operations.
template <typename Transport>
using Http2ClientStream = MultiplexedClientStream<Http2ClientSession<Transport>, std::uint32_t>;

template <typename Transport>
class Http2ClientSession final : public MultiplexedClientSession<Http2ClientSession<Transport>> {
  public:
    using Executor = decltype(std::declval<Transport &>().get_executor());
    using Base = MultiplexedClientSession<Http2ClientSession<Transport>>;

  private:
    // Dependent base: bring the shared helpers and state into scope.
    using Base::hop;
    using Base::m_active_streams;
    using Base::m_alive;
    using Base::m_authority;
    using Base::m_close_requested;
    using Base::m_executor;
    using Base::m_goaway_received;
    using Base::stream_closed;
    using Base::stream_opened;

  public:
    Http2ClientSession(std::shared_ptr<Transport> transport, ClientTarget target, EngineLimits limits,
                       std::chrono::milliseconds idle_timeout)
        : Base(transport->get_executor(), target.authority()), m_transport(std::move(transport)),
          m_target(std::move(target)), m_limits(limits), m_idle_timeout(idle_timeout), m_notify(m_executor, 1) {
        // What we advertise bounds what the peer may index into, so the decoder
        // is set to the same value. We advertise nothing, i.e. the 4096 default.
        m_decoder.set_max_table_size(4096);
        // Bound the decoded response-header list to the same value we advertise
        // as SETTINGS_MAX_HEADER_LIST_SIZE (see h2_settings_payload), so a
        // compressed response cannot expand into an unbounded list.
        m_decoder.set_max_list_size(m_limits.effective_max_header_list_size());
    }

    // --- connection setup ---

    // Sends the preface + our SETTINGS and starts the loops. Returns once the
    // connection is running (the loops hold the session alive themselves).
    void start(WriteCompletion done) {
        start_impl(std::nullopt, {}, [done = std::move(done)](error_code ec, std::shared_ptr<ClientStream>) mutable {
            std::move(done)(ec);
        });
    }

    // Starts the connection and replays `seed` as stream 1 — the request that
    // carried `Upgrade: h2c` (RFC 9113 §3.2). No HEADERS is sent for it: the
    // peer already has the request. Returns stream 1's handle.
    // `initial` carries whatever the peer already sent after its 101 — the h2
    // connection preface and SETTINGS, typically, or even the response itself.
    // Dropping those bytes costs a round trip at best and hangs at worst.
    void start_with_stream(std::shared_ptr<Request> seed, std::string initial, OpenCompletion done) {
        start_impl(std::move(seed), std::move(initial),
                   [done = std::move(done)](error_code ec, std::shared_ptr<ClientStream> stream) mutable {
                       if (ec) {
                           std::move(done)(ec, nullptr);
                           return;
                       }
                       if (!stream) {
                           std::move(done)(make_error_code(client_errc::protocol_error), nullptr);
                           return;
                       }
                       std::move(done)(error_code{}, std::move(stream));
                   });
    }

    // --- ClientSession ---

    void open_stream(std::shared_ptr<Request> request, OpenCompletion done) override {
        auto self = this->shared_from_this();
        hop([self, request = std::move(request), done = std::move(done)]() mutable {
            self->open_stream_impl(std::move(request), std::move(done));
        });
    }

  private:
    void open_stream_impl(std::shared_ptr<Request> request, OpenCompletion done) {
        if (m_goaway_received) {
            std::move(done)(make_error_code(client_errc::goaway), nullptr);
            return;
        }
        if (!m_alive) {
            std::move(done)(make_error_code(client_errc::session_closed), nullptr);
            return;
        }
        if (request->stream_body() && request->content_length() >= 0) {
            std::move(done)(make_error_code(client_errc::invalid_spec), nullptr);
            return;
        }
        if (request->close())
            m_close_requested = true;
        if (m_peer_max_concurrent_announced && m_streams.size() >= m_peer_max_concurrent_streams) {
            std::move(done)(make_error_code(client_errc::too_many_streams), nullptr);
            return;
        }
        if (m_next_stream_id > 0x7FFFFFFFu) {
            std::move(done)(make_error_code(client_errc::too_many_streams), nullptr);
            return;
        }

        const std::uint32_t id = m_next_stream_id;
        m_next_stream_id += 2;
        m_last_stream_id = id;

        auto [it, inserted] = m_streams.try_emplace(id);
        (void)inserted;
        stream_opened();
        Stream &st = it->second;
        init_stream(st, id);
        st.is_head = request->method() == Method::Head;
        if (const auto expect = request->header("expect")) {
            st.expect_continue = icontains_ci(*expect, "100-continue");
        }

        std::string block;
        if (auto ec = encode_request_head(*request, block); ec) {
            erase_stream(it);
            std::move(done)(ec, nullptr);
            return;
        }

        auto self = this->shared_from_this();
        auto finish_open = [self, request, id, block = std::move(block),
                            done = std::move(done)](error_code ec, std::string up_front) mutable {
            if (ec) {
                if (auto stream_it = self->m_streams.find(id); stream_it != self->m_streams.end())
                    self->erase_stream(stream_it);
                std::move(done)(ec, nullptr);
                return;
            }
            auto stream_it = self->m_streams.find(id);
            if (stream_it == self->m_streams.end()) {
                std::move(done)(make_error_code(client_errc::session_closed), nullptr);
                return;
            }
            Stream &stream = stream_it->second;
            const bool have_body = !up_front.empty() || request->stream_body();
            if (!up_front.empty()) {
                stream.out_queued = up_front.size();
                stream.out_queue.push_back(std::move(up_front));
            }
            if (!have_body || !request->stream_body())
                stream.out_finished = true;
            if (!have_body)
                stream.local_end = true;
            auto handle = std::make_shared<Http2ClientStream<Transport>>(self, id, stream.outcome);
            stream.handle = handle;
            self->submit_headers(id, block, /*end_stream=*/!have_body);
            self->flush();
            std::move(done)(error_code{}, handle);
        };

        if (!request->stream_body() && request->content_length() > 0) {
            request->body().async_read_all(
                [finish_open = std::move(finish_open)](error_code ec, std::string body) mutable {
                    finish_open(ec, std::move(body));
                });
            return;
        }
        finish_open(error_code{}, std::string{});
    }

  public:
    // alive()/reusable()/keepable()/multiplexes()/authority()/executor() come
    // from MultiplexedClientSession. A request that asked to close
    // (`Request::close`) is what m_close_requested records: HTTP/2 has no
    // `Connection: close`, so that flag is the whole of the mechanism.

    Version version() const override { return Version::Http2; }

    void close() override {
        asio::post(m_executor, [self = this->shared_from_this()] {
            self->m_alive = false;
            self->fail_all_streams(client_errc::session_closed);
            if (self->m_transport)
                self->m_transport->close();
        });
    }

    // --- introspection (tests / diagnostics) ---
    std::size_t active_streams() const { return m_streams.size(); }

    std::uint32_t last_stream_id() const { return m_last_stream_id; }

    bool draining() const { return m_goaway_received || m_goaway_sent; }

    // --- stream operations (called by Http2ClientStream) ---

    // Ends a streamed request with an HPACK trailer block. The block is sent as
    // trailing HEADERS with END_STREAM after every queued DATA frame, so it
    // cannot overtake the body.
    void stream_finish_trailers(std::uint32_t id, Headers trailers, WriteCompletion done) {
        auto self = this->shared_from_this();
        hop([self, id, trailers = std::move(trailers), done = std::move(done)]() mutable {
            auto it = self->m_streams.find(id);
            if (it == self->m_streams.end()) {
                std::move(done)(make_error_code(client_errc::session_closed));
                return;
            }
            Stream &st = it->second;
            if (st.write_error) {
                std::move(done)(st.write_error);
                return;
            }
            if (st.reset || st.failed) {
                std::move(done)(st.error ? st.error : make_error_code(client_errc::stream_reset));
                return;
            }
            if (st.local_end || st.out_finished) {
                std::move(done)(make_error_code(client_errc::body_not_streaming));
                return;
            }
            std::string block;
            for (const auto &[name, value] : trailers) {
                if (name.empty() || name.front() == ':' || is_connection_specific(name) || contains_ctl(name) ||
                    contains_ctl(value)) {
                    continue;
                }
                codec::hpack_append_literal(block, name, value);
            }
            if (block.empty()) {
                // Nothing valid to send: fall back to a normal END_STREAM.
                self->stream_write(id, {}, true, std::move(done));
                return;
            }
            st.out_trailers = std::move(block);
            st.out_trailers_set = true;
            st.out_finished = true;
            self->flush();
            std::move(done)(error_code{});
        });
    }

    void stream_write(std::uint32_t id, std::string data, bool last, WriteCompletion done) {
        auto self = this->shared_from_this();
        hop([self, id, data = std::move(data), last, done = std::move(done)]() mutable {
            auto it = self->m_streams.find(id);
            if (it == self->m_streams.end()) {
                std::move(done)(make_error_code(client_errc::session_closed));
                return;
            }
            Stream &st = it->second;
            if (st.write_error) {
                std::move(done)(st.write_error);
                return;
            }
            if (st.reset || st.failed) {
                std::move(done)(st.error ? st.error : make_error_code(client_errc::stream_reset));
                return;
            }
            if (st.local_end || st.out_finished) {
                std::move(done)(make_error_code(client_errc::body_not_streaming));
                return;
            }

            const bool has_data = !data.empty();
            auto enqueue = [self, id, data = std::move(data), last, done = std::move(done)](error_code ec) mutable {
                if (ec) {
                    if (auto found = self->m_streams.find(id); found != self->m_streams.end())
                        found->second.write_error = ec;
                    std::move(done)(ec);
                    return;
                }
                auto stream_it = self->m_streams.find(id);
                if (stream_it == self->m_streams.end()) {
                    std::move(done)(make_error_code(client_errc::session_closed));
                    return;
                }
                if (!data.empty()) {
                    stream_it->second.out_queued += data.size();
                    stream_it->second.out_queue.push_back(std::move(data));
                }
                if (last)
                    stream_it->second.out_finished = true;
                self->flush();
                std::move(done)(error_code{});
            };
            if (has_data) {
                self->await_out_space(id, std::move(enqueue));
                return;
            }
            enqueue(error_code{});
        });
    }

    // Waits for the response head without consuming body bytes: what read_head()
    // needs, and what read() calls first when the caller goes straight to the
    // body. The head itself is published to the caller's handle when it is
    // parsed.
    void await_stream_head(std::uint32_t id, WriteCompletion done) {
        auto self = this->shared_from_this();
        hop([self, id, done = std::move(done)]() mutable { self->await_head_impl(id, std::move(done)); });
    }

    void stream_read(std::uint32_t id, ReadCompletion done) {
        auto self = this->shared_from_this();
        hop([self, id, done = std::move(done)]() mutable { self->stream_read_impl(id, std::move(done)); });
    }

    bool stream_finished(std::uint32_t id) const {
        auto it = m_streams.find(id);
        if (it == m_streams.end())
            return true;
        const Stream &st = it->second;
        return st.remote_end || st.failed || st.reset;
    }

    void stream_cancel(std::uint32_t id) {
        // dispatch, not post: the cancel must have taken effect before the caller
        // can start another exchange, or the "session is idle again" signal that
        // returns it to the pool arrives too late to be reused.
        asio::dispatch(m_executor, [self = this->shared_from_this(), id] {
            auto it = self->m_streams.find(id);
            if (it == self->m_streams.end())
                return;
            // RST_STREAM: only this stream dies; the connection lives on.
            self->send_rst_stream(id, codec::H2_CANCEL);
            self->it_finish(it, StreamState::Reset, make_error_code(client_errc::stream_reset));
            self->flush();
        });
    }

  private:
    void await_head_impl(std::uint32_t id, WriteCompletion done) {
        auto it = m_streams.find(id);
        if (it == m_streams.end()) {
            std::move(done)(make_error_code(client_errc::protocol_error));
            return;
        }
        Stream &st = it->second;
        if (st.head_seen) {
            std::move(done)(error_code{});
            return;
        }
        if (st.failed || st.reset) {
            std::move(done)(outcome_error(*st.outcome));
            return;
        }
        if (st.remote_end) {
            std::move(done)(make_error_code(client_errc::protocol_error));
            return;
        }
        if (!st.in_notify) {
            st.in_notify = std::make_shared<asio::experimental::concurrent_channel<void(error_code)>>(m_executor, 1);
        }
        auto notify = st.in_notify;
        auto outcome = st.outcome;
        auto self = this->shared_from_this();
        notify->async_receive([self, id, notify, outcome, done = std::move(done)](error_code ec) mutable {
            if (ec) {
                std::move(done)(outcome_error(*outcome));
                return;
            }
            self->await_head_impl(id, std::move(done));
        });
    }

    void stream_read_impl(std::uint32_t id, ReadCompletion done) {
        auto it = m_streams.find(id);
        if (it == m_streams.end()) {
            std::move(done)(make_error_code(client_errc::session_closed), ReadResult{});
            return;
        }
        if (!it->second.head_seen) {
            auto self = this->shared_from_this();
            await_head_impl(id, [self, id, done = std::move(done)](error_code ec) mutable {
                if (ec) {
                    std::move(done)(ec, ReadResult{});
                    return;
                }
                self->stream_read_impl(id, std::move(done));
            });
            return;
        }
        it = m_streams.find(id);
        if (it == m_streams.end()) {
            std::move(done)(make_error_code(client_errc::session_closed), ReadResult{});
            return;
        }
        Stream &st = it->second;
        if (!st.in_queue.empty()) {
            std::string data = std::move(st.in_queue.front());
            st.in_queue.pop_front();
            st.in_bytes -= data.size();
            st.recv_owed_conn -= static_cast<std::int64_t>(data.size());
            if (st.recv_owed_conn < 0)
                st.recv_owed_conn = 0;
            credit_consumed(st, static_cast<std::int64_t>(data.size()));
            flush();
            std::move(done)(error_code{}, ReadResult::chunk(std::move(data)));
            return;
        }
        if (st.outcome->get() != StreamState::Open) {
            auto terminal = outcome_result(*st.outcome);
            erase_stream(it);
            if (terminal) {
                std::move(done)(error_code{}, std::move(*terminal));
            } else {
                std::move(done)(terminal.error(), ReadResult{});
            }
            return;
        }
        if (st.failed || st.reset) {
            auto terminal = outcome_result(*st.outcome);
            erase_stream(it);
            if (terminal) {
                std::move(done)(error_code{}, std::move(*terminal));
            } else {
                std::move(done)(terminal.error(), ReadResult{});
            }
            return;
        }
        if (st.remote_end) {
            st.outcome->set(StreamState::Eof);
            erase_stream(it);
            std::move(done)(error_code{}, ReadResult::end());
            return;
        }
        if (!st.in_notify) {
            st.in_notify = std::make_shared<asio::experimental::concurrent_channel<void(error_code)>>(m_executor, 1);
        }
        auto notify = st.in_notify;
        auto outcome = st.outcome;
        auto self = this->shared_from_this();
        notify->async_receive([self, id, notify, outcome, done = std::move(done)](error_code ec) mutable {
            if (ec == asio::error::operation_aborted) {
                std::move(done)(ec, ReadResult{});
                return;
            }
            if (self->m_streams.find(id) == self->m_streams.end()) {
                // it_finish() erases the entry before waking the reader. The
                // wake must still report the terminal state captured at that
                // moment (stream_reset/refused/eof), not session_closed.
                auto terminal = outcome_result(*outcome);
                if (terminal) {
                    std::move(done)(error_code{}, std::move(*terminal));
                } else {
                    std::move(done)(terminal.error(), ReadResult{});
                }
                return;
            }
            self->stream_read_impl(id, std::move(done));
        });
    }

  private:
    struct Stream {
        std::uint32_t id{0};
        // Weak: the caller owns its stream handle, and the stream owns the
        // session — a strong handle here would be a reference cycle.
        std::weak_ptr<Http2ClientStream<Transport>> handle;
        std::shared_ptr<StreamOutcome> outcome;

        // --- inbound (response) ---
        ResponseHead head;
        bool head_seen{false};        // a final header block was decoded (and published
                                      // to the handle)
        bool block_end_stream{false}; // the HEADERS frame that started the block carried END_STREAM
        bool expect_continue{false};  // request carried Expect: 100-continue
        bool interim_100_sent{false}; // the interim 100 was published to the handle
        bool is_head{false};          // the request method was HEAD (bodyless response)
        // §8.1.2.6: a declared content-length must equal the DATA payload total.
        // -1 means no declaration; bodyless responses skip the check.
        std::int64_t declared_content_length{-1};
        std::uint64_t body_received{0};
        std::deque<std::string> in_queue;
        std::size_t in_bytes{0};
        std::shared_ptr<asio::experimental::concurrent_channel<void(error_code)>> in_notify;
        std::string header_block; // accumulating HEADERS + CONTINUATION
        bool remote_end{false};   // the peer sent END_STREAM
        bool reset{false};        // RST_STREAM seen (or sent)
        bool failed{false};       // the stream cannot deliver more
        error_code error;

        // --- outbound (request body) ---
        std::deque<std::string> out_queue;
        std::string out_trailers; // encoded trailing HEADERS block
        bool out_trailers_set{false};
        std::size_t out_offset{0};
        std::size_t out_queued{0};
        bool out_finished{false}; // the caller finished the body
        bool local_end{false};    // our END_STREAM has been framed
        error_code write_error;
        std::shared_ptr<asio::experimental::concurrent_channel<void(error_code)>> out_space;
        std::int64_t send_window{kClientInitialWindow};

        // --- flow control ---
        std::int64_t recv_owed_conn{0}; // delivered but unconsumed: owes connection credit
        std::int64_t recv_pending{0};   // consumed: awaiting a batched WINDOW_UPDATE
    };

    // --- setup ---

    // Returns the seeded stream's handle (or null when there is no seed), so the
    // caller — not a weak reference — keeps it alive.
    void start_impl(std::optional<std::shared_ptr<Request>> seed, std::string carried_over, OpenCompletion done) {
        m_authority = m_target.authority();
        m_recv_buf = std::move(carried_over);
        const std::int32_t initial = m_limits.h2_initial_window;
        std::string out{codec::kH2ClientPreface};
        append_frame(out, codec::H2FrameType::Settings, 0, 0, h2_settings_payload(m_limits));
        if (initial > kClientInitialWindow) {
            std::string window_update;
            codec::append_u32(window_update, static_cast<std::uint32_t>(initial - kClientInitialWindow));
            append_frame(out, codec::H2FrameType::WindowUpdate, 0, 0, window_update);
        }
        // §6.9.2: the connection-level window is fixed at 65535 initially and
        // can only be grown by WINDOW_UPDATE. SETTINGS_INITIAL_WINDOW_SIZE is a
        // per-stream value; recording a smaller connection window here rejects
        // compliant traffic that fits the real 65535-octet budget.
        m_conn_recv_window = std::max(initial, kClientInitialWindow);

        auto self = this->shared_from_this();
        auto bytes = std::make_shared<std::string>(std::move(out));
        m_transport->async_write(ConstByteSpan{reinterpret_cast<const std::byte *>(bytes->data()), bytes->size()},
                                 [self, bytes, seed, done = std::move(done)](error_code ec, std::size_t) mutable {
                                     if (ec) {
                                         std::move(done)(ec, nullptr);
                                         return;
                                     }
                                     std::shared_ptr<ClientStream> seeded;
                                     if (seed) {
                                         auto [it, inserted] = self->m_streams.try_emplace(1);
                                         (void)inserted;
                                         self->stream_opened();
                                         Stream &stream = it->second;
                                         self->init_stream(stream, 1);
                                         stream.is_head = (*seed)->method() == Method::Head;
                                         auto handle =
                                             std::make_shared<Http2ClientStream<Transport>>(self, 1, stream.outcome);
                                         stream.handle = handle;
                                         seeded = handle;
                                         stream.local_end = true;
                                         stream.out_finished = true;
                                         self->m_last_stream_id = 1;
                                         self->m_next_stream_id = 3;
                                     }
                                     self->m_alive = true;
                                     self->m_deadline = std::chrono::steady_clock::now() + self->m_idle_timeout;
                                     self->flush();
                                     self->run_loops();
                                     std::move(done)(error_code{}, std::move(seeded));
                                 });
    }

    void init_stream(Stream &st, std::uint32_t id) {
        st.id = id;
        st.outcome = std::make_shared<StreamOutcome>();
        st.send_window = m_peer_initial_window;
    }

    // Encodes a request head as HPACK, with the static table carrying what it can
    // (the pseudo-headers of a plain GET cost one byte each).
    error_code encode_request_head(const Request &request, std::string &block) {
        // The raw token carries extension methods; set_method() keeps it in sync
        // for the standard verbs.
        std::string_view method = request.method_token();
        if (method.empty())
            method = to_string(request.method());
        if (method.empty() || contains_ctl(method))
            return make_error_code(client_errc::protocol_error);
        const std::string_view target =
            request.target().empty() ? std::string_view{"/"} : std::string_view{request.target()};
        if (target.front() != '/' || contains_ctl(target)) {
            return make_error_code(client_errc::protocol_error); // :path must be origin-form
        }

        // RFC 7541 Appendix A: 2 = :method GET, 3 = :method POST, 4 = :path /,
        // 6 = :scheme http, 7 = :scheme https.
        if (request.method() == Method::Get) {
            codec::hpack_append_indexed(block, 2);
        } else if (request.method() == Method::Post) {
            codec::hpack_append_indexed(block, 3);
        } else {
            codec::hpack_append_literal(block, ":method", method);
        }
        codec::hpack_append_indexed(block, m_target.use_tls ? 7 : 6);
        // RFC 9113 §8.3.1: :authority is Host's HTTP/2 spelling. A `host` field
        // on the Request is an explicit override (reverse proxy preserve_host);
        // otherwise the connection target owns it.
        if (const auto host_override = request.header("host")) {
            if (contains_ctl(*host_override)) {
                return make_error_code(client_errc::invalid_header);
            }
            codec::hpack_append_literal(block, ":authority", *host_override);
        } else {
            codec::hpack_append_literal(block, ":authority", m_authority);
        }
        if (target == "/") {
            codec::hpack_append_indexed(block, 4);
        } else {
            codec::hpack_append_literal(block, ":path", target);
        }

        bool saw_agent = false;
        for (const auto &[name, value] : request.headers()) {
            // Framing belongs to the session: a caller-supplied content-length
            // would be emitted twice (once here, once below) and a mismatch
            // between the two is a request-smuggling shape.
            if (name.empty() || name.front() == ':' || is_connection_specific(name) || name == "content-length")
                continue;
            if (!is_http_token(name) || contains_ctl(name) || contains_ctl(value)) {
                // Refuse the request instead of silently dropping the field: a
                // caller that put CR/LF into a value is trying to inject a
                // header, and a mismatch between what it asked for and what
                // went on the wire is worse than a clear error.
                SIMPLE_HTTP_ERROR_LOG("h2 client: invalid header '{}'", name);
                return make_error_code(client_errc::invalid_header);
            }
            if (name == "user-agent")
                saw_agent = true;
            codec::hpack_append_literal(block, name,
                                        value); // Headers keeps names lowercased
        }
        if (!saw_agent)
            codec::hpack_append_literal(block, "user-agent", client_product);
        if (!request.stream_body() && request.content_length() >= 0) {
            // A known length: harmless on h2, and some origins want it.
            codec::hpack_append_literal(block, "content-length", std::to_string(request.content_length()));
        }
        return error_code{};
    }

    // Fields HTTP/2 forbids (RFC 9113 §8.2.2): connection-specific ones, plus
    // `host`, whose job `:authority` does. The connection-specific set is the
    // shared one the h2/h3 engines validate against.
    static bool is_connection_specific(std::string_view name) {
        return is_connection_specific_field(name) || name == "host";
    }

    // --- serve loops ---

    void run_loops() {
        auto self = this->shared_from_this();
        if (m_idle_timeout.count() > 0) {
            arm_watchdog();
        }
        read_loop();
        write_loop();
    }

    void arm_watchdog() {
        auto self = this->shared_from_this();
        m_watchdog->expires_at(m_deadline);
        m_watchdog->async_wait([self](error_code ec) {
            if (ec || self->m_finished)
                return;
            if (std::chrono::steady_clock::now() >= self->m_deadline) {
                SIMPLE_HTTP_ERROR_LOG("h2 client: {} idle for {}ms", self->m_authority, self->m_idle_timeout.count());
                self->finish_session();
                return;
            }
            self->arm_watchdog();
        });
    }

    void read_loop() {
        if (m_finished)
            return;
        if (!m_recv_buf.empty() && !parse_available()) {
            finish_session();
            return;
        }
        auto self = this->shared_from_this();
        m_transport->async_read_some(
            ByteSpan{m_read_scratch.data(), m_read_scratch.size()}, [self](error_code ec, std::size_t n) {
                if (ec) {
                    self->finish_session();
                    return;
                }
                self->m_deadline = std::chrono::steady_clock::now() + self->m_idle_timeout;
                self->m_recv_buf.append(reinterpret_cast<const char *>(self->m_read_scratch.data()), n);
                if (!self->parse_available()) {
                    self->finish_session();
                    return;
                }
                self->flush();
                self->read_loop();
            });
    }

    void write_loop() {
        if (m_finished)
            return;
        fill_data_frames();
        if (!m_out.empty()) {
            auto chunk = std::make_shared<std::string>(std::move(m_out));
            m_out.clear();
            m_deadline = std::chrono::steady_clock::now() + m_idle_timeout;
            auto self = this->shared_from_this();
            m_transport->async_write(ConstByteSpan{reinterpret_cast<const std::byte *>(chunk->data()), chunk->size()},
                                     [self, chunk](error_code ec, std::size_t) {
                                         if (ec || self->m_finished) {
                                             self->finish_session();
                                             return;
                                         }
                                         self->write_loop();
                                     });
            return;
        }
        if (m_goaway_sent && m_streams.empty()) {
            finish_session();
            return;
        }
        auto self = this->shared_from_this();
        m_notify.async_receive([self](error_code ec) {
            if (ec || self->m_finished) {
                self->finish_session();
                return;
            }
            self->write_loop();
        });
    }

    void finish_session() {
        if (m_finished)
            return;
        m_finished = true;
        m_alive = false;
        m_watchdog->cancel();
        m_notify.close();
        for (auto &[id, st] : m_streams) {
            if (st.out_space)
                st.out_space->close();
            if (st.remote_end) {
                // A complete response may still have parsed bytes in the
                // queue. Marking EOF here makes MultiplexedClientStream::read
                // short-circuit to an empty end and silently drops the body;
                // keep the stream Open until the reader drains it, then the
                // normal remote_end path reports EOF.
                if (st.in_queue.empty())
                    st.outcome->set(StreamState::Eof);
            } else {
                st.failed = true;
                st.outcome->set(StreamState::Disconnect, make_error_code(asio::error::eof));
            }
            if (st.in_notify)
                st.in_notify->close();
        }
        m_active_streams.store(0, std::memory_order_release);
        auto self = this->shared_from_this();
        auto close_transport = [self]() {
            self->m_transport->close();
            SIMPLE_HTTP_ERROR_LOG("h2 client: connection to {} closed", self->m_authority);
        };
        if (m_out.empty()) {
            close_transport();
            return;
        }
        auto chunk = std::make_shared<std::string>(std::move(m_out));
        m_out.clear();
        m_transport->async_write(
            ConstByteSpan{reinterpret_cast<const std::byte *>(chunk->data()), chunk->size()},
            asio::cancel_after(std::chrono::seconds(2),
                               [self, chunk, close_transport](error_code, std::size_t) { close_transport(); }));
    }

    void flush() { (void)m_notify.try_send(error_code{}); }

    // --- frame output ---

    static void append_frame(std::string &out, codec::H2FrameType type, std::uint8_t flags, std::uint32_t stream_id,
                             std::string_view payload) {
        codec::serialize_frame_header(out, static_cast<std::uint32_t>(payload.size()), static_cast<std::uint8_t>(type),
                                      flags, stream_id);
        out.append(payload);
    }

    // HEADERS + CONTINUATION split by the peer's SETTINGS_MAX_FRAME_SIZE. Used
    // for request trailers, whose block is caller-sized and may exceed it.
    void append_field_block(std::string &out, std::uint32_t stream_id, std::string_view block, bool end_stream) const {
        const std::size_t limit = std::max<std::size_t>(1, m_peer_max_frame_size);
        std::size_t off = 0;
        bool first = true;
        do {
            const std::size_t take = std::min(limit, block.size() - off);
            const bool last = off + take == block.size();
            std::uint8_t flags = last ? codec::H2_FLAG_END_HEADERS : 0;
            if (first && end_stream)
                flags |= codec::H2_FLAG_END_STREAM;
            append_frame(out, first ? codec::H2FrameType::Headers : codec::H2FrameType::Continuation, flags, stream_id,
                         block.substr(off, take));
            first = false;
            off += take;
        } while (off < block.size());
    }

    // Splits a header block into HEADERS + CONTINUATION frames, honouring the
    // peer's advertised frame size (a larger frame is a FRAME_SIZE_ERROR there).
    void submit_headers(std::uint32_t id, const std::string &block, bool end_stream) {
        const std::size_t limit =
            std::max<std::size_t>(1, std::min<std::size_t>(m_peer_max_frame_size, m_limits.h2_max_frame_size));
        std::size_t off = 0;
        bool first = true;
        do {
            const std::size_t take = std::min(limit, block.size() - off);
            const bool last = (off + take == block.size());
            std::uint8_t flags = last ? codec::H2_FLAG_END_HEADERS : 0;
            if (first && end_stream)
                flags |= codec::H2_FLAG_END_STREAM;
            append_frame(m_out, first ? codec::H2FrameType::Headers : codec::H2FrameType::Continuation, flags, id,
                         std::string_view{block}.substr(off, take));
            first = false;
            off += take;
        } while (off < block.size());
    }

    void send_rst_stream(std::uint32_t id, std::uint32_t error) {
        std::string payload;
        codec::append_u32(payload, error);
        append_frame(m_out, codec::H2FrameType::RstStream, 0, id, payload);
    }

    void send_window_update(std::uint32_t id, std::uint32_t delta) {
        std::string payload;
        codec::append_u32(payload, delta);
        append_frame(m_out, codec::H2FrameType::WindowUpdate, 0, id, payload);
    }

    void go_away(std::uint32_t error) {
        if (m_goaway_sent)
            return;
        std::string payload;
        codec::append_u32(payload, m_last_stream_id);
        codec::append_u32(payload, error);
        append_frame(m_out, codec::H2FrameType::Goaway, 0, 0, payload);
        m_goaway_sent = true;
        m_alive = false;
        flush();
    }

    // --- frame input ---

    // Parses every complete frame buffered in m_recv_buf. Returns false on a
    // connection-fatal error (GOAWAY already queued).
    bool parse_available() {
        std::size_t pos = 0;
        while (m_recv_buf.size() - pos >= codec::kH2FrameHeaderSize) {
            codec::H2FrameHeader hdr;
            codec::parse_frame_header(std::string_view{m_recv_buf}.substr(pos), hdr);
            // RFC 9113 §4.2: a frame larger than what we advertised is a
            // connection error, and bounding it here also bounds how much a
            // single frame can make us buffer.
            if (hdr.length > m_limits.h2_max_frame_size) {
                go_away(codec::H2_FRAME_SIZE_ERROR);
                return false;
            }
            const std::size_t frame_end = pos + codec::kH2FrameHeaderSize + hdr.length;
            if (frame_end > m_recv_buf.size())
                break; // wait for the rest
            std::string_view payload = std::string_view{m_recv_buf}.substr(pos + codec::kH2FrameHeaderSize, hdr.length);
            if (!handle_frame(hdr, payload))
                return false;
            pos = frame_end;
        }
        if (pos > 0)
            m_recv_buf.erase(0, pos);
        return true;
    }

    bool handle_frame(const codec::H2FrameHeader &hdr, std::string_view payload) {
        switch (static_cast<codec::H2FrameType>(hdr.type)) {
        case codec::H2FrameType::Headers:
            return on_headers(hdr, payload);
        case codec::H2FrameType::Continuation:
            return on_continuation(hdr, payload);
        case codec::H2FrameType::Data:
            return on_data(hdr, payload);
        case codec::H2FrameType::Settings:
            return on_settings(hdr, payload);
        case codec::H2FrameType::WindowUpdate:
            return on_window_update(hdr, payload);
        case codec::H2FrameType::RstStream:
            return on_rst_stream(hdr, payload);
        case codec::H2FrameType::Ping:
            return on_ping(hdr, payload);
        case codec::H2FrameType::Goaway:
            return on_goaway(hdr, payload);
        case codec::H2FrameType::PushPromise:
            // We advertise SETTINGS_ENABLE_PUSH=0, which makes a push a
            // connection error (RFC 9113 §6.6) rather than something to skip.
            SIMPLE_HTTP_ERROR_LOG("h2 client: PUSH_PROMISE while push is disabled");
            go_away(codec::H2_PROTOCOL_ERROR);
            return false;
        case codec::H2FrameType::Priority:
        default:
            return true; // ignored / unknown frame types are skipped
        }
    }

    static std::string_view strip_padding(std::string_view payload, bool padded, bool has_priority, bool &ok) {
        ok = true;
        std::size_t pad_len = 0;
        std::size_t off = 0;
        if (padded) {
            if (payload.empty()) {
                ok = false;
                return {};
            }
            pad_len = static_cast<unsigned char>(payload[0]);
            off = 1;
        }
        if (has_priority) {
            if (payload.size() < off + 5) {
                ok = false;
                return {};
            }
            off += 5;
        }
        if (off + pad_len > payload.size()) {
            ok = false;
            return {};
        }
        return payload.substr(off, payload.size() - off - pad_len);
    }

    // A frame for a stream we never opened (or already forgot) is not
    // necessarily a peer error: it may have raced our RST_STREAM. Tolerate it for
    // odd ids below the next one we would use, crediting back the connection
    // window its payload consumed; anything else is a protocol error.
    //
    // The DATA caller debits the whole frame from the connection window before
    // it knows the stream (RFC 9113 §6.9.1: an entire DATA payload, padding
    // included, counts), so here the full length is credited back — nothing of
    // that frame was delivered to anyone. (It used to take an
    // "already credited" amount that callers passed as the frame length,
    // cancelling the credit to zero and leaking the connection window a little
    // on every tolerated frame until a legitimate frame tripped
    // FLOW_CONTROL_ERROR.)
    bool tolerate_unknown_stream(const codec::H2FrameHeader &hdr) {
        const std::uint32_t id = hdr.stream_id;
        if (id == 0 || (id & 1u) == 0 || id >= m_next_stream_id) {
            SIMPLE_HTTP_ERROR_LOG("h2 client: frame type {} for unexpected stream {}", hdr.type, id);
            go_away(codec::H2_PROTOCOL_ERROR);
            return false;
        }
        if (static_cast<codec::H2FrameType>(hdr.type) == codec::H2FrameType::Data) {
            m_conn_recv_window += static_cast<std::int64_t>(hdr.length);
        }
        return true;
    }

    // A field block for a cancelled/unknown stream still has to reach the
    // decoder: the server's encoder updated its dynamic table when it emitted
    // the block, so skipping it desynchronizes ours (RFC 9113 §4.3.1) and the
    // next response on this pooled connection decodes to wrong values or fails.
    bool decode_discarded_header_block(std::string_view block) {
        std::vector<codec::HpackHeader> ignored;
        if (!m_decoder.decode(block, ignored) && !m_decoder.header_list_too_large()) {
            go_away(codec::H2_COMPRESSION_ERROR);
            return false;
        }
        return true;
    }

    bool on_headers(const codec::H2FrameHeader &hdr, std::string_view payload) {
        if (hdr.stream_id == 0) {
            go_away(codec::H2_PROTOCOL_ERROR);
            return false;
        }
        bool ok = false;
        const std::string_view block =
            strip_padding(payload, hdr.has_flag(codec::H2_FLAG_PADDED), hdr.has_flag(codec::H2_FLAG_PRIORITY), ok);
        if (!ok) {
            go_away(codec::H2_PROTOCOL_ERROR);
            return false;
        }

        auto it = m_streams.find(hdr.stream_id);
        if (it == m_streams.end()) {
            if (!tolerate_unknown_stream(hdr))
                return false;
            if (hdr.has_flag(codec::H2_FLAG_END_HEADERS)) {
                return decode_discarded_header_block(block);
            }
            // Consume the CONTINUATION frames, decoding them at END_HEADERS so
            // the dynamic table stays in sync.
            m_continuation_stream = hdr.stream_id;
            m_continuation_discard = true;
            m_continuation_discard_block.assign(block);
            return true;
        }
        Stream &st = it->second;
        if (st.remote_end || st.failed || st.reset) {
            // Already over: ignore stragglers, but still decode the block.
            if (hdr.has_flag(codec::H2_FLAG_END_HEADERS)) {
                return decode_discarded_header_block(block);
            }
            m_continuation_stream = hdr.stream_id;
            m_continuation_discard = true;
            m_continuation_discard_block.assign(block);
            return true;
        }

        if (st.header_block.empty()) {
            // END_STREAM is only defined on the frame that starts a block.
            st.block_end_stream = hdr.has_flag(codec::H2_FLAG_END_STREAM);
        }
        st.header_block.append(block);
        if (st.header_block.size() > m_limits.max_header_bytes) {
            SIMPLE_HTTP_ERROR_LOG("h2 client: header block on stream {} exceeds {} bytes", hdr.stream_id,
                                  m_limits.max_header_bytes);
            go_away(codec::H2_ENHANCE_YOUR_CALM);
            return false;
        }
        if (hdr.has_flag(codec::H2_FLAG_END_HEADERS))
            return finish_header_block(hdr.stream_id);
        m_continuation_stream = hdr.stream_id; // CONTINUATION frames follow
        return true;
    }

    bool on_continuation(const codec::H2FrameHeader &hdr, std::string_view payload) {
        if (hdr.stream_id == 0 || hdr.stream_id != m_continuation_stream) {
            go_away(codec::H2_PROTOCOL_ERROR);
            return false;
        }
        if (m_continuation_discard) {
            if (payload.size() > m_limits.max_header_bytes ||
                m_continuation_discard_block.size() > m_limits.max_header_bytes - payload.size()) {
                go_away(codec::H2_ENHANCE_YOUR_CALM);
                return false;
            }
            m_continuation_discard_block.append(payload);
            if (hdr.has_flag(codec::H2_FLAG_END_HEADERS)) {
                const bool decoded = decode_discarded_header_block(m_continuation_discard_block);
                m_continuation_stream = 0;
                m_continuation_discard = false;
                m_continuation_discard_block.clear();
                return decoded;
            }
            return true;
        }
        auto it = m_streams.find(hdr.stream_id);
        if (it == m_streams.end()) {
            go_away(codec::H2_PROTOCOL_ERROR);
            return false;
        }
        Stream &st = it->second;
        st.header_block.append(payload);
        if (st.header_block.size() > m_limits.max_header_bytes) {
            go_away(codec::H2_ENHANCE_YOUR_CALM); // RFC 9113 §10.5.1: CONTINUATION flood
            return false;
        }
        if (hdr.has_flag(codec::H2_FLAG_END_HEADERS)) {
            m_continuation_stream = 0;
            return finish_header_block(hdr.stream_id);
        }
        return true;
    }

    // Decodes an accumulated header block into the stream's response head (or
    // trailers, which only confirm the end of the body) and validates it the way
    // RFC 9113 §8.2 requires: a malformed block is a *stream* error, so the
    // connection and every other stream survive.
    bool finish_header_block(std::uint32_t id) {
        auto it = m_streams.find(id);
        if (it == m_streams.end())
            return true;
        Stream &st = it->second;

        std::vector<codec::HpackHeader> fields;
        if (!m_decoder.decode(st.header_block, fields)) {
            if (m_decoder.header_list_too_large()) {
                // Over the SETTINGS_MAX_HEADER_LIST_SIZE we advertised: a stream
                // error (RFC 9113 §6.5.2), not the compression error a malformed
                // block is, so the connection and its other streams survive.
                SIMPLE_HTTP_ERROR_LOG("h2 client: response header list exceeds {} bytes (stream={}); resetting",
                                      m_limits.effective_max_header_list_size(), id);
                reset_stream(id, codec::H2_ENHANCE_YOUR_CALM);
                return true;
            }
            SIMPLE_HTTP_ERROR_LOG("h2 client: HPACK decode failed on stream {} (err={})", id, m_decoder.last_error());
            go_away(codec::H2_COMPRESSION_ERROR); // the shared table is out of sync:
                                                  // fatal
            return false;
        }
        st.header_block.clear();
        const bool end_stream = st.block_end_stream;

        Headers headers;
        int status = 0;
        bool saw_status = false;
        for (auto &f : fields) {
            if (contains_ctl(f.name) || contains_ctl(f.value)) { // §8.2.1
                reset_stream(id, codec::H2_PROTOCOL_ERROR);
                return true;
            }
            if (!f.name.empty() && f.name.front() == ':') {
                // `:status` must come first and only once; any other
                // pseudo-header in a response is malformed. Trailers are a
                // second block and may carry no pseudo-header at all.
                if (st.head_seen || f.name != ":status" || saw_status || !headers.empty()) {
                    reset_stream(id, codec::H2_PROTOCOL_ERROR);
                    return true;
                }
                saw_status = true;
                status = parse_status(f.value);
                if (status == 0) {
                    reset_stream(id, codec::H2_PROTOCOL_ERROR);
                    return true;
                }
                continue;
            }
            for (char c : f.name) { // uppercase field names are malformed (§8.2.1)
                if (c >= 'A' && c <= 'Z') {
                    reset_stream(id, codec::H2_PROTOCOL_ERROR);
                    return true;
                }
            }
            // TE is the one connection-specific field HTTP/2 allows, and only
            // with the value "trailers" (case-insensitive per RFC 9110 §5.6.2).
            if (is_connection_specific(f.name) || (f.name == "te" && !iequals_ci(f.value, "trailers"))) { // §8.2.2
                reset_stream(id, codec::H2_PROTOCOL_ERROR);
                return true;
            }
            headers.add_lower(std::move(f.name), std::move(f.value));
        }
        // A second header block is a trailers section: it carries no `:status`
        // (checked above), so it must be handled *before* the "final head needs a
        // status" rule below — otherwise every legal trailers block was reset as
        // a malformed response. It can only end the stream, never start a second
        // response.
        if (st.head_seen) {
            // A trailing field section: keep it for the caller (the reverse
            // proxy forwards it through Response::finish_trailers()).
            if (!response_is_bodyless(st, st.head.status) && !content_length_ok(st)) {
                reset_stream(id, codec::H2_PROTOCOL_ERROR);
                return true;
            }
            if (auto handle = st.handle.lock())
                handle->set_trailers(std::move(headers));
            st.remote_end = true;
            wake(st);
            return true;
        }
        if (!saw_status) {
            reset_stream(id, codec::H2_PROTOCOL_ERROR);
            return true;
        }
        if (status < 200) {
            // Informational (1xx): not the final response. An asked-for
            // 100-continue is published to the caller so the automatic stream
            // can send its body; every other 1xx is dropped (RFC 9113 §8.1).
            if (status == status::continue_ && st.expect_continue && !st.interim_100_sent) {
                st.interim_100_sent = true;
                ResponseHead interim{};
                interim.status = status::continue_;
                interim.version = Version::Http2;
                interim.headers = std::move(headers);
                interim.bodyless = true;
                if (auto handle = st.handle.lock())
                    handle->set_head(std::move(interim));
                wake(st);
            }
            return true;
        }

        st.head.status = status;
        st.head.version = Version::Http2;
        st.head.headers = std::move(headers);
        // §8.1.2.6: parse its content-length once, rejecting duplicates and
        // values that cannot be represented. The body total is checked as DATA
        // arrives and again at END_STREAM.
        st.declared_content_length = -1;
        st.body_received = 0;
        if (st.head.headers.count("content-length") > 1) {
            reset_stream(id, codec::H2_PROTOCOL_ERROR);
            return true;
        }
        if (auto cl = st.head.headers.get("content-length")) {
            std::uint64_t value = 0;
            auto [ptr, ec] = std::from_chars(cl->data(), cl->data() + cl->size(), value);
            if (cl->empty() || ec != std::errc{} || ptr != cl->data() + cl->size() ||
                value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                reset_stream(id, codec::H2_PROTOCOL_ERROR);
                return true;
            }
            st.declared_content_length = static_cast<std::int64_t>(value);
        }
        // END_STREAM on the header block means the response has no body at all —
        // a response to HEAD, or a 204/304. It is never inferred from a missing
        // content-length.
        st.head.bodyless = end_stream || response_is_bodyless(st, status);
        st.head_seen = true;
        if (end_stream) {
            st.remote_end = true;
            if (!response_is_bodyless(st, status) && !content_length_ok(st)) {
                reset_stream(id, codec::H2_PROTOCOL_ERROR);
                return true;
            }
        }
        // Publish the finished head to the caller's handle: read_head() then
        // works even after the stream has been retired (the session's entry is
        // gone by then, so the handle's own cache is what answers).
        if (auto handle = st.handle.lock())
            handle->set_head(st.head);
        // The outcome stays Open until the application has actually seen the head
        // (stream_read delivers it before it reports the end).
        wake(st);
        return true;
    }

    // Responses to HEAD and 204/304 carry no body and may legitimately declare a
    // content-length (HEAD) without any DATA; the length check must skip them.
    static bool response_is_bodyless(const Stream &st, int status) {
        return st.is_head || status == status::no_content || status == status::not_modified;
    }

    // RFC 9113 §8.1.2.6: when the body ends, the sum of DATA payload lengths
    // must equal a declared content-length. `reset_stream` also handles the
    // flow-control credit for bytes already queued.
    bool content_length_ok(const Stream &st) const {
        return st.declared_content_length < 0 ||
               static_cast<std::int64_t>(st.body_received) == st.declared_content_length;
    }

    static int parse_status(std::string_view value) {
        if (value.size() != 3)
            return 0;
        int status = 0;
        for (char c : value) {
            if (c < '0' || c > '9')
                return 0;
            status = status * 10 + (c - '0');
        }
        return (status >= 100 && status <= 599) ? status : 0;
    }

    bool on_data(const codec::H2FrameHeader &hdr, std::string_view payload) {
        if (hdr.stream_id == 0) {
            go_away(codec::H2_PROTOCOL_ERROR);
            return false;
        }
        bool ok = false;
        const std::string_view data = strip_padding(payload, hdr.has_flag(codec::H2_FLAG_PADDED), false, ok);
        if (!ok) {
            go_away(codec::H2_PROTOCOL_ERROR);
            return false;
        }

        // The whole payload (padding included) counts against flow control
        // (RFC 9113 §6.9.1). Debit the connection window first: overrunning it is
        // a connection-level error.
        const std::int64_t frame_len = static_cast<std::int64_t>(hdr.length);
        m_conn_recv_window -= frame_len;
        if (m_conn_recv_window < 0) {
            go_away(codec::H2_FLOW_CONTROL_ERROR);
            return false;
        }

        auto it = m_streams.find(hdr.stream_id);
        if (it == m_streams.end())
            return tolerate_unknown_stream(hdr);
        Stream &st = it->second;
        if (st.remote_end || st.failed || st.reset || !st.head_seen) {
            // After END_STREAM (or on an unknown/closed stream) the bytes are not
            // ours to deliver: hand the connection credit straight back.
            m_conn_recv_window += frame_len;
            if (!st.head_seen && !st.remote_end)
                reset_stream(hdr.stream_id, codec::H2_PROTOCOL_ERROR);
            return true;
        }

        // §8.1.2.6: a body longer than the declared length is malformed. Check
        // before queueing/crediting: the frame is not delivered, so hand the
        // whole connection credit back (padding included) and reset the stream.
        if (!data.empty()) {
            st.body_received += data.size();
            if (!response_is_bodyless(st, st.head.status) && st.declared_content_length >= 0 &&
                static_cast<std::int64_t>(st.body_received) > st.declared_content_length) {
                m_conn_recv_window += frame_len;
                reset_stream(hdr.stream_id, codec::H2_PROTOCOL_ERROR);
                return true;
            }
        }

        const std::int64_t padding = frame_len - static_cast<std::int64_t>(data.size());
        if (padding > 0)
            credit_consumed(st, padding); // never delivered, so credit it now
        if (!data.empty()) {
            st.recv_owed_conn += static_cast<std::int64_t>(data.size());
            st.in_bytes += data.size();
            st.in_queue.emplace_back(data);
        }
        if (hdr.has_flag(codec::H2_FLAG_END_STREAM)) {
            st.remote_end = true;
            // Short body: fewer DATA bytes than declared. The queued bytes are
            // discarded by erase_stream (which returns their credit).
            if (!response_is_bodyless(st, st.head.status) && !content_length_ok(st)) {
                reset_stream(hdr.stream_id, codec::H2_PROTOCOL_ERROR);
                return true;
            }
        }
        wake(st);
        return true;
    }

    bool on_settings(const codec::H2FrameHeader &hdr, std::string_view payload) {
        if (hdr.has_flag(codec::H2_FLAG_ACK))
            return true; // our SETTINGS was acked
        if (hdr.stream_id != 0 || payload.size() % 6 != 0) {
            go_away(codec::H2_FRAME_SIZE_ERROR);
            return false;
        }
        for (std::size_t i = 0; i + 6 <= payload.size(); i += 6) {
            const std::uint16_t id = static_cast<std::uint16_t>((static_cast<unsigned char>(payload[i]) << 8) |
                                                                static_cast<unsigned char>(payload[i + 1]));
            const std::uint32_t value = codec::read_u32(payload, i + 2);
            switch (id) {
            case codec::H2_SETTINGS_INITIAL_WINDOW_SIZE: {
                if (value > 0x7FFFFFFFu) { // RFC 9113 §6.5.2
                    go_away(codec::H2_FLOW_CONTROL_ERROR);
                    return false;
                }
                // Applies retroactively to every open stream's send window.
                const std::int64_t delta = static_cast<std::int64_t>(value) - m_peer_initial_window;
                m_peer_initial_window = static_cast<std::int32_t>(value);
                for (auto &[sid, st] : m_streams)
                    st.send_window += delta;
                break;
            }
            case codec::H2_SETTINGS_MAX_FRAME_SIZE:
                if (value < 16384u || value > 16777215u) {
                    go_away(codec::H2_PROTOCOL_ERROR);
                    return false;
                }
                m_peer_max_frame_size = value;
                break;
            case codec::H2_SETTINGS_MAX_CONCURRENT_STREAMS:
                m_peer_max_concurrent_streams = value;
                m_peer_max_concurrent_announced = true; // 0 is meaningfully different from unannounced
                break;
            case codec::H2_SETTINGS_HEADER_TABLE_SIZE:
                // This bounds the table *our encoder* may use. We never index
                // (every field is a literal), so there is nothing to resize;
                // our decoder's table is governed by what we advertise.
                break;
            default:
                break; // accepted and ignored
            }
        }
        std::string ack;
        append_frame(ack, codec::H2FrameType::Settings, codec::H2_FLAG_ACK, 0, {});
        m_out.append(ack);
        return true;
    }

    bool on_window_update(const codec::H2FrameHeader &hdr, std::string_view payload) {
        if (payload.size() != 4) {
            go_away(codec::H2_FRAME_SIZE_ERROR);
            return false;
        }
        const std::uint32_t increment = codec::read_u32(payload, 0) & 0x7FFFFFFF;
        if (increment == 0) {
            // RFC 9113 §6.9: a zero increment is a stream error, or a connection
            // error on stream 0.
            if (hdr.stream_id == 0) {
                go_away(codec::H2_PROTOCOL_ERROR);
                return false;
            }
            reset_stream(hdr.stream_id, codec::H2_PROTOCOL_ERROR);
            return true;
        }
        if (hdr.stream_id == 0) {
            m_conn_send_window += increment;
            if (m_conn_send_window > 0x7FFFFFFF) {
                go_away(codec::H2_FLOW_CONTROL_ERROR);
                return false;
            }
        } else {
            auto it = m_streams.find(hdr.stream_id);
            if (it != m_streams.end()) {
                it->second.send_window += increment;
                if (it->second.send_window > 0x7FFFFFFF) {
                    reset_stream(hdr.stream_id, codec::H2_FLOW_CONTROL_ERROR);
                    return true;
                }
            }
        }
        fill_data_frames(); // a window opened: more DATA may be frameable now
        return true;
    }

    bool on_rst_stream(const codec::H2FrameHeader &hdr, std::string_view payload) {
        if (hdr.stream_id == 0 || payload.size() != 4) {
            go_away(hdr.stream_id == 0 ? codec::H2_PROTOCOL_ERROR : codec::H2_FRAME_SIZE_ERROR);
            return false;
        }
        auto it = m_streams.find(hdr.stream_id);
        if (it == m_streams.end())
            return tolerate_unknown_stream(hdr);
        const std::uint32_t error = codec::read_u32(payload, 0);
        it->second.reset = true;
        it_finish(it, StreamState::Reset,
                  error == codec::H2_REFUSED_STREAM ? make_error_code(client_errc::stream_refused)
                                                    : make_error_code(client_errc::stream_reset));
        return true;
    }

    bool on_goaway(const codec::H2FrameHeader &hdr, std::string_view payload) {
        if (hdr.stream_id != 0 || payload.size() < 8) {
            go_away(codec::H2_PROTOCOL_ERROR);
            return false;
        }
        m_goaway_received = true;
        const std::uint32_t last_stream_id = codec::read_u32(payload, 0) & 0x7FFFFFFF;
        const std::uint32_t error = codec::read_u32(payload, 4);
        // Read for the record (and the log, which may be compiled out): the code
        // itself does not change what we do beyond draining the connection.
        (void)error;
        SIMPLE_HTTP_ERROR_LOG("h2 client: GOAWAY from {} (last_stream_id={}, error={})", m_authority, last_stream_id,
                              error);
        // Streams above last_stream_id were never processed, so they may safely be
        // retried elsewhere; the rest still get their responses.
        for (auto it = m_streams.begin(); it != m_streams.end();) {
            if (it->first > last_stream_id) {
                it->second.failed = true;
                auto next = std::next(it);
                it_finish(it, StreamState::Failed, make_error_code(client_errc::stream_refused));
                it = next;
            } else {
                ++it;
            }
        }
        return true;
    }

    bool on_ping(const codec::H2FrameHeader &hdr, std::string_view payload) {
        if (hdr.has_flag(codec::H2_FLAG_ACK))
            return true;
        if (payload.size() != 8) {
            go_away(codec::H2_FRAME_SIZE_ERROR);
            return false;
        }
        std::string ack;
        append_frame(ack, codec::H2FrameType::Ping, codec::H2_FLAG_ACK, 0,
                     payload); // echo
        m_out.append(ack);
        return true;
    }

    // --- flow control ---

    // Returns `n` octets of connection-level receive credit, batched: accumulate
    // until the threshold, then emit one WINDOW_UPDATE.
    void replenish_conn(std::int64_t n) {
        m_conn_recv_pending += n;
        // Batch against the real connection window (>= 65535), not a configured
        // per-stream INITIAL_WINDOW_SIZE that may be smaller or zero.
        if (m_conn_recv_pending >= std::max(m_limits.h2_initial_window, kClientInitialWindow) / 2) {
            send_window_update(0, static_cast<std::uint32_t>(m_conn_recv_pending));
            m_conn_recv_window += m_conn_recv_pending;
            m_conn_recv_pending = 0;
        }
    }

    void credit_consumed(Stream &st, std::int64_t n) {
        st.recv_pending += n;
        if (st.recv_pending >= m_limits.h2_initial_window / 2) {
            send_window_update(st.id, static_cast<std::uint32_t>(st.recv_pending));
            st.recv_pending = 0;
        }
        replenish_conn(n);
    }

    void wake(Stream &st) {
        if (st.in_notify)
            (void)st.in_notify->try_send(error_code{});
    }

    // --- streams ---

    // Records a stream's terminal state on its outcome (so the handle keeps
    // answering) and wakes a reader parked on it.
    void it_finish(typename std::unordered_map<std::uint32_t, Stream>::iterator it, StreamState state,
                   error_code ec = {}) {
        Stream &st = it->second;
        st.failed = st.failed || state == StreamState::Failed;
        st.outcome->set(state, ec);
        // Erase before waking. The wake can resume a reader inline on this
        // executor, and that reader retires the stream itself — which would leave
        // the erase below with an iterator into a node it had just dropped.
        // `outcome` is a shared_ptr, so the reader still reports the terminal
        // state after the entry is gone.
        auto notify = st.in_notify;
        erase_stream(it);
        if (notify) {
            (void)notify->try_send(error_code{});
        }
    }

    void reset_stream(std::uint32_t id, std::uint32_t error) {
        auto it = m_streams.find(id);
        if (it == m_streams.end())
            return;
        send_rst_stream(id, error);
        it->second.reset = true;
        it_finish(it, StreamState::Reset, make_error_code(client_errc::stream_reset));
        flush();
    }

    // Drops a stream's state, first returning the connection-level credit it
    // still owed for delivered-but-unconsumed bytes (otherwise the connection
    // window would leak over a long-lived connection).
    void erase_stream(typename std::unordered_map<std::uint32_t, Stream>::iterator it) {
        Stream &st = it->second;
        if (st.recv_owed_conn > 0) {
            replenish_conn(st.recv_owed_conn);
            st.recv_owed_conn = 0;
        }
        if (st.out_space)
            st.out_space->close();
        m_streams.erase(it);
        stream_closed();
    }

    void fail_all_streams(client_errc code) {
        // State first, wakes afterwards. A wake can resume a reader inline on this
        // executor, and that reader retires its stream — mutating the very map
        // this loop is walking. Collecting the channels and signalling them once
        // the walk is over keeps the traversal safe.
        std::vector<std::shared_ptr<asio::experimental::concurrent_channel<void(error_code)>>> notifies;
        for (auto &[id, st] : m_streams) {
            st.failed = true;
            st.error = make_error_code(code);
            st.outcome->set(StreamState::Failed, st.error);
            if (st.out_space)
                st.out_space->close();
            if (st.in_notify)
                notifies.push_back(st.in_notify);
        }
        for (auto &notify : notifies) {
            (void)notify->try_send(error_code{});
        }
    }

    // Parks a writer until this stream's outbound queue drains below the low
    // mark. Returns an error once the stream is gone, so a parked writer is never
    // stranded.
    //
    // Takes the id rather than a `Stream&`: this coroutine suspends, and a
    // reference taken before the suspension would be dangling if the stream were
    // retired meanwhile — which is exactly what a reset or a GOAWAY does.
    void await_out_space(std::uint32_t id, WriteCompletion done) {
        auto it = m_streams.find(id);
        if (it == m_streams.end()) {
            std::move(done)(make_error_code(client_errc::session_closed));
            return;
        }
        Stream &st = it->second;
        if (st.failed || st.reset) {
            std::move(done)(st.error ? st.error : make_error_code(client_errc::stream_reset));
            return;
        }
        if (st.out_queued <= kClientOutHighWatermark) {
            std::move(done)(error_code{});
            return;
        }
        if (!st.out_space) {
            st.out_space = std::make_shared<asio::experimental::concurrent_channel<void(error_code)>>(m_executor, 1);
        }
        auto space = st.out_space;
        auto outcome = st.outcome;
        auto self = this->shared_from_this();
        space->async_receive([self, id, space, outcome, done = std::move(done)](error_code ec) mutable {
            if (ec) {
                // A reset/failed stream closed the space; report why it died
                // rather than a generic session_closed.
                const auto state = outcome->get();
                if (state == StreamState::Reset || state == StreamState::Failed || state == StreamState::Disconnect) {
                    std::move(done)(outcome_error(*outcome));
                } else {
                    std::move(done)(make_error_code(client_errc::session_closed));
                }
                return;
            }
            self->await_out_space(id, std::move(done));
        });
    }

    // --- send side: turn queued DATA into flow-controlled frames ---

    void fill_data_frames() {
        std::vector<std::uint32_t> finished;
        for (auto &[id, st] : m_streams) {
            if (st.local_end || st.reset || st.failed)
                continue;
            for (;;) {
                if (st.out_queue.empty()) {
                    if (st.out_finished) {
                        if (st.out_trailers_set) {
                            // A trailing HEADERS block carries END_STREAM; the
                            // preceding final DATA frame must not. Split it like
                            // the initial block: a single oversized HEADERS is a
                            // FRAME_SIZE_ERROR to the peer.
                            append_field_block(m_out, id, st.out_trailers, /*end_stream=*/true);
                            st.out_trailers_set = false;
                        } else {
                            // The body is done: an empty DATA frame carries END_STREAM.
                            append_frame(m_out, codec::H2FrameType::Data, codec::H2_FLAG_END_STREAM, id, {});
                        }
                        st.local_end = true;
                        finished.push_back(id);
                        wake(st);
                    }
                    break;
                }
                const std::int64_t window = std::min<std::int64_t>(m_conn_send_window, st.send_window);
                if (window <= 0)
                    break; // blocked on flow control until WINDOW_UPDATE

                std::string &front = st.out_queue.front();
                const std::size_t available = front.size() - st.out_offset;
                const std::size_t budget = static_cast<std::size_t>(
                    std::min<std::int64_t>(window, static_cast<std::int64_t>(std::min<std::uint32_t>(
                                                       m_peer_max_frame_size, m_limits.h2_max_frame_size))));
                const std::size_t take = std::min(available, budget);
                if (take == 0)
                    break; // nothing can advance: stop rather than spin

                const bool front_done = (st.out_offset + take >= front.size());
                const bool last = front_done && st.out_queue.size() == 1 && st.out_finished && !st.out_trailers_set;
                append_frame(m_out, codec::H2FrameType::Data, last ? codec::H2_FLAG_END_STREAM : 0, id,
                             std::string_view{front.data() + st.out_offset, take});
                st.out_offset += take;
                m_conn_send_window -= static_cast<std::int64_t>(take);
                st.send_window -= static_cast<std::int64_t>(take);
                st.out_queued -= take;
                if (st.out_queued <= kClientOutLowWatermark && st.out_space) {
                    (void)st.out_space->try_send(error_code{});
                }
                if (front_done) {
                    st.out_queue.pop_front();
                    st.out_offset = 0;
                }
                if (last) {
                    st.local_end = true;
                    finished.push_back(id);
                    break;
                }
            }
        }
        // Streams complete in both directions are retired — after the iteration,
        // because this erases entries. A retired stream's handle still answers
        // through its outcome. Only when nothing is left to deliver: response
        // bytes queued but not yet read belong to the application.
        for (std::uint32_t id : finished) {
            auto it = m_streams.find(id);
            if (it != m_streams.end() && it->second.remote_end && it->second.in_queue.empty()) {
                it_finish(it, StreamState::Eof);
            }
        }
    }

    // --- members (touched only on the connection executor); the executor,
    // authority, lifetime flags and active-stream counter live on the base ---
    std::shared_ptr<Transport> m_transport;
    ClientTarget m_target;
    EngineLimits m_limits;
    std::chrono::milliseconds m_idle_timeout;

    asio::experimental::concurrent_channel<void(error_code)> m_notify;
    std::chrono::steady_clock::time_point m_deadline{};
    std::shared_ptr<asio::steady_timer> m_watchdog{std::make_shared<asio::steady_timer>(m_executor)};
    bool m_finished{false};
    std::array<std::byte, 32 * 1024> m_read_scratch{};

    codec::HpackDecoder m_decoder; // connection-scoped: the dynamic table is shared
    std::unordered_map<std::uint32_t, Stream> m_streams;

    std::string m_recv_buf;
    std::string m_out;

    std::uint32_t m_next_stream_id{1}; // client-initiated ids are odd
    std::uint32_t m_last_stream_id{0};
    std::uint32_t m_continuation_stream{0};
    bool m_continuation_discard{false};
    std::string m_continuation_discard_block;

    std::int64_t m_conn_send_window{kClientInitialWindow};
    std::int64_t m_conn_recv_window{kClientInitialWindow};
    std::int64_t m_conn_recv_pending{0};
    std::int32_t m_peer_initial_window{kClientInitialWindow};
    std::uint32_t m_peer_max_frame_size{static_cast<std::uint32_t>(kClientMaxFrameSize)};
    std::uint32_t m_peer_max_concurrent_streams{0}; // announced limit; uncontrolled until announced
    bool m_peer_max_concurrent_announced{false};    // the peer sent a SETTINGS_MAX_CONCURRENT_STREAMS

    bool m_goaway_sent{false};
};

} // namespace simple_http::detail
