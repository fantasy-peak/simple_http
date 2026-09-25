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
// Http2ResponseWriter operation hops onto that executor before touching engine
// state, so a Response used from any thread stays safe.
//
// Lifetime: the engine is held by shared_ptr; handler coroutines capture it, so
// it (and the stream state) outlives every in-flight handler, including
// streaming handlers that suspend after run() would otherwise have returned.
// Http2ResponseWriter holds a weak_ptr and lock()s it per operation.

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>

#include "h2_frame.h"
#include "hpack_decode.h"

#include "../../core/base64.h"
#include "../../core/http_method.h"
#include "../../core/limits.h"
#include "../../core/logging.h"
#include "../../core/types.h"
#include "../../core/version.h"
#include "../../proto/headers.h"
#include "../../proto/request.h"
#include "../../proto/response.h"
#include "../../proto/response_writer.h"
#include "../../transport/transport.h"  // SslHandle, TransportLike
#include "../dispatcher.h"
#include "hpack_encoder.h"

namespace simple_http {

namespace asio = boost::asio;

template <TransportLike Transport>
class Http2Engine;

// Default HTTP/2 flow-control window (RFC 7540 §6.9.2): 65,535 octets.
inline constexpr std::int32_t kH2InitialWindow = 65535;
// Protocol default max frame size (RFC 7540 §6.5.2): the value a peer uses
// until it announces its own SETTINGS_MAX_FRAME_SIZE.
inline constexpr std::size_t kH2MaxFrameSize = 16384;

// Outbound backpressure watermarks for one stream's response queue. A producer
// (the reverse proxy streaming an upstream body, say) is parked once it has this
// many bytes queued but not yet framed, and released when the write loop has
// drained the queue back to the low mark.
//
// Without this a response body was simply accumulated: the handler ran ahead of
// the peer's flow-control window at full speed, so a client that stopped reading
// (window exhausted - congestion, a busy browser main thread, a mid-way RST)
// left the *entire* response sitting in memory, per stream. Measured with the
// 19 MiB code-server workbench.js: RSS went 12 MiB -> 30 MiB and stayed there
// while the client read nothing. The marks bound per-stream memory to roughly
// the high watermark regardless of response size.
inline constexpr std::size_t kOutHighWatermark = 1u << 20;   // 1 MiB: park the producer
inline constexpr std::size_t kOutLowWatermark = 256u << 10;  // 256 KiB: wake it again

// ResponseWriter for a single HTTP/2 stream. Holds a weak_ptr to the engine so
// it can be used safely from any thread and after the connection has closed.
template <TransportLike Transport>
class Http2ResponseWriter : public ResponseWriter {
  public:
    using Executor = decltype(std::declval<Transport&>().get_executor());

    Http2ResponseWriter(std::weak_ptr<Http2Engine<Transport>> engine, std::uint32_t stream_id, Executor exec)
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
        if (!head) eng->enqueue_body(m_stream_id, std::move(body), /*last=*/true);
        co_return error_code{};
    }

    asio::awaitable<error_code> send_bodyless(int status, Headers headers) override {
        co_await hop();
        auto eng = m_engine.lock();
        if (!eng || !eng->alive() || !eng->stream_writable(m_stream_id)) {
            co_return make_error_code(asio::error::not_connected);
        }
        // Nothing follows the HEADERS frame: END_STREAM ends the stream (RFC 9113 §8.1).
        eng->submit_headers(m_stream_id, status, headers, /*end_stream=*/true);
        co_return error_code{};
    }

    asio::awaitable<error_code> send_headers(int status, Headers headers) override {
        co_await hop();
        auto eng = m_engine.lock();
        if (!eng || !eng->alive() || !eng->stream_writable(m_stream_id)) {
            co_return make_error_code(asio::error::not_connected);
        }
        eng->submit_headers(m_stream_id, status, headers, /*end_stream=*/eng->method_is_head(m_stream_id));
        eng->flush();
        co_return error_code{};
    }

    asio::awaitable<error_code> send_chunk(std::string data) override {
        co_await hop();
        auto eng = m_engine.lock();
        if (!eng || !eng->alive() || !eng->stream_writable(m_stream_id)) {
            co_return make_error_code(asio::error::not_connected);
        }
        if (eng->method_is_head(m_stream_id)) co_return error_code{};  // HEAD: no body
        // Backpressure: block while this stream's queue is over the high mark, so
        // a fast producer (reverse proxy) is paced by the peer's window.
        if (auto ec = co_await eng->await_out_space(m_stream_id); ec) co_return ec;
        eng->enqueue_body(m_stream_id, std::move(data), /*last=*/false);
        co_return error_code{};
    }

