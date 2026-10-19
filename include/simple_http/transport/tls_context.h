#pragma once

// TlsContext: builds and owns the server-side asio::ssl::context.
//
// Responsibilities: load the server certificate chain and private key, optional
// mutual-TLS peer verification, TLS protocol floor, and ALPN advertisement
// (h2 + http/1.1; h3 lives on the QUIC listener's own context). A user hook may
// customize the ssl::context before certificates are applied.

#include <openssl/ssl.h>

#include <boost/asio/ssl.hpp>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "../core/logging.h"
#include "../core/types.h" // error_code: used below, and this header must stand alone

namespace simple_http {

namespace asio = boost::asio;

struct TlsConfig {
    std::string cert_chain_file;                     // server certificate chain (PEM)
    std::string private_key_file;                    // server private key (PEM)
    bool mutual = false;                             // require & verify a client certificate
    std::optional<std::string> ca_file;              // CA bundle for mutual TLS (else system defaults)
    std::function<void(asio::ssl::context &)> setup; // optional customization hook
};

// The ALPN protocol list a TCP-TLS listener advertises, in TLS wire format
// (length-prefixed). h2 is preferred over http/1.1.
//
// h3 is deliberately absent, even when HTTP/3 is compiled in: `h3` means QUIC,
// and a client that selected it here would have negotiated a protocol this
// connection cannot speak. HTTP/3 has its own ALPN list and its own context
// (quic/tls.h).
inline const std::vector<unsigned char> &alpn_wire_list() {
    static const std::vector<unsigned char> list{0x02, 'h', '2', 0x08, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    return list;
}

// TLS wire format for an explicit protocol list, in preference order. An
// empty span means "use alpn_wire_list()" (the historical h2 + http/1.1 set).
inline std::vector<unsigned char> make_alpn_wire_list(std::span<const std::string_view> protocols) {
    std::vector<unsigned char> out;
    std::size_t bytes = 0;
    for (auto protocol : protocols)
        bytes += 1 + protocol.size();
    out.reserve(bytes);
    for (auto protocol : protocols) {
        if (protocol.empty() || protocol.size() > 255)
            continue;
        out.push_back(static_cast<unsigned char>(protocol.size()));
        out.insert(out.end(), protocol.begin(), protocol.end());
    }
    return out;
}

class TlsContext {
  public:
    // `alpn` overrides the advertised protocol list. The listener policy
    // (`TlsHttp`) is translated here, so a listener configured as
    // HTTP/1.1-only really offers http/1.1 and no h2.
    explicit TlsContext(const TlsConfig &cfg, std::span<const std::string_view> alpn = {})
        : m_ctx(asio::ssl::context::tlsv13_server) {
        configure(cfg, alpn);
    }

    asio::ssl::context &context() { return m_ctx; }

  private:
    static bool is_regular_file(const std::filesystem::path &path) {
        std::error_code ec;
        return std::filesystem::is_regular_file(path, ec) && !ec;
    }

    void configure(const TlsConfig &cfg, std::span<const std::string_view> alpn) {
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

        install_alpn(alpn);
    }

    void install_alpn(std::span<const std::string_view> alpn) {
        m_alpn = alpn.empty() ? alpn_wire_list() : make_alpn_wire_list(alpn);
        SSL_CTX_set_alpn_select_cb(
            m_ctx.native_handle(),
            [](SSL *, const unsigned char **out, unsigned char *outlen, const unsigned char *in, unsigned int inlen,
               void *arg) -> int {
                const auto &list = *static_cast<const std::vector<unsigned char> *>(arg);
                if (SSL_select_next_proto(const_cast<unsigned char **>(out), outlen, list.data(),
                                          static_cast<unsigned int>(list.size()), in,
                                          inlen) != OPENSSL_NPN_NEGOTIATED) {
                    return SSL_TLSEXT_ERR_NOACK;
                }
                return SSL_TLSEXT_ERR_OK;
            },
            &m_alpn);
    }

    // Declared before the context it is registered with: the ALPN callback
    // holds a pointer to it, and members are destroyed in reverse order.
    std::vector<unsigned char> m_alpn;
    asio::ssl::context m_ctx;
};

} // namespace simple_http
