#pragma once

// TlsContext: builds and owns the server-side asio::ssl::context.
//
// Responsibilities: load the server certificate chain and private key, optional
// mutual-TLS peer verification, TLS protocol floor, and ALPN advertisement
// (h2 + http/1.1, and h3 when HTTP/3 is enabled). A user hook may customize the
// ssl::context before certificates are applied.

#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <boost/asio/ssl.hpp>
#include <openssl/ssl.h>

#include "../core/logging.h"

namespace simple_http {

namespace asio = boost::asio;

struct TlsConfig {
    std::string cert_chain_file;                    // server certificate chain (PEM)
    std::string private_key_file;                   // server private key (PEM)
    bool mutual = false;                            // require & verify a client certificate
    std::optional<std::string> ca_file;             // CA bundle for mutual TLS (else system defaults)
    std::function<void(asio::ssl::context&)> setup; // optional customization hook
};

// The ALPN protocol list advertised by the server, in TLS wire format
// (length-prefixed). h2 is preferred over http/1.1.
inline const std::vector<unsigned char>& alpn_wire_list() {
    static const std::vector<unsigned char> list = [] {
        std::vector<unsigned char> v{0x02, 'h', '2', 0x08, 'h', 't', 't', 'p', '/', '1', '.', '1'};
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
        // Advertise h3 as well when HTTP/3 support is compiled in.
        v.insert(v.begin(), {0x02, 'h', '3'});
#endif
        return v;
    }();
    return list;
}

class TlsContext {
  public:
    explicit TlsContext(const TlsConfig& cfg) : m_ctx(asio::ssl::context::tlsv13_server) {
        configure(cfg);
    }

    asio::ssl::context& context() { return m_ctx; }

  private:
    static bool is_regular_file(const std::filesystem::path& path) {
        std::error_code ec;
        return std::filesystem::is_regular_file(path, ec) && !ec;
    }

    void configure(const TlsConfig& cfg) {
        if (!is_regular_file(cfg.cert_chain_file)) {
            throw std::runtime_error(std::format("TLS certificate not found: {}", cfg.cert_chain_file));
        }
        if (!is_regular_file(cfg.private_key_file)) {
            throw std::runtime_error(std::format("TLS private key not found: {}", cfg.private_key_file));
        }

        // TLS 1.3 only, expressed by the context flavour rather than by stacking
        // no_tlsv1* option flags. `tlsv13_server` pins both bounds, which is the
        // entire policy — and there is deliberately no knob to lower it: the
        // previous one could not actually re-enable TLS 1.2 (removing an option
        // flag re-enables nothing once the flavour has pinned the version), so it
        // only ever produced silent handshake failures.
        m_ctx.set_options(asio::ssl::context::default_workarounds);

        // The hook customizes the context; it does not get to replace the
        // security policy. Running it first and applying the policy after means
        // a caller adding a cipher list cannot silently lose client-certificate
        // verification — which is exactly what happened when the hook was the
        // whole branch instead of an addition to it. To customize verification
        // itself, use set_verify_callback.
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
        if (ec) {
            throw std::runtime_error(std::format("use_certificate_chain_file: {}", ec.message()));
        }
        m_ctx.use_private_key_file(cfg.private_key_file, asio::ssl::context::pem, ec);
        if (ec) {
            throw std::runtime_error(std::format("use_private_key_file: {}", ec.message()));
        }

        install_alpn();
    }

    void install_alpn() {
        SSL_CTX_set_alpn_select_cb(
            m_ctx.native_handle(),
            [](SSL*, const unsigned char** out, unsigned char* outlen, const unsigned char* in, unsigned int inlen,
               void*) -> int {
                const auto& list = alpn_wire_list();
                if (SSL_select_next_proto(const_cast<unsigned char**>(out), outlen, list.data(),
                                          static_cast<unsigned int>(list.size()), in,
                                          inlen) != OPENSSL_NPN_NEGOTIATED) {
                    return SSL_TLSEXT_ERR_NOACK;
                }
                return SSL_TLSEXT_ERR_OK;
            },
            nullptr);
    }

    asio::ssl::context m_ctx;
};

}  // namespace simple_http
