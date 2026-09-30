#pragma once

// One QUIC connection, owned by ngtcp2.
//
// What this file is *not* any more: a QUIC implementation. Packet parsing,
// header protection, key derivation, ACK tracking, loss detection, PTO,
// congestion control, stream state machines and flow control all live in
// ngtcp2 now. What is left is the part a library cannot do for us — driving it
// from asio, and deciding when to send, when to close, and who to tell.
//
// The shape of that driving:
//
//   * One coroutine, `run()`, owns the connection's lifetime. It writes
//     datagrams out, then parks until either a wake-up (a datagram arrived,
//     crypto data is ready, an ACK freed the window) or ngtcp2's own timer
//     expiry — whichever comes first.
//
//   * `on_datagram()` is called by the endpoint on this connection's executor,
//     never concurrently. It hands the bytes to ngtcp2 and pokes the loop.
//
//   * The `Protocol` (the HTTP/3 engine) is pulled from, not pushed to: when
//     ngtcp2 asks for a packet, this class asks the engine for the next slice
//     of stream data. That inversion is nghttp3's design, and it is why the
//     engine has no write loop of its own.
//
// Everything here runs on the connection's single-threaded executor
// (concurrency model A), so there are no locks. The callbacks ngtcp2 invokes
// are the exception to "no re-entrancy": they must not call back into ngtcp2,
// must not throw, and must not block — see `protocol.h`.

#include <ngtcp2/ngtcp2.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <boost/asio.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__linux__)
// The GSO knobs (SOL_UDP, UDP_SEGMENT) come from the libc <netinet/udp.h>,
// which exported UDP_SEGMENT since glibc 2.36. It has to be pulled in before
// the `kPacketsPerFlush` probe below evaluates; on older glibc the macro is
// absent and batching quietly stays off, which is fine. (The kernel's own
// <linux/udp.h> is deliberately *not* included: it declares `struct udphdr`,
// which the libc header also declares, and the redefinition is a compile
// error.)
#include <netinet/udp.h>
#endif

#include "../core/logging.h"
#include "../core/types.h"
#include "../transport/transport.h" // SslHandle
#include "ngtcp2_config.h"
#include "ngtcp2_crypto.h"
#include "protocol.h"
#include "tls.h"

namespace simple_http::quic {

namespace asio = boost::asio;

// One datagram in flight, and where it goes.
//
// When `gso_size` is nonzero, `data` is a GSO segment: several QUIC packets
// written back to back, which the kernel splits into separate datagrams on the
// wire (UDP_SEGMENT). The connection allocates the buffer; from here on the
// endpoint owns it, and it is *moved* into the send queue, never copied.
struct SendItem {
    std::vector<std::uint8_t> data;
    asio::ip::udp::endpoint to;
    std::uint16_t gso_size{0};
};

// The connection hands finished datagrams to the endpoint, which owns the
// socket.
using DatagramSink = std::function<void(SendItem &&)>;

// What the endpoint learned from the client's first Initial, before a
// connection existed to hold it.
struct QuicBootstrap {
    // The client's Source Connection ID, which becomes our Destination
    // Connection ID: RFC 9000 §7.2 requires the server to send it back.
    std::array<std::uint8_t, NGTCP2_MAX_CIDLEN> client_scid{};
    std::size_t client_scid_len{0};
    // The Destination Connection ID the client used. This is the
    // original_destination_connection_id the server must echo in its transport
    // parameters (§7.3), and it is what the client compares against to detect a
    // forged first flight.
    std::array<std::uint8_t, NGTCP2_MAX_CIDLEN> original_dcid{};
    std::size_t original_dcid_len{0};
    // True when this Initial came in answer to a Retry, in which case the
    // client's DCID is the Retry's SCID and has to be echoed as
    // retry_source_connection_id.
    bool retried{false};
    // The version the client chose (already validated against our list).
    std::uint32_t version{0};
    // The Retry token the client echoed back, if any, and ngtcp2's
    // classification of it. Copied rather than referenced: ngtcp2 keeps reading
    // the token while the connection is being set up, and the datagram it
    // arrived in is gone by then.
    std::vector<std::uint8_t> token{};
    ngtcp2_token_type token_type{NGTCP2_TOKEN_TYPE_UNKNOWN};
};

// The largest datagram we will produce. QUIC's minimum MTU is 1200; 1500 covers
// a stock Ethernet path without fragmentation, and ngtcp2 will not exceed the
// peer's advertised limit or our own `max_udp_payload_size`.
inline constexpr std::size_t kMaxDatagram = 1500;

// How many packets the send loop packs into one UDP datagram. Each packet in a
// batch is padded to the path MTU (see the PADDING flag in `write_pkt`), which
// is what makes ngtcp2's packet aggregation engage — `write_aggregate_pkt2`
// only batches when the first packet is full. The endpoint sends the batch with
// UDP_SEGMENT, so every packet still lands as its own datagram on the wire, for
// the price of one syscall instead of one per packet.
//
// On platforms without GSO (non-Linux), batching is off: one packet per
// datagram, exactly what the stack has always done — the aggregation above
// exists only because GSO can deliver it.
#if defined(__linux__) && defined(SOL_UDP) && defined(UDP_SEGMENT)
inline constexpr std::size_t kPacketsPerFlush = 8;
#else
inline constexpr std::size_t kPacketsPerFlush = 1;
#endif

// What `ngtcp2_conn_get_expiry2` returns when the connection has no timer
// armed.
inline constexpr ngtcp2_tstamp kNoExpiry = std::numeric_limits<ngtcp2_tstamp>::max();

// Bound on stream data held before the protocol engine exists. It can only be
// filled by a client that sends application data in the same flight as its
// handshake, so a generous cap is still far more than a well-behaved peer will
// ever reach; past it the connection is closed rather than buffered.
inline constexpr std::size_t kMaxPendingStreamBytes = 256 * 1024;

// One STREAM event held until an engine can consume it.
struct PendingStreamData {
    std::uint32_t flags{0};
    std::int64_t stream_id{-1};
    std::vector<std::uint8_t> data{};
};

template <typename Executor>
class QuicConnection : public std::enable_shared_from_this<QuicConnection<Executor>>, public ConnectionCryptoBase {
  public:
    using executor_type = Executor;
    using Timer = asio::steady_timer;
    using Channel = asio::experimental::concurrent_channel<void(error_code)>;

