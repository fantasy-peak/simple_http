#pragma once

// HTTP/3 engine (RFC 9114) over the QUIC stream transport.
//
// The connection below this file knows nothing about HTTP/3: it hands up QUIC
// streams in arrival order and takes back bytes, resets and closes. Everything
// HTTP-shaped therefore lives here — which stream is a request and which is
// control, QPACK or push; framing; the pseudo-header rules — and the only thing
// that distinguishes one unidirectional stream from another is the type varint at
// its head (§6.2).
//
// The structure follows the HTTP/2 engine deliberately, because the two face the
// same problems:
//
//   * `serve_loops()` races `read_loop() || write_loop() || watchdog()` on the
//     connection executor. Whichever finishes first cancels the others.
//   * `read_loop()` accepts streams and spawns one coroutine per stream, so a slow
//     handler, a large upload or a blocked field section never holds up another
//     stream. That is the point of running over QUIC, and it is also why there is
//     no connection-level output buffer here: each request stream writes its own
//     response frames to its own QUIC stream. Only the control stream and the
//     QPACK decoder stream share the write loop.
//   * The engine is held by shared_ptr and every handler captures it, so stream
//     state outlives `run()`; the writer holds a weak_ptr and locks it per
//     operation.
//   * Every writer method hops onto the executor first, then checks the alive /
//     writable / HEAD triple gate.
//
// A GOAWAY is re-serialized after the race ends, exactly as the HTTP/2 engine
// re-serializes its own: the write loop that would have carried it is gone, and a
// cancelled async_write drops its buffer. §5.2 permits repeated GOAWAYs as long as
// the identifier never increases, which is what makes re-sending safe.

#ifdef SIMPLE_HTTP_ENABLE_HTTP3

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>

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
#include "../../quic/quic_stream_transport.h"
#include "../../quic/wire.h"
#include "../../transport/transport.h"
#include "../dispatcher.h"
#include "h3_frame.h"
#include "qpack.h"

namespace simple_http {

namespace asio = boost::asio;

// The executor a Connection exports, and the QUIC stream transport built on it.
// The connection decides the type; this layer only has to agree with it.
template <typename Connection>
using H3Executor = decltype(std::declval<Connection&>().get_executor());

template <typename Connection>
using H3Stream = quic::QuicStreamTransport<H3Executor<Connection>>;

template <typename Connection>
using H3StreamPtr = std::shared_ptr<H3Stream<Connection>>;

// QPACK parameters this endpoint advertises. The decoder honours exactly these
// (see QpackDecoder::init): the table capacity bounds the memory a peer's encoder
// may make this endpoint hold, and the blocked-stream count bounds how much of a
// request head a peer can make it wait for.
inline constexpr std::uint32_t kH3QpackMaxTableCapacity = 4096;
inline constexpr std::uint32_t kH3QpackBlockedStreams = 16;

// Outbound backpressure watermarks for one stream's response queue, matching the
// HTTP/2 engine's: park the producer at the high mark, release it at the low one.
// The QUIC stream transport has watermarks of its own, so what this really bounds
// is the queue plus one transport buffer rather than the queue alone.
inline constexpr std::size_t kH3OutHighWatermark = 1u << 20;   // 1 MiB
inline constexpr std::size_t kH3OutLowWatermark = 256u << 10;  // 256 KiB

// The largest frame payload this engine will buffer whole. DATA is exempt: its
// payload goes to the request body as it arrives, so a peer never has to send a
// whole one before it is acted on. For every other frame §10.8 requires the
// declared length to be matched by bytes before it is parsed, and this is the
// bound that stops a length field from being a memory commitment.
inline constexpr std::uint64_t kH3MaxBufferedFrame = 256u << 10;  // 256 KiB

template <typename Connection>
class Http3Engine;

// ResponseWriter for one HTTP/3 request stream. Holds a weak_ptr to the engine so
// it can be used from any thread and after the connection has closed.
template <typename Connection>
class Http3ResponseWriter : public ResponseWriter {
  public:
    using Executor = H3Executor<Connection>;

    Http3ResponseWriter(std::weak_ptr<Http3Engine<Connection>> engine, std::uint64_t stream_id, Executor exec)
        : m_engine(std::move(engine)), m_stream_id(stream_id), m_executor(exec) {}

    asio::awaitable<error_code> send(int status, Headers headers, std::string body) override {
        co_await hop();
        auto eng = m_engine.lock();
        if (!eng || !eng->alive() || !eng->stream_writable(m_stream_id)) {
            co_return make_error_code(asio::error::not_connected);
        }
        // A response to HEAD has no body: the HEADERS frame ends the stream, and
        // the headers still carry the Content-Length a GET would have produced.
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
        // No DATA frame follows, not even an empty one: the FIN that ends the
        // HEADERS frame is what tells the client the message is over (§4.1).
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
        co_return error_code{};
    }

    asio::awaitable<error_code> send_chunk(std::string data) override {
        co_await hop();
        auto eng = m_engine.lock();
        if (!eng || !eng->alive() || !eng->stream_writable(m_stream_id)) {
            co_return make_error_code(asio::error::not_connected);
        }
        if (eng->method_is_head(m_stream_id)) co_return error_code{};  // HEAD: no body
        // Backpressure: block while this stream's queue is over the high mark, so a
        // fast producer (a reverse proxy streaming an upstream body) is paced by
        // the peer rather than by memory.
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

    asio::awaitable<bool> connected() const override {
        // The hop matters here more than anywhere: stream_writable() walks the
        // engine's stream table, which the connection executor is inserting into
        // and erasing from concurrently.
        co_await hop();
        auto eng = m_engine.lock();
        // A reset or finished stream cannot take a response either: report the
        // response as unwritable so callers (a proxy's 502 path, say) stop rather
        // than writing onto a dead stream.
        co_return eng && eng->alive() && eng->stream_writable(m_stream_id);
    }
    asio::awaitable<void> close() override {
        co_await hop();  // reset_stream() touches the same table
        if (auto eng = m_engine.lock()) {
            eng->reset_stream(m_stream_id, h3codec::H3_REQUEST_CANCELLED);
        }
    }
    Version version() const override { return Version::Http3; }

  private:
    asio::awaitable<void> hop() const {
        co_await asio::dispatch(asio::bind_executor(m_executor, asio::use_awaitable));
    }

    std::weak_ptr<Http3Engine<Connection>> m_engine;
    std::uint64_t m_stream_id;
    Executor m_executor;
};

template <typename Connection>
class Http3Engine : public std::enable_shared_from_this<Http3Engine<Connection>> {
  public:
    using Executor = H3Executor<Connection>;
    using Stream = H3Stream<Connection>;
    using StreamPtr = H3StreamPtr<Connection>;
    // One-shot wake-up, the same capacity-1 coalescing channel the HTTP/2 engine
    // uses: a second nudge while the waiter is already runnable is not information,
    // so dropping it is correct.
    using Channel = asio::experimental::concurrent_channel<void(error_code)>;
    using Signal = std::shared_ptr<Channel>;

    explicit Http3Engine(std::shared_ptr<Connection> conn, EngineLimits limits = {})
        : m_conn(std::move(conn)), m_executor(m_conn->get_executor()), m_notify(m_executor, 1),
          m_limits(limits) {}

    Http3Engine(const Http3Engine&) = delete;
    Http3Engine& operator=(const Http3Engine&) = delete;

    ~Http3Engine() = default;

    // Serve the connection until it closes. `dispatch` runs one handler per request
    // stream.
    asio::awaitable<void> run(Dispatcher dispatch) {
        m_dispatch = std::move(dispatch);
        if (!m_qpack_encoder.init()) {
            SIMPLE_HTTP_ERROR_LOG("h3: QPACK encoder initialization failed");
            m_alive = false;
            m_conn->close(h3codec::H3_INTERNAL_ERROR, "http3: qpack encoder");
            co_return;
        }
        m_qpack_decoder.init(kH3QpackMaxTableCapacity, kH3QpackBlockedStreams);
        co_await serve_loops();
        co_return;
    }

    bool alive() const { return m_alive; }
    Executor get_executor() { return m_executor; }

