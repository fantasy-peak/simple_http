#pragma once

// HTTP/3 engine (RFC 9114), on nghttp3 over ngtcp2.
//
// What this file used to be: a framing layer, a QPACK binding, a stream
// classifier, a control-stream state machine and two write loops, all written
// by hand. What it is now: the adapter between three things that do not know
// about each other — nghttp3's callbacks, this library's `Dispatcher`, and the
// QUIC connection underneath.
//
// The shape of that adapter, and why each piece is where it is:
//
//   * **nghttp3 owns the protocol.** Framing, SETTINGS, GOAWAY, QPACK encode
//   and
//     decode, blocked field sections, and request validation (pseudo-header
//     order, duplicates, mandatory fields, lowercase names, the
//     connection-specific fields and TE) are all the library's now. The
//     hand-written engine's counterparts to those are deleted, not
//     reimplemented.
//
//   * **The engine implements `quic::Protocol`**, so the connection can pull
//     stream data out of it. That inversion — the transport asking the protocol
//     for bytes rather than the protocol writing them — is nghttp3's design,
//     and it is why there is no write loop here: the connection has one.
//
//   * **nghttp3 callbacks arrive synchronously** from inside an nghttp3 call,
//   so
//     they may not await, allocate unboundedly, or re-enter nghttp3. They
//     record state and post wake-ups; everything else happens in a coroutine.
//     See `h3_callbacks.h`.
//
// Per request stream there is one handler coroutine, spawned when the request
// completes (or, for a bodyless request, when its headers end). A slow handler
// therefore holds up nothing but its own stream — which is the point of running
// HTTP/3 over QUIC at all.
//
// One deliberate absence: the previous engine had a 120-second idle watchdog of
// its own. It is gone, because QUIC has an idle timeout as a protocol feature
// (RFC 9000 §10.1) and ngtcp2 negotiates it with the peer — see
// `QuicConnectionConfig::max_idle_timeout_ms`. A second timer on top would
// fight the first.

#ifdef SIMPLE_HTTP_ENABLE_HTTP3

#include <nghttp3/nghttp3.h>

#include <array>
#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/experimental/channel.hpp>
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
#include "../../quic/protocol.h"
#include "../dispatcher.h"
#include "h3_callbacks.h"
#include "h3_stream.h"

namespace simple_http {

namespace asio = boost::asio;

// HTTP/3 error codes, spelled as nghttp3 spells them, so that a grep for a code
// found in a trace lands on the same name the library reports.
inline constexpr std::uint64_t kH3NoError = NGHTTP3_H3_NO_ERROR;
inline constexpr std::uint64_t kH3InternalError = NGHTTP3_H3_INTERNAL_ERROR;
inline constexpr std::uint64_t kH3ExcessiveLoad = NGHTTP3_H3_EXCESSIVE_LOAD;
inline constexpr std::uint64_t kH3RequestCancelled = NGHTTP3_H3_REQUEST_CANCELLED;
inline constexpr std::uint64_t kH3MessageError = NGHTTP3_H3_MESSAGE_ERROR;

// HTTP/3's two connection-level knobs that are ours to choose, kept at the
// values the hand-written engine advertised so the wire does not change.
inline constexpr std::size_t kH3QpackMaxTableCapacity = 4096;
inline constexpr std::size_t kH3QpackBlockedStreams = 16;

template <typename Connection> class Http3Engine;

// ResponseSink for one HTTP/3 request stream. Holds a weak_ptr to the engine
// so it can be used from any thread and after the connection has closed.
//
// Every method hops onto the connection executor first: the stream table lives
// there, and so does every nghttp3 call it makes.
template <typename Connection> class Http3ResponseSink : public ResponseSink {
  public:
    using Executor = typename Connection::executor_type;

    Http3ResponseSink(std::weak_ptr<Http3Engine<Connection>> engine, std::int64_t stream_id, Executor exec)
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
        eng->begin_response(m_stream_id, status, headers, /*end_stream=*/head,
                            /*with_body=*/!head);
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
        // No DATA frame follows, not even an empty one: the FIN that ends the
        // HEADERS frame is what tells the client the message is over (§4.1).
        eng->begin_response(m_stream_id, status, headers, /*end_stream=*/true,
                            /*with_body=*/false);
        co_return error_code{};
    }

    asio::awaitable<error_code> send_headers(int status, Headers headers) override {
        co_await hop();
        auto eng = m_engine.lock();
        if (!eng || !eng->alive() || !eng->stream_writable(m_stream_id)) {
            co_return make_error_code(asio::error::not_connected);
        }
        const bool head = eng->method_is_head(m_stream_id);
        // The body may follow, so the reader is installed even though nothing is
        // queued yet: an absent reader means "this response has no body at all",
        // which is what send_bodyless is for.
        eng->begin_response(m_stream_id, status, headers, /*end_stream=*/head,
                            /*with_body=*/!head);
        co_return error_code{};
    }

    asio::awaitable<error_code> send_chunk(std::string data) override {
        co_await hop();
        auto eng = m_engine.lock();
        // Deliberately NOT gating on nghttp3_conn_is_stream_writable2 (which is
        // what `stream_writable` is): that reports false while the stream is
        // parked for WOULDBLOCK (READ_DATA_BLOCKED) — and enqueueing the data is
        // precisely what resumes it. Only a stream that is gone or reset by the
        // peer is unsendable.
        if (!eng || !eng->alive() || !eng->stream_sendable(m_stream_id)) {
            co_return make_error_code(asio::error::not_connected);
        }
        if (eng->method_is_head(m_stream_id))
            co_return error_code{}; // HEAD: no body
        // Backpressure: block while this stream's unacknowledged bytes are over
        // the high mark, so a fast producer (a reverse proxy streaming an
        // upstream body) is paced by the peer rather than by memory.
        if (auto ec = co_await eng->await_out_space(m_stream_id); ec)
            co_return ec;
        eng->enqueue_body(m_stream_id, std::move(data), /*last=*/false);
        co_return error_code{};
    }