    QuicConnection(Executor exec, SSL_CTX *ssl_ctx, QuicConnectionConfig config, QuicBootstrap bootstrap,
                   std::vector<std::uint32_t> versions, asio::ip::udp::endpoint local, asio::ip::udp::endpoint remote,
                   DatagramSink sink)
        : m_executor(exec), m_timer(exec), m_wake(exec, 1), m_closed_signal(exec, 1), m_config(config),
          m_bootstrap(bootstrap), m_versions(std::move(versions)), m_local(std::move(local)),
          m_remote(std::move(remote)), m_sink(std::move(sink)), m_crypto(this) {
        ngtcp2_ccerr_default(&m_last_error);
        if (!m_crypto.init(ssl_ctx)) {
            m_init_failed = true;
        }
    }

    ~QuicConnection() override {
        if (m_conn)
            ngtcp2_conn_del(m_conn);
    }

    QuicConnection(const QuicConnection &) = delete;
    QuicConnection &operator=(const QuicConnection &) = delete;

    [[nodiscard]] Executor get_executor() const { return m_executor; }
    [[nodiscard]] bool closed() const noexcept { return m_closed; }
    [[nodiscard]] bool init_failed() const noexcept { return m_init_failed; }

    // The peer as the rest of the library thinks of it. Requests carry a
    // tcp::endpoint because that is what the h1/h2 engines have always
    // produced; QUIC has no port of its own to report, so the UDP one stands
    // in.
    [[nodiscard]] asio::ip::tcp::endpoint peer() const {
        return asio::ip::tcp::endpoint{m_remote.address(), m_remote.port()};
    }

    [[nodiscard]] SslHandle tls_handle() const {
        auto *self = const_cast<QuicConnection *>(this);
        SSL *ssl = self->m_crypto.ssl();
        return ssl != nullptr ? SslHandle{ssl} : SslHandle{};
    }

    // --- endpoint-facing -----------------------------------------------------

    // Create the ngtcp2 connection. Separate from the constructor because it
    // can fail, and because the endpoint must be able to report why.
    [[nodiscard]] bool init();

    // Feed one received datagram in. Runs on this connection's executor.
    void on_datagram(std::span<const std::uint8_t> datagram);

    // Drive the connection until it is closed. The endpoint spawns this; when it
    // returns, the connection is finished and `closed()` is true.
    asio::awaitable<void> run();

    // --- protocol-engine-facing ---------------------------------------------

    // Hand the connection its protocol engine.
    //
    // The endpoint feeds the client's Initial as soon as the connection exists,
    // because that packet is what starts the handshake — but the engine that
    // rides on the connection is built by the *server*, asynchronously, a moment
    // later. Anything ngtcp2 reports in between would otherwise be delivered to
    // nobody: the 1-RTT keys (which is when the engine may open its control
    // streams at all), and any stream data that arrived in the same flight.
    //
    // So those two are held here and replayed on registration. The reference
    // implementation instead drops them on the floor, which for stream data
    // also means never crediting the peer for bytes it has handed over — its
    // flow-control window shrinks a little on every connection.
    void set_protocol(std::shared_ptr<Protocol> protocol) {
        m_protocol = protocol;
        if (protocol == nullptr)
            return;

        if (m_tx_keys_pending) {
            m_tx_keys_pending = false;
            protocol->on_tx_keys_ready();
        }
        if (!m_pending_stream_data.empty()) {
            std::vector<PendingStreamData> pending = std::move(m_pending_stream_data);
            m_pending_stream_data.clear();
            for (const PendingStreamData &event : pending) {
                protocol->on_stream_data(event.flags, event.stream_id,
                                         std::span<const std::uint8_t>{event.data.data(), event.data.size()});
            }
        }
        if (m_closed)
            protocol->on_connection_closed();
        poke();
    }

    // Wake the send loop: the engine has data to write.
    void flush() noexcept { poke(); }

