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

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

#include <simple_http.h>

namespace asio = boost::asio;
namespace sh = simple_http;

namespace {

int g_failed = 0;
int g_checks = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok)
        ++g_failed;
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
}

std::string describe(const sh::error_code& ec) {
    return ec.message();
}

// --- a raw HTTP/1.x responder, for what the real server will not do ----------
//
// It answers every connection with the canned response it is given (optionally
// after reading one request head), and can be told to close right after, which
// is how the stale-pooled-connection and EOF-delimited cases are produced.
class FakeServer {
  public:
    FakeServer(asio::io_context& ctx, std::string response, bool close_after = true, bool read_request = true)
        : m_acceptor(ctx, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)),
          m_response(std::move(response)),
          m_close_after(close_after),
          m_read_request(read_request) {
        m_port = m_acceptor.local_endpoint().port();
        accept();
    }

    std::uint16_t port() const {
        return m_port;
    }

    std::atomic<int> connections{0};

  private:
    void accept() {
        // A fresh socket per connection: one shared socket would have the
        // coroutines of different connections read and write the same descriptor.
        auto socket = std::make_shared<asio::ip::tcp::socket>(m_acceptor.get_executor());
        m_acceptor.async_accept(*socket, [this, socket](const sh::error_code& ec) {
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
                seen.append(reinterpret_cast<const char*>(buf.data()), n);
                if (seen.find("\r\n\r\n") != std::string::npos)
                    break;
            }
        }
        co_await asio::async_write(*socket, asio::buffer(m_response), asio::as_tuple(asio::use_awaitable));
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
    std::uint16_t m_port{0};
};

// The raw responders must outlive the suite that uses them: an acceptor still
// holding a pending accept cannot be destroyed while the test's io_context runs.
std::vector<std::shared_ptr<FakeServer>> g_fakes;

FakeServer& make_fake(asio::io_context& ctx, std::string response, bool close_after = true) {
    g_fakes.push_back(std::make_shared<FakeServer>(ctx, std::move(response), close_after));
    return *g_fakes.back();
}

// --- the in-process server ---------------------------------------------------

sh::ServerConfig server_config(std::uint16_t port, std::optional<sh::TlsConfig> tls) {
    sh::ServerConfig cfg;
    cfg.listen = {sh::Listen{"127.0.0.1", port, false}};
    cfg.worker_threads = 2;
    cfg.tls = std::move(tls);
    cfg.limits.idle_timeout = std::chrono::seconds(30);
    return cfg;
}