    asio::awaitable<error_code> send_last(std::string data) override {
        co_await hop();
        auto eng = m_engine.lock();
        if (!eng || !eng->alive() || !eng->stream_sendable(m_stream_id)) {
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
        // A reset or finished stream cannot take a response either: report the
        // response as unwritable so callers (a proxy's 502 path, say) stop rather
        // than writing onto a dead stream.
        co_return eng && eng->alive() && eng->stream_writable(m_stream_id);
    }

    asio::awaitable<void> close() override {
        co_await hop(); // reset_stream() touches the same table
        if (auto eng = m_engine.lock())
            eng->reset_stream(m_stream_id, kH3RequestCancelled);
    }

    Version version() const override { return Version::Http3; }

  private:
    asio::awaitable<void> hop() const { co_await asio::dispatch(asio::bind_executor(m_executor, asio::use_awaitable)); }

    std::weak_ptr<Http3Engine<Connection>> m_engine;
    std::int64_t m_stream_id;
    Executor m_executor;
};

template <typename Connection>
class Http3Engine : public std::enable_shared_from_this<Http3Engine<Connection>>,
                    public quic::Protocol,
                    public h3::H3CallbackSink {
  public:
    using Executor = typename Connection::executor_type;
    using Stream = h3::H3Stream;
    using StreamPtr = std::shared_ptr<Stream>;
    // One-shot wake-up, the same capacity-1 coalescing channel the HTTP/2 engine
    // uses: a second nudge while the waiter is already runnable is not
    // information, so dropping it is correct.
    using Channel = h3::Channel;
    using Signal = h3::Signal;

    explicit Http3Engine(std::shared_ptr<Connection> conn, EngineLimits limits = {})
        : m_conn(std::move(conn)), m_executor(m_conn->get_executor()), m_limits(limits) {}

    Http3Engine(const Http3Engine &) = delete;
    Http3Engine &operator=(const Http3Engine &) = delete;

    ~Http3Engine() override {
        if (m_h3 != nullptr)
            nghttp3_conn_del(m_h3);
    }

    // Serve the connection until it closes. `dispatch` runs one handler per
    // request stream.
    //
    // The engine registers itself with the connection *before* driving it,
    // because the connection may already have handed ngtcp2 the client's
    // Initial — and with it the 1-RTT keys, which is when the control streams
    // may be opened at all. Anything reported before registration is replayed by
    // `set_protocol`.
    asio::awaitable<void> run(Dispatcher dispatch) {
        m_dispatch = std::move(dispatch);
        m_conn->set_protocol(this->shared_from_this());
        co_await m_conn->run();
        // The connection is gone. Close every stream so a handler parked on a
        // body or a response write observes the end instead of waiting forever.
        m_alive = false;
        for (auto &[id, stream] : m_streams) {
            if (stream->request) {
                (void)stream->request->body().fail(make_error_code(asio::error::connection_reset));
            }
            h3::wake(stream->in_space);
            h3::wake(stream->out_space);
        }
        m_streams.clear();
        co_return;
    }

    [[nodiscard]] bool alive() const { return m_alive; }
    [[nodiscard]] Executor get_executor() { return m_executor; }

    // Begin a graceful shutdown: tell the peer not to open new streams, and let
    // the ones in flight finish (§5.2). nghttp3 sends the GOAWAY; there is no
    // API to choose its error code, which is why the error paths below close the
    // connection instead of inventing one.
    void shutdown() {
        if (h3() != nullptr)
            (void)nghttp3_conn_submit_shutdown_notice(m_h3);
        m_conn->flush();
    }

    // Wake the send loop. The engine does not own one — writing happens in
    // `next_stream_data()` — so this is the connection's.
    void flush() { m_conn->flush(); }

    // --- called by Http3ResponseSink (already hopped onto our executor) ---

    // Status and headers for `stream_id`. `with_body` installs the data reader
    // that will feed the body; `end_stream` says nothing follows the headers.
    //
    // These two are separate because nghttp3 makes them separate: a reader
    // supplied at submit time is asked for data later, but *not* supplying one is
    // itself the end-of-stream signal (`nghttp3_conn_submit_response` with a null
    // reader), and there is no way to say "no body yet" other than by having the
    // reader report it.
    void begin_response(std::int64_t stream_id, int status, const Headers &headers, bool end_stream, bool with_body) {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end() || h3() == nullptr)
            return;
        Stream &stream = *it->second;

        // Submitted immediately, even when the request body is still arriving: a
        // full-duplex "stream-one" client only finishes its upload once it has
        // seen the server answer, so waiting for the request to end here would
        // deadlock it.
        do_submit_response(stream, status, headers.fields(), end_stream, with_body);
    }

