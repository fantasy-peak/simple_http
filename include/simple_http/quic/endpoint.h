#pragma once

// The QUIC listener: one UDP socket, and the connection table behind it.
//
// A UDP socket has no accept, so "which connection is this?" has to be answered
// from the packet itself — by its destination connection ID. That is the whole
// job of this file:
//
//   * a table from connection ID to connection, keyed by *both* the ID we chose
//     for a connection and the original one the client used, because a client
//     retransmits its Initial under its own ID until our first reply reaches it;
//   * creating a connection when an Initial for an unknown ID turns up, which
//     is the closest thing QUIC has to an accept;
//   * the two replies a listener owes a stranger: a Version Negotiation packet
//     for a version we do not speak, and a stateless reset for a connection ID
//     that used to be ours.
//   * optionally, a Retry — which is what stops a spoofed source address from
//     making the server do the work of a handshake.
//
// The endpoint is pinned to one executor and owns its socket there, so the
// connection table needs no lock. Several sockets on one port (SO_REUSEPORT) are
// several endpoints in several contexts; the kernel's four-tuple hash keeps one
// connection's datagrams together, which is what makes that safe. The cost is
// that a connection cannot migrate *between* sockets — within one, it can.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <boost/asio.hpp>
#include <algorithm>
#include <ranges>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include "../core/logging.h"
#include "../core/types.h"
#include "connection.h"
#include "crypto.h"
#include "frame.h"
#include "packet.h"
#include "tls.h"
#include "transport_params.h"
#include "wire.h"

namespace simple_http::quic {

namespace asio = boost::asio;

// The UDP socket buffer a QUIC listener asks the kernel for. One socket carries
// every connection on the port, so the default is a queue for one flow rather
// than for a server.
inline constexpr std::size_t kSocketBufferBytes = 8u << 20;

// How many recently-closed connection IDs to remember for stateless resets. The
// window only has to outlast a peer's last flight, and each entry is a string.
inline constexpr std::size_t kRecentCidLimit = 64;

struct QuicEndpointConfig {
    QuicConnectionConfig connection{};
    // Ask for a Retry before doing any handshake work. Costs one round trip on
    // every new connection and buys address validation up front: without it a
    // spoofed source address makes the server commit memory and a handshake to
    // a peer that will never receive the answer.
    bool retry{false};
    // The address a Version Negotiation packet advertises. Unused for now
    // because only one version exists here, but the packet still has to be
    // *sent* for an unknown version.
    std::vector<std::uint32_t> supported_versions{kQuicVersion1};
};

template <typename Executor>
class QuicEndpoint : public std::enable_shared_from_this<QuicEndpoint<Executor>> {
  public:
    using Connection = QuicConnection<Executor>;
    using StreamTransport = typename Connection::StreamTransport;
    // What to do with a connection once it exists. The server supplies this; it
    // is where the HTTP/3 engine is built, which is what keeps this file free of
    // any knowledge of HTTP.
    using ServeFn = std::function<asio::awaitable<void>(std::shared_ptr<Connection>)>;

    QuicEndpoint(Executor exec, SSL_CTX* ssl_ctx, QuicEndpointConfig config, ServeFn serve)
        : m_executor(std::move(exec)), m_socket(m_executor), m_ssl_ctx(ssl_ctx), m_config(std::move(config)),
          m_serve(std::move(serve)) {
        m_retry_key.fill(0);
        RAND_bytes(m_retry_key.data(), static_cast<int>(m_retry_key.size()));
        m_reset_key.fill(0);
        RAND_bytes(m_reset_key.data(), static_cast<int>(m_reset_key.size()));
    }

