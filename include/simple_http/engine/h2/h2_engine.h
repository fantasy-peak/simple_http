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
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
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
#include "../../transport/transport.h" // SslHandle, TransportLike
#include "../dispatcher.h"
#include "../ws_proxy_common.h"
#include "h2_frame.h"
#include "h2_ws_transport.h"
#include "hpack_decode.h"
#include "hpack_encoder.h"

namespace simple_http {

namespace asio = boost::asio;

template <TransportLike Transport> class Http2Engine;

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

// ResponseSink for a single HTTP/2 stream. Holds a weak_ptr to the engine so
// it can be used safely from any thread and after the connection has closed.
template <TransportLike Transport> class Http2ResponseSink : public ResponseSink {
  public:
    using Executor = decltype(std::declval<Transport &>().get_executor());

    Http2ResponseSink(std::weak_ptr<Http2Engine<Transport>> engine, std::uint32_t stream_id, Executor exec)
        : m_engine(std::move(engine)), m_stream_id(stream_id), m_executor(exec) {}

    asio::awaitable<error_code> send(int status, Headers headers, std::string body) override {
        co_await hop();
        auto eng = m_engine.lock();
        if (!eng || !eng->alive() || !eng->stream_writable(m_stream_id)) {
            co_return make_error_code(asio::error::not_connected);
        }
        // A response to HEAD has no body: END_STREAM on the HEADERS frame, and the
        // headers still carry the Content-Length a GET would have produced.
        //
        // Only a HEAD is given that length explicitly. HTTP/2 does not need
        // content-length to frame a body - END_STREAM delimits it - so adding one
        // to every one-shot response would be redundant. A HEAD is the exception:
        // nothing else in the response conveys the size that was deliberately not
        // sent, so without this a `HEAD /world` over HTTP/2 answered `200 +
        // content-type + server` where the same route over HTTP/1.1 answered
        // `content-length: 1024` (h1_engine.h send() sets it for HEAD too).
        ensure_date(headers);
        const bool head = eng->method_is_head(m_stream_id);
        if (head) {
            headers.erase("content-length");
            headers.add_lower("content-length", std::to_string(body.size()));
        }
        eng->submit_headers(m_stream_id, status, headers, /*end_stream=*/head);
        if (!head)
            eng->enqueue_body(m_stream_id, std::move(body), /*last=*/true);
        co_return error_code{};
    }

    asio::awaitable<error_code> send_bodyless(int status, Headers headers) override {
        co_await hop();
        auto eng = m_engine.lock();
        if (!eng || !eng->alive() || !eng->stream_writable(m_stream_id)) {
            co_return make_error_code(asio::error::not_connected);
        }
        ensure_date(headers);
        // Nothing follows the HEADERS frame: END_STREAM ends the stream (RFC 9113
        // §8.1).
        eng->submit_headers(m_stream_id, status, headers, /*end_stream=*/true);
        co_return error_code{};
    }

    asio::awaitable<error_code> send_headers(int status, Headers headers) override {
        co_await hop();
        auto eng = m_engine.lock();
        if (!eng || !eng->alive() || !eng->stream_writable(m_stream_id)) {
            co_return make_error_code(asio::error::not_connected);
        }
        ensure_date(headers);
        // A streamed response has no length it can promise - END_STREAM delimits
        // it - so a handler-supplied Content-Length must not survive next to it.
        // HTTP/1.1's sink erases it for the same reason (h1_engine.h,
        // send_headers): a message carrying two framings is a response-splitting
        // vector for any intermediary, and a wrong one here would be repeated
        // downstream by the reverse proxy.
        headers.erase("content-length");
        eng->submit_headers(m_stream_id, status, headers,
                            /*end_stream=*/eng->method_is_head(m_stream_id));
        eng->flush();
        co_return error_code{};
    }

    asio::awaitable<error_code> send_chunk(std::string data) override {
        co_await hop();
        auto eng = m_engine.lock();
        if (!eng || !eng->alive()) {
            co_return make_error_code(asio::error::not_connected);
        }
        // Check the cached HEAD flag before stream_writable(): a response to HEAD
        // ends the stream with its HEADERS frame, and the entry is retired right
        // there. Looking the method up in the stream table (or checking
        // writability first) would report not_connected for a body that must
        // simply be dropped.
        if (m_head_request)
            co_return error_code{}; // HEAD: no body
        if (!eng->stream_writable(m_stream_id))
            co_return make_error_code(asio::error::not_connected);
        // Backpressure: block while this stream's queue is over the high mark, so
        // a fast producer (reverse proxy) is paced by the peer's window.
        if (auto ec = co_await eng->await_out_space(m_stream_id); ec)
            co_return ec;
        eng->enqueue_body(m_stream_id, std::move(data), /*last=*/false);
        co_return error_code{};
    }

    asio::awaitable<error_code> send_last(std::string data) override {
        co_await hop();
        auto eng = m_engine.lock();
        if (!eng || !eng->alive()) {
            co_return make_error_code(asio::error::not_connected);
        }
        if (m_head_request)
            co_return error_code{}; // HEAD: no body
        if (!eng->stream_writable(m_stream_id))
            co_return make_error_code(asio::error::not_connected);
        if (auto ec = co_await eng->await_out_space(m_stream_id); ec)
            co_return ec;
        eng->enqueue_body(m_stream_id, std::move(data), /*last=*/true);
        co_return error_code{};
    }

    asio::awaitable<error_code> send_trailers(Headers trailers) override {
        co_await hop();
        auto eng = m_engine.lock();
        if (!eng || !eng->alive()) {
            co_return make_error_code(asio::error::not_connected);
        }
        if (m_head_request)
            co_return error_code{}; // HEAD: no body, so no trailer section either
        if (!eng->stream_writable(m_stream_id))
            co_return make_error_code(asio::error::not_connected);
        eng->submit_trailers(m_stream_id, trailers);
        co_return error_code{};
    }

    asio::awaitable<bool> connected() const override {
        // The hop matters here more than anywhere: stream_writable() walks the
        // engine's stream table, which the connection executor is inserting into
        // and erasing from concurrently.
        co_await hop();
        auto eng = m_engine.lock();
        // A reset/finished stream cannot take a response either: report the
        // response as unwritable so callers (e.g. a proxy's 502 path) stop
        // rather than trying to write onto a dead stream.
        co_return eng && eng->alive() && eng->stream_writable(m_stream_id);
    }
    asio::awaitable<void> close() override {
        co_await hop(); // close_stream() touches the same table
        if (auto eng = m_engine.lock()) {
            eng->close_stream(m_stream_id);
        }
    }
    Version version() const override { return Version::Http2; }
    bool head_request() const noexcept override { return m_head_request; }

    // Cached when the handler starts (the parsed method is only available then).
    // The write methods need it after the HEADERS frame has already retired the
    // stream, when a stream-table lookup would no longer find the request.
    void set_head_request(bool head) noexcept { m_head_request = head; }

  private:
    asio::awaitable<void> hop() const { co_await asio::dispatch(asio::bind_executor(m_executor, asio::use_awaitable)); }

    std::weak_ptr<Http2Engine<Transport>> m_engine;
    std::uint32_t m_stream_id;
    Executor m_executor;
    bool m_head_request{false};
};

template <TransportLike Transport> class Http2Engine : public std::enable_shared_from_this<Http2Engine<Transport>> {
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

    // Serve the connection until it closes. `dispatch` runs a handler per stream.
    // `prior_knowledge_bytes` (if any) are bytes already consumed during protocol
    // detection — for prior-knowledge h2 they are the client preface and are fed
    // to the parser first.
    asio::awaitable<void> run(Dispatcher dispatch, std::string prior_knowledge_bytes = {}, WsLookup ws_lookup = {},
                              WsLookup ws_regex_lookup = {}, WsProxyLookup ws_proxy_lookup = {}) {
        m_dispatch = std::move(dispatch);
        m_ws_lookup = std::move(ws_lookup);
        m_ws_regex_lookup = std::move(ws_regex_lookup);
        m_ws_proxy_lookup = std::move(ws_proxy_lookup);
        queue_settings();
        m_recv_buf = std::move(prior_knowledge_bytes);
        co_await serve_loops();
        co_return;
    }

    // Serve an h2c upgrade: the client's base64url HTTP2-Settings seed the peer
    // SETTINGS, and the original HTTP/1.1 request is replayed as stream 1.
    asio::awaitable<void> run_h2c(Dispatcher dispatch, const std::string &settings_b64, Method method,
                                  std::string target, Headers headers, std::string body,
                                  std::string prior_knowledge_bytes = {}, WsLookup ws_lookup = {},
                                  WsLookup ws_regex_lookup = {}, WsProxyLookup ws_proxy_lookup = {}) {
        m_dispatch = std::move(dispatch);
        m_ws_lookup = std::move(ws_lookup);
        m_ws_regex_lookup = std::move(ws_regex_lookup);
        m_ws_proxy_lookup = std::move(ws_proxy_lookup);
        // Apply the client's SETTINGS payload (raw settings frame body).
        std::string settings = base64_url_decode(settings_b64);
        // A SETTINGS body is a whole number of 6-octet entries (RFC 9113 §6.5).
        // on_settings() enforces that for a SETTINGS frame on the wire; this path
        // decodes a header and used to skip the check, and apply_settings_payload
        // silently ignores a trailing partial entry — so a corrupt HTTP2-Settings
        // header would be applied as if it were well-formed. base64_url_decode
        // stopping at the first invalid character is what makes that reachable:
        // one bad byte truncates the payload instead of failing it.
        if (settings.size() % 6 != 0) {
            go_away(codec::H2_FRAME_SIZE_ERROR);
            // serve_loops() has not started, so no write loop will flush this
            // GOAWAY; send it directly before the connection goes.
            co_await flush_out_directly();
            co_return;
        }
        apply_settings_payload(settings);
        if (m_goaway_sent) {
            // The h2c HTTP2-Settings were rejected; same missing write loop.
            co_await flush_out_directly();
            co_return;
        }
        queue_settings();

        // Seed stream 1 from the initial request and dispatch it.
        auto &st = ensure_stream(1);
        st.request->set_method(method);
        st.request->set_method_token(std::string{to_string(method)});
        st.request->set_target(std::move(target));
        st.request->mutable_headers() = std::move(headers);
        if (!body.empty())
            (void)st.request->body().feed(std::move(body));
        (void)st.request->body().finish();
        // The upgraded request arrived in full as HTTP/1.1 - body included - so
        // its read side is already closed. Without this the stream can never
        // satisfy maybe_complete_stream() (which is what retires a finished
        // stream) and stays in the table for the connection's life: one leaked
        // entry per upgraded connection, still counted against
        // h2_max_concurrent_streams. Measured with the limit at 2: the upgraded
        // request was answered 200 and ended, and the *next* new stream on that
        // connection was refused with REFUSED_STREAM.
        st.half_closed_remote = true;
        m_next_peer_stream_id = 1; // the upgraded request's id is the highest seen so far
        (void)dispatch_stream(1);

        // Bytes already read past the HTTP/1.1 upgrade request: the client's
        // connection preface and whatever followed it. Handed to the reader
        // instead of dropped (see Http1Engine::do_h2c_upgrade).
        m_recv_buf = std::move(prior_knowledge_bytes);
        co_await serve_loops();
        co_return;
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
        if (const std::string alt_svc = m_limits.alt_svc_value(); !alt_svc.empty()) {
            codec::hpack_append_literal(block, "alt-svc", alt_svc);
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
    asio::awaitable<error_code> await_out_space(std::uint32_t stream_id) {
        for (;;) {
            auto it = m_streams.find(stream_id);
            if (it == m_streams.end())
                co_return make_error_code(asio::error::operation_aborted);
            if (it->second.out_queued <= kOutHighWatermark)
                co_return error_code{};
            if (!it->second.out_space) {
                it->second.out_space = std::make_shared<asio::experimental::channel<void(error_code)>>(m_executor, 1);
            }
            // Keep the channel alive across the wait: the stream entry itself may
            // be erased (and the channel closed, waking us) while we are parked.
            auto space = it->second.out_space;
            auto [ec] = co_await space->async_receive(asio::as_tuple(asio::use_awaitable));
            if (ec)
                co_return make_error_code(asio::error::operation_aborted);
        }
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
        erase_stream(stream_id);
        flush();
    }

    // ResponseSink::close() for one stream.
    //
    // Deliberately *not* reset_stream(): this response may already be complete
    // and merely queued. HTTP/1.1's sink writes to the socket inside send()
    // (`co_await write_raw(...)`), so a handler that does send() and then
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
    struct Stream {
        std::shared_ptr<Request> request;
        std::shared_ptr<Http2ResponseSink<Transport>> writer;
        std::string header_block; // accumulates HEADERS + CONTINUATION

        // Outbound response body: a queue of chunks the write loop drains into
        // flow-controlled DATA frames. out_offset marks how much of the front
        // chunk has already been framed (so partial sends need no substr copy).
        // A vector with a read cursor, not a deque: a default-constructed
        // std::deque allocates its block map, and every stream paid that (plus
        // paused_frames') for the life of the request. The cursor resets when
        // the queue empties, so the capacity is reused.
        std::vector<std::string> out_queue;
        std::size_t out_head = 0;
        bool out_empty() const { return out_head >= out_queue.size(); }
        std::string &out_front() { return out_queue[out_head]; }
        std::size_t out_size() const { return out_queue.size() - out_head; }
        void out_pop_front() {
            if (++out_head >= out_queue.size()) {
                out_queue.clear();
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
            }
        }
        // END_STREAM arrived while frames were still parked. Body::read() reports
        // EOF as soon as the channel drains, so body().finish() must wait until the
        // parked queue is empty or the consumer would see a truncated body; see
        // on_body_consumed().
        bool finish_pending = false;

        // --- HTTP/2 WebSocket (RFC 8441) ---
        // Set once the request head carried `:protocol: websocket`. From then on
        // DATA on this stream is tunnel bytes, not a request body.
        bool websocket = false;
        std::string protocol;                                                   // the raw `:protocol` value (RFC 8441)
        std::string ws_inbox;                                                   // inbound tunnel bytes, ordered
        bool ws_eof = false;                                                    // the peer ended its half (END_STREAM)
        bool ws_closed = false;                                                 // we tore the tunnel down
        std::shared_ptr<asio::experimental::channel<void(error_code)>> ws_wake; // wakes a parked reader

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

    asio::awaitable<void> serve_loops() {
        using namespace asio::experimental::awaitable_operators;
        m_deadline = std::chrono::steady_clock::now() + m_limits.idle_timeout;
        flush(); // push our initial SETTINGS
        if (m_limits.idle_timeout.count() > 0) {
            co_await (read_loop() || write_loop() || watchdog());
        } else {
            // Same convention as h1 and net/connection.h: <= 0 disables the
            // idle deadline (racing a zero-length timer would close instantly).
            co_await (read_loop() || write_loop());
        }
        m_alive = false;
        // Release every producer still parked on outbound backpressure: the write
        // loop is gone, so nothing will ever drain their queues. Also fail the
        // request body of every stream still in the table: a handler parked in
        // body().read() (dispatched at END_HEADERS) must observe the connection's
        // death, or it waits forever and keeps this engine alive with it.
        for (auto &entry : m_streams) {
            if (entry.second.request) {
                (void)entry.second.request->body().fail(make_error_code(asio::error::connection_reset));
                entry.second.request->cancel(); // connection gone: signal every handler
            }
            if (entry.second.out_space)
                entry.second.out_space->close();
            entry.second.ws_closed = true;
            if (entry.second.ws_wake)
                entry.second.ws_wake->close();
        }
        // Whichever loop finished first cancels the others, and a cancelled
        // async_write drops its buffer - so the GOAWAY the write loop had just
        // taken may never reach the wire, leaving the peer with a bare TCP close
        // and no explanation. Re-serialize it here and write it directly (RFC 9113
        // §6.8 allows an endpoint to send GOAWAY more than once, so re-sending is
        // safe).
        if (m_goaway_sent) {
            m_out.clear(); // queued DATA is moot on a connection error
            std::string payload;
            codec::append_u32(payload, m_next_peer_stream_id);
            codec::append_u32(payload, m_goaway_error);
            append_frame(codec::H2FrameType::Goaway, 0, 0, payload);
        }
        // Flush with a deadline: a peer that stopped reading must not keep the
        // connection (and this coroutine) alive.
        co_await (flush_pending_output() || flush_deadline());
        // TLS gets a close_notify before the socket goes; plaintext's
        // shutdown() is a no-op.
        co_await m_transport->shutdown();
        m_transport->close();
        co_return;
    }

    // Write m_out synchronously, for phases where no write loop exists yet
    // (the h2c handshake's GOAWAY before serve_loops()).
    asio::awaitable<void> flush_out_directly() {
        if (m_out.empty())
            co_return;
        auto bytes = std::as_bytes(std::span<const char>{m_out.data(), m_out.size()});
        (void)co_await m_transport->async_write(bytes);
        m_out.clear();
    }

    // Best-effort flush of the serialized output left behind by the write loop.
    asio::awaitable<void> flush_pending_output() {
        if (m_out.empty())
            co_return;
        auto bytes = std::as_bytes(std::span<const char>{m_out.data(), m_out.size()});
        auto [ec, n] = co_await m_transport->async_write(bytes);
        (void)n;
        m_out.clear();
        if (ec)
            co_return;
        co_return;
    }

    // Deadline for the teardown flush above: never let a non-reading peer keep
    // the connection (and its coroutine frame) alive.
    asio::awaitable<void> flush_deadline() {
        asio::steady_timer timer{m_executor};
        timer.expires_after(std::chrono::seconds(2));
        co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        co_return;
    }

    asio::awaitable<void> read_loop() {
        // Parse any bytes carried over from protocol detection first.
        if (!parse_available())
            co_return;
        std::array<std::byte, 32 * 1024> buf; // no init: read_some fills [0,n)
        for (;;) {
            m_deadline = std::chrono::steady_clock::now() + m_limits.idle_timeout;
            auto [ec, n] = co_await m_transport->async_read_some(std::span<std::byte>{buf});
            if (ec)
                break; // EOF or transport error ends the connection
            m_recv_buf.append(reinterpret_cast<const char *>(buf.data()), n);
            if (!parse_available())
                break; // protocol error -> GOAWAY queued, stop
            flush();
        }
        co_return;
    }

    asio::awaitable<void> write_loop() {
        // Hoisted out of the loop deliberately. Swapping into a fresh local each
        // pass hands m_out's capacity to a string that dies at the end of that
        // pass, so m_out regrew from nothing on every write (a malloc per pass,
        // for the life of the connection). With the buffer held here the two
        // strings trade capacities instead, and neither reallocates again.
        std::string chunk;
        for (;;) {
            fill_data_frames(); // frame as much stream DATA as flow control allows
            while (!m_out.empty()) {
                chunk.clear();
                chunk.swap(m_out);
                // Writing is connection activity: refresh the idle deadline so the
                // watchdog does not reap a connection that is busy sending (e.g.
                // streaming a large response to a slow client).
                m_deadline = std::chrono::steady_clock::now() + m_limits.idle_timeout;
                // Composed async_write: whole buffer or error, no partial-write loop.
                auto [ec, n] =
                    co_await m_transport->async_write(std::as_bytes(std::span<const char>{chunk.data(), chunk.size()}));
                (void)n;
                if (ec)
                    co_return;
                fill_data_frames(); // a handler may have queued more while we wrote
            }
            // Idle: no stream is active on this connection right now. An
            // xhttp/stream-one client pools many such connections (Go/xray keeps
            // tens alive with keepalive PINGs), and each would otherwise pin its
            // output buffers' capacity (m_out + chunk, hundreds of KiB after a
            // large response) for as long as it is held - a per-connection floor
            // that grows with the pool. Give the capacity back between requests;
            // the next response reallocates. gated on an empty stream table so an
            // in-flight response never pays a realloc per chunk.
            if (m_streams.empty()) {
                std::string().swap(m_out);
                std::string().swap(chunk);
            }
            if (m_goaway_sent && streams_all_done())
                co_return;
            auto [ec] = co_await m_notify.async_receive(asio::as_tuple(asio::use_awaitable));
            if (ec)
                co_return;
        }
    }

    asio::awaitable<void> watchdog() {
        asio::steady_timer timer{m_executor};
        for (;;) {
            timer.expires_at(m_deadline);
            co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
            if (std::chrono::steady_clock::now() >= m_deadline)
                break; // idle timeout elapsed
        }
        co_return;
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
            if (m_streams.contains(hdr.stream_id)) {
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
    bool refuse_stream(const codec::H2FrameHeader &hdr, std::size_t payload_size, std::uint32_t error_code_value) {
        if (hdr.stream_id > m_next_peer_stream_id)
            m_next_peer_stream_id = hdr.stream_id;
        reset_stream(hdr.stream_id, error_code_value);
        if (!hdr.has_flag(codec::H2_FLAG_END_HEADERS)) {
            m_continuation_stream = hdr.stream_id;
            m_continuation_discard = true;
            m_continuation_discard_bytes = payload_size;
        }
        return true;
    }

    bool on_headers(const codec::H2FrameHeader &hdr, std::string_view payload) {
        if (hdr.stream_id == 0) {
            go_away(codec::H2_PROTOCOL_ERROR);
            return false;
        }
        // RFC 9113 §5.1.1: a client opens odd-numbered streams, each higher than
        // every stream it has opened before. A HEADERS on anything else - an even
        // (server-initiated) id, or an id that goes backwards - is a connection
        // error; tolerating it would let a peer open streams it must not.
        if (m_streams.find(hdr.stream_id) == m_streams.end() && !is_new_client_stream(hdr.stream_id)) {
            SIMPLE_HTTP_ERROR_LOG("h2: client opened stream {} (highest so far {}); GOAWAY", hdr.stream_id,
                                  m_next_peer_stream_id);
            go_away(codec::H2_PROTOCOL_ERROR);
            return false;
        }
        bool ok = false;
        std::string_view block =
            strip_padding(payload, hdr.has_flag(codec::H2_FLAG_PADDED), hdr.has_flag(codec::H2_FLAG_PRIORITY), ok);
        if (!ok) {
            go_away(codec::H2_PROTOCOL_ERROR);
            return false;
        }

        // §5.3.1: a stream cannot depend on itself. The priority prefix was just
        // skipped over, so the dependency it held is re-read here for the check.
        if (hdr.has_flag(codec::H2_FLAG_PRIORITY)) {
            const std::size_t off = hdr.has_flag(codec::H2_FLAG_PADDED) ? 1 : 0;
            if ((codec::read_u32(payload, off) & 0x7FFFFFFFu) == hdr.stream_id) {
                return refuse_stream(hdr, payload.size(), codec::H2_PROTOCOL_ERROR);
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
            return refuse_stream(hdr, payload.size(), codec::H2_REFUSED_STREAM);
        }
        if (header_block_too_big(m_streams.contains(hdr.stream_id) ? m_streams.at(hdr.stream_id).header_block.size()
                                                                   : 0,
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
                mark_ws_remote_end(it->second); // extended CONNECT: the head ends the tunnel's read side
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
            // The stream was refused at its HEADERS: consume the rest of the
            // split head without buffering or decoding it. The byte bound keeps
            // a peer from turning the discard path into a CONTINUATION flood.
            const bool too_big = payload.size() > m_limits.max_header_bytes ||
                                 m_continuation_discard_bytes > m_limits.max_header_bytes - payload.size();
            if (too_big) {
                SIMPLE_HTTP_ERROR_LOG("h2: refused header block exceeds {} bytes (stream={})",
                                      m_limits.max_header_bytes, hdr.stream_id);
                go_away(codec::H2_ENHANCE_YOUR_CALM);
                return false;
            }
            m_continuation_discard_bytes += payload.size();
            if (hdr.has_flag(codec::H2_FLAG_END_HEADERS)) {
                m_continuation_stream = 0;
                m_continuation_discard = false;
                m_continuation_discard_bytes = 0;
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
                mark_ws_remote_end(it2->second); // END_STREAM arrived on the (split) head
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
            if (contains_ctl(f.name) || contains_ctl(f.value)) {
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
                    st.request->set_method_token(f.value);
                } else if (f.name == ":path") {
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
            st.request->mutable_headers().add_lower(std::move(f.name), std::move(f.value));
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
            // `:protocol`, must be a CONNECT, and — unlike a plain CONNECT —
            // carries `:scheme` and `:path`. Only "websocket" is acted on; any
            // other value is left to normal dispatch (which answers 404).
            if (seen_protocol) {
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
            // that follows is then held to it — see on_data().
            if (auto declared = st.request->header("content-length")) {
                std::uint64_t value = 0;
                auto [ptr, ec] = std::from_chars(declared->data(), declared->data() + declared->size(), value);
                if (ec != std::errc{} || ptr != declared->data() + declared->size()) {
                    malformed("invalid content-length");
                    return true;
                }
                st.declared_content_length = static_cast<std::int64_t>(value);
            }
        }

        // :authority is HTTP/2's spelling of Host, and a compliant client sends
        // it with no Host field at all — so without this every h2 request reaches
        // handlers, and the reverse proxy's x-forwarded-host, host-less. Done
        // after the loop so a real Host field wins over the synthesized one.
        if (!authority.empty() && !st.request->mutable_headers().contains("host")) {
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

            // RFC 8441: on an extended-CONNECT stream DATA is tunnel bytes, not a
            // request body. Buffer it for the WebSocket transport and return the
            // flow-control credit only as the tunnel consumes it, so a stalled
            // handler cannot make the engine buffer without bound.
            if (st.websocket) {
                if (!data.empty()) {
                    if (st.ws_closed) {
                        // The tunnel is gone: refused (no ws route), a failed proxy
                        // handshake, or a handler that already returned. Nothing
                        // will ever read ws_inbox, so hand the connection credit
                        // straight back instead of banking bytes nobody drains -
                        // banked bytes keep the shared connection window debited
                        // for the rest of the connection's life. Measured: 1 MiB
                        // of DATA on a 404'd extended CONNECT left the connection
                        // unable to send any request body on *any* stream.
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
                    mark_ws_remote_end(st); // tunnel EOF; shared with the head-END_STREAM path
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
            // connection error; on a stream that has already closed it is a stream
            // error instead. Which one it is, is what m_next_peer_stream_id says.
            if (hdr.stream_id > m_next_peer_stream_id) {
                go_away(codec::H2_PROTOCOL_ERROR);
                return false;
            }
            reset_stream(hdr.stream_id, codec::H2_STREAM_CLOSED);
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
        // Wake a WebSocket reader parked on this stream, so its adapter observes
        // the stream's disappearance instead of waiting forever.
        if (it->second.ws_wake)
            it->second.ws_wake->close();
        // A stream reset while its handler was still queued (Rapid Reset): drop
        // the queue entry so the bound is not spent on a stream that is gone.
        // The handler itself, if one was already running, keeps its slot and
        // releases it via handler_finished().
        std::erase(m_pending_dispatch, stream_id);
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
        // RFC 8441: an extended-CONNECT WebSocket stream runs the ws middleware
        // chain and, if it reaches the terminal, becomes a tunnel.
        if (it->second.websocket) {
            dispatch_ws_stream(stream_id);
            return;
        }
        auto request = it->second.request;
        auto writer = it->second.writer;
        // Remember the method on the sink before the response starts: once a HEAD
        // response's HEADERS frame ends the stream, the entry is retired and the
        // stream table can no longer answer "was this a HEAD?".
        writer->set_head_request(request->method() == Method::Head);
        auto self = this->shared_from_this(); // keep engine alive across suspensions
        std::uint32_t sid = stream_id;
        asio::co_spawn(
            m_executor,
            [self, request, writer, sid]() -> asio::awaitable<void> {
                // Same wrapper as h1: see the note there. `writer` stays in the
                // engine's hands for stream bookkeeping; only the Response gets
                // the compressing view.
                auto response = std::make_shared<ResponseWriter>(
                    maybe_compress_writer(writer, self->m_executor, request, request->method() == Method::Head));
                try {
                    co_await self->m_dispatch(request, response, self->m_transport->tls_handle());
                } catch (const std::exception &e) {
                    SIMPLE_HTTP_ERROR_LOG("h2 handler(stream={}) threw: {}", sid, e.what());
                    self->reset_stream(sid, codec::H2_INTERNAL_ERROR);
                } catch (...) {
                    SIMPLE_HTTP_ERROR_LOG("h2 handler(stream={}) threw unknown exception", sid);
                    self->reset_stream(sid, codec::H2_INTERNAL_ERROR);
                }
                self->flush();
                self->handler_finished(sid);
                co_return;
            },
            asio::detached);
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
    // through Http2WsTransport. A nested class so it can reach the engine's
    // stream table directly.
    class WsStreamAdapter final : public H2WsStream {
      public:
        WsStreamAdapter(std::weak_ptr<Http2Engine> engine, std::uint32_t id, Executor exec)
            : m_engine(std::move(engine)), m_id(id), m_exec(std::move(exec)) {}

        asio::any_io_executor get_executor() override { return m_exec; }

        asio::awaitable<IoResult> read_some(std::span<std::byte> out) override {
            if (auto eng = m_engine.lock())
                co_return co_await eng->ws_stream_read(m_id, out);
            co_return IoResult{make_error_code(asio::error::operation_aborted), 0};
        }

        asio::awaitable<IoResult> write_seq(std::span<const ConstByteSpan> bufs) override {
            if (auto eng = m_engine.lock())
                co_return co_await eng->ws_stream_write(m_id, bufs);
            co_return IoResult{make_error_code(asio::error::operation_aborted), 0};
        }

        void close() override {
            if (auto eng = m_engine.lock())
                eng->ws_stream_close(m_id);
        }

      private:
        std::weak_ptr<Http2Engine> m_engine;
        std::uint32_t m_id;
        Executor m_exec;
    };

    // Dispatch an extended-CONNECT WebSocket stream: run the ws middleware chain
    // (global + group + per-route) and, if it reaches the terminal, upgrade. With
    // no matching ws route the stream is answered 404 and ended.
    void dispatch_ws_stream(std::uint32_t stream_id) {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end()) {
            // The stream vanished between start_handler and here: release the slot
            // taken for it, or the leak would stall the dispatch queue.
            handler_finished(stream_id);
            return;
        }
        auto request = it->second.request;
        auto writer = it->second.writer;
        const std::string path{request->path()};
        std::optional<WsHandlerFn> handler_fn;
        auto self = this->shared_from_this();
        if (m_ws_lookup)
            handler_fn = m_ws_lookup(path);
        // Nginx order: local exact ws route beats a proxy route, which beats a
        // local regex ws route.
        if (!handler_fn && m_ws_proxy_lookup) {
            if (auto target = m_ws_proxy_lookup(path)) {
                asio::co_spawn(
                    m_executor,
                    [self, request, stream_id, target = std::move(*target)]() mutable -> asio::awaitable<void> {
                        (void)co_await self->run_h2_ws_proxy(stream_id, std::move(request), std::move(target));
                        self->handler_finished(stream_id);
                    },
                    asio::detached);
                return;
            }
        }
        if (!handler_fn && m_ws_regex_lookup)
            handler_fn = m_ws_regex_lookup(path);
        if (!handler_fn) {
            asio::co_spawn(
                m_executor,
                [self, writer, stream_id]() -> asio::awaitable<void> {
                    (void)co_await writer->send_bodyless(404, Headers{});
                    // There is no tunnel on this stream, so close its read side:
                    // inbound DATA then returns its flow-control credit instead of
                    // being banked for a reader that will never exist (see
                    // on_data).
                    self->ws_stream_close(stream_id);
                    self->handler_finished(stream_id);
                },
                asio::detached);
            return;
        }
        asio::co_spawn(
            m_executor,
            [self, request, writer, stream_id, handler = std::move(*handler_fn)]() mutable -> asio::awaitable<void> {
                auto response = std::make_shared<ResponseWriter>(writer);
                auto upgrade = [self, stream_id](std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res,
                                                 WsHandler h) -> asio::awaitable<bool> {
                    co_return co_await self->do_ws_upgrade(stream_id, std::move(req), std::move(res), std::move(h));
                };
                try {
                    (void)co_await handler(request, response, self->m_transport->tls_handle(), std::move(upgrade));
                } catch (const std::exception &e) {
                    SIMPLE_HTTP_ERROR_LOG("h2 ws handler(stream={}) threw: {}", stream_id, e.what());
                    self->reset_stream(stream_id, codec::H2_INTERNAL_ERROR);
                } catch (...) {
                    SIMPLE_HTTP_ERROR_LOG("h2 ws handler(stream={}) threw unknown exception", stream_id);
                    self->reset_stream(stream_id, codec::H2_INTERNAL_ERROR);
                }
                self->handler_finished(stream_id);
                co_return;
            },
            asio::detached);
    }

    // The ws middleware chain's terminal: accept the tunnel with a 200 head and
    // run the handler over a WsBackendImpl backed by this stream.
    asio::awaitable<bool> do_ws_upgrade(std::uint32_t stream_id, std::shared_ptr<Request> request,
                                        std::shared_ptr<ResponseWriter> res, WsHandler handler) {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end())
            co_return false;
        auto sink = it->second.writer;

        // permessage-deflate (RFC 7692) is negotiated with the same headers as
        // over HTTP/1.1, carried inside the extended CONNECT request/response.
        Headers response_headers = res->headers();
        WsDeflateConfig deflate;
        if (m_limits.ws_compression) {
            const auto offered = request->header("sec-websocket-extensions").value_or(std::string_view{});
            if (auto parsed = ws_parse_deflate_offer(offered)) {
                deflate = *parsed;
                response_headers.add_lower("sec-websocket-extensions", ws_deflate_response_value(deflate));
            }
        }

        // RFC 8441 §4: a 2xx response *is* the acceptance; there is no
        // Sec-WebSocket-Accept over HTTP/2, and the stream stays open.
        if (auto ec = co_await sink->send_headers(200, std::move(response_headers)); ec)
            co_return false;

        auto adapter = std::make_shared<WsStreamAdapter>(this->weak_from_this(), stream_id, m_executor);
        auto transport = std::make_shared<Http2WsTransport>(adapter);
        auto backend = std::make_shared<WsBackendImpl<Http2WsTransport>>(transport, m_limits.max_body_bytes,
                                                                         m_limits.idle_timeout, /*expect_masked=*/true,
                                                                         m_limits.ws_read_ahead_bytes, deflate);
        auto ws = std::make_shared<WebSocket>(std::move(backend));
        asio::co_spawn(m_executor, ws->run_writer(), asio::detached);
        try {
            co_await handler(std::move(request), ws); // copy: keep ours for the graceful close
        } catch (const std::exception &e) {
            SIMPLE_HTTP_ERROR_LOG("h2 ws handler(stream={}) threw: {}", stream_id, e.what());
        } catch (...) {
            SIMPLE_HTTP_ERROR_LOG("h2 ws handler(stream={}) threw unknown exception", stream_id);
        }
        (void)co_await ws->close();
        co_return true;
    }

    // --- HTTP/2 WebSocket reverse proxy (RFC 8441 frontend, HTTP/1.1 backend) ---

    // Byte-level WebSocket reverse proxy over HTTP/2. The frontend handshake is
    // an extended CONNECT (already matched), so we synthesize an ordinary
    // HTTP/1.1 upgrade to the backend, wait for its 101, answer the h2 stream
    // with 200, then splice bytes verbatim. Because the proxy is the *client* on
    // both sides, masking, fragmentation, control frames and permessage-deflate
    // pass through unchanged.
    asio::awaitable<bool> run_h2_ws_proxy(std::uint32_t stream_id, std::shared_ptr<Request> request,
                                          WsProxyTarget target) {
        using namespace asio::experimental::awaitable_operators;
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end())
            co_return false;
        auto sink = it->second.writer;
        const auto idle = m_limits.idle_timeout;

        const auto peer = m_transport->peer();
        std::string peer_address = address_string_if_specified(peer.address());
        const bool client_is_tls = m_transport->tls_handle().has_value();

        auto fail = [this, sink, stream_id](int status, Headers headers = {}) -> asio::awaitable<void> {
            (void)co_await sink->send_bodyless(status, std::move(headers));
            // The tunnel is not happening, whatever the reason: close its read
            // side so inbound DATA is credited back rather than banked (on_data).
            ws_stream_close(stream_id);
            co_return;
        };

        auto connected = co_await detail::ws_proxy_connect(m_executor, target);
        if (!connected) {
            const error_code ec = connected.error();
            SIMPLE_HTTP_ERROR_LOG("h2 ws-proxy connect {}:{} failed (client {}): {}", target.host, target.port,
                                  address_string(peer.address()), ec.message());
            co_await fail(502);
            co_return false;
        }
        auto backend = std::move(*connected);

        auto close_backend = [backend]() { detail::ws_proxy_close(*backend); };

        // Rewrite the path and keep the client's query string — see run_ws_proxy
        // (h1) for why dropping it breaks session-carrying endpoints.
        // The h2 frontend only ever had a '?' when the query was non-empty.
        const auto inbound_query = request->query();
        auto replay = detail::ws_proxy_replay_target(
            target.rewrite_path, request->path(),
            inbound_query.empty() ? std::nullopt : std::optional<std::string_view>{inbound_query});
        if (!replay) {
            SIMPLE_HTTP_ERROR_LOG("h2 ws-proxy: refusing a request target with CR/LF/NUL (client {})",
                                  address_string(peer.address()));
            close_backend();
            co_await fail(400);
            co_return false;
        }
        std::string replay_target = std::move(*replay);

        const std::string key = detail::make_ws_client_key();
        const auto inbound_host = request->header("host");
        std::string head =
            detail::build_ws_upgrade_head(request->headers(), inbound_host ? *inbound_host : std::string_view{},
                                          replay_target, target, peer_address, client_is_tls, key);
        {
            auto [wec, w] = co_await asio::async_write(*backend, asio::buffer(head.data(), head.size()),
                                                       asio::as_tuple(asio::use_awaitable));
            (void)w;
            if (wec) {
                SIMPLE_HTTP_ERROR_LOG("h2 ws-proxy backend handshake write failed (client {}): {}",
                                      address_string(peer.address()), wec.message());
                close_backend();
                co_await fail(502);
                co_return false;
            }
        }

        auto reply = co_await detail::ws_proxy_read_reply(*backend, m_limits.max_header_bytes, target.connect_timeout);
        if (!reply) {
            SIMPLE_HTTP_ERROR_LOG("h2 ws-proxy backend handshake failed (client {}): {}",
                                  address_string(peer.address()), reply.error().message());
            close_backend();
            co_await fail(502);
            co_return false;
        }

        if (reply->head.status != 101) {
            // Surface the backend's actual refusal status and end-to-end fields
            // (WWW-Authenticate, Location, …), not an invented 502. The body is
            // not relayed: an h2 extended-CONNECT response is terminated at the
            // headers here, and the peer's status is the actionable part.
            Headers response_headers;
            for (const auto &[name, value] : reply->head.headers) {
                if (is_hop_by_hop_field(name) || name == "content-length" || name == "transfer-encoding")
                    continue;
                if (contains_ctl(name) || contains_ctl(value))
                    continue;
                response_headers.add_lower(name, value);
            }
            SIMPLE_HTTP_ERROR_LOG("h2 ws-proxy backend refused the upgrade with {} (client {})", reply->head.status,
                                  address_string(peer.address()));
            close_backend();
            co_await fail(reply->head.status, std::move(response_headers));
            co_return false;
        }

        if (!detail::ws_validate_101(reply->head, ws_accept_key(key))) {
            SIMPLE_HTTP_ERROR_LOG("h2 ws-proxy backend answered 101 with a mismatched Upgrade/accept (client {})",
                                  address_string(peer.address()));
            close_backend();
            co_await fail(502);
            co_return false;
        }

        // Forward the backend's negotiated fields into the h2 200, minus the
        // HTTP/1.1-only / hop-by-hop ones (the h2 client must not see Upgrade,
        // Connection or Sec-WebSocket-Accept).
        Headers response_headers;
        for (const auto &[name, value] : reply->head.headers) {
            if (is_hop_by_hop_field(name) || name == "sec-websocket-accept" || name == "content-length" ||
                name == "transfer-encoding")
                continue;
            if (contains_ctl(name) || contains_ctl(value))
                continue;
            response_headers.add_lower(name, value);
        }
        if (auto ec = co_await sink->send_headers(200, std::move(response_headers)); ec) {
            close_backend();
            co_return false;
        }

        std::string initial{std::move(reply->initial)};
        auto deadline = std::chrono::steady_clock::now() + idle;

        auto backend_to_client = [&]() -> asio::awaitable<void> {
            if (!initial.empty()) {
                if (auto ec = co_await ws_stream_write_bytes(
                        stream_id, std::as_bytes(std::span<const char>{initial.data(), initial.size()}));
                    ec)
                    co_return;
                initial.clear();
            }
            std::array<std::byte, 16384> buf{};
            for (;;) {
                auto [rec2, n] = co_await backend->async_read_some(asio::buffer(buf.data(), buf.size()),
                                                                   asio::as_tuple(asio::use_awaitable));
                if (rec2 || n == 0) {
                    // The backend is done sending. End the tunnel's read side,
                    // which also wakes client_to_backend; unlike the old `||`
                    // race this happens only after the direction really ended.
                    ws_stream_close(stream_id);
                    break;
                }
                deadline = std::chrono::steady_clock::now() + idle;
                if (auto ec = co_await ws_stream_write_bytes(stream_id, std::span<const std::byte>{buf.data(), n}); ec)
                    break;
                deadline = std::chrono::steady_clock::now() + idle;
            }
        };
        auto client_to_backend = [&]() -> asio::awaitable<void> {
            std::array<std::byte, 16384> buf{};
            for (;;) {
                auto [rec2, n] = co_await ws_stream_read(stream_id, std::span<std::byte>{buf.data(), buf.size()});
                if (rec2 || n == 0) {
                    // Client half-close (END_STREAM): stop sending to the
                    // backend, but leave the opposite direction running so the
                    // backend's Close frame and trailing messages still arrive.
                    error_code sec;
                    backend->shutdown(asio::ip::tcp::socket::shutdown_send, sec);
                    break;
                }
                deadline = std::chrono::steady_clock::now() + idle;
                auto [wec, w] = co_await asio::async_write(*backend, asio::buffer(buf.data(), n),
                                                           asio::as_tuple(asio::use_awaitable));
                (void)w;
                if (wec)
                    break;
                deadline = std::chrono::steady_clock::now() + idle;
            }
        };
        // Go's io.Copy pair: both directions run to completion (a half-close in
        // one does not cancel the other), with the idle watchdog as the only
        // early exit. This matches the h1 tunnel, which fixed the same mistake.
        if (idle.count() > 0) {
            auto outcome = co_await ((backend_to_client() && client_to_backend()) ||
                                     detail::ws_tunnel_idle_guard(m_executor, deadline));
            if (const auto *timed_out = std::get_if<bool>(&outcome); timed_out && *timed_out) {
                SIMPLE_HTTP_ERROR_LOG("h2 ws-proxy tunnel idle for {}s (client {})",
                                      std::chrono::duration_cast<std::chrono::seconds>(idle).count(), peer_address);
            }
        } else {
            co_await (backend_to_client() && client_to_backend());
        }
        close_backend();
        ws_stream_close(stream_id);
        co_return true;
    }

    // Raw byte write onto the tunnel stream (the proxy's backend->client half):
    // frames the bytes as DATA, honoring flow control.
    asio::awaitable<error_code> ws_stream_write_bytes(std::uint32_t id, std::span<const std::byte> data) {
        auto it = m_streams.find(id);
        if (it == m_streams.end() || it->second.end_stream_sent)
            co_return make_error_code(asio::error::not_connected);
        if (auto ec = co_await await_out_space(id); ec)
            co_return ec;
        enqueue_body(id, std::string(reinterpret_cast<const char *>(data.data()), data.size()), /*last=*/false);
        co_return error_code{};
    }

    // The peer ended its half of a stream (END_STREAM). For a WebSocket tunnel
    // that is the reader's EOF, and it has to be recorded wherever the end can
    // arrive — a DATA frame, or the request head itself. On an extended CONNECT
    // whose HEADERS carry END_STREAM nothing else will ever set it: RFC 9113 §5.1
    // forbids further DATA on a half-closed(remote) stream, so the tunnel reader
    // would park on ws_wake until the idle watchdog fired (measured: the peer
    // half-closing at CONNECT time got no reply for 120s).
    static void mark_ws_remote_end(Stream &st) {
        if (!st.websocket)
            return;
        st.ws_eof = true;
        if (st.ws_wake)
            (void)st.ws_wake->try_send(error_code{});
    }

    // Inbound tunnel read: hands the ws backend buffered DATA bytes and returns
    // flow-control credit for exactly what it consumed. Parks on ws_wake until
    // bytes arrive, the peer ends its half, or the stream is torn down.
    asio::awaitable<IoResult> ws_stream_read(std::uint32_t id, std::span<std::byte> out) {
        for (;;) {
            auto it = m_streams.find(id);
            if (it == m_streams.end())
                co_return IoResult{make_error_code(asio::error::connection_reset), 0};
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
                co_return IoResult{error_code{}, n};
            }
            if (st.ws_closed)
                co_return IoResult{make_error_code(asio::error::operation_aborted), 0};
            if (st.ws_eof)
                co_return IoResult{make_error_code(asio::error::eof), 0};
            if (!st.ws_wake)
                st.ws_wake = std::make_shared<asio::experimental::channel<void(error_code)>>(m_executor, 1);
            auto wake = st.ws_wake;
            // Re-look-up after every wake: the stream entry may have been erased
            // while we were parked.
            (void)co_await wake->async_receive(asio::as_tuple(asio::use_awaitable));
        }
    }

    // Outbound tunnel write: frames every buffer as DATA on this stream, waiting
    // on the same backpressure the response path uses.
    asio::awaitable<IoResult> ws_stream_write(std::uint32_t id, std::span<const ConstByteSpan> bufs) {
        std::size_t total = 0;
        for (const auto &b : bufs)
            total += b.size();
        {
            auto it = m_streams.find(id);
            if (it == m_streams.end() || it->second.end_stream_sent)
                co_return IoResult{make_error_code(asio::error::not_connected), 0};
        }
        if (auto ec = co_await await_out_space(id); ec)
            co_return IoResult{ec, 0};
        std::string data;
        data.reserve(total);
        for (const auto &b : bufs)
            data.append(reinterpret_cast<const char *>(b.data()), b.size());
        enqueue_body(id, std::move(data), /*last=*/false);
        co_return IoResult{error_code{}, total};
    }

    // Ends the tunnel with END_STREAM and wakes any parked reader. Never closes
    // the rest of the HTTP/2 connection.
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

    // --- members (touched only on the connection executor) ---
    std::shared_ptr<Transport> m_transport;
    Executor m_executor;
    // Thread the connection is bound to (model A). Used to run body-consumption
    // bookkeeping inline instead of posting it when the reader is already here.
    const std::thread::id m_thread_id{std::this_thread::get_id()};
    asio::experimental::channel<void(error_code)> m_notify;
    EngineLimits m_limits;
    std::chrono::steady_clock::time_point m_deadline{};

    Dispatcher m_dispatch;
    WsLookup m_ws_lookup;            // local exact ws routes (RFC 8441), empty if none
    WsLookup m_ws_regex_lookup;      // local regex ws routes, empty if none
    WsProxyLookup m_ws_proxy_lookup; // byte-level ws reverse-proxy routes, empty if none
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

    bool m_alive = true;
    bool m_goaway_sent = false;
    bool m_preface_consumed = false; // client connection preface stripped yet?

    friend class Http2ResponseSink<Transport>;
}; // class Http2Engine

} // namespace simple_http
