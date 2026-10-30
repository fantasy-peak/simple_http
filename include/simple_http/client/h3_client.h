#pragma once

// HTTP/3 client session (RFC 9114), on nghttp3 over the QUIC connection.
//
// The mirror image of engine/h3/h3_engine.h: where the server adapter turns
// nghttp3 callbacks into requests for a Dispatcher, this one turns them into the
// response head/body a ClientStream reads, and turns a Request into
// `nghttp3_conn_submit_request`. Everything between — framing, QPACK, SETTINGS,
// stream state — is nghttp3's, exactly as on the server side.
//
// Three pieces:
//
//   * QuicClientUdp — one UDP socket, its receive loop and the DatagramSink the
//     QUIC connection writes into. A client has one peer, so there is no
//     demultiplexing table (that is the server endpoint's job).
//
//   * Http3ClientStream — one request stream's read side (a queue of body
//     chunks, the response head, trailers) and write side (the request body
//     queue nghttp3 pulls from). Mirrors client/h2_client.h.
//
//   * Http3ClientSession — implements ClientSession, quic::Protocol and
//     h3::H3CallbackSink. All of it runs on the single connection executor
//     (model A), so there are no locks; the stream handles hop onto it before
//     touching session state.
//
// Only compiled with SIMPLE_HTTP_ENABLE_HTTP3 (see client/http_client.h).

#include <nghttp3/nghttp3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
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

#include "../core/http_field.h" // is_connection_specific_field
#include "../core/http_method.h"
#include "../core/limits.h"
#include "../core/logging.h"
#include "../core/resolve.h"
#include "../core/types.h"
#include "../core/version.h"
#include "../engine/h3/h3_callbacks.h"
#include "../engine/h3/h3_stream.h" // Channel / Signal / wake / water marks
#include "../proto/headers.h"
#include "../proto/request.h"
#include "../quic/connection.h"
#include "client_config.h"
#include "client_stream.h"
#include "tls_client.h"

