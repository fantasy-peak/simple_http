#pragma once

// The bridge between OpenSSL's TLS 1.3 handshake and this QUIC implementation
// (RFC 9001 §4), via the QUIC-TLS API OpenSSL 3.5 introduced.
//
// What OpenSSL does for us: everything in the handshake. What it does not do,
// and what this file is: move handshake bytes, and decide which encryption level
// they belong to.
//
// That last part is the whole difficulty, because `crypto_recv_rcd` — the
// callback through which OpenSSL asks for handshake data — **has no level
// parameter**. OpenSSL reads each level through its own record layer and asks
// whichever one it is currently working on, so the QUIC side has to track the
// level itself. The rule (following OpenSSL's own implementation in
// `ssl/quic/quic_channel.c`) is:
//
//   * `rx_level` starts at Initial and `tx_level` at Initial; neither level's
//     keys come from TLS. Initial keys are derived from the client's destination
//     connection ID and the fixed salt, which is why no secret is yielded for
//     them.
//   * `yield_secret` advances the level it names, in the direction it names.
//     So the level advances exactly when OpenSSL is ready to move on — which is
//     also exactly when it will next ask for data at that level.
//   * `crypto_recv_rcd` therefore serves the current level's crypto stream, and
//     `crypto_send` appends to the current transmit level's.
//   * The lower level's stream must be empty when the level advances. Data
//     arriving late at an abandoned level is a protocol violation
//     (RFC 9001 §4.1.3), and detecting it here is what stops a peer from
//     smuggling handshake bytes past the point where they would be replayed.
//
// Crypto data uses the same `SendStream`/`RecvStream` machinery as application
// data, because a CRYPTO frame *is* a stream (RFC 9000 §19.6) — one per
// encryption level, whose offsets are indexed per packet number space rather
// than by stream id. That reuse is what gives handshake retransmission for free.

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include <boost/asio/ssl.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/core_dispatch.h>

#include "../core/logging.h"
#include "../transport/tls_context.h"
#include "crypto.h"
#include "stream.h"
#include "transport_params.h"
#include "wire.h"

namespace simple_http::quic {

// The ALPN token HTTP/3 uses. QUIC mandates ALPN (§8.1), so this is not
// optional the way it is for TCP: a connection that negotiates nothing is
// closed with a no_application_protocol alert.
inline constexpr std::string_view kHttp3Alpn = "h3";

// The same token in ALPN *wire* form: a length octet followed by the name. The
// two are easy to confuse and impossible to confuse silently — passing the bare
// name to SSL_select_next_proto reads 'h' (0x68) as the length of the first
// entry and fails to match anything, which surfaces as a bare
// no_application_protocol alert with no hint as to why.
inline constexpr std::array<unsigned char, 3> kHttp3AlpnWire{0x02, 'h', '3'};

// The SSL_CTX a QUIC listener needs. Deliberately separate from
// transport/tls_context.h's TlsContext: that one advertises h2 and http/1.1
// over TCP, and a QUIC connection must offer `h3` and nothing else. Sharing a
// context would mean advertising protocols neither transport can actually
// speak.
//
// It is an asio::ssl::context rather than a raw SSL_CTX so that the TlsConfig
// fields — including the user's `setup` hook — go through exactly the same asio
// calls the TCP side uses. Reimplementing certificate loading against the raw
// OpenSSL API would be a second copy of the same policy, free to drift.
class QuicTlsContext {
  public:
    explicit QuicTlsContext(const TlsConfig& cfg) : m_ctx(asio::ssl::context::tlsv13_server) {
        configure(cfg);
    }

    [[nodiscard]] asio::ssl::context& context() { return m_ctx; }
    [[nodiscard]] SSL_CTX* native_handle() { return m_ctx.native_handle(); }

  private:
    static bool is_regular_file(const std::filesystem::path& path) {
        std::error_code ec;
        return std::filesystem::is_regular_file(path, ec) && !ec;
    }

