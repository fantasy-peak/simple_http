#pragma once

// A QUIC connection: packets in, packets out, streams and flow control in
// between (RFC 9000).
//
// Shape, and why:
//
//   * **Everything runs on one executor.** A connection is pinned to the
//     io_context that owns its UDP socket, so there are no locks and no
//     atomics. Datagrams arrive as direct calls rather than through a channel:
//     handling one is entirely synchronous (the handshake is driven by
//     `SSL_do_handshake`, which never blocks), so a queue would only add a copy
//     and a hop.
//   * **Two coroutines, raced.** `write_loop` builds datagrams and parks until
//     something wants to send; `timer_loop` drives loss detection, PTO and the
//     idle timeout. Racing them is the whole shutdown mechanism — the same
//     idiom as the h2 engine's `serve_loops`.
//   * **The connection knows nothing about HTTP.** It produces streams; what
//     the bytes on them mean is the engine's business. `accept_stream` and
//     `open_uni_stream` are the whole interface upward.
//
// Three things here are easy to get wrong and are called out where they happen:
// the amplification limit (§8.1) applies until the peer's address is validated;
// header protection forces a two-pass parse (§17.2); and a coalesced datagram
// has to be split before either pass, because header protection addresses "the
// first octet of the packet" and only the first packet in a datagram has that
// octet at offset zero.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>

#include "../core/logging.h"
#include "../core/types.h"
#include "ack.h"
#include "congestion.h"
#include "crypto.h"
#include "frame.h"
#include "packet.h"
#include "quic_stream_transport.h"
#include "recovery.h"
#include "stream_state.h"
#include "tls.h"
#include "transport_params.h"
#include "wire.h"

namespace simple_http::quic {

namespace asio = boost::asio;

// The largest offset a stream can reach (RFC 9000 §4.5): 2^62 - 1.
inline constexpr std::uint64_t kMaxStreamOffset = (1ULL << 62) - 1;

// How many closed peer streams are accumulated before MAX_STREAMS advertises
// them, unless the peer is (or is about to be) out of credit — see
// QuicConnection::credit_stream_slot.
inline constexpr std::uint64_t kStreamCreditBatch = 8;

// The levels in the order packets are coalesced into a datagram.
inline constexpr std::array<EncryptionLevel, 4> kAllLevels{EncryptionLevel::Initial, EncryptionLevel::ZeroRtt,
                                                           EncryptionLevel::Handshake, EncryptionLevel::OneRtt};

// Tunables for a connection, all advertised to the peer in transport
// parameters except the purely local ones.
struct QuicConnectionConfig {
    std::uint64_t max_idle_timeout_ms{30000};
    std::uint64_t max_udp_payload_size{1472};
    std::uint64_t initial_max_data{1u << 20};
    std::uint64_t initial_max_stream_data_bidi_local{256u << 10};
    std::uint64_t initial_max_stream_data_bidi_remote{256u << 10};
    std::uint64_t initial_max_stream_data_uni{256u << 10};
    std::uint64_t initial_max_streams_bidi{100};
    std::uint64_t initial_max_streams_uni{100};
    std::uint64_t active_connection_id_limit{4};
    std::uint64_t ack_delay_exponent{3};
    std::uint64_t max_ack_delay_ms{25};

    std::size_t connection_id_length{8};
    // Bound on handshake data held in flight, both directions
    // (CRYPTO_BUFFER_EXCEEDED).
    std::size_t max_crypto_buffer{64 * 1024};
    // A backstop behind MAX_STREAMS.
    std::size_t max_streams{512};
    bool disable_active_migration{false};
    bool enable_0rtt{false};
};

// Where a connection's datagrams go: a lambda the endpoint supplies, writing to
// the UDP socket the connection arrived on.
using QuicDatagramSink = std::function<void(Bytes, const asio::ip::udp::endpoint&)>;

template <typename Executor>
class QuicConnection : public std::enable_shared_from_this<QuicConnection<Executor>> {
  public:
    using executor_type = Executor;
    using StreamTransport = QuicStreamTransport<Executor>;
    using Clock = std::chrono::steady_clock;

    QuicConnection(Executor exec, SSL_CTX* ssl_ctx, QuicConnectionConfig config, std::string original_dcid,
                   std::string peer_scid, QuicDatagramSink sink, asio::ip::udp::endpoint peer,
                   std::function<std::string()> cid_factory, bool address_validated)
        : m_executor(std::move(exec)), m_config(config), m_original_dcid(std::move(original_dcid)),
          m_peer_scid(std::move(peer_scid)), m_sink(std::move(sink)), m_peer(std::move(peer)),
          m_cid_factory(std::move(cid_factory)), m_recovery(kMaxDatagramSize) {
        m_ssl_ctx = ssl_ctx;
        m_local_scid = m_cid_factory ? m_cid_factory() : std::string(m_config.connection_id_length, '\0');
        m_stream_notify =
            std::make_shared<asio::experimental::concurrent_channel<void(error_code)>>(m_executor, 1);
        m_send_notify =
            std::make_shared<asio::experimental::concurrent_channel<void(error_code)>>(m_executor, 1);

        // The Initial keys come from the *client's* destination connection ID
        // with a fixed salt, not from TLS (RFC 9001 §5.2) — which is what makes
        // the first flight readable before any secret exists.
        auto [client_secret, server_secret] = initial_secrets(m_original_dcid);
        m_initial_tx = derive_initial_keys(server_secret);
        m_initial_rx = derive_initial_keys(client_secret);

        m_recv_max_data = m_config.initial_max_data;
        m_recv_data_start = m_config.initial_max_data;
        m_local_max_streams_bidi = m_config.initial_max_streams_bidi;
        m_local_max_streams_uni = m_config.initial_max_streams_uni;

        m_recovery.set_max_ack_delay(std::chrono::milliseconds(m_config.max_ack_delay_ms));
        m_recovery.set_address_validated(address_validated);
    }

    ~QuicConnection() = default;

    // Set up the TLS handshake. Must happen before the first datagram is
    // handed in: the client's Initial carries the ClientHello, and there is
    // nothing to feed it into until the SSL object exists. Returns false if the
    // context is unusable, in which case the connection must be dropped.
    bool init() { return init_tls(); }

    [[nodiscard]] Executor get_executor() const { return m_executor; }
    // `TransportLike` (and Request, which carries a peer address) speaks in TCP
    // endpoints. A QUIC peer is a UDP address; the port and address are the same
    // numbers, so the conversion is a restatement rather than a translation —
    // which is why it is done here and not left to every caller.
    [[nodiscard]] asio::ip::tcp::endpoint peer() const {
        return asio::ip::tcp::endpoint{m_peer.address(), m_peer.port()};
    }
    [[nodiscard]] const asio::ip::udp::endpoint& udp_peer() const { return m_peer; }
    [[nodiscard]] SslHandle tls_handle() const { return m_tls.ssl(); }
    [[nodiscard]] const std::string& local_scid() const noexcept { return m_local_scid; }
    [[nodiscard]] bool closed() const noexcept { return !m_alive; }

    // --- the engine's view -------------------------------------------------

    // The next peer-initiated stream, in arrival order. Resolves to nullptr once
    // the connection is closed, which is how an engine's accept loop ends.
    asio::awaitable<std::shared_ptr<StreamTransport>> accept_stream() {
        for (;;) {
            if (!m_incoming.empty()) {
                const std::uint64_t id = m_incoming.front();
                m_incoming.pop_front();
                auto it = m_transports.find(id);
                if (it != m_transports.end()) {
                    co_return it->second;
                }
                continue;
            }
            if (!m_alive) co_return nullptr;
            auto [ec] = co_await m_stream_notify->async_receive(asio::as_tuple(asio::use_awaitable));
            if (ec) co_return nullptr;
        }
    }

    // A new locally-initiated unidirectional stream: the HTTP/3 control stream
    // and its two QPACK streams. Nothing goes on it until the caller writes.
    std::shared_ptr<StreamTransport> open_uni_stream() {
        if (!m_alive) return nullptr;
        if (m_next_local_uni / 4 >= m_peer_max_streams_uni) {
            SIMPLE_HTTP_ERROR_LOG("QUIC: peer's unidirectional stream limit reached");
            return nullptr;
        }
        const std::uint64_t id = m_next_local_uni;
        m_next_local_uni += 4;
        return make_stream(id);
    }

    std::shared_ptr<StreamTransport> open_bidi_stream() {
        if (!m_alive) return nullptr;
        if (m_next_local_bidi / 4 >= m_peer_max_streams_bidi) return nullptr;
        const std::uint64_t id = m_next_local_bidi;
        m_next_local_bidi += 4;
        return make_stream(id);
    }

    // Abort one stream without disturbing the others. This is what a cancelled
    // HTTP/3 request uses, and the reason HTTP/3 runs over QUIC at all.
    void reset_stream(std::uint64_t stream_id, std::uint64_t error_code) {
        auto it = m_streams.find(stream_id);
        if (it != m_streams.end()) reset_stream_impl(it->second, error_code);
    }

    // Close the whole connection with an application error (CONNECTION_CLOSE
    // type 0x1d), which is what an HTTP/3 engine does on a protocol error.
    void close(std::uint64_t error_code, std::string_view reason) {
        if (!m_alive || m_closing) return;
        SIMPLE_HTTP_WARN_LOG("QUIC: application close, code=0x{:x} reason={}", error_code, reason);
        m_close_error = error_code;
        m_close_reason.assign(reason);
        m_close_application = true;
        m_closing = true;
        flush();
    }

    void shutdown(std::uint64_t error_code) { close(error_code, {}); }

    [[nodiscard]] std::uint64_t recv_consumed() const noexcept { return m_recv_consumed; }

    // --- the endpoint's view ----------------------------------------------

    // A datagram for this connection. Called on the connection's executor.
    void on_datagram(std::span<const std::uint8_t> data, const asio::ip::udp::endpoint& from) {
        if (!m_alive) return;
        m_bytes_received += data.size();
        m_last_activity = Clock::now();

        if (from != m_peer) {
            if (!on_migrating_datagram(data, from)) return;
        }
        process_datagram(data);
        // Whatever arrived may have freed flow-control credit or produced
        // frames to acknowledge.
        flush();
    }

    asio::awaitable<void> run() {
        using namespace asio::experimental::awaitable_operators;
        m_deadline = Clock::now() + std::chrono::milliseconds(m_config.max_idle_timeout_ms);

        // TLS was set up before the first datagram was fed in, and the
        // ClientHello may already have advanced the handshake past its first
        // flight. Re-driving it here is what turns "received bytes" into
        // "produced bytes" for anything still queued.
        //
        // A *failed* handshake is not a reason to return: the failure queued a
        // CONNECTION_CLOSE carrying the TLS alert that explains it, and bailing
        // out here would drop it, leaving the peer with a connection that simply
        // stops. The loops below send it and then end on their own — closing is
        // what makes them return.
        (void)drive_handshake();

        co_await (write_loop() || timer_loop());

        m_alive = false;
        // Unpark everything that could be waiting on this connection. A reader
        // parked on a channel nobody will ever write to is a leak, not a stall.
        for (auto& [id, stream] : m_streams) {
            (void)id;
            if (stream->notify) stream->notify->close();
            if (stream->write_space) stream->write_space->close();
        }
        m_incoming.clear();
        if (m_stream_notify) m_stream_notify->close();
        if (m_send_notify) m_send_notify->close();
    }

