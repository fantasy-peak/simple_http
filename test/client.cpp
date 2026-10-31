// The client layer's exercise program: one binary that starts a server in the
// same process (both listeners the library uses), then drives the client at it
// and at a hand-rolled HTTP/1.x responder for the cases a well-behaved server
// will not produce.
//
// Run it from the repository root (the TLS test certificates are relative):
//
//   xmake run client
//
// Every check prints PASS/FAIL and the process exits non-zero if any failed, so
// it works as a gate as well as a demonstration.

#include <simple_http.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace asio = boost::asio;
namespace sh = simple_http;

namespace client_test {
// A glaze-reflectable body for the .json() builder test. It must have external
// linkage: glaze derives field names from glz::detail::external<T>, which
// clang rejects for types in an anonymous namespace.
struct HttpApiPet {
    std::int64_t id{};
    std::string name;
};
} // namespace client_test

namespace {

int g_failed = 0;
int g_checks = 0;

void check(bool ok, const std::string &what) {
    ++g_checks;
    if (!ok)
        ++g_failed;
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
}

std::string describe(const sh::error_code &ec) { return ec.message(); }

// --- a raw HTTP/1.x responder, for what the real server will not do ----------
//
// It answers every connection with the canned response it is given (optionally
// after reading one request head), and can be told to close right after, which
// is how the stale-kept-connection and EOF-delimited cases are produced.
class FakeServer {
  public:
    FakeServer(asio::io_context &ctx, std::string response, bool close_after = true, bool read_request = true,
               bool split_write = false)
        : m_acceptor(ctx, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)),
          m_response(std::move(response)), m_close_after(close_after), m_read_request(read_request),
          m_split_write(split_write) {
        m_port = m_acceptor.local_endpoint().port();
        accept();
    }

    std::uint16_t port() const { return m_port; }

    std::atomic<int> connections{0};

  private:
    void accept() {
        // A fresh socket per connection: one shared socket would have the
        // coroutines of different connections read and write the same descriptor.
        auto socket = std::make_shared<asio::ip::tcp::socket>(m_acceptor.get_executor());
        m_acceptor.async_accept(*socket, [this, socket](const sh::error_code &ec) {
            if (!ec) {
                ++connections;
                asio::co_spawn(socket->get_executor(), serve(socket), asio::detached);
            }
            accept();
        });
    }

    asio::awaitable<void> serve(std::shared_ptr<asio::ip::tcp::socket> socket) {
        if (m_read_request) {
            // Read whatever the client sends until the blank line (best effort:
            // the tests only need the request to have been consumed).
            std::array<std::byte, 4096> buf{};
            std::string seen;
            for (;;) {
                auto [ec, n] = co_await socket->async_read_some(asio::buffer(buf), asio::as_tuple(asio::use_awaitable));
                if (ec)
                    co_return;
                seen.append(reinterpret_cast<const char *>(buf.data()), n);
                if (seen.find("\r\n\r\n") != std::string::npos)
                    break;
            }
        }
        if (m_split_write) {
            // Deliver it in small pieces: a head that arrives complete is parsed in
            // one go, so only the incremental path can observe a running size check.
            asio::steady_timer timer{socket->get_executor()};
            for (std::size_t off = 0; off < m_response.size(); off += 32) {
                const auto piece = std::string_view{m_response}.substr(off, 32);
                auto [wec, wn] = co_await asio::async_write(*socket, asio::buffer(piece.data(), piece.size()),
                                                            asio::as_tuple(asio::use_awaitable));
                (void)wn;
                if (wec)
                    co_return;
                timer.expires_after(std::chrono::milliseconds(1));
                co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
            }
        } else {
            co_await asio::async_write(*socket, asio::buffer(m_response), asio::as_tuple(asio::use_awaitable));
        }
        if (m_close_after) {
            sh::error_code ec;
            socket->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            socket->close(ec);
            co_return;
        }
        // Stay connected: keep the socket (and the connection) alive until the
        // peer goes away, since returning here would destroy it.
        std::array<std::byte, 1024> discard{};
        for (;;) {
            auto [ec, n] = co_await socket->async_read_some(asio::buffer(discard), asio::as_tuple(asio::use_awaitable));
            (void)n;
            if (ec)
                co_return;
        }
    }

    asio::ip::tcp::acceptor m_acceptor;
    std::string m_response;
    bool m_close_after;
    bool m_read_request;
    bool m_split_write;
    std::uint16_t m_port{0};
};

// The raw responders must outlive the suite that uses them: an acceptor still
// holding a pending accept cannot be destroyed while the test's io_context
// runs.
std::vector<std::shared_ptr<FakeServer>> g_fakes;

FakeServer &make_fake(asio::io_context &ctx, std::string response, bool close_after = true, bool split_write = false) {
    g_fakes.push_back(std::make_shared<FakeServer>(ctx, std::move(response), close_after,
                                                   /*read_request=*/true, split_write));
    return *g_fakes.back();
}

// A peer that drops the connection (after reading the request head, so the
// request is provably "sent" and nothing answered) for the first `fails`
// connections, then answers every later one with `response`. This is how the
// retry policy's transport-loss case is produced deterministically: the first
// attempt(s) see EOF where the response should be, a retry reaches the healthy
// side.
class FlakyPeer {
  public:
    FlakyPeer(asio::io_context &ctx, std::size_t fails, std::string response)
        : m_acceptor(ctx, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)), m_fails(fails),
          m_response(std::move(response)), m_connections(std::make_shared<std::atomic<std::size_t>>(0)) {
        m_port = m_acceptor.local_endpoint().port();
        accept();
    }
    std::uint16_t port() const { return m_port; }
    std::size_t connections() const { return m_connections->load(); }

  private:
    void accept() {
        auto socket = std::make_shared<asio::ip::tcp::socket>(m_acceptor.get_executor());
        m_acceptor.async_accept(*socket, [this, socket](const sh::error_code &ec) {
            if (!ec) {
                asio::co_spawn(socket->get_executor(), serve(socket, m_fails, m_response, m_connections),
                               asio::detached);
            }
            accept();
        });
    }
    // serve() takes everything it needs by copy: it must survive this peer's
    // destruction, which the suites rely on only after every request is done.
    static asio::awaitable<void> serve(std::shared_ptr<asio::ip::tcp::socket> socket, std::size_t fails,
                                       std::string response, std::shared_ptr<std::atomic<std::size_t>> connections) {
        const std::size_t n = connections->fetch_add(1) + 1;
        std::array<std::byte, 4096> buf{};
        auto [ec, got] = co_await socket->async_read_some(asio::buffer(buf), asio::as_tuple(asio::use_awaitable));
        if (ec)
            co_return;
        if (n <= fails) {
            socket->close(); // the client sees EOF where the head should be
            co_return;
        }
        auto [wec, written] =
            co_await asio::async_write(*socket, asio::buffer(response), asio::as_tuple(asio::use_awaitable));
        (void)written;
        (void)wec;
        socket->close();
    }

    asio::ip::tcp::acceptor m_acceptor;
    std::size_t m_fails;
    std::string m_response;
    std::shared_ptr<std::atomic<std::size_t>> m_connections;
    std::uint16_t m_port{0};
};

// Flaky responders must outlive the suite (same reason as g_fakes).
std::vector<std::shared_ptr<FlakyPeer>> g_flaky;

FlakyPeer &make_flaky(asio::io_context &ctx, std::size_t fails, std::string response) {
    g_flaky.push_back(std::make_shared<FlakyPeer>(ctx, fails, std::move(response)));
    return *g_flaky.back();
}

// --- in-process outbound proxies (CONNECT / HTTPS CONNECT / SOCKS5) ----------
//
// These are real TCP servers that speak just enough of each proxy protocol to
// establish the tunnel, then splice bytes. The HTTPS proxy serves the test
// certificate, so the client's nested-TLS path is a real TLS-over-TLS
// connection. `http_status` / `malformed` inject the failure modes the client
// must map to proxy_error / proxy_auth_failed.
enum class TestProxyKind { Http, Https, Socks5 };

class TestProxy {
  public:
    TestProxy(asio::io_context &ctx, TestProxyKind kind, std::string user = {}, std::string pass = {},
              int http_status = 0, bool malformed = false)
        : m_kind(kind), m_user(std::move(user)), m_pass(std::move(pass)), m_status(http_status), m_malformed(malformed),
          m_acceptor(ctx, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)) {
        if (kind == TestProxyKind::Https) {
            m_tls = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_server);
            m_tls->set_options(asio::ssl::context::default_workarounds);
            m_tls->use_certificate_chain_file("./test/tls_certificates/server_cert.pem");
            m_tls->use_private_key_file("./test/tls_certificates/server_key.pem", asio::ssl::context::pem);
        }
        m_port = m_acceptor.local_endpoint().port();
        accept();
    }

    std::uint16_t port() const { return m_port; }
    std::size_t connections() const { return m_connections; }
    std::size_t tunnels() const { return m_tunnels; }
    std::size_t auth_failures() const { return m_auth_failures; }
    const std::string &last_host() const { return m_last_host; }

  private:
    void accept() {
        auto socket = std::make_shared<asio::ip::tcp::socket>(m_acceptor.get_executor());
        m_acceptor.async_accept(*socket, [this, socket](const sh::error_code &ec) {
            if (!ec) {
                ++m_connections;
                if (m_kind == TestProxyKind::Https) {
                    auto tls = std::make_shared<asio::ssl::stream<asio::ip::tcp::socket>>(std::move(*socket), *m_tls);
                    asio::co_spawn(tls->get_executor(), serve_tls(tls), asio::detached);
                } else if (m_kind == TestProxyKind::Socks5) {
                    asio::co_spawn(socket->get_executor(), serve_socks(socket), asio::detached);
                } else {
                    asio::co_spawn(socket->get_executor(), serve_http(socket), asio::detached);
                }
            }
            accept();
        });
    }

    asio::awaitable<void> serve_tls(std::shared_ptr<asio::ssl::stream<asio::ip::tcp::socket>> tls) {
        auto [ec] = co_await tls->async_handshake(asio::ssl::stream_base::server, asio::as_tuple(asio::use_awaitable));
        if (ec)
            co_return;
        co_await serve_http(std::move(tls));
    }

    template <typename StreamPtr> asio::awaitable<void> serve_http(StreamPtr stream_ptr) {
        auto &stream = *stream_ptr;
        auto head = co_await read_head(stream);
        if (head.empty())
            co_return;
        const auto line_end = head.find("\r\n");
        const auto line = head.substr(0, line_end);
        if (!line.starts_with("CONNECT "))
            co_return;
        const auto space = line.find(' ', 8);
        if (space == std::string::npos)
            co_return;
        const std::string authority = line.substr(8, space - 8);

        if (m_malformed) {
            co_await write_str(stream, "this is not an HTTP response\r\n\r\n");
            close(stream);
            co_return;
        }
        if (!m_user.empty()) {
            const std::string wanted = "Proxy-Authorization: Basic " + sh::base64_encode(m_user + ":" + m_pass);
            if (head.find(wanted) == std::string::npos) {
                ++m_auth_failures;
                co_await write_str(stream, "HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 0\r\n\r\n");
                close(stream);
                co_return;
            }
        }
        if (m_status != 0) {
            co_await write_str(stream,
                               "HTTP/1.1 " + std::to_string(m_status) + " Refused\r\nContent-Length: 0\r\n\r\n");
            close(stream);
            co_return;
        }

        auto [host, port] = split_authority(authority);
        m_last_host = host;
        auto upstream = co_await dial(host, port);
        if (!upstream) {
            co_await write_str(stream, "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n");
            close(stream);
            co_return;
        }
        ++m_tunnels;
        co_await write_str(stream, "HTTP/1.1 200 Connection established\r\n\r\n");
        co_await splice(stream, *upstream);
    }

    asio::awaitable<void> serve_socks(std::shared_ptr<asio::ip::tcp::socket> socket_ptr) {
        auto &socket = *socket_ptr;
        std::array<std::byte, 2> greeting{};
        if (co_await read_exact(socket, greeting))
            co_return;
        if (static_cast<unsigned char>(greeting[0]) != 5)
            co_return;
        std::vector<std::byte> offered(static_cast<unsigned char>(greeting[1]));
        if (!offered.empty() && co_await read_exact(socket, offered))
            co_return;

        if (m_malformed) {
            co_await write_str(socket, "garbage");
            close(socket);
            co_return;
        }

        const bool want_auth = !m_user.empty();
        auto offered_method = [&](unsigned char method) {
            return std::find(offered.begin(), offered.end(), std::byte{method}) != offered.end();
        };
        if (want_auth) {
            if (!offered_method(0x02)) {
                ++m_auth_failures;
                const std::array<std::byte, 2> no{std::byte{5}, std::byte{0xFF}};
                (void)co_await write_exact(socket, no);
                close(socket);
                co_return;
            }
            const std::array<std::byte, 2> select{std::byte{5}, std::byte{2}};
            if (co_await write_exact(socket, select))
                co_return;
            std::array<std::byte, 2> ver_len{};
            if (co_await read_exact(socket, ver_len))
                co_return;
            std::string user(static_cast<unsigned char>(ver_len[1]), '\0');
            if (!user.empty()) {
                std::vector<std::byte> bytes(user.size());
                if (co_await read_exact(socket, bytes))
                    co_return;
                user.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
            }
            std::array<std::byte, 1> pass_len{};
            if (co_await read_exact(socket, pass_len))
                co_return;
            std::string pass(static_cast<unsigned char>(pass_len[0]), '\0');
            if (!pass.empty()) {
                std::vector<std::byte> bytes(pass.size());
                if (co_await read_exact(socket, bytes))
                    co_return;
                pass.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
            }
            const bool ok = user == m_user && pass == m_pass;
            const std::array<std::byte, 2> reply{std::byte{1}, std::byte{ok ? 0x00 : 0x01}};
            if (co_await write_exact(socket, reply))
                co_return;
            if (!ok) {
                ++m_auth_failures;
                close(socket);
                co_return;
            }
        } else {
            if (!offered_method(0x00)) {
                const std::array<std::byte, 2> no{std::byte{5}, std::byte{0xFF}};
                (void)co_await write_exact(socket, no);
                close(socket);
                co_return;
            }
            const std::array<std::byte, 2> select{std::byte{5}, std::byte{0}};
            if (co_await write_exact(socket, select))
                co_return;
        }

        std::array<std::byte, 4> request{};
        if (co_await read_exact(socket, request))
            co_return;
        if (static_cast<unsigned char>(request[0]) != 5 || static_cast<unsigned char>(request[1]) != 1)
            co_return;
        const auto atyp = static_cast<unsigned char>(request[3]);
        std::string host;
        if (atyp == 0x01) {
            std::array<std::byte, 4> addr{};
            if (co_await read_exact(socket, addr))
                co_return;
            asio::ip::address_v4::bytes_type bytes{};
            std::memcpy(bytes.data(), addr.data(), addr.size());
            host = asio::ip::make_address_v4(bytes).to_string();
        } else if (atyp == 0x04) {
            std::array<std::byte, 16> addr{};
            if (co_await read_exact(socket, addr))
                co_return;
            asio::ip::address_v6::bytes_type bytes{};
            std::memcpy(bytes.data(), addr.data(), addr.size());
            host = asio::ip::make_address_v6(bytes).to_string();
        } else if (atyp == 0x03) {
            std::array<std::byte, 1> len{};
            if (co_await read_exact(socket, len))
                co_return;
            std::vector<std::byte> addr(static_cast<unsigned char>(len[0]));
            if (!addr.empty() && co_await read_exact(socket, addr))
                co_return;
            host.assign(reinterpret_cast<const char *>(addr.data()), addr.size());
        } else {
            co_return;
        }
        std::array<std::byte, 2> port_bytes{};
        if (co_await read_exact(socket, port_bytes))
            co_return;
        const auto port = static_cast<std::uint16_t>((static_cast<unsigned char>(port_bytes[0]) << 8) |
                                                     static_cast<unsigned char>(port_bytes[1]));
        m_last_host = host;

        if (m_status != 0) {
            const std::array<std::byte, 10> reply{
                std::byte{5}, static_cast<std::byte>(m_status), std::byte{0}, std::byte{1}, {}, {}, {}, {}, {}, {}};
            (void)co_await write_exact(socket, reply);
            close(socket);
            co_return;
        }
        auto upstream = co_await dial(host, port);
        if (!upstream) {
            const std::array<std::byte, 10> reply{std::byte{5}, std::byte{4}, std::byte{0}, std::byte{1}, {},
                                                  {},           {},           {},           {},           {}};
            (void)co_await write_exact(socket, reply);
            close(socket);
            co_return;
        }
        ++m_tunnels;
        const std::array<std::byte, 10> reply{std::byte{5}, std::byte{0}, std::byte{0}, std::byte{1}, {},
                                              {},           {},           {},           {},           {}};
        if (co_await write_exact(socket, reply))
            co_return;
        co_await splice(socket, *upstream);
    }

    template <typename Stream> asio::awaitable<void> splice(Stream &client, asio::ip::tcp::socket &upstream) {
        using namespace asio::experimental::awaitable_operators;
        auto down = [&]() -> asio::awaitable<void> {
            std::array<std::byte, 16384> buf{};
            for (;;) {
                auto [ec, n] = co_await client.async_read_some(asio::buffer(buf), asio::as_tuple(asio::use_awaitable));
                if (ec)
                    break;
                auto [wec, wn] = co_await asio::async_write(upstream, asio::buffer(buf.data(), n),
                                                            asio::as_tuple(asio::use_awaitable));
                (void)wn;
                if (wec)
                    break;
            }
        };
        auto up = [&]() -> asio::awaitable<void> {
            std::array<std::byte, 16384> buf{};
            for (;;) {
                auto [ec, n] =
                    co_await upstream.async_read_some(asio::buffer(buf), asio::as_tuple(asio::use_awaitable));
                if (ec)
                    break;
                auto [wec, wn] = co_await asio::async_write(client, asio::buffer(buf.data(), n),
                                                            asio::as_tuple(asio::use_awaitable));
                (void)wn;
                if (wec)
                    break;
            }
        };
        co_await (down() || up());
        close(client);
        sh::error_code ec;
        upstream.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        upstream.close(ec);
    }

    asio::awaitable<std::optional<asio::ip::tcp::socket>> dial(const std::string &host, std::uint16_t port) {
        asio::ip::tcp::socket upstream{m_acceptor.get_executor()};
        asio::ip::tcp::resolver resolver{m_acceptor.get_executor()};
        auto [ec, results] =
            co_await resolver.async_resolve(host, std::to_string(port), asio::as_tuple(asio::use_awaitable));
        if (ec)
            co_return std::nullopt;
        auto [cec, endpoint] = co_await asio::async_connect(upstream, results, asio::as_tuple(asio::use_awaitable));
        (void)endpoint;
        if (cec)
            co_return std::nullopt;
        co_return std::optional<asio::ip::tcp::socket>{std::move(upstream)};
    }

    static std::pair<std::string, std::uint16_t> split_authority(std::string_view authority) {
        std::string host;
        std::string_view port_str;
        if (!authority.empty() && authority.front() == '[') {
            const auto close = authority.find(']');
            if (close == std::string_view::npos)
                return {};
            host = std::string{authority.substr(1, close - 1)};
            if (close + 1 < authority.size() && authority[close + 1] == ':')
                port_str = authority.substr(close + 2);
        } else if (const auto colon = authority.rfind(':'); colon != std::string_view::npos) {
            host = std::string{authority.substr(0, colon)};
            port_str = authority.substr(colon + 1);
        } else {
            return {};
        }
        if (port_str.empty())
            return {};
        return {host, static_cast<std::uint16_t>(std::stoul(std::string{port_str}))};
    }

    template <typename Stream> static asio::awaitable<std::string> read_head(Stream &stream) {
        std::string head;
        std::array<std::byte, 4096> buf{};
        while (head.find("\r\n\r\n") == std::string::npos) {
            auto [ec, n] = co_await stream.async_read_some(asio::buffer(buf), asio::as_tuple(asio::use_awaitable));
            if (ec)
                co_return std::string{};
            head.append(reinterpret_cast<const char *>(buf.data()), n);
            if (head.size() > 16 * 1024)
                co_return std::string{};
        }
        co_return head;
    }

    template <typename Stream> static asio::awaitable<void> write_str(Stream &stream, std::string_view data) {
        (void)co_await asio::async_write(stream, asio::buffer(data.data(), data.size()),
                                         asio::as_tuple(asio::use_awaitable));
    }

    template <typename Stream>
    static asio::awaitable<sh::error_code> read_exact(Stream &stream, std::span<std::byte> buf) {
        auto [ec, n] = co_await asio::async_read(stream, asio::buffer(buf.data(), buf.size()),
                                                 asio::as_tuple(asio::use_awaitable));
        (void)n;
        co_return ec;
    }

    template <typename Stream>
    static asio::awaitable<sh::error_code> write_exact(Stream &stream, std::span<const std::byte> buf) {
        auto [ec, n] = co_await asio::async_write(stream, asio::buffer(buf.data(), buf.size()),
                                                  asio::as_tuple(asio::use_awaitable));
        (void)n;
        co_return ec;
    }

    template <typename Stream> static void close(Stream &stream) {
        sh::error_code ec;
        stream.lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        stream.lowest_layer().close(ec);
    }

    TestProxyKind m_kind;
    std::string m_user;
    std::string m_pass;
    int m_status{0};
    bool m_malformed{false};
    std::shared_ptr<asio::ssl::context> m_tls;
    asio::ip::tcp::acceptor m_acceptor;
    std::uint16_t m_port{0};
    std::size_t m_connections{0};
    std::size_t m_tunnels{0};
    std::size_t m_auth_failures{0};
    std::string m_last_host;
};

