#pragma once

// Outbound proxy support for the client layer: proxy-URL parsing, selection
// from explicit configuration or the environment (HTTP_PROXY / HTTPS_PROXY /
// ALL_PROXY / NO_PROXY), and the two tunnel handshakes.
//
// The handshakes are templated on an Asio byte stream (a tcp::socket, or an
// ssl::stream<tcp::socket> when the proxy itself speaks TLS), so the engine
// runs them on the proxy leg before the target's own TLS starts. Nothing here
// owns a transport: the caller keeps the socket and wraps it afterwards.

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio.hpp>

#include "../core/base64.h"
#include "../core/logging.h"
#include "../core/types.h"
#include "../transport/transport.h"
#include "client_config.h"
#include "url.h" // url_valid_port

namespace simple_http::detail {

namespace asio = boost::asio;

// --- proxy URL ---------------------------------------------------------------

// RFC 3986 percent-decoding (used for userinfo). '+' stays a literal plus: this
// is not form encoding. Returns false on a truncated or non-hex escape.
inline bool percent_decode(std::string_view in, std::string &out) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    out.clear();
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '%') {
            out.push_back(in[i]);
            continue;
        }
        if (i + 2 >= in.size())
            return false;
        const int hi = hex(in[i + 1]);
        const int lo = hex(in[i + 2]);
        if (hi < 0 || lo < 0)
            return false;
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
    }
    return true;
}

// A proxy host must be a name or IP literal: no whitespace, no bytes that would
// truncate the CONNECT request line, and nothing that only makes sense in a URL
// around it. A manually built ProxyConfig goes through the same check, so a typo
// fails at the boundary instead of producing a nonsensical request line.
inline bool valid_proxy_host(std::string_view host) {
    if (host.empty())
        return false;
    for (char c : host) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte == 0x7F)
            return false;
        switch (c) {
        case '"':
        case '\'':
        case '<':
        case '>':
        case '@':
        case '/':
        case '\\':
        case '?':
        case '#':
        case '|':
        case '[':
        case ']':
            return false;
        default:
            break;
        }
    }
    return true;
}

// Parses a proxy URL: [http|https|socks5|socks5h]://[user[:password]@]host[:port]
// (a missing scheme means http). A path/query/fragment is accepted and ignored,
// like curl: proxies are addressed by authority only. Any malformed input is
// client_errc::proxy_error - the caller must fail closed rather than fall back
// to a direct connection.
inline std::expected<ProxyConfig, error_code> parse_proxy_url(std::string_view raw) {
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.front())))
        raw.remove_prefix(1);
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back())))
        raw.remove_suffix(1);
    if (raw.empty())
        return std::unexpected{make_error_code(client_errc::proxy_error)};

    std::string_view scheme = "http";
    std::string_view rest = raw;
    if (auto sep = raw.find("://"); sep != std::string_view::npos) {
        scheme = raw.substr(0, sep);
        rest = raw.substr(sep + 3);
    }
    const std::string scheme_lower = ascii_lower_copy(scheme);
    ProxyConfig out;
    if (scheme_lower == "http") {
        out.kind = ProxyKind::Http;
    } else if (scheme_lower == "https") {
        out.kind = ProxyKind::Https;
    } else if (scheme_lower == "socks5" || scheme_lower == "socks5h") {
        // Both schemes resolve the target at the proxy (ATYP=3). The client
        // deliberately performs no DNS lookup for a proxied target.
        out.kind = ProxyKind::Socks5;
    } else {
        return std::unexpected{make_error_code(client_errc::proxy_error)};
    }

    if (auto end = rest.find_first_of("/?#"); end != std::string_view::npos)
        rest = rest.substr(0, end);
    if (rest.empty())
        return std::unexpected{make_error_code(client_errc::proxy_error)};

    std::string_view userinfo;
    if (auto at = rest.rfind('@'); at != std::string_view::npos) {
        userinfo = rest.substr(0, at);
        rest = rest.substr(at + 1);
    }
    if (rest.empty())
        return std::unexpected{make_error_code(client_errc::proxy_error)};

    std::string_view port_str;
    if (rest.front() == '[') {
        const auto close = rest.find(']');
        if (close == std::string_view::npos)
            return std::unexpected{make_error_code(client_errc::proxy_error)};
        out.host = std::string{rest.substr(1, close - 1)};
        std::string_view after = rest.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':')
                return std::unexpected{make_error_code(client_errc::proxy_error)};
            port_str = after.substr(1);
        }
    } else if (auto colon = rest.rfind(':'); colon != std::string_view::npos) {
        out.host = std::string{rest.substr(0, colon)};
        port_str = rest.substr(colon + 1);
    } else {
        out.host = std::string{rest};
    }
    if (!valid_proxy_host(out.host))
        return std::unexpected{make_error_code(client_errc::proxy_error)};
    if (!port_str.empty() || rest.back() == ':') {
        if (!url_valid_port(port_str, out.port))
            return std::unexpected{make_error_code(client_errc::proxy_error)};
    }

    if (!userinfo.empty()) {
        const auto colon = userinfo.find(':');
        const std::string_view user = colon == std::string_view::npos ? userinfo : userinfo.substr(0, colon);
        const std::string_view pass = colon == std::string_view::npos ? std::string_view{} : userinfo.substr(colon + 1);
        if (!percent_decode(user, out.username) || !percent_decode(pass, out.password))
            return std::unexpected{make_error_code(client_errc::proxy_error)};
        // The header is `Basic base64(user ":" password)`, so a colon in the
        // user part cannot be represented and would silently authenticate as a
        // different identity.
        if (out.username.find(':') != std::string::npos || contains_ctl(out.username) || contains_ctl(out.password))
            return std::unexpected{make_error_code(client_errc::proxy_error)};
    }
    return out;
}

