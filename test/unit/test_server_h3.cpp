// HTTP/3 discovery regressions: the server must advertise the QUIC listener
// it actually bound on every h1/h2 response, not a snapshot of the limits
// taken before that port existed.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <optional>
#include <string>

#include "simple_http.h"
#include <simple_http/core/asio_compat.h>

using namespace simple_http;

#ifdef SIMPLE_HTTP_ENABLE_HTTP3

TEST_CASE("server: auto Alt-Svc advertises the bound QUIC port on h1/h2", "[server][h3]") {
    ServerConfig cfg;
    cfg.listen = InetAddress{"127.0.0.1", 0, false};
    cfg.quic = InetAddress{"127.0.0.1", 0, false};
    cfg.worker_threads = 1;
    cfg.auto_alt_svc = true;
    cfg.shutdown_grace = std::chrono::milliseconds{200};
    cfg.tls = TlsConfig{};
    cfg.tls->cert_chain_file = "./test/tls_certificates/server_cert.pem";
    cfg.tls->private_key_file = "./test/tls_certificates/server_key.pem";

    Server server{cfg};
    server.route({Method::Get}, "/", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(status::ok).async_send("ok", asio::use_awaitable);
    });
    REQUIRE(server.start().has_value());
    REQUIRE(server.quic_port() != 0);

    // The advertised value must name the port the QUIC socket actually got,
    // not the (zero) configured port.
    const std::string expected = cfg.limits.render_alt_svc(server.quic_port());
    REQUIRE_FALSE(expected.empty());

    asio::io_context io;
    ClientConfig ccfg;
    ccfg.proxy_from_env = false;
    ccfg.tls.verify_peer = false;
    ccfg.tls.verify_host = false;
    Client client{io.get_executor(), ccfg};
    const std::string url = "https://127.0.0.1:" + std::to_string(server.port()) + "/";

    std::optional<error_code> h2_ec;
    std::optional<error_code> h11_ec;
    std::optional<std::string> h2_alt;
    std::optional<std::string> h11_alt;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            {
                auto [ec, r] = co_await client.get(url)
                                   .version(HttpVersionPolicy::Http2)
                                   .send(asio::as_tuple(asio::use_awaitable));
                h2_ec = ec;
                if (!ec) {
                    if (auto value = r.header("alt-svc")) {
                        h2_alt = std::string(*value);
                    }
                }
            }
            {
                auto [ec, r] = co_await client.get(url)
                                   .version(HttpVersionPolicy::Http11)
                                   .send(asio::as_tuple(asio::use_awaitable));
                h11_ec = ec;
                if (!ec) {
                    if (auto value = r.header("alt-svc")) {
                        h11_alt = std::string(*value);
                    }
                }
            }
        },
        asio::detached);
    io.run_for(std::chrono::seconds(5));

    REQUIRE(h2_ec.has_value());
    CHECK_FALSE(*h2_ec);
    REQUIRE(h11_ec.has_value());
    CHECK_FALSE(*h11_ec);

    CHECK(h2_alt == expected);
    CHECK(h11_alt == expected);

    client.close();
    server.stop();
}

#endif