    // Begin a graceful shutdown: GOAWAY with H3_NO_ERROR, after which the
    // connection is told to close once the in-flight requests have drained (§5.2)
    // rather than being cut off. This is the one place the connection's own
    // shutdown() is the right call, and the counterpart of the error path, which
    // ends the connection with the code that caused it.
    void shutdown() { go_away(h3codec::H3_NO_ERROR); }

    // Wake the write loop to drain the control stream / QPACK decoder stream.
    void flush() { (void)m_notify.try_send(error_code{}); }

    // --- called by Http3ResponseWriter (already hopped onto our executor) ---

    // Serialize a response field section for `stream_id` and queue it as a HEADERS
    // frame. The connection-specific fields §4.2 forbids are dropped first: they
    // describe a connection the peer is not on, and replaying one downstream would
    // be framing a message nobody sent.
    void submit_headers(std::uint64_t stream_id, int status, const Headers& headers, bool end_stream) {
        // Never write on a stream that is gone (reset by the peer) or already
        // ended: a frame after our FIN is a protocol error the peer answers by
        // closing the connection, taking every other stream with it.
        if (!stream_writable(stream_id)) return;
        auto it = m_streams.find(stream_id);
        StreamState& st = *it->second;

        Headers filtered;
        for (const auto& [name, value] : headers.fields()) {
            if (is_connection_specific_field(name)) continue;
            if (contains_ctl(name) || contains_ctl(value)) continue;  // the guard Response already applies, restated
            filtered.add_lower(name, value);
        }

        h3codec::Bytes block;
        if (!m_qpack_encoder.encode_response(stream_id, m_next_seqno++, status, filtered, block)) {
            SIMPLE_HTTP_ERROR_LOG("h3: QPACK encode failed (stream={})", stream_id);
            reset_stream(stream_id, h3codec::H3_INTERNAL_ERROR);
            return;
        }
        // Unlike the peer's limit on our own field sections, this one cannot be
        // repaired: a QPACK field section is a single unit and cannot be split over
        // several HEADERS frames the way HPACK blocks are split over CONTINUATION.
        // So the send goes ahead and the peer decides whether to reset the stream.
        if (m_peer_max_field_section != 0 && block.size() > m_peer_max_field_section) {
            SIMPLE_HTTP_ERROR_LOG("h3: response field section of {} bytes exceeds the peer's limit of {} (stream={})",
                                  block.size(), m_peer_max_field_section, stream_id);
        }

        h3codec::Bytes frame;
        h3codec::append_frame(frame, h3codec::H3FrameType::Headers, block);
        queue_out(stream_id, st, std::move(frame), end_stream);
    }

    // Queue a response body chunk as a DATA frame. `last` marks end-of-body, which
    // the drain turns into the FIN that ends this half of the stream.
    void enqueue_body(std::uint64_t stream_id, std::string data, bool last) {
        auto it = m_streams.find(stream_id);
        // Stream gone (reset) or already ended: queueing would only be dropped by
        // the drain, so refuse it here as well.
        if (it == m_streams.end() || it->second->out_finished) return;
        if (data.empty()) {
            // An empty DATA frame carries nothing, so only its FIN could matter.
            if (last) {
                it->second->out_finished = true;
                kick_drain(stream_id);
            }
            return;
        }
        h3codec::Bytes frame;
        h3codec::append_frame(frame, h3codec::H3FrameType::Data, data);
        queue_out(stream_id, *it->second, std::move(frame), last);
    }

    // Parks the calling producer until this stream's outbound queue has drained
    // below the low watermark. Returns an error once the stream is gone, so the
    // caller stops rather than waiting for a queue that will never be drained.
    asio::awaitable<error_code> await_out_space(std::uint64_t stream_id) {
        for (;;) {
            auto it = m_streams.find(stream_id);
            if (it == m_streams.end()) co_return make_error_code(asio::error::operation_aborted);
            StreamState& st = *it->second;
            if (st.out_queued <= kH3OutHighWatermark) co_return error_code{};
            if (!st.out_space) st.out_space = std::make_shared<Channel>(m_executor, 1);
            // Keep the channel alive across the wait: the stream entry may be erased
            // (and the channel closed, waking us) while we are parked.
            auto space = st.out_space;
            auto [ec] = co_await space->async_receive(asio::as_tuple(asio::use_awaitable));
            if (ec) co_return make_error_code(asio::error::operation_aborted);
        }
    }

    // Whether this stream still accepts response frames. False once the peer has
    // reset it (erased from the table) or this endpoint has sent its FIN.
    bool stream_writable(std::uint64_t stream_id) const {
        auto it = m_streams.find(stream_id);
        return it != m_streams.end() && !it->second->end_stream_sent;
    }

    // Whether the request on `stream_id` was a HEAD (its response carries no body).
    bool method_is_head(std::uint64_t stream_id) const {
        auto it = m_streams.find(stream_id);
        return it != m_streams.end() && it->second->request->method() == Method::Head;
    }

    // Abort one stream: §4.2 makes this a RESET_STREAM carrying an HTTP/3 error
    // code rather than a connection close, so a cancelled request does not take the
    // other streams with it.
    void reset_stream(std::uint64_t stream_id, std::uint64_t error_code) {
        erase_stream(stream_id);
        m_conn->reset_stream(stream_id, error_code);
    }

  private:
    // Per-stream state. Everything here is touched only on the connection executor
    // (concurrency model A), so there are no locks; the shared_ptr is for lifetime,
    // not for sharing across threads.
    struct StreamState {
        StreamPtr transport;
        std::shared_ptr<Request> request;
        std::shared_ptr<Http3ResponseWriter<Connection>> writer;

        // --- inbound ---
        std::uint64_t data_remaining = 0;  // octets of the current DATA frame left to deliver
        bool in_data_frame = false;
        bool headers_seen = false;  // the field section that begins the message
        // Any DATA since it, which is what makes a later second HEADERS a trailer
        // section rather than a second request (§4.1).
        bool data_seen = false;
        bool trailers_seen = false;
        bool request_complete = false;  // the peer sent FIN and every frame was whole
        bool dispatched = false;
        // Parks the read loop while the handler is not draining its body: the
        // unconsumed bytes stay in the read buffer, so nothing is copied twice and
        // the buffer cannot grow past what the handler has yet to take.
        Signal in_space;

        // --- outbound ---
        std::deque<std::string> out_queue;  // serialized frames awaiting the stream
        std::size_t out_queued = 0;
        bool out_finished = false;  // the handler signalled end-of-body
        bool out_draining = false;  // a drain coroutine owns this stream's writes
        bool end_stream_sent = false;
        Signal out_space;

        // --- QPACK ---
        // A field section the peer's encoder stream has not yet freed (§2.1.2).
        h3codec::QpackDecoder::Block* blocked = nullptr;
        std::vector<h3codec::Header> decoded;
        bool decode_failed = false;
        Signal decode_wake;
    };

    // --- buffered input over one stream ---

    class StreamInput {
      public:
        explicit StreamInput(StreamPtr stream) : m_stream(std::move(stream)) {}
        StreamInput(StreamInput&&) = default;
        StreamInput& operator=(StreamInput&&) = default;
        StreamInput(const StreamInput&) = delete;
        StreamInput& operator=(const StreamInput&) = delete;
        ~StreamInput() = default;

        [[nodiscard]] std::string& buffer() { return m_buf; }
        [[nodiscard]] const StreamPtr& stream() const { return m_stream; }

        // Reads whatever is available, appending it to the buffer. The error comes
        // back alongside any bytes that arrived with it — a FIN that shares a packet
        // with the last data must not discard it — which is why callers inspect the
        // buffer before acting on the error.
        asio::awaitable<error_code> refill() {
            std::array<std::byte, 16 * 1024> scratch;
            auto [ec, n] = co_await m_stream->async_read_some(std::span<std::byte>{scratch});
            if (n != 0) m_buf.append(reinterpret_cast<const char*>(scratch.data()), n);
            co_return ec;
        }

        // Reads until at least `n` bytes are buffered. A default error_code means it
        // succeeded; anything else is the error that ended the stream first, so the
        // caller can tell a clean FIN from a reset rather than guessing.
        asio::awaitable<error_code> fill_at_least(std::size_t n) {
            while (m_buf.size() < n) {
                if (error_code ec = co_await refill()) co_return ec;
            }
            co_return error_code{};
        }

