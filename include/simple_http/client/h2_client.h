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
// racing under awaitable_operators, with all connection state owned by the single
// connection executor:
//   * read_loop   transport bytes -> frame parser -> per-stream inbox
//   * write_loop  control frames + per-stream DATA (flow-controlled) -> transport
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
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
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

#include "../core/base64.h"
#include "../core/http_method.h"
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

namespace simple_http {

namespace asio = boost::asio;

// Protocol defaults (RFC 9113 §6.5.2, §6.9.2): what a peer may assume about us
// until our SETTINGS say otherwise. The server engine has its own copies; these
// are the client's so the two session types stay independent headers.
inline constexpr std::int32_t kClientInitialWindow = 65535;
inline constexpr std::size_t kClientMaxFrameSize = 16384;

// Outbound backpressure watermarks for one stream's request-body queue: a writer
// is parked once it has this much queued but unframed, and released when the
// write loop has drained the queue back to the low mark. Without it a writer
// outruns the peer's flow-control window at full speed and queues the whole body
// in memory (the lesson the server engine learned for responses).
inline constexpr std::size_t kClientOutHighWatermark = 1u << 20;   // 1 MiB
inline constexpr std::size_t kClientOutLowWatermark = 256u << 10;  // 256 KiB

// The SETTINGS payload we advertise, raw (6 bytes per entry, for the SETTINGS
// frame) and base64url (for an h2c upgrade's HTTP2-Settings header).
inline std::string h2_settings_payload(const EngineLimits& limits) {
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
    add(codec::H2_SETTINGS_ENABLE_PUSH, 0);  // server push is not supported here
    return payload;
}

inline std::string h2_settings_base64url(const EngineLimits& limits) {
    return base64_url_encode(h2_settings_payload(limits));
}

template <TransportLike Transport>
class Http2ClientSession;

// How a stream ended, shared between the session (which sets it) and the stream
// handle (which reports it). It has to live on the handle, not in the session's
// stream table: the session erases that entry as soon as the stream is over, so
// a handle kept a little longer still answers truthfully instead of reporting a
// closed session.
enum class StreamState : int {
    Open = 0,
    Eof,         // the response body ended normally
    Reset,       // the peer reset the stream (RST_STREAM)
    Disconnect,  // the connection went away mid-stream
    Failed,      // a specific error_code (in StreamOutcome::ec)
};

struct StreamOutcome {
    std::atomic<int> state{static_cast<int>(StreamState::Open)};
    error_code ec;

    StreamState get() const {
        return static_cast<StreamState>(state.load(std::memory_order_acquire));
    }

    void set(StreamState s, error_code e = {}) {
        ec = std::move(e);
        state.store(static_cast<int>(s), std::memory_order_release);
    }

    // The error a failed stream reports; Open/Eof/Rst/Disconnect have their own
    // event or error at the call site.
    error_code to_error() const {
        return ec ? ec : make_error_code(client_errc::session_closed);
    }
};

// Maps a terminal stream state to what the caller sees: a clean end-of-body, or
// the error that ended it. A reset and a vanished connection are failures — the
// body the caller got is a truncation, not a body.
inline std::expected<ReadResult, error_code> outcome_result(const StreamOutcome& outcome) {
    switch (outcome.get()) {
        case StreamState::Eof:
            return ReadResult::end();
        case StreamState::Reset:
            return std::unexpected{make_error_code(client_errc::stream_reset)};
        case StreamState::Disconnect:
            return std::unexpected{make_error_code(asio::error::connection_reset)};
        case StreamState::Failed:
            return std::unexpected{outcome.to_error()};
        case StreamState::Open:
        default:
            return std::unexpected{make_error_code(client_errc::session_closed)};
    }
}

// The error form, for the paths that only need to know why a stream ended (a
// clean end-of-body is then reported as a protocol error: no head ever arrived).
inline error_code outcome_error(const StreamOutcome& outcome) {
    switch (outcome.get()) {
        case StreamState::Reset:
            return make_error_code(client_errc::stream_reset);
        case StreamState::Disconnect:
            return make_error_code(asio::error::connection_reset);
        case StreamState::Failed:
            return outcome.to_error();
        case StreamState::Eof:
            return make_error_code(client_errc::protocol_error);
        case StreamState::Open:
        default:
            return make_error_code(client_errc::session_closed);
    }
}

// One HTTP/2 stream, from the caller's side. Holds the id, the shared outcome
// and a weak reference to the session: a stream kept past the connection's death
// reports that instead of touching a dead transport.
template <TransportLike Transport>
class Http2ClientStream final : public ClientStream {
  public:
    Http2ClientStream(std::shared_ptr<Http2ClientSession<Transport>> session,
                      std::uint32_t id,
                      std::shared_ptr<StreamOutcome> outcome)
        : m_session(std::move(session)), m_id(id), m_outcome(std::move(outcome)) {
    }

    asio::awaitable<error_code> write(std::string data) override {
        // Terminal state first: the session may have dropped its table entry
        // already, so the outcome is the only truthful source.
        if (m_outcome->get() != StreamState::Open)
            co_return m_outcome->to_error();
        co_return co_await m_session->stream_write(m_id, std::move(data), /*last=*/false);
    }

    asio::awaitable<error_code> finish(std::string data) override {
        if (m_outcome->get() != StreamState::Open)
            co_return m_outcome->to_error();
        co_return co_await m_session->stream_write(m_id, std::move(data), /*last=*/true);
    }

    asio::awaitable<std::expected<ReadResult, error_code>> read() override {
        if (m_outcome->get() != StreamState::Open)
            co_return outcome_result(*m_outcome);
        co_return co_await m_session->stream_read(m_id);
    }