    // A timestamp ngtcp2 understands. ngtcp2 measures in nanoseconds from an
    // arbitrary epoch, and every call that takes one must use the same clock —
    // mixing in a different origin would compute an RTT of decades.
    [[nodiscard]] ngtcp2_tstamp now() const noexcept {
        return ngtcp2_tstamp(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    // How many unidirectional streams we may still open. The HTTP/3 engine
    // needs three (control + the two QPACK streams) and asks before trying.
    [[nodiscard]] std::uint64_t streams_uni_left() const noexcept {
        return m_conn != nullptr ? ngtcp2_conn_get_streams_uni_left2(m_conn) : 0;
    }

    // Our advertised `initial_max_streams_bidi`, which nghttp3 needs to know so
    // its own accounting matches our transport parameters.
    [[nodiscard]] std::uint64_t local_max_streams_bidi() const noexcept {
        if (m_conn == nullptr)
            return 0;
        const auto *params = ngtcp2_conn_get_local_transport_params2(m_conn);
        return params != nullptr ? params->initial_max_streams_bidi : 0;
    }

    // Wait until the connection has finished. The engine's `run()` returns when
    // this does.
    asio::awaitable<void> await_closed();

    // Open a unidirectional stream. Returns nullopt when the peer's limit is
    // reached — which for the three HTTP/3 critical streams is fatal, not a
    // retry.
    [[nodiscard]] std::optional<std::int64_t> open_uni_stream();

    // Credit the peer for `count` bytes the application consumed. Both halves
    // matter: the stream window and the connection window.
    void extend_stream_offset(std::int64_t stream_id, std::uint64_t count) noexcept;
    void extend_connection_offset(std::uint64_t count) noexcept;

    // Tell ngtcp2 we will not read this stream any more (the peer reset it, or
    // sent STOP_SENDING).
    void shutdown_stream_read(std::int64_t stream_id, std::uint64_t app_error_code) noexcept;

    // Give the peer credit for a request stream that has finished, so it may
    // open another.
    //
    // ngtcp2 raises MAX_STREAMS by itself only for streams it never announced
    // through `stream_open`; for every other one the application has to say when
    // it is done. A server that never says it lets a client open exactly
    // `initial_max_streams_bidi` requests on a connection and then stall — which
    // looks like a working server that dies on the hundredth request.
    void extend_max_streams_bidi(std::uint64_t count = 1) noexcept;

    // Cancel the response half: RESET_STREAM with this application code.
    void reset_stream(std::int64_t stream_id, std::uint64_t app_error_code) noexcept;

    // The same, but only the send half — what nghttp3 asks for when it decides a
    // stream must be reset rather than when the application does.
    void shutdown_stream_write(std::int64_t stream_id, std::uint64_t app_error_code) noexcept;

    // Application-level close with an HTTP/3 error code.
    void close(std::uint64_t app_error_code, std::string_view reason) noexcept;

    // The same, for a QUIC transport error ngtcp2 raised.
    void shutdown(std::uint64_t app_error_code) noexcept { close(app_error_code, {}); }

    // ngtcp2's connection, for the crypto helper's `get_conn`.
    [[nodiscard]] ngtcp2_conn *native_conn() noexcept override { return m_conn; }

  private:
    // --- ngtcp2 callbacks ----------------------------------------------------
    //
    // Every one of these is a static trampoline that recovers the connection
    // from `user_data` and forwards. They return int per ngtcp2's ABI: anything
    // non-zero is turned into NGTCP2_ERR_CALLBACK_FAILURE, and 0 means
    // "continue". They must not throw — a C library has no way to see a C++
    // exception. `noexcept` here is load-bearing, not decoration.

    static QuicConnection *from(void *user_data) noexcept { return static_cast<QuicConnection *>(user_data); }

    static int cb_recv_stream_data(ngtcp2_conn *, std::uint32_t flags, std::int64_t stream_id, std::uint64_t,
                                   const std::uint8_t *data, std::size_t datalen, void *user_data, void *) noexcept;
    static int cb_acked_stream_data_offset(ngtcp2_conn *, std::int64_t stream_id, std::uint64_t, std::uint64_t datalen,
                                           void *user_data, void *) noexcept;
    static int cb_stream_close(ngtcp2_conn *, std::uint32_t flags, std::int64_t stream_id, std::uint64_t app_error_code,
                               void *user_data, void *) noexcept;
    static int cb_stream_reset(ngtcp2_conn *, std::int64_t stream_id, std::uint64_t final_size,
                               std::uint64_t app_error_code, void *user_data, void *) noexcept;
    static int cb_stream_stop_sending(ngtcp2_conn *, std::int64_t stream_id, std::uint64_t app_error_code,
                                      void *user_data, void *) noexcept;
    static int cb_extend_max_stream_data(ngtcp2_conn *, std::int64_t stream_id, std::uint64_t max_data, void *user_data,
                                         void *) noexcept;
    static int cb_extend_max_remote_streams_bidi(ngtcp2_conn *, std::uint64_t max_streams, void *user_data) noexcept;
    static int cb_recv_tx_key(ngtcp2_conn *, ngtcp2_encryption_level level, void *user_data) noexcept;
    static int cb_get_new_connection_id(ngtcp2_conn *, ngtcp2_cid *cid, ngtcp2_stateless_reset_token *token,
                                        std::size_t cidlen, void *user_data) noexcept;
    static int cb_remove_connection_id(ngtcp2_conn *, const ngtcp2_cid *cid, void *user_data) noexcept;
    static void cb_rand(std::uint8_t *dest, std::size_t destlen, const ngtcp2_rand_ctx *) noexcept;
    static int cb_handshake_completed(ngtcp2_conn *, void *user_data) noexcept;
    static ngtcp2_ssize cb_write_pkt(ngtcp2_conn *, ngtcp2_path *path, ngtcp2_pkt_info *pi, std::uint8_t *dest,
                                     std::size_t destlen, ngtcp2_tstamp ts, void *user_data) noexcept;

    // The endpoint needs to hear about CIDs ngtcp2 hands out mid-connection, so
    // that datagrams addressed to a new CID still find this connection.
    using CidCallback = std::function<void(std::span<const std::uint8_t>, std::span<const std::uint8_t>)>;
    using CidRetireCallback = std::function<void(std::span<const std::uint8_t>)>;

  public:
    void set_cid_callbacks(CidCallback on_new, CidRetireCallback on_retire) {
        m_on_new_cid = std::move(on_new);
        m_on_retire_cid = std::move(on_retire);
    }

  private:
    // --- internals ----------------------------------------------------------

    void poke() noexcept;

    // The engine, if it is still there. Every use goes through this — see the
    // note on `m_protocol`.
    [[nodiscard]] std::shared_ptr<Protocol> protocol() const noexcept { return m_protocol.lock(); }

    // Write everything ngtcp2 has to send. Returns false if the connection must
    // be torn down (and has been).
    [[nodiscard]] bool flush_writes();
    // Produce one packet's worth of stream data for ngtcp2. Called from inside
    // ngtcp2's write loop, so: no allocation, no throw, no re-entry.
    ngtcp2_ssize write_pkt(ngtcp2_path *path, ngtcp2_pkt_info *pi, std::uint8_t *dest, std::size_t destlen,
                           ngtcp2_tstamp ts) noexcept;
    // ngtcp2's timer expired — or, since any wake-up runs this, might have.
    void handle_expiry_due();
    // Emit the terminal packet and mark the connection finished.
    void close_now();
    // Turn a fatal ngtcp2 error into a close.
    void fail(int liberr);
    // Park until there is work or ngtcp2's timer expires.
    asio::awaitable<void> wait_for_event();

    void arm_timer();

    Executor m_executor;
    Timer m_timer;
    Channel m_wake;
    // Latched once when the connection finishes. Capacity 1 means a close that
    // happens before anyone waits is still observed by the waiter — which is
    // the common case, since the engine's `run()` usually outlives the closes.
    Channel m_closed_signal;

    QuicConnectionConfig m_config;
    QuicBootstrap m_bootstrap;
    std::vector<std::uint32_t> m_versions;
    asio::ip::udp::endpoint m_local;
    asio::ip::udp::endpoint m_remote;
    DatagramSink m_sink;

    QuicCrypto m_crypto;
    ngtcp2_conn *m_conn{nullptr};
    ngtcp2_ccerr m_last_error{};
    ngtcp2_cid m_scid{};

    // Held *weakly*, and that is load-bearing. The engine owns this connection
    // (it holds a shared_ptr to it), so a strong reference here would close the
    // loop: neither object's count ever reaches zero and the connection — with
    // its ngtcp2 connection, its nghttp3 connection and its SSL — is never
    // destroyed. A sanitizer run over the h3 load found exactly that: 200
    // connections, 200 leaked ngtcp2 connections, 24 MB.
    //
    // The engine outlives the connection because the server's serve callback
    // holds it for as long as `run()` is on the stack, which is the connection's
    // whole life. `lock()` is what makes each use safe: it keeps the engine
    // alive for the duration of the call even if the connection is being torn
    // down inside it.
    std::weak_ptr<Protocol> m_protocol;
    // See set_protocol(): work that arrived before there was anyone to hand it
    // to.
    std::vector<PendingStreamData> m_pending_stream_data;
    std::size_t m_pending_stream_bytes{0};
    bool m_tx_keys_pending{false};
    CidCallback m_on_new_cid;
    CidRetireCallback m_on_retire_cid;

    // Scratch for the scatter list handed to ngtcp2. A member rather than a
    // local because the bytes have to survive until the packet is written, and
    // because `write_pkt` runs inside ngtcp2's frame.
    std::array<ngtcp2_vec, 8> m_vec_scratch{};

    bool m_closed{false};
    bool m_init_failed{false};
    bool m_close_sent{false};
    // Whether `m_last_error` carries a reason. ngtcp2's `ngtcp2_ccerr` has no
    // "unset" state — `ngtcp2_ccerr_default` produces a valid transport error
    // with code 0 — so "did anyone record why" has to be tracked separately, or
    // a close for an unrelated reason would report itself as a clean transport
    // close.
    bool m_has_error{false};
};

// ---------------------------------------------------------------------------
// Definition
// ---------------------------------------------------------------------------

template <typename Executor> bool QuicConnection<Executor>::init() {
    if (m_init_failed)
        return false;

    // Our Source Connection ID: the name the client will use from now on. It is
    // ours to choose and must not be predictable (RFC 9000 §7.3), so it comes
    // from the CSPRNG rather than a counter.
    m_scid.datalen = std::min<std::size_t>(m_config.connection_id_length, NGTCP2_MAX_CIDLEN);
    if (RAND_bytes(m_scid.data, static_cast<int>(m_scid.datalen)) != 1) {
        SIMPLE_HTTP_ERROR_LOG("quic: could not generate a source connection id");
        return false;
    }

    ngtcp2_cid client_scid{};
    ngtcp2_cid_init(&client_scid, m_bootstrap.client_scid.data(), m_bootstrap.client_scid_len);
    ngtcp2_cid original_dcid{};
    ngtcp2_cid_init(&original_dcid, m_bootstrap.original_dcid.data(), m_bootstrap.original_dcid_len);

    // The path is the quadruple ngtcp2 uses to identify where this connection
    // lives. It copies the addresses into the connection, so a local storage
    // object is enough.
    ngtcp2_path_storage ps;
    ngtcp2_path_storage_init(&ps, reinterpret_cast<const ngtcp2_sockaddr *>(m_local.data()),
                             static_cast<ngtcp2_socklen>(m_local.size()),
                             reinterpret_cast<const ngtcp2_sockaddr *>(m_remote.data()),
                             static_cast<ngtcp2_socklen>(m_remote.size()), nullptr);

    auto params = make_ngtcp2_params(m_config, now());
    params.settings.available_versions = m_versions.data();
    params.settings.available_versionslen = m_versions.size();
    // The client's Retry token, if it presented one. ngtcp2 validates it
    // during connection setup and rejects the connection itself when the token
    // does not hold up, which is why the endpoint's only job was to route by
    // CID.
    params.settings.token = m_bootstrap.token.empty() ? nullptr : m_bootstrap.token.data();
    params.settings.tokenlen = m_bootstrap.token.size();
    params.settings.token_type = m_bootstrap.token_type;
    // §7.3: the server must echo the client's original Destination Connection
    // ID, and (after a Retry) the Source Connection ID the Retry carried. The
    // client checks both against what it sent; a mismatch is a forged first
    // flight and it tears the connection down.
    params.transport.original_dcid = original_dcid;
    params.transport.original_dcid_present = 1;
    if (m_bootstrap.retried) {
        params.transport.retry_scid = original_dcid;
        params.transport.retry_scid_present = 1;
    }

    static constexpr ngtcp2_callbacks callbacks = [] {
        ngtcp2_callbacks cb{};
        // The whole crypto layer is the helper's, not ours: these ten callbacks
        // are where the handshake, packet protection and key updates live.
        cb.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
        cb.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
        cb.encrypt = ngtcp2_crypto_encrypt_cb;
        cb.decrypt = ngtcp2_crypto_decrypt_cb;
        cb.hp_mask = ngtcp2_crypto_hp_mask_cb;
        cb.update_key = ngtcp2_crypto_update_key_cb;
        cb.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
        cb.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
        cb.get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb;
        cb.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
        // Ours: the things only the application can answer.
        cb.recv_stream_data = &QuicConnection::cb_recv_stream_data;
        cb.acked_stream_data_offset = &QuicConnection::cb_acked_stream_data_offset;
        cb.stream_close = &QuicConnection::cb_stream_close;
        cb.stream_reset = &QuicConnection::cb_stream_reset;
        cb.stream_stop_sending = &QuicConnection::cb_stream_stop_sending;
        cb.extend_max_stream_data = &QuicConnection::cb_extend_max_stream_data;
        cb.extend_max_remote_streams_bidi = &QuicConnection::cb_extend_max_remote_streams_bidi;
        cb.recv_tx_key = &QuicConnection::cb_recv_tx_key;
        cb.handshake_completed = &QuicConnection::cb_handshake_completed;
        cb.get_new_connection_id2 = &QuicConnection::cb_get_new_connection_id;
        cb.remove_connection_id = &QuicConnection::cb_remove_connection_id;
        cb.rand = &QuicConnection::cb_rand;
        return cb;
    }();

    const int rv = ngtcp2_conn_server_new(&m_conn, &client_scid, &m_scid, &ps.path, m_bootstrap.version, &callbacks,
                                          &params.settings, &params.transport, ngtcp2_mem_default(), this);
    if (rv != 0) {
        SIMPLE_HTTP_ERROR_LOG("quic: ngtcp2_conn_server_new: {}", ngtcp2_strerror(rv));
        m_conn = nullptr;
        return false;
    }
    // Where the OpenSSL crypto helper keeps this connection's handshake state.
    // ngtcp2 looks it up through the connection on every CRYPTO callback and
    // dereferences the result, so this is not optional.
    ngtcp2_conn_set_tls_native_handle(m_conn, m_crypto.native_handle());

    // ngtcp2 pre-allocates `active_connection_id_limit` connection IDs, and the
    // endpoint has to know all of them up front — a client may switch to any of
    // them on its very next packet, and a datagram addressed to a CID we do not
    // recognise is indistinguishable from one for a connection that no longer
    // exists.
    if (m_on_new_cid) {
        const std::size_t count = ngtcp2_conn_get_scid2(m_conn, nullptr);
        std::vector<ngtcp2_cid> cids(count);
        ngtcp2_conn_get_scid2(m_conn, cids.data());
        for (const ngtcp2_cid &cid : cids) {
            m_on_new_cid(std::span<const std::uint8_t>{cid.data, cid.datalen}, {});
        }
    }
    return true;
}

template <typename Executor> void QuicConnection<Executor>::on_datagram(std::span<const std::uint8_t> datagram) {
    if (m_closed || m_conn == nullptr)
        return;

    ngtcp2_path_storage ps;
    ngtcp2_path_storage_init(&ps, reinterpret_cast<const ngtcp2_sockaddr *>(m_local.data()),
                             static_cast<ngtcp2_socklen>(m_local.size()),
                             reinterpret_cast<const ngtcp2_sockaddr *>(m_remote.data()),
                             static_cast<ngtcp2_socklen>(m_remote.size()), nullptr);
    ngtcp2_pkt_info pi{};

    const int rv = ngtcp2_conn_read_pkt(m_conn, &ps.path, &pi, datagram.data(), datagram.size(), now());
    if (rv != 0) {
        // A datagram that arrives for a connection already closing is expected,
        // not an error: the peer has not seen our close yet.
        if (rv == NGTCP2_ERR_DRAINING) {
            close_now();
            return;
        }
        if (rv == NGTCP2_ERR_CALLBACK_FAILURE && m_has_error) {
            close_now();
            return;
        }
        fail(rv);
        return;
    }
    // Reading a packet can have queued ACKs, opened the peer's flow-control
    // window, or completed the handshake. All of that is "the send loop has
    // something to do".
    poke();
}

template <typename Executor> asio::awaitable<void> QuicConnection<Executor>::run() {
    if (m_conn == nullptr) {
        close_now();
        co_return;
    }
    // The first flight (ServerHello and friends) is queued by the handshake,
    // which the crypto helper already drove when the client's Initial was read.
    if (!flush_writes())
        co_return;

    while (!m_closed) {
        arm_timer();
        co_await wait_for_event();
        if (m_closed)
            break;
        handle_expiry_due();
        if (m_closed)
            break;
        if (!flush_writes())
            break;
    }
    co_return;
}

template <typename Executor> asio::awaitable<void> QuicConnection<Executor>::await_closed() {
    if (m_closed)
        co_return;
    co_await m_closed_signal.async_receive(asio::as_tuple(asio::use_awaitable));
    co_return;
}

template <typename Executor> std::optional<std::int64_t> QuicConnection<Executor>::open_uni_stream() {
    if (m_closed || m_conn == nullptr)
        return std::nullopt;
    std::int64_t stream_id = -1;
    const int rv = ngtcp2_conn_open_uni_stream(m_conn, &stream_id, nullptr);
    if (rv != 0)
        return std::nullopt;
    return stream_id;
}

template <typename Executor>
void QuicConnection<Executor>::extend_stream_offset(std::int64_t stream_id, std::uint64_t count) noexcept {
    if (m_closed || m_conn == nullptr || count == 0)
        return;
    // ngtcp2 queues a MAX_STREAM_DATA (and MAX_DATA) frame rather than sending
    // one immediately, so this cannot fail for want of window and cannot
    // re-enter the write path.
    (void)ngtcp2_conn_extend_max_stream_offset(m_conn, stream_id, count);
    poke();
}

template <typename Executor> void QuicConnection<Executor>::extend_connection_offset(std::uint64_t count) noexcept {
    if (m_closed || m_conn == nullptr || count == 0)
        return;
    ngtcp2_conn_extend_max_offset(m_conn, count);
    poke();
}

template <typename Executor>
void QuicConnection<Executor>::shutdown_stream_read(std::int64_t stream_id, std::uint64_t app_error_code) noexcept {
    if (m_closed || m_conn == nullptr)
        return;
    if (ngtcp2_conn_shutdown_stream_read(m_conn, 0, stream_id, app_error_code) != 0) {
        // The stream is usually already gone — the peer reset it, or the
        // connection is closing. Nothing left to do either way.
        return;
    }
    poke();
}

template <typename Executor>
void QuicConnection<Executor>::reset_stream(std::int64_t stream_id, std::uint64_t app_error_code) noexcept {
    if (m_closed || m_conn == nullptr)
        return;
    if (ngtcp2_conn_shutdown_stream(m_conn, 0, stream_id, app_error_code) != 0)
        return;
    poke();
}

template <typename Executor> void QuicConnection<Executor>::extend_max_streams_bidi(std::uint64_t count) noexcept {
    if (m_closed || m_conn == nullptr || count == 0)
        return;
    ngtcp2_conn_extend_max_streams_bidi(m_conn, count);
    poke();
}

template <typename Executor>
void QuicConnection<Executor>::shutdown_stream_write(std::int64_t stream_id, std::uint64_t app_error_code) noexcept {
    if (m_closed || m_conn == nullptr)
        return;
    if (ngtcp2_conn_shutdown_stream_write(m_conn, 0, stream_id, app_error_code) != 0)
        return;
    poke();
}

template <typename Executor>
void QuicConnection<Executor>::close(std::uint64_t app_error_code, std::string_view reason) noexcept {
    if (m_closed || m_conn == nullptr) {
        close_now();
        return;
    }
    // An application close carries the HTTP/3 error code; ngtcp2 frames it as
    // CONNECTION_CLOSE with error code 0x1d (RFC 9000 §19.19).
    ngtcp2_ccerr_set_application_error(&m_last_error, app_error_code,
                                       reinterpret_cast<const std::uint8_t *>(reason.data()), reason.size());
    m_has_error = true;
    close_now();
}

// --- the send path ---------------------------------------------------------

template <typename Executor> void QuicConnection<Executor>::poke() noexcept {
    // Coalescing on purpose: a wake that arrives while one is already queued
    // adds no information. `try_send` posts rather than inlining the wake-up,
    // which matters because `poke` is called from ngtcp2 callbacks.
    (void)m_wake.try_send(error_code{});
    // The timer is the other half of the wait. Cancelling one that is not
    // armed is a no-op, so this needs no state.
    (void)m_timer.cancel();
}

template <typename Executor> void QuicConnection<Executor>::arm_timer() {
    if (m_conn == nullptr)
        return;
    // asio allows one outstanding wait per timer; the previous arm is either a
    // no-op cancel (already fired) or a live wait that must go before re-arming.
    (void)m_timer.cancel();
    const ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry2(m_conn);
    if (expiry == kNoExpiry) {
        m_timer.expires_at(Timer::time_point::max());
    } else {
        m_timer.expires_at(Timer::time_point{} + std::chrono::nanoseconds(expiry));
    }
    // The expiry becomes a wake: the loop is waiting on the channel, so posting
    // into it is how the timer is observed at all (see wait_for_event). A
    // cancellation (poke) lands here too with operation_aborted and is dropped —
    // the loop re-derives what to do from the connection's state either way.
    m_timer.async_wait([this](const error_code &ec) {
        if (!ec && !m_closed) {
            (void)m_wake.try_send(error_code{});
        }
    });
}

template <typename Executor> void QuicConnection<Executor>::handle_expiry_due() {
    if (m_conn == nullptr)
        return;
    const ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry2(m_conn);
    // Any wake-up runs this, so the common case is that the timer has not
    // actually expired and there is nothing to do.
    if (expiry == kNoExpiry || now() < expiry)
        return;

    const int rv = ngtcp2_conn_handle_expiry(m_conn, now());
    if (rv == NGTCP2_ERR_IDLE_CLOSE) {
        // The idle timer fired: drop the connection without a terminal packet,
        // which is what the peer will do too — both sides time out.
        close_now();
        return;
    }
    if (rv != 0) {
        fail(rv);
        return;
    }
    // A PTO probe or a retransmission is now queued; the caller's flush writes
    // it.
}

template <typename Executor>
ngtcp2_ssize QuicConnection<Executor>::write_pkt(ngtcp2_path *path, ngtcp2_pkt_info *pi, std::uint8_t *dest,
                                                 std::size_t destlen, ngtcp2_tstamp ts) noexcept {
    // ngtcp2's write loop, as documented: after NGTCP2_ERR_WRITE_MORE the same
    // call must be repeated with the *same* conn/path/pi/dest/destlen/ts, and
    // no other ngtcp2 function may be called in between. The only two
    // exceptions are the close and shutdown calls — which is exactly what the
    // block/stop paths below need. Everything here is therefore either a local
    // computation or one of those few allowed calls.
    for (;;) {
        std::int64_t stream_id = -1;
        int fin = 0;
        std::size_t veccnt = 0;

        std::uint64_t engine_error = 0;
        const std::shared_ptr<Protocol> proto = protocol();
        if (proto != nullptr && ngtcp2_conn_get_max_data_left2(m_conn) > 0) {
            const StreamData sd = proto->next_stream_data();
            engine_error = sd.error;
            if (sd.error == 0 && sd.stream_id >= 0) {
                stream_id = sd.stream_id;
                fin = sd.fin;
                veccnt = std::min(sd.vec.size(), m_vec_scratch.size());
                for (std::size_t i = 0; i < veccnt; ++i) {
                    m_vec_scratch[i].base = const_cast<std::uint8_t *>(sd.vec[i].base);
                    m_vec_scratch[i].len = sd.vec[i].len;
                }
            }
        }
        if (engine_error != 0) {
            // The engine found a connection-level error while producing data
            // (a QPACK failure, say). Ask for a close with its error code.
            ngtcp2_ccerr_set_application_error(&m_last_error, engine_error, nullptr, 0);
            m_has_error = true;
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }

        std::uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE;
        if (fin != 0)
            flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
        if constexpr (kPacketsPerFlush > 1) {
            // Pad 1-RTT ack-eliciting packets to the path MTU: ngtcp2 only
            // aggregates (`write_aggregate_pkt2`) when the first packet in the
            // buffer is full-size, and padding is how a small response becomes
            // full-size. ngtcp2 itself excludes handshake-level packets, so the
            // anti-amplification limit is untouched; ACK-only packets are not
            // ack-eliciting and are likewise left alone.
            flags |= NGTCP2_WRITE_STREAM_FLAG_PADDING;
        }

        ngtcp2_ssize datalen = -1;
        const ngtcp2_ssize nwrite = ngtcp2_conn_writev_stream(m_conn, path, pi, dest, destlen, &datalen, flags,
                                                              stream_id, m_vec_scratch.data(), veccnt, ts);
        if (nwrite < 0) {
            switch (nwrite) {
            case NGTCP2_ERR_STREAM_DATA_BLOCKED:
                // The peer's stream window is full. nghttp3 has to stop
                // offering this stream until MAX_STREAM_DATA arrives.
                proto->on_stream_blocked(stream_id);
                continue;
            case NGTCP2_ERR_STREAM_SHUT_WR:
                // The stream is gone from the sending side — the peer reset
                // it, or we did. nghttp3 has to stop writing to it.
                proto->on_stream_shut_wr(stream_id);
                continue;
            case NGTCP2_ERR_WRITE_MORE:
                // Accepted into a packet that is already being built. The
                // offset has to advance by exactly what ngtcp2 took, or the
                // same bytes are offered again forever.
                proto->on_stream_data_written(stream_id, static_cast<std::size_t>(datalen));
                continue;
            default:
                break;
            }
            ngtcp2_ccerr_set_liberr(&m_last_error, static_cast<int>(nwrite), nullptr, 0);
            m_has_error = true;
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
        if (datalen >= 0 && proto != nullptr) {
            proto->on_stream_data_written(stream_id, static_cast<std::size_t>(datalen));
        }
        return nwrite;
    }
}

template <typename Executor> bool QuicConnection<Executor>::flush_writes() {
    if (m_closed || m_conn == nullptr)
        return false;

    for (;;) {
        // The aggregate buffer: up to kPacketsPerFlush full datagrams, one GSO
        // segment when there is that much to say. The vector is moved into the
        // send queue and ends up at the socket un-copied; allocating it per
        // flush avoids threading a lifetime through (and the allocator's
        // per-thread cache keeps the cost of doing so negligible).
        std::vector<std::uint8_t> buf(static_cast<std::size_t>(kMaxDatagram) * kPacketsPerFlush);
        ngtcp2_path_storage ps;
        ngtcp2_path_storage_zero(&ps);
        ngtcp2_pkt_info pi{};
        std::size_t gso_size = 0;
        const ngtcp2_tstamp ts = now();

        const ngtcp2_ssize n =
            ngtcp2_conn_write_aggregate_pkt2(m_conn, &ps.path, &pi, buf.data(), buf.size(), &gso_size,
                                             &QuicConnection::cb_write_pkt, kPacketsPerFlush, ts);
        if (n < 0) {
            // NGTCP2_ERR_CALLBACK_FAILURE means one of our callbacks already set
            // `m_last_error`; anything else — a TLS alert included — is
            // ngtcp2's own diagnosis, which `fail` knows how to frame.
            fail(static_cast<int>(n));
            return false;
        }
        // Must be called after a writev_stream sequence, per ngtcp2's
        // documentation — it is what paces packet transmission.
        ngtcp2_conn_update_pkt_tx_time(m_conn, ts);

        if (n == 0) {
            return true;
        }

        SendItem item;
        item.to = m_remote;
        item.gso_size = static_cast<std::uint16_t>(gso_size);
        buf.resize(static_cast<std::size_t>(n));
        item.data = std::move(buf);
        m_sink(std::move(item));
    }
}

template <typename Executor> void QuicConnection<Executor>::close_now() {
    if (m_closed)
        return;
    m_closed = true;

    if (m_conn != nullptr && !m_close_sent) {
        m_close_sent = true;
        std::array<std::uint8_t, kMaxDatagram> buf{};
        ngtcp2_path_storage ps;
        ngtcp2_path_storage_zero(&ps);
        ngtcp2_pkt_info pi{};
        const ngtcp2_ssize n =
            ngtcp2_conn_write_connection_close(m_conn, &ps.path, &pi, buf.data(), buf.size(), &m_last_error, now());
        if (n > 0) {
            // The terminal packet is built into a stack buffer; the sink wants
            // ownership, so hand it a copy — this is once per connection, not
            // per datagram, and not worth threading a buffer through.
            SendItem item;
            item.data.assign(buf.data(), buf.data() + n);
            item.to = m_remote;
            m_sink(std::move(item));
        }
    }

    // Release the engine and anyone waiting: nothing will feed them again.
    if (auto proto = protocol())
        proto->on_connection_closed();
    (void)m_closed_signal.try_send(error_code{});
    (void)m_wake.try_send(error_code{});
    // Drop the arming handler so it cannot fire into the loop afterwards.
    (void)m_timer.cancel();
}

template <typename Executor> void QuicConnection<Executor>::fail(int liberr) {
    if (m_closed)
        return;
    // A callback may already have recorded something more specific than
    // `liberr` says — an HTTP/3 or QPACK error code, say. That one wins.
    if (!m_has_error) {
        if (liberr == NGTCP2_ERR_CRYPTO) {
            // RFC 9001 §4.8: a TLS alert is reported as CRYPTO_ERROR, whose
            // value *is* the alert description. Collapsing it into the generic
            // transport error ngtcp2 wraps around it would tell the peer
            // "protocol violation" when it needs to hear, for instance,
            // "no_application_protocol".
            ngtcp2_ccerr_set_tls_alert(&m_last_error, ngtcp2_conn_get_tls_alert2(m_conn), nullptr, 0);
        } else {
            ngtcp2_ccerr_set_liberr(&m_last_error, liberr, nullptr, 0);
        }
        m_has_error = true;
    }
    close_now();
}

template <typename Executor> asio::awaitable<void> QuicConnection<Executor>::wait_for_event() {
    // Not a race between the timer and the channel any more: the timer's
    // completion handler *posts into this channel* (arm_timer), so a single
    // receive observes both a wake and a real expiry. That removes asio's
    // parallel-group machinery — shared state and a heap allocation per wait —
    // from the hot loop, where a sustained-load profile showed it.
    co_await m_wake.async_receive(asio::as_tuple(asio::use_awaitable));
    co_return;
}

// --- ngtcp2 callbacks ------------------------------------------------------
//
// All of these recover the connection and forward. They run inside ngtcp2, so
// none may throw, block, or call back into ngtcp2 — the last is why the
// forwarders below only touch the engine's own state and post wake-ups.

template <typename Executor>
int QuicConnection<Executor>::cb_recv_stream_data(ngtcp2_conn *, std::uint32_t flags, std::int64_t stream_id,
                                                  std::uint64_t, const std::uint8_t *data, std::size_t datalen,
                                                  void *user_data, void *) noexcept {
    auto *self = from(user_data);
    // ngtcp2's flags are not the engine's vocabulary; the one bit that crosses
    // the seam is translated here so `engine/h3/` needs no ngtcp2 header.
    const std::uint32_t translated = (flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0 ? quic::kStreamDataFin : 0u;
    if (auto proto = self->protocol()) {
        proto->on_stream_data(translated, stream_id, std::span<const std::uint8_t>{data, datalen});
        return 0;
    }
    // No engine yet — see set_protocol(). Hold the event and let the engine
    // credit the peer when it replays.
    self->m_pending_stream_bytes += datalen;
    if (self->m_pending_stream_bytes > kMaxPendingStreamBytes) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    self->m_pending_stream_data.push_back(
        PendingStreamData{translated, stream_id, std::vector<std::uint8_t>(data, data + datalen)});
    return 0;
}

template <typename Executor>
int QuicConnection<Executor>::cb_acked_stream_data_offset(ngtcp2_conn *, std::int64_t stream_id, std::uint64_t,
                                                          std::uint64_t datalen, void *user_data, void *) noexcept {
    auto *self = from(user_data);
    if (auto proto = self->protocol())
        proto->on_acked_stream_data(stream_id, datalen);
    return 0;
}

template <typename Executor>
int QuicConnection<Executor>::cb_stream_close(ngtcp2_conn *conn, std::uint32_t flags, std::int64_t stream_id,
                                              std::uint64_t app_error_code, void *user_data, void *) noexcept {
    auto *self = from(user_data);
    // Hand the stream slot back. `ngtcp2_is_bidi_stream` and bit 0 together
    // spell "the client opened this one" — this is a server, so streams we
    // initiate are the other parity, and crediting those would be crediting
    // ourselves.
    if (ngtcp2_is_bidi_stream(stream_id) != 0 && (stream_id & 0x1) == 0) {
        ngtcp2_conn_extend_max_streams_bidi(conn, 1);
    }
    if (auto proto = self->protocol()) {
        // ngtcp2 1.24 reports a single application error code, and only when it
        // is meaningful; the flag says which.
        const bool has_code = (flags & NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET) != 0;
        proto->on_stream_close(stream_id, has_code ? std::optional<std::uint64_t>{app_error_code} : std::nullopt,
                               std::nullopt);
    }
    return 0;
}

template <typename Executor>
int QuicConnection<Executor>::cb_stream_reset(ngtcp2_conn *, std::int64_t stream_id, std::uint64_t,
                                              std::uint64_t app_error_code, void *user_data, void *) noexcept {
    auto *self = from(user_data);
    if (auto proto = self->protocol())
        proto->on_stream_reset(stream_id, app_error_code);
    return 0;
}

template <typename Executor>
int QuicConnection<Executor>::cb_stream_stop_sending(ngtcp2_conn *, std::int64_t stream_id,
                                                     std::uint64_t app_error_code, void *user_data, void *) noexcept {
    auto *self = from(user_data);
    if (auto proto = self->protocol())
        proto->on_stream_stop_sending(stream_id, app_error_code);
    return 0;
}

template <typename Executor>
int QuicConnection<Executor>::cb_extend_max_stream_data(ngtcp2_conn *, std::int64_t stream_id, std::uint64_t max_data,
                                                        void *user_data, void *) noexcept {
    auto *self = from(user_data);
    if (auto proto = self->protocol())
        proto->on_extend_max_stream_data(stream_id, max_data);
    self->poke();
    return 0;
}

template <typename Executor>
int QuicConnection<Executor>::cb_extend_max_remote_streams_bidi(ngtcp2_conn *, std::uint64_t max_streams,
                                                                void *user_data) noexcept {
    auto *self = from(user_data);
    if (auto proto = self->protocol())
        proto->on_extend_max_remote_streams_bidi(max_streams);
    return 0;
}

template <typename Executor>
int QuicConnection<Executor>::cb_recv_tx_key(ngtcp2_conn *, ngtcp2_encryption_level level, void *user_data) noexcept {
    // Only the application (1-RTT) keys matter: the HTTP/3 control and QPACK
    // streams cannot be opened before them, and opening them earlier would be
    // sending application data at the wrong encryption level.
    if (level != NGTCP2_ENCRYPTION_LEVEL_1RTT)
        return 0;
    auto *self = from(user_data);
    if (auto proto = self->protocol()) {
        proto->on_tx_keys_ready();
    } else {
        self->m_tx_keys_pending = true;
    }
    self->poke();
    return 0;
}

template <typename Executor>
int QuicConnection<Executor>::cb_handshake_completed(ngtcp2_conn *, void *user_data) noexcept {
    from(user_data)->poke();
    return 0;
}

template <typename Executor>
int QuicConnection<Executor>::cb_get_new_connection_id(ngtcp2_conn *, ngtcp2_cid *cid,
                                                       ngtcp2_stateless_reset_token *token, std::size_t cidlen,
                                                       void *user_data) noexcept {
    auto *self = from(user_data);
    // ngtcp2 asks for a fresh connection ID; we mint the bytes and tell the
    // endpoint so that a datagram addressed to it still finds this connection.
    if (RAND_bytes(cid->data, static_cast<int>(cidlen)) != 1)
        return NGTCP2_ERR_CALLBACK_FAILURE;
    cid->datalen = cidlen;
    if (RAND_bytes(token->data, sizeof(token->data)) != 1)
        return NGTCP2_ERR_CALLBACK_FAILURE;
    if (self->m_on_new_cid) {
        self->m_on_new_cid(std::span<const std::uint8_t>{cid->data, cid->datalen},
                           std::span<const std::uint8_t>{token->data, sizeof(token->data)});
    }
    return 0;
}

template <typename Executor>
int QuicConnection<Executor>::cb_remove_connection_id(ngtcp2_conn *, const ngtcp2_cid *cid, void *user_data) noexcept {
    auto *self = from(user_data);
    if (self->m_on_retire_cid) {
        self->m_on_retire_cid(std::span<const std::uint8_t>{cid->data, cid->datalen});
    }
    return 0;
}

template <typename Executor>
void QuicConnection<Executor>::cb_rand(std::uint8_t *dest, std::size_t destlen, const ngtcp2_rand_ctx *) noexcept {
    // ngtcp2 uses this for the random it needs in its own protocol machinery.
    // Failure here is unrecoverable — there is no return value to report it
    // with — so the process is in no state to continue if the CSPRNG is gone.
    if (RAND_bytes(dest, static_cast<int>(destlen)) != 1) {
        std::abort();
    }
}

template <typename Executor>
ngtcp2_ssize QuicConnection<Executor>::cb_write_pkt(ngtcp2_conn *, ngtcp2_path *path, ngtcp2_pkt_info *pi,
                                                    std::uint8_t *dest, std::size_t destlen, ngtcp2_tstamp ts,
                                                    void *user_data) noexcept {
    return from(user_data)->write_pkt(path, pi, dest, destlen, ts);
}

} // namespace simple_http::quic