    void configure(const TlsConfig& cfg) {
        if (!is_regular_file(cfg.cert_chain_file)) {
            throw std::runtime_error("QUIC TLS certificate not found: " + cfg.cert_chain_file);
        }
        if (!is_regular_file(cfg.private_key_file)) {
            throw std::runtime_error("QUIC TLS private key not found: " + cfg.private_key_file);
        }

        m_ctx.set_options(asio::ssl::context::default_workarounds);

        // The hook customizes the context; it does not get to replace the
        // security policy, exactly as on the TCP side — running it first and
        // applying the policy after means a caller cannot silently lose
        // client-certificate verification.
        if (cfg.setup) {
            cfg.setup(m_ctx);
        }
        if (cfg.mutual) {
            m_ctx.set_verify_mode(asio::ssl::verify_peer | asio::ssl::verify_fail_if_no_peer_cert);
            if (cfg.ca_file) {
                m_ctx.load_verify_file(*cfg.ca_file);
            } else {
                m_ctx.set_default_verify_paths();
            }
        }

        error_code ec;
        m_ctx.use_certificate_chain_file(cfg.cert_chain_file, ec);
        if (ec) throw std::runtime_error("QUIC use_certificate_chain_file: " + ec.message());
        m_ctx.use_private_key_file(cfg.private_key_file, asio::ssl::context::pem, ec);
        if (ec) throw std::runtime_error("QUIC use_private_key_file: " + ec.message());

        // A QUIC *server must* have an alpn_select_cb: the QUIC-TLS layer refuses
        // to configure without one (ssl/quic/quic_tls.c), because a QUIC
        // connection with no negotiated protocol has no meaning.
        SSL_CTX_set_alpn_select_cb(m_ctx.native_handle(), &QuicTlsContext::select_alpn, nullptr);
    }

    static int select_alpn(SSL*, const unsigned char** out, unsigned char* outlen, const unsigned char* in,
                           unsigned int inlen, void*) {
        // SSL_select_next_proto does the offer/selection matching, including the
        // no-overlap case. The failure is returned as a fatal alert rather than
        // NOACK: a client that offers only http/1.1 has asked for something this
        // listener cannot do, and saying so is better than a connection that
        // silently has no protocol.
        if (SSL_select_next_proto(const_cast<unsigned char**>(out), outlen, kHttp3AlpnWire.data(),
                                  static_cast<unsigned int>(kHttp3AlpnWire.size()), in,
                                  inlen) != OPENSSL_NPN_NEGOTIATED) {
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
        return SSL_TLSEXT_ERR_OK;
    }

    asio::ssl::context m_ctx;
};

// The connection's hooks into the handshake. All of them are called from inside
// an OpenSSL callback, so they must not throw and must not re-enter OpenSSL.
struct QuicTlsHooks {
    // Crypto data is queued for sending at this level; the connection should
    // flush its send loop.
    std::function<void(EncryptionLevel)> on_crypto_ready;
    // Keys for this level and direction are installed.
    std::function<void(EncryptionLevel, bool read_direction)> on_keys;
    // The peer's transport parameters arrived (from the TLS extension).
    std::function<void(std::string_view)> on_peer_transport_params;
    // The peer sent a TLS alert; the connection must close.
    std::function<void(std::uint8_t)> on_alert;
};

class QuicTls {
  public:
    QuicTls() = default;

    ~QuicTls() {
        if (m_ssl) SSL_free(m_ssl);
    }

    QuicTls(const QuicTls&) = delete;
    QuicTls& operator=(const QuicTls&) = delete;

    // Set up the SSL object for a server-side QUIC connection. `ours` is
    // encoded into the quic_transport_parameters extension; the peer's comes
    // back through the hook.
    bool init(SSL_CTX* ctx, const TransportParams& ours, QuicTlsHooks hooks, bool allow_early_data = false) {
        m_hooks = std::move(hooks);
        m_ssl = SSL_new(ctx);
        if (!m_ssl) return false;

        // Installs the custom record layer and the transport-parameters
        // extension, and pins TLS 1.3. Everything the QUIC-TLS layer needs is
        // configured by this one call.
        if (SSL_set_quic_tls_cbs(m_ssl, dispatch_table(), this) != 1) {
            SIMPLE_HTTP_ERROR_LOG("SSL_set_quic_tls_cbs: {}", last_error());
            return false;
        }

        // The encoded parameters live in a member, not a local.
        //
        // `ossl_quic_tls_set_transport_params` stores the *pointer*
        // (ssl/quic/quic_tls.c), and OpenSSL reads it when it builds the
        // EncryptedExtensions — after this function has returned. A local here
        // leaves it writing freed heap into the extension, and the peer reports
        // that as a transport-parameter error naming whichever field the garbage
        // happened to land in, which is nowhere near the cause.
        m_local_transport_params = encode_transport_params(ours, /*server=*/true);
        if (SSL_set_quic_tls_transport_params(
                m_ssl, reinterpret_cast<const unsigned char*>(m_local_transport_params.data()),
                m_local_transport_params.size()) != 1) {
            SIMPLE_HTTP_ERROR_LOG("SSL_set_quic_tls_transport_params: {}", last_error());
            return false;
        }

        if (allow_early_data && SSL_set_quic_tls_early_data_enabled(m_ssl, 1) != 1) {
            SIMPLE_HTTP_ERROR_LOG("SSL_set_quic_tls_early_data_enabled: {}", last_error());
            return false;
        }

        // `SSL_set_quic_tls_cbs` does not pick a side; the SSL was made from
        // TLS_server_method, so this only has to make it explicit.
        SSL_set_accept_state(m_ssl);
        return true;
    }

