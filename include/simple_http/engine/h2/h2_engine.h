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
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../../core/base64.h"
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
        const bool head = eng->method_is_head(m_stream_id);
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
        eng->submit_headers(m_stream_id, status, headers,
                            /*end_stream=*/eng->method_is_head(m_stream_id));
        eng->flush();
        co_return error_code{};
    }

    asio::awaitable<error_code> send_chunk(std::string data) override {
        co_await hop();
        auto eng = m_engine.lock();
        if (!eng || !eng->alive() || !eng->stream_writable(m_stream_id)) {
            co_return make_error_code(asio::error::not_connected);
        }
        if (eng->method_is_head(m_stream_id))
            co_return error_code{}; // HEAD: no body
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
        if (!eng || !eng->alive() || !eng->stream_writable(m_stream_id)) {
            co_return make_error_code(asio::error::not_connected);
        }
        if (eng->method_is_head(m_stream_id))
            co_return error_code{}; // HEAD: no body
        if (auto ec = co_await eng->await_out_space(m_stream_id); ec)
            co_return ec;
        eng->enqueue_body(m_stream_id, std::move(data), /*last=*/true);
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
        co_await hop(); // reset_stream() touches the same table
        if (auto eng = m_engine.lock()) {
            eng->reset_stream(m_stream_id, codec::H2_CANCEL);
        }
    }
    Version version() const override { return Version::Http2; }

  private:
    asio::awaitable<void> hop() const { co_await asio::dispatch(asio::bind_executor(m_executor, asio::use_awaitable)); }

    std::weak_ptr<Http2Engine<Transport>> m_engine;
    std::uint32_t m_stream_id;
    Executor m_executor;
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
            co_return;
        }
        apply_settings_payload(settings);
        if (m_goaway_sent)
            co_return; // the h2c HTTP2-Settings were rejected
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
        m_next_peer_stream_id = 1; // the upgraded request's id is the highest seen so far
        dispatch_stream(1);

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
        std::string block;
        codec::hpack_append_status(block, status);
        for (const auto &[name, value] : headers) {
            if (name == "connection" || name == "transfer-encoding" || name == "keep-alive" || name == "upgrade" ||
                name == "proxy-connection") {
                continue; // hop-by-hop headers are forbidden in HTTP/2 (RFC 7540
                          // §8.1.2.2)
            }
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
        // A header block larger than the peer's advertised frame size must be split
        // over HEADERS + CONTINUATION frames (RFC 9113 §6.2/§6.10): a compliant
        // peer answers a larger frame with FRAME_SIZE_ERROR and closes the
        // connection.
        const std::size_t limit =
            std::max<std::size_t>(1, std::min<std::size_t>(m_peer_max_frame_size, m_limits.h2_max_frame_size));
        std::size_t off = 0;
        bool first = true;
        do {
            const std::size_t take = std::min(limit, block.size() - off);
            const bool last = (off + take == block.size());
            std::uint8_t flags = last ? codec::H2_FLAG_END_HEADERS : 0;
            if (first) {
                if (end_stream)
                    flags |= codec::H2_FLAG_END_STREAM;
                append_frame(codec::H2FrameType::Headers, flags, stream_id, std::string_view{block}.substr(off, take));
            } else {
                append_frame(codec::H2FrameType::Continuation, flags, stream_id,
                             std::string_view{block}.substr(off, take));
            }
            first = false;
            off += take;
        } while (off < block.size());

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
        append_u32(payload, error_code_value);
        append_frame(codec::H2FrameType::RstStream, 0, stream_id, payload);
        // A handler may already be parked in body().read() (dispatch runs at
        // END_HEADERS, before the body is complete). The stream is going away
        // and nothing will feed that channel again, so fail it rather than leave
        // the handler waiting forever — which would also keep the engine (held
        // by the handler's shared_from_this()) and the whole connection alive.
        // fail() is a no-op once the body terminated normally, so this cannot
        // rewrite a completed upload into an error.
        if (auto it = m_streams.find(stream_id); it != m_streams.end())
            (void)it->second.request->body().fail(make_error_code(asio::error::connection_reset));
        erase_stream(stream_id);
        flush();
    }

  private:
    struct Stream {
        std::shared_ptr<Request> request;
        std::shared_ptr<Http2ResponseSink<Transport>> writer;
        std::string header_block; // accumulates HEADERS + CONTINUATION

        // Outbound response body: a queue of chunks the write loop drains into
        // flow-controlled DATA frames. out_offset marks how much of the front
        // chunk has already been framed (so partial sends need no substr copy).
        std::deque<std::string> out_queue;
        std::size_t out_offset = 0;
        bool out_finished = false; // handler signalled end-of-body (send/send_last)

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
        bool half_closed_remote = false; // client sent END_STREAM
        bool end_stream_sent = false;    // our terminating DATA (END_STREAM) emitted

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
        std::deque<std::string> paused_frames;
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

        bool out_drained() const { return out_finished && out_queue.empty(); }
    };

    // --- byte helpers ---

    static void append_u32(std::string &out, std::uint32_t v) {
        out.push_back(static_cast<char>((v >> 24) & 0xFF));
        out.push_back(static_cast<char>((v >> 16) & 0xFF));
        out.push_back(static_cast<char>((v >> 8) & 0xFF));
        out.push_back(static_cast<char>(v & 0xFF));
    }

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
            append_u32(payload, value);
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
                for (auto &[sid, st] : m_streams)
                    st.send_window += delta;
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
            st.request->body().set_on_consumed([weak, exec, stream_id](std::size_t n) {
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
        co_await (read_loop() || write_loop() || watchdog());
        m_alive = false;
        // Release every producer still parked on outbound backpressure: the write
        // loop is gone, so nothing will ever drain their queues. Also fail the
        // request body of every stream still in the table: a handler parked in
        // body().read() (dispatched at END_HEADERS) must observe the connection's
        // death, or it waits forever and keeps this engine alive with it.
        for (auto &entry : m_streams) {
            if (entry.second.request)
                (void)entry.second.request->body().fail(make_error_code(asio::error::connection_reset));
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
            append_u32(payload, m_next_peer_stream_id);
            append_u32(payload, m_goaway_error);
            append_frame(codec::H2FrameType::Goaway, 0, 0, payload);
        }
        // Flush with a deadline: a peer that stopped reading must not keep the
        // connection (and this coroutine) alive.
        co_await (flush_pending_output() || flush_deadline());
        m_transport->close();
        co_return;
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
        for (;;) {
            fill_data_frames(); // frame as much stream DATA as flow control allows
            while (!m_out.empty()) {
                std::string chunk;
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
            reset_stream(hdr.stream_id, codec::H2_PROTOCOL_ERROR);
            return true;
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
                reset_stream(hdr.stream_id, codec::H2_PROTOCOL_ERROR);
                return true;
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
            reset_stream(hdr.stream_id, codec::H2_REFUSED_STREAM);
            return true;
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
        st.header_block.append(block);
        if (hdr.stream_id > m_next_peer_stream_id)
            m_next_peer_stream_id = hdr.stream_id;

        // If END_STREAM is set, the client sends no request body.
        bool end_stream = hdr.has_flag(codec::H2_FLAG_END_STREAM);

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
            if (!finish_header_block(hdr.stream_id, st))
                return false;
            // finish_header_block may have reset (and erased) a malformed stream, so
            // `st` must not be used again: re-look it up before dispatching.
            auto it = m_streams.find(hdr.stream_id);
            if (it == m_streams.end())
                return true;
            if (end_stream) {
                it->second.half_closed_remote = true;
                (void)it->second.request->body().finish();
            }
            // Dispatch as soon as the request head is complete. The handler runs
            // concurrently with any DATA frames still arriving, reading the body
            // through the async Body channel (see on_data). A body-less request
            // simply sees an immediately-finished body.
            dispatch_stream(hdr.stream_id);
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
            if (!finish_header_block(hdr.stream_id, it->second))
                return false;
            auto it2 = m_streams.find(hdr.stream_id); // may have been reset+erased
            if (it2 == m_streams.end())
                return true;
            if (it2->second.half_closed_remote) {
                (void)it2->second.request->body().finish();
            }
            dispatch_stream(hdr.stream_id);
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
    static bool is_connection_specific_field(std::string_view name) noexcept {
        return name == "connection" || name == "keep-alive" || name == "proxy-connection" ||
               name == "transfer-encoding" || name == "upgrade";
    }

    bool finish_header_block(std::uint32_t stream_id, Stream &st) {
        std::vector<codec::HpackHeader> fields;
        if (!m_decoder.decode(st.header_block, fields)) {
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
                m_conn_recv_window += frame_len;
                reset_stream(hdr.stream_id, codec::H2_STREAM_CLOSED);
                return true;
            }
            st.recv_window -= frame_len;
            if (st.recv_window < 0) {
                // Stream-level overrun: reset just this stream, keep the connection.
                reset_stream(hdr.stream_id, codec::H2_FLOW_CONTROL_ERROR);
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
                    st.recv_owed_conn += static_cast<std::int64_t>(data.size());
                    st.ws_inbox.append(data);
                    if (st.ws_wake)
                        (void)st.ws_wake->try_send(error_code{});
                }
                if (hdr.has_flag(codec::H2_FLAG_END_STREAM)) {
                    st.half_closed_remote = true;
                    st.ws_eof = true;
                    if (st.ws_wake)
                        (void)st.ws_wake->try_send(error_code{});
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
                    reset_stream(hdr.stream_id, codec::H2_PROTOCOL_ERROR);
                    return true;
                }
                // These delivered bytes owe connection-level credit until the
                // handler consumes them (or the stream is torn down).
                st.recv_owed_conn += static_cast<std::int64_t>(data.size());
                if (!st.paused_frames.empty() || !st.request->body().feed(std::string{data})) {
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
                if (st.paused_frames.empty()) {
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
        // §6.9: a zero increment is a PROTOCOL_ERROR — a connection error on
        // stream 0, a stream error anywhere else.
        if (increment == 0) {
            if (hdr.stream_id == 0) {
                go_away(codec::H2_PROTOCOL_ERROR);
                return false;
            }
            reset_stream(hdr.stream_id, codec::H2_PROTOCOL_ERROR);
            return true;
        }
        if (hdr.stream_id == 0) {
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
            // connection error, and on a closed one it is ignored.
            if (hdr.stream_id > m_next_peer_stream_id) {
                go_away(codec::H2_PROTOCOL_ERROR);
                return false;
            }
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
        append_u32(payload, delta);
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
        while (!it->second.paused_frames.empty()) {
            if (!it->second.request->body().feed(std::move(it->second.paused_frames.front())))
                break; // still full: try again on the next consume
            it->second.paused_frames.pop_front();
        }
        // If END_STREAM arrived while frames were parked, hand the end over now
        // that the queue is empty (see on_data): only then may Body::read() report
        // EOF without cutting the body short.
        if (it->second.finish_pending && it->second.paused_frames.empty()) {
            it->second.finish_pending = false;
            it->second.request->body().finish();
        }
        if (!m_recv_buf.empty()) {
            (void)parse_available();
        }
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
        append_u32(payload, m_next_peer_stream_id); // last stream id we processed
        append_u32(payload, error_code_value);
        append_frame(codec::H2FrameType::Goaway, 0, 0, payload);
        m_goaway_sent = true;
        flush();
    }

    // --- send side: turn queued DATA into flow-controlled DATA frames ---

    void fill_data_frames() {
        // HEADERS always precede DATA: submit_headers() runs synchronously on
        // this executor when the handler replies, appending the HEADERS frame to
        // m_out before any body bytes are queued here.
        std::vector<std::uint32_t> finished; // streams that just emitted END_STREAM
        for (auto &[stream_id, st] : m_streams) {
            if (st.end_stream_sent)
                continue;

            // Emit DATA while the connection and stream windows both allow it.
            for (;;) {
                if (st.out_queue.empty()) {
                    // No queued bytes. If the body is done, emit the terminating
                    // empty DATA frame with END_STREAM, once.
                    if (st.out_finished) {
                        append_frame(codec::H2FrameType::Data, codec::H2_FLAG_END_STREAM, stream_id, {});
                        st.end_stream_sent = true;
                        finished.push_back(stream_id);
                    }
                    break;
                }

                std::int64_t window = std::min<std::int64_t>(m_conn_send_window, st.send_window);
                if (window <= 0)
                    break; // blocked on flow control; resume on WINDOW_UPDATE

                std::string &front = st.out_queue.front();
                std::size_t available = front.size() - st.out_offset;
                std::size_t budget = static_cast<std::size_t>(
                    std::min<std::int64_t>(window, static_cast<std::int64_t>(std::min<std::uint32_t>(
                                                       m_peer_max_frame_size, m_limits.h2_max_frame_size))));
                std::size_t take = std::min(available, budget);
                if (take == 0) {
                    // No progress possible (frame-size limit configured to 0):
                    // stop rather than spin and grow m_out without bound.
                    break;
                }

                // Frame [out_offset, out_offset+take) of the front chunk directly,
                // with no intermediate copy.
                std::string_view piece{front.data() + st.out_offset, take};
                st.out_offset += take;
                bool front_done = (st.out_offset >= front.size());
                bool last = front_done && st.out_queue.size() == 1 && st.out_finished;

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
                    st.out_queue.pop_front();
                    st.out_offset = 0;
                }
                if (last) {
                    st.end_stream_sent = true;
                    finished.push_back(stream_id);
                    break;
                }
            }
        }
        // Retire any stream that is now complete in both directions (this may
        // erase entries, so it must happen after the iteration above).
        for (std::uint32_t sid : finished)
            maybe_complete_stream(sid);
    }

    // --- handler dispatch ---

    void dispatch_stream(std::uint32_t stream_id) {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end() || it->second.dispatched)
            return;
        it->second.dispatched = true;
        // RFC 8441: an extended-CONNECT WebSocket stream runs the ws middleware
        // chain and, if it reaches the terminal, becomes a tunnel.
        if (it->second.websocket) {
            dispatch_ws_stream(stream_id);
            return;
        }
        auto request = it->second.request;
        auto writer = it->second.writer;
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
                co_return;
            },
            asio::detached);
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
        if (it == m_streams.end())
            return;
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
                [self, writer]() -> asio::awaitable<void> { (void)co_await writer->send_bodyless(404, Headers{}); },
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

    // A Sec-WebSocket-Key the way a client makes one (base64 of 16 random
    // bytes). The proxy is the client to the backend, so it presents its own key;
    // the frontend's extended CONNECT carries none.
    static std::string make_ws_client_key() {
        std::array<unsigned char, 16> nonce{};
        std::random_device rd;
        for (auto &b : nonce)
            b = static_cast<unsigned char>(rd());
        return base64_encode(std::string_view{reinterpret_cast<const char *>(nonce.data()), nonce.size()});
    }

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

        auto fail = [sink](int status) -> asio::awaitable<void> {
            (void)co_await sink->send_bodyless(status, Headers{});
            co_return;
        };

        auto backend = std::make_shared<asio::ip::tcp::socket>(m_executor);
        asio::ip::tcp::resolver resolver{m_executor};
        auto [rec, endpoints] = co_await resolver.async_resolve(target.host, std::to_string(target.port),
                                                                asio::as_tuple(asio::use_awaitable));
        if (rec || endpoints.empty()) {
            SIMPLE_HTTP_ERROR_LOG("h2 ws-proxy resolve {}:{} failed: {}", target.host, target.port, rec.message());
            co_await fail(502);
            co_return false;
        }
        auto [cec, _] = co_await asio::async_connect(*backend, endpoints, asio::as_tuple(asio::use_awaitable));
        if (cec) {
            SIMPLE_HTTP_ERROR_LOG("h2 ws-proxy connect {}:{} failed: {}", target.host, target.port, cec.message());
            co_await fail(502);
            co_return false;
        }

        auto close_backend = [backend]() {
            error_code sec;
            backend->shutdown(asio::ip::tcp::socket::shutdown_both, sec);
            backend->close(sec);
        };

        // Rewrite the path and keep the client's query string — see run_ws_proxy
        // (h1) for why dropping it breaks session-carrying endpoints.
        std::string replay_target{target.rewrite_path};
        if (replay_target.empty())
            replay_target.assign(request->path());
        if (const auto q = request->query(); !q.empty()) {
            replay_target.push_back('?');
            replay_target.append(q);
        }
        if (replay_target.empty())
            replay_target = "/";

        const std::string key = make_ws_client_key();
        // Forward the client's own Host (HTTP/2's :authority, synthesized into a
        // `host` header by the engine). The h1 proxy replays the client's request
        // verbatim, so the backend sees Host == Origin; a backend that validates
        // Origin against Host (code-server, many others) rejects the upgrade
        // otherwise. Synthesizing the backend's address here was exactly that bug.
        const auto client_host = request->header("host");
        std::string head;
        head += "GET " + replay_target + " HTTP/1.1\r\n";
        head += "Host: ";
        head += client_host ? std::string{*client_host} : (target.host + ":" + std::to_string(target.port));
        head += "\r\n";
        head += "Upgrade: websocket\r\n";
        head += "Connection: Upgrade\r\n";
        head += "Sec-WebSocket-Key: " + key + "\r\n";
        head += "Sec-WebSocket-Version: 13\r\n";
        for (const auto &[name, value] : request->headers()) {
            if (name == "host" || name == "upgrade" || name == "connection" || name == "sec-websocket-key" ||
                name == "sec-websocket-version" || name == "content-length" || name == "transfer-encoding")
                continue;
            head += name;
            head += ": ";
            head += value;
            head += "\r\n";
        }
        head += "\r\n";
        {
            auto [wec, w] = co_await asio::async_write(*backend, asio::buffer(head.data(), head.size()),
                                                       asio::as_tuple(asio::use_awaitable));
            (void)w;
            if (wec) {
                SIMPLE_HTTP_ERROR_LOG("h2 ws-proxy backend handshake write failed: {}", wec.message());
                close_backend();
                co_await fail(502);
                co_return false;
            }
        }

        // Read the backend's response head (bounded), plus any bytes it already
        // sent after it (the first tunnel bytes).
        std::string reply;
        std::array<std::byte, 4096> rbuf{};
        while (reply.find("\r\n\r\n") == std::string::npos) {
            if (reply.size() > m_limits.max_header_bytes) {
                close_backend();
                co_await fail(502);
                co_return false;
            }
            auto [rec2, n] = co_await backend->async_read_some(asio::buffer(rbuf.data(), rbuf.size()),
                                                               asio::as_tuple(asio::use_awaitable));
            if (rec2 || n == 0) {
                close_backend();
                co_await fail(502);
                co_return false;
            }
            reply.append(reinterpret_cast<const char *>(rbuf.data()), n);
        }
        const std::size_t head_end = reply.find("\r\n\r\n") + 4;
        const std::string_view response_head{reply.data(), head_end};
        // The backend refused the upgrade. Surface its own status to the h2
        // client instead of inventing a 502 — the h1 proxy splices the backend's
        // response verbatim, so a browser behind an HTTP/2 frontend should see
        // the same 401/403/302 the backend actually answered.
        if (!response_head.starts_with("HTTP/1.1 101") && !response_head.starts_with("HTTP/1.0 101")) {
            SIMPLE_HTTP_ERROR_LOG("h2 ws-proxy backend refused the upgrade");
            close_backend();
            int status = 502;
            if (const std::size_t sp = response_head.find(' '); sp != std::string_view::npos) {
                std::from_chars(response_head.data() + sp + 1, response_head.data() + response_head.size(), status);
            }
            co_await fail(status);
            co_return false;
        }

        // Forward the backend's negotiated fields into the h2 200, minus the
        // HTTP/1.1-only / hop-by-hop ones (the h2 client must not see Upgrade,
        // Connection or Sec-WebSocket-Accept).
        Headers response_headers;
        const std::size_t line1 = response_head.find("\r\n") + 2;
        const std::string_view fields = response_head.substr(line1, head_end - 4 - line1);
        for (std::size_t pos = 0; pos < fields.size();) {
            const auto nl = fields.find("\r\n", pos);
            const std::string_view line =
                fields.substr(pos, nl == std::string_view::npos ? fields.size() - pos : nl - pos);
            pos = (nl == std::string_view::npos) ? fields.size() : nl + 2;
            const auto colon = line.find(':');
            if (colon == std::string_view::npos)
                continue;
            std::string name;
            for (char c : line.substr(0, colon))
                name.push_back(ascii_lower(c));
            std::string_view value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                value.remove_prefix(1);
            if (name == "upgrade" || name == "connection" || name == "sec-websocket-accept" ||
                name == "content-length" || name == "transfer-encoding")
                continue;
            response_headers.add_lower(std::move(name), std::string{value});
        }
        if (auto ec = co_await sink->send_headers(200, std::move(response_headers)); ec) {
            close_backend();
            co_return false;
        }

        std::string initial{reply.substr(head_end)};
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
                if (rec2 || n == 0)
                    break;
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
                if (rec2 || n == 0)
                    break;
                deadline = std::chrono::steady_clock::now() + idle;
                auto [wec, w] = co_await asio::async_write(*backend, asio::buffer(buf.data(), n),
                                                           asio::as_tuple(asio::use_awaitable));
                (void)w;
                if (wec)
                    break;
                deadline = std::chrono::steady_clock::now() + idle;
            }
        };
        auto idle_check = [&]() -> asio::awaitable<void> {
            if (idle.count() <= 0)
                co_return;
            for (;;) {
                if (std::chrono::steady_clock::now() >= deadline)
                    co_return;
                asio::steady_timer timer{m_executor};
                timer.expires_at(deadline);
                auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
                if (ec)
                    co_return;
            }
        };

        co_await (backend_to_client() || client_to_backend() || idle_check());
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
    asio::experimental::channel<void(error_code)> m_notify;
    EngineLimits m_limits;
    std::chrono::steady_clock::time_point m_deadline{};

    Dispatcher m_dispatch;
    WsLookup m_ws_lookup;            // local exact ws routes (RFC 8441), empty if none
    WsLookup m_ws_regex_lookup;      // local regex ws routes, empty if none
    WsProxyLookup m_ws_proxy_lookup; // byte-level ws reverse-proxy routes, empty if none
    codec::HpackDecoder m_decoder;   // connection-scoped (read loop only)
    std::unordered_map<std::uint32_t, Stream> m_streams;

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

    bool m_alive = true;
    bool m_goaway_sent = false;
    bool m_preface_consumed = false; // client connection preface stripped yet?

    friend class Http2ResponseSink<Transport>;
}; // class Http2Engine

} // namespace simple_http