    void do_submit_response(Stream &stream, int status, const std::vector<std::pair<std::string, std::string>> &fields,
                            bool end_stream, bool with_body) {
        const std::int64_t stream_id = stream.id;

        // §4.2: fields that describe the connection are not the peer's to
        // receive. nghttp3 rejects them on the way in; on the way out it would
        // happily encode them, and a downstream hop would then act on a
        // connection that does not exist.
        m_nva.clear();
        m_owned_status = std::to_string(status);
        m_nva.push_back(make_nv(":status", m_owned_status));
        for (const auto &[name, value] : fields) {
            if (is_connection_specific_field(name))
                continue;
            if (contains_ctl(name) || contains_ctl(value))
                continue;
            m_nva.push_back(make_nv(name, value));
        }
        // Alt-Svc (RFC 7838): how a browser learns this origin also speaks
        // HTTP/3, and on an h3 response it is what renews the cache entry's
        // expiry. The value is the one the Server rendered from the QUIC
        // listener's own port, shared with the h1/h2 engines; it is empty when
        // there is nothing to advertise (h3_alt_svc off, or no QUIC listener).
        // The nv pointers must stay valid until submit_response, which the
        // member m_limits outlives.
        if (const std::string &alt_svc = m_limits.alt_svc_value(); !alt_svc.empty()) {
            m_nva.push_back(make_nv("alt-svc", alt_svc));
        }

        nghttp3_data_reader reader{};
        const nghttp3_data_reader *reader_ptr = nullptr;
        if (with_body && !end_stream) {
            reader.read_data = &Http3Engine::read_data_trampoline;
            reader_ptr = &reader;
        }

        if (nghttp3_conn_submit_response(m_h3, stream_id, m_nva.data(), m_nva.size(), reader_ptr) != 0) {
            SIMPLE_HTTP_ERROR_LOG("h3: submit_response failed (stream={})", stream_id);
            reset_stream(stream_id, kH3InternalError);
            return;
        }
        stream.response_started = true;
        stream.end_stream_sent = end_stream;
        // Nothing is queued yet for a streamed response. If the write loop runs
        // before the first enqueue_body, the reader reports "not ready" — which
        // is correct but costs a round through nghttp3's blocked path, so the
        // writer's send path is arranged to enqueue within the same synchronous
        // block. See the note on `enqueue_body`.
        m_conn->flush();
    }

    void enqueue_body(std::int64_t stream_id, std::string data, bool last) {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end())
            return;
        Stream &stream = *it->second;

        stream.out_bytes += data.size();
        stream.out_q.push_back(std::move(data));
        if (last)
            stream.out_eof = true;