    [[nodiscard]] SSL* ssl() const noexcept { return m_ssl; }
    [[nodiscard]] bool valid() const noexcept { return m_ssl != nullptr; }

    [[nodiscard]] EncryptionLevel rx_level() const noexcept { return m_rx_level; }
    [[nodiscard]] EncryptionLevel tx_level() const noexcept { return m_tx_level; }

    [[nodiscard]] bool complete() const noexcept { return m_ssl && SSL_is_init_finished(m_ssl) == 1; }
    [[nodiscard]] bool failed() const noexcept { return m_failed; }

    // The negotiated ALPN. Checked against `h3` after the handshake: OpenSSL
    // validates that *something* was negotiated, not that it was HTTP/3.
    [[nodiscard]] std::string alpn_selected() const {
        const unsigned char* proto = nullptr;
        unsigned int len = 0;
        if (!m_ssl) return {};
        SSL_get0_alpn_selected(m_ssl, &proto, &len);
        if (!proto || len == 0) return {};
        return {reinterpret_cast<const char*>(proto), len};
    }

    [[nodiscard]] std::string_view peer_transport_params() const noexcept { return m_peer_params; }

    // Feed handshake data that arrived in a CRYPTO frame at `level`.
    [[nodiscard]] bool provide_crypto(EncryptionLevel level, std::uint64_t offset, std::string_view data) {
        const std::size_t index = static_cast<std::size_t>(level);
        if (index >= kEncryptionLevelCount) return false;
        // Data for a level we have already left. The stream retains what it has,
        // so the check is "nothing new beyond what was consumed" rather than
        // "nothing at all": a retransmission of handshake bytes is legal, new
        // bytes are not (RFC 9001 §4.1.3).
        if (level < m_rx_level && m_crypto_recv[index].highest_offset() < offset + data.size()) {
            return false;
        }
        if (!m_crypto_recv[index].push(offset, data, /*fin=*/false)) return false;
        return true;
    }

    // Handshake data queued at `level`, for the connection to frame. The view
    // points into the stream's buffer and is valid until the next call.
    bool next_crypto(EncryptionLevel level, std::size_t max_len, std::uint64_t limit, std::uint64_t& offset,
                     std::string_view& data) {
        const std::size_t index = static_cast<std::size_t>(level);
        if (index >= kEncryptionLevelCount) return false;
        SendStream::Chunk chunk;
        // CRYPTO is not flow controlled (RFC 9000 §4.1), so there is no
        // connection-level credit to spend and no bound to pass but the length.
        if (!m_crypto_send[index].next(max_len, limit, SendStream::kUnlimitedNewData, chunk)) return false;
        if (chunk.fin) return false;  // CRYPTO streams are never finished
        offset = chunk.offset;
        data = chunk.data;
        return true;
    }

    [[nodiscard]] bool has_crypto_to_send(EncryptionLevel level) const {
        const std::size_t index = static_cast<std::size_t>(level);
        return index < kEncryptionLevelCount && m_crypto_send[index].has_pending();
    }

    void crypto_acked(EncryptionLevel level, std::uint64_t offset, std::uint64_t length) {
        const std::size_t index = static_cast<std::size_t>(level);
        if (index < kEncryptionLevelCount) m_crypto_send[index].on_acked(offset, length);
    }

