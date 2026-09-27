#pragma once

// The UDP side: one socket, and every QUIC connection that arrives on it.
//
// QUIC has no accept. A connection is identified by the Connection IDs in its
// packets, so the listener's job is to demultiplex: read a datagram, work out
// which connection it belongs to, and hand it over. That is what makes a QUIC
// "listener" a routing table rather than a queue of sockets.
//
// Three things have to be answered without a connection, because they are
// exactly the cases where there is none (RFC 9000 §5.2, §7.2, §10.3):
//
//   * A version we do not speak — answer with a Version Negotiation packet.
//   * An Initial with no (or a stale) Retry token, when address validation is
//     on — answer with a Retry.
//   * A short-header packet for a connection we have never heard of — answer
//     with a stateless reset, so a peer whose connection died does not retry
//     forever.
//
// All three are forged-source hazards: each answer must be cheap and must not
// allocate state, or the listener becomes a memory amplifier. The stateless
// reset in particular is derived from the packet's own Destination Connection
// ID and a secret, so no table lookup is needed to produce it.
//
// The connection objects are keyed by Connection ID. A connection owns its IDs
// and tells us about new ones (ngtcp2 mints them); every ID it has ever
// advertised must keep routing here, because the client may switch to any of
// them at any time.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <openssl/rand.h>

#include <boost/asio.hpp>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>

#include "../core/logging.h"
#include "../core/types.h"
#include "connection.h"
#include "tls.h"

namespace simple_http::quic {

namespace asio = boost::asio;

// QUIC version 1 (RFC 9000). The only one this server speaks.
inline constexpr std::uint32_t kQuicVersion1 = 0x00000001;

// How long a Retry token stays valid. Bounded because the token is what proves
// the client owned its address at some point, and "some point" has to expire.
inline constexpr ngtcp2_duration kRetryTokenTimeout = 30 * NGTCP2_SECONDS;

// The receive buffer. A QUIC datagram is at most the path MTU (1500 or so), but
// the socket may hand us more if a peer sends a giant datagram; ngtcp2 will
// reject anything past its own limit. 64 KiB matches what the previous
// implementation used and is well past any legitimate packet.
inline constexpr std::size_t kReceiveBufferSize = 64 * 1024;

// A UDP endpoint for QUIC. The fields mirror InetAddress, and that is the whole
// difference: QUIC is another *transport*, not another address family.
struct QuicEndpointConfig {
    QuicConnectionConfig connection{};
    // Ask the client to prove it owns its address before any state is created.
    // Costs one round trip at connection setup and buys resistance to
    // spoofed-source floods.
    bool retry{false};
    // The versions we will speak, advertised in Version Negotiation and offered
    // to ngtcp2.
    std::vector<std::uint32_t> supported_versions{kQuicVersion1};
};

template <typename Executor>
class QuicEndpoint {
  public:
    using Connection = QuicConnection<Executor>;

    // What to do with a connection once it exists. The server supplies this; it
    // is where the HTTP/3 engine is built, which is what keeps this file free of
    // any knowledge of HTTP.
    using ServeFn = std::function<asio::awaitable<void>(std::shared_ptr<Connection>)>;

    QuicEndpoint(Executor exec, SSL_CTX* ssl_ctx, QuicEndpointConfig config, ServeFn serve)
        : m_executor(std::move(exec)), m_socket(m_executor), m_ssl_ctx(ssl_ctx), m_config(std::move(config)),
          m_serve(std::move(serve)) {
        if (RAND_bytes(m_secret.data(), static_cast<int>(m_secret.size())) != 1) {
            SIMPLE_HTTP_ERROR_LOG("quic: could not generate the endpoint secret");
        }
    }

    QuicEndpoint(const QuicEndpoint&) = delete;
    QuicEndpoint& operator=(const QuicEndpoint&) = delete;