inline std::expected<void, error_code> validate_proxy(const ProxyConfig &proxy) {
    if (!valid_proxy_host(proxy.host))
        return std::unexpected{make_error_code(client_errc::proxy_error)};
    return {};
}

// --- NO_PROXY ----------------------------------------------------------------

struct NoProxyRule {
    bool any{false};
    bool ip{false};        // the entry is an IP literal: exact address match
    std::string host;      // lowercase, brackets stripped, no leading dot
    std::uint16_t port{0}; // 0 = any port
};

inline bool parse_ip_literal(std::string_view host) {
    error_code ec;
    (void)asio::ip::make_address(std::string{host}, ec);
    return !ec;
}

// One comma-separated entry. Supported: "*", an exact host, a bare or
// leading-dot domain (both also match its subdomains), host:port, an IPv4/IPv6
// literal, and a bracketed IPv6 with an optional port. CIDR prefixes and glob
// forms are not supported; an unparseable entry simply never matches.
inline std::optional<NoProxyRule> parse_no_proxy_entry(std::string_view entry) {
    while (!entry.empty() && std::isspace(static_cast<unsigned char>(entry.front())))
        entry.remove_prefix(1);
    while (!entry.empty() && std::isspace(static_cast<unsigned char>(entry.back())))
        entry.remove_suffix(1);
    if (entry.empty())
        return std::nullopt;
    if (entry == "*") {
        NoProxyRule rule;
        rule.any = true;
        return rule;
    }

    NoProxyRule rule;
    std::string_view host = entry;
    std::string_view port_str;
    if (host.front() == '[') {
        const auto close = host.find(']');
        if (close == std::string_view::npos)
            return std::nullopt;
        rule.host = ascii_lower_copy(host.substr(1, close - 1));
        std::string_view after = host.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':')
                return std::nullopt;
            port_str = after.substr(1);
        }
    } else if (auto colon = host.rfind(':'); colon != std::string_view::npos && host.find(':') == colon) {
        // Exactly one colon: host:port. A bare IPv6 literal has several, and is
        // left intact.
        rule.host = ascii_lower_copy(host.substr(0, colon));
        port_str = host.substr(colon + 1);
    } else {
        rule.host = ascii_lower_copy(host);
    }
    if (!rule.host.empty() && rule.host.front() == '.')
        rule.host.erase(rule.host.begin());
    if (rule.host.empty())
        return std::nullopt;
    if (!port_str.empty()) {
        if (!url_valid_port(port_str, rule.port))
            return std::nullopt;
    }
    rule.ip = parse_ip_literal(rule.host);
    return rule;
}

