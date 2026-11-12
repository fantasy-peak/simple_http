#pragma once

// HTTP/2 engine — self-contained frame codec (no nghttp2).
//
// Http2Engine drives one connection with concurrent coroutines on the
// connection executor:
//   * read_loop:  transport bytes -> frame parser -> stream table / body feed
//   * write_loop: pending control frames + per-stream DATA (flow-controlled)
//                 -> transport bytes, woken by a notify channel
//   * watchdog:   closes the connection after an idle timeout
//
// Framing and HPACK come from the sibling headers in this directory
// (namespace simple_http::codec): frame
// header parse/serialize (h2_frame.h), HPACK decode (HpackDecoder) and a small
// fresh HPACK response encoder (hpack_encoder.h). All connection state
// (stream table, HPACK decoder, flow-control windows, output buffer) is touched
// only on the single-threaded connection executor. Per "model A", every
// Http2ResponseSink operation hops onto that executor before touching engine
// state, so a Response used from any thread stays safe.
//
// Lifetime: the engine is held by shared_ptr; handler coroutines capture it, so
// it (and the stream state) outlives every in-flight handler, including
// streaming handlers that suspend after run() would otherwise have returned.
// Http2ResponseSink holds a weak_ptr and lock()s it per operation.

#include <array>
#include <boost/asio.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "../../core/base64.h"
#include "../../core/http_field.h" // is_connection_specific_field / is_hop_by_hop_field
#include "../../core/http_method.h"
#include "../../core/limits.h"
#include "../../core/logging.h"
#include "../../core/types.h"
#include "../../core/version.h"
#include "../../proto/compressing_writer.h"
#include "../../proto/headers.h"
#include "../../proto/request.h"
#include "../../proto/response.h"
#include "../../proto/response_writer.h"
#include "../../proto/websocket.h"
#include "../../transport/transport.h" // SslHandle, TransportLike
#include "../dispatcher.h"
#include "../ws_proxy_common.h"
#include "h2_frame.h"
#include "h2_ws_transport.h"
#include "hpack_decode.h"
#include "hpack_encoder.h"

namespace simple_http {

namespace asio = boost::asio;

template <typename Transport> class Http2Engine;

// Default HTTP/2 flow-control window (RFC 7540 §6.9.2): 65,535 octets.
inline constexpr std::int32_t kH2InitialWindow = 65535;

// The largest flow-control window either endpoint may hold (RFC 9113 §6.9.1).
// Exceeding it is a FLOW_CONTROL_ERROR, and it is also the bound that keeps the
// signed window arithmetic from wrapping.
inline constexpr std::int64_t kH2MaxWindow = 2147483647; // 2^31 - 1
// Protocol default max frame size (RFC 7540 §6.5.2): the value a peer uses
// until it announces its own SETTINGS_MAX_FRAME_SIZE.
inline constexpr std::size_t kH2MaxFrameSize = 16384;

// Outbound backpressure watermarks for one stream's response queue. A producer
// (the reverse proxy streaming an upstream body, say) is parked once it has
// this many bytes queued but not yet framed, and released when the write loop
// has drained the queue back to the low mark.
//
// Without this a response body was simply accumulated: the handler ran ahead of
// the peer's flow-control window at full speed, so a client that stopped
// reading (window exhausted - congestion, a busy browser main thread, a mid-way
// RST) left the *entire* response sitting in memory, per stream. Measured with
// the 19 MiB code-server workbench.js: RSS went 12 MiB -> 30 MiB and stayed
// there while the client read nothing. The marks bound per-stream memory to
// roughly the high watermark regardless of response size.
inline constexpr std::size_t kOutHighWatermark = 1u << 20;  // 1 MiB: park the producer
inline constexpr std::size_t kOutLowWatermark = 256u << 10; // 256 KiB: wake it again

// Minimum consumed prefix (in queue entries) before a vector-backed queue
// compacts. Advancing a read cursor is not enough on its own: a queue that is
// continuously refilled - any streaming response - never drains completely, so
// the cursor never reaches the end and `clear()` never runs. Every chunk string
// ever enqueued then stays alive until the response ends. Measured on the
// XHTTP/HTTP2 video path: a 40 MiB response kept ~38 MiB of 64 KiB strings
// resident even though the flow-control watermarks held the *unframed* queue at
// 1 MiB. Compaction destroys the consumed prefix; the fraction below keeps the
// cost amortized O(1) per pop while bounding the retained prefix.
inline constexpr std::size_t kQueueCompactFloor = 8;
inline constexpr std::size_t kQueueCompactDivisor = 3;

// Upper bound on how many bytes of DATA one fill_data_frames() pass may frame
// into the connection's output buffer (m_out). Without it, m_out grows to the
// peer's whole flow-control window: a client that advertises a 4 MiB window
// (Go/xray's HTTP/2 client does) made each connection frame several MiB before
// the write loop was given a chance to drain, so every connection held multi-MB
// output buffers (measured: 2-4 MiB std::string allocations inside
// append_frame, per connection). Those are freed when the connection closes,
// but on a server that churns connections the allocator retains that much more,
// which shows up as slowly rising RSS. Capping a pass keeps m_out roughly this
// size regardless of the peer's window; the write loop simply refills after
// each write, so throughput is unaffected (it writes this much per syscall
// anyway).
inline constexpr std::size_t kMaxFramedPerPass = 256u << 10; // 256 KiB

// ResponseSink for a single HTTP/2 stream. Holds a weak_ptr to the engine so it
// can be used safely from any thread and after the connection has closed.
template <typename Transport>
class Http2ResponseSink final : public ResponseSink, public std::enable_shared_from_this<Http2ResponseSink<Transport>> {
  public:
    using Executor = asio::any_io_executor;

    Http2ResponseSink(std::weak_ptr<Http2Engine<Transport>> engine, std::uint32_t stream_id, Executor exec)
        : m_engine(std::move(engine)), m_stream_id(stream_id), m_executor(std::move(exec)) {}

    void send(int status, Headers headers, std::string body, WriteCompletion done) override {
        on_executor([self = this->shared_from_this(), status, headers = std::move(headers), body = std::move(body),
                     done = std::move(done)]() mutable {
            auto eng = self->m_engine.lock();
            if (!eng || !eng->alive() || !eng->stream_writable(self->m_stream_id)) {
                std::move(done)(make_error_code(asio::error::not_connected));
                return;
            }
            ensure_date(headers);
            const bool head = eng->method_is_head(self->m_stream_id);
            if (head) {
                headers.erase("content-length");
                headers.add_lower("content-length", std::to_string(body.size()));
            }
            eng->submit_headers(self->m_stream_id, status, headers, /*end_stream=*/head);
            if (!head)
                eng->enqueue_body(self->m_stream_id, std::move(body), /*last=*/true);
            std::move(done)(error_code{});
        });
    }

    void send_bodyless(int status, Headers headers, WriteCompletion done) override {
        on_executor(
            [self = this->shared_from_this(), status, headers = std::move(headers), done = std::move(done)]() mutable {
                auto eng = self->m_engine.lock();
                if (!eng || !eng->alive() || !eng->stream_writable(self->m_stream_id)) {
                    std::move(done)(make_error_code(asio::error::not_connected));
                    return;
                }
                // 204 and 1xx carry no content; a handler-set Content-Length
                // would describe a body that cannot exist. 304 may keep one
                // (RFC 9110 §15.4.5), matching h1.
                if (status == status::no_content || (status >= 100 && status < 200)) {
                    headers.erase("content-length");
                }
                ensure_date(headers);
                eng->submit_headers(self->m_stream_id, status, headers, /*end_stream=*/true);
                std::move(done)(error_code{});
            });
    }

    void send_continue(WriteCompletion done) override {
        on_executor([self = this->shared_from_this(), done = std::move(done)]() mutable {
            auto eng = self->m_engine.lock();
            if (!eng || !eng->alive() || !eng->stream_writable(self->m_stream_id)) {
                std::move(done)(make_error_code(asio::error::not_connected));
                return;
            }
            if (!self->m_continue_sent) {
                self->m_continue_sent = true;
                eng->submit_headers(self->m_stream_id, status::continue_, Headers{}, /*end_stream=*/false);
                eng->flush();
            }
            std::move(done)(error_code{});
        });
    }

    void send_headers(int status, Headers headers, WriteCompletion done) override {
        on_executor(
            [self = this->shared_from_this(), status, headers = std::move(headers), done = std::move(done)]() mutable {
                auto eng = self->m_engine.lock();
                if (!eng || !eng->alive() || !eng->stream_writable(self->m_stream_id)) {
                    std::move(done)(make_error_code(asio::error::not_connected));
                    return;
                }
                ensure_date(headers);
                headers.erase("content-length");
                eng->submit_headers(self->m_stream_id, status, headers,
                                    /*end_stream=*/eng->method_is_head(self->m_stream_id));
                eng->flush();
                std::move(done)(error_code{});
            });
    }

    void send_chunk(std::string data, WriteCompletion done) override {
        on_executor([self = this->shared_from_this(), data = std::move(data), done = std::move(done)]() mutable {
            auto eng = self->m_engine.lock();
            if (!eng || !eng->alive()) {
                std::move(done)(make_error_code(asio::error::not_connected));
                return;
            }
            if (self->m_head_request) {
                std::move(done)(error_code{});
                return;
            }
            if (!eng->stream_writable(self->m_stream_id)) {
                std::move(done)(make_error_code(asio::error::not_connected));
                return;
            }
            auto self_keep = self;
            eng->await_out_space(self->m_stream_id, [self_keep, eng, data = std::move(data),
                                                     done = std::move(done)](error_code ec) mutable {
                if (ec) {
                    std::move(done)(ec);
                    return;
                }
                eng->enqueue_body(self_keep->m_stream_id, std::move(data), /*last=*/false);
                std::move(done)(error_code{});
            });
        });
    }

    void send_last(std::string data, WriteCompletion done) override {
        on_executor([self = this->shared_from_this(), data = std::move(data), done = std::move(done)]() mutable {
            auto eng = self->m_engine.lock();
            if (!eng || !eng->alive()) {
                std::move(done)(make_error_code(asio::error::not_connected));
                return;
            }
            if (self->m_head_request) {
                std::move(done)(error_code{});
                return;
            }
            if (!eng->stream_writable(self->m_stream_id)) {
                std::move(done)(make_error_code(asio::error::not_connected));
                return;
            }
            auto self_keep = self;
            eng->await_out_space(self->m_stream_id, [self_keep, eng, data = std::move(data),
                                                     done = std::move(done)](error_code ec) mutable {
                if (ec) {
                    std::move(done)(ec);
                    return;
                }
                eng->enqueue_body(self_keep->m_stream_id, std::move(data), /*last=*/true);
                std::move(done)(error_code{});
            });
        });
    }

    void send_trailers(Headers trailers, WriteCompletion done) override {
        on_executor(
            [self = this->shared_from_this(), trailers = std::move(trailers), done = std::move(done)]() mutable {
                auto eng = self->m_engine.lock();
                if (!eng || !eng->alive()) {
                    std::move(done)(make_error_code(asio::error::not_connected));
                    return;
                }
                if (self->m_head_request) {
                    std::move(done)(error_code{});
                    return;
                }
                if (!eng->stream_writable(self->m_stream_id)) {
                    std::move(done)(make_error_code(asio::error::not_connected));
                    return;
                }
                eng->submit_trailers(self->m_stream_id, trailers);
                std::move(done)(error_code{});
            });
    }

    void connected(ConnectedCompletion done) const override {
        on_executor([self = this->shared_from_this(), done = std::move(done)]() mutable {
            auto eng = self->m_engine.lock();
            std::move(done)(error_code{}, eng && eng->alive() && eng->stream_writable(self->m_stream_id));
        });
    }

    void close(WriteCompletion done) override {
        on_executor([self = this->shared_from_this(), done = std::move(done)]() mutable {
            if (auto eng = self->m_engine.lock()) {
                eng->close_stream(self->m_stream_id);
            }
            std::move(done)(error_code{});
        });
    }

    Version version() const override { return Version::Http2; }
    asio::any_io_executor get_executor() const override { return m_executor; }
    bool head_request() const noexcept override { return m_head_request; }

    void set_head_request(bool head) noexcept { m_head_request = head; }

  private:
    template <typename Fn> void on_executor(Fn &&fn) const {
        asio::dispatch(asio::bind_executor(asio::any_io_executor{m_executor}, std::forward<Fn>(fn)));
    }