        // Drops the first `n` buffered bytes (a whole frame, once it is done with).
        void consume(std::size_t n) { m_buf.erase(0, n); }

      private:
        StreamPtr m_stream;
        std::string m_buf;
    };

    // --- serve loops ---

    asio::awaitable<void> serve_loops() {
        using namespace asio::experimental::awaitable_operators;
        m_deadline = std::chrono::steady_clock::now() + m_limits.idle_timeout;
        if (!co_await open_critical_streams()) {
            SIMPLE_HTTP_ERROR_LOG("h3: could not open the control/QPACK streams");
            m_alive = false;
            m_conn->close(h3codec::H3_INTERNAL_ERROR, "http3: critical streams");
            co_return;
        }
        flush();
        co_await (read_loop() || write_loop() || watchdog());
        m_alive = false;
        // Release everything parked on a channel the loops will no longer feed:
        // whichever loop finished first cancelled the others, and a parked producer
        // would otherwise never run again.
        for (auto& entry : m_streams) {
            StreamState& st = *entry.second;
            if (st.out_space) st.out_space->close();
            if (st.in_space) st.in_space->close();
            if (st.decode_wake) st.decode_wake->close();
        }
        // Re-serialize the GOAWAY: the write loop that would have carried it is
        // gone, and a cancelled async_write drops its buffer — so the frame the peer
        // needs in order to tell "processed" from "retry" may never have reached the
        // wire. §5.2 allows an endpoint to send GOAWAY more than once as long as the
        // identifier never increases.
        if (m_goaway_sent) {
            m_control_out.clear();  // queued SETTINGS are moot on a dying connection
            h3codec::append_frame(m_control_out, h3codec::H3FrameType::Goaway,
                                  h3codec::goaway_payload(m_goaway_stream_id));
        }
        co_await (flush_control_output() || flush_deadline());
        // A graceful wind-down was already handed to the connection (`shutdown`);
        // anything else ends here, with the code that caused it.
        if (!m_graceful) {
            m_conn->close(m_goaway_sent ? m_goaway_code : h3codec::H3_NO_ERROR, "http3: teardown");
        }
        co_return;
    }

    asio::awaitable<void> read_loop() {
        for (;;) {
            StreamPtr stream = co_await m_conn->accept_stream();
            if (!stream) co_return;  // the connection is gone
            m_deadline = std::chrono::steady_clock::now() + m_limits.idle_timeout;
            classify_stream(std::move(stream));
        }
    }

