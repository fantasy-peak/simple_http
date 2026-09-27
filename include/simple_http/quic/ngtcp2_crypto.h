#pragma once

// One connection's TLS session, as ngtcp2's OpenSSL helper wants it.
//
// The handshake itself is no longer ours: `ngtcp2_crypto_ossl` drives OpenSSL
// through the QUIC-TLS API, and the ten crypto callbacks ngtcp2 needs
// (encrypt/decrypt/hp_mask/...) are its own functions, wired up wholesale in
// the connection's callback table. What is left for this file is the plumbing
// that ties one `SSL*` to one `ngtcp2_conn`:
//
//   * a `ngtcp2_crypto_conn_ref` whose `get_conn` hands the library the
//     connection it is currently working on — OpenSSL callbacks such as the
//     certificate-verify hook arrive with only the SSL, so the SSL has to carry
//     the way back. That is `SSL_set_app_data`.
//
//   * the `ngtcp2_crypto_ossl_ctx`, which is where the helper keeps per-connection
//     OpenSSL state and which owns the `SSL*` for freeing.
//
// Only TLS 1.3 exists here, and only as a server: QUIC forbids anything else
// (RFC 9001 §4.2), and this library has no HTTP/3 client.

#include <cstdint>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>
#include <openssl/ssl.h>

#include "../core/logging.h"
#include "../core/types.h"

namespace simple_http::quic {

// What `QuicCrypto` needs from whatever owns it. A virtual call rather than a
// std::function because it runs inside an OpenSSL callback: no allocation, no
// throw, and nothing that can fail.
//
// The accessor is named `native_conn` rather than `ngtcp2_conn` on purpose — a
// member function sharing a name with the type would hide that type inside the
// deriving class, and `ngtcp2_conn*` appears in half of its declarations.
class ConnectionCryptoBase {
  public:
    ConnectionCryptoBase() = default;
    virtual ~ConnectionCryptoBase() = default;
    ConnectionCryptoBase(const ConnectionCryptoBase&) = delete;
    ConnectionCryptoBase& operator=(const ConnectionCryptoBase&) = delete;

    [[nodiscard]] virtual ngtcp2_conn* native_conn() noexcept = 0;
};

// The OpenSSL glue for one QUIC connection. Owned by the connection, which must
// outlive it — `get_conn` dereferences the owner from inside OpenSSL callbacks.
//
// Not copyable or movable: the `SSL*` holds a pointer to `m_conn_ref`, which
// holds a pointer to this object, so a move would leave both dangling.
class QuicCrypto {
  public:
    // `owner` must be the object that owns this one; it is handed back to
    // ngtcp2 as the connection's `user_data`.
    //
    // The parameter type is the base class rather than `void*` on purpose: it
    // takes the upcast with whatever pointer adjustment the object's layout
    // needs, and `get_conn` then reverses exactly that adjustment. Storing an
    // unadjusted `void*` and casting it back would work only while this base
    // happens to sit at offset zero.
    explicit QuicCrypto(ConnectionCryptoBase* owner) {
        // Process-wide setup for the OpenSSL crypto helper. Documented as
        // optional (it exists to avoid a performance regression), but it is what
        // the reference implementation does before creating any context, and it
        // costs one static initialisation.
        static const bool crypto_initialised = [] {
            (void)ngtcp2_crypto_ossl_init();
            return true;
        }();
        (void)crypto_initialised;

        if (ngtcp2_crypto_ossl_ctx_new(&m_ctx, nullptr) != 0) {
            m_ctx = nullptr;
            SIMPLE_HTTP_ERROR_LOG("quic: ngtcp2_crypto_ossl_ctx_new failed");
            return;
        }
        m_conn_ref.get_conn = &QuicCrypto::get_conn;
        m_conn_ref.user_data = owner;
    }

    ~QuicCrypto() {
        if (SSL* ssl = ngtcp2_crypto_ossl_ctx_get_ssl(m_ctx)) {
            // Clear the back-pointer before the SSL dies: a free is not a
            // callback, but leaving a pointer to a destroyed ref inside a
            // still-reachable SSL object is the kind of thing that only shows up
            // as a crash much later.
            SSL_set_app_data(ssl, nullptr);
            SSL_free(ssl);
        }
        ngtcp2_crypto_ossl_ctx_del(m_ctx);
    }

    QuicCrypto(const QuicCrypto&) = delete;
    QuicCrypto& operator=(const QuicCrypto&) = delete;
    QuicCrypto(QuicCrypto&&) = delete;
    QuicCrypto& operator=(QuicCrypto&&) = delete;

    // Create this connection's SSL from the listener's context and put it in
    // server mode. Returns false if OpenSSL refuses — which at this point means
    // the context itself is unusable, not that the peer did anything.
    [[nodiscard]] bool init(SSL_CTX* ssl_ctx) {
        if (m_ctx == nullptr) return false;

        SSL* ssl = SSL_new(ssl_ctx);
        if (ssl == nullptr) {
            SIMPLE_HTTP_ERROR_LOG("quic: SSL_new failed");
            return false;
        }
        ngtcp2_crypto_ossl_ctx_set_ssl(m_ctx, ssl);
        if (ngtcp2_crypto_ossl_configure_server_session(ssl) != 0) {
            SIMPLE_HTTP_ERROR_LOG("quic: ngtcp2_crypto_ossl_configure_server_session failed");
            return false;
        }
        // Where OpenSSL callbacks find their way back to the ngtcp2 connection.
        SSL_set_app_data(ssl, &m_conn_ref);
        SSL_set_accept_state(ssl);
        return true;
    }

    [[nodiscard]] SSL* ssl() { return ngtcp2_crypto_ossl_ctx_get_ssl(m_ctx); }
    // What the connection must hand to `ngtcp2_conn_set_tls_native_handle`. The
    // crypto helper retrieves its per-connection state with
    // `ngtcp2_conn_get_tls_native_handle2(conn)` and dereferences it without a
    // null check, so a connection that never sets this crashes inside the first
    // `ngtcp2_conn_read_pkt`.
    [[nodiscard]] ngtcp2_crypto_ossl_ctx* native_handle() { return m_ctx; }

  private:
    static ngtcp2_conn* get_conn(ngtcp2_crypto_conn_ref* ref) {
        // `user_data` is the QuicConnection. The connection has to supply
        // `native_conn()` — this class cannot know it.
        return static_cast<ConnectionCryptoBase*>(ref->user_data)->native_conn();
    }

    ngtcp2_crypto_conn_ref m_conn_ref{};
    ngtcp2_crypto_ossl_ctx* m_ctx{nullptr};
};

}  // namespace simple_http::quic
