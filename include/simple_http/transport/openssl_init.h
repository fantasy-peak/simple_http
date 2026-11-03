#pragma once

// One-time OpenSSL initialization for every place that builds an
// asio::ssl::context.
//
// OpenSSL 4 initializes its algorithm/property hash table lazily. If several
// threads call SSL_CTX_new() for the first time concurrently (for example,
// several Client objects constructed on different threads), TSan reports a
// data race in ossl_ht_get. OpenSSL's own init call is not enough: the first
// SSL_CTX construction is what populates the table. Doing that once under
// std::call_once, before any caller can race, makes concurrent context
// creation safe and costs one call_once check on the slow path only.
//
// This is the library-side counterpart of the warm-up an application would
// otherwise have to perform before starting client threads. It does not make
// SSL_CTX a process-wide singleton: a context still carries per-Client TLS
// policy (CA, mTLS, verification, SNI, ALPN, setup hook), so each Client keeps
// its own unless the caller explicitly shares one via
// TlsClientConfig::shared_context.

#include <mutex>

#include <boost/asio/ssl/context.hpp>
#include <openssl/ssl.h>

namespace simple_http::detail {

inline void ensure_openssl_initialized() {
    static std::once_flag once;
    std::call_once(once, [] {
        OPENSSL_init_ssl(0, nullptr);
        // Warm the lazy tables by constructing and destroying one context.
        // `tls_client` is sufficient: the tables are process-wide, and the
        // throwaway context is never used for a connection.
        boost::asio::ssl::context warmup{boost::asio::ssl::context::tls_client};
    });
}

} // namespace simple_http::detail