  private:
    // --- set-up -----------------------------------------------------------

    bool init_tls() {
        // The destination connection ID the client used is the one the *peer*
        // must confirm it saw, so it is what goes in
        // original_destination_connection_id (RFC 9000 §7.3).
        TransportParams ours;
        ours.has_original_dcid = true;
        ours.original_dcid = m_original_dcid;
        ours.max_idle_timeout = m_config.max_idle_timeout_ms;
        ours.max_udp_payload_size = m_config.max_udp_payload_size;
        ours.initial_max_data = m_config.initial_max_data;
        ours.initial_max_stream_data_bidi_local = m_config.initial_max_stream_data_bidi_local;
        ours.initial_max_stream_data_bidi_remote = m_config.initial_max_stream_data_bidi_remote;
        ours.initial_max_stream_data_uni = m_config.initial_max_stream_data_uni;
        ours.initial_max_streams_bidi = m_config.initial_max_streams_bidi;
        ours.initial_max_streams_uni = m_config.initial_max_streams_uni;
        ours.ack_delay_exponent = m_config.ack_delay_exponent;
        ours.max_ack_delay = m_config.max_ack_delay_ms;
        ours.active_connection_id_limit = m_config.active_connection_id_limit;
        ours.initial_source_connection_id = m_local_scid;
        ours.disable_active_migration = m_config.disable_active_migration;
        ours.has_stateless_reset_token = true;
        for (auto& byte : ours.stateless_reset_token) byte = static_cast<std::uint8_t>(m_random());

        QuicTlsHooks hooks;
        hooks.on_crypto_ready = [this](EncryptionLevel) { flush(); };
        hooks.on_keys = [this](EncryptionLevel level, bool read) {
            // 1-RTT keys are not handed to the TLS object's array — they go into
            // dedicated members because they are the ones key update rewrites.
            const PacketKeys& keys = m_tls.keys(level, read);
            if (level == EncryptionLevel::OneRtt) {
                if (read) {
                    m_one_rtt_rx = keys;
                } else {
                    m_one_rtt_tx = keys;
                }
            } else if (level == EncryptionLevel::ZeroRtt && read) {
                m_zero_rtt_rx = keys;
                if (m_config.enable_0rtt) replay_undecryptable();
            }
        };
        hooks.on_peer_transport_params = [](std::string_view) {};
        hooks.on_alert = [this](std::uint8_t alert) {
            // RFC 9001 §4.8: a TLS alert becomes a QUIC CRYPTO_ERROR, whose code
            // is 0x100 plus the alert. Reporting it as an internal error throws
            // away the only thing the peer needs to tell a protocol mismatch from
            // a rejected certificate — and it is what the TLS alert is *for*.
            transport_close(static_cast<TransportError>(0x100 + alert), kNoFrame, "TLS alert");
        };

        if (m_ssl_ctx == nullptr) return false;
        return m_tls.init(m_ssl_ctx, ours, std::move(hooks), m_config.enable_0rtt);
    }

    // --- stream plumbing --------------------------------------------------

    std::shared_ptr<StreamTransport> make_stream(std::uint64_t id) {
        auto state = std::make_shared<QuicStreamState>();
        state->id = id;
        state->notify =
            std::make_shared<asio::experimental::concurrent_channel<void(error_code)>>(m_executor, 1);
        // The receive window this stream starts with depends on who opened it
        // and how: the peer's transport parameters name three separate limits,
        // and using the wrong one is a flow-control error the peer closes on.
        const bool local = stream_is_local(id, /*server=*/true);
        const bool bidi = stream_is_bidi(id);
        // The receive window is *ours*: it is what we advertised in our own
        // transport parameters, so it is known from the first moment and must be
        // set here rather than left for the handshake to fill in. Leaving it at
        // zero makes every byte the peer sends a flow-control violation — and
        // only for streams created after the handshake, because the ones that
        // existed at the time get fixed up by on_handshake_complete. A real
        // client opens its request stream after the handshake, which is why the
        // hand-written one never noticed.
        const std::uint64_t recv_limit = bidi ? (local ? m_config.initial_max_stream_data_bidi_local
                                                       : m_config.initial_max_stream_data_bidi_remote)
                                              : m_config.initial_max_stream_data_uni;
        state->recv_start = recv_limit;
        state->recv.set_max_data(recv_limit);
        // The *send* window is the peer's, so it really is unknown until the
        // peer's transport parameters arrive — which for a stream created later
        // they already have.
        state->send_max_data = m_handshake_complete
                                   ? (bidi ? (local ? m_peer_initial_max_stream_data_bidi_remote
                                                    : m_peer_initial_max_stream_data_bidi_local)
                                           : m_peer_initial_max_stream_data_uni)
                                   : 0;
        state->wake_send = [this] { flush(); };

        typename StreamTransport::Hooks hooks;
        hooks.wake_send = [this] { flush(); };
        std::weak_ptr<QuicStreamState> weak = state;
        hooks.on_consumed = [this, weak](std::uint64_t n) {
            auto s = weak.lock();
            if (!s) return;
            s->consumed += n;
            m_recv_consumed += n;
            maybe_extend_stream_window(*s);
            maybe_extend_connection_window();
        };
        hooks.on_reset = [this, id](std::uint64_t code) {
            auto it = m_streams.find(id);
            if (it != m_streams.end()) reset_stream_impl(it->second, code);
        };

        auto transport = std::make_shared<StreamTransport>(state, m_executor, peer(), SslHandle{m_tls.ssl()},
                                                           std::move(hooks));
        m_streams.emplace(id, state);
        m_transports.emplace(id, transport);
        return transport;
    }

    std::shared_ptr<QuicStreamState> find_stream(std::uint64_t id) {
        auto it = m_streams.find(id);
        return it == m_streams.end() ? nullptr : it->second;
    }

    void maybe_extend_stream_window(QuicStreamState& state) {
        // Never credit a stream the peer has finished. It will not send again on
        // it, so the frame is pointless — and worse than pointless: the peer is
        // entitled to have retired the stream the moment it sent its FIN, and a
        // MAX_STREAM_DATA naming a stream it no longer has is a stream state
        // error *at its end*. The connection then dies reporting our frame as the
        // cause, several layers away from here.
        if (state.remote_closed) return;
        // The advertised limit slides with the reader: it is always "everything
        // consumed, plus one full window". Crediting only what was consumed — so
        // that the peer's limit equals what it has already sent — leaves it with
        // no headroom the moment the reader catches up, and a peer sitting exactly
        // at its limit while the reader waits for more is a deadlock rather than
        // backpressure. A body of a few windows is exactly where it shows up.
        //
        // Credited in half-window steps, the same batching the h2 engine uses:
        // acknowledging every read would put a MAX_STREAM_DATA frame in nearly
        // every packet a streaming reader receives.
        const std::uint64_t window = state.recv_start;
        const std::uint64_t target = state.consumed + window;
        if (target - state.recv.max_data() < window / 2) return;
        state.consumed_credited = state.consumed;
        state.recv.set_max_data(target);
        Bytes frame;
        append_max_stream_data(frame, state.id, target);
        queue_control(std::move(frame), PacketNumberSpace::Application, state.id);
    }

    void maybe_extend_connection_window() {
        // The same sliding window as a single stream's, for the same reason: the
        // peer is allowed one full window of unread data, not exactly what has
        // been read.
        const std::uint64_t window = m_recv_data_start;
        const std::uint64_t target = m_recv_consumed + window;
        if (target - m_recv_max_data < window / 2) return;
        m_recv_credited = m_recv_consumed;
        m_recv_max_data = target;
        Bytes frame;
        append_max_data(frame, target);
        queue_control(std::move(frame));
    }

    // A control frame that has to survive loss. The bytes are kept until the
    // packet carrying them is acknowledged, and re-queued if it is not — which
    // is what makes RESET_STREAM, whose retransmission is mandatory, need no
    // special case.
    // `space` is not decoration. RFC 9000 §12.5 allows only PADDING, PING, ACK,
    // CRYPTO and CONNECTION_CLOSE outside the application space, so a control
    // frame written into whichever packet happened to be under construction is a
    // PROTOCOL_VIOLATION — and the peer reports it as a bare violation with no
    // frame type, pointing nowhere near the cause. Every control frame this
    // connection queues is an application-space one, which is why the default is
    // the application space rather than the packet's.
    // No stream is addressed by default; only the flow-control frames name one,
    // and they are the ones whose usefulness can expire before they are sent.
    static constexpr std::uint64_t kNoSubjectStream = static_cast<std::uint64_t>(-1);

    void queue_control(Bytes frame, PacketNumberSpace space = PacketNumberSpace::Application,
                       std::uint64_t subject = kNoSubjectStream) {
        m_pending_control.push_back(PendingControl{std::move(frame), space, m_next_control_id++, false, subject});
    }

    // --- sending ----------------------------------------------------------

    void flush() {
        if (m_alive && m_send_notify) (void)m_send_notify->try_send(error_code{});
    }

    void transport_close(TransportError error, std::uint64_t frame_type, std::string_view reason) {
        if (m_closing) return;
        SIMPLE_HTTP_WARN_LOG("QUIC: closing connection, error=0x{:x} frame=0x{:x} reason={}",
                             static_cast<std::uint64_t>(error), frame_type, reason);
        m_close_error = static_cast<std::uint64_t>(error);
        m_close_reason.assign(reason);
        m_close_frame_type = frame_type;
        m_close_application = false;
        m_closing = true;
        flush();
    }

    [[nodiscard]] const PacketKeys* send_keys(EncryptionLevel level) const {
        switch (level) {
            case EncryptionLevel::Initial:
                return m_initial_tx.valid ? &m_initial_tx : nullptr;
            case EncryptionLevel::Handshake:
                return m_tls.keys(level, false).valid ? &m_tls.keys(level, false) : nullptr;
            case EncryptionLevel::OneRtt:
                return m_one_rtt_tx.valid ? &m_one_rtt_tx : nullptr;
            case EncryptionLevel::ZeroRtt:
                return nullptr;  // a server never sends 0-RTT
        }
        return nullptr;
    }

    [[nodiscard]] const PacketKeys* receive_keys(EncryptionLevel level) const {
        switch (level) {
            case EncryptionLevel::Initial:
                return m_initial_rx.valid ? &m_initial_rx : nullptr;
            case EncryptionLevel::Handshake:
                return m_tls.keys(level, true).valid ? &m_tls.keys(level, true) : nullptr;
            case EncryptionLevel::ZeroRtt:
                return m_zero_rtt_rx.valid ? &m_zero_rtt_rx : nullptr;
            case EncryptionLevel::OneRtt:
                return m_one_rtt_rx.valid ? &m_one_rtt_rx : nullptr;
        }
        return nullptr;
    }

    [[nodiscard]] bool has_sendable() const {
        if (m_closing) return !m_close_sent;
        for (const bool owed : m_ack_owed) {
            if (owed) return true;
        }
        if (!m_pending_control.empty()) return true;
        for (const auto& [id, stream] : m_streams) {
            (void)id;
            if (stream->send.has_pending()) return true;
        }
        for (const EncryptionLevel level : kAllLevels) {
            if (m_tls.has_crypto_to_send(level)) return true;
        }
        return false;
    }