inline std::vector<NoProxyRule> parse_no_proxy(std::string_view csv) {
    std::vector<NoProxyRule> rules;
    std::size_t pos = 0;
    while (pos <= csv.size()) {
        const auto comma = csv.find(',', pos);
        const auto piece = csv.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
        if (auto rule = parse_no_proxy_entry(piece))
            rules.push_back(std::move(*rule));
        if (comma == std::string_view::npos)
            break;
        pos = comma + 1;
    }
    return rules;
}

inline std::vector<NoProxyRule> parse_no_proxy(const std::vector<std::string> &entries) {
    std::vector<NoProxyRule> rules;
    for (const auto &entry : entries) {
        if (auto rule = parse_no_proxy_entry(entry))
            rules.push_back(std::move(*rule));
    }
    return rules;
}

// Whether `host` (the target name as written, no brackets) and `port` bypass the
// proxy. Domain entries match the domain itself and any subdomain; IP entries
// match the exact address only.
inline bool no_proxy_matches(const std::vector<NoProxyRule> &rules, std::string_view host, std::uint16_t port) {
    const std::string lowered = ascii_lower_copy(host);
    const bool host_is_ip = parse_ip_literal(lowered);
    for (const auto &rule : rules) {
        if (rule.any)
            return true;
        if (rule.port != 0 && rule.port != port)
            continue;
        if (rule.ip) {
            if (host_is_ip) {
                error_code a;
                error_code b;
                auto lhs = asio::ip::make_address(lowered, a);
                auto rhs = asio::ip::make_address(rule.host, b);
                if (!a && !b && lhs == rhs)
                    return true;
            }
            continue;
        }
        if (host_is_ip)
            continue;
        if (lowered == rule.host)
            return true;
        if (lowered.size() > rule.host.size() &&
            lowered.compare(lowered.size() - rule.host.size(), rule.host.size(), rule.host) == 0 &&
            lowered[lowered.size() - rule.host.size() - 1] == '.') {
            return true;
        }
    }
    return false;
}

// --- selection ---------------------------------------------------------------

// The proxy-related environment, separated from the process so tests can drive
// selection without touching global state.
struct ProxyEnvironment {
    std::string http_proxy;
    std::string https_proxy;
    std::string all_proxy;
    std::string no_proxy;

    static ProxyEnvironment from_environment() {
        ProxyEnvironment env;
        // httpoxy (CVE-2016-5385): a CGI/FastCGI front end turns a client's
        // `Proxy:` header into HTTP_PROXY, so that spelling is not trusted when
        // REQUEST_METHOD is present. The lowercase one is not affected.
        const bool cgi = std::getenv("REQUEST_METHOD") != nullptr;
        env.http_proxy = cgi ? env_get(nullptr, "http_proxy") : env_get("HTTP_PROXY", "http_proxy");
        env.https_proxy = env_get("HTTPS_PROXY", "https_proxy");
        env.all_proxy = env_get("ALL_PROXY", "all_proxy");
        env.no_proxy = env_get("NO_PROXY", "no_proxy");
        return env;
    }

  private:
    static std::string env_get(const char *upper, const char *lower) {
        if (upper != nullptr) {
            if (const char *v = std::getenv(upper); v != nullptr && *v != '\0')
                return std::string{v};
        }
        if (lower != nullptr) {
            if (const char *v = std::getenv(lower); v != nullptr && *v != '\0')
                return std::string{v};
        }
        return {};
    }
};