    // Bind the socket. Returns false and reports why on failure.
    bool open(const asio::ip::udp::endpoint& endpoint, bool reuse_port, error_code& ec) {
        m_socket.open(endpoint.protocol(), ec);
        if (ec) return false;
        if (reuse_port) {
            m_socket.set_option(asio::socket_base::reuse_address(true), ec);
            if (ec) {
                SIMPLE_HTTP_ERROR_LOG("quic: SO_REUSEADDR: {}", ec.message());
                ec.clear();
            }
#ifdef SO_REUSEPORT
            int one = 1;
            ::setsockopt(m_socket.native_handle(), SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
        }
        // A generous receive buffer: QUIC's own flow control is the real limit,
        // and a small kernel buffer turns a burst into avoidable loss.
        m_socket.set_option(asio::socket_base::receive_buffer_size(8 * 1024 * 1024), ec);
        ec.clear();
        m_socket.bind(endpoint, ec);
        if (ec) return false;
        return true;
    }

    [[nodiscard]] std::uint16_t port() const {
        error_code ec;
        const auto endpoint = m_socket.local_endpoint(ec);
        return ec ? 0 : endpoint.port();
    }

    // Start reading. Returns immediately; the receive loop runs until close().
    void start() {
        if (m_started) return;
        m_started = true;
        asio::co_spawn(m_executor, receive_loop(), asio::detached);
        asio::co_spawn(m_executor, send_loop(), asio::detached);
    }

    // Stop listening. Connections are left alone; use shutdown_connections()
    // first to wind them down.
    void close() {
        if (m_closed) return;
        m_closed = true;
        error_code ec;
        (void)m_socket.close(ec);
        (void)m_send_wake.try_send(error_code{});
        (void)ec;
    }

    // Ask every live connection to close. This is the shutdown path the Server
    // uses: it is not a connection error, so each connection gets a proper
    // application close rather than a silent socket drop.
    void shutdown_connections() {
        // The CID table holds weak references, so each entry has to be promoted
        // — and a connection may legitimately be gone already.
        for (auto& [id, weak] : m_by_cid) {
            if (auto conn = weak.lock()) conn->close(0, "server stopping");
        }
    }

  private:
    using ConnectionPtr = std::shared_ptr<Connection>;

    // --- datagram in -------------------------------------------------------

    asio::awaitable<void> receive_loop() {
        std::vector<std::uint8_t> buffer(kReceiveBufferSize);
        for (;;) {
            asio::ip::udp::endpoint remote;
            auto [ec, n] = co_await m_socket.async_receive_from(asio::buffer(buffer), remote,
                                                               asio::as_tuple(asio::use_awaitable));
            if (ec) {
                if (ec == asio::error::operation_aborted) co_return;
                // A connection-refused on a UDP socket means a previous send
                // drew an ICMP port-unreachable. Not fatal, and there is nothing
                // to reply to.
                continue;
            }
            if (n == 0) continue;
            handle_datagram(std::span<const std::uint8_t>{buffer.data(), n}, remote);
        }
    }

    void handle_datagram(std::span<const std::uint8_t> data, const asio::ip::udp::endpoint& remote) {
        ngtcp2_version_cid vc{};
        const std::size_t scid_len = m_config.connection.connection_id_length;
        switch (const int rv = ngtcp2_pkt_decode_version_cid(&vc, data.data(), data.size(), scid_len)) {
            case 0:
                break;
            case NGTCP2_ERR_VERSION_NEGOTIATION:
                // The client offered a version we do not speak. Tell it what we
                // do speak, and drop the packet: there is no connection yet and
                // creating one for a version we cannot complete would be worse
                // than saying so.
                send_version_negotiation(vc, remote);
                return;
            default:
                return;
        }

        const std::string key = cid_key(vc.dcid, vc.dcidlen);
        auto it = m_by_cid.find(key);
        if (it != m_by_cid.end()) {
            if (auto conn = it->second.lock()) {
                conn->on_datagram(data);
            } else {
                m_by_cid.erase(it);
            }
            return;
        }

        // No connection owns this Destination Connection ID. Either it is a new
        // Initial (which is how connections are born), or it is a stale packet
        // for a connection that is gone.
        ngtcp2_pkt_hd hd{};
        if (ngtcp2_accept(&hd, data.data(), data.size()) != 0) {
            // A packet we cannot accept. If it is a short-header packet long
            // enough to be indistinguishable from a reset, answer with a
            // stateless reset — that is the only way a peer learns its
            // connection is dead (RFC 9000 §10.3).
            if (!(data[0] & 0x80) && data.size() >= NGTCP2_MIN_STATELESS_RESET_RANDLEN) {
                send_stateless_reset(data.size(), vc, remote);
            }
            return;
        }

        // hd is an Initial. Decide whether to validate the client's address.
        std::vector<std::uint8_t> token;
        ngtcp2_token_type token_type = NGTCP2_TOKEN_TYPE_UNKNOWN;
        std::array<std::uint8_t, NGTCP2_MAX_CIDLEN> original_dcid{};
        std::size_t original_dcid_len = 0;
        bool retried = false;

        if (m_config.retry) {
            if (hd.tokenlen == 0) {
                send_retry(hd, vc, remote);
                return;
            }
            ngtcp2_cid odcid{};
            ngtcp2_cid retry_scid{};
            ngtcp2_cid_init(&retry_scid, vc.scid, vc.scidlen);
            const int vrv = ngtcp2_crypto_verify_retry_token2(
                &odcid, hd.token, hd.tokenlen, m_secret.data(), m_secret.size(), hd.version,
                reinterpret_cast<const ngtcp2_sockaddr*>(remote.data()), static_cast<ngtcp2_socklen>(remote.size()),
                &retry_scid, kRetryTokenTimeout, std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                   std::chrono::steady_clock::now().time_since_epoch())
                                                   .count());
            if (vrv != 0) {
                // The token is unreadable, expired, or was issued for a
                // different address. Ask again rather than guessing.
                send_retry(hd, vc, remote);
                return;
            }
            // After a Retry the client's DCID is the Retry's SCID, and the
            // *original* DCID is what the token carried.
            original_dcid_len = std::min<std::size_t>(odcid.datalen, original_dcid.size());
            std::copy(odcid.data, odcid.data + original_dcid_len, original_dcid.begin());
            retried = true;
            token.assign(hd.token, hd.token + hd.tokenlen);
            token_type = NGTCP2_TOKEN_TYPE_RETRY;
        } else {
            // Address validation off: the client's DCID is the original one.
            original_dcid_len = std::min<std::size_t>(vc.dcidlen, original_dcid.size());
            std::copy(vc.dcid, vc.dcid + original_dcid_len, original_dcid.begin());
            if (hd.tokenlen > 0) {
                token.assign(hd.token, hd.token + hd.tokenlen);
                token_type = NGTCP2_TOKEN_TYPE_NEW_TOKEN;
            }
        }

        QuicBootstrap bootstrap;
        bootstrap.client_scid_len = std::min<std::size_t>(hd.scid.datalen, bootstrap.client_scid.size());
        std::copy(hd.scid.data, hd.scid.data + bootstrap.client_scid_len, bootstrap.client_scid.begin());
        bootstrap.original_dcid = original_dcid;
        bootstrap.original_dcid_len = original_dcid_len;
        bootstrap.retried = retried;
        bootstrap.version = hd.version;
        bootstrap.token = std::move(token);
        bootstrap.token_type = token_type;

        auto self = this;
        error_code local_ec;
        auto local = m_socket.local_endpoint(local_ec);
        if (local_ec) local = asio::ip::udp::endpoint{};

        auto conn = std::make_shared<Connection>(m_executor, m_ssl_ctx, m_config.connection, bootstrap,
                                                 m_config.supported_versions, local, remote,
                                                 [self](std::span<const std::uint8_t> datagram,
                                                        const asio::ip::udp::endpoint& to) {
                                                     self->queue_datagram(datagram, to);
                                                 });
        if (conn->init_failed()) {
            SIMPLE_HTTP_ERROR_LOG("quic: could not set up TLS for a new connection");
            return;
        }
        // Every CID ngtcp2 may hand out later has to route here too. Without
        // this a client that migrates to a fresh connection ID becomes
        // unreachable — and its packets look exactly like those of a connection
        // that never existed.
        conn->set_cid_callbacks(
            [self, weak = std::weak_ptr<Connection>(conn)](std::span<const std::uint8_t> cid, std::span<const std::uint8_t>) {
                self->associate_cid(cid, weak);
            },
            [self](std::span<const std::uint8_t> cid) { self->dissociate_cid(cid); });

        if (!conn->init()) {
            SIMPLE_HTTP_ERROR_LOG("quic: ngtcp2 could not create a connection");
            return;
        }
        // The Initial itself still has to be delivered — it carries the
        // ClientHello. Our own Source Connection ID is already in the table:
        // `init()` walks the IDs ngtcp2 pre-allocated and registers each one.
        m_live.push_back(conn);
        conn->on_datagram(data);

        asio::co_spawn(m_executor, serve(conn), asio::detached);
    }