namespace simple_http::detail {

namespace asio = boost::asio;

// nghttp3 settings the server adapter also uses; kept local so this header does
// not depend on engine/h3/h3_engine.h (which would pull the whole Dispatcher).
inline constexpr std::size_t kH3ClientQpackMaxTableCapacity = 4096;
inline constexpr std::size_t kH3ClientQpackBlockedStreams = 16;

// The UDP transport for one QUIC client connection.
//
// The connection hands finished datagrams to `sink()` from its own executor; the
// socket's receive loop feeds them back through `on_datagram`. Both are on the
// same pinned executor, so no queue or lock is needed.
template <typename Executor> class QuicClientUdp : public std::enable_shared_from_this<QuicClientUdp<Executor>> {
  public:
    using Connection = quic::QuicConnection<Executor>;

    explicit QuicClientUdp(std::shared_ptr<asio::ip::udp::socket> socket) : m_socket(std::move(socket)) {}

    QuicClientUdp(const QuicClientUdp &) = delete;
    QuicClientUdp &operator=(const QuicClientUdp &) = delete;

    // The DatagramSink the connection is built with. Each datagram gets its own
    // shared buffer because the async send outlives this call; UDP itself is
    // allowed to lose anything, so a failed send is simply dropped and QUIC's
    // retransmission covers it.
    quic::DatagramSink sink() {
        std::weak_ptr<QuicClientUdp> weak = this->weak_from_this();
        return [weak](quic::SendItem &&item) {
            auto self = weak.lock();
            if (!self || !self->m_socket->is_open())
                return;
            auto data = std::make_shared<std::vector<std::uint8_t>>(std::move(item.data));
            const auto to = item.to;
            self->m_socket->async_send_to(asio::buffer(*data), to,
                                          [data](const error_code &, std::size_t) { /* loss is QUIC's */ });
        };
    }

    // Feed datagrams into the connection until the socket is closed.
    asio::awaitable<void> receive_loop(std::shared_ptr<Connection> conn) {
        std::array<std::uint8_t, 65535> buf{};
        for (;;) {
            asio::ip::udp::endpoint from;
            auto [ec, n] =
                co_await m_socket->async_receive_from(asio::buffer(buf), from, asio::as_tuple(asio::use_awaitable));
            if (ec)
                co_return;
            conn->on_datagram(std::span<const std::uint8_t>{buf.data(), n}, from);
        }
    }

    // Unblocks receive_loop. The connection's run() calls this when it ends.
    void close() {
        error_code ec;
        m_socket->close(ec);
    }

  private:
    std::shared_ptr<asio::ip::udp::socket> m_socket;
};

template <typename Executor> class Http3ClientSession;

// One HTTP/3 request, from the caller's side. Holds the shared outcome so a
// handle kept past the session's table entry still reports the truth.
template <typename Executor> class Http3ClientStream final : public ClientStream {
  public:
    Http3ClientStream(std::shared_ptr<Http3ClientSession<Executor>> session, std::int64_t id,
                      std::shared_ptr<StreamOutcome> outcome)
        : m_session(std::move(session)), m_id(id), m_outcome(std::move(outcome)) {}

    asio::awaitable<error_code> write(std::string data) override {
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

    bool finished() const override { return m_outcome->get() != StreamState::Open; }

    Version version() const override { return Version::Http3; }

    std::uint32_t id() const override { return static_cast<std::uint32_t>(m_id); }

    asio::awaitable<void> cancel() override {
        co_await asio::dispatch(asio::bind_executor(m_session->get_executor(), asio::use_awaitable));
        m_session->stream_cancel(m_id);
    }

  private:
    friend class Http3ClientSession<Executor>; // publishes the head/trailers here

    asio::awaitable<error_code> await_head() override { co_return co_await m_session->await_stream_head(m_id); }

    // Strong: the caller's stream keeps the connection alive. The session refers
    // back to its streams weakly, so there is no cycle.
    std::shared_ptr<Http3ClientSession<Executor>> m_session;
    std::int64_t m_id;
    std::shared_ptr<StreamOutcome> m_outcome;
};

template <typename Executor>
class Http3ClientSession final : public ClientSession,
                                 public quic::Protocol,
                                 public h3::H3CallbackSink,
                                 public std::enable_shared_from_this<Http3ClientSession<Executor>> {
  public:
    using Connection = quic::QuicConnection<Executor>;
    using Udp = QuicClientUdp<Executor>;
    using Channel = h3::Channel;
    using Clock = std::chrono::steady_clock;
    using Signal = h3::Signal;

    // Dial and start a QUIC connection to `target`.
    //
    // `ssl_ctx` is the client's already-configured TLS context (verification,
    // CA, client certificate); `hs` carries SNI / host verification and the
    // ALPN list, which for HTTP/3 must be exactly `h3`. The returned session has
    // completed the handshake and has its control/QPACK streams open, or the
    // expected holds why it could not.
    static asio::awaitable<std::expected<std::shared_ptr<Http3ClientSession>, error_code>>
    connect(Executor ex, SSL_CTX *ssl_ctx, const ClientTarget &target, const ClientTlsHandshake &hs,
            const EngineLimits &limits, std::chrono::milliseconds connect_timeout,
            std::chrono::milliseconds idle_timeout, const ResolveFn &resolve_fn) {
        if (!target.use_tls) {
            // QUIC carries its own TLS 1.3: there is no plaintext form.
            co_return std::unexpected{make_error_code(client_errc::version_not_negotiated)};
        }

        auto endpoints = co_await resolve_endpoints(resolve_fn, target.host, target.effective_port(), ex);
        if (endpoints.first) {
            co_return std::unexpected{endpoints.first};
        }
        if (endpoints.second.empty()) {
            co_return std::unexpected{make_error_code(asio::error::host_not_found)};
        }

        // Try each address in turn. There is no socket-level connect to fail
        // fast on (UDP), so an unreachable first address would otherwise burn
        // the whole budget. IPv4 and IPv6 are interleaved (v4 first) and each
        // non-final attempt is capped at kAttemptCap: a dead IPv6 route or a bad
        // anycast address then costs seconds, not the whole deadline. The last
        // address still gets everything that is left.
        std::vector<asio::ip::udp::endpoint> ordered;
        ordered.reserve(endpoints.second.size());
        const auto v4_end = std::stable_partition(endpoints.second.begin(), endpoints.second.end(),
                                                  [](const asio::ip::tcp::endpoint &e) { return e.address().is_v4(); });
        for (auto v4 = endpoints.second.begin(), v6 = v4_end; v4 != v4_end || v6 != endpoints.second.end();) {
            if (v4 != v4_end) {
                ordered.emplace_back(v4->address(), v4->port());
                ++v4;
            }
            if (v6 != endpoints.second.end()) {
                ordered.emplace_back(v6->address(), v6->port());
                ++v6;
            }
        }

        constexpr auto kAttemptCap = std::chrono::seconds(5);
        const auto deadline = connect_timeout.count() > 0 ? Clock::now() + connect_timeout : Clock::time_point::max();
        error_code last_error = make_error_code(asio::error::host_not_found);
        for (std::size_t i = 0; i < ordered.size(); ++i) {
            std::chrono::milliseconds slice = connect_timeout;
            if (connect_timeout.count() > 0) {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
                if (remaining.count() <= 0)
                    co_return std::unexpected{make_error_code(client_errc::connect_timeout)};
                slice = remaining;
                if (i + 1 < ordered.size() && slice > kAttemptCap)
                    slice = std::chrono::duration_cast<std::chrono::milliseconds>(kAttemptCap);
            }
            auto attempt = co_await connect_one(ex, ssl_ctx, target, hs, limits, idle_timeout, ordered[i], slice);
            if (attempt)
                co_return attempt;
            last_error = attempt.error();
            SIMPLE_HTTP_ERROR_LOG("h3 client: {} failed ({}), trying the next address",
                                  ordered[i].address().to_string(), last_error.message());
        }
        co_return std::unexpected{last_error};
    }

    // One address, one QUIC connection attempt.
    static asio::awaitable<std::expected<std::shared_ptr<Http3ClientSession>, error_code>>
    connect_one(Executor ex, SSL_CTX *ssl_ctx, const ClientTarget &target, const ClientTlsHandshake &hs,
                const EngineLimits &limits, std::chrono::milliseconds idle_timeout,
                const asio::ip::udp::endpoint &remote, std::chrono::milliseconds attempt_timeout) {
        auto socket = std::make_shared<asio::ip::udp::socket>(ex);
        error_code ec;
        socket->open(remote.protocol(), ec);
        if (!ec)
            socket->bind(
                asio::ip::udp::endpoint(remote.address().is_v6() ? asio::ip::udp::v6() : asio::ip::udp::v4(), 0), ec);
        if (ec) {
            SIMPLE_HTTP_ERROR_LOG("h3 client: UDP socket setup failed: {}", ec.message());
            co_return std::unexpected{ec};
        }
        const asio::ip::udp::endpoint local = socket->local_endpoint(ec);
        if (ec) {
            co_return std::unexpected{ec};
        }

        auto udp = std::make_shared<Udp>(socket);

        quic::QuicConnectionConfig qcfg;
        if (idle_timeout.count() > 0) {
            qcfg.max_idle_timeout_ms = static_cast<std::uint64_t>(idle_timeout.count());
        }
        // A client is the receiver: these are the windows *we* advertise, and
        // they pace a large response. The server defaults (1 MiB connection /
        // 256 KiB stream) are sized for many small requests; a single 1.3 MB
        // page would otherwise wait on window updates every 256 KiB. 4 MiB /
        // 1 MiB matches the h2 client's per-stream window and halves the
        // round trips.
        qcfg.initial_max_data = 4u << 20;
        qcfg.initial_max_stream_data_bidi_remote = 1u << 20;

        quic::QuicClientOptions options;
        options.configure_ssl = [hs](SSL *ssl) -> error_code {
            // QUIC is TLS 1.3 or nothing (RFC 9001 §4.2).
            if (SSL_set_min_proto_version(ssl, TLS1_3_VERSION) != 1 ||
                SSL_set_max_proto_version(ssl, TLS1_3_VERSION) != 1) {
                return make_error_code(asio::error::invalid_argument);
            }
            return apply_client_ssl_settings(ssl, hs);
        };

        auto conn = std::make_shared<Connection>(ex, ssl_ctx, qcfg, std::move(options), local, remote, udp->sink());
        if (!conn->init()) {
            socket->close(ec);
            co_return std::unexpected{make_error_code(client_errc::protocol_error)};
        }

        auto session = std::make_shared<Http3ClientSession>(ex, std::move(conn), std::move(udp), target, limits);
        SIMPLE_HTTP_DEBUG_LOG("h3 client: connection built, waiting for handshake");
        if (auto start_ec = co_await session->start(attempt_timeout))
            co_return std::unexpected{start_ec};
        SIMPLE_HTTP_DEBUG_LOG("h3 client: handshake complete and h3 ready");
        co_return session;
    }

    Http3ClientSession(Executor ex, std::shared_ptr<Connection> conn, std::shared_ptr<Udp> udp,
                       const ClientTarget &target, const EngineLimits &limits)
        : m_executor(std::move(ex)), m_conn(std::move(conn)), m_udp(std::move(udp)), m_authority(target.authority()),
          m_limits(limits) {}

    ~Http3ClientSession() override {
        if (m_h3 != nullptr)
            nghttp3_conn_del(m_h3);
    }

    Http3ClientSession(const Http3ClientSession &) = delete;
    Http3ClientSession &operator=(const Http3ClientSession &) = delete;

    [[nodiscard]] Executor get_executor() const { return m_executor; }

    // --- ClientSession ---

    asio::awaitable<std::expected<std::shared_ptr<ClientStream>, error_code>>
    open_stream(std::shared_ptr<Request> request) override {
        co_await hop();
        if (!m_alive)
            co_return std::unexpected{make_error_code(client_errc::session_closed)};
        if (m_goaway_received)
            co_return std::unexpected{make_error_code(client_errc::goaway)};
        if (h3() == nullptr)
            co_return std::unexpected{make_error_code(client_errc::version_not_negotiated)};
        if (request->stream_body() && request->content_length() >= 0)
            co_return std::unexpected{make_error_code(client_errc::invalid_spec)};
        if (request->close())
            m_close_requested = true;

        const auto id = m_conn->open_bidi_stream();
        if (!id)
            co_return std::unexpected{make_error_code(client_errc::too_many_streams)};

        auto fresh = std::make_shared<Stream>();
        fresh->outcome = std::make_shared<StreamOutcome>();
        fresh->is_head = request->method() == Method::Head;
        // Capacity-1 coalescing wake-ups on the session executor; see Stream.
        fresh->in_space = std::make_shared<Channel>(m_executor, 1);
        fresh->out_space = std::make_shared<Channel>(m_executor, 1);
        m_streams.emplace(*id, fresh);
        m_active_streams.fetch_add(1, std::memory_order_release);

        // The up-front body (a non-streaming request): nghttp3 pulls it from the
        // queue below, so read it before submitting the head. This awaits, so no
        // iterator or Stream& may be held across it — the map can be rehashed by
        // another exchange opening on this same executor.
        std::string up_front;
        // Only a declared, positive length means the Body has bytes to read: an
        // absent body (-1) has nothing fed, and reading it would wait forever.
        if (!request->stream_body() && request->content_length() > 0) {
            auto body = co_await request->body().read_all();
            if (!body) {
                if (auto it = m_streams.find(*id); it != m_streams.end())
                    erase_stream(it);
                co_return std::unexpected{body.error()};
            }
            up_front = std::move(*body);
        }

        auto it = m_streams.find(*id);
        if (it == m_streams.end())
            co_return std::unexpected{make_error_code(client_errc::session_closed)};
        std::shared_ptr<Stream> st = it->second;
        if (!request->stream_body()) {
            if (!up_front.empty()) {
                st->out_bytes = up_front.size();
                st->out_q.push_back(std::move(up_front));
            }
            st->out_eof = true; // the whole body is queued (or there is none)
        }

        std::vector<nghttp3_nv> nva;
        std::deque<std::string> owned; // stable addresses: nva points into it
        if (auto ec = build_request_headers(*request, nva, owned)) {
            erase_stream(it);
            co_return std::unexpected{ec};
        }

        auto handle = std::make_shared<Http3ClientStream<Executor>>(this->shared_from_this(), *id, st->outcome);
        st->handle = handle;

        // A reader is installed whenever bytes have to be delivered: a streaming
        // request keeps producing later, and an up-front body is already queued.
        // A non-streaming request with no body omits it, which is nghttp3's way
        // of saying "HEADERS carries END_STREAM".
        nghttp3_data_reader reader{};
        const nghttp3_data_reader *reader_ptr = nullptr;
        if (request->stream_body() || !st->out_q.empty()) {
            reader.read_data = &Http3ClientSession::read_request_data_trampoline;
            reader_ptr = &reader;
        }
        const int rv = nghttp3_conn_submit_request(h3(), *id, nva.data(), nva.size(), reader_ptr, nullptr);
        if (rv != 0) {
            SIMPLE_HTTP_ERROR_LOG("h3 client: submit_request failed (stream={}): {}", *id, nghttp3_strerror(rv));
            erase_stream(it);
            co_return std::unexpected{make_error_code(client_errc::protocol_error)};
        }
        m_conn->flush();
        SIMPLE_HTTP_DEBUG_LOG("h3 client: request submitted (stream={})", *id);
        co_return handle;
    }

    bool alive() const override { return m_alive; }

    bool reusable() const override {
        return m_alive && !m_goaway_received && !m_close_requested &&
               m_active_streams.load(std::memory_order_acquire) == 0;
    }

    bool keepable() const override { return m_alive && !m_goaway_received && !m_close_requested; }

    bool multiplexes() const override { return true; }

    Version version() const override { return Version::Http3; }

    std::string_view authority() const override { return m_authority; }

    asio::any_io_executor executor() const override { return m_executor; }

    void close() override {
        asio::post(m_executor, [self = this->shared_from_this()] { self->shutdown_now(); });
    }

    // --- stream operations (called by Http3ClientStream, already hopped) ---

    asio::awaitable<error_code> stream_write(std::int64_t id, std::string data, bool last) {
        co_await hop();
        {
            auto it = m_streams.find(id);
            if (it == m_streams.end())
                co_return make_error_code(client_errc::session_closed);
            Stream &st = *it->second;
            if (st.reset || st.write_shut)
                co_return make_error_code(client_errc::stream_reset);
            if (st.out_eof)
                co_return make_error_code(client_errc::body_not_streaming);
        }
        if (!data.empty()) {
            if (auto ec = co_await await_out_space(id); ec)
                co_return ec;
            auto it = m_streams.find(id);
            if (it == m_streams.end())
                co_return make_error_code(client_errc::session_closed);
            it->second->out_bytes += data.size();
            it->second->out_q.push_back(std::move(data));
        }
        if (last) {
            auto it = m_streams.find(id);
            if (it == m_streams.end())
                co_return make_error_code(client_errc::session_closed);
            it->second->out_eof = true;
        }
        resume_stream(id);
        m_conn->flush();
        co_return error_code{};
    }

    asio::awaitable<error_code> await_stream_head(std::int64_t id) {
        co_await hop();
        for (;;) {
            auto it = m_streams.find(id);
            if (it == m_streams.end()) {
                // Retired already: the head was published to the handle when it
                // was parsed, so reaching here means there never was one.
                co_return make_error_code(client_errc::protocol_error);
            }
            Stream &st = *it->second;
            // An interim 100-continue is handed to the caller first: it is the
            // "send your body now" signal. The cached head on the handle is
            // invalidated so the next read_head() waits for the final response,
            // exactly like the HTTP/1.1 session.
            if (st.interim) {
                ResponseHead interim = std::move(*st.interim);
                st.interim.reset();
                if (auto handle = st.handle.lock()) {
                    handle->set_head(std::move(interim));
                    handle->reset_head();
                }
                co_return error_code{};
            }
            if (st.head_seen)
                co_return error_code{};
            if (st.failed || st.reset || st.outcome->get() != StreamState::Open)
                co_return outcome_error(*st.outcome);
            if (st.remote_end)
                co_return make_error_code(client_errc::protocol_error); // FIN, no HEADERS

            auto wake = st.in_space;
            auto [ec] = co_await wake->async_receive(asio::as_tuple(asio::use_awaitable));
            // A wake (state change) arrives with no error. Anything else is a
            // cancelled/failed wait — typically the caller's deadline cancelling
            // this coroutine — and re-waiting would spin under terminal
            // cancellation, so report it.
            if (ec)
                co_return ec;
        }
    }

    asio::awaitable<std::expected<ReadResult, error_code>> stream_read(std::int64_t id) {
        co_await hop();
        if (!m_streams.contains(id))
            co_return std::unexpected{make_error_code(client_errc::session_closed)};

        if (auto it = m_streams.find(id); it != m_streams.end() && !it->second->head_seen) {
            if (auto ec = co_await await_stream_head(id))
                co_return std::unexpected{ec};
        }

        // Re-resolve every pass: an await can retire this stream.
        for (;;) {
            auto it = m_streams.find(id);
            if (it == m_streams.end())
                co_return std::unexpected{make_error_code(client_errc::session_closed)};
            Stream &st = *it->second;
            if (!st.in_queue.empty()) {
                std::string data = std::move(st.in_queue.front());
                st.in_queue.pop_front();
                st.in_bytes -= data.size();
                // Credit exactly what the application took: un-consumed bytes
                // keep the peer's window closed, which is what bounds memory.
                m_conn->extend_stream_offset(id, data.size());
                m_conn->extend_connection_offset(data.size());
                m_conn->flush();
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
            // The connection went away (or the stream failed) without ending the
            // body: report that rather than parking on a wake that never comes.
            if (st.outcome->get() != StreamState::Open) {
                auto terminal = outcome_result(*st.outcome);
                erase_stream(it);
                co_return terminal;
            }

            auto wake = st.in_space;
            auto [ec] = co_await wake->async_receive(asio::as_tuple(asio::use_awaitable));
            if (ec)
                co_return std::unexpected{ec};
        }
    }

    void stream_cancel(std::int64_t id) {
        asio::dispatch(m_executor, [self = this->shared_from_this(), id] {
            auto it = self->m_streams.find(id);
            if (it == self->m_streams.end())
                return;
            self->m_conn->reset_stream(id, NGHTTP3_H3_REQUEST_CANCELLED);
            self->it_finish(it, StreamState::Reset, make_error_code(client_errc::stream_reset));
            self->erase_stream(it);
            self->m_conn->flush();
        });
    }

    // --- quic::Protocol: write side ---

    quic::StreamData next_stream_data() noexcept override {
        quic::StreamData out;
        if (h3() == nullptr)
            return out;

        std::int64_t stream_id = -1;
        int fin = 0;
        std::array<nghttp3_vec, 8> vec{};
        const nghttp3_ssize count = nghttp3_conn_writev_stream(m_h3, &stream_id, &fin, vec.data(), vec.size());
        if (count < 0) {
            m_h3_dead = true;
            out.error = nghttp3_err_infer_quic_app_error_code(static_cast<int>(count));
            return out;
        }
        if (stream_id < 0)
            return out;

        m_vec_count = std::min<std::size_t>(static_cast<std::size_t>(count), m_vec.size());
        for (std::size_t i = 0; i < m_vec_count; ++i) {
            m_vec[i].base = vec[i].base;
            m_vec[i].len = vec[i].len;
        }
        out.stream_id = stream_id;
        out.fin = fin;
        out.vec = std::span<const quic::ByteVec>(m_vec.data(), m_vec_count);
        return out;
    }

    void on_stream_data_written(std::int64_t stream_id, std::size_t datalen) noexcept override {
        if (auto it = m_streams.find(stream_id); it != m_streams.end())
            h3::wake(it->second->out_space);
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
        if (auto it = m_streams.find(stream_id); it != m_streams.end()) {
            it->second->write_shut = true;
            h3::wake(it->second->out_space);
        }
        if (h3() != nullptr)
            nghttp3_conn_shutdown_stream_write(m_h3, stream_id);
    }

    // --- quic::Protocol: read side and events ---

    void on_stream_data(std::uint32_t flags, std::int64_t stream_id,
                        std::span<const std::uint8_t> data) noexcept override {
        if (h3() == nullptr)
            return;

        const bool fin = (flags & quic::kStreamDataFin) != 0;
        // The return value excludes the DATA payload (nghttp3's recv_data
        // contract: the application credits that itself), so it is not a
        // "leftover bytes" count — the whole slice has been parsed.
        const nghttp3_ssize nconsumed =
            nghttp3_conn_read_stream2(h3(), stream_id, data.data(), data.size(), fin ? 1 : 0, m_conn->now());
        if (nconsumed < 0) {
            m_h3_dead = true;
            SIMPLE_HTTP_ERROR_LOG("h3 client: read_stream2 failed: {} (stream={})",
                                  nghttp3_strerror(static_cast<int>(nconsumed)), stream_id);
            m_conn->close(nghttp3_err_infer_quic_app_error_code(static_cast<int>(nconsumed)), "http3");
            return;
        }
        m_conn->extend_stream_offset(stream_id, static_cast<std::uint64_t>(nconsumed));
        m_conn->extend_connection_offset(static_cast<std::uint64_t>(nconsumed));
    }

    void on_acked_stream_data(std::int64_t stream_id, std::uint64_t datalen) noexcept override {
        if (h3() != nullptr)
            (void)nghttp3_conn_add_ack_offset(m_h3, stream_id, datalen);
    }

    void on_stream_close(std::int64_t stream_id, std::optional<std::uint64_t> rx_error,
                         std::optional<std::uint64_t> tx_error) noexcept override {
        if (h3() != nullptr) {
            const std::uint64_t code = rx_error.value_or(tx_error.value_or(0));
            const int rv = nghttp3_conn_close_stream(m_h3, stream_id, code);
            if (rv != 0 && rv != NGHTTP3_ERR_STREAM_NOT_FOUND) {
                SIMPLE_HTTP_ERROR_LOG("h3 client: close_stream failed: {} (stream={})", nghttp3_strerror(rv),
                                      stream_id);
            }
        }
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end())
            return;
        // The stream is finished on both sides, but body already decoded may
        // still be queued: keep the entry so read() drains it, and erase lazily.
        it->second->remote_end = true;
        if (it->second->outcome->get() == StreamState::Open && it->second->in_queue.empty())
            it->second->outcome->set(StreamState::Eof);
        h3::wake(it->second->in_space);
        h3::wake(it->second->out_space);
    }

    void on_stream_reset(std::int64_t stream_id, std::uint64_t /*app_error_code*/) noexcept override {
        auto it = m_streams.find(stream_id);
        if (it != m_streams.end()) {
            it->second->reset = true;
            it->second->outcome->set(StreamState::Reset);
            h3::wake(it->second->in_space);
            h3::wake(it->second->out_space);
        }
        if (h3() != nullptr)
            (void)nghttp3_conn_shutdown_stream_read(m_h3, stream_id);
    }

    void on_stream_stop_sending(std::int64_t stream_id, std::uint64_t /*app_error_code*/) noexcept override {
        auto it = m_streams.find(stream_id);
        if (it != m_streams.end()) {
            // The peer no longer wants our request body.
            it->second->write_shut = true;
            h3::wake(it->second->out_space);
        }
        if (h3() != nullptr)
            nghttp3_conn_shutdown_stream_write(m_h3, stream_id);
    }

    void on_extend_max_stream_data(std::int64_t stream_id, std::uint64_t /*max_data*/) noexcept override {
        if (h3() != nullptr)
            (void)nghttp3_conn_unblock_stream(m_h3, stream_id);
    }

    void on_extend_max_remote_streams_bidi(std::uint64_t /*max_streams*/) noexcept override {
        // The stream table (and its MAX_STREAMS accounting) is ngtcp2's; the
        // client never calls nghttp3_conn_set_max_client_streams_bidi, which is
        // a server-only API (nghttp3 asserts `conn->server`).
    }

    // The 1-RTT keys may be installed before the peer's transport parameters
    // (and thus its unidirectional-stream credit) are applied on the client
    // side, so the nghttp3 connection is created on handshake completion
    // instead — see Protocol::on_handshake_complete.
    void on_tx_keys_ready() noexcept override {}

    void on_handshake_complete() noexcept override {
        if (m_h3 != nullptr)
            return; // idempotent: a late set_protocol replay can reach here twice

        if (m_conn->streams_uni_left() < 3) {
            SIMPLE_HTTP_ERROR_LOG("h3 client: peer allows fewer than 3 unidirectional streams (left={})",
                                  m_conn->streams_uni_left());
            fail_ready(make_error_code(client_errc::version_not_negotiated));
            return;
        }
        // QUIC mandates ALPN, and HTTP/3 is only meaningful if it selected h3.
        if (m_conn->alpn_selected() != quic::kHttp3Alpn) {
            SIMPLE_HTTP_ERROR_LOG("h3 client: peer selected ALPN '{}', not 'h3'", m_conn->alpn_selected());
            fail_ready(make_error_code(client_errc::version_not_negotiated));
            return;
        }

        nghttp3_settings settings;
        nghttp3_settings_default(&settings);
        settings.max_field_section_size = m_limits.max_header_bytes;
        settings.qpack_max_dtable_capacity = kH3ClientQpackMaxTableCapacity;
        settings.qpack_blocked_streams = kH3ClientQpackBlockedStreams;
        settings.qpack_encoder_max_dtable_capacity = 0;

        // The user data is the sink subobject, not `this` (three bases): the
        // trampolines cast it back to H3CallbackSink* and must receive the
        // adjusted pointer.
        if (nghttp3_conn_client_new(&m_h3, &h3::h3_callbacks(), &settings, nghttp3_mem_default(),
                                    static_cast<h3::H3CallbackSink *>(this)) != 0) {
            SIMPLE_HTTP_ERROR_LOG("h3 client: nghttp3_conn_client_new failed");
            fail_ready(make_error_code(client_errc::protocol_error));
            return;
        }
        const std::optional<std::int64_t> control = m_conn->open_uni_stream();
        if (!control || nghttp3_conn_bind_control_stream(m_h3, *control) != 0) {
            SIMPLE_HTTP_ERROR_LOG("h3 client: could not open the control stream");
            fail_ready(make_error_code(client_errc::protocol_error));
            return;
        }
        const std::optional<std::int64_t> qpack_enc = m_conn->open_uni_stream();
        const std::optional<std::int64_t> qpack_dec = m_conn->open_uni_stream();
        if (!qpack_enc || !qpack_dec || nghttp3_conn_bind_qpack_streams(m_h3, *qpack_enc, *qpack_dec) != 0) {
            SIMPLE_HTTP_ERROR_LOG("h3 client: could not open the QPACK streams");
            fail_ready(make_error_code(client_errc::protocol_error));
            return;
        }
        m_alive = true;
        SIMPLE_HTTP_DEBUG_LOG("h3 client: h3 connection initialised (left={}, alpn={})", m_conn->streams_uni_left(),
                              m_conn->alpn_selected());
        h3::wake(m_ready);
        m_conn->flush();
    }

    void on_connection_closed() noexcept override {
        m_alive = false;
        m_conn_closed = true;
        // Closed before the handshake produced h3: report a transport failure
        // rather than a stale default. (fail_ready's specific diagnosis wins.)
        if (m_h3 == nullptr && !m_failure_set)
            m_failure = make_error_code(asio::error::connection_refused);
        for (auto &[id, stream] : m_streams) {
            if (stream->outcome->get() == StreamState::Open)
                stream->outcome->set(StreamState::Disconnect);
            h3::wake(stream->in_space);
            h3::wake(stream->out_space);
        }
        if (m_ready)
            (void)m_ready->try_send(error_code{});
    }

    // --- h3::H3CallbackSink ---

    bool on_headers_begin(std::int64_t stream_id) noexcept override {
        auto it = m_streams.find(stream_id);
        return it != m_streams.end();
    }

    void on_header(std::int64_t stream_id, std::int32_t /*token*/, std::string_view name,
                   std::string_view value) noexcept override {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end())
            return;
        Stream &st = *it->second;
        if (contains_ctl(name) || contains_ctl(value)) {
            st.failed = true;
            st.outcome->set(StreamState::Failed, make_error_code(client_errc::protocol_error));
            return;
        }
        if (!name.empty() && name.front() == ':') {
            if (name == ":status")
                st.status_text.assign(value);
            return; // other pseudo-headers are not response state we surface
        }
        st.header_tmp.emplace_back(std::string{name}, std::string{value});
    }

    void on_headers_end(std::int64_t stream_id, bool fin) noexcept override {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end())
            return;
        Stream &st = *it->second;

        // A section before the final head with a 1xx status is informational
        // (100-continue, 103 Early Hints); the final response follows. Only 100
        // is surfaced — it is what a caller waiting on `Expect` needs.
        const int status = parse_status(st.status_text);
        if (!st.head_seen && status >= 100 && status < 200) {
            if (status == 100) {
                ResponseHead interim;
                interim.status = 100;
                interim.version = Version::Http3;
                interim.bodyless = true;
                st.interim = std::move(interim);
            }
            st.header_tmp.clear();
            st.status_text.clear();
            h3::wake(st.in_space);
            return;
        }

        if (!st.head_seen) {
            if (status < 200 || status > 999) {
                st.failed = true;
                st.outcome->set(StreamState::Failed, make_error_code(client_errc::protocol_error));
                h3::wake(st.in_space);
                return;
            }
            st.head.status = status;
            st.head.version = Version::Http3;
            st.head.bodyless = fin || status == 204 || status == 304 || st.is_head;
            for (auto &[name, value] : st.header_tmp)
                st.head.headers.add_lower(std::move(name), std::move(value));
            st.header_tmp.clear();
            st.status_text.clear();
            st.head_seen = true;
            if (auto handle = st.handle.lock())
                handle->set_head(st.head);
            if (fin)
                st.remote_end = true;
            h3::wake(st.in_space);
            return;
        }

        // A second field section is the trailer section, which nghttp3 reports
        // through the dedicated callbacks below rather than here.
        st.header_tmp.clear();
        st.status_text.clear();
    }

