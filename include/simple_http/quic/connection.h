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

#include "../core/asio_compat.h"
#include "../core/compat.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#if defined(__linux__)
// The GSO knobs (SOL_UDP, UDP_SEGMENT) come from the libc <netinet/udp.h>,
// which exported UDP_SEGMENT since glibc 2.36. It has to be pulled in before
// the `kPacketsPerFlush` probe below evaluates; on older glibc the macro is
// absent and batching quietly stays off, which is fine. (The kernel's own
// <linux/udp.h> is deliberately *not* included: it declares `struct udphdr`,
// which the libc header also declares, and the redefinition is a compile
// error.)
#include <netinet/udp.h>
#include <sys/socket.h> // sendmsg, cmsghdr, CMSG_LEN
#include <sys/uio.h>    // iovec
#endif

#include "../core/logging.h"
#include "../core/span.h"
#include "../core/types.h"
#include "../transport/transport.h" // SslHandle
#include "ngtcp2_config.h"
#include "ngtcp2_crypto.h"
#include "protocol.h"
#include "tls.h"

namespace simple_http::quic {

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

// Hand one datagram to the kernel. With `gso_size` set, `data` is a GSO
// segment — several QUIC packets the kernel splits into separate datagrams
// (UDP_SEGMENT) — so a whole send burst costs one syscall instead of one per
// packet. Shared by the server endpoint (which queues sends) and the HTTP/3
// client (which sends inline from its sink).
inline int raw_send_datagram(int fd, const SendItem &item) {
    if (item.to.size() > sizeof(sockaddr_storage)) {
        errno = EINVAL;
        return -1;
    }
    sockaddr_storage ss{};
    std::memcpy(&ss, item.to.data(), item.to.size());
#if defined(__linux__) && defined(SOL_UDP) && defined(UDP_SEGMENT)
    iovec iov{const_cast<std::uint8_t *>(item.data.data()), item.data.size()};
    msghdr mh{};
    mh.msg_name = &ss;
    mh.msg_namelen = item.to.size();
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    // The UDP_SEGMENT control message, as a POD with the same layout as
    // `struct cmsghdr` (len/level/type) plus the segment size. It cannot embed
    // a real cmsghdr — glibc ends that with a flexible array member, so it can
    // never be the first member of a wrapper.
    //
    // Declared at function scope, not inside the `if`: msg_control points at it
    // and sendmsg below reads it after the `if` ends, so its lifetime must
    // cover the call. Scoping it inside the `if` was a genuine use-after-scope
    // — ASan flagged it outright, and at -O2 it read stale stack bytes
    // (EINVAL) because the compiler is free to reuse the dead slot. The `asm`
    // barrier is belt-and-braces against the same class of -O2 store elision
    // (GCC 16 trunk) and costs nothing.
    struct cmsg_gso {
        std::size_t cmsg_len; // cmsghdr: 8-byte len, 4-byte level, 4-byte type
        int cmsg_level;
        int cmsg_type;
        std::uint16_t seg; // the GSO segment size
    } c{};
    if (item.gso_size > 0) {
        c.cmsg_len = CMSG_LEN(sizeof(std::uint16_t));
        c.cmsg_level = SOL_UDP;
        c.cmsg_type = UDP_SEGMENT;
        c.seg = item.gso_size;
        static_assert(sizeof(c) >= sizeof(cmsghdr) + sizeof(std::uint16_t));
        mh.msg_control = &c;
        mh.msg_controllen = sizeof(c);
    }
    asm volatile("" : : "r"(&c) : "memory");
    return static_cast<int>(::sendmsg(fd, &mh, 0));
#else
    (void)item.gso_size;
    return static_cast<int>(
        ::sendto(fd, item.data.data(), item.data.size(), 0, reinterpret_cast<const sockaddr *>(&ss), item.to.size()));
#endif
}

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
    // The Retry packet's Source Connection ID (= the second Initial's DCID).
    // Distinct from original_dcid, which is the *first* Initial's DCID carried
    // inside the token; §7.3 requires echoing both, each in its own parameter.
    std::array<std::uint8_t, NGTCP2_MAX_CIDLEN> retry_scid{};
    std::size_t retry_scid_len{0};
    // The version the client chose (already validated against our list).
    std::uint32_t version{0};
    // The Retry token the client echoed back, if any, and ngtcp2's
    // classification of it. Copied rather than referenced: ngtcp2 keeps reading
    // the token while the connection is being set up, and the datagram it
    // arrived in is gone by then.
    std::vector<std::uint8_t> token{};
    ngtcp2_token_type token_type{NGTCP2_TOKEN_TYPE_UNKNOWN};
};

// QUIC version 1 (RFC 9000). The only one this library speaks. Lives here
// rather than in endpoint.h because the client connection has no endpoint and
// still needs a version to offer.
inline constexpr std::uint32_t kQuicVersion1 = 0x00000001;

// The largest datagram we will produce. QUIC's minimum MTU is 1200; 1500 covers
// a stock Ethernet path without fragmentation, and ngtcp2 will not exceed the
// peer's advertised limit or our own `max_udp_payload_size`.
inline constexpr std::size_t kMaxDatagram = 1500;

// The receive batch. recvmmsg gathers up to this many datagrams per syscall,
// which is what keeps the receive side from being one event-loop wake-up per
// packet; the scratch it reads into is `kRecvBatch * kMaxDatagram` bytes per
// endpoint/connection. Shared by the server endpoint and the HTTP/3 client.
#if defined(__linux__)
inline constexpr std::size_t kRecvBatch = 64;
inline constexpr std::size_t kRecvScratch = kRecvBatch * kMaxDatagram;
#endif