// Reads `?key=value` out of a request target (the tests' routes take one
// numeric parameter each).
long long query_number(const sh::RequestPtr& req, std::string_view key, long long fallback) {
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

void register_routes(sh::Server& server) {
    server.route("/world", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).send(std::string{"hello from "} + std::string{sh::to_string(res->version())});
    });
    server.route("/echo", [](sh::RequestPtr req, sh::ResponsePtr res) -> asio::awaitable<void> {
        auto body = co_await req->body().read_all();
        co_await res->status(200).send(body ? *body : std::string{});
    });
    server.route("/drain", [](sh::RequestPtr req, sh::ResponsePtr res) -> asio::awaitable<void> {
        auto body = co_await req->body().read_all();
        co_await res->status(200).send("received " + std::to_string(body ? body->size() : 0) + " bytes");
    });
    server.route("/big", [](sh::RequestPtr req, sh::ResponsePtr res) -> asio::awaitable<void> {
        const std::size_t n = static_cast<std::size_t>(query_number(req, "n", 65536));
        std::string body(n, 'x');
        for (std::size_t i = 0; i < n; i += 4096)
            body[i] = 'a';  // a pattern to check, cheaply
        co_await res->status(200).send(std::move(body));
    });
    server.route("/empty", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(204).send_bodyless();
    });
    server.route("/delay", [](sh::RequestPtr req, sh::ResponsePtr res) -> asio::awaitable<void> {
        const auto ms = static_cast<int>(query_number(req, "ms", 200));
        asio::steady_timer timer{co_await asio::this_coro::executor};
        timer.expires_after(std::chrono::milliseconds(ms));
        co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        co_await res->status(200).send("delayed");
    });
    server.route("/stream", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        // A streamed response is chunked on HTTP/1.1, so this route exercises the
        // chunked decoder both directly and through the reverse proxy.
        (void)co_await res->status(200).content_type("text/plain").begin();
        (void)co_await res->write("alpha-");
        (void)co_await res->write("beta-");
        (void)co_await res->finish("gamma");
    });
    server.route("/headers", [](sh::RequestPtr req, sh::ResponsePtr res) -> asio::awaitable<void> {
        std::string out;
        for (const auto& [name, value] : req->headers()) {
            out.append(name).append(": ").append(value).append("\n");
        }
        co_await res->status(200).send(std::move(out));
    });
    server.route("/whoami", [](sh::RequestPtr, sh::ResponsePtr res, sh::SslHandle ssl) -> asio::awaitable<void> {
        std::string who = "no client certificate";
        if (ssl)
            if (X509* cert = SSL_get_peer_certificate(*ssl)) {
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

std::string url(std::uint16_t port, const char* path, bool tls = false) {
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

// The reverse proxy, exercised against this process's own listeners.
asio::awaitable<void> suite_reverse_proxy(std::uint16_t plain_port) {
    std::printf("\n== reverse proxy ==\n");
    sh::HttpClient http{base_config()};
    const std::string front = "http://127.0.0.1:" + std::to_string(plain_port);

    auto plain_backend = co_await http.get(front + "/rp/world");
    check(plain_backend && plain_backend->status == 200 && plain_backend->body == "hello from HTTP/1.1",
          "proxied request to a plaintext backend stays HTTP/1.1 -> " +
              (plain_backend ? plain_backend->body : describe(plain_backend.error())));

    auto echo = co_await http.post(front + "/rp/echo", "round tripped");
    check(echo && echo->body == "round tripped", "proxied POST keeps the body (chunked re-framing)");

    auto empty = co_await http.get(front + "/rp/empty");
    check(empty && empty->status == 204 && empty->bodyless, "proxied 204 is bodyless");

    auto big = co_await http.get(front + "/rp/big?n=200000");
    check(big && big->body.size() == 200000, "proxied 200000-byte body streamed intact");

    auto headers = co_await http.get(front + "/rp/headers");
    check(headers && headers->body.find("x-forwarded-for: 127.0.0.1") != std::string::npos &&
              headers->body.find("x-forwarded-proto: http") != std::string::npos &&
              headers->body.find("host: 127.0.0.1:") != std::string::npos,
          "proxied request carries X-Forwarded-* and the backend's Host -> " +
              (headers ? headers->body : describe(headers.error())));

    {
        // Pinned to HTTP/1.1 so the response really is chunked on the wire (over
        // HTTP/2 the same route is DATA frames, which would not exercise the
        // chunked decoder at all).
        auto cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http11;
        cfg.default_h2c = sh::H2cMode::Off;
        sh::HttpClient h1_http{cfg};
        auto streamed = co_await h1_http.get(front + "/stream");
        check(streamed && streamed->body == "alpha-beta-gamma",
              "a chunked response decodes directly -> " + (streamed ? streamed->body : describe(streamed.error())));
    }

    auto proxied_stream = co_await http.get(front + "/rp/stream");
    check(proxied_stream && proxied_stream->body == "alpha-beta-gamma",
          "a chunked backend passes through the proxy intact -> " +
              (proxied_stream ? proxied_stream->body : describe(proxied_stream.error())));

    auto tls_backend = co_await http.get(front + "/rptls/world");
    check(tls_backend && tls_backend->status == 200 && tls_backend->body == "hello from HTTP/2",
          "proxied request to a TLS backend negotiates HTTP/2 (ALPN) -> " +
              (tls_backend ? tls_backend->body : describe(tls_backend.error())));

    auto whoami = co_await http.get(front + "/rptls/whoami");
    check(whoami && whoami->body.find("SimpleHttpClient") != std::string::npos,
          "proxied request presents its client certificate to an mTLS backend");
    co_return;
}

// --- suites ------------------------------------------------------------------

asio::awaitable<void> suite_protocol_matrix(std::uint16_t plain, std::uint16_t tls_port) {
    std::printf("\n== protocol matrix ==\n");

    struct Case {
        const char* name;
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

    for (const auto& c : cases) {
        auto cfg = base_config();
        cfg.default_version = c.policy;
        cfg.default_h2c = c.h2c;
        sh::HttpClient http{cfg};
        const std::string target = url(c.use_tls ? tls_port : plain, "/world", c.use_tls);
        auto r = co_await http.get(target);
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
        sh::HttpClient http{cfg};
        auto plain_r = co_await http.get(url(plain, "/world"));
        check(plain_r && plain_r->version == sh::Version::Http2, "Auto over http negotiates h2c (upgrade)");
        auto tls_r = co_await http.get(url(tls_port, "/world", true));
        check(tls_r && tls_r->version == sh::Version::Http2, "Auto over https negotiates h2 via ALPN");
    }
    {
        // The same client, but told never to use plaintext h2: it must stay 1.1.
        auto cfg = base_config();
        cfg.default_h2c = sh::H2cMode::Off;
        sh::HttpClient http{cfg};
        auto r = co_await http.get(url(plain, "/world"));
        check(r && r->version == sh::Version::Http11, "Auto with H2cMode::Off stays HTTP/1.1");
    }
    co_return;
}

asio::awaitable<void> suite_framing(std::uint16_t plain) {
    std::printf("\n== framing ==\n");
    sh::HttpClient http{base_config()};

    auto echo = co_await http.post(url(plain, "/echo"), "round trip");
    check(echo && echo->status == 200 && echo->body == "round trip",
          "POST /echo round trip -> " + (echo ? echo->body : describe(echo.error())));

    auto empty = co_await http.get(url(plain, "/empty"));
    check(empty && empty->status == 204 && empty->body.empty() && empty->bodyless, "204 is bodyless");

    sh::RequestSpec head;
    head.method = sh::Method::Head;
    head.target = "/big?n=1000";
    auto head_res = co_await http.request(url(plain, "/big?n=1000"), head);
    check(head_res && head_res->status == 200 && head_res->body.empty() && head_res->bodyless,
          "HEAD returns the head and no body");
    {
        // Over HTTP/1.1 the server synthesizes the Content-Length a GET would
        // have produced (RFC 9110 §9.3.2); its HTTP/2 writer does not, so that
        // part is checked on the h1 path.
        auto cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http11;
        cfg.default_h2c = sh::H2cMode::Off;
        sh::HttpClient h1_http{cfg};
        auto r = co_await h1_http.request(url(plain, "/big?n=1000"), head);
        check(r && r->body.empty() && r->header("content-length").value_or("") == "1000",
              "HEAD over HTTP/1.1 keeps the Content-Length a GET would have produced");
    }

    auto big = co_await http.get(url(plain, "/big?n=300000"));
    check(big && big->body.size() == 300000 && big->body[0] == 'a', "300000-byte body streamed and intact");

    auto missing = co_await http.get(url(plain, "/nope"));
    check(missing && missing->status == 404, "404 is reported, not followed");

    auto capped = co_await http.get(url(plain, "/big?n=200000"), sh::RequestOptions{.max_body_bytes = 1000});
    check(!capped && capped.error() == sh::client_errc::body_too_large, "body cap stops an oversized response");

    // Keep-alive: a second request on the same client reuses the connection.
    auto before = http.stats().connections_opened;
    auto again = co_await http.get(url(plain, "/world"));
    check(again && http.stats().connections_opened == before, "keep-alive reuses the pooled connection");
    co_return;
}

asio::awaitable<void> suite_streaming(std::uint16_t plain) {
    std::printf("\n== streaming ==\n");
    for (const auto mode : {sh::H2cMode::PriorKnowledge, sh::H2cMode::Upgrade}) {
        auto cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http2;
        cfg.default_h2c = mode;
        sh::HttpClient http{cfg};

        // A streamed request body: chunked on h1, DATA frames on h2, with the
        // response read back on the same exchange.
        sh::RequestSpec spec;
        spec.method = sh::Method::Post;
        spec.target = "/drain";
        spec.stream_body = true;
        auto opened = co_await http.open_stream(url(plain, "/drain"), spec);
        if (!opened) {
            check(false, "streamed POST could not start: " + describe(opened.error()));
            continue;
        }
        auto stream = opened->stream;
        std::size_t sent = 0;
        for (int i = 0; i < 16; ++i) {
            std::string chunk(64 * 1024, 'y');
            sent += chunk.size();
            if (auto ec = co_await stream->write(std::move(chunk)); ec) {
                check(false, "streamed write failed: " + describe(ec));
                break;
            }
        }
        if (auto ec = co_await stream->finish("tail"); ec)
            check(false, "finish failed: " + describe(ec));
        auto body = co_await stream->read_all(1024);
        const std::string expected = "received " + std::to_string(sent + 4) + " bytes";
        check(body && *body == expected,
              std::string{"streamed upload (1 MiB) over "} +
                  (mode == sh::H2cMode::Upgrade ? "h2c upgrade" : "h2c prior knowledge") + " -> " +
                  (body ? *body : describe(body.error())));
    }
    co_return;
}

asio::awaitable<void> suite_h2_multiplex(std::uint16_t plain) {
    std::printf("\n== HTTP/2 multiplexing ==\n");
    auto cfg = base_config();
    cfg.default_version = sh::HttpVersionPolicy::Http2;
    cfg.default_h2c = sh::H2cMode::PriorKnowledge;
    sh::HttpClient http{cfg};

    auto session = co_await http.connect(url(plain, "/"));
    if (!session) {
        check(false, "connect failed: " + describe(session.error()));
        co_return;
    }

    constexpr int kStreams = 16;
    auto ex = co_await asio::this_coro::executor;
    asio::experimental::concurrent_channel<void(sh::error_code)> done{ex, kStreams};
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kStreams; ++i) {
        asio::co_spawn(
            ex,
            [&, session]() -> asio::awaitable<void> {
                sh::RequestSpec spec;
                spec.target = "/delay?ms=300";
                auto stream = co_await (*session)->open_stream(spec);
                if (!stream) {
                    (void)done.try_send(stream.error());
                    co_return;
                }
                auto head = co_await (*stream)->read_head();
                auto body = co_await (*stream)->read_all(1024);
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
    check(http.stats().connections_opened == 1, "all 16 streams shared one connection");

    // A stream that is abandoned mid-body must not take the connection down: on
    // HTTP/2 the session resets just that stream.
    sh::RequestSpec big;
    big.target = "/big?n=400000";
    auto st = co_await (*session)->open_stream(big);
    if (st) {
        auto head = co_await (*st)->read_head();
        (void)head;
        (*st)->cancel();
    }
    sh::RequestSpec again_spec;
    again_spec.target = "/world";
    auto after = co_await (*session)->open_stream(again_spec);
    bool usable = false;
    if (after) {
        auto head = co_await (*after)->read_head();
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
        sh::HttpClient http{cfg};
        auto r = co_await http.get(url(plain, "/delay?ms=3000"));
        check(!r && r.error() == sh::client_errc::request_timeout, "a slow response hits request_timeout");
    }
    {
        sh::HttpClient http{base_config()};
        auto r = co_await http.get("http://127.0.0.1:1/world");
        check(!r, "a closed port is an error, not a hang: " + describe(r.error()));
        auto bad = co_await http.get("ftp://127.0.0.1/world");
        check(!bad && bad.error() == sh::client_errc::unsupported_scheme, "a non-http scheme is rejected");
        auto malformed = co_await http.get("not a url");
        check(!malformed && malformed.error() == sh::client_errc::bad_url, "a malformed URL is rejected");
    }
    co_return;
}

asio::awaitable<void> suite_tls(std::uint16_t tls_port) {
    std::printf("\n== TLS ==\n");
    {
        auto cfg = base_config();  // CA + client cert + CN verification
        sh::HttpClient http{cfg};
        auto r = co_await http.get(url(tls_port, "/whoami", true));
        check(r && r->status == 200 && r->body.find("SimpleHttpClient") != std::string::npos,
              "mutual TLS presented our client certificate: " + (r ? r->body : describe(r.error())));
    }
    {
        auto cfg = base_config();
        cfg.tls.sni_override.clear();  // verify "127.0.0.1" against the certificate
        sh::HttpClient http{cfg};
        auto r = co_await http.get(url(tls_port, "/world", true));
        check(!r, "name verification fails for a certificate issued to another name");
    }
    {
        auto cfg = base_config();
        cfg.tls.verify_peer = false;
        sh::HttpClient http{cfg};
        auto r = co_await http.get(url(tls_port, "/world", true));
        check(r && r->status == 200, "verification can be turned off deliberately");
    }
    {
        auto cfg = base_config();
        cfg.tls.ca_file = "./test/tls_certificates/client_cert.pem";  // a CA that did not sign the server
        cfg.tls.verify_host = false;
        sh::HttpClient http{cfg};
        auto r = co_await http.get(url(tls_port, "/world", true));
        check(!r, "an unrelated CA fails verification");
    }
    {
        // ALPN pinned to http/1.1 must stay there.
        auto cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Http11;
        sh::HttpClient http{cfg};
        auto r = co_await http.get(url(tls_port, "/world", true));
        check(r && r->version == sh::Version::Http11, "ALPN pinned to http/1.1 stays on HTTP/1.1");
    }
    co_return;
}

// Everything a well-behaved server will not do, driven by the raw responder.
asio::awaitable<void> suite_raw_peer(asio::io_context& ctx) {
    std::printf("\n== raw peer edge cases ==\n");

    {  // an informational response before the real one
        FakeServer& fake = make_fake(ctx,
                                     "HTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\n"
                                     "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok",
                                     /*close_after=*/false);
        sh::HttpClient http{base_config()};
        auto r = co_await http.get(url(fake.port(), "/x"));
        check(r && r->status == 200 && r->body == "ok", "1xx is skipped, the final head is used");
    }
    {  // a body delimited by the close
        FakeServer& fake = make_fake(ctx, "HTTP/1.1 200 OK\r\n\r\nuntil eof");
        sh::HttpClient http{base_config()};
        auto r = co_await http.get(url(fake.port(), "/x"));
        check(r && r->body == "until eof", "an EOF-delimited body is read to the end");
    }
    {  // a truncated Content-Length body
        FakeServer& fake = make_fake(ctx, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort");
        sh::HttpClient http{base_config()};
        auto r = co_await http.get(url(fake.port(), "/x"));
        check(!r, "a truncated body is an error, not a short body: " + describe(r.error()));
    }
    {  // a malformed status line
        FakeServer& fake = make_fake(ctx, "NOT-HTTP 200 OK\r\n\r\n");
        sh::HttpClient http{base_config()};
        auto r = co_await http.get(url(fake.port(), "/x"));
        check(!r && r.error() == sh::client_errc::protocol_error, "a malformed status line is a protocol error");
    }
    {  // a chunked response, with an extension and trailers (what a dynamic
       // backend such as code-server sends): every byte must come back intact.
       // Regression: the size line used to be read through a view into the read
       // buffer that the consume step then shifted, so any chunked body failed.
        FakeServer& fake = make_fake(ctx,
                                     "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                                     "5;ext=1\r\nhello\r\n"
                                     "6\r\n world\r\n"
                                     "0\r\nX-Trailer: done\r\n\r\n",
                                     /*close_after=*/false);
        sh::HttpClient http{base_config()};
        auto r = co_await http.get(url(fake.port(), "/chunked"));
        check(r && r->status == 200 && r->body == "hello world",
              "a chunked response decodes (with an extension and trailers) -> " +
                  (r ? r->body : describe(r.error())));
    }
    {  // a chunked body cut short
        FakeServer& fake = make_fake(ctx, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel",
                                     /*close_after=*/true);
        sh::HttpClient http{base_config()};
        auto r = co_await http.get(url(fake.port(), "/chunked-truncated"));
        check(!r, "a truncated chunked body is an error: " + describe(r.error()));
    }
    {  // a malformed chunk-size line
        FakeServer& fake = make_fake(ctx, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nhello\r\n0\r\n\r\n",
                                     /*close_after=*/true);
        sh::HttpClient http{base_config()};
        auto r = co_await http.get(url(fake.port(), "/chunked-bad"));
        check(!r && r.error() == sh::client_errc::protocol_error, "a malformed chunk size is a protocol error");
    }
    {  // a stale pooled connection: the peer closes while the session sits idle
        FakeServer& fake = make_fake(ctx, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
        sh::HttpClient http{base_config()};
        auto first = co_await http.get(url(fake.port(), "/x"));
        check(first && first->body == "ok", "first request to the raw peer answered");
        // The peer closed the connection after answering; the next request may
        // pick it out of the pool, discover it is dead and retry on a fresh one.
        auto second = co_await http.get(url(fake.port(), "/x"));
        check(second && second->body == "ok", "a stale pooled connection is retried on a fresh one");
    }
    {  // a peer that ignores `Upgrade: h2c` answers with an ordinary response
        FakeServer& fake = make_fake(ctx, "HTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\nno h2c!", /*close_after=*/false);
        auto cfg = base_config();
        cfg.default_version = sh::HttpVersionPolicy::Auto;  // upgrade is attempted, and may be declined
        sh::HttpClient http{cfg};
        auto r = co_await http.get(url(fake.port(), "/x"));
        check(r && r->status == 200 && r->body == "no h2c!" && r->version == sh::Version::Http11,
              "a declined h2c upgrade falls back to the HTTP/1.1 response -> " +
                  (r ? std::to_string(r->status) + " " + std::string{sh::to_string(r->version)} + " '" + r->body + "'"
                     : describe(r.error())));

        auto cfg2 = base_config();
        cfg2.default_version = sh::HttpVersionPolicy::Http2;  // ... but a pinned HTTP/2 policy may not
        sh::HttpClient strict{cfg2};
        auto r2 = co_await strict.get(url(fake.port(), "/x"));
        check(!r2 && r2.error() == sh::client_errc::version_not_negotiated,
              "a pinned HTTP/2 policy fails when the upgrade is declined");
    }
    co_return;
}

// Every suite, in order. A named coroutine rather than a lambda: a lambda
// coroutine whose handle escapes (here, into co_spawn) trips a GCC
// coroutine-frame lifetime problem in this toolchain, and the suites would then
// run against a frame that has already been reused.
asio::awaitable<void> run_all_suites(asio::io_context& ctx, std::uint16_t plain, std::uint16_t tls_port) {
    co_await suite_protocol_matrix(plain, tls_port);
    co_await suite_framing(plain);
    co_await suite_streaming(plain);
    co_await suite_h2_multiplex(plain);
    co_await suite_errors(plain);
    co_await suite_tls(tls_port);
    co_await suite_reverse_proxy(plain);
    co_await suite_raw_peer(ctx);
    co_return;
}

}  // namespace

int main() {
    sh::set_log_level(sh::LogLevel::Error);
    sh::LOG_CB = [](sh::LogLevel, std::string_view file, int line, std::string msg) {
        std::printf("  [lib] %s:%d %s\n", std::string{file}.c_str(), line, msg.c_str());
    };

    // The plaintext and TLS listeners of the library's own server, in-process.
    // Fixed ports, because the reverse-proxy routes below must name the backend's
    // port and are registered before the servers start (the route table is not
    // mutated while workers serve).
    constexpr std::uint16_t kPlainPort = 27910;
    constexpr std::uint16_t kTlsPort = 27911;
    sh::ServerConfig plain_cfg = server_config(kPlainPort, std::nullopt);
    // The proxy's client role must trust the test CA (and present the client
    // certificate) to reach this process's own mTLS listener.
    plain_cfg.proxy_client.tls.ca_file = "./test/tls_certificates/ca_cert.pem";
    plain_cfg.proxy_client.tls.cert_chain_file = "./test/tls_certificates/client_cert.pem";
    plain_cfg.proxy_client.tls.private_key_file = "./test/tls_certificates/client_key.pem";
    plain_cfg.proxy_client.tls.verify_host = false;  // the test certificate has no SAN
    sh::Server plain{plain_cfg};
    register_routes(plain);
    sh::TlsConfig tls_cfg;
    tls_cfg.cert_chain_file = "./test/tls_certificates/server_cert.pem";
    tls_cfg.private_key_file = "./test/tls_certificates/server_key.pem";
    tls_cfg.mutual = true;  // mutual TLS: the client must present its certificate
    tls_cfg.ca_file = std::string{"./test/tls_certificates/ca_cert.pem"};
    sh::Server secure{server_config(kTlsPort, tls_cfg)};
    register_routes(secure);
    // Reverse-proxy routes: one to the plaintext listener (the historical
    // behaviour: a plaintext backend stays HTTP/1.1) and one to the TLS listener
    // (the backend leg is HTTPS, so ALPN decides — and offers h2).
    plain.http_proxy_regex("/rp/(.*)", "127.0.0.1", kPlainPort, "/$1");
    {
        sh::HttpProxyTarget backend;
        backend.host = "127.0.0.1";
        backend.port = kTlsPort;
        backend.rewrite_path = "/$1";
        backend.tls = true;
        plain.http_proxy_regex("/rptls/(.*)", std::move(backend));
    }

    if (!plain.start() || !secure.start()) {
        std::printf("only http://SimpleHttpServer:7788\n");
        std::printf("FAIL  servers did not start (run from the repository root)\n");
        return 1;
    }
    const auto plain_port = plain.port();
    const auto tls_port = secure.port();
    std::printf("server up: http on :%u, https on :%u\n", plain_port, tls_port);

    asio::io_context ctx;
    bool finished = false;
    asio::co_spawn(ctx, run_all_suites(ctx, plain_port, tls_port), [&finished](const std::exception_ptr& ep) {
        try {
            if (ep)
                std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            std::printf("FAIL  suite threw: %s\n", e.what());
            ++g_failed;
        }
        finished = true;
    });

    // Poll rather than run(): idle pooled connections and the raw responders'
    // pending accepts keep the context busy, so run() would never return.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (!finished && std::chrono::steady_clock::now() < deadline) {
        ctx.run_for(std::chrono::milliseconds(50));
    }
    if (!finished) {
        std::printf("FAIL  the suite did not finish within 120s\n");
        ++g_failed;
    }
    g_fakes.clear();  // drop the pending accepts before stopping
    ctx.stop();

    plain.stop();
    secure.stop();

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