    std::weak_ptr<Http2Engine<Transport>> m_engine;
    std::uint32_t m_stream_id;
    asio::any_io_executor m_executor;
    bool m_head_request{false};
    bool m_continue_sent{false};
};

template <typename Transport> class Http2Engine : public std::enable_shared_from_this<Http2Engine<Transport>> {
  public:
    using Executor = decltype(std::declval<Transport &>().get_executor());

    explicit Http2Engine(std::shared_ptr<Transport> transport, EngineLimits limits = {})
        : m_transport(std::move(transport)), m_executor(m_transport->get_executor()), m_notify(m_executor, 1),
          m_limits(limits) {
        // Our advertised (receive) windows start at the configured initial size.
        // SETTINGS_INITIAL_WINDOW_SIZE only governs *stream* windows, though: the
        // connection window is 65535 until a WINDOW_UPDATE raises it, so a
        // configured value below that cannot shrink the peer's connection window
        // and one above it must be granted explicitly (see queue_settings()).
        m_conn_recv_window = std::max<std::int32_t>(m_limits.h2_initial_window, kH2InitialWindow);
        // Bound the *decoded* header-list size as well as the compressed block:
        // indexed references make a tiny block expand without bound.
        m_decoder.set_max_list_size(m_limits.effective_max_header_list_size());
    }

    Http2Engine(const Http2Engine &) = delete;
    Http2Engine &operator=(const Http2Engine &) = delete;

    // Serves the connection until it closes. `prior_knowledge_bytes` are bytes
    // already consumed during protocol detection (the client preface for
    // prior-knowledge h2). `on_done` runs exactly once, after the transport has
    // been shut down and closed.
    void run(Dispatcher dispatch, std::string prior_knowledge_bytes, WsLookup ws_lookup, WsProxyLookup ws_proxy_lookup,
             WsLookup ws_regex_lookup, std::function<void()> on_done) {
        m_dispatch = std::move(dispatch);
        m_ws_lookup = std::move(ws_lookup);
        m_ws_proxy_lookup = std::move(ws_proxy_lookup);
        m_ws_regex_lookup = std::move(ws_regex_lookup);
        m_on_done = std::move(on_done);
        queue_settings();
        m_recv_buf = std::move(prior_knowledge_bytes);
        serve_loops();
    }

    // Serves a connection upgraded from HTTP/1.1 (RFC 9113 §3.2). The client's
    // base64url HTTP2-Settings seed the peer SETTINGS, and the original HTTP/1.1
    // request is replayed as stream 1, already complete (its body arrived with
    // the upgrade request). `prior_knowledge_bytes` are the bytes read past that
    // request -- the client's connection preface and whatever followed it.
    void run_h2c(Dispatcher dispatch, std::string settings_b64, Method method, std::string method_token,
                 std::string target, Headers headers, std::string body, std::string prior_knowledge_bytes,
                 WsLookup ws_lookup, WsProxyLookup ws_proxy_lookup, WsLookup ws_regex_lookup,
                 std::function<void()> on_done) {
        m_dispatch = std::move(dispatch);
        m_ws_lookup = std::move(ws_lookup);
        m_ws_proxy_lookup = std::move(ws_proxy_lookup);
        m_ws_regex_lookup = std::move(ws_regex_lookup);
        m_on_done = std::move(on_done);

        std::string settings = base64_url_decode(settings_b64);
        // A SETTINGS body is a whole number of 6-octet entries (RFC 9113 §6.5).
        // apply_settings_payload() ignores a trailing partial entry, so a
        // truncated base64url value must be rejected here.
        if (settings.size() % 6 != 0) {
            go_away(codec::H2_FRAME_SIZE_ERROR);
            serve_loops();
            return;
        }
        apply_settings_payload(settings);
        if (m_goaway_sent) {
            serve_loops();
            return;
        }
        queue_settings();

        // Replay the upgrade request as stream 1. Its body is already complete,
        // so the read side is closed; without half_closed_remote the stream
        // would never retire and would leak an executing-handler slot.
        Stream &st = ensure_stream(1);
        st.request->set_method(method);
        st.request->set_method_token(std::move(method_token));
        st.request->set_target(std::move(target));
        st.request->mutable_headers() = std::move(headers);
        if (!body.empty()) {
            (void)st.request->body().feed(std::move(body));
        }
        (void)st.request->body().finish();
        st.half_closed_remote = true;
        m_next_peer_stream_id = 1;
        (void)dispatch_stream(1);

        m_recv_buf = std::move(prior_knowledge_bytes);
        serve_loops();
    }

    bool alive() const { return m_alive; }
    Executor get_executor() { return m_executor; }

    // Wake the write loop to drain pending frames / DATA.
    void flush() { (void)m_notify.try_send(error_code{}); }

    // --- called by Http2ResponseSink (already hopped onto our executor) ---

    // Serialize a response HEADERS block for `stream_id` into the control-frame
    // output queue. Connection-specific headers illegal in HTTP/2 are dropped.
    void submit_headers(std::uint32_t stream_id, int status, const Headers &headers, bool end_stream) {
        // Never emit a header block on a stream that is gone (reset by the peer)
        // or already ended: RFC 9113 §5.1 makes a frame on a closed stream a
        // connection-level protocol error, so the peer answers with GOAWAY and
        // every other stream on the connection dies with it. DEFENCE IN DEPTH:
        // writers check first, this guards any other caller.
        if (!stream_writable(stream_id))
            return;
        // Reuse one serialization buffer across responses: submit_headers() is
        // synchronous on the connection executor, so no other producer can be
        // using it. `append_field_block` copies the finished block into m_out
        // before this returns.
        std::string &block = m_response_block;
        block.clear();
        codec::hpack_append_status(block, status);
        for (const auto &[name, value] : headers) {
            if (is_connection_specific_field(name))
                continue; // hop-by-hop headers are forbidden in HTTP/2 (RFC 7540 §8.1.2.2)
            codec::hpack_append_literal(block, name, value);
        }
        // Alt-Svc: how a browser learns this origin also speaks HTTP/3
        // (RFC 7838). Sent on every response rather than once — it is a cache
        // entry with an expiry, and a client arriving on a fresh profile has
        // seen none of the earlier ones. HTTP/3 sends it as well, renewing the
        // entry's expiry for a client that already knew how to get here.
        if (status >= 200) {
            if (const std::string alt_svc = m_limits.alt_svc_value(); !alt_svc.empty()) {
                codec::hpack_append_literal(block, "alt-svc", alt_svc);
            }
        }
        append_field_block(stream_id, block, end_stream);

        if (end_stream) {
            // The HEADERS block itself ends the stream (a response to HEAD, or a
            // 204/304 via send_bodyless). Record that here: the framing loop only
            // sets the flag when it emits a terminating DATA frame, so without
            // this the entry never satisfied maybe_complete_stream() and leaked
            // for the life of the connection - and the stream kept reporting
            // itself writable, so a later write would emit frames after
            // END_STREAM (the same RFC 9113 §5.1 violation as above).
            auto it = m_streams.find(stream_id);
            if (it != m_streams.end()) {
                it->second.end_stream_sent = true;
                maybe_complete_stream(stream_id);
            }
        }
    }

    // Serialize an already-compressed field block as one HEADERS frame (plus
    // CONTINUATION frames when it exceeds the peer's advertised
    // SETTINGS_MAX_FRAME_SIZE, RFC 9113 §6.2/§6.10), setting END_STREAM on the
    // first frame when `end_stream`. Only the peer's value bounds what we send;
    // m_limits.h2_max_frame_size is what *we* accept.
    void append_field_block(std::uint32_t stream_id, std::string_view block, bool end_stream) {
        const std::size_t limit = std::max<std::size_t>(1, m_peer_max_frame_size);
        std::size_t off = 0;
        bool first = true;
        do {
            const std::size_t take = std::min(limit, block.size() - off);
            const bool last = (off + take == block.size());
            std::uint8_t flags = last ? codec::H2_FLAG_END_HEADERS : 0;
            if (first) {
                if (end_stream)
                    flags |= codec::H2_FLAG_END_STREAM;
                append_frame(codec::H2FrameType::Headers, flags, stream_id, block.substr(off, take));
            } else {
                append_frame(codec::H2FrameType::Continuation, flags, stream_id, block.substr(off, take));
            }
            first = false;
            off += take;
        } while (off < block.size());
    }

    // Queue a trailer section for `stream_id`: stored compressed and emitted as a
    // trailing HEADERS frame with END_STREAM once the body queue drains (RFC 9113
    // §8.1). Marks the response body finished, since a trailer section ends it.
    void submit_trailers(std::uint32_t stream_id, const Headers &trailers) {
        if (!stream_writable(stream_id))
            return;
        std::string block;
        for (const auto &[name, value] : trailers) {
            if (is_forbidden_trailer_field(name))
                continue;
            codec::hpack_append_literal(block, name, value);
        }
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end())
            return;
        it->second.out_trailers = std::move(block);
        it->second.out_trailers_set = true;
        it->second.out_finished = true;
        flush();
    }

    // Enqueue a response body chunk for `stream_id`. `last` marks end-of-body so
    // the write loop can emit the terminating END_STREAM once the queue drains.
    // The chunk is moved into the stream's queue (no copy) and the write loop is
    // nudged; actual framing/flow-control happens there.
    void enqueue_body(std::uint32_t stream_id, std::string data, bool last) {
        auto it = m_streams.find(stream_id);
        // Stream gone (reset/closed) or already ended: queueing would only be
        // dropped by the framing loop, so refuse it here as well.
        if (it == m_streams.end() || it->second.end_stream_sent)
            return;
        Stream &st = it->second;
        if (!data.empty()) {
            st.out_queued += data.size();
            st.out_queue.push_back(std::move(data));
        }
        if (last)
            st.out_finished = true;
        flush();
    }

    // Whether this stream still accepts response frames. False once the peer has
    // reset it (RST_STREAM -> erased from the table) or we have already emitted
    // END_STREAM. Writing after either is a connection-level protocol error that
    // costs the peer's whole connection (RFC 9113 §5.1), and browsers reset
    // streams constantly while a page loads - cancelled preloads, navigation,
    // superseded fetches - so this must be checked, not assumed.
    bool stream_writable(std::uint32_t stream_id) const {
        auto it = m_streams.find(stream_id);
        return it != m_streams.end() && !it->second.end_stream_sent;
    }