    asio::awaitable<error_code> send_last(std::string data) override {
        co_await hop();
        auto eng = m_engine.lock();
        if (!eng || !eng->alive() || !eng->stream_writable(m_stream_id)) {
            co_return make_error_code(asio::error::not_connected);
        }
        if (eng->method_is_head(m_stream_id)) co_return error_code{};  // HEAD: no body
        if (auto ec = co_await eng->await_out_space(m_stream_id); ec) co_return ec;
        eng->enqueue_body(m_stream_id, std::move(data), /*last=*/true);
        co_return error_code{};
    }

    bool connected() const override {
        auto eng = m_engine.lock();
        // A reset/finished stream cannot take a response either: report the
        // response as unwritable so callers (e.g. a proxy's 502 path) stop
        // rather than trying to write onto a dead stream.
        return eng && eng->alive() && eng->stream_writable(m_stream_id);
    }
    void close() override {
        if (auto eng = m_engine.lock()) eng->reset_stream(m_stream_id, codec::H2_CANCEL);
    }
    Version version() const override { return Version::Http2; }

  private:
    asio::awaitable<void> hop() {
        co_await asio::dispatch(asio::bind_executor(m_executor, asio::use_awaitable));
    }

    std::weak_ptr<Http2Engine<Transport>> m_engine;
    std::uint32_t m_stream_id;
    Executor m_executor;
};

template <TransportLike Transport>
class Http2Engine : public std::enable_shared_from_this<Http2Engine<Transport>> {
  public:
    using Executor = decltype(std::declval<Transport&>().get_executor());

    explicit Http2Engine(std::shared_ptr<Transport> transport, EngineLimits limits = {})
        : m_transport(std::move(transport)),
          m_executor(m_transport->get_executor()),
          m_notify(m_executor, 1),
          m_limits(limits) {
        // Our advertised (receive) windows start at the configured initial size.
        m_conn_recv_window = m_limits.h2_initial_window;
    }

    Http2Engine(const Http2Engine&) = delete;
    Http2Engine& operator=(const Http2Engine&) = delete;

    // Serve the connection until it closes. `dispatch` runs a handler per stream.
    // `prior_knowledge_bytes` (if any) are bytes already consumed during protocol
    // detection — for prior-knowledge h2 they are the client preface and are fed
    // to the parser first.
    asio::awaitable<void> run(Dispatcher dispatch, std::string prior_knowledge_bytes = {}) {
        m_dispatch = std::move(dispatch);
        queue_settings();
        m_recv_buf = std::move(prior_knowledge_bytes);
        co_await serve_loops();
        co_return;
    }

    // Serve an h2c upgrade: the client's base64url HTTP2-Settings seed the peer
    // SETTINGS, and the original HTTP/1.1 request is replayed as stream 1.
    asio::awaitable<void> run_h2c(Dispatcher dispatch, const std::string& settings_b64, Method method,
                                  std::string target, Headers headers, std::string body) {
        m_dispatch = std::move(dispatch);
        // Apply the client's SETTINGS payload (raw settings frame body).
        std::string settings = base64_url_decode(settings_b64);
        apply_settings_payload(settings);
        if (m_goaway_sent) co_return;  // the h2c HTTP2-Settings were rejected
        queue_settings();

        // Seed stream 1 from the initial request and dispatch it.
        auto& st = ensure_stream(1);
        st.request->set_method(method);
        st.request->set_method_token(std::string{to_string(method)});
        st.request->set_target(std::move(target));
        st.request->mutable_headers() = std::move(headers);
        if (!body.empty()) (void)st.request->body().feed(std::move(body));
        (void)st.request->body().finish();
        m_next_peer_stream_id = 1;  // the upgraded request's id is the highest seen so far
        dispatch_stream(1);

        co_await serve_loops();
        co_return;
    }

    bool alive() const { return m_alive; }
    Executor get_executor() { return m_executor; }

    // Wake the write loop to drain pending frames / DATA.
    void flush() { (void)m_notify.try_send(error_code{}); }

    // --- called by Http2ResponseWriter (already hopped onto our executor) ---