    void crypto_lost(EncryptionLevel level, std::uint64_t offset, std::uint64_t length) {
        const std::size_t index = static_cast<std::size_t>(level);
        if (index < kEncryptionLevelCount) m_crypto_send[index].on_lost(offset, length);
    }

    // How much unacknowledged handshake data is queued at a level. Used to bound
    // the buffer a peer can make us hold, which is CRYPTO_BUFFER_EXCEEDED.
    [[nodiscard]] std::size_t crypto_pending(EncryptionLevel level) const {
        const std::size_t index = static_cast<std::size_t>(level);
        return index < kEncryptionLevelCount ? m_crypto_send[index].buffered() : 0;
    }

    // The keys for a level, in one direction. `read` selects the receive keys.
    [[nodiscard]] const PacketKeys& keys(EncryptionLevel level, bool read) const {
        return read ? m_rx_keys[static_cast<std::size_t>(level)] : m_tx_keys[static_cast<std::size_t>(level)];
    }

    // Advance the handshake by one step. `WANT_READ`/`WANT_WRITE` are the normal
    // outcomes for a peer that has not sent its next flight yet.
    enum class Status { Ok, WantData, Error };

    Status tick() {
        if (!m_ssl || m_failed) return Status::Error;

        // SSL_get_error guesses from the error stack, and a stray entry left by
        // an earlier operation makes it report SSL_ERROR_SSL for what was really
        // WANT_READ. Marking the stack and checking whether the call added to it
        // is how OpenSSL's own QUIC layer separates the two; the same trick is
        // needed here.
        const int mark = ERR_count_to_mark();
        const int ret = complete() ? SSL_read(m_ssl, nullptr, 0) : SSL_do_handshake(m_ssl);
        if (ret > 0) return Status::Ok;

        const int err = SSL_get_error(m_ssl, ret);
        switch (err) {
            case SSL_ERROR_WANT_READ:
            case SSL_ERROR_WANT_WRITE:
            case SSL_ERROR_WANT_CLIENT_HELLO_CB:
            case SSL_ERROR_WANT_X509_LOOKUP:
            case SSL_ERROR_WANT_RETRY_VERIFY:
                ERR_pop_to_mark();
                return Status::WantData;
            default:
                if (ERR_count_to_mark() > mark) {
                    SIMPLE_HTTP_ERROR_LOG("QUIC TLS handshake: {}", last_error());
                }
                m_failed = true;
                return Status::Error;
        }
    }

  private:
    using self = QuicTls;

    static const OSSL_DISPATCH* dispatch_table() {
        static const OSSL_DISPATCH table[] = {
            {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_SEND, reinterpret_cast<void (*)(void)>(&self::crypto_send)},
            {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_RECV_RCD, reinterpret_cast<void (*)(void)>(&self::recv_rcd)},
            {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_RELEASE_RCD, reinterpret_cast<void (*)(void)>(&self::release_rcd)},
            {OSSL_FUNC_SSL_QUIC_TLS_YIELD_SECRET, reinterpret_cast<void (*)(void)>(&self::yield_secret)},
            {OSSL_FUNC_SSL_QUIC_TLS_GOT_TRANSPORT_PARAMS,
             reinterpret_cast<void (*)(void)>(&self::got_transport_params)},
            {OSSL_FUNC_SSL_QUIC_TLS_ALERT, reinterpret_cast<void (*)(void)>(&self::alert)},
            {0, nullptr},
        };
        return table;
    }

    // OpenSSL hands us handshake bytes to put in CRYPTO frames at the current
    // transmit level. It can be called repeatedly for one flight.
    static int crypto_send(SSL*, const unsigned char* buf, size_t buf_len, size_t* consumed, void* arg) {
        auto* self_ptr = static_cast<QuicTls*>(arg);
        const std::size_t index = static_cast<std::size_t>(self_ptr->m_tx_level);
        if (index >= kEncryptionLevelCount) return 0;
        if (!self_ptr->m_crypto_send[index].write(
                std::string_view{reinterpret_cast<const char*>(buf), buf_len})) {
            return 0;
        }
        *consumed = buf_len;
        if (self_ptr->m_hooks.on_crypto_ready) self_ptr->m_hooks.on_crypto_ready(self_ptr->m_tx_level);
        return 1;
    }