    // Parks the calling producer until this stream's outbound queue has drained
    // below the low watermark, so a handler that outruns the peer's flow-control
    // window cannot queue an unbounded response body. Returns an error once the
    // stream is gone (reset, completed, connection closed): the caller stops
    // rather than waiting forever.
    //
    // Callers hop onto the connection executor first, so the check-then-wait
    // sequence below cannot race the write loop (single-threaded model A). The
    // channel is deliberately the lock-free `channel`, not `concurrent_channel`:
    // every producer (the write loop's low-watermark refill) and consumer (this
    // wait) runs on the connection executor, so the thread-safe variant would
    // pay a mutex per window update for a guarantee nothing here uses.
    void await_out_space(std::uint32_t stream_id, WriteCompletion done) {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end()) {
            std::move(done)(make_error_code(asio::error::operation_aborted));
            return;
        }
        if (it->second.out_queued <= kOutHighWatermark) {
            std::move(done)(error_code{});
            return;
        }
        if (!it->second.out_space) {
            it->second.out_space = std::make_shared<asio::experimental::channel<void(error_code)>>(m_executor, 1);
        }
        auto space = it->second.out_space;
        auto self = this->shared_from_this();
        space->async_receive([self, stream_id, space, done = std::move(done)](error_code ec) mutable {
            if (ec) {
                std::move(done)(make_error_code(asio::error::operation_aborted));
                return;
            }
            self->await_out_space(stream_id, std::move(done));
        });
    }

    // Abort a stream with the given error code (RST_STREAM).
    void reset_stream(std::uint32_t stream_id, std::uint32_t error_code_value) {
        std::string payload;
        codec::append_u32(payload, error_code_value);
        append_frame(codec::H2FrameType::RstStream, 0, stream_id, payload);
        // A handler may already be parked in body().read() (dispatch runs at
        // END_HEADERS, before the body is complete). The stream is going away
        // and nothing will feed that channel again, so fail it rather than leave
        // the handler waiting forever — which would also keep the engine (held
        // by the handler's shared_from_this()) and the whole connection alive.
        // fail() is a no-op once the body terminated normally, so this cannot
        // rewrite a completed upload into an error.
        if (auto it = m_streams.find(stream_id); it != m_streams.end()) {
            if (it->second.request) {
                (void)it->second.request->body().fail(make_error_code(asio::error::connection_reset));
                // Signal the request's cancellation token too: a handler parked
                // on wait_cancelled() (or polling cancelled()) observes the reset
                // even when it is not touching the body or the response.
                it->second.request->cancel();
            }
        }
        // §5.1: frames still in flight on this stream must be minimally
        // processed and discarded, not answered.
        remember_closed_stream(stream_id, /*local_reset=*/true);
        erase_stream(stream_id);
        flush();
    }

    // ResponseSink::close() for one stream.
    //
    // Deliberately *not* reset_stream(): this response may already be complete
    // and merely queued. HTTP/1.1's sink writes to the socket inside send()
    // (`async_write`), so a handler that does send() and then
    // close() - which is exactly what the static-file stage does for its 400 and
    // 405 replies, "closing keeps the engine from draining a request body that
    // was never going to be used" - has its response on the wire before close()
    // runs. HTTP/2's sink only queues, so an immediate RST_STREAM there would
    // discard the response and answer the peer with a cancelled stream whose
    // headers it already received (measured: a 405 with an empty body and
    // StreamReset(CANCEL)). So:
    //   * response already ended          -> nothing left to close;
    //   * body complete but not yet framed -> deliver it, then reset (the client
    //     still needs to be told to stop sending, which is what close() is for);
    //   * anything else (mid-stream abort, or nothing sent at all) -> reset now.
    void close_stream(std::uint32_t stream_id) {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end() || it->second.end_stream_sent)
            return; // gone, or the response already carried END_STREAM
        if (it->second.out_finished) {
            it->second.reset_after_end = true;
            flush(); // make sure the write loop is running to finish it
            return;
        }
        reset_stream(stream_id, codec::H2_CANCEL);
    }

  private:
    // Recently closed peer-initiated streams. RFC 9113 §5.1 answers a frame on
    // a closed stream by how the stream closed: one we reset must be minimally
    // processed and discarded, while one the peer closed may be treated as a
    // STREAM_CLOSED connection error. An id that is neither open nor remembered
    // was never opened (its id was skipped), which §5.1.1 makes a connection
    // PROTOCOL_ERROR. Client stream ids are never reused, so a fixed ring of
    // recent ids is enough; an id that ages out is treated like one never
    // opened, which is the safe answer for a peer replaying old ids.
    struct ClosedStream {
        std::uint32_t id = 0;
        bool local_reset = false;
    };
    static constexpr std::size_t kMaxClosedStreams = 256;
    std::array<ClosedStream, kMaxClosedStreams> m_closed_streams{};
    std::size_t m_closed_cursor = 0; // ring slot to overwrite next

    // Records how a stream closed, so a later frame on its id gets the answer
    // §5.1 prescribes (discard after our RST_STREAM, STREAM_CLOSED after the
    // peer's close, PROTOCOL_ERROR when there is no record at all).
    void remember_closed_stream(std::uint32_t stream_id, bool local_reset) {
        m_closed_streams[m_closed_cursor] = ClosedStream{stream_id, local_reset};
        m_closed_cursor = (m_closed_cursor + 1) % kMaxClosedStreams;
    }

    const ClosedStream *find_closed_stream(std::uint32_t stream_id) const {
        for (const ClosedStream &entry : m_closed_streams) {
            if (entry.id == stream_id)
                return &entry;
        }
        return nullptr;
    }

    struct Stream {
        std::shared_ptr<Request> request;
        std::shared_ptr<Http2ResponseSink<Transport>> writer;
        std::string header_block; // accumulates HEADERS + CONTINUATION

        // Outbound response body: a queue of chunks the write loop drains into
        // flow-controlled DATA frames. out_offset marks how much of the front
        // chunk has already been framed (so partial sends need no substr copy).
        // A vector with a read cursor, not a deque: a default-constructed
        // std::deque allocates its block map, and every stream paid that (plus
        // paused_frames') for the life of the request. Entries left of the
        // cursor are destroyed by compaction (see kQueueCompactFloor) - without
        // it a refilled queue never empties and every chunk string ever queued
        // stayed resident until the stream ended.
        std::vector<std::string> out_queue;
        std::size_t out_head = 0;
        bool out_empty() const { return out_head >= out_queue.size(); }
        std::string &out_front() { return out_queue[out_head]; }
        std::size_t out_size() const { return out_queue.size() - out_head; }
        void out_pop_front() {
            if (++out_head >= out_queue.size()) {
                out_queue.clear();
                out_head = 0;
                return;
            }
            if (out_head >= kQueueCompactFloor && out_head * kQueueCompactDivisor >= out_queue.size()) {
                out_queue.erase(out_queue.begin(), out_queue.begin() + static_cast<std::ptrdiff_t>(out_head));
                out_head = 0;
            }
        }
        std::size_t out_offset = 0;
        bool out_finished = false; // handler signalled end-of-body (send/send_last)
        // Compressed trailer field block, emitted as a trailing HEADERS frame
        // with END_STREAM once the body queue drains (RFC 9113 §8.1). When set,
        // the final DATA frame does NOT carry END_STREAM - the trailer block does.
        std::string out_trailers;
        bool out_trailers_set = false;

        // Bytes queued but not yet framed into DATA frames. Drives the producer
        // backpressure in await_out_space() (kOutHighWatermark/kOutLowWatermark).
        std::size_t out_queued = 0;
        // Wakes a producer parked on backpressure. Created lazily on the first
        // wait, closed on teardown (stream erased, connection ending) so a parked
        // producer is always released rather than stranded.
        std::shared_ptr<asio::experimental::channel<void(error_code)>> out_space;

        std::uint32_t id = 0;                        // this stream's id (for WINDOW_UPDATE etc.)
        std::int64_t send_window = kH2InitialWindow; // peer's advertised window for us (send side)
        std::int64_t recv_window = kH2InitialWindow; // our window advertised to the peer (recv side)
        std::int64_t recv_pending = 0;               // consumed bytes awaiting a batched stream WINDOW_UPDATE
        std::int64_t recv_owed_conn = 0; // delivered-but-unconsumed body bytes still owing connection credit
        bool dispatched = false;
        // Whether this stream's handler has actually started. `dispatched` is set
        // the moment the request head is accepted — so a later HEADERS is still
        // read as trailers — while `handler_started` distinguishes a started
        // handler from one queued behind the executing-handler cap (Rapid Reset).
        bool handler_started = false;
        bool half_closed_remote = false; // client sent END_STREAM
        // The handler returned before the client's side ended. Its response is
        // complete, so keep the exchange alive but drop (and credit) the rest of
        // the request body instead of buffering it for a reader that is gone.
        bool body_discarded = false;
        bool end_stream_sent = false; // our terminating DATA (END_STREAM) emitted

        // close() was called while this stream's response was already complete
        // but not yet framed. The response is delivered first (END_STREAM), and
        // the RST_STREAM follows in the write loop - see close_stream().
        bool reset_after_end = false;

        // The request's declared body length (content-length), or -1 when it
        // declared none, and the body octets seen so far. A body that does not
        // match the declared length is malformed (§8.1.2.6).
        std::int64_t declared_content_length = -1;
        std::int64_t body_received = 0;

        // Inbound backpressure: DATA frames the body channel refused. Held here
        // (rather than dropped) until the handler consumes enough to make room;
        // see on_data() and on_body_consumed(). Per-stream on purpose: a backed-up
        // stream must not stop the connection from parsing frames for its
        // siblings. The connection receive window - not replenished for these
        // bytes until they are consumed - is what bounds the total across streams.
        std::vector<std::string> paused_frames;
        std::size_t paused_head = 0;
        bool paused_empty() const { return paused_head >= paused_frames.size(); }
        std::string &paused_front() { return paused_frames[paused_head]; }
        void paused_pop_front() {
            if (++paused_head >= paused_frames.size()) {
                paused_frames.clear();
                paused_head = 0;
                return;
            }
            // Same reason as out_pop_front(): drop the consumed prefix instead
            // of pinning it until the whole queue happens to drain.
            if (paused_head >= kQueueCompactFloor && paused_head * kQueueCompactDivisor >= paused_frames.size()) {
                paused_frames.erase(paused_frames.begin(),
                                    paused_frames.begin() + static_cast<std::ptrdiff_t>(paused_head));
                paused_head = 0;
            }
        }
        // END_STREAM arrived while frames were still parked. Body::read() reports
        // EOF as soon as the channel drains, so body().finish() must wait until the
        // parked queue is empty or the consumer would see a truncated body; see
        // on_body_consumed().
        bool finish_pending = false;

        // --- HTTP/2 WebSocket (RFC 8441) ---
        bool websocket = false;
        std::string protocol;   // raw `:protocol` value
        std::string ws_inbox;   // inbound tunnel bytes, ordered
        bool ws_eof = false;    // the peer ended its half (END_STREAM)
        bool ws_closed = false; // we tore the tunnel down
        std::shared_ptr<asio::experimental::channel<void(error_code)>> ws_wake;

        bool out_drained() const { return out_finished && out_empty(); }
    };

    // --- byte helpers ---

    // Serialize a complete frame (9-octet header + payload) into the output
    // buffer, splitting nothing (payload must already be within max frame size;
    // HEADERS blocks that big are not expected for typical responses — a future
    // enhancement can add CONTINUATION here).
    void append_frame(codec::H2FrameType type, std::uint8_t flags, std::uint32_t stream_id, std::string_view payload) {
        codec::serialize_frame_header(m_out, static_cast<std::uint32_t>(payload.size()),
                                      static_cast<std::uint8_t>(type), flags, stream_id);
        m_out.append(payload);
    }

    // --- SETTINGS ---

    void queue_settings() {
        std::string payload;
        auto add = [&](std::uint16_t id, std::uint32_t value) {
            payload.push_back(static_cast<char>((id >> 8) & 0xFF));
            payload.push_back(static_cast<char>(id & 0xFF));
            codec::append_u32(payload, value);
        };
        add(codec::H2_SETTINGS_MAX_CONCURRENT_STREAMS, m_limits.h2_max_concurrent_streams);
        add(codec::H2_SETTINGS_INITIAL_WINDOW_SIZE, static_cast<std::uint32_t>(m_limits.h2_initial_window));
        add(codec::H2_SETTINGS_MAX_FRAME_SIZE, m_limits.h2_max_frame_size);
        // RFC 8441: advertise extended CONNECT support so a client may open a
        // WebSocket over this connection.
        add(codec::H2_SETTINGS_ENABLE_CONNECT_PROTOCOL, m_limits.h2_enable_connect_protocol ? 1u : 0u);
        // Advertise the decoded header-list bound the decoder enforces, so a
        // conforming peer keeps under it instead of being reset.
        add(codec::H2_SETTINGS_MAX_HEADER_LIST_SIZE,
            static_cast<std::uint32_t>(m_limits.effective_max_header_list_size()));
        append_frame(codec::H2FrameType::Settings, 0, 0, payload);

        // SETTINGS_INITIAL_WINDOW_SIZE does not touch the connection window,
        // which stays at the protocol default of 65535 until an explicit
        // connection WINDOW_UPDATE raises it. Without this, a configured initial
        // window larger than 65535 leaves the peer able to send only 65535 octets
        // on the whole connection while our refill threshold (initial/2) exceeds
        // that — the connection stalls permanently on any sizable request body.
        if (m_limits.h2_initial_window > kH2InitialWindow) {
            send_window_update(0, static_cast<std::uint32_t>(m_limits.h2_initial_window - kH2InitialWindow));
        }
    }

    // Apply a SETTINGS payload received from the peer (or from the h2c upgrade
    // HTTP2-Settings header). Only the settings that affect our send path are
    // acted on; others are accepted and ignored.
    void apply_settings_payload(std::string_view payload) {
        for (std::size_t i = 0; i + 6 <= payload.size(); i += 6) {
            std::uint16_t id = static_cast<std::uint16_t>((static_cast<unsigned char>(payload[i]) << 8) |
                                                          static_cast<unsigned char>(payload[i + 1]));
            std::uint32_t value = codec::read_u32(payload, i + 2);
            if (id == codec::H2_SETTINGS_INITIAL_WINDOW_SIZE) {
                // §6.5.2: a window larger than 2^31-1 is a FLOW_CONTROL_ERROR, not
                // a value to be stored.
                if (value > static_cast<std::uint32_t>(kH2MaxWindow)) {
                    go_away(codec::H2_FLOW_CONTROL_ERROR);
                    return;
                }
                std::int64_t delta = static_cast<std::int64_t>(value) - m_peer_initial_window;
                m_peer_initial_window = static_cast<std::int32_t>(value);
                for (auto &[sid, st] : m_streams) {
                    st.send_window += delta;
                    // §6.9.1: a SETTINGS change that pushes an existing stream's
                    // window past 2^31-1 is a FLOW_CONTROL_ERROR, not a value to
                    // keep (a peer can first raise it with WINDOW_UPDATE, then
                    // raise INITIAL_WINDOW_SIZE here).
                    if (st.send_window > kH2MaxWindow) {
                        go_away(codec::H2_FLOW_CONTROL_ERROR);
                        return;
                    }
                }
            } else if (id == codec::H2_SETTINGS_ENABLE_PUSH) {
                // §6.5.2: ENABLE_PUSH is a boolean. This endpoint never pushes, so
                // the value is not acted on — but a peer sending anything but 0 or
                // 1 is malformed and must be sent away.
                if (value > 1u) {
                    go_away(codec::H2_PROTOCOL_ERROR);
                    return;
                }
            } else if (id == codec::H2_SETTINGS_MAX_FRAME_SIZE) {
                // RFC 7540 §6.5.2: the value must lie in [2^14, 2^24-1]; anything
                // else is a connection error. Accepting 0 here (reachable from a
                // 6-byte SETTINGS frame, or through the h2c HTTP2-Settings header)
                // would leave fill_data_frames() with a zero frame budget: it could
                // not advance, would re-loop forever and grow m_out without bound.
                if (value < 16384u || value > 16777215u) {
                    go_away(codec::H2_PROTOCOL_ERROR);
                    return;
                }
                m_peer_max_frame_size = value;
            }
        }
    }

    // Whether `id` may open a new client-initiated stream: odd, and higher than
    // any the peer has opened so far (RFC 9113 §5.1.1).
    bool is_new_client_stream(std::uint32_t id) const { return (id & 1u) == 1u && id > m_next_peer_stream_id; }

    // Whether adding `extra` bytes to a header block of `current` bytes would
    // exceed the bound we accept. Without it, HEADERS without END_HEADERS plus
    // endless CONTINUATION accumulate without limit (RFC 9113 §10.5.1).
    bool header_block_too_big(std::size_t current, std::size_t extra) const {
        return current + extra > m_limits.max_header_bytes;
    }

    // Whether the request on `stream_id` was a HEAD (its response carries no
    // body).
    bool method_is_head(std::uint32_t stream_id) const {
        auto it = m_streams.find(stream_id);
        return it != m_streams.end() && it->second.request->method() == Method::Head;
    }

    // --- stream table ---

    Stream &ensure_stream(std::uint32_t stream_id) {
        auto [it, inserted] = m_streams.try_emplace(stream_id);
        if (inserted) {
            Stream &st = it->second;
            st.id = stream_id;
            st.request = std::make_shared<Request>(Version::Http2, m_executor, m_transport->peer());
            st.writer = std::make_shared<Http2ResponseSink<Transport>>(this->weak_from_this(), stream_id, m_executor);
            st.send_window = m_peer_initial_window;
            st.recv_window = m_limits.h2_initial_window; // our advertised per-stream window

            // Consumption-based flow control: when the handler reads body bytes,
            // hop back onto the connection executor and replenish credit for
            // exactly that many bytes. Un-consumed data therefore keeps the
            // window closed, bounding in-flight memory (no unbounded backlog).
            std::weak_ptr<Http2Engine> weak = this->weak_from_this();
            auto exec = m_executor;
            // The common case is a handler reading its body on the connection
            // executor, where on_body_consumed() may run inline. Posting for
            // every body chunk cost an executor operation (and its allocation)
            // per request. A reader on any other thread still hops, which is
            // what keeps this correct for a Body handed to another executor.
            const std::thread::id connection_thread = m_thread_id;
            st.request->body().set_on_consumed([weak, exec, stream_id, connection_thread](std::size_t n) {
                if (std::this_thread::get_id() == connection_thread) {
                    if (auto eng = weak.lock())
                        eng->on_body_consumed(stream_id, n);
                    return;
                }
                asio::post(exec, [weak, stream_id, n]() {
                    if (auto eng = weak.lock())
                        eng->on_body_consumed(stream_id, n);
                });
            });
        }
        return it->second;
    }

    // Consumes the 24-octet client connection preface once, at the very start of
    // the byte stream (RFC 7540 §3.5). Returns false only if enough bytes are
    // present to decide and they are NOT the preface (a protocol error); returns
    // true when the preface was stripped, or when there are not yet enough bytes
    // to tell (caller waits for more).
    bool consume_client_preface() {
        if (m_preface_consumed)
            return true;
        if (m_recv_buf.size() < codec::kH2ClientPreface.size())
            return true; // need more bytes
        if (std::string_view{m_recv_buf}.substr(0, codec::kH2ClientPreface.size()) != codec::kH2ClientPreface) {
            go_away(codec::H2_PROTOCOL_ERROR);
            return false;
        }
        m_recv_buf.erase(0, codec::kH2ClientPreface.size());
        m_preface_consumed = true;
        return true;
    }

    // --- serve loops ---

    void serve_loops() {
        m_deadline = std::chrono::steady_clock::now() + m_limits.idle_timeout;
        m_alive = true;
        flush();
        if (m_limits.idle_timeout.count() > 0) {
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
                self->m_transport->close();
                return;
            }
            self->arm_watchdog();
        });
    }

    void read_loop() {
        if (m_finished)
            return;
        if (!parse_available()) {
            finish_connection();
            return;
        }
        auto self = this->shared_from_this();
        m_transport->async_read_some(
            ByteSpan{m_read_scratch.data(), m_read_scratch.size()}, [self](error_code ec, std::size_t n) {
                if (ec) {
                    self->finish_connection();
                    return;
                }
                self->m_deadline = std::chrono::steady_clock::now() + self->m_limits.idle_timeout;
                self->m_recv_buf.append(reinterpret_cast<const char *>(self->m_read_scratch.data()), n);
                if (!self->parse_available()) {
                    self->finish_connection();
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
            m_deadline = std::chrono::steady_clock::now() + m_limits.idle_timeout;
            auto self = this->shared_from_this();
            m_write_in_flight = true;
            m_transport->async_write(ConstByteSpan{reinterpret_cast<const std::byte *>(chunk->data()), chunk->size()},
                                     [self, chunk](error_code ec, std::size_t) {
                                         self->m_write_in_flight = false;
                                         if (self->m_finished) {
                                             self->finish_connection();
                                             return;
                                         }
                                         if (ec) {
                                             self->finish_connection();
                                             return;
                                         }
                                         self->write_loop();
                                     });
            return;
        }
        if (m_streams.empty()) {
            std::string().swap(m_out);
        }
        if (m_goaway_sent && streams_all_done()) {
            finish_connection();
            return;
        }
        auto self = this->shared_from_this();
        m_notify.async_receive([self](error_code ec) {
            if (ec) {
                self->finish_connection();
                return;
            }
            self->write_loop();
        });
    }

    void finish_connection() {
        if (!m_finished) {
            m_finished = true;
            m_alive = false;
            m_watchdog->cancel();
            m_notify.close();
            for (auto &entry : m_streams) {
                if (entry.second.request) {
                    (void)entry.second.request->body().fail(make_error_code(asio::error::connection_reset));
                    entry.second.request->cancel();
                }
                if (entry.second.out_space)
                    entry.second.out_space->close();
                entry.second.ws_closed = true;
                if (entry.second.ws_wake)
                    entry.second.ws_wake->close();
            }
        }
        // An in-flight write owns the transport; waiting for it avoids two
        // concurrent async_write operations corrupting the byte stream. Its
        // completion calls back here.
        if (m_write_in_flight)
            return;
        if (m_finish_started)
            return;
        m_finish_started = true;
        if (m_goaway_sent) {
            // Keep whatever is already queued. The RST_STREAM that refused the
            // offending stream is still meaningful to the peer, and the write
            // loop may not have flushed it before the read loop saw the error.
            // The GOAWAY follows it; whole frames only, so the stream stays
            // parseable.
            std::string payload;
            codec::append_u32(payload, m_next_peer_stream_id);
            codec::append_u32(payload, m_goaway_error);
            append_frame(codec::H2FrameType::Goaway, 0, 0, payload);
        }
        auto self = this->shared_from_this();
        auto shutdown = [self]() {
            self->m_transport->async_shutdown([self](error_code) {
                self->m_transport->close();
                self->complete_on_done();
            });
        };
        if (m_out.empty()) {
            shutdown();
            return;
        }
        auto chunk = std::make_shared<std::string>(std::move(m_out));
        m_out.clear();
        m_write_in_flight = true;
        m_transport->async_write(
            ConstByteSpan{reinterpret_cast<const std::byte *>(chunk->data()), chunk->size()},
            asio::cancel_after(std::chrono::seconds(2), [self, chunk, shutdown](error_code, std::size_t) {
                self->m_write_in_flight = false;
                shutdown();
            }));
    }

    void complete_on_done() {
        if (m_done_called)
            return;
        m_done_called = true;
        auto on_done = std::move(m_on_done);
        if (on_done) {
            on_done();
        }
    }

    bool streams_all_done() const {
        for (const auto &[sid, st] : m_streams) {
            if (!st.end_stream_sent)
                return false;
        }
        return true;
    }

    // --- frame parsing ---

    // Parses every complete frame currently buffered in m_recv_buf. Returns
    // false on a connection-fatal protocol error (a GOAWAY has been queued);
    // true if it consumed all it could and wants more bytes.
    bool parse_available() {
        if (!consume_client_preface())
            return false; // fatal: not the h2 preface
        if (!m_preface_consumed)
            return true; // still waiting for the full preface
        // Backed-up body data does not stop parsing: a stream whose Body channel
        // is full parks its DATA frames (see on_data) and the loop keeps going, so
        // frames for its siblings are still handled. The bound is the connection
        // receive window, which is not replenished for parked bytes until they are
        // consumed; on_body_consumed() hands the parked frames back as room frees.
        std::size_t pos = 0;
        while (m_recv_buf.size() - pos >= codec::kH2FrameHeaderSize) {
            codec::H2FrameHeader hdr;
            codec::parse_frame_header(std::string_view{m_recv_buf}.substr(pos), hdr);
            // RFC 9113 §4.2: a frame larger than our advertised
            // SETTINGS_MAX_FRAME_SIZE is a connection error, for every frame type (a
            // compliant peer never sends one). Bounding it here also bounds how much
            // a single frame can buffer.
            if (hdr.length > m_limits.h2_max_frame_size) {
                go_away(codec::H2_FRAME_SIZE_ERROR);
                return false;
            }
            std::size_t frame_end = pos + codec::kH2FrameHeaderSize + hdr.length;
            if (frame_end > m_recv_buf.size())
                break; // wait for the rest of this frame

            std::string_view payload = std::string_view{m_recv_buf}.substr(pos + codec::kH2FrameHeaderSize, hdr.length);
            if (!handle_frame(hdr, payload)) {
                return false; // fatal; GOAWAY already queued by handler
            }
            pos = frame_end;
        }
        if (pos > 0)
            m_recv_buf.erase(0, pos);
        return true;
    }

    bool handle_frame(const codec::H2FrameHeader &hdr, std::string_view payload) {
        const auto type = static_cast<codec::H2FrameType>(hdr.type);

        // RFC 9113 §6.10: between a HEADERS without END_HEADERS and its closing
        // CONTINUATION, nothing may be interleaved on that stream. Only the frame
        // *type* is judged here; on_continuation() checks the stream it names.
        if (m_continuation_stream != 0 && type != codec::H2FrameType::Continuation) {
            SIMPLE_HTTP_ERROR_LOG("h2: frame type {} interleaved in a header block (stream={})", hdr.type,
                                  m_continuation_stream);
            go_away(codec::H2_PROTOCOL_ERROR);
            return false;
        }

        switch (type) {
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
            return on_rst_stream(hdr);
        case codec::H2FrameType::Ping:
            return on_ping(hdr, payload);
        case codec::H2FrameType::Goaway:
            // §6.8: the stream identifier of a GOAWAY is reserved and must
            // be zero.
            if (hdr.stream_id != 0) {
                go_away(codec::H2_PROTOCOL_ERROR);
                return false;
            }
            // §6.8: a GOAWAY shorter than the two 32-bit fields is malformed.
            if (hdr.length < 8) {
                go_away(codec::H2_FRAME_SIZE_ERROR);
                return false;
            }
            return true; // peer is going away; let the read EOF close us
        case codec::H2FrameType::Priority:
            return on_priority(hdr, payload);
        case codec::H2FrameType::PushPromise:
            // §8.2: only clients receive pushes. A server that is sent one is
            // talking to a peer that has the direction wrong.
            go_away(codec::H2_PROTOCOL_ERROR);
            return false;
        default:
            // §5.5: unknown frame types must be ignored. This branch now
            // means exactly that, and nothing else.
            return true;
        }
    }

    // RFC 9113 §6.3: the priority scheme is advisory, so the frame may be
    // ignored outright — but its framing is still checked, and §5.3.1 makes a
    // stream that depends on itself a stream error.
    bool on_priority(const codec::H2FrameHeader &hdr, std::string_view payload) {
        if (hdr.stream_id == 0) {
            go_away(codec::H2_PROTOCOL_ERROR);
            return false;
        }
        if (hdr.length != 5) {
            go_away(codec::H2_FRAME_SIZE_ERROR);
            return false;
        }
        const std::uint32_t dependency = codec::read_u32(payload, 0) & 0x7FFFFFFFu;
        if (dependency == hdr.stream_id) {
            if (m_streams.find(hdr.stream_id) != m_streams.end()) {
                reset_stream(hdr.stream_id, codec::H2_PROTOCOL_ERROR);
                return true;
            }
            if (hdr.stream_id > m_next_peer_stream_id) {
                // An idle stream: RST_STREAM there is itself a protocol
                // violation (§5.4.2), so the self-dependency is a connection
                // error instead.
                go_away(codec::H2_PROTOCOL_ERROR);
                return false;
            }
            return true; // already closed: there is nothing left to reset
        }
        return true; // nothing to reorder: this engine sends one stream at a time
    }

    // Strips optional padding (RFC 7540 §6.1/§6.2) from a DATA/HEADERS payload,
    // returning the field-block/data slice. `has_priority` handles the 5-octet
    // priority prefix on HEADERS.
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
            off += 5; // 4-octet stream dependency + 1-octet weight
        }
        if (off + pad_len > payload.size()) {
            ok = false;
            return {};
        }
        return payload.substr(off, payload.size() - off - pad_len);
    }

    // Refuses a stream while still recording it as seen: its id must advance
    // m_next_peer_stream_id, otherwise a DATA/RST_STREAM/WINDOW_UPDATE that was
    // already in flight on it is classified as "idle stream" and turns into a
    // connection error (RFC 9113 §5.1). If the refused HEADERS did not carry
    // END_HEADERS, the peer's CONTINUATION frames still have to be consumed;
    // they are discarded (bounded by max_header_bytes) rather than treated as a
    // header block for a stream that no longer exists.
    // Every complete field block must pass through the connection's HPACK
    // decoder, even when its stream is refused or already closed: the peer's
    // encoder updates its dynamic table when it emits the block, so skipping
    // the decode leaves our table out of sync and poisons every later block on
    // the connection (RFC 9113 §4.3.1, RFC 7541 §2.2). The decoded fields are
    // discarded; only the table side effects matter.
    bool decode_discarded_header_block(std::string_view block) {
        std::vector<codec::HpackHeader> ignored;
        if (!m_decoder.decode(block, ignored) && !m_decoder.header_list_too_large()) {
            go_away(codec::H2_COMPRESSION_ERROR);
            return false;
        }
        return true; // list-too-large still decoded to the end: table stays in sync
    }

    // Starts consuming a discarded block whose END_HEADERS has not arrived yet.
    void begin_discarding_header_block(const codec::H2FrameHeader &hdr, std::string_view block) {
        m_continuation_stream = hdr.stream_id;
        m_continuation_discard = true;
        m_continuation_discard_bytes = block.size();
        m_continuation_discard_block.assign(block);
    }

    bool refuse_stream(const codec::H2FrameHeader &hdr, std::string_view block, std::uint32_t error_code_value) {
        if (hdr.stream_id > m_next_peer_stream_id)
            m_next_peer_stream_id = hdr.stream_id;
        if (hdr.has_flag(codec::H2_FLAG_END_HEADERS)) {
            if (!decode_discarded_header_block(block))
                return false;
        } else {
            begin_discarding_header_block(hdr, block);
        }
        reset_stream(hdr.stream_id, error_code_value);
        return true;
    }

    bool on_headers(const codec::H2FrameHeader &hdr, std::string_view payload) {
        if (hdr.stream_id == 0) {
            go_away(codec::H2_PROTOCOL_ERROR);
            return false;
        }
        // RFC 9113 §5.1.1: a client opens odd-numbered streams, each higher than
        // every stream it has opened before. An even (server-initiated) id is a
        // connection error. An odd id at or below the highest opened one is one
        // of three things (§5.1):
        //   * never opened (the id was skipped, so it is implicitly closed):
        //     an unexpected stream identifier, a PROTOCOL_ERROR connection error;
        //   * reset by us: later frames must be minimally processed and discarded
        //     (the peer may have sent them before it saw our RST_STREAM);
        //   * closed by the peer (RST_STREAM or END_STREAM): a STREAM_CLOSED
        //     connection error.
        bool discard_closed = false;
        if (m_streams.find(hdr.stream_id) == m_streams.end() && !is_new_client_stream(hdr.stream_id)) {
            if ((hdr.stream_id & 1u) != 1u) {
                SIMPLE_HTTP_ERROR_LOG("h2: client opened stream {} (highest so far {}); GOAWAY", hdr.stream_id,
                                      m_next_peer_stream_id);
                go_away(codec::H2_PROTOCOL_ERROR);
                return false;
            }
            const ClosedStream *closed = find_closed_stream(hdr.stream_id);
            if (closed == nullptr) {
                SIMPLE_HTTP_ERROR_LOG("h2: HEADERS on never-opened stream {} (highest so far {}); GOAWAY",
                                      hdr.stream_id, m_next_peer_stream_id);
                go_away(codec::H2_PROTOCOL_ERROR);
                return false;
            }
            if (!closed->local_reset) {
                SIMPLE_HTTP_ERROR_LOG("h2: HEADERS on closed stream {} (highest so far {}); GOAWAY", hdr.stream_id,
                                      m_next_peer_stream_id);
                go_away(codec::H2_STREAM_CLOSED);
                return false;
            }
            discard_closed = true;
        }
        bool ok = false;
        std::string_view block =
            strip_padding(payload, hdr.has_flag(codec::H2_FLAG_PADDED), hdr.has_flag(codec::H2_FLAG_PRIORITY), ok);
        if (!ok) {
            go_away(codec::H2_PROTOCOL_ERROR);
            return false;
        }
        if (discard_closed) {
            // §5.1: after sending RST_STREAM, frames on that stream are minimally
            // processed and then discarded, never answered with another error
            // (and never with RST_STREAM: no non-PRIORITY frame may be sent on a
            // closed stream). The field block still has to be decoded (and
            // discarded) or the connection's HPACK dynamic table diverges from
            // the peer's.
            if (hdr.has_flag(codec::H2_FLAG_END_HEADERS))
                return decode_discarded_header_block(block);
            begin_discarding_header_block(hdr, block);
            return true;
        }

        // §5.3.1: a stream cannot depend on itself. The priority prefix was just
        // skipped over, so the dependency it held is re-read here for the check.
        if (hdr.has_flag(codec::H2_FLAG_PRIORITY)) {
            const std::size_t off = hdr.has_flag(codec::H2_FLAG_PADDED) ? 1 : 0;
            if ((codec::read_u32(payload, off) & 0x7FFFFFFFu) == hdr.stream_id) {
                return refuse_stream(hdr, block, codec::H2_PROTOCOL_ERROR);
            }
        }

        // Refuse streams beyond the concurrency limit we advertise (RFC 9113
        // §5.1.2): each accepted stream allocates a Request (with its Body
        // channel), a ResponseSink and a handler coroutine, so an unbounded
        // stream count is an unbounded resource commitment.
        if (m_streams.find(hdr.stream_id) == m_streams.end() &&
            m_streams.size() >= m_limits.h2_max_concurrent_streams) {
            SIMPLE_HTTP_ERROR_LOG("h2: refusing stream {} ({} open, limit {})", hdr.stream_id, m_streams.size(),
                                  m_limits.h2_max_concurrent_streams);
            return refuse_stream(hdr, block, codec::H2_REFUSED_STREAM);
        }
        if (header_block_too_big(
                m_streams.find(hdr.stream_id) != m_streams.end() ? m_streams.at(hdr.stream_id).header_block.size() : 0,
                block.size())) {
            SIMPLE_HTTP_ERROR_LOG("h2: header block exceeds {} bytes (stream={})", m_limits.max_header_bytes,
                                  hdr.stream_id);
            go_away(codec::H2_ENHANCE_YOUR_CALM);
            return false;
        }
        Stream &st = ensure_stream(hdr.stream_id);
        // RFC 9113 §5.1: once the peer has closed its half of the stream, only
        // WINDOW_UPDATE, PRIORITY and RST_STREAM are still in order there — a
        // HEADERS is a STREAM_CLOSED stream error. Checked before the trailing
        // -block rule below so the error code follows the stream state rather
        // than the frame's flags.
        if (st.half_closed_remote) {
            SIMPLE_HTTP_ERROR_LOG("h2: HEADERS on a half-closed(remote) stream (stream={}); resetting", hdr.stream_id);
            // Even an illegal HEADERS carries a field block: decode it before
            // the stream error, or the connection's HPACK table diverges.
            if (hdr.has_flag(codec::H2_FLAG_END_HEADERS)) {
                if (!decode_discarded_header_block(block))
                    return false;
            } else {
                begin_discarding_header_block(hdr, block);
            }
            reset_stream(hdr.stream_id, codec::H2_STREAM_CLOSED);
            return true;
        }
        // If END_STREAM is set, the client sends no request body.
        bool end_stream = hdr.has_flag(codec::H2_FLAG_END_STREAM);

        // A whole header block in one frame (the overwhelmingly common shape) is
        // decoded straight out of the receive buffer. Appending it to
        // st.header_block first copied every request head and allocated a new
        // buffer per request; the accumulated form is only needed when
        // CONTINUATION frames follow.
        const bool single_frame_block = hdr.has_flag(codec::H2_FLAG_END_HEADERS) && st.header_block.empty();
        std::string_view decoded_block = block;
        if (!single_frame_block) {
            st.header_block.append(block);
            decoded_block = st.header_block;
        }
        if (hdr.stream_id > m_next_peer_stream_id)
            m_next_peer_stream_id = hdr.stream_id;

        if (hdr.has_flag(codec::H2_FLAG_END_HEADERS)) {
            // A trailing header block completes the request, so it must also end
            // the stream (§8.1): a second HEADERS without END_STREAM describes a
            // message that never terminates. `dispatched` is read before
            // finish_header_block, which may erase `st`.
            if (st.dispatched && !hdr.has_flag(codec::H2_FLAG_END_STREAM)) {
                SIMPLE_HTTP_ERROR_LOG("h2: trailing HEADERS without END_STREAM (stream={}); resetting", hdr.stream_id);
                reset_stream(hdr.stream_id, codec::H2_PROTOCOL_ERROR);
                return true;
            }
            if (!finish_header_block(hdr.stream_id, st, decoded_block))
                return false;
            // finish_header_block may have reset (and erased) a malformed stream, so
            // `st` must not be used again: re-look it up before dispatching.
            auto it = m_streams.find(hdr.stream_id);
            if (it == m_streams.end())
                return true;
            if (end_stream) {
                it->second.half_closed_remote = true;
                mark_ws_remote_end(it->second);
                (void)it->second.request->body().finish();
            }
            // Dispatch as soon as the request head is complete. The handler runs
            // concurrently with any DATA frames still arriving, reading the body
            // through the async Body channel (see on_data). A body-less request
            // simply sees an immediately-finished body.
            return dispatch_stream(hdr.stream_id);
        } else {
            m_continuation_stream = hdr.stream_id; // CONTINUATION frames follow
            if (end_stream)
                st.half_closed_remote = true; // remember for on_continuation
        }
        return true;
    }

    bool on_continuation(const codec::H2FrameHeader &hdr, std::string_view payload) {
        if (hdr.stream_id == 0 || hdr.stream_id != m_continuation_stream) {
            go_away(codec::H2_PROTOCOL_ERROR);
            return false;
        }
        if (m_continuation_discard) {
            // The stream was refused at its HEADERS: the rest of the split head
            // still has to reach the HPACK decoder to keep the dynamic table in
            // sync, but its fields are discarded. The byte bound keeps a peer
            // from turning the discard path into a CONTINUATION flood.
            const bool too_big = payload.size() > m_limits.max_header_bytes ||
                                 m_continuation_discard_bytes > m_limits.max_header_bytes - payload.size();
            if (too_big) {
                SIMPLE_HTTP_ERROR_LOG("h2: refused header block exceeds {} bytes (stream={})",
                                      m_limits.max_header_bytes, hdr.stream_id);
                go_away(codec::H2_ENHANCE_YOUR_CALM);
                return false;
            }
            m_continuation_discard_bytes += payload.size();
            m_continuation_discard_block.append(payload);
            if (hdr.has_flag(codec::H2_FLAG_END_HEADERS)) {
                const bool decoded = decode_discarded_header_block(m_continuation_discard_block);
                m_continuation_stream = 0;
                m_continuation_discard = false;
                m_continuation_discard_bytes = 0;
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
        if (header_block_too_big(it->second.header_block.size(), payload.size())) {
            SIMPLE_HTTP_ERROR_LOG("h2: CONTINUATION header block exceeds {} bytes (stream={})",
                                  m_limits.max_header_bytes, hdr.stream_id);
            go_away(codec::H2_ENHANCE_YOUR_CALM); // RFC 9113 §10.5.1 (CONTINUATION
                                                  // flood)
            return false;
        }
        it->second.header_block.append(payload);
        if (hdr.has_flag(codec::H2_FLAG_END_HEADERS)) {
            m_continuation_stream = 0;
            if (!finish_header_block(hdr.stream_id, it->second, it->second.header_block))
                return false;
            auto it2 = m_streams.find(hdr.stream_id); // may have been reset+erased
            if (it2 == m_streams.end())
                return true;
            if (it2->second.half_closed_remote) {
                mark_ws_remote_end(it2->second);
                (void)it2->second.request->body().finish();
            }
            return dispatch_stream(hdr.stream_id);
        }
        return true;
    }

    // Decodes an accumulated header block into the stream's Request and clears
    // the buffer. Dispatch happens once END_STREAM is also seen.
    // RFC 9113 §8.2.1: HTTP/2 field names are lowercase. An uppercase byte makes
    // the request malformed rather than something to fold away — folding is how
    // HTTP/1.1 tolerated it, and this layer deliberately does not.
    static bool has_upper_ascii(std::string_view s) noexcept {
        for (char c : s) {
            if (ascii_lower(c) != c)
                return true;
        }
        return false;
    }

    // RFC 9113 §8.3.1: :path is origin-form ("/" ...) or "*" for OPTIONS. It
    // never carries a fragment, and a raw space would split an HTTP/1.1 request
    // line when the proxy replays it.
    static bool valid_h2_path(std::string_view p) noexcept {
        if (p.empty())
            return false;
        if (p == "*")
            return true;
        if (p.front() != '/')
            return false;
        return p.find_first_of(" #\t") == std::string_view::npos;
    }

    // A field value may contain VCHAR, SP, HTAB and obs-text, but no other CTL
    // and not DEL. contains_ctl() only knows CR/LF/NUL — enough to stop header
    // injection, not enough to reject a malformed field.
    static bool has_forbidden_value_byte(std::string_view s) noexcept {
        for (unsigned char c : s) {
            if (c == '\t')
                continue;
            if (c < 0x20 || c == 0x7F)
                return true;
        }
        return false;
    }

    // RFC 9113 §8.2.2: the HTTP/1.1 connection-specific fields, which a client
    // must not send in HTTP/2. A hop downstream that acted on one — say
    // `transfer-encoding: chunked` replayed through a proxy route — would be
    // framing a message the sender never described.
    // (The predicate itself is core/http_field.h's, shared with the h3 engine.)

    bool finish_header_block(std::uint32_t stream_id, Stream &st, std::string_view block) {
        // `fields` is connection-scoped scratch: clear() keeps the vector's
        // capacity (and the small-string storage of its entries) so a request
        // head no longer allocates a fresh vector every time. Decoding is not
        // re-entrant, so one buffer per engine is enough.
        auto &fields = m_decode_fields;
        fields.clear();
        if (!m_decoder.decode(block, fields)) {
            if (m_decoder.header_list_too_large()) {
                // The decoded list exceeded the bound advertised as
                // SETTINGS_MAX_HEADER_LIST_SIZE. RFC 9113 §6.5.2 makes that a
                // *stream* error (the connection and every other stream survive),
                // not the compression error a malformed block is.
                SIMPLE_HTTP_ERROR_LOG("h2: decoded header list exceeds {} bytes (stream={}); resetting",
                                      m_limits.effective_max_header_list_size(), stream_id);
                reset_stream(stream_id, codec::H2_ENHANCE_YOUR_CALM);
                return true;
            }
            SIMPLE_HTTP_ERROR_LOG("h2 HPACK decode failed (stream={}, err={})", stream_id, m_decoder.last_error());
            go_away(codec::H2_COMPRESSION_ERROR);
            return false;
        }
        st.header_block.clear();

        // A header block on a stream that has already been dispatched is a
        // trailing block (§8.1), and a trailing block carries no pseudo-header.
        const bool trailers = st.dispatched;

        // :authority is collected here and applied after the loop, so a real
        // Host field — which may follow it — wins over the synthesized one.
        std::string authority;
        // Presence flags, so a missing or duplicated request pseudo-header is a
        // malformed request rather than a silently-synthesized default (§8.3.1).
        bool seen_method = false;
        bool seen_scheme = false;
        bool seen_path = false;
        bool seen_authority = false;
        bool seen_regular_field = false;
        // RFC 8441 extended CONNECT selector, present only when `:protocol` was.
        bool seen_protocol = false;

        // The decoder already knows how many fields the block held. Reserving
        // once keeps the request's header vector from growing 1 -> 2 -> 4 -> 8
        // (each growth reallocating and moving every stored string) per request.
        st.request->mutable_headers().reserve(fields.size());

        // Every rejection below is a *stream* error (§8.1.1): the connection stays
        // usable and only the offending stream is reset. Returning true then means
        // "handled" — the caller must not dispatch a stream that is already gone.
        auto malformed = [&]([[maybe_unused]] std::string why) {
            SIMPLE_HTTP_ERROR_LOG("h2 malformed request (stream={}): {}; resetting stream", stream_id, why);
            reset_stream(stream_id, codec::H2_PROTOCOL_ERROR);
        };

        for (auto &f : fields) {
            // RFC 9113 §8.2.1: a field name or value must not contain CR, LF or NUL.
            // HTTP/2 has no line folding, so these bytes survive decoding verbatim
            // and would let a peer inject request lines/headers into whatever
            // HTTP/1.1 message is built downstream (handlers, reverse proxy). A
            // request is malformed -> stream error (§8.1.1); the connection stays
            // usable.
            if (contains_ctl(f.name) || has_forbidden_value_byte(f.value)) {
                SIMPLE_HTTP_ERROR_LOG("h2 field with CR/LF/NUL (stream={}); resetting stream", stream_id);
                reset_stream(stream_id, codec::H2_PROTOCOL_ERROR);
                return true; // stream is gone: the caller must not dispatch it
            }

            const bool pseudo = !f.name.empty() && f.name[0] == ':';
            // §8.1: trailers describe a request that has already been received, so
            // they carry no pseudo-header fields.
            if (pseudo && trailers) {
                malformed("pseudo-header in trailers");
                return true;
            }
            // §8.2.1: pseudo-header fields must appear before every regular field.
            if (pseudo && seen_regular_field) {
                malformed("pseudo-header after a regular field");
                return true;
            }

            if (pseudo) {
                bool *seen = nullptr;
                if (f.name == ":method") {
                    seen = &seen_method;
                } else if (f.name == ":scheme") {
                    seen = &seen_scheme;
                } else if (f.name == ":path") {
                    seen = &seen_path;
                } else if (f.name == ":authority") {
                    seen = &seen_authority;
                } else if (f.name == ":protocol") {
                    // RFC 8441: the extended-CONNECT protocol selector. Only
                    // meaningful together with :method = CONNECT; validated below.
                    seen = &seen_protocol;
                } else {
                    // :status and every other pseudo-header belong to a response,
                    // or to nothing at all — either way a client must not send one.
                    malformed(std::string{"unknown pseudo-header "} + f.name);
                    return true;
                }
                if (*seen) {
                    malformed(std::string{"duplicate "} + f.name);
                    return true;
                }
                *seen = true;

                if (f.name == ":method") {
                    // The method is replayed into an HTTP/1.1 request line by
                    // the reverse proxy and the WebSocket upstream handshake, so
                    // it has to re-parse as a token.
                    if (!is_http_token(f.value)) {
                        malformed("invalid :method");
                        return true;
                    }
                    st.request->set_method_token(f.value);
                } else if (f.name == ":path") {
                    // Origin-form or "*" (OPTIONS); a fragment never belongs in
                    // a request target, and a raw SP would split the request
                    // line downstream.
                    if (!valid_h2_path(f.value)) {
                        malformed("invalid :path");
                        return true;
                    }
                    st.request->set_target(std::move(f.value));
                } else if (f.name == ":authority") {
                    authority = std::move(f.value);
                } else if (f.name == ":protocol") {
                    st.protocol = std::move(f.value);
                }
                // :scheme is checked for presence and then dropped: nothing
                // downstream consumes it, and an engine only runs once the
                // connection's security is already settled.
                continue;
            }

            seen_regular_field = true;
            // RFC 9113 §8.2.1: a regular field name is an HTTP/1.1 token. HPACK
            // can encode any byte, and this name is replayed into an HTTP/1.1
            // upstream by the reverse proxy, so "bad name" or "x:y" must not
            // survive into Request::headers().
            if (!is_http_token(f.name)) {
                malformed("invalid field name");
                return true;
            }
            if (has_upper_ascii(f.name)) {
                malformed("uppercase field name");
                return true;
            }
            // §8.2.2: connection-specific fields are prohibited in HTTP/2.
            if (is_connection_specific_field(f.name)) {
                malformed("connection-specific field");
                return true;
            }
            // §8.2.2: TE is the one exception to that rule, and only with the
            // value "trailers".
            if (f.name == "te" && !iequals_ci(f.value, "trailers")) {
                malformed("TE with a value other than trailers");
                return true;
            }
            if (trailers) {
                // RFC 9110 §6.5.1: framing/routing/hop-by-hop fields do not
                // belong in a trailer section.
                if (is_forbidden_trailer_field(f.name)) {
                    malformed("forbidden trailer field");
                    return true;
                }
                st.request->mutable_trailers().add_lower(std::move(f.name), std::move(f.value));
            } else {
                st.request->mutable_headers().add_lower(std::move(f.name), std::move(f.value));
            }
        }

        // §8.3.1: a request carries :method, :scheme and :path — CONNECT is the
        // one method that must omit the latter two.
        if (!trailers) {
            if (!seen_method) {
                malformed("missing :method");
                return true;
            }
            if (st.request->method() != Method::Connect) {
                if (!seen_scheme) {
                    malformed("missing :scheme");
                    return true;
                }
                if (!seen_path) {
                    malformed("missing :path");
                    return true;
                }
                if (st.request->target().empty()) {
                    malformed("empty :path");
                    return true;
                }
            }
            // RFC 8441 §3: an extended CONNECT selects a protocol with
            // `:protocol`, must be a CONNECT, and carries `:scheme` and `:path`.
            if (seen_protocol) {
                if (!m_limits.h2_enable_connect_protocol) {
                    SIMPLE_HTTP_ERROR_LOG("h2: extended CONNECT while ENABLE_CONNECT_PROTOCOL=0 (stream={})",
                                          stream_id);
                    go_away(codec::H2_PROTOCOL_ERROR);
                    return false;
                }
                if (st.request->method() != Method::Connect) {
                    malformed(":protocol on a non-CONNECT request");
                    return true;
                }
                if (!seen_scheme) {
                    malformed("missing :scheme");
                    return true;
                }
                if (!seen_path || st.request->target().empty()) {
                    malformed("missing :path");
                    return true;
                }
                if (iequals_ci(st.protocol, "websocket")) {
                    st.websocket = true;
                }
            }
            // §8.1.2.6: content-length is a non-negative decimal integer. The body
            // that follows is then held to it — see on_data() and
            // dispatch_stream(). Duplicate fields are ambiguous (the smuggling
            // shape); a value above INT64_MAX would wrap negative and silently
            // disable the check.
            if (st.request->headers().count("content-length") > 1) {
                malformed("duplicate content-length");
                return true;
            }
            if (auto declared = st.request->header("content-length")) {
                std::uint64_t value = 0;
                auto [ptr, ec] = std::from_chars(declared->data(), declared->data() + declared->size(), value);
                if (ec != std::errc{} || ptr != declared->data() + declared->size() ||
                    value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                    malformed("invalid content-length");
                    return true;
                }
                st.declared_content_length = static_cast<std::int64_t>(value);
            }
        }

        // :authority is HTTP/2's spelling of Host, and §8.3.1 makes it the
        // authoritative value: a compliant client sends neither a Host field or
        // sends one with the same value. A conflicting pair (or duplicate Host)
        // is malformed, not a value to pick arbitrarily.
        const std::size_t host_count = st.request->mutable_headers().count("host");
        if (host_count > 1) {
            malformed("duplicate Host");
            return true;
        }
        if (!authority.empty()) {
            if (host_count == 1) {
                const auto host = st.request->header("host");
                if (host && *host != authority) {
                    malformed("Host and :authority disagree");
                    return true;
                }
            }
            st.request->mutable_headers().erase("host");
            st.request->mutable_headers().add_lower("host", std::move(authority));
        }
        // Dispatch is triggered by the caller once END_STREAM is also observed.
        return true;
    }

    bool on_data(const codec::H2FrameHeader &hdr, std::string_view payload) {
        if (hdr.stream_id == 0) {
            go_away(codec::H2_PROTOCOL_ERROR);
            return false;
        }
        bool ok = false;
        std::string_view data = strip_padding(payload, hdr.has_flag(codec::H2_FLAG_PADDED), false, ok);
        if (!ok) {
            go_away(codec::H2_PROTOCOL_ERROR);
            return false;
        }

        // The whole frame length (payload incl. padding) counts against flow
        // control (RFC 7540 §6.9.1). Debit the connection window first; a peer
        // that overruns it is a connection-level FLOW_CONTROL_ERROR.
        const std::int64_t frame_len = static_cast<std::int64_t>(hdr.length);
        m_conn_recv_window -= frame_len;
        if (m_conn_recv_window < 0) {
            go_away(codec::H2_FLOW_CONTROL_ERROR);
            return false;
        }

        auto it = m_streams.find(hdr.stream_id);
        if (it != m_streams.end()) {
            Stream &st = it->second;
            // §5.1: once the peer has ended its side of the stream it may send no
            // more DATA. Checked ahead of the flow-control accounting so the
            // outcome follows the stream state rather than the window.
            if (st.half_closed_remote) {
                // The frame's whole length was already debited from the
                // connection window above (§6.9.1 counts DATA on any stream),
                // and none of it will be delivered to anyone — the stream is
                // reset and erased right below, and erase_stream only returns
                // credit the bytes were counted into (recv_owed_conn). Without
                // returning the window here, each such frame leaks one
                // frame_len of connection credit, and a peer can exhaust the
                // window by ending a handful of streams and sending one DATA on
                // each — killing every legitimate stream on the connection with
                // GOAWAY(FLOW_CONTROL_ERROR). (The client's on_data is the
                // mirror: "hand the connection credit straight back".)
                replenish_conn(frame_len);
                reset_stream(hdr.stream_id, codec::H2_STREAM_CLOSED);
                return true;
            }
            st.recv_window -= frame_len;
            if (st.recv_window < 0) {
                // Stream-level overrun: reset just this stream, keep the
                // connection. The frame was debited from the connection window
                // above but is never delivered, so hand that credit back now
                // rather than leaking it with the stream.
                replenish_conn(frame_len);
                reset_stream(hdr.stream_id, codec::H2_FLOW_CONTROL_ERROR);
                return true;
            }
            // The handler returned without reading the body. Discard each frame
            // and give its credit straight back — do not reset: the response is
            // already in flight, and a RST here tells a client that has the
            // complete response in hand that the exchange failed (our own h2
            // client treats it as an error). The stream retires when the peer
            // ends it.
            if (st.body_discarded) {
                credit_consumed(st, frame_len);
                if (hdr.has_flag(codec::H2_FLAG_END_STREAM)) {
                    st.half_closed_remote = true;
                    maybe_complete_stream(hdr.stream_id);
                }
                return true;
            }

            // Padding counts against flow control but is not delivered to the
            // handler, so it is never "consumed" via the Body hook — replenish
            // its credit immediately (both stream and connection level).
            std::int64_t padding = frame_len - static_cast<std::int64_t>(data.size());
            if (padding > 0)
                credit_consumed(st, padding);

            // RFC 8441: on an extended-CONNECT stream DATA is tunnel bytes.
            if (st.websocket) {
                if (!data.empty()) {
                    if (st.ws_closed) {
                        credit_consumed(st, static_cast<std::int64_t>(data.size()));
                    } else {
                        st.recv_owed_conn += static_cast<std::int64_t>(data.size());
                        st.ws_inbox.append(data);
                        if (st.ws_wake)
                            (void)st.ws_wake->try_send(error_code{});
                    }
                }
                if (hdr.has_flag(codec::H2_FLAG_END_STREAM)) {
                    st.half_closed_remote = true;
                    mark_ws_remote_end(st);
                    maybe_complete_stream(hdr.stream_id);
                }
                return true;
            }

            if (!data.empty()) {
                // §8.1.2.6: a body that overruns its declared content-length is
                // malformed. Caught as the overrun arrives — before the bytes
                // reach the handler — so an unterminated body is caught as well.
                st.body_received += static_cast<std::int64_t>(data.size());
                if (st.declared_content_length >= 0 && st.body_received > st.declared_content_length) {
                    SIMPLE_HTTP_ERROR_LOG("h2: request body overruns content-length (stream={}, "
                                          "declared={}, seen={})",
                                          hdr.stream_id, st.declared_content_length, st.body_received);
                    // These bytes were debited from the connection window but
                    // never delivered; the stream is going away, so return the
                    // connection credit the erase path cannot see.
                    replenish_conn(static_cast<std::int64_t>(data.size()));
                    reset_stream(hdr.stream_id, codec::H2_PROTOCOL_ERROR);
                    return true;
                }
                // The same bound h1 enforces from the declared length/chunk
                // size: a peer must not be able to make the handler (or this
                // engine's parking queue) hold an arbitrary request body. The
                // handler was already dispatched at END_HEADERS, so a 413 is not
                // available here; refuse the stream and stop accepting body.
                if (m_limits.max_body_bytes != 0 &&
                    st.body_received > static_cast<std::int64_t>(m_limits.max_body_bytes)) {
                    SIMPLE_HTTP_ERROR_LOG("h2: request body exceeds max_body_bytes (stream={}, limit={})",
                                          hdr.stream_id, m_limits.max_body_bytes);
                    replenish_conn(static_cast<std::int64_t>(data.size()));
                    reset_stream(hdr.stream_id, codec::H2_ENHANCE_YOUR_CALM);
                    return true;
                }
                // These delivered bytes owe connection-level credit until the
                // handler consumes them (or the stream is torn down).
                st.recv_owed_conn += static_cast<std::int64_t>(data.size());
                if (!st.paused_empty() || !st.request->body().feed(std::string{data})) {
                    // The handler has not drained the body, so the channel is full.
                    // Park the frame on this stream rather than dropping it (the
                    // bytes are already counted against the connection window, so
                    // losing them would strand the connection as well as truncate
                    // the request) and keep parsing: another stream's frames must
                    // not be held up by this one. on_body_consumed() hands parked
                    // frames back as room frees.
                    st.paused_frames.emplace_back(data);
                }
            }
            if (hdr.has_flag(codec::H2_FLAG_END_STREAM)) {
                st.half_closed_remote = true;
                // §8.1.2.6, the other half: a body that stops short of its
                // declared content-length is malformed too. Unlike the overrun
                // above, this is only knowable here, where the body ends.
                if (st.declared_content_length >= 0 && st.body_received != st.declared_content_length) {
                    SIMPLE_HTTP_ERROR_LOG("h2: request body short of content-length (stream={}, "
                                          "declared={}, seen={})",
                                          hdr.stream_id, st.declared_content_length, st.body_received);
                    reset_stream(hdr.stream_id, codec::H2_PROTOCOL_ERROR);
                    return true;
                }
                // End of request body. The handler (already dispatched at
                // END_HEADERS) observes end-of-body on its next Body::read().
                // finish() cannot be dropped - a lost terminator hangs the reader -
                // but it must not overtake the DATA still parked for this stream:
                // Body::read() reports EOF as soon as the channel drains, so
                // finishing here with frames parked would truncate the body. Record
                // it and let on_body_consumed() finish once the parked queue drains.
                if (st.paused_empty()) {
                    st.request->body().finish();
                } else {
                    st.finish_pending = true;
                }
                // If our response already finished, the stream is now complete.
                maybe_complete_stream(hdr.stream_id);
            }
        } else {
            // DATA on a stream we do not hold still counted against the connection
            // window; give that credit straight back.
            replenish_conn(frame_len);
            // §5.1: a stream the peer never opened is idle, and DATA there is a
            // connection error. A skipped id is implicitly closed, but it was
            // never opened either. On a closed stream the cause decides (§5.1):
            // frames after our own RST_STREAM are minimally processed and
            // discarded; frames after the peer's RST_STREAM (or END_STREAM) may
            // be treated as a STREAM_CLOSED connection error.
            if (hdr.stream_id > m_next_peer_stream_id) {
                go_away(codec::H2_PROTOCOL_ERROR);
                return false;
            }
            const ClosedStream *closed = find_closed_stream(hdr.stream_id);
            if (closed == nullptr) {
                go_away(codec::H2_PROTOCOL_ERROR);
                return false;
            }
            if (closed->local_reset)
                return true; // credit already returned above
            go_away(codec::H2_STREAM_CLOSED);
            return false;
        }
        return true;
    }

    bool on_settings(const codec::H2FrameHeader &hdr, std::string_view payload) {
        // §6.5: SETTINGS applies to the connection, so its stream id must be 0.
        if (hdr.stream_id != 0) {
            go_away(codec::H2_PROTOCOL_ERROR);
            return false;
        }
        if (hdr.has_flag(codec::H2_FLAG_ACK)) {
            // §6.5: an ACK is a bare acknowledgement, so any payload is a
            // FRAME_SIZE_ERROR — not a settings block to be applied.
            if (!payload.empty()) {
                go_away(codec::H2_FRAME_SIZE_ERROR);
                return false;
            }
            return true; // our SETTINGS was acked
        }
        if (payload.size() % 6 != 0) {
            go_away(codec::H2_FRAME_SIZE_ERROR);
            return false;
        }
        apply_settings_payload(payload);
        if (m_goaway_sent)
            return false; // a rejected setting sent us away: no ACK
        append_frame(codec::H2FrameType::Settings, codec::H2_FLAG_ACK, 0, {}); // ACK
        return true;
    }

    bool on_window_update(const codec::H2FrameHeader &hdr, std::string_view payload) {
        if (payload.size() != 4) {
            go_away(codec::H2_FRAME_SIZE_ERROR);
            return false;
        }
        const std::uint32_t increment = codec::read_u32(payload, 0) & 0x7FFFFFFFu;
        if (hdr.stream_id == 0) {
            // §6.9: a zero increment is a connection PROTOCOL_ERROR on stream 0.
            if (increment == 0) {
                go_away(codec::H2_PROTOCOL_ERROR);
                return false;
            }
            m_conn_send_window += increment;
            // §6.9.1: a window may not exceed 2^31-1.
            if (m_conn_send_window > kH2MaxWindow) {
                go_away(codec::H2_FLOW_CONTROL_ERROR);
                return false;
            }
            return true;
        }
        auto it = m_streams.find(hdr.stream_id);
        if (it == m_streams.end()) {
            // §5.1: WINDOW_UPDATE on a stream that was never opened is a
            // connection error, and on a closed one it is ignored. Checked
            // before the zero-increment rule: resetting an idle stream would
            // itself be an RST_STREAM on an idle stream (§5.4.2).
            if (hdr.stream_id > m_next_peer_stream_id) {
                go_away(codec::H2_PROTOCOL_ERROR);
                return false;
            }
            return true;
        }
        // §6.9: a zero increment is a stream PROTOCOL_ERROR on an open stream.
        if (increment == 0) {
            reset_stream(hdr.stream_id, codec::H2_PROTOCOL_ERROR);
            return true;
        }
        it->second.send_window += increment;
        if (it->second.send_window > kH2MaxWindow) {
            reset_stream(hdr.stream_id, codec::H2_FLOW_CONTROL_ERROR);
            return true;
        }
        return true;
    }

    bool on_rst_stream(const codec::H2FrameHeader &hdr) {
        // §6.4: an RST_STREAM carries a 4-octet error code, on a non-zero stream.
        if (hdr.length != 4) {
            go_away(codec::H2_FRAME_SIZE_ERROR);
            return false;
        }
        if (hdr.stream_id == 0) {
            go_away(codec::H2_PROTOCOL_ERROR);
            return false;
        }
        auto it = m_streams.find(hdr.stream_id);
        if (it == m_streams.end()) {
            // §5.1: on a stream the peer never opened this is a connection error;
            // on one that has already closed it is ignored — the peer may have
            // sent it before it saw our own end of the stream.
            if (hdr.stream_id > m_next_peer_stream_id) {
                go_away(codec::H2_PROTOCOL_ERROR);
                return false;
            }
            return true;
        }
        (void)it->second.request->body().fail(make_error_code(asio::error::connection_reset));
        it->second.request->cancel(); // peer reset: signal the handler's token
        remember_closed_stream(hdr.stream_id, /*local_reset=*/false);
        erase_stream(hdr.stream_id);
        return true;
    }

    // Removes a stream from the table, first returning any connection-level flow
    // -control credit it still owed for delivered-but-unconsumed body bytes, so
    // the connection window cannot leak when a stream is torn down early.
    void erase_stream(std::uint32_t stream_id) {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end())
            return;
        if (it->second.recv_owed_conn > 0) {
            replenish_conn(it->second.recv_owed_conn);
        }
        // Release a producer parked on backpressure: this stream will never
        // drain now, so it must not be left waiting on a queue that is gone.
        if (it->second.out_space)
            it->second.out_space->close();
        if (it->second.ws_wake)
            it->second.ws_wake->close();
        // A stream reset while its handler was still queued (Rapid Reset): drop
        // the queue entry so the bound is not spent on a stream that is gone.
        // The handler itself, if one was already running, keeps its slot and
        // releases it via handler_finished().
        m_pending_dispatch.erase(std::remove(m_pending_dispatch.begin(), m_pending_dispatch.end(), stream_id),
                                 m_pending_dispatch.end());
        m_streams.erase(it);
    }

    bool on_ping(const codec::H2FrameHeader &hdr, std::string_view payload) {
        // §6.7: a PING is 8 octets, and applies to the connection.
        if (hdr.stream_id != 0) {
            go_away(codec::H2_PROTOCOL_ERROR);
            return false;
        }
        if (payload.size() != 8) {
            go_away(codec::H2_FRAME_SIZE_ERROR);
            return false;
        }
        if (hdr.has_flag(codec::H2_FLAG_ACK))
            return true;
        append_frame(codec::H2FrameType::Ping, codec::H2_FLAG_ACK, 0,
                     payload); // echo
        return true;
    }

    // Emits a WINDOW_UPDATE for `stream_id` (0 = connection) granting `delta`
    // octets back to the peer.
    void send_window_update(std::uint32_t stream_id, std::uint32_t delta) {
        std::string payload;
        codec::append_u32(payload, delta);
        append_frame(codec::H2FrameType::WindowUpdate, /*flags=*/0, stream_id, payload);
    }

    // Returns `n` octets of connection-level receive credit to the peer, batched:
    // accumulate until the threshold, then emit one WINDOW_UPDATE.
    void replenish_conn(std::int64_t n) {
        m_conn_recv_pending += n;
        if (m_conn_recv_pending >= m_limits.h2_initial_window / 2) {
            send_window_update(0, static_cast<std::uint32_t>(m_conn_recv_pending));
            m_conn_recv_window += m_conn_recv_pending;
            m_conn_recv_pending = 0;
        }
    }

    // Credits `n` octets consumed on one stream back to both the stream window
    // and the connection window (batched per level). Called when the handler has
    // read that much body (via the Body consume hook) or for bytes that are never
    // delivered (padding), so credit is only returned for data no longer
    // buffered.
    void credit_consumed(Stream &st, std::int64_t n) {
        st.recv_pending += n;
        if (st.recv_pending >= m_limits.h2_initial_window / 2) {
            send_window_update(st.id, static_cast<std::uint32_t>(st.recv_pending));
            st.recv_window += st.recv_pending;
            st.recv_pending = 0;
        }
        replenish_conn(n);
    }

    // Consume-hook entry point: the handler read `n` body bytes on `stream_id`.
    // Runs on the connection executor (the Body hook posts here), so touching
    // engine state is safe. Replenishes flow-control credit and wakes the writer.
    void on_body_consumed(std::uint32_t stream_id, std::size_t n) {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end()) {
            // Stream already gone: its connection-level credit was returned when
            // the stream was released (release_stream_credit), so nothing to do.
            return;
        }
        it->second.recv_owed_conn -= static_cast<std::int64_t>(n);
        if (it->second.recv_owed_conn < 0)
            it->second.recv_owed_conn = 0; // defensive
        credit_consumed(it->second, static_cast<std::int64_t>(n));
        flush();

        // The handler made room: hand over the frames this stream parked, oldest
        // first, until the channel refuses again. Then re-parse whatever is
        // buffered - it may hold frames for other streams that arrived behind them.
        //
        // feed() leaves its argument intact when it refuses, so moving the front
        // is safe: it is popped only once the channel has actually taken it.
        while (!it->second.paused_empty()) {
            if (!it->second.request->body().feed(std::move(it->second.paused_front())))
                break; // still full: try again on the next consume
            it->second.paused_pop_front();
        }
        // If END_STREAM arrived while frames were parked, hand the end over now
        // that the queue is empty (see on_data): only then may Body::read() report
        // EOF without cutting the body short.
        if (it->second.finish_pending && it->second.paused_empty()) {
            it->second.finish_pending = false;
            it->second.request->body().finish();
        }
        if (!m_recv_buf.empty()) {
            (void)parse_available();
        }
        // Now that the parked tail has actually been handed over (and the body
        // ended), a stream whose response already finished is truly complete.
        // maybe_complete_stream refused to retire it while finish_pending was
        // set, precisely so this call can do it.
        maybe_complete_stream(stream_id);
    }

    // Reclaims a fully-completed stream: once both the request body has ended
    // (half_closed_remote) and our response has emitted END_STREAM
    // (end_stream_sent), the stream can never be read or written again, so it is
    // erased — which also returns any connection-level flow-control credit still
    // owed for request-body bytes the handler never consumed. This is the single
    // place a normally-finished stream is retired (avoiding a per-connection leak
    // of stream entries and receive-window credit on long-lived connections).
    void maybe_complete_stream(std::uint32_t stream_id) {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end())
            return;
        // finish_pending means END_STREAM arrived while DATA was still parked
        // for this stream: erasing now would destroy those frames without ever
        // calling Body::finish(), leaving the handler blocked on an EOF that
        // can never arrive. Wait for on_body_consumed() to drain them.
        if (it->second.finish_pending) {
            return;
        }
        if (it->second.half_closed_remote && it->second.end_stream_sent) {
            // Both halves ended: §5.1 lets a later frame be a STREAM_CLOSED
            // connection error (it is not a stream we reset).
            remember_closed_stream(stream_id, /*local_reset=*/false);
            erase_stream(stream_id);
        }
    }

    void go_away(std::uint32_t error_code_value) {
        if (m_goaway_sent)
            return;
        // Remember the code so the teardown path can re-serialize the GOAWAY if the
        // write loop was cancelled before it reached the wire.
        m_goaway_error = error_code_value;
        std::string payload;
        codec::append_u32(payload, m_next_peer_stream_id); // last stream id we processed
        codec::append_u32(payload, error_code_value);
        append_frame(codec::H2FrameType::Goaway, 0, 0, payload);
        m_goaway_sent = true;
        flush();
    }

    // --- send side: turn queued DATA into flow-controlled DATA frames ---

    void fill_data_frames() {
        // HEADERS always precede DATA: submit_headers() runs synchronously on
        // this executor when the handler replies, appending the HEADERS frame to
        // m_out before any body bytes are queued here.
        auto &finished = m_finished_streams; // streams that just emitted END_STREAM
        finished.clear();                    // keeps capacity across passes

        // The connection window is shared, so this walk has to share it too. A
        // stream that always has bytes queued would otherwise take every refill
        // (the walk starts from the same place every pass), starving its
        // siblings for as long as it lasts. Measured before this cap: with a
        // 64 MiB response in flight, 12 of 20 concurrent 1 KiB responses on the
        // same connection were answered at ~160 ms - the instant the big one
        // finished - where the same 20 on an idle connection all answered in
        // 3.4 ms. So every stream with something to send gets an equal share of
        // what is currently available, per pass.
        std::size_t active = 0;
        for (const auto &[sid, st] : m_streams) {
            if (!st.end_stream_sent && (!st.out_empty() || st.out_finished))
                ++active;
        }
        const std::int64_t share =
            active ? std::max<std::int64_t>(1, m_conn_send_window / static_cast<std::int64_t>(active)) : 0;

        for (auto &[stream_id, st] : m_streams) {
            if (st.end_stream_sent)
                continue;
            // Enough is framed for one pass: let the write loop drain m_out
            // before framing more, so a big peer window cannot grow m_out to
            // multi-MB (see kMaxFramedPerPass). The rest stays queued.
            if (m_out.size() >= kMaxFramedPerPass)
                break;
            std::int64_t emitted = 0; // what this stream has sent so far this pass

            // Emit DATA while the connection and stream windows both allow it.
            for (;;) {
                if (st.out_empty()) {
                    // No queued bytes. If the body is done, end the stream once:
                    // with a trailing HEADERS frame carrying the trailer section
                    // when one was submitted, otherwise the terminating empty
                    // DATA frame with END_STREAM.
                    if (st.out_finished) {
                        if (st.out_trailers_set) {
                            append_field_block(stream_id, st.out_trailers, /*end_stream=*/true);
                        } else {
                            append_frame(codec::H2FrameType::Data, codec::H2_FLAG_END_STREAM, stream_id, {});
                        }
                        st.end_stream_sent = true;
                        finished.push_back(stream_id);
                    }
                    break;
                }

                std::int64_t window = std::min<std::int64_t>(m_conn_send_window, st.send_window);
                if (window <= 0)
                    break; // blocked on flow control; resume on WINDOW_UPDATE

                std::string &front = st.out_front();
                std::size_t available = front.size() - st.out_offset;
                // Bound a DATA frame by the *peer's* SETTINGS_MAX_FRAME_SIZE:
                // that is what it accepts. Our own advertised value limits what
                // we receive, not what we send, so folding it in here (as this
                // used to) only split frames smaller than the peer allows.
                std::int64_t budget = std::min<std::int64_t>(window, static_cast<std::int64_t>(m_peer_max_frame_size));
                if (share > 0)
                    budget = std::min(budget, share - emitted); // fair share is spent
                if (budget <= 0)
                    break; // this stream's share for this pass is used up
                std::size_t take = std::min(available, static_cast<std::size_t>(budget));
                if (take == 0) {
                    // No progress possible (frame-size limit configured to 0):
                    // stop rather than spin and grow m_out without bound.
                    break;
                }
                emitted += static_cast<std::int64_t>(take);

                // Frame [out_offset, out_offset+take) of the front chunk directly,
                // with no intermediate copy.
                std::string_view piece{front.data() + st.out_offset, take};
                st.out_offset += take;
                bool front_done = (st.out_offset >= front.size());
                // A pending trailer section carries END_STREAM itself, so the
                // final DATA frame must not (the trailer HEADERS follows it).
                bool last = front_done && st.out_size() == 1 && st.out_finished && !st.out_trailers_set;

                std::uint8_t flags = last ? codec::H2_FLAG_END_STREAM : 0;
                append_frame(codec::H2FrameType::Data, flags, stream_id, piece);
                m_conn_send_window -= static_cast<std::int64_t>(take);
                st.send_window -= static_cast<std::int64_t>(take);
                st.out_queued -= take;
                // Release a producer parked on backpressure once the queue is back
                // under the low mark. A wake-up that arrives too early is harmless:
                // the producer re-checks the watermark before queueing more.
                if (st.out_queued <= kOutLowWatermark && st.out_space) {
                    (void)st.out_space->try_send(error_code{});
                }

                if (front_done) {
                    st.out_pop_front();
                    st.out_offset = 0;
                }
                if (last) {
                    st.end_stream_sent = true;
                    finished.push_back(stream_id);
                    break;
                }
                if (m_out.size() >= kMaxFramedPerPass)
                    break; // one pass' worth framed; the write loop drains it next
            }
        }
        // Retire any stream that is now complete in both directions (this may
        // erase entries, so it must happen after the iteration above). A stream
        // whose close() arrived after its body was complete got its response
        // framed above; the RST_STREAM close() asked for follows it here, so the
        // peer sees the response and only then the reset that tells it to stop
        // sending (see close_stream()).
        for (std::uint32_t sid : finished) {
            auto it = m_streams.find(sid);
            if (it != m_streams.end() && it->second.reset_after_end) {
                reset_stream(sid, codec::H2_CANCEL);
                continue;
            }
            maybe_complete_stream(sid);
        }
    }

    // --- handler dispatch ---

    // Dispatch a stream's handler, or queue it while the executing-handler cap is
    // reached. Returns false on a fatal connection error (a GOAWAY has been
    // queued by the caller's own dispatch).
    //
    // Rapid Reset (CVE-2023-44487) opens a stream and immediately RST_STREAMs it.
    // The reset frees the stream-table slot that the h2_max_concurrent_streams
    // check counts, but not the handler the stream already started — so without a
    // cap on *executing* handlers a reset flood spawns handlers without bound.
    // The defense mirrors Go's HTTP/2 server: bound concurrently executing
    // handlers to h2_max_concurrent_streams, queue the excess, and treat an
    // overgrown queue as a connection error. (A Hyper-style alternative — defer
    // the per-stream allocation until the handler really runs — is stronger
    // still; this keeps the existing eager-dispatch shape.)
    bool dispatch_stream(std::uint32_t stream_id) {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end() || it->second.dispatched)
            return true;
        // §8.1.2.6: a request whose peer half is already closed must have sent
        // exactly the declared content-length. END_STREAM on the header block
        // used to bypass on_data()'s short-body check entirely.
        if (it->second.half_closed_remote && it->second.declared_content_length >= 0 &&
            it->second.body_received != it->second.declared_content_length) {
            SIMPLE_HTTP_ERROR_LOG("h2: request body short of content-length at END_STREAM (stream={}, declared={}, "
                                  "seen={})",
                                  stream_id, it->second.declared_content_length, it->second.body_received);
            reset_stream(stream_id, codec::H2_PROTOCOL_ERROR);
            return true;
        }
        // Mark the head accepted immediately: a HEADERS arriving later on this
        // stream is then a trailing block, whether or not its handler has started.
        it->second.dispatched = true;
        if (m_active_handlers >= m_limits.h2_max_concurrent_streams) {
            if (m_pending_dispatch.size() >= m_limits.effective_max_queued_streams()) {
                SIMPLE_HTTP_ERROR_LOG("h2: dispatch queue over {} (stream={}); GOAWAY", m_pending_dispatch.size(),
                                      stream_id);
                go_away(codec::H2_ENHANCE_YOUR_CALM);
                return false;
            }
            m_pending_dispatch.push_back(stream_id);
            return true;
        }
        start_handler(stream_id);
        return true;
    }

    // Runs one stream's handler now, occupying an executing-handler slot until it
    // returns (see handler_finished).
    void start_handler(std::uint32_t stream_id) {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end() || it->second.handler_started)
            return;
        it->second.handler_started = true;
        ++m_active_handlers;
        if (it->second.websocket) {
            dispatch_ws_stream(stream_id);
            return;
        }
        auto request = it->second.request;
        auto writer = it->second.writer;
        writer->set_head_request(request->method() == Method::Head);
        if (const auto expect = request->header("expect"); expect && header_has_token(*expect, "100-continue")) {
            std::weak_ptr<Http2ResponseSink<Transport>> weak_writer = writer;
            request->body().set_first_read_hook([weak_writer](asio::any_completion_handler<void(error_code)> done) {
                if (auto w = weak_writer.lock()) {
                    w->send_continue(std::move(done));
                    return;
                }
                std::move(done)(error_code{});
            });
        }
        auto self = this->shared_from_this();
        auto response = std::make_shared<ResponseWriter>(
            maybe_compress_writer(writer, self->m_executor, request, request->method() == Method::Head));
        auto completed = std::make_shared<bool>(false);
        // Keep the request and response alive until the handler signals
        // completion: a callback handler that starts an async body read and
        // returns must not have its Body (or writer) destroyed by a stream
        // reset while that read is still pending.
        auto finish = [self, stream_id, completed, request, response]() mutable {
            if (*completed)
                return;
            *completed = true;
            self->flush();
            self->handler_finished(stream_id);
        };
        try {
            m_dispatch(request, response, m_transport->tls_handle(), finish);
        } catch (const std::exception &e) {
            SIMPLE_HTTP_ERROR_LOG("h2 handler(stream={}) threw: {}", stream_id, e.what());
            self->reset_stream(stream_id, codec::H2_INTERNAL_ERROR);
            finish();
        } catch (...) {
            SIMPLE_HTTP_ERROR_LOG("h2 handler(stream={}) threw unknown exception", stream_id);
            self->reset_stream(stream_id, codec::H2_INTERNAL_ERROR);
            finish();
        }
    }

    // A dispatched handler (ordinary or WebSocket) has returned: free its slot
    // and start whatever the cap made wait. Runs on the connection executor.
    void handler_finished(std::uint32_t stream_id) {
        if (m_active_handlers > 0)
            --m_active_handlers;
        // The handler was this stream's only consumer and producer. If it
        // returned before the peer ended its request body (the early
        // 401/429/405 shape) or before the response ended, nothing will drain
        // what is left: those DATA frames keep debiting the connection window
        // and can stall every stream on the connection. Tell the peer to stop
        // and retire the stream; erase_stream returns the outstanding credit.
        if (auto it = m_streams.find(stream_id); it != m_streams.end()) {
            // out_finished means the handler queued its terminating bytes; the
            // write loop may not have framed them yet, so end_stream_sent is the
            // wrong test here (it would discard a just-finished response).
            if (!it->second.out_finished) {
                // No complete response to preserve: tell the peer instead of
                // leaving it waiting forever.
                reset_stream(stream_id, codec::H2_NO_ERROR);
            } else if (!it->second.half_closed_remote) {
                // The response is complete but the request body is still
                // arriving. Keep answering, drop the tail as it arrives, and
                // return its connection credit (see on_data).
                it->second.body_discarded = true;
            }
        }
        pump_dispatch();
    }

    void pump_dispatch() {
        while (m_active_handlers < m_limits.h2_max_concurrent_streams && !m_pending_dispatch.empty()) {
            const std::uint32_t id = m_pending_dispatch.front();
            m_pending_dispatch.pop_front();
            // A queued stream may have been reset (cheaply) while it waited — that
            // is the whole point — so it can already be gone.
            if (auto it = m_streams.find(id); it != m_streams.end() && !it->second.handler_started)
                start_handler(id);
        }
    }

    // --- HTTP/2 WebSocket (RFC 8441) ---

    // Engine-side half of one extended-CONNECT stream, handed to WsBackendImpl
    // through Http2WsTransport.
    class WsStreamAdapter final : public H2WsStream {
      public:
        WsStreamAdapter(std::weak_ptr<Http2Engine> engine, std::uint32_t id, asio::any_io_executor exec)
            : m_engine(std::move(engine)), m_id(id), m_exec(std::move(exec)) {}

        asio::any_io_executor get_executor() override { return m_exec; }

        void read_some(ByteSpan out, IoCompletion done) override {
            if (auto eng = m_engine.lock()) {
                eng->ws_stream_read(m_id, out, std::move(done));
                return;
            }
            std::move(done)(make_error_code(asio::error::operation_aborted), 0);
        }

        void write_seq(boost::span<const ConstByteSpan> buffers, IoCompletion done) override {
            if (auto eng = m_engine.lock()) {
                eng->ws_stream_write(m_id, buffers, std::move(done));
                return;
            }
            std::move(done)(make_error_code(asio::error::operation_aborted), 0);
        }

        void close() override {
            if (auto eng = m_engine.lock())
                eng->ws_stream_close(m_id);
        }

      private:
        std::weak_ptr<Http2Engine> m_engine;
        std::uint32_t m_id;
        asio::any_io_executor m_exec;
    };

    // Dispatch an extended-CONNECT WebSocket stream: run the ws middleware chain
    // and, if it reaches the terminal, upgrade. With no matching ws route the
    // stream is answered 404 and ended.
    void dispatch_ws_stream(std::uint32_t stream_id) {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end()) {
            handler_finished(stream_id);
            return;
        }
        auto request = it->second.request;
        auto writer = it->second.writer;
        const std::string path{request->path()};
        std::optional<WsHandlerFn> handler_fn;
        if (m_ws_lookup)
            handler_fn = m_ws_lookup(path);
        auto self = this->shared_from_this();
        // Nginx order: local exact ws route beats a proxy route, which beats a
        // local regex ws route.
        if (!handler_fn && m_ws_proxy_lookup) {
            if (auto target = m_ws_proxy_lookup(path)) {
                auto run = std::make_shared<H2WsProxyRun>(self, stream_id, request, writer, std::move(*target));
                run->start();
                return;
            }
        }
        if (!handler_fn && m_ws_regex_lookup)
            handler_fn = m_ws_regex_lookup(path);
        if (!handler_fn) {
            writer->send_bodyless(status::not_found, Headers{}, [self, stream_id](error_code) {
                self->ws_stream_close(stream_id);
                self->handler_finished(stream_id);
            });
            return;
        }
        auto response = std::make_shared<ResponseWriter>(writer);
        auto upgrade = [self, stream_id](RequestPtr req, ResponsePtr res, CallbackWsHandler h,
                                         WsUpgradeCompletion done) mutable {
            self->do_ws_upgrade(stream_id, std::move(req), std::move(res), std::move(h), std::move(done));
        };
        (*handler_fn)(request, response, m_transport->tls_handle(), upgrade,
                      [self, stream_id](error_code, bool) { self->handler_finished(stream_id); });
    }

    // The ws middleware chain's terminal: accept the tunnel with a 200 head and
    // run the handler over a WsBackendImpl backed by this stream.
    void do_ws_upgrade(std::uint32_t stream_id, std::shared_ptr<Request> request, std::shared_ptr<ResponseWriter> res,
                       CallbackWsHandler handler, WsUpgradeCompletion done) {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end()) {
            std::move(done)(make_error_code(asio::error::not_connected), false);
            return;
        }
        auto sink = it->second.writer;

        Headers response_headers = res->headers();
        if (!response_headers.contains("server"))
            response_headers.add_lower("server", std::string{server_product});
        WsDeflateConfig deflate;
        if (m_limits.ws_compression) {
            const auto offered = request->header("sec-websocket-extensions").value_or(std::string_view{});
            if (auto parsed = ws_parse_deflate_offer(offered)) {
                deflate = *parsed;
                response_headers.add_lower("sec-websocket-extensions", ws_deflate_response_value(deflate));
            }
        }

        auto self = this->shared_from_this();
        sink->send_headers(
            status::ok, std::move(response_headers),
            [self, stream_id, request, handler, deflate, done = std::move(done)](error_code ec) mutable {
                if (ec) {
                    std::move(done)(ec, false);
                    return;
                }
                auto adapter = std::make_shared<WsStreamAdapter>(self->weak_from_this(), stream_id, self->m_executor);
                auto transport = std::make_shared<Http2WsTransport<WsStreamAdapter>>(adapter);
                auto backend = std::make_shared<WsBackendImpl<Http2WsTransport<WsStreamAdapter>>>(
                    transport, self->m_limits.max_body_bytes, self->m_limits.idle_timeout, /*expect_masked=*/true,
                    self->m_limits.ws_read_ahead_bytes, deflate);
                auto ws = std::make_shared<WebSocket>(std::move(backend));
                ws->run_writer();
                invoke_ws_handler(WsHandler(handler), request, ws, [ws, done = std::move(done)]() mutable {
                    ws->async_close(
                        [done = std::move(done)](error_code) mutable { std::move(done)(error_code{}, true); });
                });
            });
    }

    // The peer ended its half of a stream (END_STREAM): for a tunnel that is the
    // reader's EOF, wherever the end arrives (a DATA frame or the request head).
    static void mark_ws_remote_end(Stream &st) {
        if (!st.websocket)
            return;
        st.ws_eof = true;
        if (st.ws_wake)
            (void)st.ws_wake->try_send(error_code{});
    }

    // Inbound tunnel read: hands the ws backend buffered DATA bytes and returns
    // flow-control credit for exactly what it consumed.
    void ws_stream_read(std::uint32_t id, ByteSpan out, H2WsStream::IoCompletion done) {
        auto it = m_streams.find(id);
        if (it == m_streams.end()) {
            std::move(done)(make_error_code(asio::error::connection_reset), 0);
            return;
        }
        Stream &st = it->second;
        if (!st.ws_inbox.empty()) {
            const std::size_t n = std::min<std::size_t>(out.size(), st.ws_inbox.size());
            std::memcpy(out.data(), st.ws_inbox.data(), n);
            st.ws_inbox.erase(0, n);
            st.recv_owed_conn -= static_cast<std::int64_t>(n);
            if (st.recv_owed_conn < 0)
                st.recv_owed_conn = 0;
            credit_consumed(st, static_cast<std::int64_t>(n));
            flush();
            std::move(done)(error_code{}, n);
            return;
        }
        if (st.ws_closed) {
            std::move(done)(make_error_code(asio::error::operation_aborted), 0);
            return;
        }
        if (st.ws_eof) {
            std::move(done)(make_error_code(asio::error::eof), 0);
            return;
        }
        if (!st.ws_wake)
            st.ws_wake = std::make_shared<asio::experimental::channel<void(error_code)>>(m_executor, 1);
        auto wake = st.ws_wake;
        auto self = this->shared_from_this();
        wake->async_receive([self, id, out, done = std::move(done)](error_code) mutable {
            self->ws_stream_read(id, out, std::move(done));
        });
    }

    // Outbound tunnel write: frames every buffer as DATA on this stream, waiting
    // on the same backpressure the response path uses.
    void ws_stream_write(std::uint32_t id, boost::span<const ConstByteSpan> bufs, H2WsStream::IoCompletion done) {
        std::size_t total = 0;
        for (const auto &b : bufs)
            total += b.size();
        {
            auto it = m_streams.find(id);
            if (it == m_streams.end() || it->second.end_stream_sent) {
                std::move(done)(make_error_code(asio::error::not_connected), 0);
                return;
            }
        }
        std::string data;
        data.reserve(total);
        for (const auto &b : bufs)
            data.append(reinterpret_cast<const char *>(b.data()), b.size());
        auto self = this->shared_from_this();
        await_out_space(id, [self, id, total, data = std::move(data), done = std::move(done)](error_code ec) mutable {
            if (ec) {
                std::move(done)(ec, 0);
                return;
            }
            self->enqueue_body(id, std::move(data), /*last=*/false);
            std::move(done)(error_code{}, total);
        });
    }

    // Ends the tunnel with END_STREAM and wakes any parked reader.
    void ws_stream_close(std::uint32_t id) {
        auto it = m_streams.find(id);
        if (it == m_streams.end())
            return;
        Stream &st = it->second;
        st.ws_closed = true;
        if (!st.end_stream_sent)
            enqueue_body(id, {}, /*last=*/true);
        if (st.ws_wake)
            st.ws_wake->close();
        flush();
    }

    // --- h2 extended-CONNECT WebSocket reverse proxy ---
    //
    // The frontend handshake is an extended CONNECT (already matched), so we
    // synthesize an ordinary HTTP/1.1 upgrade to the backend, wait for its 101,
    // answer the h2 stream with 200, then splice bytes verbatim. Because the
    // proxy is the *client* on both sides, masking, fragmentation, control
    // frames and permessage-deflate pass through unchanged.
    class H2WsProxyRun : public std::enable_shared_from_this<H2WsProxyRun> {
      public:
        H2WsProxyRun(std::shared_ptr<Http2Engine> engine, std::uint32_t stream_id, std::shared_ptr<Request> request,
                     std::shared_ptr<Http2ResponseSink<Transport>> sink, WsProxyTarget target)
            : m_engine(std::move(engine)), m_id(stream_id), m_request(std::move(request)), m_sink(std::move(sink)),
              m_target(std::move(target)), m_timer(m_engine->m_executor) {}

        void start() {
            auto self = this->shared_from_this();
            detail::async_ws_proxy_connect(
                m_engine->m_executor, m_target,
                [self](error_code ec, std::shared_ptr<asio::ip::tcp::socket> backend) mutable {
                    self->on_connected(ec, std::move(backend));
                });
        }

      private:
        void on_connected(error_code ec, std::shared_ptr<asio::ip::tcp::socket> backend) {
            if (ec) {
                SIMPLE_HTTP_ERROR_LOG("h2 ws-proxy connect {}:{} failed (client {}): {}", m_target.host, m_target.port,
                                      address_string(m_engine->m_transport->peer().address()), ec.message());
                fail(status::bad_gateway);
                return;
            }
            m_backend = std::move(backend);

            const auto query = m_request->query();
            auto replay =
                detail::ws_proxy_replay_target(m_target.rewrite_path, m_request->path(),
                                               query.empty() ? std::nullopt : std::optional<std::string_view>{query});
            if (!replay) {
                SIMPLE_HTTP_ERROR_LOG("h2 ws-proxy: refusing a request target with CR/LF/NUL (client {})",
                                      address_string(m_engine->m_transport->peer().address()));
                close_backend();
                fail(status::bad_request);
                return;
            }
            const std::string replay_target = std::move(*replay);

            m_key = detail::make_ws_client_key();
            const auto inbound_host = m_request->header("host");
            const bool client_is_tls = m_engine->m_transport->tls_handle().has_value();
            auto head = std::make_shared<std::string>(detail::build_ws_upgrade_head(
                m_request->headers(), inbound_host ? *inbound_host : std::string_view{}, replay_target, m_target,
                address_string_if_specified(m_engine->m_transport->peer().address()), client_is_tls, m_key));
            auto self = this->shared_from_this();
            asio::async_write(*m_backend, asio::buffer(head->data(), head->size()),
                              [self, head](error_code wec, std::size_t) mutable {
                                  if (wec) {
                                      SIMPLE_HTTP_ERROR_LOG(
                                          "h2 ws-proxy backend handshake write failed (client {}): {}",
                                          address_string(self->m_engine->m_transport->peer().address()), wec.message());
                                      self->close_backend();
                                      self->fail(status::bad_gateway);
                                      return;
                                  }
                                  self->read_reply();
                              });
        }

        void read_reply() {
            auto self = this->shared_from_this();
            detail::async_ws_proxy_read_reply(*m_backend, m_engine->m_limits.max_header_bytes, m_target.connect_timeout,
                                              [self](error_code ec, detail::WsProxyReplyHead reply) mutable {
                                                  self->on_reply(ec, std::move(reply));
                                              });
        }

        void on_reply(error_code ec, detail::WsProxyReplyHead reply) {
            if (ec) {
                SIMPLE_HTTP_ERROR_LOG("h2 ws-proxy backend handshake failed (client {}): {}",
                                      address_string(m_engine->m_transport->peer().address()), ec.message());
                close_backend();
                fail(status::bad_gateway);
                return;
            }

            if (reply.head.status != status::switching_protocols) {
                // Surface the backend's actual refusal status and end-to-end
                // fields (WWW-Authenticate, Location, ...), not an invented 502.
                // The body is not relayed: an h2 extended-CONNECT response ends
                // at the headers, and the peer's status is the actionable part.
                Headers response_headers;
                for (const auto &[name, value] : reply.head.headers) {
                    if (is_hop_by_hop_field(name) || name == "content-length" || name == "transfer-encoding")
                        continue;
                    if (contains_ctl(name) || contains_ctl(value))
                        continue;
                    response_headers.add_lower(name, value);
                }
                SIMPLE_HTTP_ERROR_LOG("h2 ws-proxy backend refused the upgrade with {} (client {})", reply.head.status,
                                      address_string(m_engine->m_transport->peer().address()));
                close_backend();
                fail(reply.head.status, std::move(response_headers));
                return;
            }

            if (!detail::ws_validate_101(reply.head, ws_accept_key(m_key))) {
                SIMPLE_HTTP_ERROR_LOG("h2 ws-proxy backend answered 101 with a mismatched Upgrade/accept (client {})",
                                      address_string(m_engine->m_transport->peer().address()));
                close_backend();
                fail(status::bad_gateway);
                return;
            }

            // Forward the backend's negotiated fields into the h2 200, minus the
            // HTTP/1.1-only / hop-by-hop ones (the h2 client must not see
            // Upgrade, Connection or Sec-WebSocket-Accept).
            Headers response_headers;
            for (const auto &[name, value] : reply.head.headers) {
                if (is_hop_by_hop_field(name) || name == "sec-websocket-accept" || name == "content-length" ||
                    name == "transfer-encoding")
                    continue;
                if (contains_ctl(name) || contains_ctl(value))
                    continue;
                response_headers.add_lower(name, value);
            }
            auto self = this->shared_from_this();
            m_initial = std::move(reply.initial);
            m_sink->send_headers(status::ok, std::move(response_headers), [self](error_code ec2) mutable {
                if (ec2) {
                    self->close_backend();
                    self->finish();
                    return;
                }
                self->start_tunnel();
            });
        }

        void start_tunnel() {
            touch();
            arm_timer();
            // The backend may have sent tunnel bytes in the same read as its
            // 101; forward them before the first read.
            if (!m_initial.empty()) {
                auto initial = std::make_shared<std::string>(std::move(m_initial));
                auto self = this->shared_from_this();
                ConstByteSpan one{reinterpret_cast<const std::byte *>(initial->data()), initial->size()};
                m_engine->ws_stream_write(m_id, boost::span<const ConstByteSpan>{&one, 1},
                                          [self, initial](error_code ec, std::size_t) mutable {
                                              if (ec) {
                                                  self->close_backend();
                                                  self->finish();
                                                  return;
                                              }
                                              self->touch();
                                              self->pump_backend_to_client();
                                              self->pump_client_to_backend();
                                          });
                return;
            }
            pump_backend_to_client();
            pump_client_to_backend();
        }

        void touch() { m_deadline = std::chrono::steady_clock::now() + m_engine->m_limits.idle_timeout; }

        void arm_timer() {
            if (m_engine->m_limits.idle_timeout.count() <= 0)
                return;
            m_timer.expires_at(m_deadline);
            auto self = this->shared_from_this();
            m_timer.async_wait([self](error_code ec) {
                if (!ec)
                    self->on_idle();
            });
        }

        void on_idle() {
            if (std::chrono::steady_clock::now() < m_deadline) {
                // touch() does not re-arm: without this the tunnel's idle
                // timeout disappears after the first tick.
                arm_timer();
                return;
            }
            SIMPLE_HTTP_ERROR_LOG("h2 ws-proxy tunnel idle (client {})",
                                  address_string(m_engine->m_transport->peer().address()));
            close_backend();
            m_engine->ws_stream_close(m_id);
        }

        void pump_backend_to_client() {
            auto buf = std::make_shared<std::array<std::byte, 16384>>();
            auto self = this->shared_from_this();
            m_backend->async_read_some(
                asio::buffer(buf->data(), buf->size()), [self, buf](error_code ec, std::size_t n) mutable {
                    if (ec || n == 0) {
                        // The backend is done: end the
                        // tunnel's read side, which also
                        // wakes the other direction.
                        self->m_b2c_done = true;
                        self->m_engine->ws_stream_close(self->m_id);
                        self->maybe_finish();
                        return;
                    }
                    self->touch();
                    ConstByteSpan one{buf->data(), n};
                    self->m_engine->ws_stream_write(self->m_id, boost::span<const ConstByteSpan>{&one, 1},
                                                    [self, buf](error_code wec, std::size_t) mutable {
                                                        if (wec) {
                                                            self->m_b2c_done = true;
                                                            self->maybe_finish();
                                                            return;
                                                        }
                                                        self->touch();
                                                        self->pump_backend_to_client();
                                                    });
                });
        }

        void pump_client_to_backend() {
            auto buf = std::make_shared<std::array<std::byte, 16384>>();
            auto self = this->shared_from_this();
            m_engine->ws_stream_read(m_id, ByteSpan{buf->data(), buf->size()},
                                     [self, buf](error_code ec, std::size_t n) mutable {
                                         if (ec || n == 0) {
                                             // Client half-close (END_STREAM): stop sending to the
                                             // backend, but leave the opposite direction running so
                                             // the backend's Close frame still arrives.
                                             error_code sec;
                                             self->m_backend->shutdown(asio::ip::tcp::socket::shutdown_send, sec);
                                             self->m_c2b_done = true;
                                             self->maybe_finish();
                                             return;
                                         }
                                         self->touch();
                                         asio::async_write(*self->m_backend, asio::buffer(buf->data(), n),
                                                           [self, buf](error_code wec, std::size_t) mutable {
                                                               if (wec) {
                                                                   self->m_c2b_done = true;
                                                                   self->maybe_finish();
                                                                   return;
                                                               }
                                                               self->touch();
                                                               self->pump_client_to_backend();
                                                           });
                                     });
        }

        void maybe_finish() {
            if (!m_b2c_done || !m_c2b_done || m_finished)
                return;
            m_finished = true;
            m_timer.cancel();
            finish();
        }

        void fail(int status_code, Headers headers = {}) {
            auto self = this->shared_from_this();
            m_sink->send_bodyless(status_code, std::move(headers), [self](error_code) mutable {
                self->m_engine->ws_stream_close(self->m_id);
                self->finish();
            });
        }

        void close_backend() {
            if (m_backend)
                detail::ws_proxy_close(*m_backend);
        }

        void finish() {
            if (m_completed)
                return;
            m_completed = true;
            m_timer.cancel();
            close_backend();
            m_engine->ws_stream_close(m_id);
            m_engine->handler_finished(m_id);
        }

        std::shared_ptr<Http2Engine> m_engine;
        std::uint32_t m_id;
        std::shared_ptr<Request> m_request;
        std::shared_ptr<Http2ResponseSink<Transport>> m_sink;
        WsProxyTarget m_target;

        std::shared_ptr<asio::ip::tcp::socket> m_backend;
        std::string m_key;
        std::string m_initial;
        asio::steady_timer m_timer;
        std::chrono::steady_clock::time_point m_deadline{};
        bool m_b2c_done{false};
        bool m_c2b_done{false};
        bool m_finished{false};
        bool m_completed{false};
    };

    // --- members (touched only on the connection executor) ---
    std::shared_ptr<Transport> m_transport;
    Executor m_executor;
    const std::thread::id m_thread_id{std::this_thread::get_id()};
    asio::experimental::channel<void(error_code)> m_notify;
    EngineLimits m_limits;
    std::chrono::steady_clock::time_point m_deadline{};
    std::shared_ptr<asio::steady_timer> m_watchdog{std::make_shared<asio::steady_timer>(m_executor)};
    std::function<void()> m_on_done;
    bool m_finished{false};
    bool m_finish_started{false};
    bool m_write_in_flight{false};
    bool m_done_called{false};
    std::array<std::byte, 32 * 1024> m_read_scratch{};

    Dispatcher m_dispatch;
    WsLookup m_ws_lookup;            // local exact ws routes (RFC 8441), empty if none
    WsProxyLookup m_ws_proxy_lookup; // byte-level ws reverse-proxy routes, empty if none
    WsLookup m_ws_regex_lookup;      // local regex ws routes, empty if none
    codec::HpackDecoder m_decoder;   // connection-scoped (read loop only)
    // Connection-scoped scratch buffers. Every write path already runs on the
    // connection executor (model A), so one buffer each is enough; they keep
    // their capacity between requests instead of per-request allocations.
    std::vector<codec::HpackHeader> m_decode_fields; // decoded request head
    std::string m_response_block;                    // serialized response head
    std::vector<std::uint32_t> m_finished_streams;   // END_STREAM emitters per pass
    std::unordered_map<std::uint32_t, Stream> m_streams;

    // Rapid Reset defense: handlers currently executing on this connection, and
    // the streams queued behind the cap. Both touched only on the connection
    // executor (dispatch runs from the read loop; handler_finished from the
    // handler coroutines, which run there too).
    std::size_t m_active_handlers{0};
    std::deque<std::uint32_t> m_pending_dispatch;

    std::string m_recv_buf; // received bytes awaiting frame parsing
    std::string m_out;      // serialized frames awaiting transport write

    std::uint32_t m_goaway_error{0};                       // code for the GOAWAY re-emitted on teardown
    std::int64_t m_conn_send_window = kH2InitialWindow;    // peer's connection-level window for us (send side)
    std::int64_t m_conn_recv_window = kH2InitialWindow;    // our connection-level window to the peer (recv side)
    std::int64_t m_conn_recv_pending = 0;                  // consumed bytes awaiting a batched connection WINDOW_UPDATE
    std::int32_t m_peer_initial_window = kH2InitialWindow; // peer SETTINGS_INITIAL_WINDOW_SIZE
    std::uint32_t m_peer_max_frame_size = static_cast<std::uint32_t>(kH2MaxFrameSize);

    std::uint32_t m_next_peer_stream_id = 0; // highest client stream id seen
    std::uint32_t m_continuation_stream = 0; // stream awaiting CONTINUATION, or 0
    // A refused stream's split head is drained rather than decoded: the stream
    // was already reset, but RFC 9113 §6.10 still requires its CONTINUATION
    // frames to be consumed (and forbids interleaving anything else).
    bool m_continuation_discard = false;
    std::size_t m_continuation_discard_bytes = 0;
    // Fields are thrown away, but the bytes are decoded so the connection's
    // HPACK dynamic table mirrors the peer's encoder (RFC 9113 §4.3.1).
    std::string m_continuation_discard_block;

    bool m_alive = true;
    bool m_goaway_sent = false;
    bool m_preface_consumed = false; // client connection preface stripped yet?

    friend class Http2ResponseSink<Transport>;
}; // class Http2Engine

} // namespace simple_http
