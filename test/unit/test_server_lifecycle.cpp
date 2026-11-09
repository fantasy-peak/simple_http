// Server lifecycle regressions: shutdown must cancel idle keep-alive
// connections (not sit out the generic io-context drain grace), and
// max_connections must stop accepting instead of serving the extra sockets.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

#include "simple_http.h"

using namespace simple_http;

namespace {

ServerConfig lifecycle_config(std::size_t max_connections) {
    ServerConfig cfg;
    cfg.listen = InetAddress{"127.0.0.1", 0, false};
    cfg.worker_threads = 1;
    cfg.plaintext_protocols = PlaintextProtocols::Http1;
    cfg.max_connections = max_connections;
    cfg.shutdown_grace = std::chrono::milliseconds{200};
    return cfg;
}

void wait_for_connections(const Server &server, std::size_t expected) {
    for (int i = 0; i < 200 && server.active_connections() != expected; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
}

} // namespace

TEST_CASE("server: stop closes idle keep-alive connections promptly", "[server][review]") {
    Server server{lifecycle_config(0)};
    auto started = server.start();
    REQUIRE(started.has_value());
    const auto port = server.port();

    asio::io_context io;
    asio::ip::tcp::socket idle{io};
    idle.connect({asio::ip::make_address("127.0.0.1"), port});
    wait_for_connections(server, 1);
    REQUIRE(server.active_connections() == 1);

    const auto begin = std::chrono::steady_clock::now();
    server.stop();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin);

    // An idle connection has no request in flight; shutdown_grace is 200 ms.
    // The generic io-context drain grace is 2 s and must not be paid here.
    CHECK(elapsed < std::chrono::milliseconds{1000});
}

TEST_CASE("client: per-request version overrides the default", "[client][server][review]") {
    ServerConfig cfg;
    cfg.listen = InetAddress{"127.0.0.1", 0, false};
    cfg.worker_threads = 1;
    cfg.plaintext_protocols = PlaintextProtocols::Both;
    Server server{cfg};
    server.route({Method::Get}, "/", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(status::ok).async_send("ok", asio::use_awaitable);
    });
    REQUIRE(server.start().has_value());

    asio::io_context io;
    Client client{io.get_executor(), ClientConfig{}};
    const std::string url = "http://127.0.0.1:" + std::to_string(server.port()) + "/";

    std::optional<Version> v11;
    std::optional<Version> v2;
    std::optional<Version> v11_again;
    std::optional<error_code> e11;
    std::optional<error_code> e2;
    std::optional<error_code> e11_again;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            {
                auto [ec, r] = co_await client.get(url).version(HttpVersionPolicy::Http11).send(
                    asio::as_tuple(asio::use_awaitable));
                e11 = ec;
                if (!ec)
                    v11 = r.version();
            }
            {
                auto [ec, r] = co_await client.get(url).version(HttpVersionPolicy::Http2).send(
                    asio::as_tuple(asio::use_awaitable));
                e2 = ec;
                if (!ec)
                    v2 = r.version();
            }
            {
                auto [ec, r] = co_await client.get(url).version(HttpVersionPolicy::Http11).send(
                    asio::as_tuple(asio::use_awaitable));
                e11_again = ec;
                if (!ec)
                    v11_again = r.version();
            }
        },
        asio::detached);
    io.run_for(std::chrono::seconds(5));

    REQUIRE(e11.has_value());
    CHECK_FALSE(*e11);
    CHECK(v11 == Version::Http11);

    REQUIRE(e2.has_value());
    CHECK_FALSE(*e2);
    CHECK(v2 == Version::Http2);

    REQUIRE(e11_again.has_value());
    CHECK_FALSE(*e11_again);
    CHECK(v11_again == Version::Http11);

    client.close();
    server.stop();
}

TEST_CASE("server: max_connections stops accepting at the limit", "[server][review]") {
    Server server{lifecycle_config(1)};
    auto started = server.start();
    REQUIRE(started.has_value());
    const auto port = server.port();

    asio::io_context io;
    asio::ip::tcp::socket first{io};
    first.connect({asio::ip::make_address("127.0.0.1"), port});
    wait_for_connections(server, 1);
    REQUIRE(server.active_connections() == 1);

    asio::ip::tcp::socket second{io};
    second.connect({asio::ip::make_address("127.0.0.1"), port});
    std::this_thread::sleep_for(std::chrono::milliseconds{200});

    // The second socket must stay in the kernel backlog, not be accepted and
    // served.
    CHECK(server.active_connections() == 1);
    server.stop();
}