    // OpenSSL asks for handshake data at whatever level it is reading. A
    // zero-length answer means "nothing yet" and is not an error: it is how a
    // handshake waits for the next flight.
    static int recv_rcd(SSL*, const unsigned char** buf, size_t* bytes_read, void* arg) {
        auto* self_ptr = static_cast<QuicTls*>(arg);
        *buf = nullptr;
        *bytes_read = 0;

        // Anything not yet released by OpenSSL has to be offered again; the
        // record layer may consume only part of what it was handed.
        if (self_ptr->m_rx_released < self_ptr->m_rx_hold.size()) {
            *buf = reinterpret_cast<const unsigned char*>(self_ptr->m_rx_hold.data() + self_ptr->m_rx_released);
            *bytes_read = self_ptr->m_rx_hold.size() - self_ptr->m_rx_released;
            return 1;
        }

        // Advancing past a level whose stream still holds unconsumed data would
        // lose it, and a peer that sent it there was trying to get handshake
        // bytes replayed at a level they no longer belong to (RFC 9001 §4.1.3).
        for (std::size_t i = 0; i < static_cast<std::size_t>(self_ptr->m_rx_level); ++i) {
            if (self_ptr->m_crypto_recv[i].readable() > 0) {
                SIMPLE_HTTP_ERROR_LOG("QUIC: crypto data left at level {} when reading level {}", i,
                                      static_cast<unsigned>(self_ptr->m_rx_level));
                return 0;
            }
        }

        const std::size_t index = static_cast<std::size_t>(self_ptr->m_rx_level);
        if (index >= kEncryptionLevelCount) return 0;
        RecvStream& stream = self_ptr->m_crypto_recv[index];
        if (stream.readable() == 0) return 1;  // nothing yet: retry later

        self_ptr->m_rx_hold.clear();
        self_ptr->m_rx_released = 0;
        stream.drain(self_ptr->m_rx_hold);
        *buf = reinterpret_cast<const unsigned char*>(self_ptr->m_rx_hold.data());
        *bytes_read = self_ptr->m_rx_hold.size();
        return 1;
    }

    static int release_rcd(SSL*, size_t bytes_read, void* arg) {
        auto* self_ptr = static_cast<QuicTls*>(arg);
        self_ptr->m_rx_released += bytes_read;
        if (self_ptr->m_rx_released >= self_ptr->m_rx_hold.size()) {
            self_ptr->m_rx_hold.clear();
            self_ptr->m_rx_released = 0;
        }
        return 1;
    }

    // A new traffic secret. In the write direction this is the signal to install
    // transmit keys and move the level forward; in the read direction, receive
    // keys — and `m_rx_level` moving is what makes `recv_rcd` start serving the
    // next level's stream.
    static int yield_secret(SSL* ssl, uint32_t prot_level, int direction, const unsigned char* secret,
                            size_t secret_len, void* arg) {
        auto* self_ptr = static_cast<QuicTls*>(arg);
        const EncryptionLevel level = level_from_protection(prot_level);
        const std::size_t index = static_cast<std::size_t>(level);
        if (index >= kEncryptionLevelCount) return 0;

        // The dispatch signature drops the cipher and digest the internal
        // callback carries, so they have to be read back off the SSL object —
        // selected by the time a secret is exported. The suite *id* is what
        // they are derived from, not the NIDs: for a TLS 1.3 suite the hash is
        // part of the suite, and `SSL_CIPHER_get_digest_nid()` answers
        // NID_undef for every one of them.
        const SSL_CIPHER* cipher = SSL_get_current_cipher(ssl);
        if (!cipher) return 0;
        const std::uint16_t suite = static_cast<std::uint16_t>(SSL_CIPHER_get_id(cipher) & 0xffff);
        const EVP_CIPHER* aead = aead_from_suite(suite);
        const EVP_MD* md = md_from_suite(suite);
        if (!aead || !md) {
            SIMPLE_HTTP_ERROR_LOG("QUIC TLS: unsupported cipher suite");
            return 0;
        }

        PacketKeys keys = derive_packet_keys(std::string_view{reinterpret_cast<const char*>(secret), secret_len},
                                             md, aead);
        if (!keys.valid) return 0;

        if (direction) {
            // A level below the one we are already transmitting at would mean
            // OpenSSL went backwards; refusing keeps the level monotonic, which
            // the receive side depends on.
            if (level <= self_ptr->m_tx_level) return 0;
            self_ptr->m_tx_keys[index] = std::move(keys);
            self_ptr->m_tx_level = level;
        } else {
            if (level <= self_ptr->m_rx_level) return 0;
            self_ptr->m_rx_keys[index] = std::move(keys);
            self_ptr->m_rx_level = level;
        }
        if (self_ptr->m_hooks.on_keys) self_ptr->m_hooks.on_keys(level, direction == 0);
        return 1;
    }