// Proxies must outlive the suite: an acceptor still holding a pending accept
// cannot be destroyed while the test's io_context runs.
std::vector<std::shared_ptr<TestProxy>> g_proxies;

TestProxy &make_proxy(asio::io_context &ctx, TestProxyKind kind, std::string user = {}, std::string pass = {},
                      int http_status = 0, bool malformed = false) {
    g_proxies.push_back(
        std::make_shared<TestProxy>(ctx, kind, std::move(user), std::move(pass), http_status, malformed));
    return *g_proxies.back();
}

// --- the in-process server ---------------------------------------------------

sh::ServerConfig server_config(std::uint16_t port, std::optional<sh::TlsConfig> tls) {
    sh::ServerConfig cfg;
    cfg.listen = sh::InetAddress{"127.0.0.1", port, false};
    cfg.worker_threads = 2;
    cfg.tls = std::move(tls);
    cfg.limits.idle_timeout = std::chrono::seconds(30);
    return cfg;
}

// Reads `?key=value` out of a request target (the tests' routes take one
// numeric parameter each).
long long query_number(const sh::RequestPtr &req, std::string_view key, long long fallback) {
    const std::string q{req->query()};
    const std::string needle = std::string{key} + "=";
    auto pos = q.find(needle);
    if (pos == std::string::npos)
        return fallback;
    try {
        return std::stoll(q.substr(pos + needle.size()));
    } catch (...) {
        return fallback;
    }
}

void register_routes(sh::Server &server) {
    server.route(sh::any_methods, "/world", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).send(std::string{"hello from "} + std::string{sh::to_string(res->version())});
    });
    // A HEAD/bodyless response that still advertises the Content-Encoding a GET
    // would have used. With auto_decompress on, the client must not install a
    // decoder for a body that does not exist.
    server.route(sh::any_methods, "/gzip-head", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        res->header(sh::field::content_encoding, "gzip");
        co_await res->status(200).send(std::string(32, 'x'));
    });
    server.route(sh::any_methods, "/echo", [](sh::RequestPtr req, sh::ResponsePtr res) -> asio::awaitable<void> {
        auto body = co_await req->body().read_all();
        co_await res->status(200).send(body ? *body : std::string{});
    });
    // Parses the request as multipart/form-data and returns a canonical summary
    // — the round-trip target for MultipartForm (suite_multipart). Shapes:
    //   field:NAME=VALUE
    //   file:NAME:FILENAME:CONTENT-TYPE:BYTES
    server.route(sh::any_methods, "/mp", [](sh::RequestPtr req, sh::ResponsePtr res) -> asio::awaitable<void> {
        auto parts = co_await sh::read_multipart_body(*req);
        if (!parts) {
            co_await res->status(400).send("not multipart");
            co_return;
        }
        std::string out;
        for (const auto &p : *parts) {
            if (!out.empty())
                out.push_back('|');
            if (p.filename.empty()) {
                out += std::string{"field:"} + p.name + "=" + p.data;
            } else {
                out += std::string{"file:"} + p.name + ":" + p.filename + ":" + p.content_type + ":" +
                       std::to_string(p.data.size());
            }
        }
        co_await res->status(200).send(std::move(out));
    });
    // WebSocket echo, for the client-side open_websocket suite (suite_websocket).
    server.ws_route("/wsecho", [](sh::RequestPtr, std::shared_ptr<sh::WebSocket> ws) -> asio::awaitable<void> {
        for (;;) {
            auto msg = co_await ws->read();
            if (!msg)
                break;
            if (auto ec = co_await ws->write(msg->data, msg->text))
                break;
        }
        co_return;
    });
    server.route(sh::any_methods, "/drain", [](sh::RequestPtr req, sh::ResponsePtr res) -> asio::awaitable<void> {
        auto body = co_await req->body().read_all();
        co_await res->status(200).send("received " + std::to_string(body ? body->size() : 0) + " bytes");
    });
    server.route(sh::any_methods, "/big", [](sh::RequestPtr req, sh::ResponsePtr res) -> asio::awaitable<void> {
        const std::size_t n = static_cast<std::size_t>(query_number(req, "n", 65536));
        std::string body(n, 'x');
        for (std::size_t i = 0; i < n; i += 4096)
            body[i] = 'a'; // a pattern to check, cheaply
        co_await res->status(200).send(std::move(body));
    });
    server.route(sh::any_methods, "/empty", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(204).send_bodyless();
    });
    server.route(sh::any_methods, "/delay", [](sh::RequestPtr req, sh::ResponsePtr res) -> asio::awaitable<void> {
        const auto ms = static_cast<int>(query_number(req, "ms", 200));
        asio::steady_timer timer{co_await asio::this_coro::executor};
        timer.expires_after(std::chrono::milliseconds(ms));
        co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        co_await res->status(200).send("delayed");
    });
    server.route(sh::any_methods, "/stream", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        // A streamed response is chunked on HTTP/1.1, so this route exercises
        // the chunked decoder both directly and through the reverse proxy.
        (void)co_await res->status(200).content_type("text/plain").begin();
        (void)co_await res->write("alpha-");
        (void)co_await res->write("beta-");
        (void)co_await res->finish("gamma");
    });
    // A body that ends with a trailer section, for the reverse proxy's
    // trailer-forwarding check (suite_reverse_proxy).
    server.route(sh::any_methods, "/trailers", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        (void)co_await res->status(200).content_type("text/plain").begin();
        (void)co_await res->write("trailer body");
        res->trailer("x-checksum", "abc123");
        (void)co_await res->finish();
    });
    server.route(sh::any_methods, "/headers", [](sh::RequestPtr req, sh::ResponsePtr res) -> asio::awaitable<void> {
        std::string out;
        for (const auto &[name, value] : req->headers()) {
            out.append(name).append(": ").append(value).append("\n");
        }
        co_await res->status(200).send(std::move(out));
    });
    // Echoes the raw method token, so extension methods (PROPFIND, …) can be
    // asserted end to end through the reverse proxy.
    server.route(sh::any_methods, "/method-token",
                 [](sh::RequestPtr req, sh::ResponsePtr res) -> asio::awaitable<void> {
                     co_await res->status(200).send(std::string{req->method_token()});
                 });
    // Echoes the raw query string, for the new API's query() builder assertion.
    server.route(sh::any_methods, "/query-echo", [](sh::RequestPtr req, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).send(std::string{req->query()});
    });
    server.route(sh::any_methods, "/whoami",
                 [](sh::RequestPtr, sh::ResponsePtr res, sh::SslHandle ssl) -> asio::awaitable<void> {
                     std::string who = "no client certificate";
                     if (ssl)
                         if (X509 *cert = SSL_get_peer_certificate(*ssl)) {
                             char name[256] = {};
                             X509_NAME_oneline(X509_get_subject_name(cert), name, sizeof(name));
                             who = name;
                             X509_free(cert);
                         }
                     co_await res->status(200).send(who);
                 });
    server.fallback([](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(404).send("not found");
    });
}

// --- helpers -----------------------------------------------------------------

std::string url(std::uint16_t port, const char *path, bool tls = false) {
    return (tls ? "https://127.0.0.1:" : "http://127.0.0.1:") + std::to_string(port) + path;
}

sh::ClientConfig base_config() {
    sh::ClientConfig cfg;
    cfg.request_timeout = std::chrono::seconds(10);
    cfg.idle_timeout = std::chrono::seconds(20);
    cfg.tls.ca_file = "./test/tls_certificates/ca_cert.pem";
    cfg.tls.cert_chain_file = "./test/tls_certificates/client_cert.pem";
    cfg.tls.private_key_file = "./test/tls_certificates/client_key.pem";
    // The test certificate carries no SAN, so name verification has to go
    // through the CN; the matrix below checks both the positive and the failing
    // case rather than waving verification aside silently.
    cfg.tls.sni_override = "SimpleHttpServer";
    return cfg;
}

// --- the new http:: API: test-side adapter --------------------------------------
//
// The suites below were written against the old client (HttpClient returning a
// fully buffered ClientResponse). The new API (simple_http::http) is the one
// under test, so this adapter reads a buffered http::Response into the old
// assertion shape — status/version/headers/body — keeping hundreds of asserts
// intact while the call sites migrate to RequestBuilder/open_stream.

namespace h = sh;

struct ApiResult {
    int status{0};
    sh::Version version{sh::Version::Http11};
    sh::Headers headers;
    bool bodyless{false};
    std::string body;

    std::optional<std::string_view> header(std::string_view name) const { return headers.get(name); }
};

// One buffered send, shaped like the old ClientResponse for the suites below.
asio::awaitable<std::expected<ApiResult, sh::error_code>> fetch(h::Client &client, h::RequestBuilder builder) {
    auto r = co_await builder.send();
    if (!r)
        co_return std::unexpected{r.error()};
    ApiResult out;
    out.status = r->status();
    out.version = r->version();
    out.headers = r->headers();
    out.bodyless = r->bodyless();
    auto body = co_await r->read_all();
    if (!body)
        co_return std::unexpected{body.error()};
    out.body = *body;
    co_return out;
}