    bool finished() const override {
        return m_outcome->get() != StreamState::Open;
    }

    Version version() const override {
        return Version::Http2;
    }

    std::uint32_t id() const override {
        return m_id;
    }

    asio::awaitable<void> cancel() override {
        co_await asio::dispatch(asio::bind_executor(m_session->get_executor(), asio::use_awaitable));
        m_session->stream_cancel(m_id);
    }

  private:
    friend class Http2ClientSession<Transport>;  // publishes the parsed head here

    asio::awaitable<error_code> await_head() override {
        co_return co_await m_session->await_stream_head(m_id);
    }

    // Strong: the caller's stream keeps the connection alive. The session refers
    // back to its streams weakly, so there is no cycle.
    std::shared_ptr<Http2ClientSession<Transport>> m_session;
    std::uint32_t m_id;
    std::shared_ptr<StreamOutcome> m_outcome;
};

template <TransportLike Transport>
class Http2ClientSession final : public ClientSession,
                                 public std::enable_shared_from_this<Http2ClientSession<Transport>> {
  public:
    using Executor = decltype(std::declval<Transport&>().get_executor());

    // The executor this session is pinned to. Streams dispatch onto it before
    // touching session state, exactly as they do on the HTTP/1.1 side.
    Executor get_executor() { return m_executor; }

    Http2ClientSession(std::shared_ptr<Transport> transport,
                       ClientTarget target,
                       EngineLimits limits,
                       std::chrono::milliseconds idle_timeout)
        : m_transport(std::move(transport)),
          m_executor(m_transport->get_executor()),
          m_target(std::move(target)),
          m_limits(limits),
          m_idle_timeout(idle_timeout),
          m_notify(m_executor, 1),
          m_idle_timer(m_executor) {
        // What we advertise bounds what the peer may index into, so the decoder
        // is set to the same value. We advertise nothing, i.e. the 4096 default.
        m_decoder.set_max_table_size(4096);
    }

    // --- connection setup ---

    // Sends the preface + our SETTINGS and starts the loops. Returns once the
    // connection is running (the loops hold the session alive themselves).
    asio::awaitable<error_code> start() {
        auto result = co_await start_impl(std::nullopt, {});
        co_return result ? error_code{} : result.error();
    }

    // Starts the connection and replays `seed` as stream 1 — the request that
    // carried `Upgrade: h2c` (RFC 9113 §3.2). No HEADERS is sent for it: the
    // peer already has the request. Returns stream 1's handle.
    // `initial` carries whatever the peer already sent after its 101 — the h2
    // connection preface and SETTINGS, typically, or even the response itself.
    // Dropping those bytes costs a round trip at best and hangs at worst.
    asio::awaitable<std::expected<std::shared_ptr<ClientStream>, error_code>> start_with_stream(
        RequestSpec seed,
        std::string initial = {}) {
        auto result = co_await start_impl(std::move(seed), std::move(initial));
        if (!result)
            co_return std::unexpected{result.error()};
        if (!*result)
            co_return std::unexpected{make_error_code(client_errc::protocol_error)};
        co_return *result;
    }

    // --- ClientSession ---

    asio::awaitable<std::expected<std::shared_ptr<ClientStream>, error_code>> open_stream(RequestSpec spec) override {
        co_await hop();
        if (m_goaway_received)
            co_return std::unexpected{make_error_code(client_errc::goaway)};
        if (!m_alive)
            co_return std::unexpected{make_error_code(client_errc::session_closed)};
        if (m_peer_max_concurrent_streams != 0 && m_streams.size() >= m_peer_max_concurrent_streams) {
            co_return std::unexpected{make_error_code(client_errc::too_many_streams)};
        }
        if (m_next_stream_id > 0x7FFFFFFFu) {  // client stream ids are odd and 31-bit
            co_return std::unexpected{make_error_code(client_errc::too_many_streams)};
        }

        const std::uint32_t id = m_next_stream_id;
        m_next_stream_id += 2;
        m_last_stream_id = id;

        auto [it, inserted] = m_streams.try_emplace(id);
        (void)inserted;
        Stream& st = it->second;
        init_stream(st, id);

        std::string block;
        if (auto ec = encode_request_head(spec, block); ec) {
            erase_stream(it);
            co_return std::unexpected{ec};
        }

        const bool body_expected = spec.stream_body || !spec.body.empty();
        if (!spec.body.empty()) {
            st.out_queued = spec.body.size();
            st.out_queue.push_back(spec.body);
        }
        if (!body_expected || !spec.stream_body)
            st.out_finished = true;  // whole body already queued
        if (!body_expected)
            st.local_end = true;
        auto handle = std::make_shared<Http2ClientStream<Transport>>(this->shared_from_this(), id, st.outcome);
        st.handle = handle;
        submit_headers(id, block, /*end_stream=*/!body_expected);
        flush();

        SIMPLE_HTTP_ERROR_LOG("h2 client: stream {} opened on {}", id, m_authority);
        co_return handle;
    }

    bool alive() const override {
        return m_alive;
    }

    bool reusable() const override {
        return m_alive && !m_goaway_received && m_streams.empty();
    }

    Version version() const override {
        return Version::Http2;
    }

    std::string_view authority() const override {
        return m_authority;
    }

    void close() override {
        asio::post(m_executor, [self = this->shared_from_this()] {
            self->m_alive = false;
            self->fail_all_streams(client_errc::session_closed);
            if (self->m_transport)
                self->m_transport->close();
        });
    }

    void arm_idle_close(std::chrono::milliseconds ttl) override {
        m_pooled = true;
        m_idle_armed = true;
        m_idle_timer.expires_after(ttl);
        m_idle_timer.async_wait([self = this->shared_from_this()](const error_code& ec) {
            if (ec || !self->m_idle_armed)
                return;  // cancelled by disarm() or a re-arm
            self->m_idle_armed = false;
            SIMPLE_HTTP_ERROR_LOG("h2 client: closing idle pooled connection to {}", self->m_authority);
            self->m_alive = false;
            self->fail_all_streams(client_errc::session_closed);
            if (self->m_transport)
                self->m_transport->close();
        });
    }

    void disarm_idle_close() override {
        m_pooled = false;
        m_idle_armed = false;
        m_idle_timer.cancel();
    }

    // Invoked (on this session's executor) when the session has finished
    // everything in flight and is reusable again — the facade returns it to the
    // pool from here.
    void set_on_idle(std::function<void()> cb) {
        m_on_idle = std::move(cb);
    }

    void notify_idle() {
        if (m_pooled || !m_on_idle || !reusable())
            return;
        m_on_idle();
    }

    // --- introspection (tests / diagnostics) ---
    std::size_t active_streams() const {
        return m_streams.size();
    }

    std::uint32_t last_stream_id() const {
        return m_last_stream_id;
    }

    bool draining() const {
        return m_goaway_received || m_goaway_sent;
    }

    // --- stream operations (called by Http2ClientStream) ---

    asio::awaitable<error_code> stream_write(std::uint32_t id, std::string data, bool last) {
        co_await hop();
        auto it = m_streams.find(id);
        if (it == m_streams.end())
            co_return make_error_code(client_errc::session_closed);
        Stream& st = it->second;
        if (st.write_error)
            co_return st.write_error;  // a failed write poisons the stream
        if (st.reset || st.failed)
            co_return st.error ? st.error : make_error_code(client_errc::stream_reset);
        if (st.local_end || st.out_finished)
            co_return make_error_code(client_errc::body_not_streaming);

        if (!data.empty()) {
            // Backpressure: park while this stream's queue is over the high mark,
            // so a fast writer is paced by the peer's window.
            if (auto ec = co_await await_out_space(st); ec) {
                st.write_error = ec;
                co_return ec;
            }
            st.out_queued += data.size();
            st.out_queue.push_back(std::move(data));
        }
        if (last)
            st.out_finished = true;
        flush();
        co_return error_code{};
    }

    // Waits for the response head without consuming body bytes: what read_head()
    // needs, and what read() calls first when the caller goes straight to the
    // body. The head itself is published to the caller's handle when it is parsed.
    asio::awaitable<error_code> await_stream_head(std::uint32_t id) {
        co_await hop();
        for (;;) {
            auto it = m_streams.find(id);
            if (it == m_streams.end()) {
                // Retired already: the head was published to the handle when it
                // was parsed, so reaching here means there never was one.
                co_return make_error_code(client_errc::protocol_error);
            }
            Stream& st = it->second;
            if (st.head_seen)
                co_return error_code{};
            if (st.failed || st.reset)
                co_return outcome_error(*st.outcome);
            if (st.remote_end)
                co_return make_error_code(client_errc::protocol_error);  // END_STREAM, no HEADERS

            if (!st.in_notify) {
                st.in_notify =
                    std::make_shared<asio::experimental::concurrent_channel<void(error_code)>>(m_executor, 1);
            }
            auto notify = st.in_notify;
            auto [ec] = co_await notify->async_receive(asio::as_tuple(asio::use_awaitable));
            if (ec)
                co_return outcome_error(*st.outcome);
        }
    }

    asio::awaitable<std::expected<ReadResult, error_code>> stream_read(std::uint32_t id) {
        co_await hop();
        auto it = m_streams.find(id);
        if (it == m_streams.end())
            co_return std::unexpected{make_error_code(client_errc::session_closed)};
        Stream& st = it->second;

        // The head is not a body event: make sure it has been parsed (and thus
        // published to the handle), then hand back body bytes.
        if (!st.head_seen) {
            if (auto ec = co_await await_stream_head(id); ec)
                co_return std::unexpected{ec};
        }

        for (;;) {
            if (!st.in_queue.empty()) {
                std::string data = std::move(st.in_queue.front());
                st.in_queue.pop_front();
                st.in_bytes -= data.size();
                // Credit exactly what the application took: un-consumed bytes keep
                // the peer's window closed, which is what bounds our memory.
                st.recv_owed_conn -= static_cast<std::int64_t>(data.size());
                if (st.recv_owed_conn < 0)
                    st.recv_owed_conn = 0;
                credit_consumed(st, static_cast<std::int64_t>(data.size()));
                flush();
                co_return ReadResult::chunk(std::move(data));
            }
            if (st.failed || st.reset) {
                auto terminal = outcome_result(*st.outcome);
                erase_stream(it);
                co_return terminal;
            }
            if (st.remote_end) {
                st.outcome->set(StreamState::Eof);
                erase_stream(it);
                co_return ReadResult::end();
            }

            if (!st.in_notify) {
                st.in_notify =
                    std::make_shared<asio::experimental::concurrent_channel<void(error_code)>>(m_executor, 1);
            }
            auto notify = st.in_notify;
            auto [ec] = co_await notify->async_receive(asio::as_tuple(asio::use_awaitable));
            if (ec) {
                // The channel was closed while we waited: the stream (or the whole
                // session) ended, and the outcome says how.
                co_return outcome_result(*st.outcome);
            }
        }
    }

    bool stream_finished(std::uint32_t id) const {
        auto it = m_streams.find(id);
        if (it == m_streams.end())
            return true;
        const Stream& st = it->second;
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
    struct Stream {
        std::uint32_t id{0};
        // Weak: the caller owns its stream handle, and the stream owns the
        // session — a strong handle here would be a reference cycle.
        std::weak_ptr<Http2ClientStream<Transport>> handle;
        std::shared_ptr<StreamOutcome> outcome;

        // --- inbound (response) ---
        ResponseHead head;
        bool head_seen{false};  // a final header block was decoded (and published to the handle)
        bool block_end_stream{false};  // the HEADERS frame that started the block carried END_STREAM
        std::deque<std::string> in_queue;
        std::size_t in_bytes{0};
        std::shared_ptr<asio::experimental::concurrent_channel<void(error_code)>> in_notify;
        std::string header_block;  // accumulating HEADERS + CONTINUATION
        bool remote_end{false};    // the peer sent END_STREAM
        bool reset{false};         // RST_STREAM seen (or sent)
        bool failed{false};        // the stream cannot deliver more
        error_code error;

        // --- outbound (request body) ---
        std::deque<std::string> out_queue;
        std::size_t out_offset{0};
        std::size_t out_queued{0};
        bool out_finished{false};  // the caller finished the body
        bool local_end{false};     // our END_STREAM has been framed
        error_code write_error;
        std::shared_ptr<asio::experimental::concurrent_channel<void(error_code)>> out_space;
        std::int64_t send_window{kClientInitialWindow};

        // --- flow control ---
        std::int64_t recv_owed_conn{0};  // delivered but unconsumed: owes connection credit
        std::int64_t recv_pending{0};    // consumed: awaiting a batched WINDOW_UPDATE
    };

    // --- setup ---

    // Returns the seeded stream's handle (or null when there is no seed), so the
    // caller — not a weak reference — keeps it alive.
    asio::awaitable<std::expected<std::shared_ptr<ClientStream>, error_code>> start_impl(
        std::optional<RequestSpec> seed,
        std::string carried_over) {
        m_authority = m_target.authority();
        m_recv_buf = std::move(carried_over);
        // The prefacing bytes go out as one write: the 24-octet client preface
        // followed by our SETTINGS frame (RFC 9113 §3.4). Sending the preface and
        // SETTINGS together is fine — the peer's protocol detection reads the
        // preface first and the rest stays buffered.
        const std::int32_t initial = m_limits.h2_initial_window;
        std::string out{codec::kH2ClientPreface};
        append_frame(out, codec::H2FrameType::Settings, 0, 0, h2_settings_payload(m_limits));
        if (initial > kClientInitialWindow) {
            // Per-stream windows ride in SETTINGS; the connection-level window has
            // no setting, only a WINDOW_UPDATE, so open it up explicitly.
            std::string wu;
            append_u32(wu, static_cast<std::uint32_t>(initial - kClientInitialWindow));
            append_frame(out, codec::H2FrameType::WindowUpdate, 0, 0, wu);
        }
        m_conn_recv_window = initial;

        auto [ec, n] = co_await m_transport->async_write(std::as_bytes(std::span<const char>{out.data(), out.size()}));
        (void)n;
        if (ec)
            co_return std::unexpected{ec};

        // A seeded stream (h2c upgrade): the peer already has the request, so it
        // is only recorded here. Its response arrives as a normal h2 response.
        // The handle is handed back to the caller, which owns it from here on.
        std::shared_ptr<ClientStream> seeded;
        if (seed) {
            auto [it, inserted] = m_streams.try_emplace(1);
            (void)inserted;
            Stream& st = it->second;
            init_stream(st, 1);
            // The handle must be owned by *someone*: the session keeps only a weak
            // reference (a strong one would be a cycle), so the caller gets it.
            auto handle = std::make_shared<Http2ClientStream<Transport>>(this->shared_from_this(), 1, st.outcome);
            st.handle = handle;
            seeded = handle;
            st.local_end = true;
            st.out_finished = true;
            m_last_stream_id = 1;
            m_next_stream_id = 3;  // the upgraded request took stream 1
        }

        m_alive = true;
        m_deadline = std::chrono::steady_clock::now() + m_idle_timeout;
        flush();
        // The loops own the session while they run: a caller that drops its
        // handle (an idle pooled session has no other owner) must not leave them
        // running against a destroyed object.
        asio::co_spawn(
            m_executor,
            [self = this->shared_from_this()]() -> asio::awaitable<void> { co_await self->run_loops(); },
            asio::detached);
        co_return seeded;
    }

    void init_stream(Stream& st, std::uint32_t id) {
        st.id = id;
        st.outcome = std::make_shared<StreamOutcome>();
        st.send_window = m_peer_initial_window;
    }

    // Encodes a request head as HPACK, with the static table carrying what it can
    // (the pseudo-headers of a plain GET cost one byte each).
    error_code encode_request_head(const RequestSpec& spec, std::string& block) {
        const std::string_view method = to_string(spec.method);
        if (method.empty())
            return make_error_code(client_errc::protocol_error);
        const std::string_view target = spec.target.empty() ? std::string_view{"/"} : std::string_view{spec.target};
        if (target.front() != '/' || contains_ctl(target)) {
            return make_error_code(client_errc::protocol_error);  // :path must be origin-form
        }

        // RFC 7541 Appendix A: 2 = :method GET, 3 = :method POST, 4 = :path /,
        // 6 = :scheme http, 7 = :scheme https.
        if (spec.method == Method::Get) {
            codec::hpack_append_indexed(block, 2);
        } else if (spec.method == Method::Post) {
            codec::hpack_append_indexed(block, 3);
        } else {
            codec::hpack_append_literal(block, ":method", method);
        }
        codec::hpack_append_indexed(block, m_target.use_tls ? 7 : 6);
        codec::hpack_append_literal(block, ":authority", m_authority);
        if (target == "/") {
            codec::hpack_append_indexed(block, 4);
        } else {
            codec::hpack_append_literal(block, ":path", target);
        }

        bool saw_agent = false;
        for (const auto& [name, value] : spec.headers) {
            if (name.empty() || name.front() == ':' || is_connection_specific(name))
                continue;
            if (contains_ctl(name) || contains_ctl(value)) {
                SIMPLE_HTTP_ERROR_LOG("h2 client: dropping header '{}' with CR/LF/NUL", name);
                continue;
            }
            if (name == "user-agent")
                saw_agent = true;
            codec::hpack_append_literal(block, name, value);  // Headers keeps names lowercased
        }
        if (!saw_agent)
            codec::hpack_append_literal(block, "user-agent", client_version);
        if (!spec.body.empty() && !spec.stream_body) {
            // A known length: harmless on h2, and some origins want it.
            codec::hpack_append_literal(block, "content-length", std::to_string(spec.body.size()));
        }
        return error_code{};
    }

    // Fields HTTP/2 forbids (RFC 9113 §8.2.2): connection-specific ones, plus
    // `host`, whose job `:authority` does.
    static bool is_connection_specific(std::string_view name) {
        return name == "connection" || name == "keep-alive" || name == "proxy-connection" ||
               name == "transfer-encoding" || name == "upgrade" || name == "host";
    }

    // --- serve loops ---

    asio::awaitable<void> run_loops() {
        using namespace asio::experimental::awaitable_operators;
        co_await (read_loop() || write_loop() || watchdog());
        m_alive = false;
        // Release every writer parked on backpressure: the write loop is gone, so
        // nothing will ever drain their queues.
        for (auto& [id, st] : m_streams) {
            if (st.out_space)
                st.out_space->close();
            st.outcome->set(StreamState::Disconnect, make_error_code(asio::error::eof));
            if (st.in_notify)
                st.in_notify->close();
        }
        m_streams.clear();
        if (m_transport)
            m_transport->close();
        SIMPLE_HTTP_ERROR_LOG("h2 client: connection to {} closed", m_authority);
        co_return;
    }

    asio::awaitable<void> read_loop() {
        // Bytes handed over from the h2c upgrade (or an earlier read) are parsed
        // before anything new arrives.
        if (!m_recv_buf.empty() && !parse_available())
            co_return;
        std::array<std::byte, 32 * 1024> buf;  // no init: read_some fills [0,n)
        for (;;) {
            auto [ec, n] = co_await m_transport->async_read_some(std::span<std::byte>{buf});
            if (ec)
                co_return;  // EOF or a transport error ends the connection
            m_deadline = std::chrono::steady_clock::now() + m_idle_timeout;
            m_recv_buf.append(reinterpret_cast<const char*>(buf.data()), n);
            if (!parse_available())
                co_return;  // connection-fatal: GOAWAY queued
            flush();
        }
    }

    asio::awaitable<void> write_loop() {
        for (;;) {
            fill_data_frames();  // frame as much queued DATA as flow control allows
            while (!m_out.empty()) {
                std::string chunk;
                chunk.swap(m_out);
                m_deadline = std::chrono::steady_clock::now() + m_idle_timeout;
                auto [ec, n] =
                    co_await m_transport->async_write(std::as_bytes(std::span<const char>{chunk.data(), chunk.size()}));
                (void)n;
                if (ec)
                    co_return;
                fill_data_frames();  // a writer may have queued more while we wrote
            }
            if (m_goaway_sent && m_streams.empty())
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
                break;  // idle timeout elapsed
        }
        SIMPLE_HTTP_ERROR_LOG("h2 client: {} idle for {}ms", m_authority, m_idle_timeout.count());
        co_return;
    }

    void flush() {
        (void)m_notify.try_send(error_code{});
    }

    asio::awaitable<void> hop() {
        co_await asio::dispatch(asio::bind_executor(m_executor, asio::use_awaitable));
    }

    // --- frame output ---

    static void append_u32(std::string& out, std::uint32_t v) {
        out.push_back(static_cast<char>((v >> 24) & 0xFF));
        out.push_back(static_cast<char>((v >> 16) & 0xFF));
        out.push_back(static_cast<char>((v >> 8) & 0xFF));
        out.push_back(static_cast<char>(v & 0xFF));
    }

    static void append_frame(std::string& out,
                             codec::H2FrameType type,
                             std::uint8_t flags,
                             std::uint32_t stream_id,
                             std::string_view payload) {
        codec::serialize_frame_header(
            out, static_cast<std::uint32_t>(payload.size()), static_cast<std::uint8_t>(type), flags, stream_id);
        out.append(payload);
    }

    // Splits a header block into HEADERS + CONTINUATION frames, honouring the
    // peer's advertised frame size (a larger frame is a FRAME_SIZE_ERROR there).
    void submit_headers(std::uint32_t id, const std::string& block, bool end_stream) {
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
            append_frame(m_out,
                         first ? codec::H2FrameType::Headers : codec::H2FrameType::Continuation,
                         flags,
                         id,
                         std::string_view{block}.substr(off, take));
            first = false;
            off += take;
        } while (off < block.size());
    }

    void send_rst_stream(std::uint32_t id, std::uint32_t error) {
        std::string payload;
        append_u32(payload, error);
        append_frame(m_out, codec::H2FrameType::RstStream, 0, id, payload);
    }

    void send_window_update(std::uint32_t id, std::uint32_t delta) {
        std::string payload;
        append_u32(payload, delta);
        append_frame(m_out, codec::H2FrameType::WindowUpdate, 0, id, payload);
    }

    void go_away(std::uint32_t error) {
        if (m_goaway_sent)
            return;
        std::string payload;
        append_u32(payload, m_last_stream_id);
        append_u32(payload, error);
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
                break;  // wait for the rest
            std::string_view payload = std::string_view{m_recv_buf}.substr(pos + codec::kH2FrameHeaderSize, hdr.length);
            if (!handle_frame(hdr, payload))
                return false;
            pos = frame_end;
        }
        if (pos > 0)
            m_recv_buf.erase(0, pos);
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
                return true;  // ignored / unknown frame types are skipped
        }
    }

    static std::string_view strip_padding(std::string_view payload, bool padded, bool has_priority, bool& ok) {
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
    bool tolerate_unknown_stream(const codec::H2FrameHeader& hdr, std::int64_t credited_already) {
        const std::uint32_t id = hdr.stream_id;
        if (id == 0 || (id & 1u) == 0 || id >= m_next_stream_id) {
            SIMPLE_HTTP_ERROR_LOG("h2 client: frame type {} for unexpected stream {}", hdr.type, id);
            go_away(codec::H2_PROTOCOL_ERROR);
            return false;
        }
        if (static_cast<codec::H2FrameType>(hdr.type) == codec::H2FrameType::Data) {
            m_conn_recv_window += static_cast<std::int64_t>(hdr.length) - credited_already;
        }
        return true;
    }

    bool on_headers(const codec::H2FrameHeader& hdr, std::string_view payload) {
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
            // A skipped header block cannot be resumed by CONTINUATION, so an
            // unterminated one is fatal.
            if (!hdr.has_flag(codec::H2_FLAG_END_HEADERS)) {
                go_away(codec::H2_PROTOCOL_ERROR);
                return false;
            }
            return tolerate_unknown_stream(hdr, 0);
        }
        Stream& st = it->second;
        if (st.remote_end || st.failed || st.reset)
            return true;  // already over: ignore stragglers

        if (st.header_block.empty()) {
            // END_STREAM is only defined on the frame that starts a block.
            st.block_end_stream = hdr.has_flag(codec::H2_FLAG_END_STREAM);
        }
        st.header_block.append(block);
        if (st.header_block.size() > m_limits.max_header_bytes) {
            SIMPLE_HTTP_ERROR_LOG("h2 client: header block on stream {} exceeds {} bytes",
                                  hdr.stream_id,
                                  m_limits.max_header_bytes);
            go_away(codec::H2_ENHANCE_YOUR_CALM);
            return false;
        }
        if (hdr.has_flag(codec::H2_FLAG_END_HEADERS))
            return finish_header_block(hdr.stream_id);
        m_continuation_stream = hdr.stream_id;  // CONTINUATION frames follow
        return true;
    }

    bool on_continuation(const codec::H2FrameHeader& hdr, std::string_view payload) {
        if (hdr.stream_id == 0 || hdr.stream_id != m_continuation_stream) {
            go_away(codec::H2_PROTOCOL_ERROR);
            return false;
        }
        auto it = m_streams.find(hdr.stream_id);
        if (it == m_streams.end()) {
            go_away(codec::H2_PROTOCOL_ERROR);
            return false;
        }
        Stream& st = it->second;
        st.header_block.append(payload);
        if (st.header_block.size() > m_limits.max_header_bytes) {
            go_away(codec::H2_ENHANCE_YOUR_CALM);  // RFC 9113 §10.5.1: CONTINUATION flood
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
        Stream& st = it->second;

        std::vector<codec::HpackHeader> fields;
        if (!m_decoder.decode(st.header_block, fields)) {
            SIMPLE_HTTP_ERROR_LOG("h2 client: HPACK decode failed on stream {} (err={})", id, m_decoder.last_error());
            go_away(codec::H2_COMPRESSION_ERROR);  // the shared table is out of sync: fatal
            return false;
        }
        st.header_block.clear();
        const bool end_stream = st.block_end_stream;

        Headers headers;
        int status = 0;
        bool saw_status = false;
        for (auto& f : fields) {
            if (contains_ctl(f.name) || contains_ctl(f.value)) {  // §8.2.1
                reset_stream(id, codec::H2_PROTOCOL_ERROR);
                return true;
            }
            if (!f.name.empty() && f.name.front() == ':') {
                // `:status` must come first and only once; any other
                // pseudo-header in a response is malformed.
                if (f.name != ":status" || saw_status || !headers.empty()) {
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
            for (char c : f.name) {  // uppercase field names are malformed (§8.2.1)
                if (c >= 'A' && c <= 'Z') {
                    reset_stream(id, codec::H2_PROTOCOL_ERROR);
                    return true;
                }
            }
            if (is_connection_specific(f.name) || (f.name == "te" && f.value != "trailers")) {  // §8.2.2
                reset_stream(id, codec::H2_PROTOCOL_ERROR);
                return true;
            }
            headers.add_lower(std::move(f.name), std::move(f.value));
        }
        if (!saw_status) {
            reset_stream(id, codec::H2_PROTOCOL_ERROR);
            return true;
        }

        if (st.head_seen) {
            // A second header block is a trailers section: it can only end the
            // stream, never start a second response.
            st.remote_end = true;
            wake(st);
            return true;
        }
        if (status < 200) {
            // Informational (1xx): not the response, so drop it and keep waiting
            // for the final head (RFC 9113 §8.1).
            return true;
        }

        st.head.status = status;
        st.head.version = Version::Http2;
        st.head.headers = std::move(headers);
        // END_STREAM on the header block means the response has no body at all —
        // a response to HEAD, or a 204/304. It is never inferred from a missing
        // content-length.
        st.head.bodyless = end_stream;
        st.head_seen = true;
        // Publish the finished head to the caller's handle: read_head() then
        // works even after the stream has been retired (the session's entry is
        // gone by then, so the handle's own cache is what answers).
        if (auto handle = st.handle.lock()) handle->set_head(st.head);
        if (end_stream)
            st.remote_end = true;
        // The outcome stays Open until the application has actually seen the head
        // (stream_read delivers it before it reports the end).
        wake(st);
        return true;
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

    bool on_data(const codec::H2FrameHeader& hdr, std::string_view payload) {
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
            return tolerate_unknown_stream(hdr, frame_len);
        Stream& st = it->second;
        if (st.remote_end || st.failed || st.reset || !st.head_seen) {
            // After END_STREAM (or on an unknown/closed stream) the bytes are not
            // ours to deliver: hand the connection credit straight back.
            m_conn_recv_window += frame_len;
            if (!st.head_seen && !st.remote_end)
                reset_stream(hdr.stream_id, codec::H2_PROTOCOL_ERROR);
            return true;
        }

        const std::int64_t padding = frame_len - static_cast<std::int64_t>(data.size());
        if (padding > 0)
            credit_consumed(st, padding);  // never delivered, so credit it now
        if (!data.empty()) {
            st.recv_owed_conn += static_cast<std::int64_t>(data.size());
            st.in_bytes += data.size();
            st.in_queue.emplace_back(data);
        }
        if (hdr.has_flag(codec::H2_FLAG_END_STREAM))
            st.remote_end = true;
        wake(st);
        return true;
    }

    bool on_settings(const codec::H2FrameHeader& hdr, std::string_view payload) {
        if (hdr.has_flag(codec::H2_FLAG_ACK))
            return true;  // our SETTINGS was acked
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
                    if (value > 0x7FFFFFFFu) {  // RFC 9113 §6.5.2
                        go_away(codec::H2_FLOW_CONTROL_ERROR);
                        return false;
                    }
                    // Applies retroactively to every open stream's send window.
                    const std::int64_t delta = static_cast<std::int64_t>(value) - m_peer_initial_window;
                    m_peer_initial_window = static_cast<std::int32_t>(value);
                    for (auto& [sid, st] : m_streams)
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
                    break;
                case codec::H2_SETTINGS_HEADER_TABLE_SIZE:
                    // This bounds the table *our encoder* may use. We never index
                    // (every field is a literal), so there is nothing to resize;
                    // our decoder's table is governed by what we advertise.
                    break;
                default:
                    break;  // accepted and ignored
            }
        }
        std::string ack;
        append_frame(ack, codec::H2FrameType::Settings, codec::H2_FLAG_ACK, 0, {});
        m_out.append(ack);
        return true;
    }

    bool on_window_update(const codec::H2FrameHeader& hdr, std::string_view payload) {
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
        fill_data_frames();  // a window opened: more DATA may be frameable now
        return true;
    }

    bool on_rst_stream(const codec::H2FrameHeader& hdr, std::string_view payload) {
        if (hdr.stream_id == 0 || payload.size() != 4) {
            go_away(hdr.stream_id == 0 ? codec::H2_PROTOCOL_ERROR : codec::H2_FRAME_SIZE_ERROR);
            return false;
        }
        auto it = m_streams.find(hdr.stream_id);
        if (it == m_streams.end())
            return tolerate_unknown_stream(hdr, 0);
        const std::uint32_t error = codec::read_u32(payload, 0);
        it->second.reset = true;
        it_finish(it,
                  StreamState::Reset,
                  error == codec::H2_REFUSED_STREAM ? make_error_code(client_errc::stream_refused)
                                                    : make_error_code(client_errc::stream_reset));
        return true;
    }

    bool on_goaway(const codec::H2FrameHeader& hdr, std::string_view payload) {
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
        SIMPLE_HTTP_ERROR_LOG("h2 client: GOAWAY from {} (last_stream_id={}, error={})",
                              m_authority,
                              last_stream_id,
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

    bool on_ping(const codec::H2FrameHeader& hdr, std::string_view payload) {
        if (hdr.has_flag(codec::H2_FLAG_ACK))
            return true;
        if (payload.size() != 8) {
            go_away(codec::H2_FRAME_SIZE_ERROR);
            return false;
        }
        std::string ack;
        append_frame(ack, codec::H2FrameType::Ping, codec::H2_FLAG_ACK, 0, payload);  // echo
        m_out.append(ack);
        return true;
    }

    // --- flow control ---

    // Returns `n` octets of connection-level receive credit, batched: accumulate
    // until the threshold, then emit one WINDOW_UPDATE.
    void replenish_conn(std::int64_t n) {
        m_conn_recv_pending += n;
        if (m_conn_recv_pending >= m_limits.h2_initial_window / 2) {
            send_window_update(0, static_cast<std::uint32_t>(m_conn_recv_pending));
            m_conn_recv_window += m_conn_recv_pending;
            m_conn_recv_pending = 0;
        }
    }

    void credit_consumed(Stream& st, std::int64_t n) {
        st.recv_pending += n;
        if (st.recv_pending >= m_limits.h2_initial_window / 2) {
            send_window_update(st.id, static_cast<std::uint32_t>(st.recv_pending));
            st.recv_pending = 0;
        }
        replenish_conn(n);
    }

    void wake(Stream& st) {
        if (st.in_notify)
            (void)st.in_notify->try_send(error_code{});
    }

    // --- streams ---

    // Records a stream's terminal state on its outcome (so the handle keeps
    // answering) and wakes a reader parked on it.
    void it_finish(std::unordered_map<std::uint32_t, Stream>::iterator it, StreamState state, error_code ec = {}) {
        Stream& st = it->second;
        st.failed = st.failed || state == StreamState::Failed;
        st.outcome->set(state, ec);
        if (st.in_notify)
            (void)st.in_notify->try_send(error_code{});
        erase_stream(it);
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
    void erase_stream(std::unordered_map<std::uint32_t, Stream>::iterator it) {
        Stream& st = it->second;
        if (st.recv_owed_conn > 0) {
            replenish_conn(st.recv_owed_conn);
            st.recv_owed_conn = 0;
        }
        if (st.out_space)
            st.out_space->close();
        m_streams.erase(it);
        // Nothing in flight: the session is idle again, so the pool may have it
        // back (notify_idle checks reusability itself).
        if (m_streams.empty())
            notify_idle();
    }

    void fail_all_streams(client_errc code) {
        for (auto& [id, st] : m_streams) {
            st.failed = true;
            st.error = make_error_code(code);
            st.outcome->set(StreamState::Failed, st.error);
            if (st.out_space)
                st.out_space->close();
            if (st.in_notify)
                (void)st.in_notify->try_send(error_code{});
        }
    }

    // Parks a writer until this stream's outbound queue drains below the low
    // mark. Returns an error once the stream is gone, so a parked writer is never
    // stranded.
    asio::awaitable<error_code> await_out_space(Stream& st) {
        for (;;) {
            if (st.failed || st.reset) {
                co_return st.error ? st.error : make_error_code(client_errc::stream_reset);
            }
            if (st.out_queued <= kClientOutHighWatermark)
                co_return error_code{};
            if (!st.out_space) {
                st.out_space =
                    std::make_shared<asio::experimental::concurrent_channel<void(error_code)>>(m_executor, 1);
            }
            auto space = st.out_space;
            auto [ec] = co_await space->async_receive(asio::as_tuple(asio::use_awaitable));
            if (ec)
                co_return make_error_code(client_errc::session_closed);
        }
    }

    // --- send side: turn queued DATA into flow-controlled frames ---

    void fill_data_frames() {
        std::vector<std::uint32_t> finished;
        for (auto& [id, st] : m_streams) {
            if (st.local_end || st.reset || st.failed)
                continue;
            for (;;) {
                if (st.out_queue.empty()) {
                    if (st.out_finished) {
                        // The body is done: an empty DATA frame carries END_STREAM.
                        append_frame(m_out, codec::H2FrameType::Data, codec::H2_FLAG_END_STREAM, id, {});
                        st.local_end = true;
                        finished.push_back(id);
                        wake(st);
                    }
                    break;
                }
                const std::int64_t window = std::min<std::int64_t>(m_conn_send_window, st.send_window);
                if (window <= 0)
                    break;  // blocked on flow control until WINDOW_UPDATE

                std::string& front = st.out_queue.front();
                const std::size_t available = front.size() - st.out_offset;
                const std::size_t budget = static_cast<std::size_t>(
                    std::min<std::int64_t>(window,
                                           static_cast<std::int64_t>(std::min<std::uint32_t>(
                                               m_peer_max_frame_size, m_limits.h2_max_frame_size))));
                const std::size_t take = std::min(available, budget);
                if (take == 0)
                    break;  // nothing can advance: stop rather than spin

                const bool front_done = (st.out_offset + take >= front.size());
                const bool last = front_done && st.out_queue.size() == 1 && st.out_finished;
                append_frame(m_out,
                             codec::H2FrameType::Data,
                             last ? codec::H2_FLAG_END_STREAM : 0,
                             id,
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

    // --- members (touched only on the connection executor) ---
    std::shared_ptr<Transport> m_transport;
    Executor m_executor;
    ClientTarget m_target;
    EngineLimits m_limits;
    std::chrono::milliseconds m_idle_timeout;
    std::string m_authority;

    asio::experimental::concurrent_channel<void(error_code)> m_notify;
    asio::steady_timer m_idle_timer;
    bool m_idle_armed{false};
    bool m_pooled{false};
    std::function<void()> m_on_idle;
    std::chrono::steady_clock::time_point m_deadline{};

    codec::HpackDecoder m_decoder;  // connection-scoped: the dynamic table is shared
    std::unordered_map<std::uint32_t, Stream> m_streams;

    std::string m_recv_buf;
    std::string m_out;

    std::uint32_t m_next_stream_id{1};  // client-initiated ids are odd
    std::uint32_t m_last_stream_id{0};
    std::uint32_t m_continuation_stream{0};

    std::int64_t m_conn_send_window{kClientInitialWindow};
    std::int64_t m_conn_recv_window{kClientInitialWindow};
    std::int64_t m_conn_recv_pending{0};
    std::int32_t m_peer_initial_window{kClientInitialWindow};
    std::uint32_t m_peer_max_frame_size{static_cast<std::uint32_t>(kClientMaxFrameSize)};
    std::uint32_t m_peer_max_concurrent_streams{0};  // 0 = no limit announced yet

    bool m_alive{false};
    bool m_goaway_sent{false};
    bool m_goaway_received{false};
};

}  // namespace simple_http