    static int got_transport_params(SSL*, const unsigned char* params, size_t params_len, void* arg) {
        auto* self_ptr = static_cast<QuicTls*>(arg);
        self_ptr->m_peer_params.assign(reinterpret_cast<const char*>(params), params_len);
        if (self_ptr->m_hooks.on_peer_transport_params) {
            self_ptr->m_hooks.on_peer_transport_params(self_ptr->m_peer_params);
        }
        return 1;
    }

    static int alert(SSL*, unsigned char alert_code, void* arg) {
        auto* self_ptr = static_cast<QuicTls*>(arg);
        if (self_ptr->m_hooks.on_alert) self_ptr->m_hooks.on_alert(alert_code);
        return 1;
    }

    static EncryptionLevel level_from_protection(std::uint32_t prot_level) {
        switch (prot_level) {
            case OSSL_RECORD_PROTECTION_LEVEL_EARLY:
                return EncryptionLevel::ZeroRtt;
            case OSSL_RECORD_PROTECTION_LEVEL_HANDSHAKE:
                return EncryptionLevel::Handshake;
            case OSSL_RECORD_PROTECTION_LEVEL_APPLICATION:
                return EncryptionLevel::OneRtt;
            default:
                // NONE (0) is what a record layer with no TLS secret reports —
                // which for QUIC is the Initial level, whose keys come from the
                // connection ID instead of from TLS. It never reaches here
                // because no secret is yielded for it.
                return EncryptionLevel::Initial;
        }
    }

    // The TLS 1.3 cipher suites of RFC 8446 §B.4, by their IANA value. AEAD and
    // hash travel together in a TLS 1.3 suite, which is why one switch answers
    // both.
    enum Suite : std::uint16_t {
        kAes128GcmSha256 = 0x1301,
        kAes256GcmSha384 = 0x1302,
        kChacha20Poly1305Sha256 = 0x1303,
    };

    static const EVP_CIPHER* aead_from_suite(std::uint16_t suite) {
        switch (suite) {
            case kAes128GcmSha256:
                return EVP_aes_128_gcm();
            case kAes256GcmSha384:
                return EVP_aes_256_gcm();
            case kChacha20Poly1305Sha256:
                return EVP_chacha20_poly1305();
            default:
                return nullptr;
        }
    }

    static const EVP_MD* md_from_suite(std::uint16_t suite) {
        switch (suite) {
            case kAes128GcmSha256:
            case kChacha20Poly1305Sha256:
                return EVP_sha256();
            case kAes256GcmSha384:
                return EVP_sha384();
            default:
                return nullptr;
        }
    }

    static std::string last_error() {
        std::array<char, 256> buf{};
        ERR_error_string_n(ERR_get_error(), buf.data(), buf.size());
        return buf.data();
    }

    SSL* m_ssl{nullptr};
    QuicTlsHooks m_hooks;

    // One CRYPTO stream per encryption level, in each direction. Indexed by
    // EncryptionLevel; 0-RTT has no CRYPTO stream of its own in practice, but
    // the array is uniform so an index is never out of range.
    std::array<SendStream, kEncryptionLevelCount> m_crypto_send{};
    std::array<RecvStream, kEncryptionLevelCount> m_crypto_recv{};

    std::array<PacketKeys, kEncryptionLevelCount> m_rx_keys{};
    std::array<PacketKeys, kEncryptionLevelCount> m_tx_keys{};

    EncryptionLevel m_rx_level{EncryptionLevel::Initial};
    EncryptionLevel m_tx_level{EncryptionLevel::Initial};

    // The chunk handed to OpenSSL, and how much of it it has released. The
    // record layer is not obliged to consume everything at once, so the buffer
    // has to outlive the call and be re-offered until it is drained.
    std::string m_rx_hold;
    std::size_t m_rx_released{0};

    // Ours, held because OpenSSL only borrows the pointer.
    Bytes m_local_transport_params;
    std::string m_peer_params;
    bool m_failed{false};
};

}  // namespace simple_http::quic