// The reverse proxy, exercised against this process's own listeners.
asio::awaitable<void> suite_reverse_proxy(std::uint16_t plain_port) {
    std::printf("\n== reverse proxy ==\n");
    h::Client http{co_await asio::this_coro::executor, base_config()};
    const std::string front = "http://127.0.0.1:" + std::to_string(plain_port);

    auto plain_backend = co_await fetch(http, http.get(front + "/rp/world"));
    check(plain_backend && plain_backend->status == 200 && plain_backend->body == "hello from HTTP/1.1",
          "proxied request to a plaintext backend stays HTTP/1.1 -> " +
              (plain_backend ? plain_backend->body : describe(plain_backend.error())));

    auto echo = co_await fetch(http, http.post(front + "/rp/echo").body("round tripped"));
    check(echo && echo->body == "round tripped", "proxied POST keeps the body (chunked re-framing)");

    auto empty = co_await fetch(http, http.get(front + "/rp/empty"));
    check(empty && empty->status == 204 && empty->bodyless, "proxied 204 is bodyless");

    auto big = co_await fetch(http, http.get(front + "/rp/big?n=200000"));
    check(big && big->body.size() == 200000, "proxied 200000-byte body streamed intact");

    auto headers = co_await fetch(http, http.get(front + "/rp/headers"));
    check(headers && headers->body.find("x-forwarded-for: 127.0.0.1") != std::string::npos &&
              headers->body.find("x-forwarded-proto: http") != std::string::npos &&
              headers->body.find("host: 127.0.0.1:") != std::string::npos,
          "proxied request carries X-Forwarded-* and the backend's Host -> " +
              (headers ? headers->body : describe(headers.error())));

    {
        // Header hygiene of the forwarded request, matching Go's Rewrite mode:
        // the proxy's own X-Forwarded-Proto/-Host win over a caller's, the
        // caller's X-Forwarded-For is stripped (only the real peer remains),
        // `TE: trailers` is kept (gRPC), and Proxy-Connection is hop-by-hop.
        // Pinned to HTTP/1.1 because those are connection-specific headers an
        // HTTP/2 frontend could not carry.
        auto cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http11;
        cfg.default_h2c = sh::H2cMode::Off;
        h::Client h1_http{co_await asio::this_coro::executor, cfg};
        auto r = co_await fetch(h1_http, h1_http.get(front + "/rp/headers")
                                             .header("x-forwarded-proto", "https")
                                             .header("x-forwarded-host", "evil.example")
                                             .header("x-forwarded-for", "1.1.1.1")
                                             .header("x-forwarded-for", "2.2.2.2")
                                             .header("te", "trailers")
                                             .header("proxy-connection", "keep-alive"));
        const std::string b = r ? r->body : std::string{};
        check(r && b.find("x-forwarded-proto: http") != std::string::npos &&
                  b.find("x-forwarded-proto: https") == std::string::npos,
              "proxy overwrites a caller's X-Forwarded-Proto -> " + b);
        check(r && b.find("x-forwarded-host: 127.0.0.1") != std::string::npos &&
                  b.find("evil.example") == std::string::npos,
              "proxy overwrites a caller's X-Forwarded-Host -> " + b);
        check(r && b.find("x-forwarded-for: 127.0.0.1\n") != std::string::npos &&
                  b.find("1.1.1.1") == std::string::npos && b.find("2.2.2.2") == std::string::npos,
              "proxy strips a caller's X-Forwarded-For by default -> " + b);
        check(r && b.find("te: trailers") != std::string::npos, "proxy preserves TE: trailers -> " + b);
        check(r && b.find("proxy-connection") == std::string::npos, "proxy strips Proxy-Connection -> " + b);

        // Legacy/Director mode: preserve the inbound chain and append the peer.
        auto chain = co_await fetch(h1_http, h1_http.get(front + "/rpxff/headers")
                                                 .header("x-forwarded-for", "1.1.1.1")
                                                 .header("x-forwarded-for", "2.2.2.2"));
        const std::string chain_body = chain ? chain->body : std::string{};
        check(chain && chain_body.find("x-forwarded-for: 1.1.1.1, 2.2.2.2, 127.0.0.1") != std::string::npos,
              "preserve_inbound_forwarding appends to the whole chain -> " + chain_body);
    }

    {
        // Pinned to HTTP/1.1 so the response really is chunked on the wire (over
        // HTTP/2 the same route is DATA frames, which would not exercise the
        // chunked decoder at all).
        auto cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http11;
        cfg.default_h2c = sh::H2cMode::Off;
        h::Client h1_http{co_await asio::this_coro::executor, cfg};
        auto streamed = co_await fetch(h1_http, h1_http.get(front + "/stream"));
        check(streamed && streamed->body == "alpha-beta-gamma",
              "a chunked response decodes directly -> " + (streamed ? streamed->body : describe(streamed.error())));
    }

    auto proxied_stream = co_await fetch(http, http.get(front + "/rp/stream"));
    check(proxied_stream && proxied_stream->body == "alpha-beta-gamma",
          "a chunked backend passes through the proxy intact -> " +
              (proxied_stream ? proxied_stream->body : describe(proxied_stream.error())));

    // Trailer forwarding: the backend ends its body with a trailer section; the
    // proxy must relay the fields. The upstream leg is HTTP/1.1 (chunked), the
    // frontend leg is h2 for the default client and h1 for the pinned one, so
    // both wire shapes are exercised end to end.
    {
        auto s = co_await http.open_stream(front + "/rp/trailers");
        if (!s) {
            check(false, "open_stream /rp/trailers (h2 frontend): " + describe(s.error()));
        } else {
            // open_stream() keeps the write end open (full-duplex), so even a
            // bodyless GET must finish() to send END_STREAM - otherwise the
            // backend-side request never ends.
            (void)co_await s->finish();
            std::string got;
            while (auto c = co_await s->read()) {
                if (c->eof)
                    break;
                got += c->data;
            }
            check(got == "trailer body" && s->trailers().get("x-checksum") == "abc123",
                  "a proxied response's trailers reach an h2 client -> " +
                      std::string{s->trailers().get("x-checksum").value_or("<none>")});
        }
    }
    {
        auto cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http11;
        cfg.default_h2c = sh::H2cMode::Off;
        h::Client h1_http{co_await asio::this_coro::executor, cfg};
        auto s = co_await h1_http.open_stream(front + "/rp/trailers");
        if (!s) {
            check(false, "open_stream /rp/trailers (h1 frontend): " + describe(s.error()));
        } else {
            (void)co_await s->finish(); // see above: the write end must be closed explicitly
            std::string got;
            while (auto c = co_await s->read()) {
                if (c->eof)
                    break;
                got += c->data;
            }
            check(got == "trailer body" && s->trailers().get("x-checksum") == "abc123",
                  "a proxied response's trailers reach an h1 client -> " +
                      std::string{s->trailers().get("x-checksum").value_or("<none>")});
        }
    }

    {
        auto cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http11;
        cfg.default_h2c = sh::H2cMode::Off;
        h::Client h1_http{co_await asio::this_coro::executor, cfg};
        auto host = co_await fetch(h1_http, h1_http.get(front + "/rphost").header("host", "front.example"));
        check(host && host->status == 200 && host->body.find("host: front.example") != std::string::npos,
              "preserve_host forwards the frontend Host -> " + (host ? host->body : describe(host.error())));
    }

    {
        auto hooked = co_await fetch(http, http.get(front + "/rphook"));
        check(hooked && hooked->status == 203 && hooked->header("x-proxy-hook") == "yes" &&
                  hooked->body == "hello from HTTP/1.1",
              "Rewrite and ModifyResponse hooks run -> " +
                  (hooked ? std::to_string(hooked->status) + " " + hooked->body : describe(hooked.error())));
    }

    {
        auto h2c = co_await fetch(http, http.get(front + "/rph2c"));
        check(h2c && h2c->status == 200 && h2c->body == "hello from HTTP/2",
              "h2c prior-knowledge backend -> " + (h2c ? h2c->body : describe(h2c.error())));
    }

    {
        auto cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http11;
        cfg.default_h2c = sh::H2cMode::Off;
        h::Client h1_http{co_await asio::this_coro::executor, cfg};
        auto req = std::make_shared<sh::Request>(sh::Version::Http11, co_await asio::this_coro::executor);
        req->set_method_token("PROPFIND");
        req->set_target("/rpmethod");
        auto sent = co_await h1_http.send(std::move(req), front + "/rpmethod");
        std::string body;
        if (sent) {
            auto text = co_await sent->text();
            if (text)
                body = *text;
        }
        check(sent && sent->status() == 200 && body == "PROPFIND",
              "an extension method survives the proxy -> " +
                  (sent ? std::to_string(sent->status()) + " " + body : describe(sent.error())));
    }

    {
        auto cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http11;
        cfg.default_h2c = sh::H2cMode::Off;
        h::Client h1_http{co_await asio::this_coro::executor, cfg};
        auto connect = co_await fetch(h1_http, h1_http.request(sh::Method::Connect, front + "/rp/world"));
        check(connect && connect->status == 405,
              "CONNECT is refused by the reverse proxy -> " +
                  (connect ? std::to_string(connect->status) : describe(connect.error())));
    }

    auto tls_backend = co_await fetch(http, http.get(front + "/rptls/world"));
    check(tls_backend && tls_backend->status == 200 && tls_backend->body == "hello from HTTP/2",
          "proxied request to a TLS backend negotiates HTTP/2 (ALPN) -> " +
              (tls_backend ? tls_backend->body : describe(tls_backend.error())));

    auto whoami = co_await fetch(http, http.get(front + "/rptls/whoami"));
    check(whoami && whoami->body.find("SimpleHttpClient") != std::string::npos,
          "proxied request presents its client certificate to an mTLS backend");
    co_return;
}

// --- suites ------------------------------------------------------------------

asio::awaitable<void> suite_protocol_matrix(std::uint16_t plain, std::uint16_t tls_port) {
    std::printf("\n== protocol matrix ==\n");

    struct Case {
        const char *name;
        sh::HttpVersionPolicy policy;
        sh::H2cMode h2c;
        bool use_tls;
    };

    const Case cases[] = {
        {"http + HTTP/1.1", sh::HttpVersionPolicy::Http11, sh::H2cMode::Off, false},
        {"http + h2c (prior knowledge)", sh::HttpVersionPolicy::Http2, sh::H2cMode::PriorKnowledge, false},
        {"http + h2c (upgrade)", sh::HttpVersionPolicy::Http2, sh::H2cMode::Upgrade, false},
        {"https + HTTP/1.1 (ALPN pinned)", sh::HttpVersionPolicy::Http11, sh::H2cMode::Off, true},
        {"https + HTTP/2 (ALPN)", sh::HttpVersionPolicy::Http2, sh::H2cMode::Off, true},
    };

    for (const auto &c : cases) {
        auto cfg = base_config();
        cfg.default_version = c.policy;
        cfg.default_h2c = c.h2c;
        h::Client http{co_await asio::this_coro::executor, cfg};
        const std::string target = url(c.use_tls ? tls_port : plain, "/world", c.use_tls);
        auto r = co_await fetch(http, http.get(target));
        const std::string want = c.use_tls && c.policy == sh::HttpVersionPolicy::Http11 ? "HTTP/1.1" : "";
        std::string expected = (c.policy == sh::HttpVersionPolicy::Http2) ? "hello from HTTP/2" : "hello from HTTP/1.1";
        check(r && r->status == 200 && r->body == expected,
              std::string{c.name} + " -> " + (r ? r->body : describe(r.error())));
        (void)want;
    }

    // Auto policy: the whole point of the layer — it must pick what the peer
    // offers (ALPN on TLS, h2c upgrade on plaintext) without being told.
    {
        auto cfg = base_config();
        h::Client http{co_await asio::this_coro::executor, cfg};
        auto plain_r = co_await fetch(http, http.get(url(plain, "/world")));
        check(plain_r && plain_r->version == sh::Version::Http2, "Auto over http negotiates h2c (upgrade)");
        auto tls_r = co_await fetch(http, http.get(url(tls_port, "/world", true)));
        check(tls_r && tls_r->version == sh::Version::Http2, "Auto over https negotiates h2 via ALPN");
    }
    {
        // The same client, but told never to use plaintext h2: it must stay 1.1.
        auto cfg = base_config();
        cfg.default_h2c = sh::H2cMode::Off;
        h::Client http{co_await asio::this_coro::executor, cfg};
        auto r = co_await fetch(http, http.get(url(plain, "/world")));
        check(r && r->version == sh::Version::Http11, "Auto with H2cMode::Off stays HTTP/1.1");
    }
    {
        // Per-request policy: the client default stays Auto, but one request can
        // pin HTTP/1.1 (and so gets its own kept connection, because the policy
        // is part of the connection key).
        h::Client http{co_await asio::this_coro::executor, base_config()};
        auto auto_r = co_await fetch(http, http.get(url(plain, "/world")));
        check(auto_r && auto_r->version == sh::Version::Http2, "the default Auto still negotiates h2c");
        auto h1_r = co_await fetch(http, http.get(url(plain, "/world")).version(sh::HttpVersionPolicy::Http11));
        check(h1_r && h1_r->version == sh::Version::Http11 && h1_r->body == "hello from HTTP/1.1",
              "a per-request Http11 policy overrides the client default");
        auto stream =
            co_await http.open_stream(url(plain, "/world"), sh::StreamSpec{.version = sh::HttpVersionPolicy::Http11});
        check(stream.has_value(), stream ? "per-request policy on the stream path opens"
                                         : "per-request policy on the stream path opens: " + describe(stream.error()));
        if (stream) {
            check(stream->version() == sh::Version::Http11, "the explicit stream honours the per-request policy");
            check(!co_await stream->finish(), "finish the per-request stream");
            auto body = co_await stream->read_all();
            check(body && *body == "hello from HTTP/1.1", "the per-request stream body is correct");
        }
    }
    co_return;
}

asio::awaitable<void> suite_framing(std::uint16_t plain, asio::io_context &ctx) {
    std::printf("\n== framing ==\n");
    h::Client http{co_await asio::this_coro::executor, base_config()};

    auto echo = co_await fetch(http, http.post(url(plain, "/echo")).body("round trip"));
    check(echo && echo->status == 200 && echo->body == "round trip",
          "POST /echo round trip -> " + (echo ? echo->body : describe(echo.error())));

    auto empty = co_await fetch(http, http.get(url(plain, "/empty")));
    check(empty && empty->status == 204 && empty->body.empty() && empty->bodyless, "204 is bodyless");

    const auto head_res = co_await fetch(http, http.head(url(plain, "/big?n=1000")));
    check(head_res && head_res->status == 200 && head_res->body.empty() && head_res->bodyless,
          "HEAD returns the head and no body");
    {
        // Over HTTP/1.1 the server synthesizes the Content-Length a GET would
        // have produced (RFC 9110 §9.3.2); its HTTP/2 writer does not, so that
        // part is checked on the h1 path.
        auto cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http11;
        cfg.default_h2c = sh::H2cMode::Off;
        h::Client h1_http{co_await asio::this_coro::executor, cfg};
        auto r = co_await fetch(h1_http, h1_http.head(url(plain, "/big?n=1000")));
        check(r && r->body.empty() && r->header("content-length").value_or("") == "1000",
              "HEAD over HTTP/1.1 keeps the Content-Length a GET would have "
              "produced");
    }

    auto big = co_await fetch(http, http.get(url(plain, "/big?n=300000")));
    check(big && big->body.size() == 300000 && big->body[0] == 'a', "300000-byte body streamed and intact");

    auto missing = co_await fetch(http, http.get(url(plain, "/nope")));
    check(missing && missing->status == 404, "404 is reported, not followed");

    auto capped = co_await fetch(http, http.get(url(plain, "/big?n=200000")).max_body_bytes(1000));
    check(!capped && capped.error() == sh::client_errc::body_too_large, "body cap stops an oversized response");

    // Keep-alive: a second request on the same client reuses the connection.
    auto before = http.stats().connections_opened;
    auto again = co_await fetch(http, http.get(url(plain, "/world")));
    check(again && http.stats().connections_opened == before, "keep-alive reuses the single connection");

    // read() before read_head(): the head is cached, so asking for it later (and
    // repeatedly) still reports the same thing.
    {
        auto opened = co_await http.open_stream(url(plain, "/world"));
        if (!opened) {
            check(false, "read_head-after-read could not start: " + describe(opened.error()));
        } else {
            auto first_chunk = co_await opened->read(); // body first, without the head
            auto head = co_await opened->read_head();
            auto head_again = co_await opened->read_head();
            auto rest = co_await opened->read_all(4096);
            const bool chunk_ok = first_chunk.has_value() && !first_chunk->eof;
            check(chunk_ok && head && head_again && rest && head->status == 200 && head_again->status == 200 &&
                      head->headers.get("content-length") == head_again->headers.get("content-length"),
                  "read_head() is idempotent and works after read()");
        }
    }

    // h2 bodyless responses (HEAD and 204) — the same rule as on the h1 path.
    {
        auto cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http2;
        cfg.default_h2c = sh::H2cMode::PriorKnowledge;
        h::Client h2_http{co_await asio::this_coro::executor, cfg};
        auto head_res = co_await fetch(h2_http, h2_http.head(url(plain, "/big?n=2048")));
        check(head_res && head_res->status == 200 && head_res->body.empty() && head_res->bodyless,
              "HEAD over HTTP/2 ends at the headers");
        auto empty = co_await fetch(h2_http, h2_http.get(url(plain, "/empty")));
        check(empty && empty->status == 204 && empty->bodyless, "204 over HTTP/2 is bodyless");
    }

    // The configuration hooks are actually used.
    {
        auto cfg = base_config();
        int resolves = 0;
        int setups = 0;
        cfg.resolve = [&resolves](std::string host, std::string port)
            -> asio::awaitable<std::pair<sh::error_code, std::vector<asio::ip::tcp::endpoint>>> {
            ++resolves;
            asio::ip::tcp::resolver resolver{co_await asio::this_coro::executor};
            auto [ec, results] = co_await resolver.async_resolve(host, port, asio::as_tuple(asio::use_awaitable));
            std::vector<asio::ip::tcp::endpoint> endpoints;
            if (!ec) {
                for (const auto &entry : results)
                    endpoints.push_back(entry.endpoint());
            }
            co_return std::make_pair(ec, std::move(endpoints));
        };
        cfg.socket_setup = [&setups](asio::ip::tcp::socket &socket) {
            ++setups;
            sh::error_code ec;
            socket.set_option(asio::ip::tcp::no_delay(true), ec);
        };
        h::Client hooked{co_await asio::this_coro::executor, cfg};
        auto response = co_await fetch(hooked, hooked.get(url(plain, "/world")));
        check(response && resolves == 1 && setups == 1, "resolve and socket_setup hooks are invoked");
    }

    // 304 is bodyless and its connection stays at a request boundary.
    {
        FakeServer &fake = make_fake(ctx, "HTTP/1.1 304 Not Modified\r\nETag: \"abc\"\r\n\r\n",
                                     /*close_after=*/false);
        auto cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http11;
        cfg.default_h2c = sh::H2cMode::Off;
        h::Client fake_http{co_await asio::this_coro::executor, cfg};
        auto r = co_await fetch(fake_http, fake_http.get(url(fake.port(), "/cached")));
        check(r && r->status == 304 && r->bodyless && r->header("etag") == "\"abc\"",
              "a 304 is bodyless and keeps its validators");
    }

    // An oversized response head is refused instead of buffered.
    {
        FakeServer &fake = make_fake(ctx, "HTTP/1.1 200 OK\r\nx-pad: " + std::string(4096, 'p') + "\r\n\r\n",
                                     /*close_after=*/false, /*split_write=*/true);
        auto cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http11;
        cfg.default_h2c = sh::H2cMode::Off;
        cfg.limits.max_header_bytes = 1024;
        h::Client capped{co_await asio::this_coro::executor, cfg};
        auto r = co_await fetch(capped, capped.get(url(fake.port(), "/big-head")));
        check(!r && r.error() == sh::client_errc::header_too_large,
              "an oversized response head is refused with header_too_large");
    }
    co_return;
}