    // Serialize a response HEADERS block for `stream_id` into the control-frame
    // output queue. Connection-specific headers illegal in HTTP/2 are dropped.
    void submit_headers(std::uint32_t stream_id, int status, const Headers& headers, bool end_stream) {
        // Never emit a header block on a stream that is gone (reset by the peer)
        // or already ended: RFC 9113 §5.1 makes a frame on a closed stream a
        // connection-level protocol error, so the peer answers with GOAWAY and
        // every other stream on the connection dies with it. DEFENCE IN DEPTH:
        // writers check first, this guards any other caller.
        if (!stream_writable(stream_id)) return;
        std::string block;
        codec::hpack_append_status(block, status);
        for (const auto& [name, value] : headers) {
            if (name == "connection" || name == "transfer-encoding" || name == "keep-alive" ||
                name == "upgrade" || name == "proxy-connection") {
                continue;  // hop-by-hop headers are forbidden in HTTP/2 (RFC 7540 §8.1.2.2)
            }
            codec::hpack_append_literal(block, name, value);
        }
        // A header block larger than the peer's advertised frame size must be split
        // over HEADERS + CONTINUATION frames (RFC 9113 §6.2/§6.10): a compliant peer
        // answers a larger frame with FRAME_SIZE_ERROR and closes the connection.
        const std::size_t limit =
            std::max<std::size_t>(1, std::min<std::size_t>(m_peer_max_frame_size, m_limits.h2_max_frame_size));
        std::size_t off = 0;
        bool first = true;
        do {
            const std::size_t take = std::min(limit, block.size() - off);
            const bool last = (off + take == block.size());
            std::uint8_t flags = last ? codec::H2_FLAG_END_HEADERS : 0;
            if (first) {
                if (end_stream) flags |= codec::H2_FLAG_END_STREAM;
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
        if (it == m_streams.end() || it->second.end_stream_sent) return;
        Stream& st = it->second;
        if (!data.empty()) {
            st.out_queued += data.size();
            st.out_queue.push_back(std::move(data));
        }
        if (last) st.out_finished = true;
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
    // sequence below cannot race the write loop (single-threaded model A).
    asio::awaitable<error_code> await_out_space(std::uint32_t stream_id) {
        for (;;) {
            auto it = m_streams.find(stream_id);
            if (it == m_streams.end()) co_return make_error_code(asio::error::operation_aborted);
            if (it->second.out_queued <= kOutHighWatermark) co_return error_code{};
            if (!it->second.out_space) {
                it->second.out_space =
                    std::make_shared<asio::experimental::concurrent_channel<void(error_code)>>(m_executor, 1);
            }
            // Keep the channel alive across the wait: the stream entry itself may
            // be erased (and the channel closed, waking us) while we are parked.
            auto space = it->second.out_space;
            auto [ec] = co_await space->async_receive(asio::as_tuple(asio::use_awaitable));
            if (ec) co_return make_error_code(asio::error::operation_aborted);
        }
    }

    // Abort a stream with the given error code (RST_STREAM).
    void reset_stream(std::uint32_t stream_id, std::uint32_t error_code_value) {
        std::string payload;
        append_u32(payload, error_code_value);
        append_frame(codec::H2FrameType::RstStream, 0, stream_id, payload);
        erase_stream(stream_id);
        flush();
    }

  private:
    struct Stream {
        std::shared_ptr<Request> request;
        std::shared_ptr<Http2ResponseWriter<Transport>> writer;
        std::string header_block;  // accumulates HEADERS + CONTINUATION

        // Outbound response body: a queue of chunks the write loop drains into
        // flow-controlled DATA frames. out_offset marks how much of the front
        // chunk has already been framed (so partial sends need no substr copy).
        std::deque<std::string> out_queue;
        std::size_t out_offset = 0;
        bool out_finished = false;  // handler signalled end-of-body (send/send_last)

        // Bytes queued but not yet framed into DATA frames. Drives the producer
        // backpressure in await_out_space() (kOutHighWatermark/kOutLowWatermark).
        std::size_t out_queued = 0;
        // Wakes a producer parked on backpressure. Created lazily on the first
        // wait, closed on teardown (stream erased, connection ending) so a parked
        // producer is always released rather than stranded.
        std::shared_ptr<asio::experimental::concurrent_channel<void(error_code)>> out_space;

        std::uint32_t id = 0;                         // this stream's id (for WINDOW_UPDATE etc.)
        std::int64_t send_window = kH2InitialWindow;  // peer's advertised window for us (send side)
        std::int64_t recv_window = kH2InitialWindow;  // our window advertised to the peer (recv side)
        std::int64_t recv_pending = 0;                // consumed bytes awaiting a batched stream WINDOW_UPDATE
        std::int64_t recv_owed_conn = 0;              // delivered-but-unconsumed body bytes still owing connection credit
        bool dispatched = false;
        bool half_closed_remote = false;  // client sent END_STREAM
        bool end_stream_sent = false;     // our terminating DATA (END_STREAM) emitted

        bool out_drained() const { return out_finished && out_queue.empty(); }
    };

    // --- byte helpers ---

    static void append_u32(std::string& out, std::uint32_t v) {
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
        append_frame(codec::H2FrameType::Settings, 0, 0, payload);
    }

    // Apply a SETTINGS payload received from the peer (or from the h2c upgrade
    // HTTP2-Settings header). Only the settings that affect our send path are
    // acted on; others are accepted and ignored.
    void apply_settings_payload(std::string_view payload) {
        for (std::size_t i = 0; i + 6 <= payload.size(); i += 6) {
            std::uint16_t id = static_cast<std::uint16_t>(
                (static_cast<unsigned char>(payload[i]) << 8) | static_cast<unsigned char>(payload[i + 1]));
            std::uint32_t value = codec::read_u32(payload, i + 2);
            if (id == codec::H2_SETTINGS_INITIAL_WINDOW_SIZE) {
                std::int64_t delta = static_cast<std::int64_t>(value) - m_peer_initial_window;
                m_peer_initial_window = static_cast<std::int32_t>(value);
                for (auto& [sid, st] : m_streams) st.send_window += delta;
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
    bool is_new_client_stream(std::uint32_t id) const {
        return (id & 1u) == 1u && id > m_next_peer_stream_id;
    }

    // Whether adding `extra` bytes to a header block of `current` bytes would exceed
    // the bound we accept. Without it, HEADERS without END_HEADERS plus endless
    // CONTINUATION accumulate without limit (RFC 9113 §10.5.1).
    bool header_block_too_big(std::size_t current, std::size_t extra) const {
        return current + extra > m_limits.max_header_bytes;
    }

    // Whether the request on `stream_id` was a HEAD (its response carries no body).
    bool method_is_head(std::uint32_t stream_id) const {
        auto it = m_streams.find(stream_id);
        return it != m_streams.end() && it->second.request->method() == Method::Head;
    }

    // --- stream table ---

    Stream& ensure_stream(std::uint32_t stream_id) {
        auto [it, inserted] = m_streams.try_emplace(stream_id);
        if (inserted) {
            Stream& st = it->second;
            st.id = stream_id;
            st.request = std::make_shared<Request>(Version::Http2, m_executor, m_transport->peer());
            st.writer = std::make_shared<Http2ResponseWriter<Transport>>(this->weak_from_this(), stream_id, m_executor);
            st.send_window = m_peer_initial_window;
            st.recv_window = m_limits.h2_initial_window;  // our advertised per-stream window

            // Consumption-based flow control: when the handler reads body bytes,
            // hop back onto the connection executor and replenish credit for
            // exactly that many bytes. Un-consumed data therefore keeps the
            // window closed, bounding in-flight memory (no unbounded backlog).
            std::weak_ptr<Http2Engine> weak = this->weak_from_this();
            auto exec = m_executor;
            st.request->body().set_on_consumed([weak, exec, stream_id](std::size_t n) {
                asio::post(exec, [weak, stream_id, n]() {
                    if (auto eng = weak.lock()) eng->on_body_consumed(stream_id, n);
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
        if (m_preface_consumed) return true;
        if (m_recv_buf.size() < codec::kH2ClientPreface.size()) return true;  // need more bytes
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
        flush();  // push our initial SETTINGS
        co_await (read_loop() || write_loop() || watchdog());
        m_alive = false;
        // Release every producer still parked on outbound backpressure: the write
        // loop is gone, so nothing will ever drain their queues.
        for (auto& entry : m_streams) {
            if (entry.second.out_space) entry.second.out_space->close();
        }
        // Whichever loop finished first cancels the others, and a cancelled
        // async_write drops its buffer - so the GOAWAY the write loop had just taken
        // may never reach the wire, leaving the peer with a bare TCP close and no
        // explanation. Re-serialize it here and write it directly (RFC 9113 §6.8
        // allows an endpoint to send GOAWAY more than once, so re-sending is safe).
        if (m_goaway_sent) {
            m_out.clear();  // queued DATA is moot on a connection error
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
        if (m_out.empty()) co_return;
        auto bytes = std::as_bytes(std::span<const char>{m_out.data(), m_out.size()});
        auto [ec, n] = co_await m_transport->async_write(bytes);
        (void)n;
        m_out.clear();
        if (ec) co_return;
        co_return;
    }

    // Deadline for the teardown flush above: never let a non-reading peer keep the
    // connection (and its coroutine frame) alive.
    asio::awaitable<void> flush_deadline() {
        asio::steady_timer timer{m_executor};
        timer.expires_after(std::chrono::seconds(2));
        co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        co_return;
    }

    asio::awaitable<void> read_loop() {
        // Parse any bytes carried over from protocol detection first.
        if (!parse_available()) co_return;
        std::array<std::byte, 32 * 1024> buf;  // no init: read_some fills [0,n)
        for (;;) {
            m_deadline = std::chrono::steady_clock::now() + m_limits.idle_timeout;
            auto [ec, n] = co_await m_transport->async_read_some(std::span<std::byte>{buf});
            if (ec) break;  // EOF or transport error ends the connection
            m_recv_buf.append(reinterpret_cast<const char*>(buf.data()), n);
            if (!parse_available()) break;  // protocol error -> GOAWAY queued, stop
            flush();
        }
        co_return;
    }

    asio::awaitable<void> write_loop() {
        for (;;) {
            fill_data_frames();  // frame as much stream DATA as flow control allows
            while (!m_out.empty()) {
                std::string chunk;
                chunk.swap(m_out);
                // Writing is connection activity: refresh the idle deadline so the
                // watchdog does not reap a connection that is busy sending (e.g.
                // streaming a large response to a slow client).
                m_deadline = std::chrono::steady_clock::now() + m_limits.idle_timeout;
                // Composed async_write: whole buffer or error, no partial-write loop.
                auto [ec, n] = co_await m_transport->async_write(
                    std::as_bytes(std::span<const char>{chunk.data(), chunk.size()}));
                (void)n;
                if (ec) co_return;
                fill_data_frames();  // a handler may have queued more while we wrote
            }
            if (m_goaway_sent && streams_all_done()) co_return;
            auto [ec] = co_await m_notify.async_receive(asio::as_tuple(asio::use_awaitable));
            if (ec) co_return;
        }
    }

    asio::awaitable<void> watchdog() {
        asio::steady_timer timer{m_executor};
        for (;;) {
            timer.expires_at(m_deadline);
            co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
            if (std::chrono::steady_clock::now() >= m_deadline) break;  // idle timeout elapsed
        }
        co_return;
    }

    bool streams_all_done() const {
        for (const auto& [sid, st] : m_streams) {
            if (!st.end_stream_sent) return false;
        }
        return true;
    }

    // --- frame parsing ---

    // Parses every complete frame currently buffered in m_recv_buf. Returns
    // false on a connection-fatal protocol error (a GOAWAY has been queued);
    // true if it consumed all it could and wants more bytes.
    bool parse_available() {
        if (!consume_client_preface()) return false;   // fatal: not the h2 preface
        if (!m_preface_consumed) return true;           // still waiting for the full preface
        std::size_t pos = 0;
        while (m_recv_buf.size() - pos >= codec::kH2FrameHeaderSize) {
            codec::H2FrameHeader hdr;
            codec::parse_frame_header(std::string_view{m_recv_buf}.substr(pos), hdr);
            // RFC 9113 §4.2: a frame larger than our advertised SETTINGS_MAX_FRAME_SIZE
            // is a connection error, for every frame type (a compliant peer never sends
            // one). Bounding it here also bounds how much a single frame can buffer.
            if (hdr.length > m_limits.h2_max_frame_size) {
                go_away(codec::H2_FRAME_SIZE_ERROR);
                return false;
            }
            std::size_t frame_end = pos + codec::kH2FrameHeaderSize + hdr.length;
            if (frame_end > m_recv_buf.size()) break;  // wait for the rest of this frame
            std::string_view payload =
                std::string_view{m_recv_buf}.substr(pos + codec::kH2FrameHeaderSize, hdr.length);
            if (!handle_frame(hdr, payload)) {
                return false;  // fatal; GOAWAY already queued by handler
            }
            pos = frame_end;
        }
        if (pos > 0) m_recv_buf.erase(0, pos);
        return true;
    }

    bool handle_frame(const codec::H2FrameHeader& hdr, std::string_view payload) {
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
                on_rst_stream(hdr);
                return true;
            case codec::H2FrameType::Ping:
                return on_ping(hdr, payload);
            case codec::H2FrameType::Goaway:
                return true;  // peer is going away; let the read EOF close us
            case codec::H2FrameType::Priority:
            case codec::H2FrameType::PushPromise:
            default:
                return true;  // ignored / not applicable to a server receive path
        }
    }

    // Strips optional padding (RFC 7540 §6.1/§6.2) from a DATA/HEADERS payload,
    // returning the field-block/data slice. `has_priority` handles the 5-octet
    // priority prefix on HEADERS.
    static std::string_view strip_padding(std::string_view payload, bool padded, bool has_priority, bool& ok) {
        ok = true;
        std::size_t pad_len = 0;
        std::size_t off = 0;
        if (padded) {
            if (payload.empty()) { ok = false; return {}; }
            pad_len = static_cast<unsigned char>(payload[0]);
            off = 1;
        }
        if (has_priority) {
            if (payload.size() < off + 5) { ok = false; return {}; }
            off += 5;  // 4-octet stream dependency + 1-octet weight
        }
        if (off + pad_len > payload.size()) { ok = false; return {}; }
        return payload.substr(off, payload.size() - off - pad_len);
    }

    bool on_headers(const codec::H2FrameHeader& hdr, std::string_view payload) {
        if (hdr.stream_id == 0) { go_away(codec::H2_PROTOCOL_ERROR); return false; }
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
        std::string_view block = strip_padding(payload, hdr.has_flag(codec::H2_FLAG_PADDED),
                                               hdr.has_flag(codec::H2_FLAG_PRIORITY), ok);
        if (!ok) { go_away(codec::H2_PROTOCOL_ERROR); return false; }

        // Refuse streams beyond the concurrency limit we advertise (RFC 9113 §5.1.2):
        // each accepted stream allocates a Request (with its Body channel), a
        // ResponseWriter and a handler coroutine, so an unbounded stream count is an
        // unbounded resource commitment.
        if (m_streams.find(hdr.stream_id) == m_streams.end() &&
            m_streams.size() >= m_limits.h2_max_concurrent_streams) {
            SIMPLE_HTTP_ERROR_LOG("h2: refusing stream {} ({} open, limit {})", hdr.stream_id, m_streams.size(),
                                  m_limits.h2_max_concurrent_streams);
            reset_stream(hdr.stream_id, codec::H2_REFUSED_STREAM);
            return true;
        }
        if (header_block_too_big(m_streams.contains(hdr.stream_id) ? m_streams.at(hdr.stream_id).header_block.size() : 0,
                                 block.size())) {
            SIMPLE_HTTP_ERROR_LOG("h2: header block exceeds {} bytes (stream={})", m_limits.max_header_bytes,
                                  hdr.stream_id);
            go_away(codec::H2_ENHANCE_YOUR_CALM);
            return false;
        }
        Stream& st = ensure_stream(hdr.stream_id);
        st.header_block.append(block);
        if (hdr.stream_id > m_next_peer_stream_id) m_next_peer_stream_id = hdr.stream_id;

        // If END_STREAM is set, the client sends no request body.
        bool end_stream = hdr.has_flag(codec::H2_FLAG_END_STREAM);

        if (hdr.has_flag(codec::H2_FLAG_END_HEADERS)) {
            if (!finish_header_block(hdr.stream_id, st)) return false;
            // finish_header_block may have reset (and erased) a malformed stream, so
            // `st` must not be used again: re-look it up before dispatching.
            auto it = m_streams.find(hdr.stream_id);
            if (it == m_streams.end()) return true;
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
            m_continuation_stream = hdr.stream_id;  // CONTINUATION frames follow
            if (end_stream) st.half_closed_remote = true;  // remember for on_continuation
        }
        return true;
    }

    bool on_continuation(const codec::H2FrameHeader& hdr, std::string_view payload) {
        if (hdr.stream_id == 0 || hdr.stream_id != m_continuation_stream) {
            go_away(codec::H2_PROTOCOL_ERROR);
            return false;
        }
        auto it = m_streams.find(hdr.stream_id);
        if (it == m_streams.end()) { go_away(codec::H2_PROTOCOL_ERROR); return false; }
        if (header_block_too_big(it->second.header_block.size(), payload.size())) {
            SIMPLE_HTTP_ERROR_LOG("h2: CONTINUATION header block exceeds {} bytes (stream={})",
                                  m_limits.max_header_bytes, hdr.stream_id);
            go_away(codec::H2_ENHANCE_YOUR_CALM);  // RFC 9113 §10.5.1 (CONTINUATION flood)
            return false;
        }
        it->second.header_block.append(payload);
        if (hdr.has_flag(codec::H2_FLAG_END_HEADERS)) {
            m_continuation_stream = 0;
            if (!finish_header_block(hdr.stream_id, it->second)) return false;
            auto it2 = m_streams.find(hdr.stream_id);  // may have been reset+erased
            if (it2 == m_streams.end()) return true;
            if (it2->second.half_closed_remote) {
                (void)it2->second.request->body().finish();
            }
            dispatch_stream(hdr.stream_id);
        }
        return true;
    }

    // Decodes an accumulated header block into the stream's Request and clears
    // the buffer. Dispatch happens once END_STREAM is also seen.
    bool finish_header_block(std::uint32_t stream_id, Stream& st) {
        std::vector<codec::HpackHeader> fields;
        if (!m_decoder.decode(st.header_block, fields)) {
            SIMPLE_HTTP_ERROR_LOG("h2 HPACK decode failed (stream={}, err={})", stream_id, m_decoder.last_error());
            go_away(codec::H2_COMPRESSION_ERROR);
            return false;
        }
        st.header_block.clear();
        for (auto& f : fields) {
            // RFC 9113 §8.2.1: a field name or value must not contain CR, LF or NUL.
            // HTTP/2 has no line folding, so these bytes survive decoding verbatim and
            // would let a peer inject request lines/headers into whatever HTTP/1.1
            // message is built downstream (handlers, reverse proxy). A request is
            // malformed -> stream error (§8.1.1); the connection stays usable.
            if (contains_ctl(f.name) || contains_ctl(f.value)) {
                SIMPLE_HTTP_ERROR_LOG("h2 field with CR/LF/NUL (stream={}); resetting stream", stream_id);
                reset_stream(stream_id, codec::H2_PROTOCOL_ERROR);
                return true;  // stream is gone: the caller must not dispatch it
            }
            if (f.name == ":method") {
                st.request->set_method_token(f.value);
            } else if (f.name == ":path") {
                st.request->set_target(std::move(f.value));
            } else if (!f.name.empty() && f.name[0] == ':') {
                // :scheme / :authority — not surfaced as ordinary headers.
            } else {
                st.request->mutable_headers().add_lower(std::move(f.name), std::move(f.value));
            }
        }
        (void)stream_id;
        // Dispatch is triggered by the caller once END_STREAM is also observed.
        return true;
    }

    bool on_data(const codec::H2FrameHeader& hdr, std::string_view payload) {
        if (hdr.stream_id == 0) { go_away(codec::H2_PROTOCOL_ERROR); return false; }
        bool ok = false;
        std::string_view data = strip_padding(payload, hdr.has_flag(codec::H2_FLAG_PADDED), false, ok);
        if (!ok) { go_away(codec::H2_PROTOCOL_ERROR); return false; }

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
            Stream& st = it->second;
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
            if (padding > 0) credit_consumed(st, padding);

            if (!data.empty()) {
                // These delivered bytes owe connection-level credit until the
                // handler consumes them (or the stream is torn down).
                st.recv_owed_conn += static_cast<std::int64_t>(data.size());
                (void)st.request->body().feed(std::string{data});
            }
            if (hdr.has_flag(codec::H2_FLAG_END_STREAM)) {
                st.half_closed_remote = true;
                // End of request body. The handler (already dispatched at
                // END_HEADERS) observes end-of-body on its next Body::read().
                (void)st.request->body().finish();
                // If our response already finished, the stream is now complete.
                maybe_complete_stream(hdr.stream_id);
            }
        } else {
            // DATA on an unknown/closed stream still counted against the
            // connection window; give that credit straight back.
            replenish_conn(frame_len);
        }
        return true;
    }

    bool on_settings(const codec::H2FrameHeader& hdr, std::string_view payload) {
        if (hdr.has_flag(codec::H2_FLAG_ACK)) return true;  // our SETTINGS was acked
        if (payload.size() % 6 != 0) { go_away(codec::H2_FRAME_SIZE_ERROR); return false; }
        apply_settings_payload(payload);
        if (m_goaway_sent) return false;  // a rejected setting sent us away: no ACK
        append_frame(codec::H2FrameType::Settings, codec::H2_FLAG_ACK, 0, {});  // ACK
        return true;
    }

    bool on_window_update(const codec::H2FrameHeader& hdr, std::string_view payload) {
        if (payload.size() != 4) { go_away(codec::H2_FRAME_SIZE_ERROR); return false; }
        std::uint32_t increment = codec::read_u32(payload, 0) & 0x7FFFFFFF;
        if (increment == 0) {
            if (hdr.stream_id != 0) reset_stream(hdr.stream_id, codec::H2_PROTOCOL_ERROR);
            return true;
        }
        if (hdr.stream_id == 0) {
            m_conn_send_window += increment;
        } else {
            auto it = m_streams.find(hdr.stream_id);
            if (it != m_streams.end()) it->second.send_window += increment;
        }
        return true;
    }

    void on_rst_stream(const codec::H2FrameHeader& hdr) {
        auto it = m_streams.find(hdr.stream_id);
        if (it != m_streams.end()) {
            (void)it->second.request->body().fail(make_error_code(asio::error::connection_reset));
            erase_stream(hdr.stream_id);
        }
    }

    // Removes a stream from the table, first returning any connection-level flow
    // -control credit it still owed for delivered-but-unconsumed body bytes, so
    // the connection window cannot leak when a stream is torn down early.
    void erase_stream(std::uint32_t stream_id) {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end()) return;
        if (it->second.recv_owed_conn > 0) {
            replenish_conn(it->second.recv_owed_conn);
        }
        // Release a producer parked on backpressure: this stream will never
        // drain now, so it must not be left waiting on a queue that is gone.
        if (it->second.out_space) it->second.out_space->close();
        m_streams.erase(it);
    }

    bool on_ping(const codec::H2FrameHeader& hdr, std::string_view payload) {
        if (hdr.has_flag(codec::H2_FLAG_ACK)) return true;
        if (payload.size() != 8) { go_away(codec::H2_FRAME_SIZE_ERROR); return false; }
        append_frame(codec::H2FrameType::Ping, codec::H2_FLAG_ACK, 0, payload);  // echo
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
    // delivered (padding), so credit is only returned for data no longer buffered.
    void credit_consumed(Stream& st, std::int64_t n) {
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
        if (it->second.recv_owed_conn < 0) it->second.recv_owed_conn = 0;  // defensive
        credit_consumed(it->second, static_cast<std::int64_t>(n));
        flush();
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
        if (it == m_streams.end()) return;
        if (it->second.half_closed_remote && it->second.end_stream_sent) {
            erase_stream(stream_id);
        }
    }

    void go_away(std::uint32_t error_code_value) {
        if (m_goaway_sent) return;
        // Remember the code so the teardown path can re-serialize the GOAWAY if the
        // write loop was cancelled before it reached the wire.
        m_goaway_error = error_code_value;
        std::string payload;
        append_u32(payload, m_next_peer_stream_id);  // last stream id we processed
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
        std::vector<std::uint32_t> finished;  // streams that just emitted END_STREAM
        for (auto& [stream_id, st] : m_streams) {
            if (st.end_stream_sent) continue;

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
                if (window <= 0) break;  // blocked on flow control; resume on WINDOW_UPDATE

                std::string& front = st.out_queue.front();
                std::size_t available = front.size() - st.out_offset;
                std::size_t budget = static_cast<std::size_t>(std::min<std::int64_t>(
                    window,
                    static_cast<std::int64_t>(std::min<std::uint32_t>(m_peer_max_frame_size, m_limits.h2_max_frame_size))));
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
        for (std::uint32_t sid : finished) maybe_complete_stream(sid);
    }

    // --- handler dispatch ---

    void dispatch_stream(std::uint32_t stream_id) {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end() || it->second.dispatched) return;
        it->second.dispatched = true;
        auto request = it->second.request;
        auto writer = it->second.writer;
        auto self = this->shared_from_this();  // keep engine alive across suspensions
        std::uint32_t sid = stream_id;
        asio::co_spawn(
            m_executor,
            [self, request, writer, sid]() -> asio::awaitable<void> {
                auto response = std::make_shared<Response>(writer);
                try {
                    co_await self->m_dispatch(request, response, self->m_transport->tls_handle());
                } catch (const std::exception& e) {
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

    // --- members (touched only on the connection executor) ---
    std::shared_ptr<Transport> m_transport;
    Executor m_executor;
    asio::experimental::concurrent_channel<void(error_code)> m_notify;
    EngineLimits m_limits;
    std::chrono::steady_clock::time_point m_deadline{};

    Dispatcher m_dispatch;
    codec::HpackDecoder m_decoder;                       // connection-scoped (read loop only)
    std::unordered_map<std::uint32_t, Stream> m_streams;

    std::string m_recv_buf;  // received bytes awaiting frame parsing
    std::string m_out;       // serialized frames awaiting transport write

    std::uint32_t m_goaway_error{0};  // code for the GOAWAY re-emitted on teardown
    std::int64_t m_conn_send_window = kH2InitialWindow;  // peer's connection-level window for us (send side)
    std::int64_t m_conn_recv_window = kH2InitialWindow;  // our connection-level window to the peer (recv side)
    std::int64_t m_conn_recv_pending = 0;                // consumed bytes awaiting a batched connection WINDOW_UPDATE
    std::int32_t m_peer_initial_window = kH2InitialWindow;  // peer SETTINGS_INITIAL_WINDOW_SIZE
    std::uint32_t m_peer_max_frame_size = static_cast<std::uint32_t>(kH2MaxFrameSize);

    std::uint32_t m_next_peer_stream_id = 0;  // highest client stream id seen
    std::uint32_t m_continuation_stream = 0;  // stream awaiting CONTINUATION, or 0

    bool m_alive = true;
    bool m_goaway_sent = false;
    bool m_preface_consumed = false;  // client connection preface stripped yet?

    friend class Http2ResponseWriter<Transport>;
};  // class Http2Engine

}  // namespace simple_http
