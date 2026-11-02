// C++ client probe for the compression differential.
//
// Mirrors test/differential/go/compression_probe.go line for line; both run
// against the Go reference server and their PASS/FAIL outputs are diffed. This
// is what pins the client's Go-compatible rules: default gzip negotiation,
// "only decode what we advertised", HEAD/Range/explicit-header opt-out, and the
// Uncompressed flag/header removal.

#include "simple_http.h"

#include <boost/asio.hpp>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>

namespace sh = simple_http;
namespace asio = boost::asio;

namespace {

int g_failed = 0;

void check(std::string_view name, bool ok) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", std::string{name}.c_str());
    if (!ok) {
        ++g_failed;
    }
}

struct Result {
    int status{0};
    bool uncompressed{false};
    bool bodyless{false};
    bool ok{false};
    std::string ce;
    std::string cl;
    std::string saw;
    std::string body;
};

std::string header_of(const sh::Response &response, std::string_view name) {
    auto value = response.header(name);
    return value ? std::string{*value} : std::string{};
}

asio::awaitable<Result> fetch(sh::Client &client, const std::string &url, bool head = false,
                              std::string header_name = {}, std::string header_value = {}) {
    Result out;
    auto builder = head ? client.head(url) : client.get(url);
    if (!header_name.empty()) {
        builder.header(std::move(header_name), std::move(header_value));
    }
    auto response = co_await builder.send();
    if (!response) {
        co_return out;
    }
    out.status = response->status();
    out.uncompressed = response->uncompressed();
    out.bodyless = response->bodyless();
    out.ce = header_of(*response, "content-encoding");
    out.cl = header_of(*response, "content-length");
    out.saw = header_of(*response, "x-saw-accept-encoding");
    auto body = co_await response->read_all();
    if (!body) {
        co_return out;
    }
    out.body = std::move(*body);
    out.ok = true;
    co_return out;
}

asio::awaitable<void> run(std::string base) {
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    sh::ClientConfig cfg;
    cfg.default_version = sh::HttpVersionPolicy::Http11;
    sh::Client client{co_await asio::this_coro::executor, cfg};

    auto plain = co_await fetch(client, base + "/plain");
    check("plain: identity passes through",
          plain.ok && plain.status == 200 && plain.body == "plain" && !plain.uncompressed && plain.ce.empty());

    auto gzip = co_await fetch(client, base + "/gzip");
    check("gzip: default client decodes and marks Uncompressed",
          gzip.ok && gzip.status == 200 && gzip.body == "gzip-body" && gzip.uncompressed);
    check("gzip: decoded response drops Content-Encoding and Content-Length",
          gzip.ok && gzip.ce.empty() && gzip.cl.empty());

    auto explicit_gzip = co_await fetch(client, base + "/gzip", false, "accept-encoding", "gzip");
    check("gzip: explicit Accept-Encoding keeps the body encoded", explicit_gzip.ok && !explicit_gzip.uncompressed &&
                                                                       explicit_gzip.ce == "gzip" &&
                                                                       explicit_gzip.body != "gzip-body");

    auto deflate = co_await fetch(client, base + "/deflate");
    check("deflate: not auto-decoded",
          deflate.ok && !deflate.uncompressed && deflate.ce == "deflate" && deflate.body != "deflate-body");

    auto multi = co_await fetch(client, base + "/multi");
    check("multi-coding: not auto-decoded",
          multi.ok && !multi.uncompressed && multi.ce == "gzip, br" && multi.body != "multi-body");

    auto head = co_await fetch(client, base + "/gzip-head", true);
    check("HEAD: no automatic Accept-Encoding", head.ok && !head.uncompressed && head.saw == "no");

    auto range = co_await fetch(client, base + "/range", false, "range", "bytes=0-3");
    check("Range: no automatic Accept-Encoding", range.ok && range.saw == "no");

    sh::ClientConfig disabled_cfg;
    disabled_cfg.default_version = sh::HttpVersionPolicy::Http11;
    disabled_cfg.auto_decompress = false;
    sh::Client disabled{co_await asio::this_coro::executor, disabled_cfg};
    auto disabled_result = co_await fetch(disabled, base + "/gzip");
    check("disabled compression: no automatic Accept-Encoding",
          disabled_result.ok && !disabled_result.uncompressed && disabled_result.saw == "no" &&
              disabled_result.ce == "gzip" && disabled_result.body != "gzip-body");

    auto headers = co_await fetch(client, base + "/headers");
    check("default client advertises gzip",
          headers.ok && headers.body.find("accept-encoding: gzip") != std::string::npos);

    co_return;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: compression_diff_probe <base-url>\n");
        return 2;
    }
    const std::string base = argv[1];
    asio::io_context io;
    asio::co_spawn(io, run(base), [](std::exception_ptr ep) {
        if (ep) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception &e) {
                std::fprintf(stderr, "compression_diff_probe: %s\n", e.what());
            }
        }
        std::exit(g_failed == 0 ? 0 : 1);
    });
    io.run();
    return 0;
}