asio::awaitable<void> suite_streaming(std::uint16_t plain) {
    std::printf("\n== streaming ==\n");
    for (const auto mode : {sh::H2cMode::PriorKnowledge, sh::H2cMode::Upgrade}) {
        auto cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http2;
        cfg.default_h2c = mode;
        h::Client http{co_await asio::this_coro::executor, cfg};

        // A streamed request body: chunked on h1, DATA frames on h2, with the
        // response read back on the same exchange.
        auto opened = co_await http.open_stream(url(plain, "/drain"), {.method = sh::Method::Post});
        if (!opened) {
            check(false, "streamed POST could not start: " + describe(opened.error()));
            continue;
        }
        std::size_t sent = 0;
        for (int i = 0; i < 16; ++i) {
            std::string chunk(64 * 1024, 'y');
            sent += chunk.size();
            if (auto ec = co_await opened->write(std::move(chunk)); ec) {
                check(false, "streamed write failed: " + describe(ec));
                break;
            }
        }
        if (auto ec = co_await opened->finish("tail"); ec)
            check(false, "finish failed: " + describe(ec));
        auto body = co_await opened->read_all(1024);
        const std::string expected = "received " + std::to_string(sent + 4) + " bytes";
        check(body && *body == expected, std::string{"streamed upload (1 MiB) over "} +
                                             (mode == sh::H2cMode::Upgrade ? "h2c upgrade" : "h2c prior knowledge") +
                                             " -> " + (body ? *body : describe(body.error())));
    }
    // An 8 MiB streamed upload: many flow-control rounds, and the response must
    // still match what the handler counted.
    {
        auto cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http2;
        cfg.default_h2c = sh::H2cMode::PriorKnowledge;
        h::Client big_http{co_await asio::this_coro::executor, cfg};
        auto opened = co_await big_http.open_stream(url(plain, "/drain"), {.method = sh::Method::Post});
        if (!opened) {
            check(false, "8 MiB streamed upload could not start: " + describe(opened.error()));
        } else {
            std::size_t sent = 0;
            for (int i = 0; i < 128; ++i) { // 128 x 64 KiB
                std::string chunk(64 * 1024, 'z');
                sent += chunk.size();
                if (auto ec = co_await opened->write(std::move(chunk)); ec) {
                    check(false, "8 MiB upload write failed: " + describe(ec));
                    break;
                }
            }
            if (auto ec = co_await opened->finish(""); ec)
                check(false, "8 MiB finish failed: " + describe(ec));
            auto body = co_await opened->read_all(1024);
            check(body && *body == "received " + std::to_string(sent) + " bytes",
                  "an 8 MiB streamed upload is framed and counted correctly -> " +
                      (body ? *body : describe(body.error())));
        }
    }
    co_return;
}

asio::awaitable<void> suite_abort_gate(std::uint16_t plain) {
    std::printf("\n== abort releases the h1 gate ==\n");
    auto cfg = base_config();
    cfg.default_version = sh::HttpVersionPolicy::Http11;
    h::Client http{co_await asio::this_coro::executor, cfg};
    const std::string target = url(plain, "/world");

    auto opened = co_await http.open_stream(target, {.method = sh::Method::Get});
    if (!opened) {
        check(false, "abort: open_stream failed: " + describe(opened.error()));
        co_return;
    }
    co_await opened->abort();

    // Keep `opened` alive across the next request. Before the fix the aborted
    // exchange kept the single-connection gate, so this second request parked on
    // acquire() forever (the old session is dead, so it dials a new one first).
    bool completed = false;
    auto second = [&]() -> asio::awaitable<void> {
        auto response = co_await http.get(target).send();
        completed = response.has_value();
    };
    auto timer = [&]() -> asio::awaitable<void> {
        asio::steady_timer deadline{co_await asio::this_coro::executor};
        deadline.expires_after(std::chrono::seconds(2));
        co_await deadline.async_wait(asio::as_tuple(asio::use_awaitable));
    };
    using namespace asio::experimental::awaitable_operators;
    co_await (second() || timer());
    check(completed, "a request after Stream::abort() completes while the aborted Stream is still alive");
    co_return;
}

asio::awaitable<void> suite_h2_multiplex(std::uint16_t plain) {
    std::printf("\n== HTTP/2 multiplexing ==\n");
    auto cfg = base_config();
    cfg.default_version = sh::HttpVersionPolicy::Http2;
    cfg.default_h2c = sh::H2cMode::PriorKnowledge;
    h::Client http{co_await asio::this_coro::executor, cfg};

    constexpr int kStreams = 16;
    auto ex = co_await asio::this_coro::executor;
    asio::experimental::concurrent_channel<void(sh::error_code)> done{ex, kStreams};
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kStreams; ++i) {
        asio::co_spawn(
            ex,
            [&]() -> asio::awaitable<void> {
                auto stream = co_await http.open_stream(url(plain, "/delay?ms=300"));
                if (!stream) {
                    (void)done.try_send(stream.error());
                    co_return;
                }
                auto head = co_await stream->read_head();
                auto body = co_await stream->read_all(1024);
                if (!head || !body || *body != "delayed") {
                    (void)done.try_send(sh::make_error_code(sh::client_errc::protocol_error));
                    co_return;
                }
                (void)done.try_send(sh::error_code{});
                co_return;
            },
            asio::detached);
    }
    int failures = 0;
    for (int i = 0; i < kStreams; ++i) {
        auto [ec] = co_await done.async_receive(asio::as_tuple(asio::use_awaitable));
        if (ec)
            ++failures;
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    check(failures == 0, std::to_string(kStreams) + " concurrent streams all answered");
    check(elapsed.count() < 16 * 300,
          "16 x /delay?ms=300 ran concurrently (" + std::to_string(elapsed.count()) + " ms, serial would be 4800 ms)");
    // With the connection hidden (as in Go/reqwest), concurrent first hops dial
    // their own connections; steady-state reuse is exercised by the keep-alive
    // and pool-TTL checks elsewhere — this suite asserts correctness, not the
    // old explicit-session "exactly one connection" guarantee.

    // The multiplexing limit is the peer's, not ours: 64 concurrent streams on
    // the same connection, with a body written on each.
    {
        constexpr int kWide = 64;
        auto ex2 = co_await asio::this_coro::executor;
        asio::experimental::concurrent_channel<void(sh::error_code)> done2{ex2, kWide};
        for (int i = 0; i < kWide; ++i) {
            asio::co_spawn(
                ex2,
                [&, i]() -> asio::awaitable<void> { // i by value: the coroutine
                                                    // outlives the loop body
                    auto stream = co_await http.open_stream(url(plain, "/echo"), {.method = sh::Method::Post});
                    if (!stream) {
                        std::printf("  stream %d: open failed: %s\n", i, describe(stream.error()).c_str());
                        (void)done2.try_send(stream.error());
                        co_return;
                    }
                    const std::string body_text = "stream-" + std::to_string(i);
                    if (auto ec = co_await stream->write(body_text); ec) {
                        (void)done2.try_send(ec);
                        co_return;
                    }
                    (void)co_await stream->finish("");
                    auto body = co_await stream->read_all(1024);
                    const bool ok = body && *body == body_text;
                    if (!ok) {
                        std::printf("  stream %d: read failed: %s\n", i,
                                    body ? ("body='" + *body + "'").c_str() : describe(body.error()).c_str());
                    }
                    (void)done2.try_send(ok ? sh::error_code{} : sh::make_error_code(sh::client_errc::protocol_error));
                    co_return;
                },
                asio::detached);
        }
        int wide_failures = 0;
        std::string first_error;
        for (int i = 0; i < kWide; ++i) {
            auto [ec] = co_await done2.async_receive(asio::as_tuple(asio::use_awaitable));
            if (ec) {
                ++wide_failures;
                if (first_error.empty())
                    first_error = describe(ec);
            }
        }
        check(wide_failures == 0, "64 concurrent streams on one connection answer independently -> " +
                                      std::to_string(wide_failures) + " failed (" + first_error + ")");
    }

    // Deliver more request-body bytes than the connection window to a handler
    // that answers without reading them (`/big`). The engine must discard and
    // credit the tail; otherwise the connection stalls after ~64 KiB.
    {
        constexpr int kRequests = 8;
        constexpr std::size_t kBody = 100 * 1024;
        int answered = 0;
        for (int i = 0; i < kRequests; ++i) {
            auto r = co_await http.post(url(plain, "/big?n=16")).body(std::string(kBody, 'x')).send();
            if (r && r->status() == 200)
                ++answered;
        }
        check(answered == kRequests, "unread request bodies return their connection-window credit (" +
                                         std::to_string(answered) + "/" + std::to_string(kRequests) + ")");
    }

    // A stream that is abandoned mid-body must not take the connection down: on
    // HTTP/2 the session resets just that stream.
    auto st = co_await http.open_stream(url(plain, "/big?n=400000"));
    if (st) {
        auto head = co_await st->read_head();
        (void)head;
        (void)co_await st->abort();
    }
    auto after = co_await http.open_stream(url(plain, "/world"));
    bool usable = false;
    if (after) {
        auto head = co_await after->read_head();
        usable = head && head->status == 200;
    }
    check(usable, "cancelling a stream leaves the HTTP/2 connection usable");
    co_return;
}

asio::awaitable<void> suite_errors(std::uint16_t plain) {
    std::printf("\n== errors and timeouts ==\n");
    {
        auto cfg = base_config();
        cfg.request_timeout = std::chrono::milliseconds(300);
        h::Client http{co_await asio::this_coro::executor, cfg};
        auto r = co_await fetch(http, http.get(url(plain, "/delay?ms=3000")));
        check(!r && r.error() == sh::client_errc::request_timeout, "a slow response hits request_timeout");
    }
    {
        h::Client http{co_await asio::this_coro::executor, base_config()};
        auto r = co_await fetch(http, http.get("http://127.0.0.1:1/world"));
        check(!r, "a closed port is an error, not a hang: " + describe(r.error()));
        auto bad = co_await fetch(http, http.get("ftp://127.0.0.1/world"));
        check(!bad && bad.error() == sh::client_errc::unsupported_scheme, "a non-http scheme is rejected");
        auto malformed = co_await fetch(http, http.get("not a url"));
        check(!malformed && malformed.error() == sh::client_errc::bad_url, "a malformed URL is rejected");
    }
    {
        // Layered budgets on the explicit Stream path (production P0): a slow
        // head fails under the TTFB budget with response_head_timeout — while
        // the convenience path above keeps its overall request_timeout.
        auto cfg = base_config();
        cfg.response_head_timeout = std::chrono::milliseconds(300);
        h::Client http{co_await asio::this_coro::executor, cfg};
        auto s = co_await http.open_stream(url(plain, "/delay?ms=2000"));
        if (!s) {
            check(false, "layered timeout: could not open: " + describe(s.error()));
        } else {
            auto head = co_await s->read_head(); // the server delays 2s
            check(!head && head.error() == sh::client_errc::response_head_timeout,
                  "the TTFB budget rejects a slow head -> " + describe(head.error()));
            (void)co_await s->abort();
        }
    }
    co_return;
}

asio::awaitable<void> suite_tls(std::uint16_t tls_port) {
    std::printf("\n== TLS ==\n");
    {
        auto cfg = base_config(); // CA + client cert + CN verification
        h::Client http{co_await asio::this_coro::executor, cfg};
        auto r = co_await fetch(http, http.get(url(tls_port, "/whoami", true)));
        check(r && r->status == 200 && r->body.find("SimpleHttpClient") != std::string::npos,
              "mutual TLS presented our client certificate: " + (r ? r->body : describe(r.error())));
    }
    {
        auto cfg = base_config();
        cfg.tls.sni_override.clear(); // verify "127.0.0.1" against the certificate
        h::Client http{co_await asio::this_coro::executor, cfg};
        auto r = co_await fetch(http, http.get(url(tls_port, "/world", true)));
        check(!r, "name verification fails for a certificate issued to another name");
    }
    {
        auto cfg = base_config();
        cfg.tls.verify_peer = false;
        h::Client http{co_await asio::this_coro::executor, cfg};
        auto r = co_await fetch(http, http.get(url(tls_port, "/world", true)));
        check(r && r->status == 200, "verification can be turned off deliberately");
    }
    {
        auto cfg = base_config();
        cfg.tls.ca_file = "./test/tls_certificates/client_cert.pem"; // a CA that did not sign
                                                                     // the server
        cfg.tls.verify_host = false;
        h::Client http{co_await asio::this_coro::executor, cfg};
        auto r = co_await fetch(http, http.get(url(tls_port, "/world", true)));
        check(!r, "an unrelated CA fails verification");
    }
    {
        // ALPN pinned to http/1.1 must stay there.
        auto cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http11;
        h::Client http{co_await asio::this_coro::executor, cfg};
        auto r = co_await fetch(http, http.get(url(tls_port, "/world", true)));
        check(r && r->version == sh::Version::Http11, "ALPN pinned to http/1.1 stays on HTTP/1.1");
    }
    {
        // A chunked response over TLS. The HTTP/1.x writer scatters each frame's
        // pieces into separate buffers and lets the transport hand them to OpenSSL
        // one at a time, so the wire framing has to come out identical to the
        // concatenated form it replaced.
        auto cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http11;
        h::Client http{co_await asio::this_coro::executor, cfg};
        auto r = co_await fetch(http, http.get(url(tls_port, "/stream", true)));
        check(r && r->version == sh::Version::Http11 && r->body == "alpha-beta-gamma",
              "a chunked response over TLS decodes intact -> " + (r ? r->body : describe(r.error())));
    }
    co_return;
}