    // The write loop carries only the two locally-initiated streams more than one
    // coroutine can write to. Request-stream responses do not come through here:
    // each has its own QUIC stream and its own drain, which is what keeps one slow
    // response from delaying another.
    asio::awaitable<void> write_loop() {
        for (;;) {
            if (!m_control_out.empty()) {
                std::string chunk;
                chunk.swap(m_control_out);
                m_deadline = std::chrono::steady_clock::now() + m_limits.idle_timeout;
                auto [ec, n] = co_await m_control_stream->async_write(
                    std::as_bytes(std::span<const char>{chunk.data(), chunk.size()}));
                (void)n;
                if (ec) co_return;
                continue;
            }
            if (!m_qpack_decoder_out.empty()) {
                std::string chunk;
                chunk.swap(m_qpack_decoder_out);
                auto [ec, n] = co_await m_qpack_decoder_stream->async_write(
                    std::as_bytes(std::span<const char>{chunk.data(), chunk.size()}));
                (void)n;
                if (ec) co_return;
                continue;
            }
            // Everything queued for the control stream has reached it, and a GOAWAY
            // means the connection is over. Stopping here is what unwinds
            // serve_loops()'s race; the teardown there re-serializes the GOAWAY
            // before closing, because a cancelled write would have dropped it.
            if (m_goaway_sent) co_return;
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

    // Best-effort flush of whatever the write loop left behind.
    asio::awaitable<void> flush_control_output() {
        if (!m_control_out.empty() && m_control_stream) {
            std::string chunk;
            chunk.swap(m_control_out);
            (void)co_await m_control_stream->async_write(
                std::as_bytes(std::span<const char>{chunk.data(), chunk.size()}));
        }
        if (!m_qpack_decoder_out.empty() && m_qpack_decoder_stream) {
            std::string chunk;
            chunk.swap(m_qpack_decoder_out);
            (void)co_await m_qpack_decoder_stream->async_write(
                std::as_bytes(std::span<const char>{chunk.data(), chunk.size()}));
        }
        co_return;
    }

    // Deadline for the teardown flush: never let a peer that stopped reading keep
    // the connection (and its coroutine frames) alive.
    asio::awaitable<void> flush_deadline() {
        asio::steady_timer timer{m_executor};
        timer.expires_after(std::chrono::seconds(2));
        co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        co_return;
    }

    // --- locally-initiated unidirectional streams ---

    asio::awaitable<bool> open_critical_streams() {
        m_control_stream = m_conn->open_uni_stream();
        m_qpack_encoder_stream = m_conn->open_uni_stream();
        m_qpack_decoder_stream = m_conn->open_uni_stream();
        if (!m_control_stream || !m_qpack_encoder_stream || !m_qpack_decoder_stream) co_return false;

        // The control stream leads with its type and then SETTINGS, which §6.2.1
        // makes its first frame. The QPACK streams lead with their types and then
        // stay open and empty: the encoder never inserts an entry, and RFC 9204 §4.2
        // forbids closing either stream.
        h3codec::append_varint(m_control_out, h3codec::H3_STREAM_CONTROL);
        std::string encoder_type;
        std::string decoder_type;
        h3codec::append_varint(encoder_type, h3codec::H3_STREAM_QPACK_ENCODER);
        h3codec::append_varint(decoder_type, h3codec::H3_STREAM_QPACK_DECODER);
        auto [enc_ec, enc_n] = co_await m_qpack_encoder_stream->async_write(
            std::as_bytes(std::span<const char>{encoder_type.data(), encoder_type.size()}));
        (void)enc_n;
        if (enc_ec) co_return false;
        auto [dec_ec, dec_n] = co_await m_qpack_decoder_stream->async_write(
            std::as_bytes(std::span<const char>{decoder_type.data(), decoder_type.size()}));
        (void)dec_n;
        if (dec_ec) co_return false;
        queue_settings();
        co_return true;
    }

    void queue_settings() {
        const std::vector<h3codec::H3Setting> settings{
            {h3codec::H3_SETTINGS_QPACK_MAX_TABLE_CAPACITY, kH3QpackMaxTableCapacity},
            {h3codec::H3_SETTINGS_QPACK_BLOCKED_STREAMS, kH3QpackBlockedStreams},
            {h3codec::H3_SETTINGS_MAX_FIELD_SECTION_SIZE, static_cast<std::uint64_t>(m_limits.max_header_bytes)},
            // §7.2.4.1 asks every endpoint to send at least one reserved identifier,
            // so that a peer which mishandles the reserved range is caught at once
            // rather than whenever an extension first appears.
            {0x21, 0},
        };
        std::string payload;
        h3codec::append_settings(payload, settings);
        h3codec::append_frame(m_control_out, h3codec::H3FrameType::Settings, payload);
    }

    // --- stream classification ---

    void classify_stream(StreamPtr stream) {
        const std::uint64_t id = stream->stream_id();
        const auto self = this->shared_from_this();
        // The low two bits of a QUIC stream ID say who opened it and whether it is
        // bidirectional (RFC 9000 §2.1). Only two of the four kinds can arrive here.
        switch (id & 0x03u) {
            case 0x00:  // client-initiated bidirectional: a request stream (§6.1)
                // A request stream that arrives before the peer's SETTINGS frame is
                // *not* an error. §6.2.1 gives H3_MISSING_SETTINGS exactly one
                // cause — the control stream's first frame not being SETTINGS — and
                // §7.2.4.1 is explicit that every setting has an initial value: "This
                // removes the need to wait for the SETTINGS frame before sending
                // messages", and a client "SHOULD NOT wait indefinitely for SETTINGS
                // to arrive before sending requests". Rejecting early requests would
                // also make the outcome depend on which stream the engine happens to
                // classify first, so a peer that opens both would be answered
                // differently from one run to the next.
                if (m_goaway_sent) {
                    // Requests at or above the GOAWAY identifier are rejected rather
                    // than processed (§5.2), and H3_REQUEST_REJECTED tells the client
                    // the request is safe to retry elsewhere.
                    m_conn->reset_stream(id, h3codec::H3_REQUEST_REJECTED);
                    return;
                }
                if (id + 4 > m_next_peer_stream_id) m_next_peer_stream_id = id + 4;
                asio::co_spawn(m_executor, serve_request(self, std::move(stream)), asio::detached);
                return;
            case 0x02:  // client-initiated unidirectional: control, push or QPACK (§6.2)
                asio::co_spawn(m_executor, serve_unidirectional(self, std::move(stream)), asio::detached);
                return;
            default:
                // 0x01 and 0x03 are server-initiated: this endpoint opens them, so a
                // peer cannot have sent one. Being told otherwise means the two ends
                // disagree about who owns which stream, which no further parsing can
                // repair.
                SIMPLE_HTTP_ERROR_LOG("h3: peer sent stream {} with unusable stream-id bits", id);
                go_away(h3codec::H3_STREAM_CREATION_ERROR);
                return;
        }
    }

    // --- unidirectional streams ---

    asio::awaitable<void> serve_unidirectional([[maybe_unused]] std::shared_ptr<Http3Engine> self, StreamPtr stream) {
        StreamInput in(std::move(stream));
        // The stream type is a varint at the head of the stream (§6.2). A stream
        // closed or reset before it arrives is allowed and simply ignored.
        std::optional<std::uint64_t> type;
        while (!type) {
            quic::Reader reader = quic::reader_of(in.buffer());
            const std::size_t width = reader.peek_varint_width();
            if (width != 0 && in.buffer().size() >= width) {
                type = reader.varint();
                in.consume(width);
                break;
            }
            if (co_await in.refill()) co_return;
        }

        const std::uint64_t id = in.stream()->stream_id();
        switch (*type) {
            case h3codec::H3_STREAM_CONTROL:
                if (m_control_stream_seen) {
                    // "Only one control stream per peer is permitted" (§6.2.1).
                    SIMPLE_HTTP_ERROR_LOG("h3: second control stream ({})", id);
                    go_away(h3codec::H3_STREAM_CREATION_ERROR);
                    co_return;
                }
                m_control_stream_seen = true;
                co_await serve_control_stream(std::move(in));
                co_return;
            case h3codec::H3_STREAM_QPACK_ENCODER:
                // RFC 9204 §4.2: a second instance of either QPACK stream is a
                // connection error of the same kind as a second control stream.
                if (m_qpack_encoder_seen) {
                    go_away(h3codec::H3_STREAM_CREATION_ERROR);
                    co_return;
                }
                m_qpack_encoder_seen = true;
                co_await serve_qpack_encoder_stream(std::move(in));
                co_return;
            case h3codec::H3_STREAM_QPACK_DECODER:
                if (m_qpack_decoder_seen) {
                    go_away(h3codec::H3_STREAM_CREATION_ERROR);
                    co_return;
                }
                m_qpack_decoder_seen = true;
                co_await serve_qpack_decoder_stream(std::move(in));
                co_return;
            case h3codec::H3_STREAM_PUSH:
                // Only servers push. A client that opens a push stream has its
                // directions reversed (§6.2.2).
                go_away(h3codec::H3_STREAM_CREATION_ERROR);
                co_return;
            default:
                if (h3codec::h3_reserved_code(*type)) {
                    // §6.2.3: reserved types must be treated as meaningless. The
                    // data is discarded rather than the stream reset, which is the
                    // other option §6.2 offers.
                    for (;;) {
                        if (co_await in.refill()) co_return;
                        in.buffer().clear();
                    }
                }
                // An unknown type cannot be interpreted, so the stream is aborted —
                // and §6.2 is explicit that this is a *stream* error, never a
                // connection error.
                SIMPLE_HTTP_ERROR_LOG("h3: unknown unidirectional stream type {} (stream {})", *type, id);
                m_conn->reset_stream(id, h3codec::H3_STREAM_CREATION_ERROR);
                co_return;
        }
    }

    // `self` is not a parameter here: the caller is still suspended on this
    // coroutine and holds the engine, so the lifetime is already covered.
    asio::awaitable<void> serve_control_stream(StreamInput in) {
        h3codec::H3FrameHeader hdr;
        std::size_t header_len = 0;
        // §6.2.1: the first frame must be SETTINGS. This is checked before any
        // per-type rule, so even a frame that is illegal on a control stream in its
        // own right (DATA, say) reports H3_MISSING_SETTINGS when it comes first.
        const error_code first_ec = co_await next_frame(in, hdr, header_len);
        if (first_ec || hdr.type != static_cast<std::uint64_t>(h3codec::H3FrameType::Settings)) {
            go_away(h3codec::H3_MISSING_SETTINGS);
            co_return;
        }
        // The bound is checked before the payload is waited for, not after: the
        // first frame's length is the peer's to choose, and this is the one frame
        // whose payload the type check above cannot have already justified.
        if (hdr.length > kH3MaxBufferedFrame) {
            go_away(h3codec::H3_EXCESSIVE_LOAD);
            co_return;
        }
        const std::size_t first_len = header_len + static_cast<std::size_t>(hdr.length);
        if (const error_code ec = co_await in.fill_at_least(first_len); ec) {
            go_away(h3codec::H3_CLOSED_CRITICAL_STREAM);
            co_return;
        }
        if (!apply_settings(std::string_view{in.buffer()}.substr(header_len, static_cast<std::size_t>(hdr.length)))) {
            co_return;  // apply_settings() has already sent us away
        }
        in.consume(first_len);

        for (;;) {
            if (co_await next_frame(in, hdr, header_len)) {
                // The sender MUST NOT close the control stream, and §6.2.1 makes the
                // closure a connection error. A truncated final frame is worse still
                // (§7.1). Either way the connection ends.
                go_away(h3codec::H3_CLOSED_CRITICAL_STREAM);
                co_return;
            }
            switch (static_cast<h3codec::H3FrameType>(hdr.type)) {
                case h3codec::H3FrameType::Settings:
                    // "If an endpoint receives a second SETTINGS frame on the control
                    // stream ... H3_FRAME_UNEXPECTED" (§7.2.4).
                    go_away(h3codec::H3_FRAME_UNEXPECTED);
                    co_return;
                case h3codec::H3FrameType::Data:
                case h3codec::H3FrameType::Headers:
                    // Neither belongs on a control stream (§7.2.1, §7.2.2).
                    go_away(h3codec::H3_FRAME_UNEXPECTED);
                    co_return;
                case h3codec::H3FrameType::PushPromise:
                    // PUSH_PROMISE is a server-to-client frame; a server that
                    // receives one is talking to a peer with the roles reversed.
                    go_away(h3codec::H3_FRAME_UNEXPECTED);
                    co_return;
                case h3codec::H3FrameType::CancelPush:
                    // This endpoint never promises a push, so every push ID the peer
                    // could name is beyond the maximum it is allowed to use (§7.2.3).
                    go_away(h3codec::H3_ID_ERROR);
                    co_return;
                case h3codec::H3FrameType::MaxPushId:
                case h3codec::H3FrameType::Goaway:
                    // Both are legal here and neither gives this endpoint anything
                    // to do: it never pushes, and a peer's GOAWAY only means it will
                    // stop making requests, which the connection reports to us by
                    // closing.
                    break;
                default:
                    if (h3codec::h3_h2_reserved_frame(hdr.type)) {
                        // §7.2.8: the HTTP/2 codes HTTP/3 has no equivalent for must
                        // not be sent, and are not ignorable.
                        go_away(h3codec::H3_FRAME_UNEXPECTED);
                        co_return;
                    }
                    break;  // reserved or unknown: ignored (§7.2.8, §9)
            }
            // Every frame that reaches here is one this endpoint does not act on,
            // but its payload still has to be stepped over to find the next header.
            if (hdr.length > kH3MaxBufferedFrame) {
                go_away(h3codec::H3_EXCESSIVE_LOAD);
                co_return;
            }
            const std::size_t frame_len = header_len + static_cast<std::size_t>(hdr.length);
            if (co_await in.fill_at_least(frame_len)) {
                go_away(h3codec::H3_FRAME_ERROR);
                co_return;
            }
            in.consume(frame_len);
        }
    }

    bool apply_settings(std::string_view payload) {
        std::vector<h3codec::H3Setting> settings;
        const std::uint64_t status = h3codec::decode_settings(h3codec::reader_of(payload), settings);
        if (status != h3codec::H3_NO_ERROR) {
            go_away(status);
            return false;
        }
        // Everything this endpoint does not implement is accepted and ignored, as
        // §7.2.4 requires — including QPACK's settings, whose table capacity the
        // peer offers to an encoder that never inserts, and every reserved
        // identifier.
        if (const std::uint64_t* value =
                h3codec::find_setting(settings, h3codec::H3_SETTINGS_MAX_FIELD_SECTION_SIZE)) {
            m_peer_max_field_section = *value;
        }
        m_settings_received = true;
        return true;
    }

    asio::awaitable<void> serve_qpack_encoder_stream(StreamInput in) {
        for (;;) {
            // Drain what is already buffered *before* asking for more. The
            // stream-type varint was read from this same buffer, so anything the
            // peer sent in the same flight is sitting here already — and a loop
            // that refills first waits for a second flight that a peer which sends
            // its capacity and then waits for an answer will never send. The whole
            // instruction is then silently ignored, which is exactly what a
            // conformance case for "the capacity exceeds the limit" catches.
            if (!in.buffer().empty()) {
                const bool ok = m_qpack_decoder.feed_encoder_stream(in.buffer());
                in.buffer().clear();
                if (!ok) {
                    go_away(h3codec::QPACK_ENCODER_STREAM_ERROR);
                    co_return;
                }
                // Encoder-stream data is what frees a blocked field section, and
                // the streams waiting on those are parked on this executor, so they
                // are resumed here — before the loop reads anything else.
                resume_unblocked_decodes();
                continue;
            }
            const error_code ec = co_await in.refill();
            if (ec) {
                // RFC 9204 §4.2: the encoder stream must not be closed.
                go_away(h3codec::H3_CLOSED_CRITICAL_STREAM);
                co_return;
            }
        }
    }

    asio::awaitable<void> serve_qpack_decoder_stream(StreamInput in) {
        for (;;) {
            // The peer's decoder stream acknowledges what *this* endpoint's encoder
            // put in the dynamic table. This encoder never inserts — it is
            // static-table-only — so two of the three instructions describe state we
            // do not have:
            //
            //   * an Insert Count Increment of zero acknowledges nothing while
            //     looking like progress, and any non-zero one exceeds the zero
            //     insertions we have sent (§4.4.3 makes both a connection error);
            //   * a Section Acknowledgment names a field section with a non-zero
            //     Required Insert Count, and we have never sent one (§4.4.1).
            //
            // A Stream Cancellation is different, and treating it as an error —
            // which this loop did — breaks real clients: §4.4.2 gives it no error
            // condition at all, because it is what a decoder emits when it resets a
            // stream or simply stops reading one. A client streaming a request body
            // and giving up on a stream emits one in the ordinary course of events;
            // there is nothing for an encoder that never inserts to cancel, so it is
            // accepted and dropped.
            for (;;) {
                h3codec::DecoderInstruction kind{};
                std::uint64_t value = 0;
                std::size_t consumed = 0;
                // An instruction split across two reads is not an error, only a
                // reason to read more: the stream is bytes, not messages.
                if (!h3codec::read_decoder_instruction(in.buffer(), kind, value, consumed)) break;
                in.consume(consumed);
                if (kind == h3codec::DecoderInstruction::StreamCancellation) continue;
                SIMPLE_HTTP_ERROR_LOG("h3: decoder-stream instruction {} acknowledging insertions we never made",
                                      kind == h3codec::DecoderInstruction::SectionAcknowledgment ? "section-ack"
                                                                                                 : "insert-count");
                go_away(h3codec::QPACK_DECODER_STREAM_ERROR);
                co_return;
            }
            const error_code ec = co_await in.refill();
            if (ec) {
                // RFC 9204 §4.2: the decoder stream must not be closed either.
                go_away(h3codec::H3_CLOSED_CRITICAL_STREAM);
                co_return;
            }
        }
    }

    // --- request streams ---

    // `self` is not read below — it is the parameter that keeps the engine (and
    // therefore the stream table this coroutine walks) alive for as long as the
    // request lasts, exactly as the captured shared_ptr does in the HTTP/2 engine's
    // dispatch lambda.
    asio::awaitable<void> serve_request([[maybe_unused]] std::shared_ptr<Http3Engine> self, StreamPtr transport) {
        const std::uint64_t id = transport->stream_id();
        auto st = std::make_shared<StreamState>();
        st->transport = transport;
        st->request = std::make_shared<Request>(Version::Http3, m_executor, transport->peer());
        st->writer = std::make_shared<Http3ResponseWriter<Connection>>(this->weak_from_this(), id, m_executor);
        // The consume hook exists for this loop alone: QUIC credits its own
        // connection window as bytes leave the stream, so the only thing this wakes
        // is a read loop parked on a body the handler was not draining.
        const auto executor = m_executor;
        std::weak_ptr<Http3Engine> weak = this->weak_from_this();
        st->request->body().set_on_consumed([weak, executor, id](std::size_t) {
            asio::post(executor, [weak, id]() {
                if (auto eng = weak.lock()) eng->on_body_consumed(id);
            });
        });
        m_streams.emplace(id, st);

        StreamInput in(std::move(transport));
        for (;;) {
            if (st->in_data_frame) {
                // A DATA frame is delivered as it arrives rather than buffered whole:
                // its length is the peer's to choose, and holding it would turn one
                // length field into an allocation commitment.
                const std::size_t take =
                    std::min<std::size_t>(in.buffer().size(), static_cast<std::size_t>(st->data_remaining));
                if (take != 0) {
                    if (!st->request->body().feed(in.buffer().substr(0, take))) {
                        // The handler is not draining: stop reading and wait, so the
                        // buffer holds exactly what has not been consumed yet. A
                        // closed channel means the wait is over for good — see
                        // wait_for_body_space — so the read loop ends with it.
                        if (co_await wait_for_body_space(st)) co_return;
                        if (m_streams.find(id) == m_streams.end()) co_return;
                        continue;
                    }
                    in.consume(take);
                    st->data_remaining -= take;
                }
                if (st->data_remaining == 0) st->in_data_frame = false;
                if (st->in_data_frame) {
                    const error_code ec = co_await in.refill();
                    if (ec) {
                        end_request(st, id, in, ec);
                        co_return;
                    }
                    m_deadline = std::chrono::steady_clock::now() + m_limits.idle_timeout;
                }
                continue;
            }

            h3codec::H3FrameHeader hdr;
            std::size_t header_len = 0;
            if (const error_code ec = co_await next_frame(in, hdr, header_len); ec) {
                end_request(st, id, in, ec);
                co_return;
            }
            m_deadline = std::chrono::steady_clock::now() + m_limits.idle_timeout;
            const std::size_t frame_len = header_len + static_cast<std::size_t>(hdr.length);
            switch (static_cast<h3codec::H3FrameType>(hdr.type)) {
                case h3codec::H3FrameType::Headers: {
                    if (st->trailers_seen) {
                        // Nothing may follow the trailing field section (§4.1).
                        go_away(h3codec::H3_FRAME_UNEXPECTED);
                        co_return;
                    }
                    if (st->headers_seen && !st->data_seen) {
                        // A second field section with no content between them is a
                        // second request on one stream, which §4.1 makes malformed.
                        go_away(h3codec::H3_FRAME_UNEXPECTED);
                        co_return;
                    }
                    if (hdr.length > m_limits.max_header_bytes) {
                        // A field section beyond what this endpoint announced in
                        // SETTINGS. It cannot be decoded without holding it, and it
                        // cannot be held without a bound — and skipping it would
                        // leave the connection-wide QPACK state half-updated, so it
                        // ends the connection rather than the stream.
                        go_away(h3codec::H3_EXCESSIVE_LOAD);
                        co_return;
                    }
                    if (const error_code ec = co_await in.fill_at_least(frame_len); ec) {
                        end_request(st, id, in, ec);
                        co_return;
                    }
                    // The field section is copied out of the read buffer before the
                    // buffer is consumed. `consume()` shifts the remaining bytes
                    // down, so a view would silently start reading the frames that
                    // followed; and decoding may block on the peer's encoder stream,
                    // over which the buffer keeps growing — an append can move it
                    // outright.
                    std::string block{in.buffer(), header_len, static_cast<std::size_t>(hdr.length)};
                    in.consume(frame_len);
                    // A second field section is a trailer section: §4.1 puts it after
                    // the content, and data_seen is what distinguishes it from a
                    // second request.
                    const bool trailers = st->headers_seen;
                    if (!co_await on_request_field_section(st, id, block, trailers)) co_return;
                    if (trailers) {
                        st->trailers_seen = true;
                    } else {
                        st->headers_seen = true;
                        dispatch_stream(id);
                    }
                    break;
                }
                case h3codec::H3FrameType::Data:
                    if (!st->headers_seen || st->trailers_seen) {
                        // Content before the header section, or after the trailer
                        // section, describes a message that is already over (§4.1).
                        go_away(h3codec::H3_FRAME_UNEXPECTED);
                        co_return;
                    }
                    in.consume(header_len);
                    st->in_data_frame = true;
                    st->data_seen = true;
                    st->data_remaining = hdr.length;
                    break;
                default:
                    if (forbidden_on_request_stream(hdr.type)) {
                        go_away(h3codec::H3_FRAME_UNEXPECTED);
                        co_return;
                    }
                    // Reserved and unknown frame types are ignored (§7.2.8, §9); their
                    // payload is skipped so the next frame header is found.
                    if (hdr.length > kH3MaxBufferedFrame) {
                        go_away(h3codec::H3_EXCESSIVE_LOAD);
                        co_return;
                    }
                    if (const error_code ec = co_await in.fill_at_least(frame_len); ec) {
                        end_request(st, id, in, ec);
                        co_return;
                    }
                    in.consume(frame_len);
                    break;
            }
        }
    }

    // --- handler dispatch ---

    void dispatch_stream(std::uint64_t stream_id) {
        if (auto it = m_streams.find(stream_id); it != m_streams.end()) {
        }
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end() || it->second->dispatched) return;
        it->second->dispatched = true;
        auto request = it->second->request;
        auto writer = it->second->writer;
        auto self = this->shared_from_this();  // keep the engine alive across suspensions
        asio::co_spawn(
            m_executor,
            [self, request, writer, stream_id]() -> asio::awaitable<void> {
                // The same wrapper as the h1 and h2 engines: `writer` stays in the
                // engine's hands for stream bookkeeping, and only the Response gets
                // the compressing view.
                auto response = std::make_shared<Response>(
                    maybe_compress_writer(writer, self->m_executor, self->m_limits.compression,
                                          request->header("accept-encoding").value_or(std::string_view{}),
                                          request->method() == Method::Head));
                try {
                    co_await self->m_dispatch(request, response, self->m_conn->tls_handle());
                } catch (const std::exception& e) {
                    SIMPLE_HTTP_ERROR_LOG("h3 handler(stream={}) threw: {}", stream_id, e.what());
                    self->reset_stream(stream_id, h3codec::H3_INTERNAL_ERROR);
                } catch (...) {
                    SIMPLE_HTTP_ERROR_LOG("h3 handler(stream={}) threw unknown exception", stream_id);
                    self->reset_stream(stream_id, h3codec::H3_INTERNAL_ERROR);
                }
                self->flush();
                co_return;
            },
            asio::detached);
    }

    // Frames HTTP/3 defines that must not appear on a request stream, plus the
    // HTTP/2 codes §7.2.8 reserves and forbids. Everything else — the GREASE range
    // and unknown extension frames — is ignored (§9, Table 1).
    static bool forbidden_on_request_stream(std::uint64_t type) noexcept {
        return h3codec::h3_h2_reserved_frame(type) ||
               type == static_cast<std::uint64_t>(h3codec::H3FrameType::Settings) ||
               type == static_cast<std::uint64_t>(h3codec::H3FrameType::Goaway) ||
               type == static_cast<std::uint64_t>(h3codec::H3FrameType::PushPromise) ||
               type == static_cast<std::uint64_t>(h3codec::H3FrameType::CancelPush) ||
               type == static_cast<std::uint64_t>(h3codec::H3FrameType::MaxPushId);
    }

    // Reads one frame header, waiting for bytes as needed. A default error_code
    // means a header is ready; anything else is the transport error that ended the
    // stream, passed through rather than collapsed into one value — a clean FIN, a
    // RESET_STREAM and a dead connection call for three different responses, and
    // the caller is the only one that knows which stream it was reading.
    //
    // It returns as soon as the *header* is parsed and deliberately does not wait
    // for the payload. The length field is the peer's to choose, so waiting on it
    // is waiting on a number the peer picked, and the wait would happen before any
    // caller has had the chance to look at the number: every bound the callers
    // enforce — `kH3MaxBufferedFrame` on the control stream, `max_header_bytes` on
    // a field section — would then be checked against a buffer that already holds
    // what it was meant to refuse. DATA is the sharpest case: it is meant to be
    // stepped through as it arrives, and it never is if the header read insists on
    // the whole frame.
    asio::awaitable<error_code> next_frame(StreamInput& in, h3codec::H3FrameHeader& hdr, std::size_t& header_len) {
        for (;;) {
            quic::Reader reader = quic::reader_of(in.buffer());
            header_len = h3codec::frame_header_len(reader);
            if (header_len != 0) {
                hdr = h3codec::parse_frame_header(reader);
                co_return error_code{};
            }
            error_code ec = co_await in.refill();
            if (ec) co_return ec;
            m_deadline = std::chrono::steady_clock::now() + m_limits.idle_timeout;
        }
    }

    // The request stream ended. `ec` is `eof` for a clean FIN, `connection_reset`
    // for a RESET_STREAM and `operation_aborted` for a connection that went away.
    void end_request(const std::shared_ptr<StreamState>& st, std::uint64_t id, StreamInput& in,
                     const error_code& ec) {
        if (ec == asio::error::connection_reset) {
            // The peer abandoned the request: the handler sees the failure through
            // its body rather than as a truncated read, and this half is reset too
            // so the stream is not left half-open.
            (void)st->request->body().fail(make_error_code(asio::error::connection_reset));
            reset_stream(id, h3codec::H3_REQUEST_CANCELLED);
            return;
        }
        if (ec != asio::error::eof) return;  // the connection is gone; nothing left to report on

        // A clean FIN with a frame still incomplete is a truncated frame, which §7.1
        // makes a connection error: the peer's framing and this endpoint's disagree,
        // and every later frame boundary is guesswork.
        if (!in.buffer().empty() || st->in_data_frame) {
            go_away(h3codec::H3_FRAME_ERROR);
            return;
        }
        if (!st->headers_seen) {
            // The client closed a request stream without a complete request. No
            // response can be produced for it, and §4.1 says to say so.
            reset_stream(id, h3codec::H3_REQUEST_INCOMPLETE);
            return;
        }
        st->request_complete = true;
        // As in the HTTP/2 engine, the terminator is recorded out of band rather
        // than queued: losing it would hang the reader instead of truncating it.
        st->request->body().finish();
        maybe_complete_stream(id);
    }

    // Decodes one field section into `st->request` (or discards it, for trailers).
    // Returns false when the connection is finished.
    asio::awaitable<bool> on_request_field_section(const std::shared_ptr<StreamState>& st, std::uint64_t id,
                                                   std::string_view block, bool trailers) {
        std::vector<h3codec::Header> fields;
        h3codec::QpackDecoder::Block* pending = nullptr;
        const auto status = m_qpack_decoder.decode(id, block, fields, m_qpack_decoder_out, &pending);
        if (status == h3codec::QpackDecoder::Status::Error) {
            // §4.2.1's whole point: the dynamic table is connection-scoped, so a
            // field section that cannot be decoded leaves every later one in doubt.
            go_away(h3codec::QPACK_DECOMPRESSION_FAILED);
            co_return false;
        }
        if (status == h3codec::QpackDecoder::Status::Blocked) {
            pending->user = st.get();
            st->blocked = pending;
            st->decoded.clear();
            flush();
            // Wait for the peer's encoder stream to carry the entries this field
            // section referenced (RFC 9204 §2.1.2). Only this stream parks — that is
            // the point of QPACK.
            if (st->decode_wake == nullptr) st->decode_wake = std::make_shared<Channel>(m_executor, 1);
            auto wake = st->decode_wake;
            // A closed channel is teardown releasing this coroutine, not the
            // encoder stream arriving: it leaves `blocked` set, and reading that
            // as a decode failure answers an ordinary idle close with a GOAWAY
            // naming the wrong cause — and that GOAWAY becomes the connection's
            // close code. Only a wake that actually arrived is a decode event.
            const auto [ec] = co_await wake->async_receive(asio::as_tuple(asio::use_awaitable));
            if (ec) co_return false;
            if (m_streams.find(id) == m_streams.end()) co_return false;
            if (st->decode_failed || st->blocked != nullptr) {
                go_away(h3codec::QPACK_DECOMPRESSION_FAILED);
                co_return false;
            }
            fields = std::move(st->decoded);
        } else {
            flush();  // the Section Acknowledgment, if any, is now queued for the decoder stream
        }

        // Trailers are accepted and their values discarded: this implementation does
        // not expose trailer fields to handlers, and §4.1 only requires the frame to
        // be understood, not acted on.
        if (trailers) co_return true;

        if (!populate_request(st, id, fields)) {
            // §4.1.2 makes a malformed request a *stream* error: the connection
            // stays usable and only this request is lost.
            reset_stream(id, h3codec::H3_MESSAGE_ERROR);
            co_return false;
        }
        co_return true;
    }

    // Folds a decoded field section into the request, applying RFC 9114 §4.2 and
    // §4.3.1.
    bool populate_request(const std::shared_ptr<StreamState>& st, std::uint64_t id,
                          const std::vector<h3codec::Header>& fields) {
        Request& request = *st->request;
        // :authority is collected and applied after the loop, so a real Host field —
        // which may follow it — wins over the synthesized one.
        std::string authority;
        std::string scheme;
        bool seen_method = false;
        bool seen_scheme = false;
        bool seen_path = false;
        bool seen_authority = false;
        bool seen_regular_field = false;
        std::size_t decoded_size = 0;

        auto malformed = [&](std::string_view why) {
            SIMPLE_HTTP_ERROR_LOG("h3 malformed request (stream={}): {}", id, why);
            return false;
        };

        for (const auto& field : fields) {
            // §4.2 forbids CR, LF and NUL in a field. HTTP/3 has no line folding, so
            // those bytes survive decoding and would let a peer inject a request line
            // into whatever HTTP/1.1 message is built downstream.
            if (contains_ctl(field.first) || contains_ctl(field.second)) return malformed("field with CR/LF/NUL");
            // §4.2.2 sizes a field list as name + value + 32 octets per field.
            decoded_size += field.first.size() + field.second.size() + 32;
            if (decoded_size > m_limits.max_header_bytes) return malformed("field section too large");

            const bool pseudo = !field.first.empty() && field.first[0] == ':';
            // §4.3: pseudo-header fields must precede every regular field.
            if (pseudo && seen_regular_field) return malformed("pseudo-header after a regular field");

            if (pseudo) {
                if (field.first == ":method") {
                    if (seen_method) return malformed("duplicate :method");
                    seen_method = true;
                    request.set_method_token(field.second);
                } else if (field.first == ":scheme") {
                    // Kept rather than dropped: §4.3.1 makes the scheme decide
                    // whether an authority is mandatory, which is checked after the
                    // loop. Nothing downstream consumes the value itself — the
                    // connection's security is already settled by the time an engine
                    // runs.
                    if (seen_scheme) return malformed("duplicate :scheme");
                    seen_scheme = true;
                    scheme = field.second;
                } else if (field.first == ":path") {
                    if (seen_path) return malformed("duplicate :path");
                    seen_path = true;
                    request.set_target(field.second);
                } else if (field.first == ":authority") {
                    if (seen_authority) return malformed("duplicate :authority");
                    seen_authority = true;
                    authority = field.second;
                } else {
                    // :status and every other pseudo-header belong to a response.
                    return malformed("unknown pseudo-header");
                }
                continue;
            }

            seen_regular_field = true;
            for (char c : field.first) {
                if (ascii_lower(c) != c) return malformed("uppercase field name");
            }
            // §4.2: the connection-specific fields, which a client must not send in
            // HTTP/3, and which would be framing a message the sender never described
            // if replayed into an HTTP/1.1 hop.
            if (is_connection_specific_field(field.first)) return malformed("connection-specific field");
            // TE is the one exception, and only with the value "trailers".
            if (field.first == "te" && !iequals_ci(field.second, "trailers")) {
                return malformed("TE with a value other than trailers");
            }
            request.mutable_headers().add_lower(field.first, field.second);
        }

        // §4.3.1: a request carries exactly one :method, :scheme and :path — except
        // CONNECT, which must omit the latter two.
        if (!seen_method) return malformed("missing :method");
        if (request.method() != Method::Connect) {
            if (!seen_scheme) return malformed("missing :scheme");
            if (!seen_path) return malformed("missing :path");
            if (request.target().empty()) return malformed("empty :path");
        }
        // §4.3.1: a scheme with a mandatory authority component — http and https
        // — requires *either* :authority or Host, and neither may be empty. The
        // check is not decorative: a request that names a scheme and no host has
        // no origin, and a reverse proxy that forwards it would ask the backend
        // to guess which site was meant.
        const bool authority_scheme = iequals_ci(scheme, "http") || iequals_ci(scheme, "https");
        const bool has_host = request.mutable_headers().contains("host");
        if (authority_scheme) {
            if (authority.empty() && !has_host) return malformed("missing :authority and Host");
            if (seen_authority && authority.empty()) return malformed("empty :authority");
            if (has_host && request.header("host")->empty()) return malformed("empty Host");
            // Both present means both must agree; otherwise the origin is ambiguous
            // and which one wins depends on the reader.
            if (seen_authority && has_host && request.header("host") != authority) {
                return malformed(":authority and Host disagree");
            }
        } else if (seen_authority || has_host) {
            // A scheme without a mandatory authority must not carry one.
            return malformed(":authority or Host on a scheme that has no authority");
        }

        // :authority is HTTP/3's spelling of Host, and a compliant client sends no
        // Host field at all — so without this every request would reach handlers, and
        // the reverse proxy's x-forwarded-host, host-less. Applied after the loop so
        // a real Host field wins.
        if (!authority.empty() && !has_host) {
            request.mutable_headers().add_lower("host", std::move(authority));
        }
        return true;
    }

    // --- QPACK plumbing (connection executor only) ---

    // Resumes every field section the last encoder-stream feed freed, and wakes the
    // stream waiting for each. Runs on the executor immediately after
    // `lsqpack_dec_enc_in` returned, so no other coroutine can observe half of it.
    void resume_unblocked_decodes() {
        for (h3codec::QpackDecoder::Block* block : m_qpack_decoder.take_unblocked()) {
            if (block->user == nullptr) {
                // The stream went away while the block was queued; the decoder must
                // still be told to let the entry reference go.
                m_qpack_decoder.abandon(block, m_qpack_decoder_out);
                continue;
            }
            auto* st = static_cast<StreamState*>(block->user);
            const auto status = m_qpack_decoder.resume(block, st->decoded, m_qpack_decoder_out);
            if (status == h3codec::QpackDecoder::Status::Blocked) continue;  // still missing a later insert
            st->blocked = nullptr;
            st->decode_failed = (status == h3codec::QpackDecoder::Status::Error);
            if (st->decode_wake) (void)st->decode_wake->try_send(error_code{});
        }
        m_qpack_decoder.flush_ici(m_qpack_decoder_out);
        flush();
    }

    void on_body_consumed(std::uint64_t stream_id) {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end()) return;
        if (it->second->in_space) (void)it->second->in_space->try_send(error_code{});
    }