    // --- trailer section (RFC 9110 §6.5, RFC 9114 §4.1) -------------------

    bool on_trailers_begin(std::int64_t stream_id) noexcept override { return m_streams.contains(stream_id); }

    void on_trailer(std::int64_t stream_id, std::int32_t /*token*/, std::string_view name,
                    std::string_view value) noexcept override {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end())
            return;
        Stream &st = *it->second;
        if (contains_ctl(name) || contains_ctl(value)) {
            st.failed = true;
            st.outcome->set(StreamState::Failed, make_error_code(client_errc::protocol_error));
            return;
        }
        if (!name.empty() && name.front() == ':')
            return; // pseudo-headers are not valid in a trailer section
        st.trailer_tmp.emplace_back(std::string{name}, std::string{value});
    }

    void on_trailers_end(std::int64_t stream_id, bool fin) noexcept override {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end())
            return;
        Stream &st = *it->second;
        if (auto handle = st.handle.lock()) {
            Headers trailers;
            for (auto &[name, value] : st.trailer_tmp)
                trailers.add_lower(std::move(name), std::move(value));
            handle->set_trailers(std::move(trailers));
        }
        st.trailer_tmp.clear();
        if (fin)
            st.remote_end = true;
        h3::wake(st.in_space);
    }

    void on_data(std::int64_t stream_id, std::span<const std::uint8_t> data) noexcept override {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end())
            return;
        Stream &st = *it->second;
        st.in_bytes += data.size();
        st.in_queue.emplace_back(reinterpret_cast<const char *>(data.data()), data.size());
        h3::wake(st.in_space);
    }

    void on_end_stream(std::int64_t stream_id) noexcept override {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end())
            return;
        it->second->remote_end = true;
        h3::wake(it->second->in_space);
    }

    void on_stream_closed(std::int64_t stream_id, std::uint64_t /*app_error_code*/) noexcept override {
        // nghttp3-level close: the QUIC stream is gone in both directions. Any
        // decoded body already in the queue is still the application's; keep the
        // entry and let read() drain it (it erases when it reports the end).
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end())
            return;
        it->second->remote_end = true;
        if (it->second->in_queue.empty() && it->second->outcome->get() == StreamState::Open)
            it->second->outcome->set(StreamState::Eof);
        h3::wake(it->second->in_space);
    }

    void on_acked(std::int64_t stream_id, std::uint64_t datalen) noexcept override {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end())
            return;
        Stream &st = *it->second;
        st.out_ack += datalen;
        // Bytes the peer acknowledged are the only ones that may be released:
        // QUIC retransmits from this memory.
        while (!st.out_q.empty() && st.out_base + st.out_q.front().size() <= st.out_ack) {
            st.out_base += st.out_q.front().size();
            st.out_bytes -= st.out_q.front().size();
            st.out_q.pop_front();
        }
        h3::wake(st.out_space);
    }

    void on_stop_sending(std::int64_t stream_id, std::uint64_t app_error_code) noexcept override {
        // nghttp3 asking the application to send STOP_SENDING — i.e. stop
        // reading this stream. (The peer's own STOP_SENDING arrives at
        // Protocol::on_stream_stop_sending and shuts our write half.)
        m_conn->shutdown_stream_read(stream_id, app_error_code);
    }

    void on_reset_stream(std::int64_t stream_id, std::uint64_t app_error_code) noexcept override {
        m_conn->shutdown_stream_write(stream_id, app_error_code);
    }

    void on_deferred_consume(std::int64_t stream_id, std::size_t nconsumed) noexcept override {
        m_conn->extend_stream_offset(stream_id, nconsumed);
        m_conn->extend_connection_offset(nconsumed);
    }

    void on_peer_settings(const nghttp3_proto_settings & /*settings*/) noexcept override {}

    void on_shutdown(std::int64_t /*push_id*/) noexcept override { m_goaway_received = true; }

    nghttp3_ssize read_response_data(std::int64_t stream_id, nghttp3_vec *vec, std::size_t veccnt,
                                     std::uint32_t *flags) noexcept override {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end()) {
            *flags |= NGHTTP3_DATA_FLAG_EOF;
            return 0;
        }
        Stream &st = *it->second;

        std::size_t filled = 0;
        std::size_t total = 0;
        std::uint64_t offset = st.out_base;
        for (const std::string &chunk : st.out_q) {
            if (filled >= veccnt || total >= h3::kMaxBodyPerRead)
                break;
            const std::uint64_t chunk_end = offset + chunk.size();
            if (chunk_end <= st.out_write) {
                offset = chunk_end;
                continue; // wholly handed over already
            }
            const std::size_t skip = st.out_write > offset ? static_cast<std::size_t>(st.out_write - offset) : 0;
            vec[filled].base = reinterpret_cast<std::uint8_t *>(const_cast<char *>(chunk.data())) + skip;
            vec[filled].len = chunk.size() - skip;
            total += vec[filled].len;
            ++filled;
            offset = chunk_end;
        }
        if (filled == 0) {
            if (st.out_eof) {
                *flags |= NGHTTP3_DATA_FLAG_EOF;
                return 0;
            }
            st.data_blocked = true;
            return NGHTTP3_ERR_WOULDBLOCK;
        }
        if (st.out_eof && st.out_write + total >= st.out_base + st.out_bytes)
            *flags |= NGHTTP3_DATA_FLAG_EOF;
        st.out_write += total;
        return static_cast<nghttp3_ssize>(filled);
    }

  private:
    struct Stream {
        std::weak_ptr<Http3ClientStream<Executor>> handle;
        std::shared_ptr<StreamOutcome> outcome;

        // response
        ResponseHead head;
        std::optional<ResponseHead> interim;
        bool head_seen{false};
        bool is_head{false};
        bool remote_end{false};
        bool reset{false};
        bool failed{false};
        bool write_shut{false};
        std::string status_text;
        std::vector<std::pair<std::string, std::string>> header_tmp;
        std::vector<std::pair<std::string, std::string>> trailer_tmp;
        std::deque<std::string> in_queue;
        std::size_t in_bytes{0};

        // request body
        std::deque<std::string> out_q;
        std::uint64_t out_base{0};
        std::uint64_t out_bytes{0};
        std::uint64_t out_write{0};
        std::uint64_t out_ack{0};
        bool out_eof{false};
        bool data_blocked{false};

        // Capacity-1 coalescing wake-ups, created with the session's executor
        // when the stream is opened. `wake()` posts, so they are safe to nudge
        // from inside an nghttp3/ngtcp2 callback.
        Signal in_space;
        Signal out_space;
    };

    asio::awaitable<void> hop() const { co_await asio::dispatch(asio::bind_executor(m_executor, asio::use_awaitable)); }

    // Start the connection and wait for the 1-RTT keys (when the HTTP/3 control
    // streams can be opened).
    asio::awaitable<error_code> start(std::chrono::milliseconds connect_timeout) {
        m_ready = std::make_shared<Channel>(m_executor, 1);
        asio::co_spawn(
            m_executor, [self = this->shared_from_this()]() -> asio::awaitable<void> { co_await self->run(); },
            asio::detached);

        auto wait_ready = [this]() -> asio::awaitable<bool> {
            co_await m_ready->async_receive(asio::as_tuple(asio::use_awaitable));
            co_return m_alive;
        };
        if (connect_timeout.count() <= 0) {
            const bool ok = co_await wait_ready();
            co_return ok ? error_code{} : m_failure;
        }
        using namespace asio::experimental::awaitable_operators;
        auto timer_op = [connect_timeout]() -> asio::awaitable<void> {
            asio::steady_timer timer{co_await asio::this_coro::executor};
            timer.expires_after(connect_timeout);
            co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        };
        auto outcome = co_await (wait_ready() || timer_op());
        if (auto *ok = std::get_if<bool>(&outcome))
            co_return *ok ? error_code{} : m_failure;
        shutdown_now();
        co_return make_error_code(client_errc::connect_timeout);
    }

    asio::awaitable<void> run() {
        m_conn->set_protocol(this->shared_from_this());
        asio::co_spawn(
            m_executor,
            [self = this->shared_from_this()]() -> asio::awaitable<void> {
                co_await self->m_udp->receive_loop(self->m_conn);
            },
            asio::detached);
        co_await m_conn->run();
        m_udp->close();
        m_alive = false;
        m_conn_closed = true;
        // finish_close() normally already ran on_connection_closed; keep the
        // streams' waiters guaranteed to wake even if it did not.
        for (auto &[id, stream] : m_streams) {
            if (stream->outcome->get() == StreamState::Open)
                stream->outcome->set(StreamState::Disconnect);
            h3::wake(stream->in_space);
            h3::wake(stream->out_space);
        }
        if (m_ready)
            (void)m_ready->try_send(error_code{});
    }

    void shutdown_now() {
        m_alive = false;
        for (auto &[id, stream] : m_streams) {
            if (stream->outcome->get() == StreamState::Open)
                stream->outcome->set(StreamState::Disconnect);
            h3::wake(stream->in_space);
            h3::wake(stream->out_space);
        }
        if (m_conn && !m_conn_closed)
            m_conn->close(NGHTTP3_H3_NO_ERROR, "client closed");
    }

    void fail_ready(error_code ec) noexcept {
        m_failure = ec;
        m_failure_set = true;
        m_alive = false;
        if (m_conn)
            m_conn->close(NGHTTP3_H3_INTERNAL_ERROR, "http3 client init");
        if (m_ready)
            (void)m_ready->try_send(error_code{});
    }

    // The nghttp3 connection, or null once it reported a connection-level error.
    // After a negative return nghttp3 may only be handed to `nghttp3_conn_del`.
    [[nodiscard]] nghttp3_conn *h3() const noexcept { return m_h3_dead ? nullptr : m_h3; }

    static nghttp3_ssize read_request_data_trampoline(nghttp3_conn *, std::int64_t stream_id, nghttp3_vec *vec,
                                                      std::size_t veccnt, std::uint32_t *flags, void *conn_user_data,
                                                      void *) {
        return static_cast<h3::H3CallbackSink *>(conn_user_data)->read_response_data(stream_id, vec, veccnt, flags);
    }

    static int parse_status(std::string_view text) noexcept {
        if (text.empty())
            return 0;
        int value = 0;
        for (char c : text) {
            if (c < '0' || c > '9')
                return 0;
            value = value * 10 + (c - '0');
            if (value > 999)
                return 0;
        }
        return value;
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

    // `owned` keeps the strings alive until nghttp3_conn_submit_request has run.
    error_code build_request_headers(const Request &request, std::vector<nghttp3_nv> &nva,
                                     std::deque<std::string> &owned) {
        std::string_view method = request.method_token();
        if (method.empty())
            method = to_string(request.method());
        if (method.empty() || contains_ctl(method))
            return make_error_code(client_errc::protocol_error);

        std::string_view target = request.target().empty() ? std::string_view{"/"} : std::string_view{request.target()};
        if (target.front() != '/' || contains_ctl(target))
            return make_error_code(client_errc::protocol_error);

        owned.emplace_back(method);
        nva.push_back(make_nv(":method", owned.back()));
        owned.emplace_back("https");
        nva.push_back(make_nv(":scheme", owned.back()));
        owned.emplace_back(request.header("host").value_or(m_authority));
        nva.push_back(make_nv(":authority", owned.back()));
        owned.emplace_back(target);
        nva.push_back(make_nv(":path", owned.back()));

        bool saw_agent = false;
        for (const auto &[name, value] : request.headers()) {
            if (name.empty() || name.front() == ':' || is_connection_specific_field(name) || name == "host")
                continue;
            if (contains_ctl(name) || contains_ctl(value)) {
                SIMPLE_HTTP_ERROR_LOG("h3 client: dropping header '{}' with CR/LF/NUL", name);
                continue;
            }
            if (name == "user-agent")
                saw_agent = true;
            owned.emplace_back(name);
            owned.emplace_back(value);
            nva.push_back(make_nv(owned[owned.size() - 2], owned.back()));
        }
        if (!saw_agent) {
            owned.emplace_back(client_product);
            nva.push_back(make_nv("user-agent", owned.back()));
        }
        if (!request.stream_body() && request.content_length() >= 0 && request.content_length() > 0) {
            owned.emplace_back(std::to_string(request.content_length()));
            nva.push_back(make_nv("content-length", owned.back()));
        }
        return {};
    }

    void resume_stream(std::int64_t id) {
        auto it = m_streams.find(id);
        if (it == m_streams.end() || h3() == nullptr)
            return;
        it->second->data_blocked = false;
        (void)nghttp3_conn_resume_stream(m_h3, id);
    }

    asio::awaitable<error_code> await_out_space(std::int64_t id) {
        for (;;) {
            auto it = m_streams.find(id);
            if (it == m_streams.end())
                co_return make_error_code(client_errc::session_closed);
            Stream &st = *it->second;
            if (st.write_shut)
                co_return make_error_code(client_errc::stream_reset);
            if (st.out_write - st.out_ack <= h3::kOutLowWatermark)
                co_return error_code{};
            auto space = st.out_space;
            auto [ec] = co_await space->async_receive(asio::as_tuple(asio::use_awaitable));
            if (ec)
                co_return ec;
        }
    }

    void it_finish(std::unordered_map<std::int64_t, std::shared_ptr<Stream>>::iterator it, StreamState state,
                   error_code ec) {
        if (it->second->outcome->get() == StreamState::Open)
            it->second->outcome->set(state, std::move(ec));
        h3::wake(it->second->in_space);
        h3::wake(it->second->out_space);
    }

    void erase_stream(std::unordered_map<std::int64_t, std::shared_ptr<Stream>>::iterator it) {
        h3::wake(it->second->in_space);
        h3::wake(it->second->out_space);
        m_streams.erase(it);
        m_active_streams.fetch_sub(1, std::memory_order_release);
    }

    Executor m_executor;
    std::shared_ptr<Connection> m_conn;
    std::shared_ptr<Udp> m_udp;
    std::string m_authority;
    EngineLimits m_limits;
    nghttp3_conn *m_h3{nullptr};
    bool m_h3_dead{false};
    bool m_alive{false};
    bool m_conn_closed{false};
    bool m_goaway_received{false};
    bool m_close_requested{false};
    error_code m_failure{make_error_code(client_errc::version_not_negotiated)};
    bool m_failure_set{false};
    Signal m_ready;
    std::unordered_map<std::int64_t, std::shared_ptr<Stream>> m_streams;
    std::atomic<std::size_t> m_active_streams{0};
    std::array<quic::ByteVec, 8> m_vec{};
    std::size_t m_vec_count{0};
};

} // namespace simple_http::detail