    // Bind the socket. Returns false and reports why on failure.
    bool open(const asio::ip::udp::endpoint& endpoint, bool reuse_port, error_code& ec) {
        m_socket.open(endpoint.protocol(), ec);
        if (ec) return false;
        if (reuse_port) {
#ifdef SO_REUSEPORT
            m_socket.set_option(asio::detail::socket_option::boolean<SOL_SOCKET, SO_REUSEPORT>(true), ec);
            if (ec) {
                SIMPLE_HTTP_ERROR_LOG("QUIC SO_REUSEPORT: {}", ec.message());
                ec.clear();
            }
#else
            ec.clear();
#endif
        }
        // Large kernel buffers, best-effort.
        //
        // A QUIC server's UDP socket takes a whole connection's traffic as a
        // burst — a client's Initial flight, a window's worth of stream data, a
        // batch of acknowledgements — and the kernel default (a couple of
        // hundred kilobytes) drops the overflow. QUIC's own loss recovery then
        // does its job, which is to say the connection stalls for a PTO and
        // backs off, and a load run shows multi-second pauses that look like
        // nothing in the code. The kernel caps this at rmem_max/wmem_max, so a
        // failure here is a smaller buffer, not a broken socket.
        m_socket.set_option(asio::socket_base::receive_buffer_size(kSocketBufferBytes), ec);
        ec.clear();
        m_socket.set_option(asio::socket_base::send_buffer_size(kSocketBufferBytes), ec);
        ec.clear();

        m_socket.bind(endpoint, ec);
        return !ec;
    }

    [[nodiscard]] std::uint16_t port() const {
        error_code ec;
        return m_socket.local_endpoint(ec).port();
    }

    // Start receiving. The endpoint must be held by a shared_ptr for the
    // coroutine to keep it alive.
    void start() {
        auto self = this->shared_from_this();
        asio::co_spawn(m_executor, recv_loop(self), asio::detached);
    }

    void close() {
        error_code ec;
        m_socket.close(ec);
    }

    // Ask every connection on this endpoint to close.
    //
    // Posted, not called: a connection's state belongs to the endpoint's
    // executor, and this is called from whichever thread is stopping the server.
    // Closing the socket above stops new connections arriving but leaves the
    // running ones exactly as they were — their engines are parked on stream
    // reads, and each would hold the pool's context until its idle timeout.
    void shutdown_connections() {
        auto self = this->shared_from_this();
        asio::post(m_executor, [self] {
            for (auto& [cid, connection] : self->m_by_cid) {
                (void)cid;
                connection->close(0, "server stopping");
            }
        });
    }

    // Drop connection IDs so a peer that keeps talking after the connection is
    // gone gets a stateless reset rather than silence.
    void remember_cid(std::string cid) {
        m_recent_cids.push_back(std::move(cid));
        while (m_recent_cids.size() > kRecentCidLimit) m_recent_cids.pop_front();
    }

    [[nodiscard]] std::size_t connection_count() const noexcept { return m_by_cid.size(); }

  private:
    using Clock = std::chrono::steady_clock;

    asio::awaitable<void> recv_loop(std::shared_ptr<QuicEndpoint> self) {
        std::array<std::byte, 2048> buffer{};
        for (;;) {
            asio::ip::udp::endpoint from;
            auto [ec, n] = co_await m_socket.async_receive_from(asio::buffer(buffer), from,
                                                                asio::as_tuple(asio::use_awaitable));
            if (ec) {
                if (ec == asio::error::operation_aborted) co_return;
                // A persistent receive error would otherwise spin a core with no
                // diagnostic at all — the port would just look "slow".
                SIMPLE_HTTP_ERROR_LOG("QUIC recv: {}", ec.message());
                continue;
            }
            if (n == 0) continue;
            handle_datagram(std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(buffer.data()),
                                                          n},
                            from);
        }
    }