// How many packets the send loop packs into one UDP datagram. Each packet in a
// batch is padded to the path MTU (see the PADDING flag in `write_pkt`), which
// is what makes ngtcp2's packet aggregation engage — `write_aggregate_pkt2`
// only batches when the first packet is full. Both the server endpoint and the
// HTTP/3 client send the batch with UDP_SEGMENT, so every packet still lands as
// its own datagram on the wire, for the price of one syscall instead of one per
// packet.
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

// How a client-side connection dials its peer. The server's counterpart is
// QuicBootstrap: the two exist because the roles differ in what they must
// remember, not because the transport logic differs.
struct QuicClientOptions {
    // The QUIC version to offer. Version negotiation is ngtcp2's business; this
    // library only speaks v1.
    std::uint32_t version{kQuicVersion1};
    // Runs on the per-connection SSL before the handshake starts: SNI, host-name
    // verification, ALPN. The QUIC layer deliberately has no TLS-client policy
    // of its own — client/tls_client.h owns that, and its hook is shared with
    // the TCP path.
    std::function<error_code(SSL *)> configure_ssl;
    // TLS session resumption. `session_ticket` is installed on the fresh SSL
    // before the handshake begins; the transport parameters are what ngtcp2
    // needs before any 0-RTT stream may be opened.
    std::shared_ptr<SSL_SESSION> session_ticket;
    std::vector<std::uint8_t> zero_rtt_transport_params;
    bool allow_0rtt{false};
    // OpenSSL hands a NewSessionTicket to the connection after the handshake.
    // The connection pairs it with the peer's 0-RTT transport parameters and
    // invokes this on its own executor, never from inside the OpenSSL callback.
    std::function<void(std::shared_ptr<SSL_SESSION>, std::vector<std::uint8_t>)> on_session_ticket;
};

template <typename Executor>
class QuicConnection : public std::enable_shared_from_this<QuicConnection<Executor>>, public ConnectionCryptoBase {
  public:
    using executor_type = Executor;
    using Timer = asio::steady_timer;
    using Channel = asio::experimental::concurrent_channel<void(error_code)>;

    QuicConnection(Executor exec, SSL_CTX *ssl_ctx, QuicConnectionConfig config, QuicBootstrap bootstrap,
                   std::vector<std::uint32_t> versions, asio::ip::udp::endpoint local, asio::ip::udp::endpoint remote,
                   span<const std::uint8_t> reset_secret, DatagramSink sink)
        : m_executor(exec), m_timer(exec), m_wake(exec, 1), m_closed_signal(exec, 1), m_config(config),
          m_bootstrap(bootstrap), m_versions(std::move(versions)), m_local(std::move(local)),
          m_remote(std::move(remote)), m_sink(std::move(sink)), m_crypto(this) {
        m_reset_secret_len = std::min(reset_secret.size(), m_reset_secret.size());
        std::copy_n(reset_secret.begin(), m_reset_secret_len, m_reset_secret.begin());
        ngtcp2_ccerr_default(&m_last_error);
        m_client = false;
        if (!m_crypto.init(ssl_ctx, /*client=*/false, {}, m_config.enable_0rtt)) {
            m_init_failed = true;
        } else if (SSL *ssl = m_crypto.ssl(); ssl != nullptr) {
            (void)SSL_set_ex_data(ssl, session_owner_ex_index(), this);
        }
    }