// Decides whether this target goes through a proxy. Returns nullopt for a direct
// connection, a ProxyConfig for a tunnel, or client_errc::proxy_error when a
// configured proxy URL is malformed (fail closed rather than silently going
// direct).
//
// Precedence: ClientConfig::proxy wins. Otherwise, when proxy_from_env is on,
// HTTPS_PROXY/HTTP_PROXY (by the target's scheme) then ALL_PROXY. NO_PROXY is
// evaluated first: ClientConfig::no_proxy always applies; the environment's
// NO_PROXY is used only for environment-selected proxies and only when the
// config list is empty.
inline std::expected<std::optional<ProxyConfig>, error_code>
select_proxy(const ClientConfig &config, const ClientTarget &target, const ProxyEnvironment &env) {
    const auto config_rules = parse_no_proxy(config.no_proxy);
    if (config.proxy) {
        if (!config_rules.empty() && no_proxy_matches(config_rules, target.host, target.effective_port()))
            return std::optional<ProxyConfig>{};
        if (auto ok = validate_proxy(*config.proxy); !ok)
            return std::unexpected{ok.error()};
        return std::optional<ProxyConfig>{*config.proxy};
    }
    if (!config.proxy_from_env)
        return std::optional<ProxyConfig>{};

    std::vector<NoProxyRule> rules = config_rules;
    if (rules.empty())
        rules = parse_no_proxy(env.no_proxy);
    if (no_proxy_matches(rules, target.host, target.effective_port()))
        return std::optional<ProxyConfig>{};

    const std::string &scheme_specific = target.use_tls ? env.https_proxy : env.http_proxy;
    const std::string &chosen = scheme_specific.empty() ? env.all_proxy : scheme_specific;
    if (chosen.empty())
        return std::optional<ProxyConfig>{};
    auto parsed = parse_proxy_url(chosen);
    if (!parsed)
        return std::unexpected{parsed.error()};
    return std::optional<ProxyConfig>{std::move(*parsed)};
}

inline std::expected<std::optional<ProxyConfig>, error_code> select_proxy(const ClientConfig &config,
                                                                          const ClientTarget &target) {
    return select_proxy(config, target, ProxyEnvironment::from_environment());
}

// --- tunnel handshakes -------------------------------------------------------

template <typename Stream> asio::awaitable<error_code> proxy_write(Stream &stream, std::string_view data) {
    auto [ec, n] = co_await stream_write_all(stream, std::as_bytes(std::span<const char>{data.data(), data.size()}));
    (void)n;
    co_return ec;
}

template <typename Stream> asio::awaitable<error_code> proxy_read_exact(Stream &stream, std::span<std::byte> buf) {
    auto [ec, n] = co_await stream_read_exact(stream, buf);
    (void)n;
    co_return ec;
}

inline std::string build_connect_request(const ProxyConfig &proxy, std::string_view authority) {
    std::string req;
    req.reserve(64 + authority.size() + proxy.username.size() + proxy.password.size());
    req += "CONNECT ";
    req += authority;
    req += " HTTP/1.1\r\nHost: ";
    req += authority;
    req += "\r\n";
    if (!proxy.username.empty()) {
        req += "Proxy-Authorization: Basic ";
        std::string credentials = proxy.username;
        credentials.push_back(':');
        credentials += proxy.password;
        req += base64_encode(credentials);
        req += "\r\n";
    }
    req += "\r\n";
    return req;
}

// Parses the status code of one response head ("HTTP/1.x NNN ...").
inline std::expected<int, error_code> parse_connect_status(std::string_view head) {
    const auto eol = head.find("\r\n");
    if (eol == std::string_view::npos)
        return std::unexpected{make_error_code(client_errc::proxy_error)};
    const std::string_view line = head.substr(0, eol);
    if (!line.starts_with("HTTP/"))
        return std::unexpected{make_error_code(client_errc::proxy_error)};
    const auto space = line.find(' ');
    if (space == std::string_view::npos || line.size() < space + 4)
        return std::unexpected{make_error_code(client_errc::proxy_error)};
    int status = 0;
    for (std::size_t i = 1; i <= 3; ++i) {
        const char c = line[space + i];
        if (c < '0' || c > '9')
            return std::unexpected{make_error_code(client_errc::proxy_error)};
        status = status * 10 + (c - '0');
    }
    if (line.size() > space + 4 && line[space + 4] != ' ')
        return std::unexpected{make_error_code(client_errc::proxy_error)};
    return status;
}