    void handle_datagram(std::span<const std::uint8_t> data, const asio::ip::udp::endpoint& from) {
        Reader r{data};
        PacketHeader hdr;
        const PacketParseStatus status = parse_packet_header(r, m_config.connection.connection_id_length, hdr);

        if (status == PacketParseStatus::UnsupportedVersion) {
            // A version we do not speak. The peer cannot know that without being
            // told, and the packet's connection IDs are there precisely so this
            // reply can be addressed (RFC 9000 §6).
            send_version_negotiation(hdr, from);
            return;
        }
        if (status != PacketParseStatus::Ok) return;

        if (auto it = m_by_cid.find(hdr.dcid); it != m_by_cid.end()) {
            it->second->on_datagram(data, from);
            return;
        }

        if (hdr.long_header && hdr.type == LongHeaderType::Initial) {
            if (m_config.retry && !token_valid(hdr.token, from, hdr.dcid)) {
                send_retry(hdr, from);
                return;
            }
            create_connection(data, from, hdr);
            return;
        }

        // Nothing matches. If the connection ID looks like one we issued, the
        // peer is talking to a connection that has closed: a stateless reset
        // tells it so without keeping any state (RFC 9000 §10.3).
        if (!hdr.long_header) maybe_stateless_reset(hdr.dcid, from);
    }

    void create_connection(std::span<const std::uint8_t> data, const asio::ip::udp::endpoint& from,
                           const PacketHeader& hdr) {
        if (m_by_cid.size() / 2 >= m_config.connection.max_streams) {
            // A crude bound, but the point is that an attacker cannot make the
            // table grow without limit by opening connections.
            SIMPLE_HTTP_WARN_LOG("QUIC: too many connections, dropping Initial");
            return;
        }

        const std::string original_dcid{hdr.dcid};
        const std::string peer_scid{hdr.scid};
        auto self = this->shared_from_this();

        auto connection = std::make_shared<Connection>(
            m_executor, m_ssl_ctx, m_config.connection, original_dcid, peer_scid,
            [self](Bytes datagram, const asio::ip::udp::endpoint& to) { self->send(std::move(datagram), to); },
            from, [self] { return self->make_connection_id(); }, /*address_validated=*/m_config.retry);
        if (!connection->init()) {
            SIMPLE_HTTP_ERROR_LOG("QUIC: TLS setup failed for a new connection");
            return;
        }

        // Registered under both IDs: ours, because that is what the peer will
        // use from its next flight on, and the client's original one, because
        // until our first reply arrives the client keeps retransmitting its
        // Initial under that.
        m_by_cid[connection->local_scid()] = connection;
        m_by_cid[original_dcid] = connection;
        m_original_of[connection.get()] = original_dcid;

        // The Initial that created this connection is fed in first, so the
        // handshake has something to chew on before the loops start — and so an
        // engine that asks for a stream immediately is not waiting on a
        // datagram that has already been consumed.
        connection->on_datagram(data, from);

        // The loops. `run` owns them and returns when the connection is over;
        // it must be spawned before the engine, because the engine's writes are
        // only turned into packets by the write loop inside it.
        asio::co_spawn(m_executor, connection->run(), asio::detached);

        asio::co_spawn(
            m_executor,
            [self, connection]() -> asio::awaitable<void> {
                co_await self->m_serve(connection);
                // The engine's accept loop has returned. The connection's own
                // loops may still be draining, so wait for it to report itself
                // closed before retiring its connection IDs.
                while (!connection->closed()) {
                    asio::steady_timer timer{self->m_executor};
                    timer.expires_after(std::chrono::milliseconds(20));
                    co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
                }
                self->retire(std::move(connection));
            },
            asio::detached);
    }

    void retire(std::shared_ptr<Connection> connection) {
        const std::string local = connection->local_scid();
        auto it = m_original_of.find(connection.get());
        if (it != m_original_of.end()) {
            m_by_cid.erase(it->second);
            m_original_of.erase(it);
        }
        m_by_cid.erase(local);
        // The IDs stay remembered so that a late packet gets a stateless reset
        // instead of being silently dropped — which is what a peer interprets as
        // a stalled connection.
        remember_cid(local);
    }

    void send(Bytes datagram, const asio::ip::udp::endpoint& to) {
        // A best-effort send, synchronous because a UDP send only blocks when
        // the socket buffer is full and every datagram here is at most one MTU.
        // Errors are dropped on purpose: they are per-datagram, and QUIC's own
        // loss detection is what recovers from a datagram that never arrives.
        error_code ec;
        m_socket.send_to(asio::buffer(datagram), to, 0, ec);
    }