        // nghttp3 stopped asking for this stream's body when it found none ready.
        // Nothing else will restart it: this is that moment, and missing it is
        // the classic deadlock of the pull model.
        if (h3() != nullptr) {
            stream.data_blocked = false;
            (void)nghttp3_conn_resume_stream(m_h3, stream_id);
        }
        m_conn->flush();
    }

    // Wait until this stream's unacknowledged bytes fall to the low mark.
    asio::awaitable<error_code> await_out_space(std::int64_t stream_id) {
        for (;;) {
            auto it = m_streams.find(stream_id);
            if (it == m_streams.end())
                co_return make_error_code(asio::error::operation_aborted);
            Stream &stream = *it->second;
            if (stream.out_write - stream.out_ack <= h3::kOutLowWatermark)
                co_return error_code{};
            if (!stream.out_space)
                stream.out_space = std::make_shared<Channel>(m_executor, 1);
            auto space = stream.out_space;
            auto [ec] = co_await space->async_receive(asio::as_tuple(asio::use_awaitable));
            if (ec)
                co_return make_error_code(asio::error::operation_aborted);
        }
    }

    [[nodiscard]] bool stream_writable(std::int64_t stream_id) const {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end()) {
            return false;
        }
        if (it->second->reset_by_peer) {
            return false;
        }
        // nghttp3 knows the state machine — whether the request ended, whether
        // the write side was shut — so it answers this rather than us.
        if (h3() != nullptr) {
            const int rv = nghttp3_conn_is_stream_writable2(m_h3, stream_id);
            // An unknown stream is not writable either, but the table above is
            // the authority on that; anything else is nghttp3 saying no.
            if (rv == 0) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool method_is_head(std::int64_t stream_id) const {
        auto it = m_streams.find(stream_id);
        return it != m_streams.end() && it->second->is_head;
    }

    // A peer reset makes the stream unusable; a stream merely *parked* for
    // WOULDBLOCK (READ_DATA_BLOCKED) is the opposite — enqueueing data is what
    // resumes it, so it must not be reported as unsendable.
    [[nodiscard]] bool stream_sendable(std::int64_t stream_id) const {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end())
            return false;
        return !it->second->reset_by_peer;
    }

    void reset_stream(std::int64_t stream_id, std::uint64_t app_error_code) {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end())
            return;
        it->second->end_stream_sent = true;
        m_conn->reset_stream(stream_id, app_error_code);
    }

    // --- quic::Protocol: the write side -------------------------------------

    quic::StreamData next_stream_data() noexcept override {
        quic::StreamData out;
        if (h3() == nullptr)
            return out;

        std::int64_t stream_id = -1;
        int fin = 0;
        std::array<nghttp3_vec, 8> vec{};
        const nghttp3_ssize count = nghttp3_conn_writev_stream(m_h3, &stream_id, &fin, vec.data(), vec.size());
        if (count < 0) {
            // nghttp3 is in a connection-error state.
            // `nghttp3_err_infer_quic_app_error_code` is the only sanctioned
            // translation, and the connection closes with it.
            m_h3_dead = true;
            out.error = nghttp3_err_infer_quic_app_error_code(static_cast<int>(count));
            return out;
        }
        // `stream_id < 0` is nghttp3 saying it has nothing for *any* stream. A
        // zero `count` with a stream id is not that: it is how nghttp3 reports
        // "0 length data + FIN" (nghttp3.h, nghttp3_conn_writev_stream) — the
        // response ends with an empty tail, so there is no DATA payload but the
        // END_STREAM still has to reach QUIC. Returning early on `count == 0`
        // (which this used to do) throws that FIN away and leaves the response
        // open forever: the peer reads the whole body and then waits for an end
        // that never comes. Reachable whenever the last body chunk is empty and
        // the write loop already drained the previous one — `begin(); write(x);
        // <await>; finish();`, i.e. every streamed response with a default
        // `finish()`, and the streaming reverse proxy.
        if (stream_id < 0)
            return out;

        m_vec_count = std::min<std::size_t>(static_cast<std::size_t>(count), m_vec.size());
        for (std::size_t i = 0; i < m_vec_count; ++i) {
            m_vec[i].base = vec[i].base;
            m_vec[i].len = vec[i].len;
        }
        out.stream_id = stream_id;
        out.fin = fin;
        out.vec = std::span<const quic::ByteVec>{m_vec.data(), m_vec_count};
        return out;
    }

    void on_stream_data_written(std::int64_t stream_id, std::size_t datalen) noexcept override {
        // This is nghttp3's own cursor (bytes it handed to ngtcp2 for the
        // stream); our handoff watermark is advanced inside read_response_data
        // instead, so it cannot drift when ngtcp2 reports a cumulative or
        // WRITE_MORE datalen. An encoder/control uni-stream is not in
        // m_streams, so only the wake is gated on the lookup; add_write_offset
        // applies to every stream, uni included.
        //
        // A zero is not a no-op: when a stream has nothing left but its FIN, the
        // bytes it sent are 0 and this call is still what tells nghttp3 the
        // stream is done (nghttp3.h: "It is important to call this function even
        // if |n| is 0 in this case") — skipping it leaves the stream scheduled
        // and the write loop offering the same FIN forever.
        auto it = m_streams.find(stream_id);
        if (it != m_streams.end()) {
            h3::wake(it->second->out_space);
        }
        if (h3() != nullptr)
            (void)nghttp3_conn_add_write_offset(m_h3, stream_id, datalen);
    }

    void on_stream_blocked(std::int64_t stream_id) noexcept override {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end())
            return;
        it->second->data_blocked = true;
        if (h3() != nullptr)
            nghttp3_conn_block_stream(m_h3, stream_id);
    }

    void on_stream_shut_wr(std::int64_t stream_id) noexcept override {
        if (h3() != nullptr)
            nghttp3_conn_shutdown_stream_write(m_h3, stream_id);
    }

    // --- quic::Protocol: the read side and events ---------------------------

    void on_stream_data(std::uint32_t flags, std::int64_t stream_id,
                        std::span<const std::uint8_t> data) noexcept override {
        if (h3() == nullptr)
            return;

        const bool fin = (flags & quic::kStreamDataFin) != 0;
        const nghttp3_ssize nconsumed =
            nghttp3_conn_read_stream2(m_h3, stream_id, data.data(), data.size(), fin ? 1 : 0, m_conn->now());
        if (nconsumed < 0) {
            // Negative means a connection error, after which nghttp3 may not be
            // called again except to be freed. The only correct response is to
            // close the connection with the code nghttp3 derives — and to shut
            // the parser off for the rest of this datagram, whose frames ngtcp2
            // is still going to hand us (see h3()).
            m_h3_dead = true;
            const std::uint64_t code = nghttp3_err_infer_quic_app_error_code(static_cast<int>(nconsumed));
            SIMPLE_HTTP_ERROR_LOG("h3: read_stream2 failed: {} (stream={})",
                                  nghttp3_strerror(static_cast<int>(nconsumed)), stream_id);
            // The oversized-section case is ours, not nghttp3's, and has a
            // better code than the generic one — see on_headers_begin.
            m_conn->close(m_oversize_stream == stream_id ? kH3ExcessiveLoad : code, "http3");
            return;
        }
        // Framing bytes are consumed by nghttp3 itself and credited here; the
        // DATA payload is credited later, by the Body consume hook installed in
        // ensure_stream(), as the handler reads it (nghttp3's recv_data
        // contract: app credits the payload itself).
        m_conn->extend_stream_offset(stream_id, static_cast<std::uint64_t>(nconsumed));
        m_conn->extend_connection_offset(static_cast<std::uint64_t>(nconsumed));

        // A callback decided this request was malformed for a reason nghttp3
        // does not police — a control character inside a field, or an authority
        // that disagrees with Host. Those are this layer's rules, so this layer
        // answers for them, and per-stream is the proportionate response: the
        // connection is still perfectly usable for every other stream on it.
        if (m_invalid_stream == stream_id) {
            m_invalid_stream = -1;
            reset_stream(stream_id, kH3MessageError);
        }
    }

    void on_acked_stream_data(std::int64_t stream_id, std::uint64_t datalen) noexcept override {
        // nghttp3 answers this by calling `acked_stream_data`, where the queue is
        // actually released — the two halves have to stay paired or the buffer
        // never drains.
        if (h3() != nullptr)
            (void)nghttp3_conn_add_ack_offset(m_h3, stream_id, datalen);
    }

    void on_stream_close(std::int64_t stream_id, std::optional<std::uint64_t> rx_error,
                         std::optional<std::uint64_t> tx_error) noexcept override {
        // This is ngtcp2's `stream_close`: it fires only once the QUIC stream is
        // finished in *both* directions. A client that merely half-closes its
        // request (a duplex "stream-one" session) does not get here while the
        // response is still flowing, so nothing is protected by holding the
        // stream back. Gating the teardown on the response having ended (as an
        // earlier revision did) leaks the stream: for any response with a body
        // `end_stream_sent` stays false, so neither nghttp3's stream nor our own
        // table entry is ever released — measured 20 created / 0 erased over one
        // connection, and unbounded growth on a long-lived one.
        if (h3() != nullptr) {
            // nghttp3 1.17 reports one application error code, not the rx/tx
            // pair later versions take.
            const std::uint64_t code = rx_error.value_or(tx_error.value_or(0));
            const int rv = nghttp3_conn_close_stream(m_h3, stream_id, code);
            // A stream nghttp3 never saw is not an error: it is how a reset
            // arrives for a stream that was already torn down.
            if (rv != 0 && rv != NGHTTP3_ERR_STREAM_NOT_FOUND) {
                SIMPLE_HTTP_ERROR_LOG("h3: close_stream failed: {} (stream={})", nghttp3_strerror(rv), stream_id);
            }
        }
        erase_stream(stream_id);
    }

    void on_stream_reset(std::int64_t stream_id, std::uint64_t app_error_code) noexcept override {
        auto it = m_streams.find(stream_id);
        if (it != m_streams.end()) {
            it->second->reset_by_peer = true;
            if (it->second->request) {
                (void)it->second->request->body().fail(make_error_code(asio::error::connection_reset));
            }
            h3::wake(it->second->in_space);
            h3::wake(it->second->out_space);
        }
        // Tell nghttp3 the peer abandoned the stream, so it stops waiting for the
        // rest of the request.
        if (h3() != nullptr)
            (void)nghttp3_conn_shutdown_stream_read(m_h3, stream_id);
        (void)app_error_code;
    }

    void on_stream_stop_sending(std::int64_t stream_id, std::uint64_t app_error_code) noexcept override {
        auto it = m_streams.find(stream_id);
        if (it != m_streams.end()) {
            // The peer does not want the rest of this response. Unpark a writer
            // parked on backpressure so it can observe that and stop.
            h3::wake(it->second->out_space);
        }
        if (h3() != nullptr)
            (void)nghttp3_conn_shutdown_stream_read(m_h3, stream_id);
        (void)app_error_code;
    }

    void on_extend_max_stream_data(std::int64_t stream_id, std::uint64_t /*max_data*/) noexcept override {
        // A window opened: nghttp3 had parked this stream as blocked on the QUIC
        // side, and it will not ask for its body again until it is told.
        if (h3() != nullptr)
            (void)nghttp3_conn_unblock_stream(m_h3, stream_id);
    }

    void on_extend_max_remote_streams_bidi(std::uint64_t max_streams) noexcept override {
        if (h3() != nullptr)
            nghttp3_conn_set_max_client_streams_bidi(m_h3, max_streams);
    }

    void on_tx_keys_ready() noexcept override {
        // Idempotent: ngtcp2 reports the 1-RTT keys once per connection, but a
        // replay after a late `set_protocol` could reach here twice.
        if (h3() != nullptr)
            return;

        // Three unidirectional streams: control, and the two QPACK halves. Below
        // that the protocol cannot be spoken at all, so there is nothing to do
        // but fail the connection.
        if (m_conn->streams_uni_left() < 3) {
            SIMPLE_HTTP_ERROR_LOG("h3: peer allows fewer than 3 unidirectional streams");
            m_alive = false;
            m_conn->close(kH3InternalError, "http3: no stream credit");
            return;
        }

        nghttp3_settings settings;
        nghttp3_settings_default(&settings);
        // nghttp3 enforces none of these; they are what we *advertise*, and the
        // two QPACK values in particular have to be set explicitly — their
        // defaults are zero, which would silently advertise a QPACK decoder that
        // accepts no dynamic table at all.
        settings.max_field_section_size = m_limits.max_header_bytes;
        settings.qpack_max_dtable_capacity = kH3QpackMaxTableCapacity;
        settings.qpack_blocked_streams = kH3QpackBlockedStreams;
        // Our encoder stays static-table-only, which is what the previous engine
        // did (ls-qpack was initialised with a zero-sized dynamic table). Turning
        // it on is a separate change with its own risk surface — dynamic table
        // insertions, acknowledgements and blocked streams interacting — and
        // belongs in a commit that can be reverted on its own.
        settings.qpack_encoder_max_dtable_capacity = 0;

        // The user data is the *sink* subobject, not `this`. nghttp3 hands the
        // pointer back verbatim and the trampolines cast it to `H3CallbackSink*`,
        // so it has to arrive already adjusted: this class has three bases, and
        // passing an unadjusted `Http3Engine*` would leave every callback
        // dispatching through the wrong vtable slot.
        if (nghttp3_conn_server_new(&m_h3, &h3::h3_callbacks(), &settings, nghttp3_mem_default(),
                                    static_cast<h3::H3CallbackSink *>(this)) != 0) {
            SIMPLE_HTTP_ERROR_LOG("h3: nghttp3_conn_server_new failed");
            m_alive = false;
            m_conn->close(kH3InternalError, "http3: init");
            return;
        }
        nghttp3_conn_set_max_client_streams_bidi(m_h3, m_conn->local_max_streams_bidi());

        const std::optional<std::int64_t> control = m_conn->open_uni_stream();
        if (!control || nghttp3_conn_bind_control_stream(m_h3, *control) != 0) {
            SIMPLE_HTTP_ERROR_LOG("h3: could not open the control stream");
            m_alive = false;
            m_conn->close(kH3InternalError, "http3: control stream");
            return;
        }
        const std::optional<std::int64_t> qpack_enc = m_conn->open_uni_stream();
        const std::optional<std::int64_t> qpack_dec = m_conn->open_uni_stream();
        if (!qpack_enc || !qpack_dec || nghttp3_conn_bind_qpack_streams(m_h3, *qpack_enc, *qpack_dec) != 0) {
            SIMPLE_HTTP_ERROR_LOG("h3: could not open the QPACK streams");
            m_alive = false;
            m_conn->close(kH3InternalError, "http3: qpack streams");
            return;
        }
        m_conn->flush();
    }

    void on_connection_closed() noexcept override {
        m_alive = false;
        for (auto &[id, stream] : m_streams) {
            if (stream->request) {
                (void)stream->request->body().fail(make_error_code(asio::error::connection_reset));
            }
            h3::wake(stream->in_space);
            h3::wake(stream->out_space);
        }
    }

    // --- h3::H3CallbackSink -------------------------------------------------

    bool on_headers_begin(std::int64_t stream_id) noexcept override {
        if (m_h3 == nullptr)
            return false;
        // The first field section on a stream is what brings it into existence:
        // nghttp3 reports the peer's request streams through this callback and
        // nothing else, and the library looks after its own control streams
        // without telling us.
        (void)ensure_stream(stream_id);
        // The one limit nghttp3 declines to enforce: it will happily buffer a
        // field section of any size, because "how big is too big" is the
        // application's to decide. `get_frame_payload_left2` is a getter, so
        // calling it here does not re-enter the parser.
        if (nghttp3_conn_get_frame_payload_left2(m_h3, stream_id) > m_limits.max_header_bytes) {
            m_oversize_stream = stream_id;
            return false;
        }
        return true;
    }

    void on_header(std::int64_t stream_id, std::int32_t /*token*/, std::string_view name,
                   std::string_view value) noexcept override {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end())
            return;
        Stream &stream = *it->second;

        // §4.2 forbids CR, LF and NUL in a field. HTTP/3 has no line folding, so
        // those bytes survive decoding and would let a peer inject a request line
        // into whatever HTTP/1.1 message is built downstream.
        if (contains_ctl(name) || contains_ctl(value)) {
            m_invalid_stream = stream_id;
            return;
        }

        if (!name.empty() && name.front() == ':') {
            // The pseudo-headers that need acting on. The rest — :scheme, and the
            // ones only a response may carry — are nghttp3's to validate and ours
            // to record.
            if (name == ":method") {
                stream.request->set_method_token(std::string{value});
                stream.is_head = iequals_ci(value, "HEAD");
            } else if (name == ":path") {
                stream.request->set_target(std::string{value});
            } else if (name == ":scheme") {
                stream.scheme = std::string{value};
            } else if (name == ":authority") {
                stream.seen_authority = true;
                stream.authority = std::string{value};
            }
            return;
        }
        stream.request->mutable_headers().add_lower(std::string{name}, std::string{value});
    }

    void on_headers_end(std::int64_t stream_id, bool fin) noexcept override {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end())
            return;
        Stream &stream = *it->second;
        if (stream.headers_done)
            return; // trailers, which this layer does not surface
        stream.headers_done = true;

        if (!finish_request(stream)) {
            m_invalid_stream = stream_id;
            return;
        }
        // A request with no body is complete as soon as its headers are: a client
        // cannot send more after the FIN.
        if (fin) {
            stream.request_complete = true;
            (void)stream.request->body().finish();
        }
        // Dispatch as soon as the request head is complete, exactly like the h2
        // engine, and let on_data feed the body to the running handler. A
        // handler must NOT wait for END_STREAM to start: a full-duplex
        // "stream-one" client (VLESS over xhttp, say) keeps the request body
        // open for the life of the session -- it only ends when the handler
        // closes the response -- so waiting here would deadlock instead of
        // merely serialising the body.
        spawn_handler(stream_id);
    }

    void on_data(std::int64_t stream_id, std::span<const std::uint8_t> data) noexcept override {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end())
            return;
        Stream &stream = *it->second;

        // Hand the bytes to the Body, honouring its own bound: a peer that sends
        // more than the handler will read must not be able to grow this queue
        // without limit.
        if (!stream.request->body().feed(std::string{reinterpret_cast<const char *>(data.data()), data.size()})) {
            reset_stream(stream_id, kH3RequestCancelled);
            return;
        }
        h3::wake(stream.in_space);
    }

    void on_end_stream(std::int64_t stream_id) noexcept override {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end())
            return;
        Stream &stream = *it->second;
        stream.request_complete = true;
        (void)stream.request->body().finish();
        spawn_handler(stream_id);
    }

    void on_stream_closed(std::int64_t stream_id, std::uint64_t /*app_error_code*/) noexcept override {
        erase_stream(stream_id);
    }

    void on_acked(std::int64_t stream_id, std::uint64_t datalen) noexcept override {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end())
            return;
        Stream &stream = *it->second;
        stream.out_ack += datalen;
        // Bytes the peer has acknowledged are the only ones that may be released:
        // QUIC retransmits from this memory, so dropping earlier would corrupt
        // the stream, and dropping later is a leak.
        while (!stream.out_q.empty() && stream.out_base + stream.out_q.front().size() <= stream.out_ack) {
            stream.out_base += stream.out_q.front().size();
            stream.out_bytes -= stream.out_q.front().size();
            stream.out_q.pop_front();
        }
        // Unpark a producer waiting on backpressure.
        h3::wake(stream.out_space);
    }

    void on_stop_sending(std::int64_t stream_id, std::uint64_t app_error_code) noexcept override {
        // nghttp3 *asking* us to send a STOP_SENDING — not the peer sending one
        // (that arrives at `Protocol::on_stream_stop_sending`). It means "stop
        // reading this request", so it is forwarded as exactly that. Gating it
        // on our own response state, as an earlier revision did, silently drops
        // the frame nghttp3 asked for whenever nghttp3 declines a request — the
        // peer is then left streaming a request nobody will ever read.
        m_conn->shutdown_stream_read(stream_id, app_error_code);
    }

    void on_reset_stream(std::int64_t stream_id, std::uint64_t app_error_code) noexcept override {
        // Likewise nghttp3 *asking* for the sending side to be shut down — it is
        // how a request it declines to serve (one arriving past a queued GOAWAY)
        // is answered. ngtcp2's `shutdown_stream_write` is the matching call;
        // `Protocol::on_stream_reset` is the one that reports the peer having
        // reset something.
        m_conn->shutdown_stream_write(stream_id, app_error_code);
    }

    void on_deferred_consume(std::int64_t stream_id, std::size_t nconsumed) noexcept override {
        // Bytes nghttp3 held while a field section was blocked on the QPACK
        // encoder stream. They left the peer long ago and were never credited,
        // because the parse had not got far enough to say they were consumed.
        m_conn->extend_stream_offset(stream_id, nconsumed);
        m_conn->extend_connection_offset(nconsumed);
    }

    void on_peer_settings(const nghttp3_proto_settings &settings) noexcept override {
        m_peer_max_field_section = settings.max_field_section_size;
    }

    void on_shutdown(std::int64_t /*push_id*/) noexcept override {
        // The peer is going away. Nothing to do: its in-flight requests still
        // arrive, and ngtcp2 reports the close when it comes.
    }

    nghttp3_ssize read_response_data(std::int64_t stream_id, nghttp3_vec *vec, std::size_t veccnt,
                                     std::uint32_t *flags) noexcept override {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end()) {
            // The stream is gone. Reporting EOF is the only way to stop nghttp3
            // asking; there is nothing left to send.
            *flags |= NGHTTP3_DATA_FLAG_EOF;
            return 0;
        }
        Stream &stream = *it->second;

        // Fill from the queue *past* what has already been handed over. The bytes
        // are not removed: nghttp3 keeps the pointers until the peer
        // acknowledges them.
        std::size_t filled = 0;
        std::size_t total = 0;
        std::uint64_t offset = stream.out_base;
        for (const std::string &chunk : stream.out_q) {
            if (filled >= veccnt || total >= h3::kMaxBodyPerRead)
                break;
            const std::uint64_t chunk_end = offset + chunk.size();
            if (chunk_end <= stream.out_write) {
                offset = chunk_end;
                continue; // wholly handed over already
            }
            const std::size_t skip =
                stream.out_write > offset ? static_cast<std::size_t>(stream.out_write - offset) : 0;
            vec[filled].base = reinterpret_cast<std::uint8_t *>(const_cast<char *>(chunk.data())) + skip;
            vec[filled].len = chunk.size() - skip;
            total += vec[filled].len;
            ++filled;
            offset = chunk_end;
        }

        if (filled == 0) {
            if (stream.out_eof) {
                *flags |= NGHTTP3_DATA_FLAG_EOF;
                return 0;
            }
            // More body is coming but none of it is here yet. There is no flag
            // for this — returning zero vectors without EOF trips an assertion
            // inside nghttp3 — so the error code is the only correct answer, and
            // `enqueue_body` is what clears it.
            stream.data_blocked = true;
            return NGHTTP3_ERR_WOULDBLOCK;
        }
        // The whole body has now been handed over (this call's slice included).
        if (stream.out_eof && stream.out_write + total >= stream.out_end()) {
            *flags |= NGHTTP3_DATA_FLAG_EOF;
        }
        // The bytes handed to nghttp3 (this call's slice included) have left our
        // queue's readable front; the skip logic above relies on this
        // watermark. Advancing it here — from the amount *we* supplied rather
        // than ngtcp2's writev datalen, whose exact meaning varies by code path
        // (WRITE_MORE / re-offers) — keeps read_response_data and
        // on_stream_data_written self-consistent.
        stream.out_write += total;
        return static_cast<nghttp3_ssize>(filled);
    }

  private:
    // The connection, or null once nghttp3 has reported a connection-level
    // error. Everything that calls into nghttp3 goes through this rather than
    // through m_h3: after a negative return nghttp3 may only be handed to
    // nghttp3_conn_del (nghttp3.h, nghttp3_conn_read_stream2) — *anything* else
    // is undefined — and the failing frame is rarely the last one in its
    // datagram. ngtcp2 keeps delivering the rest of that datagram's frames to
    // us regardless (`m_conn->close()` is only our own flag), so an ACK or a
    // second STREAM frame would otherwise re-enter a dead parser.
    [[nodiscard]] nghttp3_conn *h3() const noexcept { return m_h3_dead ? nullptr : m_h3; }

    static nghttp3_ssize read_data_trampoline(nghttp3_conn *, std::int64_t stream_id, nghttp3_vec *vec,
                                              std::size_t veccnt, std::uint32_t *flags, void *conn_user_data, void *) {
        return static_cast<H3CallbackSink *>(conn_user_data)->read_response_data(stream_id, vec, veccnt, flags);
    }

    static nghttp3_nv make_nv(std::string_view name, std::string_view value) {
        nghttp3_nv nv{};
        nv.name = reinterpret_cast<const std::uint8_t *>(name.data());
        nv.value = reinterpret_cast<const std::uint8_t *>(value.data());
        nv.namelen = name.size();
        nv.valuelen = value.size();
        nv.flags = NGHTTP3_NV_FLAG_NONE;
        return nv;
    }

    // Apply the rules that are about the *request* rather than the protocol —
    // nghttp3 has already enforced the protocol ones by the time a field reaches
    // us.
    [[nodiscard]] bool finish_request(Stream &stream) noexcept {
        Request &request = *stream.request;

        // §4.3.1: a scheme with an authority component requires one, and the two
        // spellings of it must agree — otherwise the origin is ambiguous and
        // which one wins depends on the reader. Not decorative: this library's
        // reverse proxy picks the upstream by Host, so a disagreement is a
        // routing decision made by whoever reads it first.
        const bool authority_scheme = iequals_ci(stream.scheme, "http") || iequals_ci(stream.scheme, "https");
        const bool has_host = request.mutable_headers().contains("host");
        if (authority_scheme) {
            if (stream.authority.empty() && !has_host)
                return false;
            if (stream.seen_authority && stream.authority.empty())
                return false;
            if (has_host && request.header("host")->empty())
                return false;
            if (stream.seen_authority && has_host && request.header("host") != stream.authority)
                return false;
        }

        // :authority is HTTP/3's spelling of Host, and a compliant client sends
        // no Host field at all — so without this every request would reach
        // handlers, and the reverse proxy's upstream selection, host-less.
        // Applied after the fields so a real Host wins.
        if (!stream.authority.empty() && !has_host) {
            request.mutable_headers().add_lower("host", std::move(stream.authority));
        }
        return true;
    }

    // Bring a peer-initiated request stream into the table. Returns null when
    // the stream is already gone (or was never ours).
    StreamPtr ensure_stream(std::int64_t stream_id) {
        auto it = m_streams.find(stream_id);
        if (it != m_streams.end())
            return it->second;
        auto stream = std::make_shared<Stream>();
        stream->id = stream_id;
        stream->request = std::make_shared<Request>(Version::Http3, m_executor, m_conn->peer());
        // Credit inbound request-body bytes back to QUIC flow control as the
        // handler consumes them. nghttp3's recv_data contract requires the app
        // to do this ("increase flow control credit by datalen"), and
        // read_stream2's return value explicitly excludes the DATA payload — so
        // without this hook the connection receive window (initial_max_data,
        // default 1 MiB) is spent exactly once: when the cumulative body bytes
        // reach 1 MiB the peer can never send again. Measured on this host:
        // POST 1 KiB body failed ~800 of 200000 requests, precisely at
        // (1 MiB / 1 KiB) requests x 200 connections.
        //
        // The hop back onto the connection executor is the same one the h2
        // engine makes, and for the same reason: the hook fires wherever the
        // body is *read*, and Request is shared ownership — a handler that was
        // spawned on this executor may hand the body to another coroutine on
        // another executor, or read it from a thread of its own. What it must
        // not do is touch the ngtcp2 connection from there: that connection
        // belongs to this one thread (model A), and `extend_*` mutates its
        // window state and pokes its wake-up.
        std::weak_ptr<Http3Engine> weak = this->weak_from_this();
        auto exec = m_executor;
        stream->request->body().set_on_consumed([weak, exec, stream_id](std::size_t n) {
            asio::post(exec, [weak, stream_id, n]() {
                if (auto eng = weak.lock()) {
                    eng->m_conn->extend_stream_offset(stream_id, n);
                    eng->m_conn->extend_connection_offset(n);
                }
            });
        });
        auto [inserted, _] = m_streams.emplace(stream_id, stream);
        return inserted->second;
    }

    void spawn_handler(std::int64_t stream_id) {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end())
            return;
        Stream &stream = *it->second;
        if (stream.dispatched)
            return;
        stream.dispatched = true;

        auto writer = std::make_shared<Http3ResponseSink<Connection>>(this->weak_from_this(), stream_id, m_executor);
        stream.response =
            std::make_shared<ResponseWriter>(maybe_compress_writer(writer, m_executor, stream.request, stream.is_head));

        auto self = this->shared_from_this();
        auto request = stream.request;
        auto response = stream.response;
        asio::co_spawn(
            m_executor,
            [self, request, response, stream_id]() -> asio::awaitable<void> {
                try {
                    co_await self->m_dispatch(request, response, self->m_conn->tls_handle());
                } catch (const std::exception &e) {
                    SIMPLE_HTTP_ERROR_LOG("h3 handler(stream={}) threw: {}", stream_id, e.what());
                    self->reset_stream(stream_id, kH3InternalError);
                } catch (...) {
                    SIMPLE_HTTP_ERROR_LOG("h3 handler(stream={}) threw unknown exception", stream_id);
                    self->reset_stream(stream_id, kH3InternalError);
                }
                co_return;
            },
            asio::detached);
    }

    void erase_stream(std::int64_t stream_id) {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end())
            return;
        StreamPtr stream = std::move(it->second);
        m_streams.erase(it);
        // Unpark anything waiting on this stream: nothing will ever feed these
        // channels again.
        if (stream->in_space)
            stream->in_space->close();
        if (stream->out_space)
            stream->out_space->close();
        if (stream->request && !stream->request_complete) {
            (void)stream->request->body().fail(make_error_code(asio::error::connection_reset));
        }
    }

    static bool is_connection_specific_field(std::string_view name) noexcept {
        return name == "connection" || name == "keep-alive" || name == "proxy-connection" ||
               name == "transfer-encoding" || name == "upgrade";
    }

    // --- members (touched only on the connection executor) ---
    std::shared_ptr<Connection> m_conn;
    Executor m_executor;
    EngineLimits m_limits;
    Dispatcher m_dispatch;

    nghttp3_conn *m_h3{nullptr};
    // nghttp3 is in a connection-error state; see h3(). Kept beside the pointer
    // rather than nulling it because ~Http3Engine still has to free the object.
    bool m_h3_dead{false};
    std::unordered_map<std::int64_t, StreamPtr> m_streams;

    // Scratch for `begin_response`, reused across calls so the header path does
    // not allocate a vector per response. Owned strings back the `nghttp3_nv`
    // pointers, which must stay valid until nghttp3 has encoded the block — it
    // does so during the submit call, so a member that outlives the call is
    // enough.
    std::vector<nghttp3_nv> m_nva;
    std::string m_owned_status;

    // Scratch for `next_stream_data`. The slice pointers belong to the stream's
    // out_q and must survive until the packet is written, so only the descriptor
    // array is copied here.
    std::array<quic::ByteVec, 8> m_vec{};
    std::size_t m_vec_count{0};

    std::uint64_t m_peer_max_field_section{0};
    // Set by a callback that has decided the stream is bad, read by the parse
    // that is still running. A member rather than a return value because the
    // callback cannot return one.
    std::int64_t m_invalid_stream{-1};
    std::int64_t m_oversize_stream{-1};
    bool m_alive{true};
};

} // namespace simple_http

#endif // SIMPLE_HTTP_ENABLE_HTTP3