    // The anti-amplification limit: until the peer's address is validated, a
    // server may send at most three times what it has received (RFC 9000 §8.1).
    // Without it a spoofed source address turns a QUIC server into a reflector.
    [[nodiscard]] bool amplification_allows(std::size_t bytes) const {
        if (m_recovery.address_validated()) return true;
        return m_bytes_sent + bytes <= 3 * m_bytes_received;
    }

    asio::awaitable<void> write_loop() {
        for (;;) {
            while (m_alive && has_sendable()) {
                if (!send_one_datagram()) break;
            }
            // A closing connection has said everything it is going to say. Parking
            // here would keep it — and every coroutine frame it owns, including
            // the engine's — alive until the idle timeout, so "closing" would mean
            // "idle" and a peer that closed cleanly would still cost a connection
            // table entry and a set of buffers for the timeout's duration.
            if (m_closing && m_close_sent) co_return;
            if (!m_alive) co_return;
            auto [ec] = co_await m_send_notify->async_receive(asio::as_tuple(asio::use_awaitable));
            if (ec) co_return;
        }
    }

    // Build and send one datagram. False means nothing more can go out right
    // now — congestion-limited, amplification-limited, or nothing to say.
    bool send_one_datagram() {
        if (m_closing) {
            if (m_close_sent) return false;
            // Drain before saying goodbye. Closing is not a truncation: bytes the
            // application already handed over — an HTTP/3 GOAWAY, most of all —
            // are still sitting in a stream's send buffer, because `async_write`
            // only *queues* them and wakes this loop. Sending CONNECTION_CLOSE
            // first would drop them, and the peer would see a bare transport
            // close instead of the frame that explained it: a different answer,
            // not a later one. The budget bounds the delay so a peer that has
            // stopped reading cannot hold the close off.
            Bytes datagram;
            if (m_close_drain > 0 && build_data_datagram(datagram)) {
                --m_close_drain;
                return transmit(std::move(datagram));
            }
            datagram.clear();
            build_close_packet(datagram);
            m_close_sent = true;
            if (datagram.empty()) return false;
            return transmit(std::move(datagram));
        }

        Bytes datagram;
        if (!build_data_datagram(datagram)) return false;
        return transmit(std::move(datagram));
    }

    // Build one datagram of ordinary frames. False when there is nothing to put
    // in one — which, in the closing state, is the signal to send the close.
    bool build_data_datagram(Bytes& datagram) {
        const std::size_t budget = m_max_datagram_size;
        if (!amplification_allows(budget)) return false;

        datagram.clear();
        bool ack_eliciting_initial = false;
        std::vector<std::uint64_t> used_control;
        std::vector<SentPacket> emitted;

        // Coalescing order matters: Initial, then Handshake, then 1-RTT
        // (RFC 9000 §12.2). A receiver parses them in that order, and the short
        // header packet has to be last because it has no length field.
        for (const EncryptionLevel level : {EncryptionLevel::Initial, EncryptionLevel::Handshake,
                                            EncryptionLevel::OneRtt}) {
            if (!send_keys(level) || !send_keys(level)->valid) continue;
            const std::size_t remaining = budget > datagram.size() ? budget - datagram.size() : 0;
            if (remaining < 64) continue;

            Bytes packet;
            SentPacket sent;
            const bool ack_eliciting = build_packet(level, packet, remaining, sent, used_control);
            if (packet.empty()) continue;
            if (level == EncryptionLevel::Initial && ack_eliciting) ack_eliciting_initial = true;
            datagram += packet;
            // Every packet is recorded, not only the ack-eliciting ones. An
            // ACK-only packet is not tracked for loss — it has nothing to
            // retransmit — but it *was* sent, so a peer that acknowledges it must
            // not be told it acknowledged something that never existed. Recording
            // only the ack-eliciting ones leaves the largest-sent number stale,
            // and the next acknowledgement of an ACK-only packet becomes a
            // spurious PROTOCOL_VIOLATION.
            emitted.push_back(std::move(sent));
        }

        if (datagram.empty()) return false;

        // A server must expand every datagram carrying an ack-eliciting Initial
        // packet to 1200 octets (RFC 9000 §14.1): a smaller one is not a valid
        // Initial, and the peer may reject it.
        if (ack_eliciting_initial && datagram.size() < kMinInitialDatagramSize) {
            datagram.resize(kMinInitialDatagramSize, '\0');
        }

        // Commit only now that the bytes are going out. Everything the packet
        // builders reserved — packet numbers, stream offsets, control frames —
        // is only real once the datagram exists.
        for (const std::uint64_t id : used_control) {
            for (auto& control : m_pending_control) {
                if (control.id == id) control.in_flight = true;
            }
        }
        for (SentPacket& sent : emitted) m_recovery.on_packet_sent(sent);
        return true;
    }

    bool transmit(Bytes datagram) {
        m_bytes_sent += datagram.size();
        m_last_activity = Clock::now();
        if (m_sink) m_sink(std::move(datagram), m_peer);
        return true;
    }

    // Fill `packet` with one packet at `level`, and `sent` with what went into
    // it. The packet number is taken from `sent.packet_number`, which the caller
    // must have set.
    bool build_packet(EncryptionLevel level, Bytes& packet, std::size_t budget, SentPacket& sent,
                      std::vector<std::uint64_t>& used_control) {
        const PacketKeys* keys = send_keys(level);
        if (!keys || !keys->valid) return false;

        const PacketNumberSpace space = space_of_level(level);
        const std::size_t index = space_index(space);
        const std::uint64_t pn = m_next_pn[index];
        const std::uint64_t largest_acked = m_recovery.has_largest_acked(space) ? m_recovery.largest_acked(space) : 0;
        const std::size_t pn_len = packet_number_len(pn, largest_acked);

        // The Length field is a varint whose width depends on the payload, so
        // the header cannot be written until the payload's size is known. Build
        // the frames into a scratch buffer against the worst-case header.
        const std::size_t header_max = header_overhead(level, pn_len);
        if (budget <= header_max + kAeadTagLen + 8) return false;
        const std::size_t frames_budget = budget - header_max - kAeadTagLen;

        Bytes frames;
        bool ack_eliciting = false;
        append_frames(level, space, frames_budget, frames, ack_eliciting, sent, used_control);
        if (frames.empty()) return false;

        std::size_t pn_offset = 0;
        if (level == EncryptionLevel::OneRtt) {
            append_short_header(packet, m_peer_scid, m_key_phase, pn_len, pn_offset);
        } else {
            // A server's Initial carries no token: the token field exists for a
            // client to echo back what a Retry gave it.
            const std::uint64_t length = pn_len + frames.size() + kAeadTagLen;
            append_long_header(packet, long_type_of(level), kQuicVersion1, m_peer_scid, m_local_scid, {},
                               length, pn_len, pn_offset);
        }
        append_packet_number(packet, pn, pn_len);
        packet += frames;

        const std::string_view aad{packet.data(), pn_offset + pn_len};
        const std::string_view plaintext{packet.data() + pn_offset + pn_len,
                                         packet.size() - pn_offset - pn_len};
        Bytes ciphertext;
        if (!aead_seal(*keys, pn, aad, plaintext, ciphertext)) {
            // The stream cursors have already advanced, so the data in this
            // packet cannot simply be dropped; the connection is unusable now
            // and has to be closed rather than continued.
            packet.clear();
            transport_close(TransportError::InternalError, kNoFrame, "packet protection failed");
            return false;
        }
        packet.resize(pn_offset + pn_len);
        packet += ciphertext;
        apply_header_protection(*keys, packet, pn_offset, pn_len);

        sent.packet_number = pn;
        sent.space = space;
        sent.time_sent = Clock::now();
        sent.sent_bytes = packet.size();
        sent.ack_eliciting = ack_eliciting;
        // An ACK-only packet stays out of the congestion window: a window that
        // counted acknowledgements could deadlock on its own feedback
        // (RFC 9002 §B.2).
        sent.in_flight = ack_eliciting;
        m_next_pn[index] = pn + 1;
        return ack_eliciting;
    }

    [[nodiscard]] std::size_t header_overhead(EncryptionLevel level, std::size_t pn_len) const {
        if (level == EncryptionLevel::OneRtt) return 1 + m_peer_scid.size() + pn_len;
        // first octet + version + two length-prefixed CIDs + an 8-octet length
        // (the worst case for a varint) + the packet number + the token length
        return 1 + 4 + 1 + m_peer_scid.size() + 1 + m_local_scid.size() + 8 + pn_len + 1;
    }

    static LongHeaderType long_type_of(EncryptionLevel level) {
        switch (level) {
            case EncryptionLevel::Initial:
                return LongHeaderType::Initial;
            case EncryptionLevel::Handshake:
                return LongHeaderType::Handshake;
            case EncryptionLevel::ZeroRtt:
                return LongHeaderType::ZeroRtt;
            case EncryptionLevel::OneRtt:
                break;
        }
        return LongHeaderType::Handshake;
    }

    void append_close_packet(Bytes& datagram, EncryptionLevel level) {
        const PacketKeys* keys = send_keys(level);
        if (!keys || !keys->valid) return;
        const PacketNumberSpace space = space_of_level(level);
        const std::size_t index = space_index(space);
        const std::uint64_t pn = m_next_pn[index];
        const std::size_t pn_len = 1;
        Bytes frames;
        append_connection_close(frames, m_close_error, m_close_reason, m_close_application, m_close_frame_type);
        Bytes packet;
        std::size_t pn_offset = 0;
        if (level == EncryptionLevel::OneRtt) {
            append_short_header(packet, m_peer_scid, m_key_phase, pn_len, pn_offset);
        } else {
            append_long_header(packet, long_type_of(level), kQuicVersion1, m_peer_scid, m_local_scid, {},
                               pn_len + frames.size() + kAeadTagLen, pn_len, pn_offset);
        }
        append_packet_number(packet, pn, pn_len);
        packet += frames;
        const std::string_view aad{packet.data(), pn_offset + pn_len};
        const std::string_view plaintext{packet.data() + pn_offset + pn_len,
                                         packet.size() - pn_offset - pn_len};
        Bytes ciphertext;
        if (!aead_seal(*keys, pn, aad, plaintext, ciphertext)) return;
        packet.resize(pn_offset + pn_len);
        packet += ciphertext;
        apply_header_protection(*keys, packet, pn_offset, pn_len);
        // The close packet is not tracked for loss recovery: there is nothing
        // left to retransmit into, and retrying it would be a loop.
        m_next_pn[index] = pn + 1;
        datagram += packet;
    }

    void build_close_packet(Bytes& datagram) {
        // Prefer the highest level the peer can read, falling back so that a
        // handshake failure is still reportable.
        for (const EncryptionLevel level : {EncryptionLevel::OneRtt, EncryptionLevel::Handshake,
                                            EncryptionLevel::Initial}) {
            if (!send_keys(level) || !send_keys(level)->valid) continue;
            // The close echoes the key phase in use, so a peer updating keys
            // concurrently can still read it.
            append_close_packet(datagram, level);
            return;
        }
    }