    // Parks the read loop until the handler has taken enough body for the feed to
    // succeed — or until the channel is closed, which is how teardown releases it.
    //
    // The closure is reported rather than swallowed. It is the *only* signal that
    // the wait can no longer make progress: the caller's other exit is "the stream
    // is gone from the table", and teardown closes these channels without erasing
    // the entry, so a caller that treats a close as "try again" spins — refilling
    // nothing, feeding nothing, on a channel that is closed forever — and the
    // coroutine frame it never leaves holds the engine, and the connection, alive
    // for the rest of the process's life. `await_out_space` reports the same
    // event the same way.
    asio::awaitable<error_code> wait_for_body_space(const std::shared_ptr<StreamState>& st) {
        if (st->in_space == nullptr) st->in_space = std::make_shared<Channel>(m_executor, 1);
        auto space = st->in_space;
        auto [ec] = co_await space->async_receive(asio::as_tuple(asio::use_awaitable));
        co_return ec;
    }

    // --- outbound framing ---

    void queue_out(std::uint64_t stream_id, StreamState& st, std::string frame, bool end_stream) {
        st.out_queued += frame.size();
        st.out_queue.push_back(std::move(frame));
        if (end_stream) st.out_finished = true;
        kick_drain(stream_id);
    }

    void kick_drain(std::uint64_t stream_id) {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end()) return;
        StreamState& st = *it->second;
        // `out_draining` is cleared before the drain returns and set here before it
        // starts, with no suspension in either window, so this check cannot race it.
        if (st.out_draining || st.end_stream_sent) return;
        st.out_draining = true;
        auto self = this->shared_from_this();
        auto state = it->second;
        auto stream = st.transport;
        asio::co_spawn(
            m_executor,
            [self, state, stream]() -> asio::awaitable<void> { co_await self->drain_stream(state, stream); },
            asio::detached);
    }