// Reads response heads one byte at a time, so the first tunnel byte after the
// CONNECT response is never swallowed. A proxy head is tiny (typically < 100
// bytes) and this runs once per connection.
template <typename Stream>
asio::awaitable<std::expected<std::string, error_code>> read_http_head(Stream &stream, std::size_t cap) {
    std::string head;
    std::array<std::byte, 1> byte{};
    for (;;) {
        auto [ec, n] = co_await stream_read_some(stream, std::span<std::byte>{byte});
        if (ec)
            co_return std::unexpected{ec};
        if (n == 0)
            co_return std::unexpected{make_error_code(client_errc::proxy_error)};
        head.push_back(static_cast<char>(byte[0]));
        if (head.size() > cap)
            co_return std::unexpected{make_error_code(client_errc::proxy_error)};
        if (head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0)
            co_return head;
    }
}

// Establishes a CONNECT tunnel to `target_authority` over `stream` (plaintext or
// TLS to the proxy). 2xx succeeds; 407 is proxy_auth_failed; anything else is
// proxy_error. Interim 1xx heads are skipped.
//
// The authority is taken by value: a coroutine copies its parameters into the
// frame only when it is *created*, but the frame may not be awaited until after
// the caller's full-expression ends - a string_view into a temporary
// target.authority() would already be dangling there (measured: a truncated
// authority produced a malformed CONNECT).
template <typename Stream>
asio::awaitable<error_code> http_connect(Stream &stream, const ProxyConfig &proxy, std::string target_authority) {
    if (target_authority.empty() || contains_ctl(target_authority))
        co_return make_error_code(client_errc::proxy_error);
    if (auto ec = co_await proxy_write(stream, build_connect_request(proxy, target_authority)); ec) {
        SIMPLE_HTTP_ERROR_LOG("client proxy: CONNECT write failed: {}", ec.message());
        co_return ec;
    }
    for (;;) {
        auto head = co_await read_http_head(stream, 16 * 1024);
        if (!head) {
            SIMPLE_HTTP_ERROR_LOG("client proxy: CONNECT response read failed: {}", head.error().message());
            co_return head.error();
        }
        auto status = parse_connect_status(*head);
        if (!status) {
            SIMPLE_HTTP_ERROR_LOG("client proxy: malformed CONNECT response: {}", head->substr(0, 120));
            co_return status.error();
        }
        if (*status >= 100 && *status < 200)
            continue; // interim response; the real one follows
        if (*status >= 200 && *status < 300)
            co_return error_code{};
        if (*status == 407) {
            SIMPLE_HTTP_ERROR_LOG("client proxy: CONNECT {} answered 407", target_authority);
            co_return make_error_code(client_errc::proxy_auth_failed);
        }
        SIMPLE_HTTP_ERROR_LOG("client proxy: CONNECT {} answered {}", target_authority, *status);
        co_return make_error_code(client_errc::proxy_error);
    }
}