    std::string make_connection_id() {
        std::string cid(m_config.connection.connection_id_length, '\0');
        auto* out = reinterpret_cast<unsigned char*>(cid.data());
        RAND_bytes(out, static_cast<int>(cid.size()));
        return cid;
    }

    void send_version_negotiation(const PacketHeader& hdr, const asio::ip::udp::endpoint& from) {
        // The reply echoes the peer's IDs *swapped*: their source is our
        // destination and vice versa, so the packet is addressed back to them
        // (RFC 9000 §17.2.1).
        Bytes packet;
        append_version_negotiation(packet, hdr.scid, hdr.dcid,
                                   std::span<const std::uint32_t>{m_config.supported_versions});
        send(std::move(packet), from);
    }

    void send_retry(const PacketHeader& hdr, const asio::ip::udp::endpoint& from) {
        // The Retry's source connection ID is freshly chosen: the client will
        // use it as its destination from now on, and its token is bound to it.
        const std::string new_scid = make_connection_id();
        const std::string token = make_token(from, hdr.dcid, new_scid);

        Bytes without_tag;
        Bytes packet;
        append_retry(packet, hdr.scid, new_scid, token, std::string(16, '\0'));
        without_tag = packet;
        // Trim the placeholder tag before computing the pseudo-packet; the tag is
        // computed over the Retry *without* it (RFC 9001 §5.8).
        without_tag.resize(without_tag.size() - 16);

        Bytes pseudo;
        append_retry_pseudo_packet(pseudo, hdr.dcid, without_tag);
        Bytes tag;
        retry_integrity_tag(pseudo, tag);
        if (tag.size() != 16) return;

        packet.resize(without_tag.size());
        packet += tag;
        send(std::move(packet), from);
    }

    // The token is minted by us and travels through the peer, so it has to be
    // both unforgeable and bound to the address that asked for it — otherwise it
    // is a connection slot an attacker can hand out.
    Bytes make_token(const asio::ip::udp::endpoint& from, std::string_view original_dcid,
                     std::string_view new_scid) {
        Bytes plain;
        append_address(plain, from);
        append_varint(plain, original_dcid.size());
        plain.append(original_dcid);
        append_varint(plain, new_scid.size());
        plain.append(new_scid);

        PacketKeys keys = retry_token_keys();
        Bytes nonce(12, '\0');
        RAND_bytes(reinterpret_cast<unsigned char*>(nonce.data()), 12);
        Bytes aad;
        Bytes sealed;
        // The nonce has to reach the peer, so it is prepended rather than
        // derived; the AEAD then also authenticates the address it was minted
        // for, and the tag is what makes forgery detectable.
        PacketKeys nonced = keys;
        nonced.iv = nonce;
        if (!aead_seal(nonced, 0, aad, plain, sealed)) return {};
        Bytes token = nonce;
        token += sealed;
        return token;
    }

    bool token_valid(std::string_view token, const asio::ip::udp::endpoint& from, std::string& out_dcid) {
        if (token.size() < 12 + kAeadTagLen + 1) return false;
        Bytes nonce{token.substr(0, 12)};
        PacketKeys nonced = retry_token_keys();
        nonced.iv = nonce;
        Bytes plain;
        Bytes aad;
        if (!aead_open(nonced, 0, aad, token.substr(12), plain)) return false;

        Reader r = reader_of(plain);
        asio::ip::udp::endpoint bound;
        if (!read_address(r, bound)) return false;
        if (bound != from) return false;  // minted for someone else
        const std::uint64_t dcid_len = r.varint();
        if (r.failed() || r.remaining() < dcid_len) return false;
        out_dcid.assign(reinterpret_cast<const char*>(r.rest().data()), static_cast<std::size_t>(dcid_len));
        return !out_dcid.empty() || dcid_len == 0;
    }

