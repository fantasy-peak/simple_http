#pragma once

// The SSL_CTX a QUIC listener needs, and the ALPN token HTTP/3 uses.
//
// This file used to also hold the per-connection handshake driver — the
// `SSL_set_quic_tls_cbs` dispatch table that moved handshake bytes between
// OpenSSL and a hand-written CRYPTO-frame layer. That is gone: ngtcp2's
// OpenSSL crypto helper (`ngtcp2_crypto_ossl`) drives the handshake now, which
// is the whole point of moving to ngtcp2. What is left is the part that is
// genuinely ours — which certificates to load, which protocols to advertise —
// and that is a policy decision no library can make for us.

#include <openssl/ssl.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "../core/asio_ssl.h"
#include "../core/logging.h"
#include "../core/types.h"
#include "../transport/tls_context.h"
#include "ngtcp2_crypto.h"

namespace simple_http::quic {

// Maps OpenSSL's decrypted-ticket verdict to the code we hand back. A ticket
// whose appdata no longer matches (stale HTTP/3 SETTINGS snapshot) must still
// be renewed: SSL_TICKET_RETURN_IGNORE would leave the client offering the
// stale ticket forever. Kept pure so this rule is unit-testable without a TLS
// handshake.
constexpr SSL_TICKET_RETURN ticket_return_code(SSL_TICKET_STATUS status, bool appdata_compatible) noexcept {
    if (status == SSL_TICKET_EMPTY || status == SSL_TICKET_NO_DECRYPT || !appdata_compatible)
        return SSL_TICKET_RETURN_IGNORE_RENEW;
    return status == SSL_TICKET_SUCCESS_RENEW ? SSL_TICKET_RETURN_USE_RENEW : SSL_TICKET_RETURN_USE;
}

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
// 0-RTT state carried by one QUIC listener's context. The HTTP/3 layer owns the
// appdata format; this file only stores the blob and asks the validator whether
// a decrypted ticket may be used for early data. An explicit appdata value is
// required: a ticket with no compatibility snapshot cannot be replayed safely.
struct QuicEarlyDataConfig {
    bool enabled{false};
    std::string ticket_appdata;
    std::function<bool(std::string_view)> validate_ticket_appdata;
};

class QuicTlsContext {
  public:
    QuicTlsContext(const TlsConfig &cfg, QuicEarlyDataConfig early = {})
        : m_ctx(context_method()), m_early(std::move(early)) {
        configure(cfg);
        configure_early_data();
    }

    QuicTlsContext(const QuicTlsContext &) = delete;
    QuicTlsContext &operator=(const QuicTlsContext &) = delete;

    [[nodiscard]] asio::ssl::context &context() { return m_ctx; }
    [[nodiscard]] SSL_CTX *native_handle() { return m_ctx.native_handle(); }

  private:
    static asio::ssl::context::method context_method() {
        simple_http::detail::ensure_openssl_initialized();
        return asio::ssl::context::tlsv13_server;
    }

    static bool is_regular_file(const std::filesystem::path &path) {
        std::error_code ec;
        return std::filesystem::is_regular_file(path, ec) && !ec;
    }

    void configure(const TlsConfig &cfg) {
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
        if (ec)
            throw std::runtime_error("QUIC use_certificate_chain_file: " + ec.message());
        m_ctx.use_private_key_file(cfg.private_key_file, asio::ssl::context::pem, ec);
        if (ec)
            throw std::runtime_error("QUIC use_private_key_file: " + ec.message());

        // A QUIC *server must* have an alpn_select_cb: the QUIC-TLS layer refuses
        // to configure without one (ssl/quic/quic_tls.c), because a QUIC
        // connection with no negotiated protocol has no meaning.
        SSL_CTX_set_alpn_select_cb(m_ctx.native_handle(), &QuicTlsContext::select_alpn, nullptr);

        // RFC 9001 §8.2 requires quic_transport_parameters in the ClientHello.
        // ngtcp2 reports a ClientHello without the extension and one whose
        // parameters lack initial_source_connection_id with the same error, and
        // OpenSSL 4 does not expose the received parameters after the fact. The
        // ClientHello callback is the only place to learn whether the extension
        // was actually present, which decides between a missing_extension alert
        // (0x016d) and TRANSPORT_PARAMETER_ERROR (0x08).
        SSL_CTX_set_client_hello_cb(m_ctx.native_handle(), &QuicTlsContext::on_client_hello, nullptr);
    }