// Everything a well-behaved server will not do, driven by the raw responder.
asio::awaitable<void> suite_raw_peer(asio::io_context &ctx) {
    std::printf("\n== raw peer edge cases ==\n");

    { // an informational response before the real one
        FakeServer &fake = make_fake(ctx,
                                     "HTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\n"
                                     "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok",
                                     /*close_after=*/false);
        h::Client http{co_await asio::this_coro::executor, base_config()};
        auto r = co_await fetch(http, http.get(url(fake.port(), "/x")));
        check(r && r->status == 200 && r->body == "ok", "1xx is skipped, the final head is used");
    }
    { // an asked-for 100-continue on the buffered path
        FakeServer &fake = make_fake(ctx,
                                     "HTTP/1.1 100 Continue\r\n\r\n"
                                     "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok",
                                     /*close_after=*/false);
        auto cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http11; // exercise the plain h1 path
        h::Client http{co_await asio::this_coro::executor, cfg};
        auto r = co_await fetch(http, http.post(url(fake.port(), "/x")).header("expect", "100-continue").body("hi"));
        check(r && r->status == 200 && r->body == "ok",
              "an asked-for 100 is interim: the buffered path returns the final response");
    }
    { // a body delimited by the close
        FakeServer &fake = make_fake(ctx, "HTTP/1.1 200 OK\r\n\r\nuntil eof");
        h::Client http{co_await asio::this_coro::executor, base_config()};
        auto r = co_await fetch(http, http.get(url(fake.port(), "/x")));
        check(r && r->body == "until eof", "an EOF-delimited body is read to the end");
    }
    { // a truncated Content-Length body
        FakeServer &fake = make_fake(ctx, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort");
        h::Client http{co_await asio::this_coro::executor, base_config()};
        auto r = co_await fetch(http, http.get(url(fake.port(), "/x")));
        check(!r, "a truncated body is an error, not a short body: " + describe(r.error()));
    }
    { // a malformed status line
        FakeServer &fake = make_fake(ctx, "NOT-HTTP 200 OK\r\n\r\n");
        h::Client http{co_await asio::this_coro::executor, base_config()};
        auto r = co_await fetch(http, http.get(url(fake.port(), "/x")));
        check(!r && r.error() == sh::client_errc::protocol_error, "a malformed status line is a protocol error");
    }
    { // a chunked response, with an extension and trailers (what a dynamic
        // backend such as code-server sends): every byte must come back intact.
        // Regression: the size line used to be read through a view into the read
        // buffer that the consume step then shifted, so any chunked body failed.
        FakeServer &fake = make_fake(ctx,
                                     "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                                     "5;ext=1\r\nhello\r\n"
                                     "6\r\n world\r\n"
                                     "0\r\nX-Trailer: done\r\n\r\n",
                                     /*close_after=*/false);
        h::Client http{co_await asio::this_coro::executor, base_config()};
        auto r = co_await fetch(http, http.get(url(fake.port(), "/chunked")));
        check(r && r->status == 200 && r->body == "hello world",
              "a chunked response decodes (with an extension and trailers) -> " + (r ? r->body : describe(r.error())));
    }
    { // a chunked body cut short
        FakeServer &fake = make_fake(ctx, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel",
                                     /*close_after=*/true);
        h::Client http{co_await asio::this_coro::executor, base_config()};
        auto r = co_await fetch(http, http.get(url(fake.port(), "/chunked-truncated")));
        check(!r, "a truncated chunked body is an error: " + describe(r.error()));
    }
    { // a malformed chunk-size line
        FakeServer &fake = make_fake(ctx,
                                     "HTTP/1.1 200 OK\r\nTransfer-Encoding: "
                                     "chunked\r\n\r\nzz\r\nhello\r\n0\r\n\r\n",
                                     /*close_after=*/true);
        h::Client http{co_await asio::this_coro::executor, base_config()};
        auto r = co_await fetch(http, http.get(url(fake.port(), "/chunked-bad")));
        check(!r && r.error() == sh::client_errc::protocol_error, "a malformed chunk size is a protocol error");
    }
    { // a stale kept connection: the peer closes while the session sits idle
        FakeServer &fake = make_fake(ctx, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
        h::Client http{co_await asio::this_coro::executor, base_config()};
        auto first = co_await fetch(http, http.get(url(fake.port(), "/x")));
        check(first && first->body == "ok", "first request to the raw peer answered");
        // The peer closed the connection after answering; the next request
        // reuses the kept (now dead) connection, discovers the drop and is
        // retried on a fresh one — the one free stale replay.
        auto second = co_await fetch(http, http.get(url(fake.port(), "/x")));
        check(second && second->body == "ok", "a stale kept connection is re-dialed on a fresh one");
    }
    { // a peer that ignores `Upgrade: h2c` answers with an ordinary response
        FakeServer &fake = make_fake(ctx, "HTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\nno h2c!",
                                     /*close_after=*/false);
        auto cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Auto; // upgrade is attempted,
                                                           // and may be declined
        h::Client http{co_await asio::this_coro::executor, cfg};
        auto r = co_await fetch(http, http.get(url(fake.port(), "/x")));
        check(r && r->status == 200 && r->body == "no h2c!" && r->version == sh::Version::Http11,
              "a declined h2c upgrade falls back to the HTTP/1.1 response -> " +
                  (r ? std::to_string(r->status) + " " + std::string{sh::to_string(r->version)} + " '" + r->body + "'"
                     : describe(r.error())));

        auto cfg2 = base_config();
        cfg2.default_version = sh::HttpVersionPolicy::Http2; // ... but a pinned HTTP/2 policy may not
        h::Client strict{co_await asio::this_coro::executor, cfg2};
        auto r2 = co_await fetch(strict, strict.get(url(fake.port(), "/x")));
        check(!r2 && r2.error() == sh::client_errc::version_not_negotiated,
              "a pinned HTTP/2 policy fails when the upgrade is declined");
    }
    co_return;
}

// Rebuilds what the /big?n=N route produces, so a decoded body can be compared
// exactly rather than by size.
std::string big_expected(std::size_t n) {
    std::string body(n, 'x');
    for (std::size_t i = 0; i < n; i += 4096) {
        body[i] = 'a';
    }
    return body;
}

// Response compression, against a listener that has it enabled. The other
// suites compare response bodies byte-for-byte, so they must keep running
// against servers that do not compress. The client never asks for compression
// or decodes it on its own, so this suite supplies Accept-Encoding and decodes
// whatever comes back.
asio::awaitable<void> suite_compression(std::uint16_t comp_port, std::uint16_t plain_port) {
    std::printf("\n== response compression ==\n");

    struct Mode {
        const char *label;
        sh::HttpVersionPolicy policy;
        sh::H2cMode h2c;
    };
    const Mode modes[] = {
        {"HTTP/1.1", sh::HttpVersionPolicy::Http11, sh::H2cMode::Off},
        {"h2c", sh::HttpVersionPolicy::Http2, sh::H2cMode::PriorKnowledge},
    };

    const std::string want = big_expected(8192);

    for (const Mode &mode : modes) {
        auto cfg = base_config();
        cfg.default_version = mode.policy;
        cfg.default_h2c = mode.h2c;
        h::Client http{co_await asio::this_coro::executor, cfg};
        const std::string tag{mode.label};

        // Both accepted: brotli is preferred.
        {
            auto r =
                co_await fetch(http, http.get(url(comp_port, "/big?n=8192")).header("accept-encoding", "br, gzip"));
            const std::string enc = r ? std::string{r->header("content-encoding").value_or("")} : std::string{};
            check(r && r->status == 200 && enc == "br" && r->body.size() < want.size() &&
                      sh::decompress_all(enc, r->body) == want,
                  tag + ": /big with 'br, gzip' -> " +
                      (r ? enc + " " + std::to_string(r->body.size()) + "B of " + std::to_string(want.size()) + "B"
                         : describe(r.error())));
        }

        // Only gzip accepted.
        {
            auto r = co_await fetch(http, http.get(url(comp_port, "/big?n=8192")).header("accept-encoding", "gzip"));
            const std::string enc = r ? std::string{r->header("content-encoding").value_or("")} : std::string{};
            check(r && enc == "gzip" && sh::decompress_all(enc, r->body) == want,
                  tag + ": /big with 'gzip' -> " + (r ? enc : describe(r.error())));
        }

        // A client that did not ask must get the bytes unchanged.
        {
            auto r = co_await fetch(http, http.get(url(comp_port, "/big?n=8192")));
            check(r && !r->header("content-encoding").has_value() && r->body == want,
                  tag + ": /big without accept-encoding is untouched");
        }

        // identity explicitly requested.
        {
            auto r =
                co_await fetch(http, http.get(url(comp_port, "/big?n=8192")).header("accept-encoding", "identity"));
            check(r && !r->header("content-encoding").has_value() && r->body == want,
                  tag + ": /big with 'identity' is untouched");
        }

        // Under min_bytes.
        {
            auto r = co_await fetch(http, http.get(url(comp_port, "/world")).header("accept-encoding", "br, gzip"));
            check(r && r->status == 200 && !r->header("content-encoding").has_value(),
                  tag + ": a short response stays uncompressed");
        }

        // 204 has no body to compress.
        {
            auto r = co_await fetch(http, http.get(url(comp_port, "/empty")));
            check(r && r->status == 204 && !r->header("content-encoding").has_value(),
                  tag + ": 204 stays uncompressed");
        }

        // Streamed: the length is unknown up front, so min_bytes cannot apply and
        // it is compressed anyway (compress_streamed defaults on). It must decode
        // back exactly, and must not claim a length.
        {
            auto r = co_await fetch(http, http.get(url(comp_port, "/stream")).header("accept-encoding", "gzip"));
            const std::string enc = r ? std::string{r->header("content-encoding").value_or("")} : std::string{};
            check(r && enc == "gzip" && sh::decompress_all(enc, r->body) == "alpha-beta-gamma",
                  tag + ": a streamed response round-trips -> " + (r ? enc : describe(r.error())));
            check(r && !r->header("content-length").has_value(),
                  tag + ": a streamed response states no Content-Length");
        }

        // HEAD carries no body, so nothing is encoded.
        {
            auto r =
                co_await fetch(http, http.head(url(comp_port, "/big?n=8192")).header("accept-encoding", "br, gzip"));
            check(r && r->status == 200 && r->bodyless && !r->header("content-encoding").has_value(),
                  tag + ": HEAD is not compressed");
        }
    }

    // A shared cache has to key on Accept-Encoding, or it can hand a gzipped
    // body to a client that only understands identity.
    {
        h::Client http{co_await asio::this_coro::executor, base_config()};
        auto r = co_await fetch(http, http.get(url(comp_port, "/big?n=8192")).header("accept-encoding", "gzip"));
        const std::string vary = r ? std::string{r->header("vary").value_or("")} : std::string{};
        check(r && vary.find("Accept-Encoding") != std::string::npos,
              "a compressed response carries Vary: Accept-Encoding");
    }

    // The uncompressed listener must be unaffected: same request, no encoding.
    {
        h::Client http{co_await asio::this_coro::executor, base_config()};
        auto r = co_await fetch(http, http.get(url(plain_port, "/big?n=8192")).header("accept-encoding", "br, gzip"));
        check(r && !r->header("content-encoding").has_value() && r->body == want,
              "a listener with compression off ignores accept-encoding");
    }

    // --- the client side: with auto_decompress on, callers see decoded bytes ---
    for (const Mode &mode : modes) {
        auto cfg = base_config();
        cfg.default_version = mode.policy;
        cfg.default_h2c = mode.h2c;
        cfg.auto_decompress = true;
        h::Client http{co_await asio::this_coro::executor, cfg};
        const std::string tag = std::string{mode.label} + " + auto_decompress";

        // The body is the original bytes, with no trace of the encoding left.
        {
            auto r = co_await fetch(http, http.get(url(comp_port, "/big?n=8192")));
            check(r && r->status == 200 && r->body == want, tag + ": the body arrives decoded");
            check(r && !r->header("content-encoding").has_value(), tag + ": the encoding header is gone");
            check(r && !r->header("content-length").has_value(), tag + ": the stale Content-Length is gone");
        }

        // Proof that Accept-Encoding actually went out: /headers echoes the
        // request head back (compressed in transit, decoded on arrival).
        {
            auto r = co_await fetch(http, http.get(url(comp_port, "/headers")));
            check(r && r->body.find("accept-encoding:") != std::string::npos,
                  tag + ": the request advertises what we can decode");
        }

        // A stream is decoded as it is read, not just the aggregate path.
        {
            auto opened = co_await http.open_stream(url(comp_port, "/stream"));
            check(opened.has_value(), tag + ": the stream opens");
            if (opened) {
                auto all = co_await opened->read_all();
                check(all && *all == "alpha-beta-gamma",
                      tag + ": a streamed body is decoded -> " +
                          (all ? "got [" + *all + "] (" + std::to_string(all->size()) + " bytes)"
                               : describe(all.error())));
                check(!opened->head().headers.contains("content-encoding"),
                      tag + ": a streamed head drops the encoding");
            }
        }

        // A caller that sets the header keeps control of the negotiation, and
        // the result is still decoded.
        {
            auto r = co_await fetch(http, http.get(url(comp_port, "/big?n=8192")).header("accept-encoding", "gzip"));
            check(r && r->body == want, tag + ": an explicit Accept-Encoding is respected");
        }
    }

    co_return;
}

// Every suite, in order. A named coroutine rather than a lambda: a lambda
// coroutine whose handle escapes (here, into co_spawn) trips a GCC
// coroutine-frame lifetime problem in this toolchain, and the suites would then
// run against a frame that has already been reused.
// A Request asking for both an up-front body and a streamed one is a
// contradiction. The engines used to resolve it differently and silently —
// HTTP/2 sent `body` as the first chunk, HTTP/1.1 dropped it entirely — so the
// answer is now one refusal in both places.
asio::awaitable<void> suite_spec_validation(std::uint16_t plain_port) {
    std::printf("\n== request spec validation ==\n");
    auto cfg = base_config();
    h::Client http{co_await asio::this_coro::executor, cfg};

    // The old spec's "body + stream_body" contradiction cannot exist in the new
    // API: a buffered body goes to send()/RequestBuilder, a streamed one goes
    // to open_stream()/Stream — the split is by type, not by runtime check.
    // Verify the two shapes each work on their own.
    {
        auto r = co_await fetch(http, http.post(url(plain_port, "/echo")).body("x"));
        check(r && r->body == "x", "a buffered body alone is accepted and echoed");
    }
    {
        auto s = co_await http.open_stream(url(plain_port, "/drain"), {.method = sh::Method::Post});
        check(s.has_value(), "a streamed (write-end open) request opens");
        if (s) {
            (void)co_await s->write("x");
            (void)co_await s->finish("");
            auto body = co_await s->read_all(16);
            check(body && *body == "received 1 bytes", "a streamed body alone is accepted and drained");
        }
    }

    // StreamSpec::close sends `Connection: close` and forces a fresh connection
    // for the next request (visible through the connection counter).
    {
        auto cfg1 = base_config();
        cfg1.default_version = sh::HttpVersionPolicy::Http11;
        cfg1.default_h2c = sh::H2cMode::Off;
        h::Client h1{co_await asio::this_coro::executor, cfg1};
        auto st = co_await h1.open_stream(url(plain_port, "/world"), {.close = true});
        if (st) {
            (void)co_await st->read_all();
        }
        const auto after_close = h1.stats().connections_opened;
        auto r = co_await fetch(h1, h1.get(url(plain_port, "/world")));
        check(r && h1.stats().connections_opened == after_close + 1,
              "HTTP/1.1: close = true forces a fresh connection");

        auto cfg2 = base_config();
        cfg2.default_version = sh::HttpVersionPolicy::Http2;
        cfg2.default_h2c = sh::H2cMode::PriorKnowledge;
        h::Client h2_http{co_await asio::this_coro::executor, cfg2};
        auto st2 = co_await h2_http.open_stream(url(plain_port, "/world"), {.close = true});
        if (st2) {
            (void)co_await st2->read_all();
        }
        const auto after_close2 = h2_http.stats().connections_opened;
        auto r2 = co_await fetch(h2_http, h2_http.get(url(plain_port, "/world")));
        check(r2 && h2_http.stats().connections_opened == after_close2 + 1,
              "HTTP/2: close = true forces a fresh connection");
    }
    {
        // Kept-connection keying: two *protocol policies* for the same origin
        // (here the in-process server) must not share the single kept
        // connection — each policy is its own connection identity, exactly the
        // partition the old pool used (scheme, host, port, version, tag).
        sh::detail::ClientEngine engine{co_await asio::this_coro::executor, base_config()};
        const auto ex = co_await asio::this_coro::executor;
        auto make_target = [&](sh::HttpVersionPolicy v, sh::H2cMode h) {
            sh::detail::ClientTarget t;
            t.host = "127.0.0.1";
            t.port = plain_port;
            t.use_tls = false;
            t.version = v;
            t.h2c = h;
            return t;
        };
        auto make_req = [&](const char *p) {
            auto r = std::make_shared<sh::Request>(sh::Version::Http11, ex);
            r->set_method(sh::Method::Get);
            r->set_target(p);
            return r;
        };
        auto open_and_drain = [&](sh::detail::ClientTarget t, std::string_view tag) -> asio::awaitable<bool> {
            auto op = co_await engine.start_exchange(std::move(t), make_req("/world"), {}, {});
            if (!op) {
                check(false, std::string{tag} + ": open -> " + op.error().message());
                co_return false;
            }
            auto b = co_await op->stream->read_all(16 * 1024);
            co_return b.has_value();
        };
        // Policy A (Auto + h2c upgrade) binds and upgrades the kept connection.
        check(co_await open_and_drain(make_target(sh::HttpVersionPolicy::Auto, sh::H2cMode::Upgrade), "policy A"),
              "policy A opens on its own connection");
        // Policy B (HTTP/1.1, no h2c) on the *same origin* must dial a separate
        // throwaway — the first connection was negotiated for a different policy.
        check(co_await open_and_drain(make_target(sh::HttpVersionPolicy::Http11, sh::H2cMode::Off), "policy B"),
              "policy B opens on a separate connection");
        check(engine.opened_count() == 2, "two policies for one origin -> two connections, not one shared");
        // Back to policy A: the kept connection is reused, not a third dial.
        check(co_await open_and_drain(make_target(sh::HttpVersionPolicy::Auto, sh::H2cMode::Upgrade), "policy A retry"),
              "policy A reuses its kept connection");
        check(engine.opened_count() == 2, "policy A still reuses the first connection after policy B");
    }
    co_return;
}

asio::awaitable<void> suite_raw_peer(asio::io_context &ctx);

// --- the new http:: client API (client/http.h) -------------------------------

asio::awaitable<void> suite_http_api(std::uint16_t port) {
    namespace h = sh;
    const std::string base = "http://127.0.0.1:" + std::to_string(port);

    // Buffered convenience: builder chain → send → Response.
    {
        h::Client client{co_await asio::this_coro::executor, base_config()};
        auto r = co_await client.get(base + "/world").send();
        check(r.has_value(), "http::get(url).send() returns expected<Response>");
        if (!r)
            co_return;
        check(r->status() == 200 && r->ok(), "http::Response ok() on a 200");
        // The response body is a uniform Body stream (Go's http.Response.Body):
        // readable via body().read_all() / body().read() in chunks.
        {
            auto whole = co_await r->body().read_all();
            check(whole && whole->find("hello from") != std::string::npos,
                  "http::Response::body() reads as a Body stream (read_all)");
            auto next = co_await r->read();
            check(next && next->eof, "http::Response::read() reports end-of-body after read_all()");
        }
    }
    // query() percent-encodes and appends to the URL.
    {
        h::Client client{co_await asio::this_coro::executor, base_config()};
        auto r = co_await client.get(base + "/query-echo").query({{"tag", "a b&c"}, {"n", "1"}}).send();
        std::string body;
        if (r) {
            if (auto text = co_await r->text())
                body = *text;
        }
        check(r && body.find("tag=a+b%26c") != std::string::npos && body.find("n=1") != std::string::npos,
              "http::query() encodes and sends");
    }
    // json() posts a glaze-serialized body with app/json.
    {
        h::Client client{co_await asio::this_coro::executor, base_config()};
        auto r = co_await client.post(base + "/echo").json(client_test::HttpApiPet{7, "rex"}).send();
        std::string body;
        if (r) {
            if (auto text = co_await r->text())
                body = *text;
        }
        check(r && body.find("\"id\":7") != std::string::npos && body.find("\"name\":\"rex\"") != std::string::npos,
              "http::json() sends a glaze body that echoes back");
    }
    // request_timeout=0 means "no limit" (ClientConfig documents it).
    {
        auto cfg = base_config();
        cfg.request_timeout = std::chrono::seconds(0);
        h::Client client{co_await asio::this_coro::executor, cfg};
        auto r = co_await client.get(base + "/world").send();
        check(r && r->status() == 200, "request_timeout=0 means no limit");
    }
    // auto_decompress must not install a decoder for a bodyless response that
    // still carries Content-Encoding (HEAD, and 204/304).
    {
        auto cfg = base_config();
        cfg.auto_decompress = true;
        h::Client client{co_await asio::this_coro::executor, cfg};
        auto r = co_await client.request(sh::Method::Head, base + "/gzip-head").send();
        check(r && r->status() == 200, "auto_decompress: a bodyless HEAD response is not a decode error");
    }
    // Client::send(shared_ptr<Request>): the documented client-side workflow is
    // to feed()+finish() the request's Body. That body is not body_source(), and
    // the redirect/retry loop rebuilds each hop from the source, so it used to be
    // dropped (an empty body went out).
    {
        h::Client client{co_await asio::this_coro::executor, base_config()};
        auto req = std::make_shared<sh::Request>(sh::Version::Http11, co_await asio::this_coro::executor);
        req->set_method(sh::Method::Post);
        req->set_target("/echo");
        req->mutable_headers().add("content-type", "text/plain");
        (void)req->body().feed("direct-body");
        req->body().finish();
        req->set_close(true); // must survive the send path too
        auto r = co_await client.send(req, base + "/echo");
        std::string body;
        if (r) {
            if (auto text = co_await r->text())
                body = *text;
        }
        check(r && body == "direct-body", "Client::send(Request): a feed()+finish() body is not dropped");
    }
    // basic/bearer auth produce the Authorization header.
    {
        h::Client client{co_await asio::this_coro::executor, base_config()};
        auto r = co_await client.get(base + "/headers").bearer_auth("tok123").send();
        std::string body;
        if (r) {
            if (auto text = co_await r->text())
                body = *text;
        }
        check(r && body.find("authorization: Bearer tok123") != std::string::npos,
              "http::bearer_auth() sets the Authorization header");
    }
    // error_for_status() turns a non-2xx into an error.
    {
        h::Client client{co_await asio::this_coro::executor, base_config()};
        auto r = co_await client.get(base + "/nothing-here").send();
        check(r.has_value(), "a 404 still returns a Response (only transport fails are errors)");
        if (r)
            check(!r->error_for_status().has_value(), "http::Response::error_for_status() on a 404");
    }
    // Explicit full-duplex: stream the request body in chunks, then read.
    {
        h::Client client{co_await asio::this_coro::executor, base_config()};
        auto up = co_await client.open_stream(base + "/drain", {.method = sh::Method::Post});
        check(up.has_value(), "http::open_stream() opens a write-end stream");
        if (!up)
            co_return;
        (void)co_await up->write("hello ");
        (void)co_await up->write("world");
        (void)co_await up->finish();
        auto head = co_await up->read_head();
        check(head && head->status == 200, "stream read_head after finish");
        auto body = co_await up->read_all();
        check(body && *body == "received 11 bytes", "chunked upload is drained by the route");
    }
    // Read end streams a chunked response.
    {
        h::Client client{co_await asio::this_coro::executor, base_config()};
        auto s = co_await client.open_stream(base + "/stream");
        if (!s) {
            check(false, "open_stream /stream");
            co_return;
        }
        auto head = co_await s->read_head();
        std::string got;
        while (auto c = co_await s->read()) {
            if (c->eof)
                break;
            got += c->data;
        }
        check(head && got == "alpha-beta-gamma", "open_stream reads a chunked response in pieces");
    }
}

// --- redirect following & the cookie jar (convenience level) ------------------

asio::awaitable<void> suite_redirect(std::uint16_t port) {
    const std::string base = "http://127.0.0.1:" + std::to_string(port);
    // No following by default: the 302 itself comes back.
    {
        h::Client client{co_await asio::this_coro::executor};
        auto r = co_await fetch(client, client.get(base + "/redir/a"));
        check(r && r->status == 302 && r->header("location") == "/redir/b",
              "redirects are not followed when max_redirects is 0 (the default)");
    }
    // With max_redirects set, the chain is followed to the 200.
    {
        sh::ClientConfig cfg;
        cfg.max_redirects = 5;
        h::Client client{co_await asio::this_coro::executor, cfg};
        auto r = co_await fetch(client, client.get(base + "/redir/a"));
        check(r && r->status == 200 && r->body == "landed", "a 302 chain is followed to the 200");
    }
    // POST → 303 → GET: the second hop is a bodyless GET.
    {
        sh::ClientConfig cfg;
        cfg.max_redirects = 5;
        h::Client client{co_await asio::this_coro::executor, cfg};
        auto r = co_await fetch(client, client.post(base + "/redir/post").body("payload"));
        check(r && r->status == 200 && r->body == "landed", "a 303 turns the POST into a GET");
    }
    // 307 keeps the method: the echo handler reports POST.
    {
        sh::ClientConfig cfg;
        cfg.max_redirects = 5;
        h::Client client{co_await asio::this_coro::executor, cfg};
        auto r = co_await fetch(client, client.post(base + "/redir/307").body("payload"));
        check(r && r->status == 200 && r->body == "POST", "a 307 keeps the method and body");
    }
    // A relative Location resolves against the current directory.
    {
        sh::ClientConfig cfg;
        cfg.max_redirects = 5;
        h::Client client{co_await asio::this_coro::executor, cfg};
        auto r = co_await fetch(client, client.get(base + "/dir/start"));
        check(r && r->body == "relative", "a relative Location resolves against the directory");
    }
    // A Set-Cookie from the redirect is replayed on the followed request.
    {
        sh::ClientConfig cfg;
        cfg.max_redirects = 5;
        cfg.cookie_jar = std::make_shared<sh::CookieJar>();
        h::Client client{co_await asio::this_coro::executor, cfg};
        auto r = co_await fetch(client, client.get(base + "/redir/cookie"));
        check(r && r->body == "sid=abc123", "a Set-Cookie from a redirect is replayed on the next hop");
    }
    // The jar is consulted before the *first* request of a send() too, not only
    // on redirect hops. max_redirects=0 stops at the 302 but still stores its
    // Set-Cookie; the next independent call must replay it.
    {
        sh::ClientConfig cfg;
        cfg.cookie_jar = std::make_shared<sh::CookieJar>();
        h::Client client{co_await asio::this_coro::executor, cfg};
        auto set = co_await fetch(client, client.get(base + "/redir/cookie"));
        check(set && set->status == 302, "the cookie-setting response is returned");
        auto echo = co_await fetch(client, client.get(base + "/redir/cookie-check"));
        check(echo && echo->body == "sid=abc123", "the jar is replayed on a later independent request");
    }
    // A hop to a different host drops the Authorization header (Go's rule).
    {
        sh::ClientConfig cfg;
        cfg.max_redirects = 5;
        // Force every hostname (including "localhost") onto the IPv4 loopback,
        // so the hop's host really differs from the first hop's "127.0.0.1".
        cfg.resolve =
            [](std::string,
               std::string port) -> asio::awaitable<std::pair<sh::error_code, std::vector<asio::ip::tcp::endpoint>>> {
            std::vector<asio::ip::tcp::endpoint> endpoints;
            endpoints.emplace_back(asio::ip::make_address("127.0.0.1"), static_cast<std::uint16_t>(std::stoul(port)));
            co_return std::pair{sh::error_code{}, std::move(endpoints)};
        };
        h::Client client{co_await asio::this_coro::executor, cfg};
        auto r = co_await fetch(client, client.get(base + "/redir-host").header("authorization", "Bearer secret"));
        check(r && r->body == "no-auth", "a cross-origin redirect drops the Authorization header");
    }
    // A redirect loop is stopped at max_redirects with an error (Go returns the
    // last response *and* an error here; we report the error).
    {
        sh::ClientConfig cfg;
        cfg.max_redirects = 3;
        h::Client client{co_await asio::this_coro::executor, cfg};
        auto r = co_await fetch(client, client.get(base + "/redir/loop"));
        check(!r && r.error() == sh::make_error_code(sh::client_errc::too_many_redirects),
              "a redirect loop is cut off at max_redirects with an error");
    }
    // A kept HTTP/1.1 connection must survive a cross-origin exchange in the
    // middle: the throwaway (other-origin) h1 exchange runs on its own
    // connection and neither blocks nor is blocked by the kept connection's
    // one-exchange gate — a follow-up same-origin request proceeds immediately.
    {
        sh::ClientConfig cfg;
        cfg.default_version = sh::HttpVersionPolicy::Http11;
        cfg.default_h2c = sh::H2cMode::Off;
        cfg.max_redirects = 5;
        cfg.resolve =
            [](std::string,
               std::string port) -> asio::awaitable<std::pair<sh::error_code, std::vector<asio::ip::tcp::endpoint>>> {
            std::vector<asio::ip::tcp::endpoint> endpoints;
            endpoints.emplace_back(asio::ip::make_address("127.0.0.1"), static_cast<std::uint16_t>(std::stoul(port)));
            co_return std::pair{sh::error_code{}, std::move(endpoints)};
        };
        h::Client client{co_await asio::this_coro::executor, cfg};
        const auto opened_before = client.stats().connections_opened;
        auto cross = co_await fetch(client, client.get(base + "/redir-host").header("authorization", "Bearer secret"));
        check(cross && cross->body == "no-auth", "HTTP/1.1: a cross-origin redirect uses a throwaway connection");
        auto back = co_await fetch(client, client.get(base + "/redir/a"));
        check(back && back->status == 200 && back->body == "landed",
              "HTTP/1.1: the kept connection still serves after a cross-origin hop");
        check(client.stats().connections_opened == opened_before + 2,
              "HTTP/1.1: one kept + one throwaway connection for the cross-origin hop");
    }
}

asio::awaitable<void> suite_retry(asio::io_context &ctx) {
    std::printf("\n== configurable retry (RetryPolicy) ==\n");
    const std::string ok = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";

    // A connection that dies before answering is retried up to max_retries.
    {
        FlakyPeer &peer = make_flaky(ctx, /*fails=*/1, ok);
        sh::ClientConfig cfg = base_config();
        cfg.retry.max_retries = 1;
        cfg.retry.initial_backoff = std::chrono::milliseconds(5);
        h::Client client{co_await asio::this_coro::executor, cfg};
        auto r = co_await fetch(client, client.get(url(peer.port(), "/x")));
        check(r && r->status == 200 && r->body == "ok", "a dropped connection is retried and succeeds");
        check(peer.connections() == 2, "exactly one retry reached the peer (2 connections)");
    }
    // The free transparent re-dial plus one policy retry absorb two failures;
    // the third connection answers.
    {
        FlakyPeer &peer = make_flaky(ctx, /*fails=*/2, ok);
        sh::ClientConfig cfg = base_config();
        cfg.retry.max_retries = 1;
        cfg.retry.initial_backoff = std::chrono::milliseconds(5);
        h::Client client{co_await asio::this_coro::executor, cfg};
        auto r = co_await fetch(client, client.get(url(peer.port(), "/x")));
        check(r && r->status == 200 && r->body == "ok",
              "two dropped connections are absorbed by the free re-dial + one policy retry");
        check(peer.connections() == 3, "two failures, three connections, then success");
    }
    // When every attempt fails — the free re-dial and the policy retry all hit
    // a dead peer — the last transport error surfaces.
    {
        FlakyPeer &peer = make_flaky(ctx, /*fails=*/3, ok);
        sh::ClientConfig cfg = base_config();
        cfg.retry.max_retries = 1;
        cfg.retry.initial_backoff = std::chrono::milliseconds(5);
        h::Client client{co_await asio::this_coro::executor, cfg};
        auto r = co_await fetch(client, client.get(url(peer.port(), "/x")));
        check(!r, "retries exhausted: the transport error surfaces: " + describe(r.error()));
        check(peer.connections() == 3, "three attempts reached the peer (free re-dial + one policy retry)");
    }
    // A POST is never *policy*-retried (a duplicate could double a side
    // effect), even when retries are configured: beyond the single free
    // transparent re-dial, a second drop surfaces the error.
    {
        FlakyPeer &peer = make_flaky(ctx, /*fails=*/2, ok);
        sh::ClientConfig cfg = base_config();
        cfg.retry.max_retries = 3;
        cfg.retry.initial_backoff = std::chrono::milliseconds(5);
        h::Client client{co_await asio::this_coro::executor, cfg};
        auto r = co_await fetch(client, client.post(url(peer.port(), "/x")));
        check(!r, "a non-idempotent POST is never policy-retried: " + describe(r.error()));
        check(peer.connections() == 2, "only the single free re-dial ran for the POST, no policy retries");
    }
    // A custom condition opts a non-idempotent request into policy retries too.
    {
        FlakyPeer &peer = make_flaky(ctx, /*fails=*/1, ok);
        sh::ClientConfig cfg = base_config();
        cfg.retry.max_retries = 2;
        cfg.retry.initial_backoff = std::chrono::milliseconds(5);
        cfg.retry.condition = [](sh::error_code, sh::Method, bool) { return true; };
        h::Client client{co_await asio::this_coro::executor, cfg};
        auto r = co_await fetch(client, client.post(url(peer.port(), "/x")));
        check(r && r->status == 200 && r->body == "ok", "a custom condition allows the POST retry");
        check(peer.connections() == 2, "the POST was retried once");
    }
    // max_retries = 0 disables *policy* retries; the single kept connection is
    // still re-dialed once on transport loss (the free transparent re-dial).
    {
        FlakyPeer &peer = make_flaky(ctx, /*fails=*/1, ok);
        h::Client client{co_await asio::this_coro::executor, base_config()};
        auto r = co_await fetch(client, client.get(url(peer.port(), "/x")));
        check(r && r->status == 200 && r->body == "ok",
              "with no retry policy a dropped connection still gets the single free re-dial");
        check(peer.connections() == 2, "the free re-dial ran without any policy");
    }
    // Retries and backoff are bounded by the request's overall deadline: a peer
    // that keeps failing cannot stretch a small request_timeout into several
    // attempts and several seconds of backoff.
    {
        FlakyPeer &peer = make_flaky(ctx, /*fails=*/5, ok);
        sh::ClientConfig cfg = base_config();
        cfg.retry.max_retries = 5;
        cfg.retry.initial_backoff = std::chrono::milliseconds(200);
        cfg.retry.backoff_multiplier = 2.0;
        cfg.request_timeout = std::chrono::milliseconds(500);
        h::Client client{co_await asio::this_coro::executor, cfg};
        const auto t0 = std::chrono::steady_clock::now();
        auto r = co_await fetch(client, client.get(url(peer.port(), "/x")));
        const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
        check(!r, "a persistent failure with retries still reports an error: " + describe(r.error()));
        check(dt < std::chrono::milliseconds(2500),
              "the request deadline held the retries (" + std::to_string(dt.count()) + "ms)");
        check(peer.connections() < 6, "not every configured retry ran once the budget ran out (" +
                                          std::to_string(peer.connections()) + " connections)");
    }
}

asio::awaitable<void> suite_websocket(std::uint16_t port) {
    std::printf("\n== client WebSocket (open_websocket) ==\n");
    const std::string base = "ws://127.0.0.1:" + std::to_string(port);

    h::Client client{co_await asio::this_coro::executor, base_config()};
    auto ws = co_await client.open_websocket(base + "/wsecho");
    if (!ws) {
        check(false, "open_websocket handshake -> " + std::string{ws.error().message()});
        co_return;
    }
    check(ws.has_value(), "client opens a WebSocket to the server's /echo");

    // Echo round-trip over WebSocket (text).
    if (auto ec = co_await (*ws)->write_text("hello ws"); ec) {
        check(false, "write_text -> " + ec.message());
        co_return;
    }
    auto msg = co_await (*ws)->read();
    check(msg && msg->text && msg->data == "hello ws", "text echo round-trips");

    // Binary echo keeps the type flag.
    if (auto ec = co_await (*ws)->write_binary(std::string{"\x00\x01\x02", 3}); ec) {
        check(false, "write_binary -> " + ec.message());
        co_return;
    }
    auto bin = co_await (*ws)->read();
    check(bin && !bin->text && bin->data == std::string{"\x00\x01\x02", 3}, "binary echo keeps type and bytes");

    // Graceful close.
    if (auto ec = co_await (*ws)->close(); ec) {
        check(false, "close -> " + ec.message());
        co_return;
    }
    auto after = co_await (*ws)->read();
    check(!after, "read after close returns an error (not an infinite loop)");
    check(!(*ws)->is_open(), "is_open() is false after close");

    // Byte-level reverse proxy: the frontend handshake is rebuilt with the
    // route's header policy, then the tunnel is spliced to the backend.
    auto proxied = co_await client.open_websocket(base + "/wsproxy-h1");
    if (!proxied) {
        check(false, "open_websocket through ws_proxy -> " + std::string{proxied.error().message()});
        co_return;
    }
    if (auto ec = co_await (*proxied)->write_text("via proxy"); ec) {
        check(false, "ws_proxy write_text -> " + ec.message());
    } else {
        auto echo = co_await (*proxied)->read();
        check(echo && echo->text && echo->data == "via proxy", "ws_proxy echoes through the tunnel");
    }
    (void)co_await (*proxied)->close();

    // A peer that accepts the connection and then never answers the Upgrade
    // must not park open_websocket() forever: the handshake shares the
    // response-head budget with ordinary requests.
    {
        asio::io_context peer_ctx;
        auto silent = std::make_shared<FakeServer>(peer_ctx, "", /*close_after=*/false);
        std::thread peer_thread([&peer_ctx] { peer_ctx.run(); });

        auto cfg = base_config();
        cfg.response_head_timeout = std::chrono::milliseconds(150);
        h::Client slow{co_await asio::this_coro::executor, cfg};
        const auto start = std::chrono::steady_clock::now();
        auto r = co_await slow.open_websocket("ws://127.0.0.1:" + std::to_string(silent->port()) + "/silent");
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        check(!r && r.error() == sh::client_errc::response_head_timeout,
              "open_websocket times out when the peer never answers the Upgrade -> " +
                  (r ? std::string{"unexpected success"} : describe(r.error())));
        check(elapsed < std::chrono::seconds(2), "the WebSocket handshake timeout is bounded");

        peer_ctx.stop();
        peer_thread.join();
    }
}

asio::awaitable<void> suite_multipart(std::uint16_t port) {
    std::printf("\n== client multipart upload (MultipartForm) ==\n");
    const std::string base = "http://127.0.0.1:" + std::to_string(port);
    h::Client client{co_await asio::this_coro::executor, base_config()};

    // A field and a file round-trip through the server's multipart parser.
    {
        sh::MultipartForm form;
        form.field("title", "hello");
        form.field("tags", "a,b");
        form.file("upload", "notes.txt", "text/plain", "file data");
        auto r = co_await fetch(client, client.post(base + "/mp").multipart(std::move(form)));
        check(r && r->status == 200 && r->body == "field:title=hello|field:tags=a,b|file:upload:notes.txt:text/plain:9",
              "a multipart field+file upload round-trips through the server parser");
    }
    // The 3-arg file() infers the content type from the filename extension
    // (mixed case: mime::by_extension lowercases it).
    {
        sh::MultipartForm form;
        form.file("up", "photo.PNG", "\x89PNG");
        auto r = co_await fetch(client, client.post(base + "/mp").multipart(std::move(form)));
        check(r && r->status == 200 && r->body == "file:up:photo.PNG:image/png:4",
              "file() infers a content type from the filename extension");
    }
    // multipart() replaces a content type set earlier instead of duplicating it.
    {
        sh::MultipartForm form;
        form.field("k", "v");
        auto r =
            co_await fetch(client, client.post(base + "/mp").content_type("text/plain").multipart(std::move(form)));
        check(r && r->status == 200 && r->body == "field:k=v", "multipart() replaces a content type set earlier");
    }
    // An empty form is a valid (empty) submission: just the closing delimiter.
    {
        sh::MultipartForm form;
        auto r = co_await fetch(client, client.post(base + "/mp").multipart(std::move(form)));
        check(r && r->status == 200 && r->body.empty(), "an empty multipart form posts an empty part set");
    }
    // The raw wire: Go-style quoting keeps `"`/`\` inside one quoted pair, and
    // the body closes with the --boundary-- delimiter.
    {
        sh::MultipartForm form{"B"};
        form.field("a\"b", "v\\w");
        const std::string body = form.body();
        check(body.find("name=\"a\\\"b\"") != std::string::npos && body.find("v\\w") != std::string::npos &&
                  body.find("--B--\r\n") != std::string::npos,
              "names/values are quoted on the wire (Go escapeQuotes)");
    }
    // An explicit boundary is exposed verbatim in Content-Type.
    {
        sh::MultipartForm form{"TESTBOUNDARY"};
        check(form.content_type() == "multipart/form-data; boundary=TESTBOUNDARY",
              "explicit boundary shows up in the content type");
        check(form.boundary() == "TESTBOUNDARY", "boundary() mirrors the explicit value");
    }
    // Generated boundaries differ per form and the body length is exact.
    {
        sh::MultipartForm a;
        sh::MultipartForm b;
        check(a.boundary().size() > 10 && a.boundary() != b.boundary(), "boundaries are generated per form");
        a.field("x", "y");
        check(a.content_length() == a.body().size(), "content_length() matches the serialized body");
    }
}

// --- one Client pinned to an executor, driven from two threads ----------------
// A single Client armed with the engine pinned to one io_context (model A),
// used from two threads in sequence, each thread with its own io_context.
// Thread 1 runs on the pinned context and executes its requests there; thread
// 2 starts only after thread 1 is done, so its requests dispatch onto the
// pinned context (hop-back) and serialize there. This is the genuine
// cross-thread guarantee of the pinned client: whoever initiates a request,
// every exchange runs on the pinned executor — and no lock is involved, since
// that executor is the only place the engine's state is ever touched. Teardown
// destroys the Client before the contexts' final drain: the engine's release of
// the kept session is posted onto the pinned context, which that drain must
// still run (the "io_context outlives its sessions" contract).
asio::awaitable<void> suite_shared_client(std::uint16_t port) {
    std::printf("\n== one Client pinned to an executor, driven from 2 threads ==\n");
    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/world?n=64";
    auto cfg = base_config();
    cfg.default_version = sh::HttpVersionPolicy::Http11;
    cfg.default_h2c = sh::H2cMode::Off;

    constexpr int kThreads = 2;
    constexpr int kPerThread = 16;
    constexpr int kTotal = kThreads * kPerThread;
    std::atomic<int> ok{0}, bad{0};
    std::atomic<int> pending{kTotal};
    std::atomic<bool> phase1_done{false}; // thread 1 finished its requests
    std::atomic<bool> timed_out{false};

    // The pinned executor: thread 0 below both drives this context and runs
    // its own phase of requests; thread 1's requests dispatch onto it.
    auto pin_ctx = std::make_shared<asio::io_context>();
    auto client = std::make_shared<sh::Client>(pin_ctx->get_executor(), cfg);

    auto worker = [&]() -> asio::awaitable<void> {
        for (int i = 0; i < kPerThread; ++i) {
            auto r = co_await client->get(url).send();
            if (r && r->status() == 200)
                ++ok;
            else
                ++bad;
            --pending;
        }
    };

    std::vector<std::shared_ptr<asio::io_context>> ctxs;
    std::vector<asio::executor_work_guard<asio::io_context::executor_type>> guards;
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        auto ctx = t == 0 ? pin_ctx : std::make_shared<asio::io_context>();
        // A work guard keeps the io_context's service alive across the
        // stop/restart/poll dance in the runners and the teardown (asio can
        // retire an io_context's impl once run()/poll() returns with no work),
        // so the kept session's executor — a raw io_context pointer — stays
        // valid until the guards die with this scope.
        guards.emplace_back(asio::make_work_guard(*ctx));
        ctxs.push_back(ctx);
        auto w = worker;
        if (t == 0) {
            asio::co_spawn(
                *ctx,
                [w, &phase1_done]() -> asio::awaitable<void> {
                    co_await w();
                    phase1_done.store(true, std::memory_order_release);
                },
                asio::detached);
        }
        workers.emplace_back([&, t, ctx, w] {
            if (t > 0) {
                // Phase 2: wait until thread 1's requests are done (the kept
                // connection is bound to the pinned context and idle), then run
                // this thread's requests — each one hops onto that context,
                // where the first thread's loop executes it.
                const auto r0 = std::chrono::steady_clock::now();
                while (!phase1_done.load(std::memory_order_acquire) &&
                       std::chrono::steady_clock::now() - r0 < std::chrono::seconds{30})
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                asio::co_spawn(*ctx, w, asio::detached);
            }
            // Keep this context polling until ALL work (including the hops this
            // thread dispatched onto the pinned context) is done, so no context
            // dies while an op is still queued for it. A stall is capped at 30s
            // so a bug fails the check instead of hanging the run.
            const auto w0 = std::chrono::steady_clock::now();
            while (pending.load() > 0 && std::chrono::steady_clock::now() - w0 < std::chrono::seconds(30))
                ctx->run_for(std::chrono::milliseconds(5));
            if (pending.load() > 0)
                timed_out.store(true);
            // Drain everything still queued before the context is destroyed.
            ctx->restart();
            while (ctx->poll())
                ;
        });
    }
    for (auto &w : workers)
        w.join();

    // Drain the contexts first so no request op is still queued, then destroy
    // the Client (the engine posts the kept session's release onto the pinned
    // context), then drain again so that release runs while the contexts are
    // still alive. Only then do the contexts themselves die — the
    // "io_context outlives its sessions" contract, made explicit.
    for (auto &ctx : ctxs) {
        ctx->restart();
        while (ctx->poll())
            ;
    }
    const auto opened = client->stats().connections_opened;
    client.reset();
    for (auto &ctx : ctxs) {
        ctx->restart();
        while (ctx->poll())
            ;
    }

    check(ok == kTotal && bad == 0 && !timed_out.load(),
          "every request through the shared Client landed on the pinned executor (" + std::to_string(ok) + " ok, " +
              std::to_string(bad) + " bad)");
    check(opened == 1, "a pinned Client still keeps exactly one connection (" + std::to_string(opened) + ")");
    co_return;
}

// --- outbound proxies -------------------------------------------------------

asio::awaitable<void> suite_proxy(asio::io_context &ctx, std::uint16_t plain_port, std::uint16_t tls_port) {
    std::printf("\n== outbound proxies (CONNECT / HTTPS proxy / SOCKS5) ==\n");
    const auto ex = co_await asio::this_coro::executor;
    const std::string http_url = "http://127.0.0.1:" + std::to_string(plain_port);
    const std::string https_url = "https://127.0.0.1:" + std::to_string(tls_port);

    // Plaintext CONNECT proxy: HTTP/1.1 origin.
    {
        auto &proxy = make_proxy(ctx, TestProxyKind::Http);
        sh::ClientConfig cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http11;
        cfg.default_h2c = sh::H2cMode::Off;
        cfg.proxy = sh::ProxyConfig{.kind = sh::ProxyKind::Http, .host = "127.0.0.1", .port = proxy.port()};
        h::Client client{ex, cfg};
        auto r = co_await fetch(client, client.get(http_url + "/world"));
        check(r && r->status == 200, "CONNECT proxy: plaintext origin over HTTP/1.1");
        check(proxy.tunnels() == 1 && proxy.connections() == 1, "CONNECT proxy: exactly one tunnel for one request");
    }

    // Plaintext CONNECT proxy: h2c prior knowledge inside the tunnel.
    {
        auto &proxy = make_proxy(ctx, TestProxyKind::Http);
        sh::ClientConfig cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http2;
        cfg.default_h2c = sh::H2cMode::PriorKnowledge;
        cfg.proxy = sh::ProxyConfig{.kind = sh::ProxyKind::Http, .host = "127.0.0.1", .port = proxy.port()};
        h::Client client{ex, cfg};
        auto r = co_await fetch(client, client.get(http_url + "/world"));
        check(r && r->status == 200 && r->version == sh::Version::Http2,
              "CONNECT proxy: a pinned h2c policy sends the preface inside the tunnel");
    }

    // Plaintext CONNECT proxy: h2c Upgrade still works through the tunnel.
    {
        auto &proxy = make_proxy(ctx, TestProxyKind::Http);
        sh::ClientConfig cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Auto;
        cfg.default_h2c = sh::H2cMode::Upgrade;
        cfg.proxy = sh::ProxyConfig{.kind = sh::ProxyKind::Http, .host = "127.0.0.1", .port = proxy.port()};
        h::Client client{ex, cfg};
        auto r = co_await fetch(client, client.get(http_url + "/world"));
        check(r && r->status == 200 && r->version == sh::Version::Http2,
              "CONNECT proxy: h2c Upgrade runs over the tunnel");
    }

    // Plaintext CONNECT proxy: a TLS origin keeps its own ALPN (h2).
    {
        auto &proxy = make_proxy(ctx, TestProxyKind::Http);
        sh::ClientConfig cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http2;
        cfg.proxy = sh::ProxyConfig{.kind = sh::ProxyKind::Http, .host = "127.0.0.1", .port = proxy.port()};
        h::Client client{ex, cfg};
        auto r = co_await fetch(client, client.get(https_url + "/world"));
        check(r && r->status == 200 && r->version == sh::Version::Http2,
              "CONNECT proxy: TLS origin negotiates h2 inside the tunnel");
    }

    // Plaintext CONNECT proxy: ws://.
    {
        auto &proxy = make_proxy(ctx, TestProxyKind::Http);
        sh::ClientConfig cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http11;
        cfg.default_h2c = sh::H2cMode::Off;
        cfg.proxy = sh::ProxyConfig{.kind = sh::ProxyKind::Http, .host = "127.0.0.1", .port = proxy.port()};
        h::Client client{ex, cfg};
        auto ws = co_await client.open_websocket("ws://127.0.0.1:" + std::to_string(plain_port) + "/wsecho");
        if (!ws) {
            check(false, "CONNECT proxy: ws:// handshake -> " + std::string{ws.error().message()});
        } else {
            check(true, "CONNECT proxy: ws:// handshake through the tunnel");
            if (auto ec = co_await (*ws)->write_text("hello proxy"); ec) {
                check(false, "CONNECT proxy: ws write -> " + ec.message());
            } else {
                auto msg = co_await (*ws)->read();
                check(msg && msg->text && msg->data == "hello proxy", "CONNECT proxy: ws echo through the tunnel");
            }
            (void)co_await (*ws)->close();
        }
    }

    // HTTPS proxy (TLS to the proxy): a plaintext origin inside the TLS tunnel.
    {
        auto &proxy = make_proxy(ctx, TestProxyKind::Https);
        sh::ClientConfig cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Auto;
        cfg.default_h2c = sh::H2cMode::Upgrade;
        cfg.proxy = sh::ProxyConfig{.kind = sh::ProxyKind::Https,
                                    .host = "127.0.0.1",
                                    .port = proxy.port(),
                                    .verify_peer = true,
                                    .verify_host = false, // the test certificate has no SAN
                                    .ca_file = "./test/tls_certificates/ca_cert.pem"};
        h::Client client{ex, cfg};
        auto r = co_await fetch(client, client.get(http_url + "/world"));
        check(r && r->status == 200 && r->version == sh::Version::Http2,
              "HTTPS proxy: plaintext origin upgrades to h2c inside the TLS tunnel");
    }

    // HTTPS proxy: a TLS origin over nested TLS (TLS over TLS), h2.
    {
        auto &proxy = make_proxy(ctx, TestProxyKind::Https);
        sh::ClientConfig cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http2;
        cfg.proxy = sh::ProxyConfig{.kind = sh::ProxyKind::Https,
                                    .host = "127.0.0.1",
                                    .port = proxy.port(),
                                    .verify_peer = true,
                                    .verify_host = false,
                                    .ca_file = "./test/tls_certificates/ca_cert.pem"};
        h::Client client{ex, cfg};
        auto r = co_await fetch(client, client.get(https_url + "/world"));
        check(r && r->status == 200 && r->version == sh::Version::Http2,
              "HTTPS proxy: TLS origin over nested TLS, h2 via ALPN");
    }

    // HTTPS proxy: wss:// through nested TLS.
    {
        auto &proxy = make_proxy(ctx, TestProxyKind::Https);
        sh::ClientConfig cfg = base_config();
        cfg.proxy = sh::ProxyConfig{.kind = sh::ProxyKind::Https,
                                    .host = "127.0.0.1",
                                    .port = proxy.port(),
                                    .verify_peer = true,
                                    .verify_host = false,
                                    .ca_file = "./test/tls_certificates/ca_cert.pem"};
        h::Client client{ex, cfg};
        auto ws = co_await client.open_websocket("wss://127.0.0.1:" + std::to_string(tls_port) + "/wsecho");
        if (!ws) {
            check(false, "HTTPS proxy: wss:// handshake -> " + std::string{ws.error().message()});
        } else {
            check(true, "HTTPS proxy: wss:// handshake through nested TLS");
            if (auto ec = co_await (*ws)->write_text("nested"); ec) {
                check(false, "HTTPS proxy: wss write -> " + ec.message());
            } else {
                auto msg = co_await (*ws)->read();
                check(msg && msg->text && msg->data == "nested", "HTTPS proxy: wss echo through nested TLS");
            }
            (void)co_await (*ws)->close();
        }
    }

    // SOCKS5, no authentication, remote DNS (a name goes out as ATYP=3).
    {
        auto &proxy = make_proxy(ctx, TestProxyKind::Socks5);
        sh::ClientConfig cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http11;
        cfg.default_h2c = sh::H2cMode::Off;
        cfg.proxy = sh::ProxyConfig{.kind = sh::ProxyKind::Socks5, .host = "127.0.0.1", .port = proxy.port()};
        h::Client client{ex, cfg};
        auto r = co_await fetch(client, client.get("http://localhost:" + std::to_string(plain_port) + "/world"));
        check(r && r->status == 200, "SOCKS5: plaintext origin through the tunnel");
        check(proxy.last_host() == "localhost", "SOCKS5: the target name is resolved at the proxy (ATYP=3)");
    }

    // SOCKS5 with username/password; TLS origin keeps h2 via ALPN.
    {
        auto &proxy = make_proxy(ctx, TestProxyKind::Socks5, "user", "pass");
        sh::ClientConfig cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http2;
        cfg.proxy = sh::ProxyConfig{.kind = sh::ProxyKind::Socks5,
                                    .host = "127.0.0.1",
                                    .port = proxy.port(),
                                    .username = "user",
                                    .password = "pass"};
        h::Client client{ex, cfg};
        auto r = co_await fetch(client, client.get(https_url + "/world"));
        check(r && r->status == 200 && r->version == sh::Version::Http2,
              "SOCKS5: authenticated tunnel carries a TLS origin (h2)");
        check(proxy.auth_failures() == 0, "SOCKS5: credentials accepted");
    }

    // Authentication failures map to proxy_auth_failed.
    {
        auto &proxy = make_proxy(ctx, TestProxyKind::Http, "user", "pass");
        sh::ClientConfig cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http11;
        cfg.default_h2c = sh::H2cMode::Off;
        cfg.proxy = sh::ProxyConfig{.kind = sh::ProxyKind::Http, .host = "127.0.0.1", .port = proxy.port()};
        h::Client client{ex, cfg};
        auto r = co_await fetch(client, client.get(http_url + "/world"));
        check(!r && r.error() == sh::make_error_code(sh::client_errc::proxy_auth_failed),
              "CONNECT proxy: a 407 surfaces as proxy_auth_failed");
        check(proxy.auth_failures() == 1, "CONNECT proxy: the rejection reached the proxy");
    }
    {
        auto &proxy = make_proxy(ctx, TestProxyKind::Socks5, "user", "pass");
        sh::ClientConfig cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http11;
        cfg.default_h2c = sh::H2cMode::Off;
        cfg.proxy = sh::ProxyConfig{.kind = sh::ProxyKind::Socks5, .host = "127.0.0.1", .port = proxy.port()};
        h::Client client{ex, cfg};
        auto r = co_await fetch(client, client.get(http_url + "/world"));
        check(!r && r.error() == sh::make_error_code(sh::client_errc::proxy_auth_failed),
              "SOCKS5: no acceptable auth method surfaces as proxy_auth_failed");
    }
    {
        auto &proxy = make_proxy(ctx, TestProxyKind::Socks5, "user", "pass");
        sh::ClientConfig cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http11;
        cfg.default_h2c = sh::H2cMode::Off;
        cfg.proxy = sh::ProxyConfig{.kind = sh::ProxyKind::Socks5,
                                    .host = "127.0.0.1",
                                    .port = proxy.port(),
                                    .username = "user",
                                    .password = "wrong"};
        h::Client client{ex, cfg};
        auto r = co_await fetch(client, client.get(http_url + "/world"));
        check(!r && r.error() == sh::make_error_code(sh::client_errc::proxy_auth_failed),
              "SOCKS5: a rejected password surfaces as proxy_auth_failed");
    }

    // Malformed and refused replies map to proxy_error.
    {
        auto &proxy = make_proxy(ctx, TestProxyKind::Http, {}, {}, 0, /*malformed=*/true);
        sh::ClientConfig cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http11;
        cfg.default_h2c = sh::H2cMode::Off;
        cfg.proxy = sh::ProxyConfig{.kind = sh::ProxyKind::Http, .host = "127.0.0.1", .port = proxy.port()};
        h::Client client{ex, cfg};
        auto r = co_await fetch(client, client.get(http_url + "/world"));
        check(!r && r.error() == sh::make_error_code(sh::client_errc::proxy_error),
              "CONNECT proxy: a malformed reply surfaces as proxy_error");
    }
    {
        auto &proxy = make_proxy(ctx, TestProxyKind::Http, {}, {}, /*http_status=*/403);
        sh::ClientConfig cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http11;
        cfg.default_h2c = sh::H2cMode::Off;
        cfg.proxy = sh::ProxyConfig{.kind = sh::ProxyKind::Http, .host = "127.0.0.1", .port = proxy.port()};
        h::Client client{ex, cfg};
        auto r = co_await fetch(client, client.get(http_url + "/world"));
        check(!r && r.error() == sh::make_error_code(sh::client_errc::proxy_error),
              "CONNECT proxy: a non-2xx, non-407 status surfaces as proxy_error");
    }

    // NO_PROXY bypasses an explicit proxy: the request works and the proxy is
    // never even connected to.
    {
        auto &proxy = make_proxy(ctx, TestProxyKind::Http);
        sh::ClientConfig cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http11;
        cfg.default_h2c = sh::H2cMode::Off;
        cfg.proxy = sh::ProxyConfig{.kind = sh::ProxyKind::Http, .host = "127.0.0.1", .port = proxy.port()};
        cfg.no_proxy = {"127.0.0.1"};
        h::Client client{ex, cfg};
        auto r = co_await fetch(client, client.get(http_url + "/world"));
        check(r && r->status == 200, "NO_PROXY: a bypassed target is reached directly");
        check(proxy.connections() == 0, "NO_PROXY: the proxy was never connected to");
    }

    // Environment selection: http_proxy is honored when no explicit proxy is
    // configured (proxy_from_env defaults true).
    {
        auto &proxy = make_proxy(ctx, TestProxyKind::Http);
        const std::string value = "http://127.0.0.1:" + std::to_string(proxy.port());
        ::setenv("http_proxy", value.c_str(), 1);
        ::unsetenv("no_proxy");
        sh::ClientConfig cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http11;
        cfg.default_h2c = sh::H2cMode::Off;
        h::Client client{ex, cfg};
        auto r = co_await fetch(client, client.get(http_url + "/world"));
        ::unsetenv("http_proxy");
        check(r && r->status == 200, "environment proxy: http_proxy is honored");
        check(proxy.tunnels() == 1, "environment proxy: the request went through the tunnel");
    }

    // A server-side reverse-proxy upstream must not follow the process's proxy
    // environment (HttpProxyTarget::use_env_proxy defaults off): the same
    // frontend server serves /rp/* through an internal Client, and that request
    // must reach the backend directly with HTTP_PROXY set.
    {
        auto &proxy = make_proxy(ctx, TestProxyKind::Http);
        const std::string value = "http://127.0.0.1:" + std::to_string(proxy.port());
        ::setenv("http_proxy", value.c_str(), 1);
        ::unsetenv("no_proxy");
        // The *front* client stays direct (otherwise it would take the tunnel
        // itself); only the server-side upstream could follow HTTP_PROXY, and it
        // must not.
        sh::ClientConfig front_cfg = base_config();
        front_cfg.proxy_from_env = false;
        h::Client client{ex, front_cfg};
        auto r = co_await fetch(client, client.get(http_url + "/rp/world"));
        ::unsetenv("http_proxy");
        check(r && r->status == 200, "reverse-proxy upstream: the front still answers with HTTP_PROXY set");
        check(proxy.tunnels() == 0, "reverse-proxy upstream: the backend was reached directly, not via the proxy");
    }
}

asio::awaitable<void> run_all_suites(asio::io_context &ctx, std::uint16_t plain, std::uint16_t tls_port,
                                     std::uint16_t comp_port) {
    co_await suite_protocol_matrix(plain, tls_port);
    co_await suite_framing(plain, ctx);
    co_await suite_streaming(plain);
    co_await suite_abort_gate(plain);
    co_await suite_h2_multiplex(plain);
    co_await suite_errors(plain);
    co_await suite_spec_validation(plain);
    co_await suite_tls(tls_port);
    co_await suite_redirect(plain);
    co_await suite_retry(ctx);
    co_await suite_shared_client(plain);
    co_await suite_websocket(plain);
    co_await suite_multipart(plain);
    co_await suite_http_api(plain);
    co_await suite_reverse_proxy(plain);
    co_await suite_proxy(ctx, plain, tls_port);
    co_await suite_raw_peer(ctx);
    co_await suite_compression(comp_port, plain);
    co_return;
}

} // namespace