    asio::awaitable<void> serve(ConnectionPtr conn) {
        // The engine's own run() drives the connection; this wrapper exists to
        // keep the connection alive until it is finished and to drop the
        // bookkeeping entry afterwards.
        co_await m_serve(conn);
        co_await conn->await_closed();
        m_live.erase(std::remove(m_live.begin(), m_live.end(), conn), m_live.end());
        co_return;
    }

    // --- CID table ---------------------------------------------------------

    static std::string cid_key(const std::uint8_t* data, std::size_t len) {
        return std::string{reinterpret_cast<const char*>(data), len};
    }

    void associate_cid(std::span<const std::uint8_t> cid, const std::weak_ptr<Connection>& conn) {
        if (cid.empty()) return;
        m_by_cid[cid_key(cid.data(), cid.size())] = conn;
    }

    void dissociate_cid(std::span<const std::uint8_t> cid) {
        if (cid.empty()) return;
        m_by_cid.erase(cid_key(cid.data(), cid.size()));
    }

    // --- stateless answers --------------------------------------------------

    void send_version_negotiation(const ngtcp2_version_cid& vc, const asio::ip::udp::endpoint& remote) {
        std::array<std::uint8_t, kMaxDatagram> buf{};
        // The server must swap the connection IDs in its answer (§6.1): the
        // client checks that its own SCID came back as the DCID, which is what
        // stops an off-path attacker from injecting a downgrade.
        const ngtcp2_ssize n = ngtcp2_pkt_write_version_negotiation(
            buf.data(), buf.size(), static_cast<std::uint8_t>(random_byte()), vc.scid, vc.scidlen, vc.dcid, vc.dcidlen,
            m_config.supported_versions.data(), m_config.supported_versions.size());
        if (n > 0) queue_datagram(std::span<const std::uint8_t>{buf.data(), static_cast<std::size_t>(n)}, remote);
    }