    void configure_early_data() {
        if (!m_early.enabled || m_early.ticket_appdata.empty())
            return;

        SSL_CTX *ctx = m_ctx.native_handle();
        static constexpr unsigned char kSessionIdContext[] = "simple_http h3";
        if (SSL_CTX_set_session_id_context(ctx, kSessionIdContext, sizeof(kSessionIdContext) - 1) != 1) {
            throw std::runtime_error("QUIC SSL_CTX_set_session_id_context failed");
        }
        const auto max_early = std::numeric_limits<std::uint32_t>::max();
        if (SSL_CTX_set_max_early_data(ctx, max_early) != 1 || SSL_CTX_set_recv_max_early_data(ctx, max_early) != 1) {
            throw std::runtime_error("QUIC could not enable TLS early data");
        }
        // Stateless tickets shared by every SO_REUSEPORT endpoint of this
        // listener. OpenSSL's built-in anti-replay would reject a ticket seen on
        // a different socket (and can only remember a bounded window); quic-go
        // makes the same trade-off: no replay protection, documented, and only
        // idempotent requests opt in.
        (void)SSL_CTX_set_options(ctx, SSL_OP_NO_ANTI_REPLAY);
        if (SSL_CTX_set_session_ticket_cb(ctx, &QuicTlsContext::generate_ticket, &QuicTlsContext::decrypt_ticket,
                                          this) != 1) {
            throw std::runtime_error("QUIC SSL_CTX_set_session_ticket_cb failed");
        }
    }

    static int generate_ticket(SSL *ssl, void *arg) {
        auto *self = static_cast<QuicTlsContext *>(arg);
        SSL_SESSION *session = SSL_get0_session(ssl);
        if (session == nullptr)
            return 0;
        if (!SSL_SESSION_set1_ticket_appdata(session, self->m_early.ticket_appdata.data(),
                                             self->m_early.ticket_appdata.size())) {
            return 0;
        }
        return 1;
    }

    static SSL_TICKET_RETURN decrypt_ticket(SSL *, SSL_SESSION *session, const unsigned char *, size_t,
                                            SSL_TICKET_STATUS status, void *arg) {
        auto *self = static_cast<QuicTlsContext *>(arg);
        if (status == SSL_TICKET_EMPTY || status == SSL_TICKET_NO_DECRYPT)
            return ticket_return_code(status, false);

        void *data = nullptr;
        size_t len = 0;
        const bool present = SSL_SESSION_get0_ticket_appdata(session, &data, &len) == 1 && data != nullptr;
        const bool compatible =
            present && self->m_early.validate_ticket_appdata &&
            self->m_early.validate_ticket_appdata(std::string_view{static_cast<const char *>(data), len});
        return ticket_return_code(status, compatible);
    }

    static int select_alpn(SSL *, const unsigned char **out, unsigned char *outlen, const unsigned char *in,
                           unsigned int inlen, void *) {
        // SSL_select_next_proto does the offer/selection matching, including the
        // no-overlap case. The failure is returned as a fatal alert rather than
        // NOACK: a client that offers only http/1.1 has asked for something this
        // listener cannot do, and saying so is better than a connection that
        // silently has no protocol.
        if (SSL_select_next_proto(const_cast<unsigned char **>(out), outlen, kHttp3AlpnWire.data(),
                                  static_cast<unsigned int>(kHttp3AlpnWire.size()), in,
                                  inlen) != OPENSSL_NPN_NEGOTIATED) {
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
        return SSL_TLSEXT_ERR_OK;
    }

    // The quic_transport_parameters extension type (RFC 9001 §8.2).
    static constexpr unsigned int kTransportParamsExtension = 0x39;

    // Runs while OpenSSL parses the ClientHello, which is the only moment
    // SSL_client_hello_get0_ext is valid. The connection object is reached
    // through the app data ngtcp2 installs on the SSL (its conn_ref); the
    // handshake cannot start before QuicCrypto::init has set it.
    static int on_client_hello(SSL *ssl, int *, void *) {
        auto *ref = static_cast<ngtcp2_crypto_conn_ref *>(SSL_get_app_data(ssl));
        if (ref == nullptr || ref->get_conn == nullptr)
            return SSL_CLIENT_HELLO_SUCCESS;
        const unsigned char *data = nullptr;
        size_t len = 0;
        const bool present = SSL_client_hello_get0_ext(ssl, kTransportParamsExtension, &data, &len) == 1;
        static_cast<ConnectionCryptoBase *>(ref->user_data)->on_client_hello_tp(present);
        return SSL_CLIENT_HELLO_SUCCESS;
    }

    asio::ssl::context m_ctx;
    QuicEarlyDataConfig m_early;
};

} // namespace simple_http::quic