    // Append whichever frames this level and space have to offer, up to
    // `budget`, recording what went in so loss recovery can re-send it.
    void append_frames(EncryptionLevel level, PacketNumberSpace space, std::size_t budget, Bytes& frames,
                       bool& ack_eliciting, SentPacket& sent, std::vector<std::uint64_t>& used_control) {
        const std::size_t index = space_index(space);

        // An acknowledgement first: it is what the peer is waiting for, and the
        // cheapest frame there is.
        if (m_ack_owed[index]) {
            // The Largest Acknowledged comes from the ranges themselves, not from
            // a separate counter: §19.3.1 hangs the first range directly below
            // that field, so the two have to agree, and taking both from one
            // place is what makes disagreeing impossible.
            const std::vector<AckRange> ranges = m_acks[index].ranges();
            Bytes ack;
            append_ack(ack, ranges.empty() ? 0 : ranges[0].largest,
                       ack_delay_units(m_acks[index], Clock::now()), ranges);
            if (ack.size() <= budget) {
                frames += ack;
                m_ack_owed[index] = false;
                // What this frame tells the peer, remembered against the packet
                // that carries it: when the peer confirms it has the packet, it
                // has the acknowledgement too, and these ranges can be forgotten.
                // An empty range set still goes out as Largest Acknowledged 0 —
                // legal, and the peer's loss detection is what it is for — so
                // there is nothing to remember in that case.
                if (!ranges.empty()) sent.acked_peer_largest = ranges[0].largest;
            }
        }

        // Handshake bytes at this level.
        while (m_tls.has_crypto_to_send(level)) {
            if (frames.size() + 24 >= budget) break;
            std::uint64_t offset = 0;
            std::string_view data;
            if (!m_tls.next_crypto(level, budget - frames.size() - 24, kMaxStreamOffset, offset, data)) break;
            if (data.empty()) break;
            append_crypto(frames, offset, data);
            ack_eliciting = true;
            sent.ranges.push_back(StreamRange{true, space, 0, offset, static_cast<std::uint64_t>(data.size())});
        }

        // HANDSHAKE_DONE tells the client its handshake is confirmed, which is
        // what lets it drop the handshake keys and stop sending Initials.
        if (space == PacketNumberSpace::Application && m_send_handshake_done) {
            Bytes done;
            append_handshake_done(done);
            if (frames.size() + done.size() <= budget) {
                frames += done;
                ack_eliciting = true;
                m_send_handshake_done = false;
            }
        }

        // Control frames that must be repeated until acknowledged.
        for (std::size_t i = 0; i < m_pending_control.size();) {
            PendingControl& control = m_pending_control[i];
            if (control.in_flight || control.bytes.empty() || control.space != space) {
                ++i;
                continue;
            }
            // A frame about a stream the peer has finished (or that is gone) can
            // only do harm: the peer may have retired the stream, and naming one
            // it no longer has is a stream state error at its end — the connection
            // then dies reporting our frame as the cause.
            if (control.subject != kNoSubjectStream) {
                auto subject = find_stream(control.subject);
                if (!subject || subject->remote_closed) {
                    m_pending_control.erase(m_pending_control.begin() + static_cast<std::ptrdiff_t>(i));
                    continue;
                }
            }
            if (frames.size() + control.bytes.size() > budget) break;
            frames += control.bytes;
            ack_eliciting = true;
            control.in_flight = true;
            sent.control_ids.push_back(control.id);
            used_control.push_back(control.id);
            ++i;
        }

        if (space != PacketNumberSpace::Application) return;

        // Stream data. Congestion control can stop it, but a probe cannot be
        // stopped: a connection whose only loss was a PTO would otherwise never
        // recover, because the packet it is waiting to re-send is exactly the
        // one the window is holding back.
        // A probe is exempt from the window because the packet it carries is the
        // one the window is holding back; a closing connection is exempt because
        // congestion control has nothing left to protect.
        const bool exempt = m_closing || (m_probe_pending && m_probe_space == space);
        if (!exempt && m_recovery.congestion().send_allowance() == 0) return;

        for (auto& [id, stream] : m_streams) {
            if (frames.size() + 32 >= budget) break;
            if (!stream->send.has_pending()) continue;
            // Two limits, two spaces. `limit` is what the peer will accept on
            // *this* stream, so it is the stream's own window. The connection's
            // limit is a sum over every stream's high-water mark, so what it
            // bounds is how many new octets may go out now — not any offset.
            // Taking the smaller of the two as if both were offsets lets a
            // connection-wide total be spent once per stream: enough concurrent
            // large responses then exceed what the peer granted and it closes the
            // connection with FLOW_CONTROL_ERROR.
            const std::uint64_t limit = stream->send_max_data;
            const std::uint64_t conn_room =
                m_send_data >= m_peer_max_data ? 0 : m_peer_max_data - m_send_data;
            const std::uint64_t before = stream->send.next_offset();
            const std::size_t room = budget - frames.size();
            SendStream::Chunk chunk;
            if (!stream->send.next(room > 24 ? room - 24 : 0, limit, conn_room, chunk)) continue;
            if (chunk.data.empty() && !chunk.fin) continue;
            append_stream(frames, id, chunk.offset, chunk.data, chunk.fin);
            ack_eliciting = true;
            sent.ranges.push_back(
                StreamRange{false, space, id, chunk.offset, static_cast<std::uint64_t>(chunk.data.size())});
            // Connection-level flow control counts every stream octet ever sent,
            // retransmissions included — but a retransmission does not raise the
            // high-water mark, so only the growth counts.
            const std::uint64_t end = chunk.offset + chunk.data.size();
            if (end > before) m_send_data += end - before;
        }
    }

    [[nodiscard]] std::uint64_t ack_delay_units(const AckTracker& tracker, Clock::time_point now) const {
        // The exponent is ours to choose and is advertised so the peer can undo
        // it (RFC 9000 §18.2). Applying it on send and removing it on receipt is
        // what keeps the two ends symmetric.
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now - tracker.largest_time());
        return static_cast<std::uint64_t>(micros.count()) >> m_config.ack_delay_exponent;
    }

    // --- receiving --------------------------------------------------------

    void process_datagram(std::span<const std::uint8_t> data) {
        std::size_t offset = 0;
        while (offset < data.size()) {
            const std::size_t consumed = process_one_packet(data.subspan(offset));
            if (consumed == 0) break;
            offset += consumed;
        }
    }

    // Parse and handle one packet, returning how many datagram octets it used.
    // Zero means "stop parsing this datagram".
    std::size_t process_one_packet(std::span<const std::uint8_t> data) {
        Reader r{data};
        PacketHeader hdr;
        switch (parse_packet_header(r, m_local_scid.size(), hdr)) {
            case PacketParseStatus::Ok:
                break;
            case PacketParseStatus::UnsupportedVersion:
                m_version_negotiation_requested = hdr;
                return 0;
            case PacketParseStatus::Retry:
                // A server never receives a Retry, and answering one would let a
                // single forged packet create a connection.
                return 0;
            case PacketParseStatus::VersionNegotiation:
            case PacketParseStatus::NotQuic:
            case PacketParseStatus::Malformed:
                return 0;
        }

        // A long header states its own length, which is what makes a coalesced
        // datagram splittable before anything is decrypted. A short header does
        // not: it runs to the end, and the RFC requires it to be last.
        std::size_t extent = data.size();
        if (hdr.long_header) {
            const std::uint64_t total = hdr.pn_offset + hdr.length;
            if (total > data.size()) return 0;
            extent = static_cast<std::size_t>(total);
        }

        EncryptionLevel level = EncryptionLevel::OneRtt;
        if (hdr.long_header) {
            switch (hdr.type) {
                case LongHeaderType::Initial:
                    level = EncryptionLevel::Initial;
                    break;
                case LongHeaderType::ZeroRtt:
                    level = EncryptionLevel::ZeroRtt;
                    break;
                case LongHeaderType::Handshake:
                    level = EncryptionLevel::Handshake;
                    break;
                case LongHeaderType::Retry:
                    return extent;
            }
        }

        // A packet is decrypted out of its own buffer, not out of the datagram:
        // header protection addresses "the first octet", which is the packet's,
        // and only happens to be the datagram's for the first packet in it.
        Bytes packet{reinterpret_cast<const char*>(data.data()), extent};
        process_protected_packet(std::move(packet), hdr, level, space_of_level(level));
        return extent;
    }

    bool process_protected_packet(Bytes packet, PacketHeader& hdr, EncryptionLevel level,
                                  PacketNumberSpace space) {
        const PacketKeys* keys = receive_keys(level);
        // No keys yet is not an error: 0-RTT and 1-RTT packets routinely arrive
        // before the handshake has produced them, and RFC 9001 §5.7 requires
        // them to be buffered rather than discarded.
        if (!keys || !keys->valid) {
            if ((level == EncryptionLevel::OneRtt || level == EncryptionLevel::ZeroRtt) &&
                m_undecryptable.size() < 32) {
                m_undecryptable.push_back(std::move(packet));
            }
            return false;
        }

        std::size_t pn_len = 0;
        if (!remove_header_protection(*keys, packet, hdr.pn_offset, pn_len)) return false;

        // The reserved bits are zero before header protection and must be zero
        // after it is removed; anything else means the peer set them
        // deliberately, which RFC 9000 §17.2 and §17.3.1 both make a connection
        // error. They are checked here because this is the first moment they are
        // readable — the mask covers exactly these bits.
        {
            const auto first = static_cast<std::uint8_t>(packet[0]);
            const std::uint8_t reserved = hdr.long_header ? 0x0c : 0x18;
            if ((first & reserved) != 0) {
                transport_close(TransportError::ProtocolViolation, kNoFrame, "reserved bits are not zero");
                return false;
            }
        }
        const std::size_t index = space_index(space);
        Reader r = reader_of(packet);
        if (!read_packet_number(r, hdr, m_acks[index].has_any() ? m_acks[index].largest() : 0)) return false;

        const std::size_t header_len = hdr.pn_offset + hdr.pn_len;
        const std::string_view aad{packet.data(), header_len};
        const std::string_view ciphertext{packet.data() + header_len, packet.size() - header_len};
        Bytes plaintext;
        if (!aead_open(*keys, hdr.packet_number, aad, ciphertext, plaintext)) {
            // A packet that will not authenticate is dropped, not a connection
            // error: an attacker injecting garbage must not be able to kill a
            // connection with it.
            return false;
        }

        if (!m_recovery.address_validated() &&
            (level == EncryptionLevel::Handshake || level == EncryptionLevel::OneRtt)) {
            // A packet that decrypts proves the peer receives at this address,
            // which is the whole of address validation for a server.
            m_recovery.set_address_validated(true);
            flush();
        }

        // A duplicate is still worth acknowledging, but nothing in it may be
        // processed twice (RFC 9000 §12.3).
        if (!m_acks[index].record(hdr.packet_number, Clock::now())) {
            // A duplicate is still worth acknowledging, but nothing in it may be
            // processed twice (RFC 9000 §12.3).
            m_ack_owed[index] = true;
            flush();
            return true;
        }

        // §12.4: a packet whose payload contains no frames — not even PADDING —
        // is a connection error. The check is "no frames" rather than "no bytes",
        // because a run of PADDING octets is itself a frame.
        if (plaintext.empty()) {
            transport_close(TransportError::ProtocolViolation, kNoFrame, "packet with no frames");
            return false;
        }

        Reader payload{reinterpret_cast<const std::uint8_t*>(plaintext.data()), plaintext.size()};
        Frame frame;
        bool saw_ack_eliciting = false;
        while (!payload.empty()) {
            const FrameParseStatus result = parse_frame(payload, frame);
            if (result == FrameParseStatus::Unknown) {
                transport_close(TransportError::FrameEncodingError, kNoFrame, "unknown frame type");
                return false;
            }
            if (result == FrameParseStatus::ProtocolViolation) {
                transport_close(TransportError::ProtocolViolation, kNoFrame, "non-minimal frame type");
                return false;
            }
            if (result != FrameParseStatus::Ok) {
                transport_close(TransportError::FrameEncodingError, kNoFrame, "malformed frame");
                return false;
            }
            if (frame.ack_eliciting()) saw_ack_eliciting = true;
            if (!handle_frame(frame, level, space)) return false;
            if (!m_alive) return false;
        }
        if (saw_ack_eliciting) {
            m_ack_owed[index] = true;
            flush();
        }
        m_last_activity = Clock::now();
        m_deadline = m_last_activity + std::chrono::milliseconds(m_config.max_idle_timeout_ms);
        return true;
    }

