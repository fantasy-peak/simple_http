// C++ client probe for the differential suite. It exercises the new streaming
// upload, 100-continue, request-trailer, 429 and JWKS paths; test/differential/go/
// client_diff_probe.go mirrors the same checks with Go's net/http, and
// test/differential/run.py client diffs the two outputs (and runs the Go probe against the
// C++ server as the independent-server leg).
//
//   xmake build probe_client_diff
//   ./build/linux/x86_64/release/client_diff_probe http://127.0.0.1:7802

#include "simple_http.h"

#include <boost/asio.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace asio = boost::asio;
using namespace simple_http;
using namespace std::chrono_literals;

namespace {

int g_failed = 0;

void check(bool ok, std::string_view name, std::string detail = {}) {
    std::cout << (ok ? "PASS " : "FAIL ") << name;
    if (!ok) {
        ++g_failed;
        if (!detail.empty()) {
            std::cout << " [" << detail << "]";
        }
    }
    std::cout << '\n';
}

asio::awaitable<std::string> read_all(Stream &stream) {
    auto body = co_await stream.read_all();
    co_return body ? std::move(*body) : std::string{"<error: " + body.error().message() + ">"};
}

asio::awaitable<void> run_checks(std::string base) {
    ClientConfig cfg;
    cfg.default_version = HttpVersionPolicy::Http11;
    cfg.default_h2c = H2cMode::Off;
    Client client{co_await asio::this_coro::executor, cfg};

    const auto path = std::filesystem::temp_directory_path() / "simple_http_client_diff.bin";
    std::string payload(200000, 'a');
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>('a' + (i % 26));
    }
    {
        std::ofstream out{path, std::ios::binary};
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }

    // Streaming multipart: field + in-memory file.
    {
        auto up = co_await MultipartStreamWriter::open(client, base + "/mp", {.boundary = "DIFFB"});
        std::string body;
        if (!up) {
            body = "<open error: " + up.error().message() + ">";
        } else {
            (void)co_await up->field("title", "hello");
            (void)co_await up->file_data("upload", "notes.txt", "text/plain", "file data");
            (void)co_await up->finish();
            body = co_await read_all(up->stream());
        }
        check(body == "field:title=hello|file:upload:notes.txt:text/plain:9", "streaming multipart field+file", body);
    }

    // Streaming multipart: disk file.
    {
        auto up = co_await MultipartStreamWriter::open(client, base + "/mp", {.boundary = "DIFFB2"});
        std::string body;
        if (!up) {
            body = "<open error: " + up.error().message() + ">";
        } else {
            (void)co_await up->file("up", "blob.bin", path.string());
            (void)co_await up->finish();
            body = co_await read_all(up->stream());
        }
        check(body == "file:up:blob.bin:application/octet-stream:200000", "streaming multipart disk file", body);
    }

    // Raw file body upload.
    {
        auto up = co_await client.open_stream(base + "/echo", {.method = Method::Put});
        std::string body;
        if (!up) {
            body = "<open error: " + up.error().message() + ">";
        } else {
            (void)co_await stream_file_body(*up, path.string());
            (void)co_await up->finish();
            body = co_await read_all(*up);
        }
        check(body == payload, "raw file body upload", body == payload ? "" : "size=" + std::to_string(body.size()));
    }

    // Automatic Expect: 100-continue.
    {
        auto up =
            co_await client.open_stream(base + "/expect-ok", {.method = Method::Post, .auto_expect_continue = true});
        std::string body;
        if (!up) {
            body = "<open error: " + up.error().message() + ">";
        } else {
            (void)co_await up->write("hello!");
            (void)co_await up->finish();
            body = co_await read_all(*up);
        }
        check(body == "got 6", "auto 100-continue", body);
    }

    // A final response instead of a 100.
    {
        auto up = co_await client.open_stream(base + "/expect-reject",
                                              {.method = Method::Post, .auto_expect_continue = true});
        std::string detail;
        bool ok = false;
        if (!up) {
            detail = "<open error: " + up.error().message() + ">";
        } else {
            const auto ec = co_await up->write("body");
            auto head = co_await up->read_head();
            const std::string body = co_await read_all(*up);
            ok = ec == client_errc::expectation_failed && head && head->status == 417 && body == "no";
            detail = ec.message() + " status=" + (head ? std::to_string(head->status) : "?") + " body=" + body;
        }
        check(ok, "expect rejected by final response", detail);
    }

    // HTTP/1.1 request trailers.
    {
        StreamSpec spec;
        spec.method = Method::Post;
        spec.headers.add("trailer", "x-check");
        auto up = co_await client.open_stream(base + "/trailer-echo", std::move(spec));
        std::string body;
        if (!up) {
            body = "<open error: " + up.error().message() + ">";
        } else {
            (void)co_await up->write("body");
            Headers trailers;
            trailers.add("x-check", "yes");
            (void)co_await up->finish_trailers(std::move(trailers));
            body = co_await read_all(*up);
        }
        check(body == "yes", "request trailers", body);
    }

    // 429 with Retry-After: two rejects, then 200. The client configuration is
    // opt-in, matching Go/reqwest.
    {
        ClientConfig retry_cfg = cfg;
        retry_cfg.retry.max_retries = 3;
        retry_cfg.retry.retry_statuses = {429};
        retry_cfg.retry.initial_backoff = 1ms;
        Client retry_client{co_await asio::this_coro::executor, retry_cfg};
        auto response = co_await retry_client.get(base + "/retry-status").send();
        std::string detail;
        bool ok = false;
        if (!response) {
            detail = response.error().message();
        } else {
            auto body = co_await response->read_all();
            ok = response->status() == 200 && body && *body == "ok";
            detail = std::to_string(response->status()) + " " + (body ? *body : "<no body>");
        }
        check(ok, "429 Retry-After retry", detail);
    }

    // JWKS fetch.
    {
        auto body = co_await fetch_jwks_json(client, base + "/jwks");
        check(body && body->find("\"keys\"") != std::string::npos, "JWKS fetch", body ? *body : body.error().message());
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
    co_return;
}

} // namespace

int main(int argc, char **argv) {
    set_log_sink(make_stderr_sink(LogLevel::Error));
    const std::string base = argc > 1 ? argv[1] : "http://127.0.0.1:7802";
    asio::io_context io;
    asio::co_spawn(io, run_checks(base), asio::detached);
    io.run();
    return g_failed == 0 ? 0 : 1;
}