    // Writes one stream's queued frames in order, then the FIN that ends this half
    // of it. The StreamState is held by shared_ptr for the length of the drain: a
    // reset from the handler can erase the table entry while a write is in flight,
    // and the drain must not wake up reading freed memory.
    asio::awaitable<void> drain_stream(std::shared_ptr<StreamState> st, StreamPtr stream) {
        const std::uint64_t id = stream->stream_id();
        for (;;) {
            while (!st->out_queue.empty()) {
                std::string chunk = std::move(st->out_queue.front());
                st->out_queue.pop_front();
                st->out_queued -= chunk.size();
                // Release a producer parked on backpressure once the queue is back
                // under the low mark; a wake-up that arrives early is harmless,
                // because the producer re-checks the watermark before queueing more.
                if (st->out_queued <= kH3OutLowWatermark && st->out_space) {
                    (void)st->out_space->try_send(error_code{});
                }
                m_deadline = std::chrono::steady_clock::now() + m_limits.idle_timeout;
                // async_write completing means the bytes are queued, not
                // acknowledged: QUIC retransmits on its own, so a writer waits for
                // room, never for the peer.
                auto [ec, n] = co_await stream->async_write(
                    std::as_bytes(std::span<const char>{chunk.data(), chunk.size()}));
                (void)n;
                if (ec) {
                    st->out_draining = false;
                    co_return;
                }
                // The stream may have been reset while that write was in flight; the
                // queued frames are moot then, and the FIN below must not be sent.
                if (m_streams.find(id) == m_streams.end()) {
                    st->out_draining = false;
                    co_return;
                }
            }
            if (st->out_finished) {
                // The FIN is what makes the response complete (§4.1), and
                // shutdown() only queues it — which is exactly the contract QUIC
                // gives.
                (void)co_await stream->async_shutdown();
                st->end_stream_sent = true;
                st->out_draining = false;
                maybe_complete_stream(id);
                co_return;
            }
            st->out_draining = false;
            co_return;
        }
    }