inline void append_u16_be(std::string &out, std::uint16_t value) {
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

// SOCKS5 CONNECT (RFC 1928) with the optional username/password method
// (RFC 1929). The target is always handed over as written: a name goes out as
// ATYP=3 (the proxy resolves it), an IP literal as ATYP=1/4.
template <typename Stream>
asio::awaitable<error_code> socks5_connect(Stream &stream, const ProxyConfig &proxy, std::string host,
                                           std::uint16_t port) {
    const bool use_auth = !proxy.username.empty();
    if (proxy.username.size() > 255 || proxy.password.size() > 255)
        co_return make_error_code(client_errc::proxy_auth_failed);

    std::string greeting;
    greeting.push_back(0x05);
    greeting.push_back(static_cast<char>(use_auth ? 2 : 1));
    greeting.push_back(0x00);
    if (use_auth)
        greeting.push_back(0x02);
    if (auto ec = co_await proxy_write(stream, greeting); ec)
        co_return ec;

    std::array<std::byte, 2> selection{};
    if (auto ec = co_await proxy_read_exact(stream, selection); ec)
        co_return ec;
    if (static_cast<unsigned char>(selection[0]) != 0x05)
        co_return make_error_code(client_errc::proxy_error);
    const auto method = static_cast<unsigned char>(selection[1]);
    if (method == 0xFF)
        co_return make_error_code(client_errc::proxy_auth_failed);
    if (method == 0x02) {
        if (!use_auth)
            co_return make_error_code(client_errc::proxy_auth_failed);
        std::string auth;
        auth.push_back(0x01);
        auth.push_back(static_cast<char>(proxy.username.size()));
        auth += proxy.username;
        auth.push_back(static_cast<char>(proxy.password.size()));
        auth += proxy.password;
        if (auto ec = co_await proxy_write(stream, auth); ec)
            co_return ec;
        std::array<std::byte, 2> reply{};
        if (auto ec = co_await proxy_read_exact(stream, reply); ec)
            co_return ec;
        if (static_cast<unsigned char>(reply[0]) != 0x01)
            co_return make_error_code(client_errc::proxy_error);
        if (static_cast<unsigned char>(reply[1]) != 0x00) {
            SIMPLE_HTTP_ERROR_LOG("client proxy: SOCKS5 authentication rejected");
            co_return make_error_code(client_errc::proxy_auth_failed);
        }
    } else if (method != 0x00) {
        co_return make_error_code(client_errc::proxy_error); // a method we never offered
    }

    std::string request;
    request.push_back(0x05); // version
    request.push_back(0x01); // CONNECT
    request.push_back(0x00); // reserved
    error_code addr_ec;
    const auto addr = asio::ip::make_address(std::string{host}, addr_ec);
    if (!addr_ec && addr.is_v4()) {
        request.push_back(0x01);
        const auto bytes = addr.to_v4().to_bytes();
        request.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    } else if (!addr_ec && addr.is_v6()) {
        request.push_back(0x04);
        const auto bytes = addr.to_v6().to_bytes();
        request.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    } else {
        if (host.empty() || host.size() > 255 || contains_ctl(host))
            co_return make_error_code(client_errc::proxy_error);
        request.push_back(0x03);
        request.push_back(static_cast<char>(host.size()));
        request.append(host);
    }
    append_u16_be(request, port);
    if (auto ec = co_await proxy_write(stream, request); ec)
        co_return ec;

    std::array<std::byte, 4> reply{};
    if (auto ec = co_await proxy_read_exact(stream, reply); ec)
        co_return ec;
    if (static_cast<unsigned char>(reply[0]) != 0x05)
        co_return make_error_code(client_errc::proxy_error);
    const auto rep = static_cast<unsigned char>(reply[1]);
    const auto atyp = static_cast<unsigned char>(reply[3]);
    if (rep != 0x00) {
        SIMPLE_HTTP_ERROR_LOG("client proxy: SOCKS5 CONNECT {}:{} rejected (rep={})", host, port,
                              static_cast<unsigned>(rep));
        co_return make_error_code(client_errc::proxy_error);
    }
    // Consume the bound address + port so the stream is positioned at the
    // tunnel's first byte. The reply's own ATYP decides its length.
    std::size_t address_len = 0;
    if (atyp == 0x01) {
        address_len = 4;
    } else if (atyp == 0x04) {
        address_len = 16;
    } else if (atyp == 0x03) {
        std::array<std::byte, 1> len{};
        if (auto ec = co_await proxy_read_exact(stream, len); ec)
            co_return ec;
        address_len = static_cast<unsigned char>(len[0]);
    } else {
        co_return make_error_code(client_errc::proxy_error);
    }
    std::vector<std::byte> discard(address_len + 2);
    if (!discard.empty()) {
        if (auto ec = co_await proxy_read_exact(stream, discard); ec)
            co_return ec;
    }
    co_return error_code{};
}

} // namespace simple_http::detail