    [[nodiscard]] PacketKeys retry_token_keys() {
        // A key that lives as long as the endpoint: a token is only useful
        // within one connection attempt, so rotating it would buy nothing and
        // cost a synchronization point.
        PacketKeys keys;
        keys.valid = true;
        keys.md = EVP_sha256();
        keys.aead = EVP_aes_128_gcm();
        keys.hp = EVP_aes_128_ecb();
        keys.key.assign(reinterpret_cast<const char*>(m_retry_key.data()), 16);
        keys.iv.assign(12, '\0');
        keys.secret.assign(reinterpret_cast<const char*>(m_retry_key.data()), m_retry_key.size());
        return keys;
    }

    static void append_address(Bytes& out, const asio::ip::udp::endpoint& endpoint) {
        const auto addr = endpoint.address();
        if (addr.is_v6()) {
            append_u8(out, 6);
            const auto bytes = addr.to_v6().to_bytes();
            out.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        } else {
            append_u8(out, 4);
            const auto bytes = addr.to_v4().to_bytes();
            out.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }
        append_u16(out, endpoint.port());
    }

    static bool read_address(Reader& r, asio::ip::udp::endpoint& out) {
        const std::uint8_t family = r.u8();
        if (r.failed()) return false;
        if (family == 4) {
            const auto bytes = r.bytes(4);
            if (r.failed()) return false;
            asio::ip::address_v4::bytes_type b{};
            std::ranges::copy(bytes, b.begin());
            out.address(asio::ip::address_v4{b});
        } else if (family == 6) {
            const auto bytes = r.bytes(16);
            if (r.failed()) return false;
            asio::ip::address_v6::bytes_type b{};
            std::ranges::copy(bytes, b.begin());
            out.address(asio::ip::address_v6{b});
        } else {
            return false;
        }
        out.port(r.u16());
        return !r.failed();
    }

    void maybe_stateless_reset(std::string_view dcid, const asio::ip::udp::endpoint& from) {
        if (dcid.size() < 21) return;  // too short to carry a reset token
        const bool known =
            std::find(m_recent_cids.begin(), m_recent_cids.end(), dcid) != m_recent_cids.end();
        if (!known) return;

        // The reset is a short header packet whose last 16 octets are a token
        // derived from the connection ID, so the peer — which knows the same
        // token from the transport parameters — recognises it (RFC 9000 §10.3).
        Bytes packet;
        std::uint8_t first = static_cast<std::uint8_t>(0x40 | 0x02);  // fixed bit, 4-octet pn length
        append_u8(packet, first);
        packet.append(dcid);
        for (std::size_t i = 0; i < 4; ++i) {
            packet.push_back(static_cast<char>(m_random() & 0xff));
        }
        while (packet.size() + 16 < kMinInitialDatagramSize) {
            packet.push_back(static_cast<char>(m_random() & 0xff));
        }
        std::array<unsigned char, 32> mac{};
        unsigned int mac_len = 0;
        if (HMAC(EVP_sha256(), m_reset_key.data(), static_cast<int>(m_reset_key.size()),
                 reinterpret_cast<const unsigned char*>(dcid.data()), dcid.size(), mac.data(), &mac_len) ==
            nullptr) {
            return;
        }
        packet.append(reinterpret_cast<const char*>(mac.data()), 16);
        send(std::move(packet), from);
    }

    Executor m_executor;
    asio::ip::udp::socket m_socket;
    SSL_CTX* m_ssl_ctx{nullptr};
    QuicEndpointConfig m_config;
    ServeFn m_serve;

    std::unordered_map<std::string, std::shared_ptr<Connection>> m_by_cid;
    std::unordered_map<const Connection*, std::string> m_original_of;
    std::deque<std::string> m_recent_cids;

    std::array<unsigned char, 16> m_retry_key{};
    std::array<unsigned char, 32> m_reset_key{};
    std::mt19937_64 m_random{std::random_device{}()};
};

}  // namespace simple_http::quic