    // The client role: dial `remote` from `local`. There is no retry token, no
    // endpoint reset secret and no CID routing table — a client has one socket
    // and one peer, so every datagram on it belongs to this connection.
    QuicConnection(Executor exec, SSL_CTX *ssl_ctx, QuicConnectionConfig config, QuicClientOptions client,
                   asio::ip::udp::endpoint local, asio::ip::udp::endpoint remote, DatagramSink sink)
        : m_executor(exec), m_timer(exec), m_wake(exec, 1), m_closed_signal(exec, 1), m_config(config),
          m_client_options(std::move(client)), m_local(std::move(local)), m_remote(std::move(remote)),
          m_sink(std::move(sink)), m_crypto(this) {
        ngtcp2_ccerr_default(&m_last_error);
        m_client = true;
        // Matches the ngtcp2 ossl reference order: QuicCrypto::init calls
        // SSL_set_connect_state() before this hook, then the hook installs the
        // session/SNI/ALPN. Wrap the caller's TLS hook so SNI/ALPN still run
        // first, then attach the cached ticket and enable early data only when
        // the ticket actually carries an early-data limit.
        auto configure = [this](SSL *ssl) -> error_code {
            if (m_client_options.configure_ssl) {
                if (error_code ec = m_client_options.configure_ssl(ssl)) {
                    return ec;
                }
            }
            if (!m_client_options.session_ticket) {
                return {};
            }
            if (SSL_set_session(ssl, m_client_options.session_ticket.get()) != 1) {
                SIMPLE_HTTP_ERROR_LOG("quic: SSL_set_session failed");
                return make_error_code(asio::error::invalid_argument);
            }
            if (m_client_options.allow_0rtt && !m_client_options.zero_rtt_transport_params.empty() &&
                SSL_SESSION_get_max_early_data(m_client_options.session_ticket.get()) != 0) {
                if (SSL_set_quic_tls_early_data_enabled(ssl, 1) != 1) {
                    SIMPLE_HTTP_ERROR_LOG("quic: SSL_set_quic_tls_early_data_enabled failed");
                    return make_error_code(asio::error::invalid_argument);
                }
                m_early_attempted = true;
            }
            return {};
        };
        if (!m_crypto.init(ssl_ctx, /*client=*/true, configure)) {
            m_init_failed = true;
        } else if (SSL *ssl = m_crypto.ssl(); ssl != nullptr) {
            // Route OpenSSL's new-session callback back to this connection. The
            // callback is installed once on the client SSL_CTX; h1/h2 SSLs that
            // share that context simply have no owner here.
            (void)SSL_set_ex_data(ssl, session_owner_ex_index(), this);
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
        SSL *ssl = m_crypto.ssl();
        return ssl != nullptr ? SslHandle{ssl} : SslHandle{};
    }

    // Whether the QUIC handshake completed (ngtcp2's view, which is what its
    // stream limits are keyed to).
    [[nodiscard]] bool handshake_completed() const noexcept {
        return m_conn != nullptr && ngtcp2_conn_get_handshake_completed2(m_conn) != 0;
    }

    // True once this connection has processed 0-RTT data: on the server when
    // early keys/stream data arrived, on the client when the server accepted
    // the early data it sent.
    [[nodiscard]] bool used_0rtt() const noexcept { return m_used_0rtt; }

    // Client only: an early session was installed and the peer's 0-RTT
    // transport parameters were accepted, so opening a stream now really is
    // early data. The HTTP/3 client gates its early submissions on this.
    [[nodiscard]] bool can_send_0rtt() const noexcept { return m_client && m_early_attempted && m_early_params_set; }

    // ConnectionCryptoBase: OpenSSL produced a NewSessionTicket. Only the
    // client can resume, and the session must be paired with the peer's 0-RTT
    // transport parameters, so it is stashed here and flushed by step() — never
    // from inside the OpenSSL/ngtcp2 callback stack.
    void on_new_session_ticket(SSL_SESSION *session) noexcept override {
        if (session == nullptr || !m_client || m_closed)
            return;
        // Duplicate instead of up-ref: OpenSSL marks the session object handed
        // to the new-session callback not_resumable, and ssl_session_dup()
        // clears that flag. The duplicate owns its own reference.
        SSL_SESSION *copy = SSL_SESSION_dup(session);
        if (copy == nullptr)
            return;
        m_pending_session_ticket = std::shared_ptr<SSL_SESSION>(copy, SSL_SESSION_free);
        m_session_ticket_pending = true;
        poke();
    }

    // ALPN the TLS handshake selected, or empty if none has been negotiated
    // yet. QUIC mandates ALPN (§8.1), so an empty answer after the handshake
    // means the peer refused every protocol we offered.
    [[nodiscard]] std::string_view alpn_selected() const {
        SSL *ssl = m_crypto.ssl();
        if (ssl == nullptr)
            return {};
        const unsigned char *data = nullptr;
        unsigned int len = 0;
        SSL_get0_alpn_selected(ssl, &data, &len);
        if (data == nullptr || len == 0)
            return {};
        return std::string_view{reinterpret_cast<const char *>(data), len};
    }

    // --- endpoint-facing -----------------------------------------------------

    // Create the ngtcp2 connection. Separate from the constructor because it
    // can fail, and because the endpoint must be able to report why.
    [[nodiscard]] bool init();

    // Feed one received datagram in. Runs on this connection's executor.
    void on_datagram(span<const std::uint8_t> datagram, const asio::ip::udp::endpoint &from);

    // Drive the connection until it is closed. Starts the send/expiry loop and
    // returns immediately; `async_await_closed()` observes the end.
    void run();

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

        if (m_0rtt_keys_pending) {
            m_0rtt_keys_pending = false;
            protocol->on_0rtt_keys_ready();
        }
        if (m_early_rejected_pending) {
            m_early_rejected_pending = false;
            protocol->on_early_data_rejected();
        }
        if (m_tx_keys_pending) {
            m_tx_keys_pending = false;
            protocol->on_tx_keys_ready();
        }
        if (!m_pending_stream_data.empty()) {
            std::vector<PendingStreamData> pending = std::move(m_pending_stream_data);
            m_pending_stream_data.clear();
            for (const PendingStreamData &event : pending) {
                protocol->on_stream_data(event.flags, event.stream_id,
                                         span<const std::uint8_t>{event.data.data(), event.data.size()});
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

    // Wait until the connection has finished. Completion signature: void().
    template <typename Token> auto async_await_closed(Token &&token) {
        auto self = this->shared_from_this();
        return asio::async_initiate<Token, void()>(
            [self](auto handler) {
                if (self->m_closed) {
                    std::move(handler)();
                    return;
                }
                self->m_closed_signal.async_receive(
                    [handler = std::move(handler)](error_code) mutable { std::move(handler)(); });
            },
            token);
    }

    // Open a unidirectional stream. Returns nullopt when the peer's limit is
    // reached — which for the three HTTP/3 critical streams is fatal, not a
    // retry.
    [[nodiscard]] std::optional<std::int64_t> open_uni_stream();

    // Open a bidirectional stream — the HTTP/3 client's request stream. Returns
    // nullopt when the peer's `initial_max_streams_bidi` (or a MAX_STREAMS it
    // later raised) is exhausted.
    [[nodiscard]] std::optional<std::int64_t> open_bidi_stream();

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

    // Recorded from the ClientHello callback; see on_datagram's RFC 9001 §8.2
    // handling.
    void on_client_hello_tp(bool present) noexcept override { m_client_hello_had_tp = present; }

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
    static int cb_recv_stop_sending(ngtcp2_conn *, std::int64_t stream_id, std::uint64_t app_error_code,
                                    void *user_data, void *) noexcept;
    static int cb_extend_max_stream_data(ngtcp2_conn *, std::int64_t stream_id, std::uint64_t max_data, void *user_data,
                                         void *) noexcept;
    static int cb_extend_max_remote_streams_bidi(ngtcp2_conn *, std::uint64_t max_streams, void *user_data) noexcept;
    static int cb_recv_tx_key(ngtcp2_conn *, ngtcp2_encryption_level level, void *user_data) noexcept;
    static int cb_recv_rx_key(ngtcp2_conn *, ngtcp2_encryption_level level, void *user_data) noexcept;
    static int cb_tls_early_data_rejected(ngtcp2_conn *, void *user_data) noexcept;
    static int cb_get_new_connection_id(ngtcp2_conn *, ngtcp2_cid *cid, ngtcp2_stateless_reset_token *token,
                                        std::size_t cidlen, void *user_data) noexcept;
    static int cb_remove_connection_id(ngtcp2_conn *, const ngtcp2_cid *cid, void *user_data) noexcept;
    static void cb_rand(std::uint8_t *dest, std::size_t destlen, const ngtcp2_rand_ctx *) noexcept;
    static int cb_handshake_completed(ngtcp2_conn *, void *user_data) noexcept;
    static ngtcp2_ssize cb_write_pkt(ngtcp2_conn *, ngtcp2_path *path, ngtcp2_pkt_info *pi, std::uint8_t *dest,
                                     std::size_t destlen, ngtcp2_tstamp ts, void *user_data) noexcept;

    // The endpoint needs to hear about CIDs ngtcp2 hands out mid-connection, so
    // that datagrams addressed to a new CID still find this connection.
    using CidCallback = std::function<void(span<const std::uint8_t>, span<const std::uint8_t>)>;
    using CidRetireCallback = std::function<void(span<const std::uint8_t>)>;

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
    // Pair a stashed NewSessionTicket with the peer's 0-RTT transport
    // parameters and hand both to the client user. Runs only from `step()`,
    // outside the OpenSSL/ngtcp2 callback stack, because encoding the
    // parameters calls into ngtcp2.
    void flush_pending_session_ticket();
    // Mark the connection finished. If a callback is on the stack the packet
    // itself is deferred to `finish_close()` — see m_in_read_pkt.
    void close_now();
    // Emit the terminal packet and release the engine and its waiters.
    void finish_close();
    // Turn a fatal ngtcp2 error into a close.
    void fail(int liberr);
    // One iteration of the run loop: arm the timer, park on the wake channel,
    // then handle expiry/sends and re-arm.
    void step();

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
    // Set by the ClientHello callback when the peer's ClientHello carries the
    // quic_transport_parameters extension. It is the only way to tell a missing
    // extension (missing_extension alert) from a present parameter set that
    // lacks initial_source_connection_id (TRANSPORT_PARAMETER_ERROR): ngtcp2
    // reports both with NGTCP2_ERR_REQUIRED_TRANSPORT_PARAM and stores neither.
    bool m_client_hello_had_tp{false};
    QuicClientOptions m_client_options;
    std::vector<std::uint32_t> m_versions;
    // Which side of the handshake this connection is. It decides how ngtcp2 is
    // asked to build the connection, which callbacks are installed, and whether
    // the send path may aggregate packets (the client's sink does not do GSO).
    bool m_client{false};
    // Convert the path address ngtcp2 selected back into an asio endpoint.
    static asio::ip::udp::endpoint endpoint_from_addr(const ngtcp2_addr &addr) {
        asio::ip::udp::endpoint out;
        if (addr.addr == nullptr || addr.addrlen == 0)
            return out;
        if (addr.addrlen == sizeof(sockaddr_in)) {
            sockaddr_in sin{};
            std::memcpy(&sin, addr.addr, sizeof sin);
            asio::ip::address_v4::bytes_type bytes{};
            std::memcpy(bytes.data(), &sin.sin_addr.s_addr, bytes.size());
            out.address(asio::ip::address_v4(bytes));
            out.port(ntohs(sin.sin_port));
        } else if (addr.addrlen == sizeof(sockaddr_in6)) {
            sockaddr_in6 sin6{};
            std::memcpy(&sin6, addr.addr, sizeof sin6);
            asio::ip::address_v6::bytes_type bytes{};
            std::memcpy(bytes.data(), sin6.sin6_addr.s6_addr, bytes.size());
            out.address(asio::ip::address_v6(bytes));
            out.port(ntohs(sin6.sin6_port));
        }
        return out;
    }

    asio::ip::udp::endpoint m_local;
    asio::ip::udp::endpoint m_remote;
    DatagramSink m_sink;

    QuicCrypto m_crypto;
    ngtcp2_conn *m_conn{nullptr};
    // True for the duration of `ngtcp2_conn_read_pkt`, which is the only place
    // ngtcp2 forbids building a packet from: its callbacks (the protocol engine)
    // must not call `ngtcp2_conn_write_connection_close`, and an HTTP/3 error
    // found while decoding — a malformed field section, an oversized one — is
    // reported *from* one of those callbacks. Such a close is recorded here and
    // finished by `on_datagram` the moment the parse has returned.
    bool m_in_read_pkt{false};
    bool m_close_deferred{false};
    ngtcp2_ccerr m_last_error{};
    ngtcp2_cid m_scid{};
    // The endpoint's reset secret. The advertised stateless reset token and the
    // endpoint's answers to stale packets must both be derived from it.
    std::array<std::uint8_t, 32> m_reset_secret{};
    std::size_t m_reset_secret_len{0};

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
    // 0-RTT state. `m_0rtt_keys_pending` / `m_early_rejected_pending` replay
    // events that arrived before the engine registered (see set_protocol).
    bool m_0rtt_keys_pending{false};
    bool m_early_rejected_pending{false};
    bool m_used_0rtt{false};
    bool m_early_attempted{false};
    bool m_early_params_set{false};
    bool m_session_ticket_pending{false};
    std::shared_ptr<SSL_SESSION> m_pending_session_ticket;
    CidCallback m_on_new_cid;
    CidRetireCallback m_on_retire_cid;

    // Scratch for the scatter list handed to ngtcp2. A member rather than a
    // local because the bytes have to survive until the packet is written, and
    // because `write_pkt` runs inside ngtcp2's frame.
    std::array<ngtcp2_vec, 8> m_vec_scratch{};

    // Client-only aggregate write buffer. The client sink sends inline, so the
    // bytes never outlive flush_writes() and this can be reused across the
    // many wake-ups that have nothing to send.
    std::vector<std::uint8_t> m_flush_scratch;

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

    if (m_client) {
        // Both IDs are ours to choose and must not be predictable (RFC 9000
        // §7.3). The client's *initial* Destination Connection ID is what the
        // server echoes as original_destination_connection_id; 16 bytes is
        // comfortably above the 8-byte minimum and what the reference client
        // uses.
        ngtcp2_cid dcid{};
        std::array<std::uint8_t, 16> dcid_bytes{};
        const std::size_t scid_len = std::min<std::size_t>(m_config.connection_id_length, NGTCP2_MAX_CIDLEN);
        if (RAND_bytes(dcid_bytes.data(), static_cast<int>(dcid_bytes.size())) != 1 ||
            RAND_bytes(m_scid.data, static_cast<int>(scid_len)) != 1) {
            SIMPLE_HTTP_ERROR_LOG("quic: could not generate a client connection id");
            return false;
        }
        ngtcp2_cid_init(&dcid, dcid_bytes.data(), dcid_bytes.size());
        m_scid.datalen = scid_len;

        ngtcp2_path_storage ps;
        ngtcp2_path_storage_init(&ps, reinterpret_cast<const ngtcp2_sockaddr *>(m_local.data()),
                                 static_cast<ngtcp2_socklen>(m_local.size()),
                                 reinterpret_cast<const ngtcp2_sockaddr *>(m_remote.data()),
                                 static_cast<ngtcp2_socklen>(m_remote.size()), nullptr);

        auto params = make_ngtcp2_params(m_config, now());

        static constexpr ngtcp2_callbacks callbacks = [] {
            ngtcp2_callbacks cb{};
            // The crypto helper drives the handshake both ways; only the two
            // role-specific callbacks differ.
            cb.client_initial = ngtcp2_crypto_client_initial_cb;
            cb.recv_retry = ngtcp2_crypto_recv_retry_cb;
            cb.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
            cb.encrypt = ngtcp2_crypto_encrypt_cb;
            cb.decrypt = ngtcp2_crypto_decrypt_cb;
            cb.hp_mask = ngtcp2_crypto_hp_mask_cb;
            cb.update_key = ngtcp2_crypto_update_key_cb;
            cb.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
            cb.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
            cb.get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb;
            cb.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
            cb.recv_stream_data = &QuicConnection::cb_recv_stream_data;
            cb.acked_stream_data_offset = &QuicConnection::cb_acked_stream_data_offset;
            cb.stream_close = &QuicConnection::cb_stream_close;
            cb.stream_reset = &QuicConnection::cb_stream_reset;
            cb.recv_stop_sending = &QuicConnection::cb_recv_stop_sending;
            cb.extend_max_stream_data = &QuicConnection::cb_extend_max_stream_data;
            cb.extend_max_remote_streams_bidi = &QuicConnection::cb_extend_max_remote_streams_bidi;
            cb.recv_tx_key = &QuicConnection::cb_recv_tx_key;
            cb.recv_rx_key = &QuicConnection::cb_recv_rx_key;
            cb.tls_early_data_rejected = &QuicConnection::cb_tls_early_data_rejected;
            cb.handshake_completed = &QuicConnection::cb_handshake_completed;
            cb.get_new_connection_id2 = &QuicConnection::cb_get_new_connection_id;
            cb.remove_connection_id = &QuicConnection::cb_remove_connection_id;
            cb.rand = &QuicConnection::cb_rand;
            return cb;
        }();

        const int rv = ngtcp2_conn_client_new(&m_conn, &dcid, &m_scid, &ps.path, m_client_options.version, &callbacks,
                                              &params.settings, &params.transport, ngtcp2_mem_default(), this);
        if (rv != 0) {
            SIMPLE_HTTP_ERROR_LOG("quic: ngtcp2_conn_client_new: {}", ngtcp2_strerror(rv));
            m_conn = nullptr;
            return false;
        }
        ngtcp2_conn_set_tls_native_handle(m_conn, m_crypto.native_handle());
        if (m_early_attempted) {
            // ngtcp2 needs the server's remembered transport parameters before
            // it will let us open a 0-RTT stream.
            if (ngtcp2_conn_decode_and_set_0rtt_transport_params(
                    m_conn, m_client_options.zero_rtt_transport_params.data(),
                    m_client_options.zero_rtt_transport_params.size()) != 0) {
                SIMPLE_HTTP_ERROR_LOG("quic: could not restore 0-RTT transport parameters");
                m_early_attempted = false;
            } else {
                m_early_params_set = true;
            }
        }
        return true;
    }

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
    // Advertise the stateless reset token for our Source Connection ID, derived
    // from the endpoint's secret — the same input send_stateless_reset() uses
    // when it answers a stale packet after this connection is gone.
    if (m_reset_secret_len != 0) {
        ngtcp2_cid scid{};
        ngtcp2_cid_init(&scid, m_scid.data, m_scid.datalen);
        if (ngtcp2_crypto_generate_stateless_reset_token(params.transport.stateless_reset_token, m_reset_secret.data(),
                                                         m_reset_secret_len, &scid) == 0) {
            params.transport.stateless_reset_token_present = 1;
        }
    }
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
        ngtcp2_cid retry_scid{};
        ngtcp2_cid_init(&retry_scid, m_bootstrap.retry_scid.data(), m_bootstrap.retry_scid_len);
        params.transport.retry_scid = retry_scid;
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
        cb.recv_stop_sending = &QuicConnection::cb_recv_stop_sending;
        cb.extend_max_stream_data = &QuicConnection::cb_extend_max_stream_data;
        cb.extend_max_remote_streams_bidi = &QuicConnection::cb_extend_max_remote_streams_bidi;
        cb.recv_tx_key = &QuicConnection::cb_recv_tx_key;
        cb.recv_rx_key = &QuicConnection::cb_recv_rx_key;
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
            m_on_new_cid(span<const std::uint8_t>{cid.data, cid.datalen}, {});
        }
    }
    return true;
}

template <typename Executor>
void QuicConnection<Executor>::on_datagram(span<const std::uint8_t> datagram, const asio::ip::udp::endpoint &from) {
    if (m_closed || m_conn == nullptr)
        return;

    // The path the datagram actually arrived on. Passing the source address is
    // what lets ngtcp2 detect a migrated peer (and validate the new path) rather
    // than pinning every packet to the address the connection was born on.
    ngtcp2_path_storage ps;
    ngtcp2_path_storage_init(
        &ps, reinterpret_cast<const ngtcp2_sockaddr *>(m_local.data()), static_cast<ngtcp2_socklen>(m_local.size()),
        reinterpret_cast<const ngtcp2_sockaddr *>(from.data()), static_cast<ngtcp2_socklen>(from.size()), nullptr);
    ngtcp2_pkt_info pi{};

    // A callback that closes the connection can only record it (see
    // close_now); the terminal packet is built here, once ngtcp2 is no longer
    // inside the parse.
    m_in_read_pkt = true;
    const int rv = ngtcp2_conn_read_pkt(m_conn, &ps.path, &pi, datagram.data(), datagram.size(), now());
    m_in_read_pkt = false;
    if (const ngtcp2_path *current = ngtcp2_conn_get_path(m_conn); current != nullptr) {
        // Follow the path ngtcp2 settled on (a validated migration, or the same
        // path as before). peer() and connection-close sends use m_remote.
        auto remote = endpoint_from_addr(current->remote);
        if (remote.port() != 0)
            m_remote = remote;
    }
    if (m_close_deferred) {
        m_close_deferred = false;
        finish_close();
        return;
    }
    // RFC 9001 §8.2: the ClientHello must carry quic_transport_parameters; an
    // endpoint that does not receive it closes the connection with 0x016d
    // (NGTCP2_CRYPTO_ERROR | missing_extension).
    //
    // ngtcp2 reports the omission as NGTCP2_ERR_REQUIRED_TRANSPORT_PARAM, but
    // so it does a present parameter set that lacks
    // initial_source_connection_id — and its generic liberr mapping would turn
    // the missing-extension case into TRANSPORT_PARAMETER_ERROR (0x08). The
    // ClientHello callback recorded whether the extension was actually there,
    // so only a genuinely absent one takes this branch; the incomplete case
    // falls through to fail(rv) and keeps ngtcp2's 0x08.
    if (ngtcp2_conn_is_server2(m_conn) && ngtcp2_conn_get_remote_transport_params2(m_conn) == nullptr &&
        !m_client_hello_had_tp) {
        bool missing = false;
        if (rv == NGTCP2_ERR_REQUIRED_TRANSPORT_PARAM) {
            // ngtcp2 only raises this after the full ClientHello has been
            // consumed, so no TLS-state guard is needed.
            missing = true;
        } else if (rv == 0 && !ngtcp2_conn_get_handshake_completed2(m_conn)) {
            // Defensive path for a TLS backend that does not flag the omission
            // at all. While the ClientHello is still fragmented across CRYPTO
            // frames the TLS state stays at TLS_ST_SR_CLNT_HELLO and the
            // parameters are legitimately absent so far.
            SSL *ssl = m_crypto.ssl();
            const OSSL_HANDSHAKE_STATE state = ssl != nullptr ? SSL_get_state(ssl) : TLS_ST_BEFORE;
            missing = state != TLS_ST_BEFORE && state != TLS_ST_SR_CLNT_HELLO;
        }
        if (missing) {
            SIMPLE_HTTP_ERROR_LOG("quic: ClientHello without quic_transport_parameters; closing");
            ngtcp2_ccerr_set_tls_alert(&m_last_error, SSL_AD_MISSING_EXTENSION, nullptr, 0);
            m_has_error = true;
            close_now();
            return;
        }
    }
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
        if (rv == NGTCP2_ERR_RETRY) {
            // A reordered/duplicate Initial (or a new attempt from the same
            // peer that shares a connection ID) landed on this connection.
            // `ERR_RETRY` is ngtcp2 asking the *application* to send a Retry
            // packet — it is not a fatal error, and the reference server
            // answers with a Retry and keeps the connection attempt alive.
            // Killing the connection here tears down in-flight sessions along
            // with the stray datagram (observed against Xray's h3 client:
            // one extra QUIC connection trips this path and, once failed,
            // drags the working request stream down with it). Drop the
            // datagram and keep the connection.
            return;
        }
        if (rv == NGTCP2_ERR_DROP_CONN) {
            // ngtcp2 asks the endpoint to discard the connection without
            // sending anything (for example an Initial that cannot complete the
            // handshake). RFC 9000 requires a silent drop here; emitting
            // CONNECTION_CLOSE would be an amplification vector. Suppress the
            // terminal packet (`finish_close` checks m_close_sent) but still
            // release the engine and its waiters.
            m_close_sent = true;
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

template <typename Executor> void QuicConnection<Executor>::run() {
    if (m_conn == nullptr) {
        close_now();
        return;
    }
    // The first flight (ServerHello and friends) is queued by the handshake,
    // which the crypto helper already drove when the client's Initial was read.
    if (!flush_writes())
        return;
    step();
}

template <typename Executor> void QuicConnection<Executor>::step() {
    if (m_closed)
        return;
    arm_timer();
    auto self = this->shared_from_this();
    m_wake.async_receive([self](error_code) {
        if (self->m_closed)
            return;
        self->handle_expiry_due();
        if (self->m_closed)
            return;
        self->flush_pending_session_ticket();
        if (!self->flush_writes())
            return;
        self->step();
    });
}

template <typename Executor> void QuicConnection<Executor>::flush_pending_session_ticket() {
    if (!m_session_ticket_pending || m_closed || m_conn == nullptr || !m_client || !handshake_completed())
        return;

    if (!m_client_options.on_session_ticket) {
        m_session_ticket_pending = false;
        m_pending_session_ticket.reset();
        return;
    }

    // The reference client encodes these at handshake completion; ngtcp2 needs
    // them next to the ticket or it will not allow a 0-RTT stream to be opened.
    std::vector<std::uint8_t> params(512);
    for (;;) {
        const ngtcp2_ssize n = ngtcp2_conn_encode_0rtt_transport_params2(m_conn, params.data(), params.size());
        if (n == NGTCP2_ERR_NOBUF) {
            params.resize(params.size() * 2);
            continue;
        }
        if (n < 0) {
            SIMPLE_HTTP_ERROR_LOG("quic: could not encode 0-RTT transport parameters: {}",
                                  ngtcp2_strerror(static_cast<int>(n)));
            m_session_ticket_pending = false;
            m_pending_session_ticket.reset();
            return;
        }
        params.resize(static_cast<std::size_t>(n));
        break;
    }

    m_session_ticket_pending = false;
    auto session = std::move(m_pending_session_ticket);
    m_client_options.on_session_ticket(std::move(session), std::move(params));
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

template <typename Executor> std::optional<std::int64_t> QuicConnection<Executor>::open_bidi_stream() {
    if (m_closed || m_conn == nullptr)
        return std::nullopt;
    std::int64_t stream_id = -1;
    const int rv = ngtcp2_conn_open_bidi_stream(m_conn, &stream_id, nullptr);
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
    //
    // The callback holds a weak_ptr, not `this`: destroying the connection
    // while a wait is pending completes the handler with operation_aborted on
    // the executor — after the connection (and this m_timer) are already gone —
    // so touching `this` there would be a use-after-free. The weak lock is the
    // same discipline the h1 pull providers and Http2ResponseSink use.
    m_timer.async_wait([weak = std::weak_ptr<QuicConnection>{this->shared_from_this()}](const error_code &ec) {
        auto self = weak.lock();
        if (ec || !self || self->m_closed) {
            return;
        }
        (void)self->m_wake.try_send(error_code{});
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
            //
            // Both roles aggregate; the HTTP/3 client's sink now emits the
            // batch with UDP_SEGMENT just like the server endpoint. The extra
            // padding is the price of one syscall per burst instead of one per
            // packet.
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
        // segment when there is that much to say. The server moves the vector
        // into its send queue, so it needs a fresh allocation per non-empty
        // flush. The client's sink sends inline and consumes the bytes before
        // this function returns, so it reuses a per-connection scratch: most
        // step() iterations have nothing to write, and those must not allocate.
        const std::size_t packets_per_flush = kPacketsPerFlush;
        // ngtcp2 is told to use `max_udp_payload_size` as both the MTU and the
        // shaping ceiling (see make_ngtcp2_params). Allocating kMaxDatagram
        // per segment would let it write past the buffer whenever the
        // configured payload is larger than the default datagram size.
        // 65535 is the largest value a UDP length field can represent.
        const auto configured_payload = static_cast<std::size_t>(m_config.max_udp_payload_size);
        const std::size_t max_datagram =
            std::min<std::size_t>(std::max<std::size_t>(kMaxDatagram, configured_payload), 65535);
        const std::size_t needed = max_datagram * packets_per_flush;
        std::vector<std::uint8_t> owned;
        std::uint8_t *buf = nullptr;
        if (m_client) {
            if (m_flush_scratch.size() < needed) {
                m_flush_scratch.resize(needed);
            }
            buf = m_flush_scratch.data();
        } else {
            owned.resize(needed);
            buf = owned.data();
        }
        ngtcp2_path_storage ps;
        ngtcp2_path_storage_zero(&ps);
        ngtcp2_pkt_info pi{};
        std::size_t gso_size = 0;
        const ngtcp2_tstamp ts = now();

        const ngtcp2_ssize n = ngtcp2_conn_write_aggregate_pkt2(m_conn, &ps.path, &pi, buf, needed, &gso_size,
                                                                &QuicConnection::cb_write_pkt, packets_per_flush, ts);
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
        // ngtcp2 fills ps.path with the path this batch is for. During a path
        // validation that is the *new* address, which is exactly where the
        // PATH_CHALLENGE has to go.
        item.to = endpoint_from_addr(ps.path.remote);
        if (item.to.port() == 0)
            item.to = m_remote;
        item.gso_size = static_cast<std::uint16_t>(gso_size);
        if (m_client) {
            // The client sink consumes the bytes inline; copying out of the
            // scratch is one memcpy and keeps the scratch reusable.
            item.data.assign(buf, buf + n);
        } else {
            owned.resize(static_cast<std::size_t>(n));
            item.data = std::move(owned);
        }
        m_sink(std::move(item));
    }
}

template <typename Executor> void QuicConnection<Executor>::close_now() {
    if (m_closed)
        return;
    m_closed = true;

    // `ngtcp2_conn_write_connection_close` "must not be called from inside the
    // callback functions" (ngtcp2.h) — and the engine's callbacks are exactly
    // where a protocol error is discovered, since `nghttp3_conn_read_stream2`
    // reports it by returning to them. Deferring the packet keeps the close
    // ordered with everything else: `on_datagram` finishes it as soon as the
    // parse is off the stack. Nothing observes the difference — `m_closed` is
    // set either way, which is what stops the read loop and the engine.
    if (m_in_read_pkt) {
        m_close_deferred = true;
        return;
    }
    finish_close();
}

template <typename Executor> void QuicConnection<Executor>::finish_close() {
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
            item.to = endpoint_from_addr(ps.path.remote);
            if (item.to.port() == 0)
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
        SIMPLE_HTTP_DEBUG_LOG("quic: connection failed: {} (tls alert={})", ngtcp2_strerror(liberr),
                              m_conn != nullptr ? ngtcp2_conn_get_tls_alert2(m_conn) : 0);
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
    // ngtcp2's flags are not the engine's vocabulary; the bits that cross the
    // seam are translated here so `engine/h3/` needs no ngtcp2 header.
    std::uint32_t translated = (flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0 ? quic::kStreamDataFin : 0u;
    // Connection-level observation only: HTTP/3 derives Request::used_0rtt()
    // from the handshake state when the header section completes.
    if ((flags & NGTCP2_STREAM_DATA_FLAG_0RTT) != 0)
        self->m_used_0rtt = true;
    if (auto proto = self->protocol()) {
        proto->on_stream_data(translated, stream_id, span<const std::uint8_t>{data, datalen});
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
int QuicConnection<Executor>::cb_recv_stop_sending(ngtcp2_conn *, std::int64_t stream_id, std::uint64_t app_error_code,
                                                   void *user_data, void *) noexcept {
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
    auto *self = from(user_data);
    if (level == NGTCP2_ENCRYPTION_LEVEL_0RTT) {
        // The client may now open early streams. The HTTP/3 engine creates
        // nghttp3 at this point; the handshake is still in progress.
        if (auto proto = self->protocol()) {
            proto->on_0rtt_keys_ready();
        } else {
            self->m_0rtt_keys_pending = true;
        }
        self->poke();
        return 0;
    }
    // Only the application (1-RTT) keys matter after this: the HTTP/3 control
    // and QPACK streams cannot be opened before them, and opening them earlier
    // would be sending application data at the wrong encryption level.
    if (level != NGTCP2_ENCRYPTION_LEVEL_1RTT)
        return 0;
    if (auto proto = self->protocol()) {
        proto->on_tx_keys_ready();
    } else {
        self->m_tx_keys_pending = true;
    }
    self->poke();
    return 0;
}

template <typename Executor>
int QuicConnection<Executor>::cb_recv_rx_key(ngtcp2_conn *, ngtcp2_encryption_level level, void *user_data) noexcept {
    // The server installs early read keys here; that is the first moment it
    // can decode a 0-RTT request, even though the handshake is not complete.
    if (level != NGTCP2_ENCRYPTION_LEVEL_0RTT)
        return 0;
    auto *self = from(user_data);
    self->m_used_0rtt = true;
    if (auto proto = self->protocol()) {
        proto->on_0rtt_keys_ready();
    } else {
        self->m_0rtt_keys_pending = true;
    }
    self->poke();
    return 0;
}

template <typename Executor>
int QuicConnection<Executor>::cb_tls_early_data_rejected(ngtcp2_conn *, void *user_data) noexcept {
    auto *self = from(user_data);
    self->m_used_0rtt = false;
    if (auto proto = self->protocol()) {
        proto->on_early_data_rejected();
    } else {
        self->m_early_rejected_pending = true;
    }
    self->poke();
    return 0;
}

template <typename Executor>
int QuicConnection<Executor>::cb_handshake_completed(ngtcp2_conn *conn, void *user_data) noexcept {
    auto *self = from(user_data);
    if (self->m_client && self->m_early_attempted) {
        SSL *ssl = self->m_crypto.ssl();
        const int status = ssl != nullptr ? SSL_get_early_data_status(ssl) : SSL_EARLY_DATA_NOT_SENT;
        if (status == SSL_EARLY_DATA_ACCEPTED) {
            self->m_used_0rtt = true;
        } else {
            // Tell ngtcp2 to discard the early streams and invoke our
            // tls_early_data_rejected callback. This is the same call the
            // ngtcp2 reference client makes from its handshake-completed hook.
            (void)ngtcp2_conn_tls_early_data_rejected(conn);
        }
    }
    // The ticket flush is deferred to `step()` (outside this callback stack);
    // `poke()` below makes sure that step runs even if this callback did not
    // come from a read/write iteration.
    if (auto proto = self->protocol())
        proto->on_handshake_complete();
    self->poke();
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
    // The token must be the value the endpoint can derive from this CID and the
    // shared secret when it later answers a stale packet; a random token here
    // would never match the stateless reset (see send_stateless_reset).
    if (self->m_reset_secret_len == 0 ||
        ngtcp2_crypto_generate_stateless_reset_token(token->data, self->m_reset_secret.data(), self->m_reset_secret_len,
                                                     cid) != 0) {
        if (RAND_bytes(token->data, sizeof(token->data)) != 1)
            return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    if (self->m_on_new_cid) {
        self->m_on_new_cid(span<const std::uint8_t>{cid->data, cid->datalen},
                           span<const std::uint8_t>{token->data, sizeof(token->data)});
    }
    return 0;
}

template <typename Executor>
int QuicConnection<Executor>::cb_remove_connection_id(ngtcp2_conn *, const ngtcp2_cid *cid, void *user_data) noexcept {
    auto *self = from(user_data);
    if (self->m_on_retire_cid) {
        self->m_on_retire_cid(span<const std::uint8_t>{cid->data, cid->datalen});
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