    // --- frame handling ---------------------------------------------------

    // False means the connection must stop processing (it is closing).
    bool handle_frame(const Frame& frame, EncryptionLevel level, PacketNumberSpace space) {
        // §12.5: most frames belong to the application space only, and the rule
        // matters — without it a peer could open streams before the handshake
        // proves who it is.
        if (space != PacketNumberSpace::Application) {
            switch (frame.type) {
                case FrameType::Padding:
                case FrameType::Ping:
                case FrameType::Ack:
                case FrameType::Crypto:
                case FrameType::ConnectionClose:
                    break;
                default:
                    transport_close(TransportError::ProtocolViolation,
                                    static_cast<std::uint64_t>(frame.type),
                                    "frame not permitted in this packet number space");
                    return false;
            }
        }
        if (level == EncryptionLevel::ZeroRtt && frame.type == FrameType::Crypto) {
            // §12.5: a 0-RTT packet may not carry CRYPTO, because the keys it is
            // protected with come from a session the peer has not proven it owns.
            transport_close(TransportError::ProtocolViolation, 0x06, "CRYPTO in a 0-RTT packet");
            return false;
        }

        switch (frame.type) {
            case FrameType::Padding:
            case FrameType::Ping:
                return true;
            case FrameType::Ack:
                return on_ack(frame, space);
            case FrameType::Crypto:
                return on_crypto(frame, level);
            case FrameType::Stream:
                return on_stream_frame(frame);
            case FrameType::ResetStream:
                return on_reset_stream(frame);
            case FrameType::StopSending:
                return on_stop_sending(frame);
            case FrameType::MaxData:
                if (frame.limit > kMaxStreamOffset) {
                    transport_close(TransportError::FrameEncodingError, 0x10, "MAX_DATA too large");
                    return false;
                }
                m_peer_max_data = std::max(m_peer_max_data, frame.limit);
                flush();
                return true;
            case FrameType::MaxStreamData: {
                // §19.10: a receive-only stream is one this endpoint can only
                // *receive* on — a unidirectional stream the peer opened — so a
                // window for us to send is meaningless, and the peer naming one
                // means it has the direction backwards.
                if (!stream_is_bidi(frame.stream_id) && is_remote_stream(frame.stream_id)) {
                    transport_close(TransportError::StreamStateError, 0x11,
                                    "MAX_STREAM_DATA for a receive-only stream");
                    return false;
                }
                bool bad = false;
                bool stale = false;
                auto stream = resolve_peer_stream(frame.stream_id, bad, stale);
                if (stale) return true;  // a frame about a stream that is closed and forgotten
                if (bad) {
                    transport_close(TransportError::StreamStateError, 0x11,
                                    "MAX_STREAM_DATA for unopened local stream");
                    return false;
                }
                if (frame.limit > kMaxStreamOffset) {
                    transport_close(TransportError::FrameEncodingError, 0x11, "MAX_STREAM_DATA too large");
                    return false;
                }
                stream->send_max_data = std::max(stream->send_max_data, frame.limit);
                flush();
                return true;
            }
            case FrameType::MaxStreams:
                if (frame.limit > (1ULL << 60)) {
                    transport_close(TransportError::FrameEncodingError, 0x12, "MAX_STREAMS too large");
                    return false;
                }
                if (frame.bidirectional) {
                    m_peer_max_streams_bidi = std::max(m_peer_max_streams_bidi, frame.limit);
                } else {
                    m_peer_max_streams_uni = std::max(m_peer_max_streams_uni, frame.limit);
                }
                return true;
            case FrameType::DataBlocked:
            case FrameType::StreamDataBlocked:
                // The peer is reporting itself stuck. Our windows are raised
                // from consumption, so there is nothing to react to beyond
                // making sure any queued MAX_* frame goes out.
                flush();
                return true;
            case FrameType::StreamsBlocked:
                // §19.14: the limit names a stream count, and one that would
                // permit a stream id beyond 2^62-1 cannot be represented. Only
                // the encoding is checked here; the count itself is a report, not
                // a request, so there is nothing else to act on.
                if (frame.limit > (1ULL << 60)) {
                    transport_close(TransportError::FrameEncodingError, 0x16, "STREAMS_BLOCKED too large");
                    return false;
                }
                flush();
                return true;
            case FrameType::NewConnectionId:
                return on_new_connection_id(frame);
            case FrameType::RetireConnectionId:
                if (frame.sequence >= m_next_cid_sequence) {
                    transport_close(TransportError::ProtocolViolation, 0x19,
                                    "RETIRE_CONNECTION_ID for an unissued sequence");
                    return false;
                }
                return true;
            case FrameType::PathChallenge:
                return on_path_challenge(frame);
            case FrameType::PathResponse:
                return on_path_response(frame);
            case FrameType::ConnectionClose:
                // The peer is done. Stop without answering: two endpoints
                // exchanging closes is how a connection never dies. The code and
                // reason are the peer's whole diagnosis, so they are logged —
                // silently treating it as "the connection ended" throws away the
                // only explanation of why.
                SIMPLE_HTTP_WARN_LOG("QUIC: peer closed the connection: app={} code=0x{:x} frame=0x{:x} reason={}",
                                     frame.application, frame.error_code, frame.frame_type, frame.reason);
                m_alive = false;
                return false;
            case FrameType::HandshakeDone:
                transport_close(TransportError::ProtocolViolation, 0x1e, "HANDSHAKE_DONE from a client");
                return false;
            case FrameType::NewToken:
                transport_close(TransportError::ProtocolViolation, 0x07, "NEW_TOKEN from a client");
                return false;
        }
        return true;
    }

    bool on_ack(const Frame& frame, PacketNumberSpace space) {
        AckOutcome outcome;
        if (!m_recovery.on_ack_received(frame, space, Clock::now(), outcome)) {
            transport_close(TransportError::ProtocolViolation, 0x02, "ACK for an unsent packet");
            return false;
        }

        std::set<std::uint64_t> acked_control;
        for (const SentPacket& packet : outcome.acked) {
            for (const std::uint64_t id : packet.control_ids) acked_control.insert(id);
            apply_ranges(packet, /*lost=*/false);
        }
        for (const SentPacket& packet : outcome.lost) apply_ranges(packet, /*lost=*/true);

        for (auto it = m_pending_control.begin(); it != m_pending_control.end();) {
            if (acked_control.count(it->id) != 0) {
                it = m_pending_control.erase(it);
            } else {
                ++it;
            }
        }
        for (SentPacket& packet : outcome.lost) {
            for (const std::uint64_t id : packet.control_ids) {
                for (auto& control : m_pending_control) {
                    if (control.id == id) control.in_flight = false;
                }
            }
            packet.control_ids.clear();
        }

        // Our acknowledgement of the peer's packets is only useful until the peer
        // says it has received it. Once it has, repeating those ranges would grow
        // the ACK frame forever.
        //
        // The number to forget below is the *peer's*, and it comes from the
        // packet the peer just acknowledged: that packet carried an ACK frame
        // saying "I have your packets up to N", and now that the peer has the
        // packet, it has that statement. This connection's own largest-acked is a
        // number in a different space entirely — both count from zero in
        // parallel and neither bounds the other — so pruning by it discards
        // ranges the peer was never told about. The ACK frame built next then
        // says nothing has been received at all, and the peer retransmits
        // everything it had already been told about: a round trip of stall per
        // occurrence, intermittent because it depends on how the two packet
        // number sequences happen to drift past each other.
        const std::size_t index = space_index(space);
        for (const SentPacket& packet : outcome.acked) {
            if (packet.acked_peer_largest != kNoPeerAck) {
                m_acks[index].drop_below(packet.acked_peer_largest);
            }
        }

        if (!outcome.acked.empty()) m_probe_pending = false;
        flush();
        return true;
    }

    // Hand one packet's byte ranges back to whatever owns them.
    void apply_ranges(const SentPacket& packet, bool lost) {
        for (const StreamRange& range : packet.ranges) {
            if (range.crypto) {
                if (lost) {
                    m_tls.crypto_lost(level_for_space(range.space), range.offset, range.length);
                } else {
                    m_tls.crypto_acked(level_for_space(range.space), range.offset, range.length);
                }
                continue;
            }
            auto stream = find_stream(range.stream_id);
            if (!stream) continue;
            if (lost) {
                stream->send.on_lost(range.offset, range.length);
            } else {
                stream->send.on_acked(range.offset, range.length);
                on_stream_acked(*stream);
            }
        }
    }

    void on_stream_acked(QuicStreamState& stream) {
        // Wake a producer parked on the outbound watermark.
        if (stream.write_space && stream.send.buffered() <= kStreamOutLowWatermark) {
            (void)stream.write_space->try_send(error_code{});
        }
        retire_stream_if_done(stream);
    }

    bool on_crypto(const Frame& frame, EncryptionLevel level) {
        if (m_tls.crypto_pending(level) > m_config.max_crypto_buffer) {
            transport_close(TransportError::CryptoBufferExceeded, 0x06, "crypto buffer exceeded");
            return false;
        }
        if (!m_tls.provide_crypto(level, frame.offset, frame.data)) {
            transport_close(TransportError::ProtocolViolation, 0x06, "crypto data at the wrong level");
            return false;
        }
        return drive_handshake();
    }