int main() {
    sh::set_log_sink(sh::make_stdout_sink(sh::LogLevel::Error));

    // Proxy environment isolation: this harness must not inherit an ambient
    // HTTP(S)_PROXY (CI machines and developer shells often have one). The proxy
    // suite sets and restores these explicitly where it needs them.
    for (const char *name :
         {"HTTP_PROXY", "http_proxy", "HTTPS_PROXY", "https_proxy", "ALL_PROXY", "all_proxy", "NO_PROXY", "no_proxy"}) {
        ::unsetenv(name);
    }

    // The plaintext and TLS listeners of the library's own server, in-process.
    // Fixed ports, because the reverse-proxy routes below must name the backend's
    // port and are registered before the servers start (the route table is not
    // mutated while workers serve).
    constexpr std::uint16_t kPlainPort = 27910;
    constexpr std::uint16_t kTlsPort = 27911;
    constexpr std::uint16_t kCompPort = 27912;
    sh::ServerConfig plain_cfg = server_config(kPlainPort, std::nullopt);
    sh::Server plain{plain_cfg};
    // The proxy's client role must trust the test CA (and present the client
    // certificate) to reach this process's own mTLS listener. The policy is
    // attached to the route whose backend is that listener — each proxy route
    // owns its client, so the plaintext-backend route keeps the public default.
    sh::ClientConfig proxy_cfg;
    proxy_cfg.tls.ca_file = "./test/tls_certificates/ca_cert.pem";
    proxy_cfg.tls.cert_chain_file = "./test/tls_certificates/client_cert.pem";
    proxy_cfg.tls.private_key_file = "./test/tls_certificates/client_key.pem";
    proxy_cfg.tls.verify_host = false; // the test certificate has no SAN
    register_routes(plain);
    // Redirect targets for suite_redirect (convenience-layer following).
    plain.route(sh::any_methods, "/redir/a", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->redirect("/redir/b").send("");
    });
    plain.route(sh::any_methods, "/redir/b", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).send("landed");
    });
    plain.route(sh::any_methods, "/redir/post", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->redirect("/redir/b", sh::status::see_other).send("");
    });
    plain.route(sh::any_methods, "/redir/307", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->redirect("/redir/echo-method", sh::status::temporary_redirect).send("");
    });
    plain.route(sh::any_methods, "/redir/echo-method",
                [](sh::RequestPtr req, sh::ResponsePtr res) -> asio::awaitable<void> {
                    co_await res->status(200).send(std::string{sh::to_string(req->method())});
                });
    plain.route(sh::any_methods, "/dir/start", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->redirect("b").send(""); // relative Location
    });
    plain.route(sh::any_methods, "/dir/b", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).send("relative");
    });
    plain.route(sh::any_methods, "/redir/cookie", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->header(sh::field::set_cookie, "sid=abc123; Path=/").redirect("/redir/cookie-check").send("");
    });
    plain.route(sh::any_methods, "/redir/cookie-check",
                [](sh::RequestPtr req, sh::ResponsePtr res) -> asio::awaitable<void> {
                    const auto cookie = req->header(sh::field::cookie);
                    co_await res->status(200).send(cookie ? std::string{*cookie} : "none");
                });
    // A hop to a different host must drop credential headers: the Location
    // names "localhost" while the client dialed "127.0.0.1" (suite_redirect
    // pins both to IPv4 with a resolve hook).
    plain.route(sh::any_methods, "/redir-host", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->redirect("http://localhost:" + std::to_string(kPlainPort) + "/redir-host-target").send("");
    });
    plain.route(sh::any_methods, "/redir-host-target",
                [](sh::RequestPtr req, sh::ResponsePtr res) -> asio::awaitable<void> {
                    const auto auth = req->header("authorization");
                    co_await res->status(200).send(auth ? std::string{*auth} : "no-auth");
                });
    // A redirection loop: the convenience layer must stop at max_redirects with
    // an error rather than chase the redirect forever.
    plain.route(sh::any_methods, "/redir/loop", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->redirect("/redir/loop").send(""); // to itself
    });
    sh::TlsConfig tls_cfg;
    tls_cfg.cert_chain_file = "./test/tls_certificates/server_cert.pem";
    tls_cfg.private_key_file = "./test/tls_certificates/server_key.pem";
    tls_cfg.mutual = true; // mutual TLS: the client must present its certificate
    tls_cfg.ca_file = std::string{"./test/tls_certificates/ca_cert.pem"};
    sh::Server secure{server_config(kTlsPort, tls_cfg)};
    register_routes(secure);
    // A third listener with response compression on. Its own port keeps the
    // byte-for-byte body assertions of every other suite valid. Compression is
    // mounted as middleware now (the chi/gin/echo shape) — no server config.
    sh::ServerConfig comp_cfg = server_config(kCompPort, std::nullopt);
    sh::Server compressed{comp_cfg};
    compressed.use(sh::middleware::compress({.min_bytes = 64})); // /world is 19 B: it must stay as-is
    register_routes(compressed);
    // Reverse-proxy routes: one to the plaintext listener (the historical
    // behaviour: a plaintext backend stays HTTP/1.1) and one to the TLS listener
    // (the backend leg is HTTPS, so ALPN decides — and offers h2).
    plain.http_proxy_regex("/rp/(.*)", "127.0.0.1", kPlainPort, "/$1");
    // Byte-level WebSocket proxy (h1 frontend): /wsproxy-h1 is spliced to this
    // same listener's /wsecho, so the client-side suite exercises the upstream
    // handshake and tunnel end to end.
    plain.ws_proxy("/wsproxy-h1", "127.0.0.1", kPlainPort, "/wsecho");
    {
        sh::HttpProxyTarget backend;
        backend.host = "127.0.0.1";
        backend.port = kPlainPort;
        backend.rewrite_path = "/$1";
        backend.preserve_inbound_forwarding = true;
        plain.http_proxy_regex("/rpxff/(.*)", std::move(backend));
    }
    {
        // Host policy: keep the frontend's Host instead of the backend authority.
        sh::HttpProxyTarget backend;
        backend.host = "127.0.0.1";
        backend.port = kPlainPort;
        backend.rewrite_path = "/headers";
        backend.preserve_host = true;
        plain.http_proxy("/rphost", std::move(backend));
    }
    {
        sh::HttpProxyTarget backend;
        backend.host = "127.0.0.1";
        backend.port = kPlainPort;
        backend.rewrite_path = "/method-token";
        plain.http_proxy("/rpmethod", std::move(backend));
    }
    {
        // Plaintext backend that speaks h2c: prior knowledge skips the Upgrade
        // round trip.
        sh::HttpProxyTarget backend;
        backend.host = "127.0.0.1";
        backend.port = kPlainPort;
        backend.rewrite_path = "/world";
        backend.h2c_prior_knowledge = true;
        plain.http_proxy("/rph2c", std::move(backend));
    }
    {
        // Go's Rewrite / ModifyResponse hooks.
        sh::HttpProxyTarget backend;
        backend.host = "127.0.0.1";
        backend.port = kPlainPort;
        backend.hooks.rewrite = [](const sh::Request &, sh::Request &out) { out.set_target("/world"); };
        backend.hooks.modify_response = [](int &status, sh::Headers &headers, const sh::Request &) {
            status = 203;
            headers.add("x-proxy-hook", "yes");
            return sh::error_code{};
        };
        plain.http_proxy("/rphook", std::move(backend));
    }
    {
        sh::HttpProxyTarget backend;
        backend.host = "127.0.0.1";
        backend.port = kTlsPort;
        backend.rewrite_path = "/$1";
        backend.tls = true;
        plain.http_proxy_regex("/rptls/(.*)", std::move(backend), std::move(proxy_cfg));
    }

    if (!plain.start() || !secure.start() || !compressed.start()) {
        std::printf("only http://SimpleHttpServer:7788\n");
        std::printf("FAIL  servers did not start (run from the repository root)\n");
        return 1;
    }
    const auto plain_port = plain.port();
    const auto tls_port = secure.port();
    const auto comp_port = compressed.port();
    std::printf("server up: http on :%u, https on :%u, compressed http on :%u\n", plain_port, tls_port, comp_port);

    asio::io_context ctx;
    bool finished = false;
    asio::co_spawn(ctx, run_all_suites(ctx, plain_port, tls_port, comp_port),
                   [&finished](const std::exception_ptr &ep) {
                       try {
                           if (ep)
                               std::rethrow_exception(ep);
                       } catch (const std::exception &e) {
                           std::printf("FAIL  suite threw: %s\n", e.what());
                           ++g_failed;
                       }
                       finished = true;
                   });

    // Poll rather than run(): the kept connections' idle timers and the raw
    // responders' pending accepts keep the context busy, so run() would never
    // return.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (!finished && std::chrono::steady_clock::now() < deadline) {
        ctx.run_for(std::chrono::milliseconds(50));
    }
    if (!finished) {
        std::printf("FAIL  the suite did not finish within 120s\n");
        ++g_failed;
    }
    g_fakes.clear(); // drop the pending accepts before stopping
    g_flaky.clear();
    ctx.stop();

    plain.stop();
    secure.stop();

    // The in-process fake servers and proxies are process-wide so their accept
    // loops outlive an individual suite, but their acceptors are bound to
    // `ctx`. Destroy them while the io_context is still alive: `ctx` is a local
    // of main, so static destructors at exit would otherwise touch a destroyed
    // execution_context (ASan: heap-use-after-free in
    // reactive_socket_service_base::destroy).
    g_proxies.clear();
    g_fakes.clear();
    g_flaky.clear();

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
