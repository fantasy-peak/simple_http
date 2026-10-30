// A manual smoke test for the client against a real, public origin.
//
//   xmake build h3_public_check
//   xmake run h3_public_check -- https://cloudflare-quic.com/
//   xmake run h3_public_check -- https://www.cloudflare.com/ 104.16.124.96
//   xmake run h3_public_check -- https://www.cloudflare.com/ "" h2
//   xmake run h3_public_check -- https://www.google.com/ 127.0.0.1 h1
//
// Arguments: URL, optional pinned address ("" to resolve normally), optional
// "h1" | "h2" | "h3" (default h3). Pinning keeps the URL's SNI / :authority /
// certificate checks while dialing one address — useful for a broken IPv6 route,
// or to point the client at a local proxy relay.
//
// It uses the system CA bundle and normal host-name verification, exactly like a
// deployed client, and prints what came back. Public network reachability is a
// property of the machine, not of the library, so this is a manual check rather
// than part of the automatic suites.
//
// Exit status: 0 when a response was received (any status), 1 on error.

#include <simple_http.h>

#include <boost/asio.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

namespace asio = boost::asio;
using namespace simple_http;

int main(int argc, char **argv) {
    set_log_sink(make_stderr_sink(LogLevel::Warn));
    const std::string url = argc > 1 ? argv[1] : "https://cloudflare-quic.com/";
    // Optional: pin resolution to one address (SNI/authority stay the URL's).
    const std::string pinned_ip = argc > 2 ? argv[2] : "";
    const std::string policy_arg = argc > 3 ? argv[3] : "h3";
    HttpVersionPolicy policy = HttpVersionPolicy::Http3;
    if (policy_arg == "h1" || policy_arg == "http/1.1")
        policy = HttpVersionPolicy::Http11;
    else if (policy_arg == "h2" || policy_arg == "http/2")
        policy = HttpVersionPolicy::Http2;
    else if (policy_arg != "h3" && policy_arg != "http/3") {
        std::fprintf(stderr, "unknown version '%s' (expected h1|h2|h3)\n", policy_arg.c_str());
        return 2;
    }
    asio::io_context ctx;

    int exit_code = 1;
    asio::co_spawn(
        ctx,
        [&]() -> asio::awaitable<void> {
            ClientConfig cfg;
            cfg.default_version = policy;
            cfg.connect_timeout = std::chrono::seconds(10);
            cfg.request_timeout = std::chrono::seconds(60);
            cfg.response_head_timeout = std::chrono::seconds(10);
            // The OpenSSL xmake builds are configured with OPENSSLDIR pointing
            // at their own (empty) ssl/certs, so SSL_CTX_set_default_verify_paths
            // has no trust anchors here. Use the system bundle when it exists;
            // a deployed client would normally do the same explicitly.
            const std::filesystem::path system_ca = "/etc/ssl/certs/ca-certificates.crt";
            if (cfg.tls.ca_file.empty() && std::filesystem::exists(system_ca))
                cfg.tls.ca_file = system_ca.string();
            if (!pinned_ip.empty()) {
                cfg.resolve = [ip = pinned_ip](std::string, std::string port)
                    -> asio::awaitable<std::pair<error_code, std::vector<asio::ip::tcp::endpoint>>> {
                    error_code ec;
                    const auto address = asio::ip::make_address(ip, ec);
                    std::vector<asio::ip::tcp::endpoint> out;
                    if (!ec)
                        out.emplace_back(address, static_cast<std::uint16_t>(std::stoi(port)));
                    co_return std::pair<error_code, std::vector<asio::ip::tcp::endpoint>>{ec, std::move(out)};
                };
            }
            // System CA bundle and host-name verification (the defaults).
            Client client{co_await asio::this_coro::executor, cfg};

            const auto started = std::chrono::steady_clock::now();
            auto response = co_await client.get(url).send();
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);

            if (!response) {
                std::printf("FAIL  %s: %s\n", url.c_str(), response.error().message().c_str());
                co_return;
            }

            auto body = co_await response->text();
            // header() views are not NUL-terminated; copy before printing.
            const auto header = [&](std::string_view name) {
                const auto value = response->header(name);
                return value ? std::string{*value} : std::string{"(none)"};
            };
            std::printf("GET %s\n", url.c_str());
            std::printf("  status  : %d\n", response->status());
            std::printf("  version : %.*s\n", static_cast<int>(to_string(response->version()).size()),
                        to_string(response->version()).data());
            std::printf("  elapsed : %lld ms\n", static_cast<long long>(elapsed.count()));
            std::printf("  server  : %s\n", header("server").c_str());
            std::printf("  type    : %s\n", header("content-type").c_str());
            if (response->header("location"))
                std::printf("  location: %s\n", header("location").c_str());
            if (body)
                std::printf("  body    : %zu bytes\n", body->size());
            else
                std::printf("  body    : error: %s\n", body.error().message().c_str());
            std::printf("  dials   : %zu\n", client.stats().connections_opened);

            exit_code = body ? 0 : 1;
            // The kept QUIC session (and its timers) would otherwise keep the
            // context busy; this is a one-shot tool.
            ctx.stop();
        },
        asio::detached);

    ctx.run();
    return exit_code;
}
