#pragma once

// Client-side TLS for the outbound HTTP client: the ssl::context builder and the
// client handshake.
//
// The byte stream itself is the server's TlsStreamTransport — a TLS connection
// is symmetric once the handshake is done, and the transport's async_read/
// async_write/alpn_selected/tls_handle are all role-agnostic. Only the handshake
// differs (asio::ssl::stream_base::client instead of ::server, plus SNI, ALPN
// and host-name verification on our side), so that is all this header adds; the
// server's own handshake() is left untouched.
//
// Configuration errors (unreadable CA file, unloadable client certificate) are
// reported by throwing std::runtime_error, matching the server's TlsContext —
// they are programming/configuration faults, not per-request failures.

#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "../core/logging.h"
#include "../transport/tls_transport.h"
#include "client_config.h"

namespace simple_http {

namespace asio = boost::asio;

// Whether `host` is an IP literal (an IPv4/IPv6 address rather than a name).
// Certificate matching for an IP goes through X509_VERIFY_PARAM_set1_ip_asc,
// not set1_host, so the two cases must be told apart.
inline bool is_ip_literal(std::string_view host) {
    error_code ec;
    (void)asio::ip::make_address(std::string{host}, ec);
    return !ec;
}

// The ALPN protocol list to offer, in TLS wire format (length-prefixed).
// TlsClientConfig::alpn overrides the policy-derived default.
inline std::vector<unsigned char> alpn_wire_list(const ClientTarget& target, const TlsClientConfig& cfg) {
    std::vector<std::string> protos = cfg.alpn;
    if (protos.empty()) {
        switch (target.version) {
            case HttpVersionPolicy::Http11:
                protos.emplace_back("http/1.1");
                break;
            case HttpVersionPolicy::Http2:
                protos.emplace_back("h2");
                break;
            case HttpVersionPolicy::Auto:
            default:
                // h2 first: the server picks the first protocol it supports.
                protos.emplace_back("h2");
                protos.emplace_back("http/1.1");
                break;
        }
    }
    std::vector<unsigned char> wire;
    for (const auto& proto : protos) {
        if (proto.empty() || proto.size() > 255) {
            throw std::runtime_error(std::format("TLS ALPN protocol name out of range: '{}'", proto));
        }
        wire.push_back(static_cast<unsigned char>(proto.size()));
        wire.insert(wire.end(), proto.begin(), proto.end());
    }
    return wire;
}

// Builds and configures the client-side ssl::context: verification, CA bundle,
// optional client certificate (mTLS) and the TLS floor.
inline std::shared_ptr<asio::ssl::context> make_client_ssl_context(const TlsClientConfig& cfg) {
    auto ctx = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_client);

    if (cfg.setup) {
        cfg.setup(*ctx);  // user customizations first; the fields below still apply
    }

    std::uint64_t options =
        asio::ssl::context::default_workarounds | asio::ssl::context::no_tlsv1 | asio::ssl::context::no_tlsv1_1;
    if (cfg.min_version == TlsMinVersion::Tls13) {
        options |= asio::ssl::context::no_tlsv1_2;
    }
    ctx->set_options(options);

    ctx->set_verify_mode(cfg.verify_peer ? asio::ssl::verify_peer : asio::ssl::verify_none);
    if (cfg.verify_peer) {
        if (!cfg.ca_file.empty()) {
            error_code ec;
            ctx->load_verify_file(cfg.ca_file, ec);
            if (ec) {
                throw std::runtime_error(std::format("client CA file '{}': {}", cfg.ca_file, ec.message()));
            }
        } else {
            error_code ec;
            ctx->set_default_verify_paths(ec);  // best effort: no store means the handshake fails later
            if (ec) {
                SIMPLE_HTTP_ERROR_LOG("client TLS: no default verify paths: {}", ec.message());
            }
        }
    }

    if (!cfg.cert_chain_file.empty() || !cfg.private_key_file.empty()) {
        if (cfg.cert_chain_file.empty() || cfg.private_key_file.empty()) {
            throw std::runtime_error("mutual TLS needs both a client certificate and a private key");
        }
        error_code ec;
        ctx->use_certificate_chain_file(cfg.cert_chain_file, ec);
        if (ec) {
            throw std::runtime_error(std::format("client certificate '{}': {}", cfg.cert_chain_file, ec.message()));
        }
        ctx->use_private_key_file(cfg.private_key_file, asio::ssl::context::pem, ec);
        if (ec) {
            throw std::runtime_error(std::format("client private key '{}': {}", cfg.private_key_file, ec.message()));
        }
    }

    return ctx;
}

// What to negotiate in one handshake: the name to send as SNI / verify against,
// whether to verify it at all, and the ALPN list to offer (pre-encoded in TLS
// wire format by alpn_wire_list()).
struct ClientTlsHandshake {
    std::string sni;
    bool verify_host{true};
    std::vector<unsigned char> alpn_wire;
};

// Performs the client-side TLS handshake on a ready transport: SNI, ALPN and
// (optionally) host-name verification are set here, then asio's client
// handshake runs. Returns the transport's error_code; a verification failure
// arrives as its own error (e.g. asio::error::certificate_verify_failed).
inline asio::awaitable<error_code> tls_client_handshake(TlsStreamTransport& transport, const ClientTlsHandshake& hs) {
    SSL* ssl = transport.stream().native_handle();

    // SNI: what the peer should match its certificate against.
    if (!hs.sni.empty() && !is_ip_literal(hs.sni)) {
        (void)SSL_set_tlsext_host_name(ssl, std::string{hs.sni}.c_str());
    }

    if (hs.verify_host) {
        X509_VERIFY_PARAM* param = SSL_get0_param(ssl);
        X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        if (is_ip_literal(hs.sni)) {
            if (X509_VERIFY_PARAM_set1_ip_asc(param, std::string{hs.sni}.c_str()) != 1) {
                co_return make_error_code(asio::error::invalid_argument);
            }
        } else if (!hs.sni.empty()) {
            if (X509_VERIFY_PARAM_set1_host(param, hs.sni.data(), hs.sni.size()) != 1) {
                co_return make_error_code(asio::error::invalid_argument);
            }
        }
    }

    if (!hs.alpn_wire.empty()) {
        // SSL_set_alpn_protos copies the list.
        if (SSL_set_alpn_protos(ssl, hs.alpn_wire.data(), static_cast<unsigned int>(hs.alpn_wire.size())) != 0) {
            co_return make_error_code(asio::error::invalid_argument);
        }
    }

    auto [ec] = co_await transport.stream().async_handshake(asio::ssl::stream_base::client,
                                                            asio::as_tuple(asio::use_awaitable));
    co_return ec;
}

}  // namespace simple_http