    // Advance the TLS handshake as far as the crypto data on hand allows.
    bool drive_handshake() {
        if (!m_tls.valid()) return true;
        for (;;) {
            const QuicTls::Status status = m_tls.tick();
            if (status == QuicTls::Status::Error) {
                transport_close(TransportError::InternalError, kNoFrame, "TLS handshake failed");
                return false;
            }
            if (status == QuicTls::Status::WantData) break;
            // A completed handshake keeps being ticked: post-handshake messages
            // (NewSessionTicket, a KeyUpdate) arrive through the same path.
            if (m_tls.complete()) break;
        }
        if (m_tls.complete() && !m_handshake_complete) {
            const std::string alpn = m_tls.alpn_selected();
            if (alpn != kHttp3Alpn) {
                // OpenSSL checked that *an* ALPN was negotiated, not that it was
                // ours. A client that offered only something else must be turned
                // away rather than served HTTP/3 it did not ask for.
                transport_close(TransportError::InternalError, kNoFrame, "ALPN is not h3");
                return false;
            }
            m_handshake_complete = true;
            SIMPLE_HTTP_DEBUG_LOG("QUIC: handshake complete (peer {}, alpn={})", m_peer.address().to_string(),
                                 alpn);
            m_recovery.set_handshake_confirmed(true);
            m_recovery.set_handshake_keys_available(true);
            m_send_handshake_done = true;
            replay_undecryptable();
            return on_handshake_complete();
        }
        flush();
        return true;
    }

    // Packets that arrived before their keys did are replayed once the keys
    // exist (RFC 9001 §5.7) — for a server that is the client's 1-RTT flight.
    void replay_undecryptable() {
        if (m_undecryptable.empty()) return;
        std::vector<Bytes> pending;
        pending.swap(m_undecryptable);
        for (Bytes& packet : pending) {
            PacketHeader hdr;
            Reader r = reader_of(packet);
            if (parse_packet_header(r, m_local_scid.size(), hdr) != PacketParseStatus::Ok) continue;
            const EncryptionLevel level = hdr.long_header && hdr.type == LongHeaderType::ZeroRtt
                                              ? EncryptionLevel::ZeroRtt
                                              : EncryptionLevel::OneRtt;
            if (!receive_keys(level) || !receive_keys(level)->valid) {
                m_undecryptable.push_back(std::move(packet));
                continue;
            }
            process_protected_packet(std::move(packet), hdr, level, space_of_level(level));
        }
    }

    bool on_handshake_complete() {
        // The peer's transport parameters are validated now that they are
        // authenticated: the connection IDs they name must match what was
        // actually observed, which is what detects an attacker rewriting a
        // connection ID in flight (RFC 9000 §7.3).
        const std::string_view encoded = m_tls.peer_transport_params();
        if (encoded.empty()) {
            // RFC 9001 §8.2: a ClientHello with no quic_transport_parameters
            // extension is answered with the missing_extension alert, and §4.8
            // turns a TLS alert into a CRYPTO_ERROR whose code is 0x100 + the
            // alert. Reporting TRANSPORT_PARAMETER_ERROR instead — which is what
            // decoding an empty buffer produces — sends the peer looking for a
            // parameter it never wrote.
            transport_close(static_cast<TransportError>(0x100 + 109), kNoFrame,
                            "no quic_transport_parameters extension");
            return false;
        }
        TransportParams peer_params;
        if (!decode_transport_params(
                std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(encoded.data()),
                                              encoded.size()},
                /*from_client=*/true, peer_params)) {
            transport_close(TransportError::TransportParameterError, kNoFrame, "invalid peer parameters");
            return false;
        }
        if (peer_params.initial_source_connection_id != m_peer_scid) {
            transport_close(TransportError::TransportParameterError, kNoFrame,
                            "initial_source_connection_id mismatch");
            return false;
        }

        m_peer_max_data = peer_params.initial_max_data;
        m_peer_initial_max_stream_data_bidi_local = peer_params.initial_max_stream_data_bidi_local;
        m_peer_initial_max_stream_data_bidi_remote = peer_params.initial_max_stream_data_bidi_remote;
        m_peer_initial_max_stream_data_uni = peer_params.initial_max_stream_data_uni;
        m_peer_max_streams_bidi = peer_params.initial_max_streams_bidi;
        m_peer_max_streams_uni = peer_params.initial_max_streams_uni;
        m_recovery.set_peer_ack_delay_exponent(peer_params.ack_delay_exponent);
        m_peer_max_udp_payload_size = peer_params.max_udp_payload_size;
        // The datagram size we may send is bounded by what the peer will accept,
        // and by our own conservative 1200 until path MTU is discovered.
        m_max_datagram_size = static_cast<std::size_t>(
            std::min<std::uint64_t>(peer_params.max_udp_payload_size, kMaxDatagramSize));

        // Streams created before their windows were known get the real limits
        // now; anything created later is made with them.
        for (auto& [id, stream] : m_streams) {
            const bool local = stream_is_local(id, /*server=*/true);
            const bool bidi = stream_is_bidi(id);
            std::uint64_t limit = 0;
            if (bidi) {
                limit = local ? m_peer_initial_max_stream_data_bidi_remote
                              : m_peer_initial_max_stream_data_bidi_local;
            } else {
                limit = m_peer_initial_max_stream_data_uni;
            }
            // Only the send window is filled in here: the receive window was
            // ours from the start and make_stream already set it.
            stream->send_max_data = std::max(stream->send_max_data, limit);
        }

        // A server must issue connection IDs up to the peer's limit so the peer
        // can migrate with them (RFC 9000 §5.1.1).
        issue_connection_id();

