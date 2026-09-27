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

#include <array>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

#include <boost/asio/ssl.hpp>
#include <openssl/ssl.h>

#include "../core/logging.h"
#include "../core/types.h"
#include "../transport/tls_context.h"

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

}  // namespace simple_http::quic