    // --- stream table ---

    // Retires a stream once the peer has finished its side and this endpoint's FIN
    // is queued: it can never be read or written again, so keeping it would leak an
    // entry, and a blocked field section, for the life of the connection.
    void maybe_complete_stream(std::uint64_t stream_id) {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end()) return;
        if (it->second->request_complete && it->second->end_stream_sent) erase_stream(stream_id);
    }

    void erase_stream(std::uint64_t stream_id) {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end()) return;
        std::shared_ptr<StreamState> st = std::move(it->second);
        m_streams.erase(it);
        // Unpark anything waiting on this stream: nothing will ever feed these
        // channels again.
        if (st->out_space) st->out_space->close();
        if (st->in_space) st->in_space->close();
        if (st->decode_wake) st->decode_wake->close();
        // A drain parked inside the transport's own outbound backpressure — the
        // peer stopped acknowledging, which is exactly what a cancelled download
        // looks like — is not waiting on any of those. Closing the transport's
        // write-space channel releases it, and leaves the reset code the connection
        // was given intact (unlike transport close(), which would reset with zero).
        if (st->out_draining && st->transport) {
            auto state = st->transport->state();
            if (state && state->write_space) state->write_space->close();
        }
        // A field section the peer's encoder stream still holds for this stream will
        // never be finished now. Dropping it in silence would leave the peer's
        // encoder believing an acknowledgement is still coming; the Stream
        // Cancellation instruction is what says otherwise (RFC 9204 §2.2.2.2).
        if (st->blocked != nullptr) {
            h3codec::QpackDecoder::Block* block = st->blocked;
            st->blocked = nullptr;
            block->user = nullptr;
            m_qpack_decoder.abandon(block, m_qpack_decoder_out);
            flush();
        }
    }

    void go_away(std::uint64_t error_code) {
        if (m_goaway_sent) return;
        m_goaway_sent = true;
        m_goaway_code = error_code;
        // The identifier is the first request stream this endpoint will not process,
        // i.e. one past the highest it accepted (§5.2).
        m_goaway_stream_id = m_next_peer_stream_id;
        h3codec::append_frame(m_control_out, h3codec::H3FrameType::Goaway,
                              h3codec::goaway_payload(m_goaway_stream_id));
        flush();
        if (error_code == h3codec::H3_NO_ERROR) {
            // A graceful wind-down: hand the connection to shutdown, which closes
            // once the streams have drained rather than cutting them off.
            m_graceful = true;
            m_conn->shutdown(error_code);
        }
    }

    // --- helpers ---

    static bool is_connection_specific_field(std::string_view name) noexcept {
        return name == "connection" || name == "keep-alive" || name == "proxy-connection" ||
               name == "transfer-encoding" || name == "upgrade";
    }

    // --- members (touched only on the connection executor) ---
    std::shared_ptr<Connection> m_conn;
    Executor m_executor;
    Channel m_notify;
    EngineLimits m_limits;
    std::chrono::steady_clock::time_point m_deadline{};

    Dispatcher m_dispatch;
    h3codec::QpackEncoder m_qpack_encoder;
    h3codec::QpackDecoder m_qpack_decoder;
    std::string m_qpack_decoder_out;  // instructions owed on this endpoint's QPACK decoder stream
    std::uint64_t m_next_seqno = 0;   // QPACK header-block sequence number

    std::unordered_map<std::uint64_t, std::shared_ptr<StreamState>> m_streams;

    StreamPtr m_control_stream;
    StreamPtr m_qpack_encoder_stream;
    StreamPtr m_qpack_decoder_stream;
    std::string m_control_out;  // serialized frames for the control stream

    std::uint64_t m_goaway_code = h3codec::H3_NO_ERROR;
    std::uint64_t m_goaway_stream_id = 0;
    std::uint64_t m_next_peer_stream_id = 0;  // next client bidi stream id this endpoint would accept
    std::uint64_t m_peer_max_field_section = 0;

    bool m_alive = true;
    bool m_goaway_sent = false;
    bool m_graceful = false;
    bool m_settings_received = false;
    bool m_control_stream_seen = false;
    bool m_qpack_encoder_seen = false;
    bool m_qpack_decoder_seen = false;
};

}  // namespace simple_http

#endif  // SIMPLE_HTTP_ENABLE_HTTP3