        // Initial and Handshake are done, and both spaces go. Dropping a space
        // drops its packets from loss recovery — a packet the peer can no longer
        // acknowledge would otherwise stay in flight forever, firing PTOs for a
        // space that will never produce another packet, and the exponential
        // backoff those PTOs drive is shared with the space that is still doing
        // work (RFC 9001 §4.9.2, RFC 9002 §B.9).
        m_initial_tx = PacketKeys{};
        m_initial_rx = PacketKeys{};
        m_recovery.discard_space(PacketNumberSpace::Initial);
        m_recovery.discard_space(PacketNumberSpace::Handshake);
        m_recovery.set_handshake_confirmed(true);
        flush();
        return true;
    }


    // Resolve a stream a control frame named, creating it if the peer was
    // entitled to open it.
    //
    // The RFC's rule is asymmetric and easy to invert: a *locally-initiated*
    // stream that has not been created is a stream state error (§19.4, §19.5,
    // §19.10), while a frame naming a peer-initiated stream the peer may open
    // *creates* it — the peer is not obliged to send a STREAM frame first when
    // all it wants to do is raise our window or cancel what we owe it. Getting
    // this backwards makes a connection that works against a hand-written client
    // fail against a real one, because a real one opens the stream and adjusts
    // its window from the same flight.
    // `error` is an id the peer had no right to name — a stream state error.
    // `stale` is an id whose stream is closed and forgotten: legal to receive,
    // and nothing to do with it.
    std::shared_ptr<QuicStreamState> resolve_peer_stream(std::uint64_t id, bool& error, bool& stale) {
        error = false;
        stale = false;
        if (auto stream = find_stream(id)) return stream;
        if (id > kMaxStreamId || !is_remote_stream(id)) {
            SIMPLE_HTTP_WARN_LOG("QUIC: frame named stream {} which is local and never created", id);
            error = true;
            return nullptr;
        }
        const bool bidi = stream_is_bidi(id);
        const std::uint64_t limit = bidi ? m_local_max_streams_bidi : m_local_max_streams_uni;
        if (id / 4 >= limit || m_streams.size() >= m_config.max_streams) {
            SIMPLE_HTTP_WARN_LOG("QUIC: stream {} is above the limit ({} created, limit {})", id, m_streams.size(),
                                 limit);
            error = true;
            return nullptr;
        }
        // A control frame naming a stream this endpoint has already retired is
        // stale — recreating it would hand the engine a stream it has already
        // finished with. The test is the retirement record, not how far the peer
        // has opened: the ids of one direction are consecutive, so a lower id
        // whose first frame is still in flight looks exactly like a retired one
        // when only the index is consulted.
        if (peer_stream_retired(id)) {
            stale = true;
            return nullptr;
        }
        auto transport = make_stream(id);
        (void)transport;
        m_incoming.push_back(id);
        (void)m_stream_notify->try_send(error_code{});
        return find_stream(id);
    }

    bool on_stream_frame(const Frame& frame) {
        const std::uint64_t id = frame.stream_id;
        if (id > kMaxStreamId) {
            transport_close(TransportError::FrameEncodingError, 0x08, "stream id too large");
            return false;
        }
        if (!is_remote_stream(id)) {
            // Client-initiated ids are 0 and 2; a client sending on 1 or 3 is
            // sending on a stream it does not own.
            transport_close(TransportError::StreamStateError, 0x08, "data on a locally-initiated stream");
            return false;
        }
        const bool bidi = stream_is_bidi(id);
        const std::uint64_t index = id / 4;
        const std::uint64_t limit = bidi ? m_local_max_streams_bidi : m_local_max_streams_uni;
        if (index >= limit) {
            transport_close(TransportError::StreamLimitError, 0x08, "stream id above our limit");
            return false;
        }
        // Stream ids of one kind are consecutive, so the highest index seen is
        // one less than the number opened. The credit rule below needs that
        // number to tell "the peer still has room" from "the peer is stuck".
        if (bidi) {
            m_peer_bidi_opened = std::max(m_peer_bidi_opened, index + 1);
        } else {
            m_peer_uni_opened = std::max(m_peer_uni_opened, index + 1);
        }

        auto stream = find_stream(id);
        if (!stream) {
            // A missing stream is not necessarily a new one. A stream this
            // endpoint has already retired is gone from the table, and a
            // *retransmission* of its data would otherwise look like the peer
            // opening it afresh — handing the engine the same request a second
            // time. Only the retirement record answers that, and answering it by
            // index alone is wrong: delivery order is not opening order, so a
            // stream whose first frame is still in flight sits below ids that
            // have already arrived and would be dropped as a retransmission. A
            // dropped *critical* stream leaves nothing to unblock the field
            // sections waiting on it, and the connection answers no further.
            //
            // (The frame is still acknowledged: whether we have anything to do
            // with the data is a separate question from whether it arrived.)
            if (peer_stream_retired(id)) return true;

            if (m_streams.size() >= m_config.max_streams) {
                transport_close(TransportError::StreamLimitError, 0x08, "too many streams");
                return false;
            }
            auto transport = make_stream(id);
            (void)transport;
            stream = find_stream(id);
            m_incoming.push_back(id);
            (void)m_stream_notify->try_send(error_code{});
        }
        // A STREAM frame on a stream the peer has already finished is not an
        // error: RFC 9000 §19.8 permits retransmission of data below the final
        // size, and a peer that repeats a frame because it believes the first
        // was lost is doing exactly that. What *is* an error — data past the
        // final size — is caught by the reassembler below, which knows the size
        // and ignores what it already has; rejecting the frame here would refuse
        // the retransmission as well.

        const std::uint64_t end = frame.offset + frame.data.size();
        // Connection-level flow control counts the growth of every stream's high
        // mark, and exceeding it is a connection error — unlike the stream-level
        // limit below, which must not take the connection down.
        if (end > stream->recv_highest) {
            if (m_recv_data + (end - stream->recv_highest) > m_recv_max_data) {
                transport_close(TransportError::FlowControlError, 0x08, "connection flow control exceeded");
                return false;
            }
        }
        if (end > stream->recv.max_data()) {
            reset_stream_impl(stream, static_cast<std::uint64_t>(TransportError::FlowControlError));
            return true;
        }

        if (!stream->recv.push(frame.offset, frame.data, frame.fin)) {
            transport_close(TransportError::FinalSizeError, 0x08, "stream final size conflict");
            return false;
        }
        if (end > stream->recv_highest) {
            m_recv_data += end - stream->recv_highest;
            stream->recv_highest = end;
        }
        if (frame.fin) stream->remote_closed = true;
        if (stream->notify) (void)stream->notify->try_send(error_code{});
        return true;
    }

    bool on_reset_stream(const Frame& frame) {
        bool bad = false;
        bool stale = false;
        auto stream = resolve_peer_stream(frame.stream_id, bad, stale);
        if (stale) return true;  // a frame about a stream that is closed and forgotten
        if (bad) {
            transport_close(TransportError::StreamStateError, 0x04,
                            "RESET_STREAM for unopened local stream");
            return false;
        }
        // A RESET_STREAM for a stream that has already finished is not an error:
        // the stream moves from "Data Recvd" to "Reset Recvd" (RFC 9000 §3.2), and
        // a repeat of one already received is redundant — the peer is entitled to
        // resend it if it thinks the first was lost. Refusing either would let a
        // perfectly ordinary teardown kill the connection.
        if (stream->recv.reset_received()) return true;
        // The final size still has to agree with what already arrived, or a peer
        // could shrink a stream it had already grown.
        if (frame.final_size < stream->recv_highest) {
            transport_close(TransportError::FinalSizeError, 0x04, "RESET_STREAM final size too small");
            return false;
        }
        stream->recv.reset(frame.error_code);
        stream->remote_closed = true;
        stream->send.reset(frame.error_code);
        if (stream->notify) (void)stream->notify->try_send(error_code{});
        retire_stream_if_done(*stream);
        return true;
    }

    bool on_stop_sending(const Frame& frame) {
        bool bad = false;
        bool stale = false;
        auto stream = resolve_peer_stream(frame.stream_id, bad, stale);
        if (stale) return true;  // a frame about a stream that is closed and forgotten
        if (bad) {
            transport_close(TransportError::StreamStateError, 0x05,
                            "STOP_SENDING for unopened local stream");
            return false;
        }
        // The peer does not want our data. Answering with RESET_STREAM is what
        // makes STOP_SENDING a request rather than a notification
        // (RFC 9000 §3.5).
        if (!stream->send.reset_sent()) {
            reset_stream_impl(stream, frame.error_code);
        }
        return true;
    }

    bool on_new_connection_id(const Frame& frame) {
        if (frame.sequence >= 8) {
            // More than a handful is more than we will hold; the peer's
            // active_connection_id_limit is what bounds it in practice.
            transport_close(TransportError::ConnectionIdLimitError, 0x18, "too many connection IDs");
            return false;
        }
        if (frame.retire_prior_to > frame.sequence) {
            transport_close(TransportError::FrameEncodingError, 0x18, "retire_prior_to above sequence");
            return false;
        }
        m_peer_cids[frame.sequence] = frame.connection_id;
        return true;
    }

    bool on_path_challenge(const Frame& frame) {
        // The response goes to the address the challenge came from, not to the
        // address we currently think the peer is at: that is the whole point of
        // the exchange (RFC 9000 §8.2).
        Bytes response;
        append_path_response(response, frame.path_data);
        queue_control(std::move(response));
        flush();
        return true;
    }

    bool on_path_response(const Frame& frame) {
        if (!m_migration_peer.has_value() || frame.path_data != m_migration_challenge) return true;
        // The new path works. Switch to it and start a fresh congestion window:
        // the old window measured a different path and has nothing to say about
        // this one (RFC 9000 §9.4).
        m_peer = *m_migration_peer;
        m_migration_peer.reset();
        m_recovery.congestion() = NewReno(m_max_datagram_size);
        m_recovery.set_address_validated(true);
        issue_connection_id();
        if (!m_pending_migrating_datagram.empty()) {
            Bytes replay = std::move(m_pending_migrating_datagram);
            m_pending_migrating_datagram.clear();
            process_datagram(std::span<const std::uint8_t>{
                reinterpret_cast<const std::uint8_t*>(replay.data()), replay.size()});
        }
        return true;
    }

    // Returns false when the datagram must not be processed as the peer's.
    bool on_migrating_datagram(std::span<const std::uint8_t> data, const asio::ip::udp::endpoint& from) {
        if (m_config.disable_active_migration) return false;
        if (m_migration_peer.has_value() && *m_migration_peer == from) {
            // A challenge is already outstanding for this address; hold the
            // datagram until the response proves it real.
            m_pending_migrating_datagram.assign(reinterpret_cast<const char*>(data.data()), data.size());
            return false;
        }
        // A packet from an unfamiliar address could be a migration or a spoof.
        // The only way to tell is to make the peer prove it can receive there,
        // so a challenge goes out and the datagram waits (RFC 9000 §9).
        m_migration_peer = from;
        for (auto& byte : m_migration_challenge) byte = static_cast<std::uint8_t>(m_random());
        Bytes challenge;
        append_path_challenge(challenge, m_migration_challenge);
        queue_control(std::move(challenge));
        m_pending_migrating_datagram.assign(reinterpret_cast<const char*>(data.data()), data.size());
        flush();
        return false;
    }

    void issue_connection_id() {
        if (m_next_cid_sequence >= 2) return;
        const std::uint64_t sequence = m_next_cid_sequence++;
        std::string cid = m_cid_factory ? m_cid_factory() : std::string(m_config.connection_id_length, '\0');
        std::array<std::uint8_t, 16> token{};
        for (auto& byte : token) byte = static_cast<std::uint8_t>(m_random());
        m_issued_cids[sequence] = cid;
        Bytes frame;
        append_new_connection_id(frame, sequence, 0, cid, token);
        queue_control(std::move(frame));
    }

    // --- housekeeping -----------------------------------------------------

    void reset_stream_impl(const std::shared_ptr<QuicStreamState>& stream, std::uint64_t error_code) {
        if (!stream || stream->send.reset_sent()) return;
        stream->send.reset(error_code);
        Bytes frame;
        // The final size is this endpoint's own: it is the size of the stream in
        // the direction *we* send (§19.4). `recv_highest` is the other direction
        // — what the peer sent us — and announcing it here tells the peer we sent
        // fewer octets than it has already received, which §4.5 makes it close the
        // whole connection over (FINAL_SIZE_ERROR). Cancelling a large download is
        // the ordinary way to reach this: the request head is a few hundred bytes
        // and the response already on the wire is not.
        append_reset_stream(frame, stream->id, error_code, stream->send.next_offset());
        queue_control(std::move(frame));
        if (stream->notify) stream->notify->close();
        flush();
    }

    void retire_stream(std::uint64_t id) {
        auto it = m_streams.find(id);
        if (it == m_streams.end()) return;
        if (it->second->notify) it->second->notify->close();
        if (it->second->write_space) it->second->write_space->close();
        m_streams.erase(it);
        m_transports.erase(id);
        {
            static const auto t0 = Clock::now();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
            SIMPLE_HTTP_INFO_LOG("QUIC: retired stream {} at {}ms ({} live)", id, ms, m_streams.size());
        }
        if (is_remote_stream(id)) {
            record_peer_stream_retired(id);
            credit_stream_slot(stream_is_bidi(id));
        }
    }

    // --- which peer streams are already retired ---------------------------
    //
    // The only reason to drop a frame naming a stream this endpoint does not have
    // is that the stream was *retired* — a retransmission of data the engine has
    // finished with. "Below the highest id the peer has opened" does not mean
    // that: the peer opens the streams of one direction consecutively, but the
    // frames arrive in whatever order the network delivers them, so an id below
    // the highest seen may simply be a stream whose first frame is later.
    //
    // Retirement is recorded per direction as a watermark over the ids that
    // retired in order, plus the ones that retired ahead of it. Traffic retires
    // in order, so the set stays empty; a stream that never arrives holds the
    // watermark back, and with it the stream credit, which is what keeps the set
    // bounded by the credit in flight rather than by the life of the connection.
    [[nodiscard]] bool peer_stream_retired(std::uint64_t id) const {
        const std::uint64_t index = id / 4;
        if (stream_is_bidi(id)) {
            return index < m_peer_bidi_retired_below || m_peer_bidi_retired_ooo.contains(index);
        }
        return index < m_peer_uni_retired_below || m_peer_uni_retired_ooo.contains(index);
    }

    void record_peer_stream_retired(std::uint64_t id) {
        const std::uint64_t index = id / 4;
        const bool bidi = stream_is_bidi(id);
        auto& below = bidi ? m_peer_bidi_retired_below : m_peer_uni_retired_below;
        auto& ooo = bidi ? m_peer_bidi_retired_ooo : m_peer_uni_retired_ooo;
        if (index != below) {
            ooo.insert(index);
            return;
        }
        ++below;
        while (ooo.erase(below) != 0) ++below;
    }

    // A stream the peer opened has closed, so its slot can be advertised again.
    //
    // The limit is *cumulative* (RFC 9000 §4.6), which means closing a stream
    // frees nothing by itself: until the credit is sent, the peer is simply out.
    // A connection that never sends MAX_STREAMS serves exactly as many requests
    // as it first advertised and then stops — which is what a load run against
    // it shows, at precisely the initial_max_streams count.
    //
    // Batched, because a frame per closed stream is overhead on every one of
    // them; but flushed the moment the peer might be out, because §4.6 is
    // explicit that an endpoint must not wait to be told: the peer would be
    // blocked for a round trip at best, and forever if it never sends
    // STREAMS_BLOCKED.
    void credit_stream_slot(bool bidi) {
        if (bidi) {
            ++m_stream_credit_bidi;
        } else {
            ++m_stream_credit_uni;
        }
        const std::uint64_t accrued = bidi ? m_stream_credit_bidi : m_stream_credit_uni;
        const std::uint64_t limit = bidi ? m_local_max_streams_bidi : m_local_max_streams_uni;
        const std::uint64_t opened = bidi ? m_peer_bidi_opened : m_peer_uni_opened;
        // The peer is out — or would be after the credit it does not know about
        // yet — so this cannot wait for a batch.
        const bool peer_may_be_stuck = opened + accrued >= limit;
        if (!peer_may_be_stuck && accrued < kStreamCreditBatch) return;

        if (bidi) {
            m_stream_credit_bidi = 0;
            m_local_max_streams_bidi += accrued;
        } else {
            m_stream_credit_uni = 0;
            m_local_max_streams_uni += accrued;
        }
        Bytes frame;
        append_max_streams(frame, bidi ? m_local_max_streams_bidi : m_local_max_streams_uni, bidi);
        queue_control(std::move(frame));
        flush();
    }

    void retire_stream_if_done(QuicStreamState& stream) {
        // Both halves have to be over, and on the send side "over" is not "the
        // buffer is empty right now". The peer's FIN arrives with the request,
        // long before the response is written, so `remote_closed` is true from the
        // start; and `all_acked` is true whenever nothing is buffered, which for a
        // stream that has written one chunk and not yet the next is exactly the
        // state after that chunk is acknowledged. Retiring there drops the stream
        // the application is still writing to: the next write succeeds — nothing
        // marks the stream closed — and the bytes go nowhere, because the send
        // path walks `m_streams` and this entry is gone. A streamed response is
        // then truncated in silence.
        //
        // So a stream the application has written to waits for its FIN to be
        // acknowledged. A stream it never wrote to has no FIN to wait for, and
        // that is not a special case but the ordinary shape of a peer's
        // unidirectional stream: requiring a FIN there would keep every control
        // and QPACK stream in the table for the life of the connection.
        // Both halves have to be over, and on the send side "over" is not "the
        // buffer is empty right now". The peer's FIN arrives with the request,
        // long before the response is written, so `remote_closed` is true from the
        // start; and `all_acked` is true whenever nothing is outstanding, which
        // for a stream that has written one chunk and not yet the next is exactly
        // the state after that chunk is acknowledged. Retiring there drops the
        // stream the application is still writing to: the next write succeeds —
        // nothing marks the stream closed — and the bytes go nowhere, because the
        // send path walks `m_streams` and this entry is gone. A streamed response
        // is then truncated in silence. See `SendStream::send_complete`.
        if (stream.remote_closed && stream.send.send_complete()) retire_stream(stream.id);
    }

    // A stream the peer initiated: the low bit of its id says who opened it, and
    // a client's streams all have it clear.
    [[nodiscard]] static bool is_remote_stream(std::uint64_t id) noexcept {
        return (id & kStreamIdInitiatorBit) == 0;
    }

    static PacketNumberSpace space_of_level(EncryptionLevel level) {
        switch (level) {
            case EncryptionLevel::Initial:
                return PacketNumberSpace::Initial;
            case EncryptionLevel::Handshake:
                return PacketNumberSpace::Handshake;
            default:
                return PacketNumberSpace::Application;
        }
    }

    static EncryptionLevel level_for_space(PacketNumberSpace space) {
        switch (space) {
            case PacketNumberSpace::Initial:
                return EncryptionLevel::Initial;
            case PacketNumberSpace::Handshake:
                return EncryptionLevel::Handshake;
            case PacketNumberSpace::Application:
                return EncryptionLevel::OneRtt;
        }
        return EncryptionLevel::OneRtt;
    }

    asio::awaitable<void> timer_loop() {
        asio::steady_timer timer{m_executor};
        for (;;) {
            if (!m_alive) co_return;
            const Clock::time_point now = Clock::now();

            Clock::time_point wake = m_deadline;
            const auto [loss_time, loss_space] = m_recovery.loss_time();
            if (loss_time != Clock::time_point{} && loss_time < wake) wake = loss_time;
            const auto [pto_time, pto_space] = m_recovery.pto(now);
            if (pto_time != Clock::time_point{} && pto_time < wake) wake = pto_time;
            if (m_closing || !m_recovery.has_ack_eliciting_in_flight()) {
                // Nothing outstanding means nothing to detect lost; only the
                // idle timeout is left to watch, plus the closing state's grace
                // period for the peer to see our close.
                const Clock::time_point closing_deadline = now + 3 * m_recovery.rtt().pto_base();
                if (m_closing && closing_deadline < wake) wake = closing_deadline;
            }
            if (wake <= now) wake = now + std::chrono::milliseconds(1);

            timer.expires_at(wake);
            co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
            if (!m_alive) co_return;

            const Clock::time_point fired = Clock::now();
            if (fired >= m_deadline) {
                // Nothing has arrived for the idle timeout, so the connection is
                // over whether or not either side says so.
                m_alive = false;
                co_return;
            }
            if (m_closing && fired >= now + 3 * m_recovery.rtt().pto_base()) {
                m_alive = false;
                co_return;
            }

            const auto [loss_now, loss_space_now] = m_recovery.loss_time();
            if (loss_now != Clock::time_point{} && fired >= loss_now) {
                AckOutcome outcome;
                m_recovery.on_loss_timeout(loss_space_now, fired, outcome);
                for (const SentPacket& packet : outcome.lost) {
                    apply_ranges(packet, /*lost=*/true);
                    for (const std::uint64_t id : packet.control_ids) {
                        for (auto& control : m_pending_control) {
                            if (control.id == id) control.in_flight = false;
                        }
                    }
                }
                flush();
                continue;
            }

            auto [pto_now, pto_space_now] = m_recovery.pto(fired);
            if (pto_now != Clock::time_point{} && fired >= pto_now) {
                // A probe: the peer has been silent for a PTO, so something has
                // to go out that demands an acknowledgement, even if nothing new
                // is queued (RFC 9002 §6.2.4: "a sender MUST send at least one
                // ack-eliciting packet ... as a probe"). Without it a connection
                // whose only loss was the last packet would wait forever.
                //
                // It goes out unconditionally, and that is the point. Queuing it
                // only when nothing ack-eliciting is in flight assumes loss
                // recovery is about to resend whatever is in flight — but
                // detection needs a largest acknowledged to work from, so an
                // opening flight that was lost in its entirety is never declared
                // lost and never resent. The probe is then suppressed by the very
                // packet it should be replacing: the endpoint sends ACK-only
                // packets, which elicit nothing, the peer stays silent, the PTO
                // doubles, and the connection spends the rest of its life backing
                // off through a space it will never leave.
                m_recovery.on_pto_fired();
                m_probe_pending = true;
                m_probe_space = pto_space_now;
                // The probe carries whatever the peer is missing, when there is
                // such a packet: a bare PING elicits an acknowledgement and
                // nothing else, which is not enough when what the peer is waiting
                // for is our data. See LossRecovery::on_pto_probe.
                {
                    AckOutcome outcome;
                    m_recovery.on_pto_probe(pto_space_now, outcome);
                    for (const SentPacket& packet : outcome.lost) apply_ranges(packet, /*lost=*/true);
                }
                Bytes ping;
                append_ping(ping);
                queue_control(std::move(ping));
                flush();
            }
        }
    }

    // --- state ------------------------------------------------------------

    Executor m_executor;
    QuicConnectionConfig m_config;
    std::string m_original_dcid;
    std::string m_peer_scid;
    std::string m_local_scid;
    QuicDatagramSink m_sink;
    asio::ip::udp::endpoint m_peer;
    std::function<std::string()> m_cid_factory;

    QuicTls m_tls;
    PacketKeys m_initial_tx;
    PacketKeys m_initial_rx;
    PacketKeys m_zero_rtt_rx;
    PacketKeys m_one_rtt_rx;
    PacketKeys m_one_rtt_tx;
    bool m_key_phase{false};

    LossRecovery m_recovery;
    std::array<AckTracker, kPacketNumberSpaceCount> m_acks{};
    std::array<std::uint64_t, kPacketNumberSpaceCount> m_next_pn{};
    // Whether an acknowledgement is *owed* for each space. Distinct from "the
    // tracker has ranges": those stay recorded so a duplicate can be recognised,
    // and treating them as a reason to send would make the write loop spin
    // forever on the same ACK frame.
    std::array<bool, kPacketNumberSpaceCount> m_ack_owed{};

    std::map<std::uint64_t, std::shared_ptr<QuicStreamState>> m_streams;
    std::map<std::uint64_t, std::shared_ptr<StreamTransport>> m_transports;
    std::deque<std::uint64_t> m_incoming;
    std::shared_ptr<asio::experimental::concurrent_channel<void(error_code)>> m_stream_notify;
    std::shared_ptr<asio::experimental::concurrent_channel<void(error_code)>> m_send_notify;

    struct PendingControl {
        Bytes bytes;
        PacketNumberSpace space;
        std::uint64_t id;
        bool in_flight;
        // The stream the frame is about, when it is about one. Checked at send
        // time rather than at queue time: a credit is queued the moment the
        // application consumes, and the FIN that makes it pointless can arrive
        // in between.
        std::uint64_t subject{kNoSubjectStream};
    };
    std::deque<PendingControl> m_pending_control;
    std::uint64_t m_next_control_id{1};

    // Flow control, both directions.
    std::uint64_t m_peer_max_data{0};
    std::uint64_t m_send_data{0};
    std::uint64_t m_peer_initial_max_stream_data_bidi_local{0};
    std::uint64_t m_peer_initial_max_stream_data_bidi_remote{0};
    std::uint64_t m_peer_initial_max_stream_data_uni{0};
    // HTTP/3 needs three unidirectional streams; a peer that grants none would
    // make the protocol unusable, so this starts at the minimum it requires
    // rather than at zero.
    std::uint64_t m_peer_max_streams_bidi{0};
    std::uint64_t m_peer_max_streams_uni{3};
    std::uint64_t m_recv_max_data{0};
    std::uint64_t m_recv_data{0};
    std::uint64_t m_recv_consumed{0};
    std::uint64_t m_recv_credited{0};
    std::uint64_t m_recv_data_start{0};
    std::uint64_t m_local_max_streams_bidi{0};
    std::uint64_t m_local_max_streams_uni{0};
    // How many streams of each kind the peer has opened, and how many of its
    // closed ones have not been credited back yet.
    std::uint64_t m_peer_bidi_opened{0};
    std::uint64_t m_peer_uni_opened{0};
    // Retired peer stream indices, as a watermark plus the ids that retired out
    // of order — see peer_stream_retired().
    std::uint64_t m_peer_bidi_retired_below{0};
    std::uint64_t m_peer_uni_retired_below{0};
    std::set<std::uint64_t> m_peer_bidi_retired_ooo;
    std::set<std::uint64_t> m_peer_uni_retired_ooo;
    std::uint64_t m_stream_credit_bidi{0};
    std::uint64_t m_stream_credit_uni{0};
    std::uint64_t m_next_local_bidi{1};  // a server's bidirectional streams: 1, 5, 9, ...
    std::uint64_t m_next_local_uni{3};   // ... and unidirectional: 3, 7, 11, ...

    std::map<std::uint64_t, std::string> m_peer_cids;
    std::map<std::uint64_t, std::string> m_issued_cids;
    std::uint64_t m_next_cid_sequence{1};

    std::vector<Bytes> m_undecryptable;
    std::optional<asio::ip::udp::endpoint> m_migration_peer;
    std::array<std::uint8_t, 8> m_migration_challenge{};
    Bytes m_pending_migrating_datagram;
    PacketHeader m_version_negotiation_requested;

    SSL_CTX* m_ssl_ctx{nullptr};
    std::uint64_t m_bytes_received{0};
    std::uint64_t m_bytes_sent{0};
    std::size_t m_max_datagram_size{kMaxDatagramSize};
    std::uint64_t m_peer_max_udp_payload_size{kDefaultMaxUdpPayloadSize};

    std::uint64_t m_close_error{0};
    std::uint64_t m_close_frame_type{kNoFrame};
    std::string m_close_reason;
    bool m_close_application{false};
    bool m_close_sent{false};
    bool m_closing{false};
    // How many datagrams of already-queued data may still go out before the
    // CONNECTION_CLOSE. Enough for a small frame plus its retransmission, few
    // enough that a peer which stopped reading cannot hold the close off.
    unsigned m_close_drain{4};
    bool m_handshake_complete{false};
    bool m_send_handshake_done{false};
    bool m_probe_pending{false};
    PacketNumberSpace m_probe_space{PacketNumberSpace::Initial};
    bool m_alive{true};

    Clock::time_point m_deadline{};
    Clock::time_point m_last_activity{};
    std::mt19937_64 m_random{std::random_device{}()};
};

}  // namespace simple_http::quic