    void send_retry(const ngtcp2_pkt_hd& hd, const ngtcp2_version_cid& vc, const asio::ip::udp::endpoint& remote) {
        std::array<std::uint8_t, kMaxDatagram> buf{};
        std::array<std::uint8_t, NGTCP2_CRYPTO_MAX_RETRY_TOKENLEN2> token{};
        // The Retry's own Source Connection ID, which the client will echo as
        // its DCID. Validating against it is what binds the token to the Retry.
        ngtcp2_cid retry_scid{};
        retry_scid.datalen = std::min<std::size_t>(m_config.connection.connection_id_length, NGTCP2_MAX_CIDLEN);
        if (RAND_bytes(retry_scid.data, static_cast<int>(retry_scid.datalen)) != 1) return;

        ngtcp2_cid odcid{};
        ngtcp2_cid_init(&odcid, vc.dcid, vc.dcidlen);

        const ngtcp2_ssize tokenlen = ngtcp2_crypto_generate_retry_token2(
            token.data(), m_secret.data(), m_secret.size(), hd.version,
            reinterpret_cast<const ngtcp2_sockaddr*>(remote.data()), static_cast<ngtcp2_socklen>(remote.size()),
            &retry_scid, &odcid, std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
        if (tokenlen < 0) return;

        const ngtcp2_ssize n = ngtcp2_crypto_write_retry(buf.data(), buf.size(), hd.version, &hd.scid, &retry_scid, &hd.dcid,
                                                         token.data(), static_cast<std::size_t>(tokenlen));
        if (n > 0) queue_datagram(std::span<const std::uint8_t>{buf.data(), static_cast<std::size_t>(n)}, remote);
    }

    void send_stateless_reset(std::size_t /*packet_len*/, const ngtcp2_version_cid& vc,
                              const asio::ip::udp::endpoint& remote) {
        std::array<std::uint8_t, kMaxDatagram> buf{};
        std::array<std::uint8_t, NGTCP2_STATELESS_RESET_TOKENLEN> token{};
        ngtcp2_cid cid{};
        // Derived from the DCID in the packet and our secret, so any observer of
        // that packet can compute it (RFC 9000 §10.3.1) — which is exactly what
        // makes it unforgeable by an attacker who cannot see the traffic, and
        // verifiable by the peer that can.
        ngtcp2_cid_init(&cid, vc.dcid, vc.dcidlen);
        if (ngtcp2_crypto_generate_stateless_reset_token(token.data(), m_secret.data(), m_secret.size(), &cid) != 0) {
            return;
        }
        std::array<std::uint8_t, NGTCP2_STATELESS_RESET_TOKENLEN> rand{};
        if (RAND_bytes(rand.data(), static_cast<int>(rand.size())) != 1) return;
        const ngtcp2_ssize n = ngtcp2_pkt_write_stateless_reset(buf.data(), buf.size(), token.data(), rand.data(), rand.size());
        if (n > 0) queue_datagram(std::span<const std::uint8_t>{buf.data(), static_cast<std::size_t>(n)}, remote);
    }

    static std::uint8_t random_byte() {
        std::uint8_t b = 0;
        if (RAND_bytes(&b, 1) != 1) return 0;
        return b;
    }

    // --- datagram out -------------------------------------------------------
    //
    // Sends are queued and drained by one coroutine rather than issued inline.
    // The reason is that connections produce datagrams from inside ngtcp2
    // callbacks and from a send loop that must not block; a socket that is
    // briefly unwritable would otherwise either stall a connection or force it
    // to drop a packet it has already committed to retransmitting.

    void queue_datagram(std::span<const std::uint8_t> datagram, const asio::ip::udp::endpoint& to) {
        if (m_closed) return;
        m_send_queue.emplace_back(std::vector<std::uint8_t>(datagram.begin(), datagram.end()), to);
        (void)m_send_wake.try_send(error_code{});
    }

    asio::awaitable<void> send_loop() {
        while (!m_closed) {
            auto [ec] = co_await m_send_wake.async_receive(asio::as_tuple(asio::use_awaitable));
            (void)ec;
            while (!m_send_queue.empty() && !m_closed) {
                auto [payload, to] = std::move(m_send_queue.front());
                auto [send_ec, sent] = co_await m_socket.async_send_to(asio::buffer(payload), to,
                                                                     asio::as_tuple(asio::use_awaitable));
                if (send_ec) {
                    // A datagram that could not be sent is a loss, and QUIC is
                    // built to tolerate loss. Retrying it would be worse:
                    // retransmission belongs to ngtcp2, which knows the packet
                    // number and the deadline.
                    break;
                }
                (void)sent;
                m_send_queue.pop_front();
            }
        }
        co_return;
    }

    Executor m_executor;
    asio::ip::udp::socket m_socket;
    SSL_CTX* m_ssl_ctx;
    QuicEndpointConfig m_config;
    ServeFn m_serve;

    std::array<std::uint8_t, 32> m_secret{};

    std::unordered_map<std::string, std::weak_ptr<Connection>> m_by_cid;
    std::vector<ConnectionPtr> m_live;

    std::deque<std::pair<std::vector<std::uint8_t>, asio::ip::udp::endpoint>> m_send_queue;
    asio::experimental::concurrent_channel<void(error_code)> m_send_wake{m_executor, 128};
    bool m_started{false};
    bool m_closed{false};
};

}  // namespace simple_http::quic
