// Client-behavior probe against an external (Go) server. Every scenario is
// mirrored by test client probes written with independent clients; the output
// format is deliberately identical so the two runs can be diffed line by line.
//
//   xmake build client_go_probe
//   ./build/linux/x86_64/release/client_go_probe http://127.0.0.1:7801

#include "simple_http.h"

#include <boost/asio.hpp>
#include <chrono>
#include <expected>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace asio = boost::asio;
using namespace simple_http;
using namespace std::chrono_literals;

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool ok, std::string_view name, std::string detail = {}) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
    }
    std::cout << (ok ? "PASS " : "FAIL ") << name;
    if (!detail.empty()) {
        std::cout << "  [" << detail << "]";
    }
    std::cout << '\n';
}

template <typename T> std::string err(const std::expected<T, error_code> &v) {
    return v ? std::string{} : v.error().message();
}

// co_spawn(use_future) requires the result type to be default-constructible,
// which Response/Stream are not. Bridge through a promise instead.
template <typename T> T run_awaitable(asio::io_context &io, asio::awaitable<T> aw) {
    struct State {
        std::promise<T> promise;
    };
    auto state = std::make_shared<State>();
    auto future = state->promise.get_future();
    asio::co_spawn(
        io,
        [state, aw = std::move(aw)]() mutable -> asio::awaitable<void> {
            try {
                state->promise.set_value(co_await std::move(aw));
            } catch (...) {
                state->promise.set_exception(std::current_exception());
            }
            co_return;
        },
        asio::detached);
    return future.get();
}

struct JsonEcho {
    std::string name;
    int age{};
};

} // namespace

int main(int argc, char **argv) {
    const std::string base = argc > 1 ? argv[1] : "http://127.0.0.1:7801";
    asio::io_context io;
    auto work = asio::make_work_guard(io);
    std::thread runner{[&] { io.run(); }};

    auto run = [&](auto fn) { return run_awaitable(io, fn()); };

    ClientConfig cfg;
    cfg.proxy_from_env = false;
    cfg.default_version = HttpVersionPolicy::Http11;
    cfg.default_h2c = H2cMode::Off;
    cfg.max_redirects = 5;
    cfg.cookie_jar = std::make_shared<CookieJar>();
    cfg.auto_decompress = true;
    Client client{io.get_executor(), cfg};

    {
        auto r = run([&]() -> asio::awaitable<std::expected<Response, error_code>> {
            co_return co_await client.get(base + "/world").send();
        });
        if (!r) {
            check(false, "GET /world", err(r));
        } else {
            auto body =
                run([&]() -> asio::awaitable<std::expected<std::string, error_code>> { co_return co_await r->text(); });
            check(r->status() == 200 && body && body->size() == 1024, "GET /world",
                  std::to_string(r->status()) + "/" + std::to_string(body ? body->size() : 0));
        }
    }

    {
        auto r = run([&]() -> asio::awaitable<std::expected<Response, error_code>> {
            co_return co_await client.post(base + "/echo").body("hello").send();
        });
        auto body =
            r ? run([&]() -> asio::awaitable<std::expected<std::string, error_code>> { co_return co_await r->text(); })
              : std::expected<std::string, error_code>{};
        check(r && r->status() == 200 && body && *body == "hello", "POST /echo",
              r ? (body ? *body : err(body)) : err(r));
    }

    {
        auto r = run([&]() -> asio::awaitable<std::expected<Response, error_code>> {
            co_return co_await client.get(base + "/redirect-chain").send();
        });
        auto body =
            r ? run([&]() -> asio::awaitable<std::expected<std::string, error_code>> { co_return co_await r->text(); })
              : std::expected<std::string, error_code>{};
        check(r && r->status() == 200 && body && *body == "target", "redirect chain",
              r ? (body ? *body : err(body)) : err(r));
    }

    {
        auto set = run([&]() -> asio::awaitable<std::expected<Response, error_code>> {
            co_return co_await client.get(base + "/set-cookie").send();
        });
        auto echo = run([&]() -> asio::awaitable<std::expected<Response, error_code>> {
            co_return co_await client.get(base + "/cookie-echo").send();
        });
        auto body = echo ? run([&]() -> asio::awaitable<std::expected<std::string, error_code>> {
            co_return co_await echo->text();
        })
                         : std::expected<std::string, error_code>{};
        check(set && echo && body && body->find("sid=abc123") != std::string::npos, "cookie store+replay",
              body ? *body : "no body");
    }

    {
        auto r = run([&]() -> asio::awaitable<std::expected<Response, error_code>> {
            co_return co_await client.get(base + "/plain").send();
        });
        auto body =
            r ? run([&]() -> asio::awaitable<std::expected<std::string, error_code>> { co_return co_await r->text(); })
              : std::expected<std::string, error_code>{};
        const bool decoded = r && r->headers().get("content-encoding").has_value() == false;
        check(r && r->status() == 200 && body && body->size() == 2048 && decoded, "gzip auto-decompress",
              body ? std::to_string(body->size()) : err(body));
    }

    {
        // A fresh Client: the kept connection belongs to the convenience layer
        // above, and the explicit stream path must also work on its own.
        Client fresh{io.get_executor(), cfg};
        auto s = run([&]() -> asio::awaitable<std::expected<Stream, error_code>> {
            co_return co_await fresh.open_stream(base + "/trailers", {});
        });
        if (!s) {
            check(false, "trailers", err(s));
        } else {
            // HTTP/1.x is half-duplex: the read end opens only after the write
            // end is closed (finish), even for a body-less GET.
            (void)run([&]() -> asio::awaitable<error_code> { co_return co_await s->finish(); });
            auto head = run([&]() -> asio::awaitable<std::expected<detail::ResponseHead, error_code>> {
                co_return co_await s->read_head();
            });
            std::string body;
            bool read_ok = true;
            std::string read_error;
            for (;;) {
                auto chunk = run(
                    [&]() -> asio::awaitable<std::expected<ReadResult, error_code>> { co_return co_await s->read(); });
                if (!chunk) {
                    read_ok = false;
                    read_error = chunk.error().message();
                    break;
                }
                if (chunk->eof) {
                    break;
                }
                body += chunk->data;
            }
            std::string trailer_dump;
            for (const auto &[name, value] : s->trailers()) {
                trailer_dump += name + "=" + value + ";";
            }
            const auto trailer = s->trailers().get("x-checksum");
            check(head && head->status == 200 && read_ok && body == "first second" && trailer && *trailer == "abc123",
                  "trailers",
                  (head ? "status=" + std::to_string(head->status) : "head_err=" + err(head)) + " body=" + body +
                      " read_err=" + read_error + " trailers=[" + trailer_dump + "]");
        }
    }

    {
        auto r = run([&]() -> asio::awaitable<std::expected<Response, error_code>> {
            co_return co_await client.get(base + "/status/404").send();
        });
        check(r && r->status() == 404, "status 404", r ? std::to_string(r->status()) : err(r));
    }

    {
        auto ok = run([&]() -> asio::awaitable<std::expected<Response, error_code>> {
            co_return co_await client.get(base + "/basic-auth").basic_auth("user", "pass").send();
        });
        auto bad = run([&]() -> asio::awaitable<std::expected<Response, error_code>> {
            co_return co_await client.get(base + "/basic-auth").basic_auth("user", "wrong").send();
        });
        auto body = ok ? run([&]() -> asio::awaitable<std::expected<std::string, error_code>> {
            co_return co_await ok->text();
        })
                       : std::expected<std::string, error_code>{};
        check(ok && ok->status() == 200 && body && *body == "secret" && bad && bad->status() == 401, "basic auth",
              (ok ? std::to_string(ok->status()) : err(ok)) + "/" + (bad ? std::to_string(bad->status()) : err(bad)));
    }

    {
        auto a = run([&]() -> asio::awaitable<std::expected<Response, error_code>> {
            co_return co_await client.get(base + "/conn-id").send();
        });
        auto b = run([&]() -> asio::awaitable<std::expected<Response, error_code>> {
            co_return co_await client.get(base + "/conn-id").send();
        });
        auto ida =
            a ? run([&]() -> asio::awaitable<std::expected<std::string, error_code>> { co_return co_await a->text(); })
              : std::expected<std::string, error_code>{};
        auto idb =
            b ? run([&]() -> asio::awaitable<std::expected<std::string, error_code>> { co_return co_await b->text(); })
              : std::expected<std::string, error_code>{};
        check(ida && idb && *ida == *idb && !ida->empty(), "connection reuse",
              (ida ? *ida : "?") + " vs " + (idb ? *idb : "?"));
    }

    {
        auto r = run([&]() -> asio::awaitable<std::expected<Response, error_code>> {
            co_return co_await client.get(base + "/headers").send();
        });
        auto body =
            r ? run([&]() -> asio::awaitable<std::expected<std::string, error_code>> { co_return co_await r->text(); })
              : std::expected<std::string, error_code>{};
        std::string lower = body ? *body : "";
        for (auto &c : lower) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        check(body && lower.find("user-agent") != std::string::npos &&
                  lower.find("accept-encoding") != std::string::npos,
              "request headers", body ? "ok" : err(body));
    }

    {
        // The layered head budget is on the explicit Stream (the convenience
        // send() keeps the whole-request timeout), so probe the stream path.
        const StreamSpec spec{.response_head_timeout = 100ms};
        auto s = run([&]() -> asio::awaitable<std::expected<Stream, error_code>> {
            co_return co_await client.open_stream(base + "/slow-head?ms=600", spec);
        });
        if (!s) {
            check(false, "response head timeout", err(s));
        } else {
            (void)run([&]() -> asio::awaitable<error_code> { co_return co_await s->finish(); });
            auto head = run([&]() -> asio::awaitable<std::expected<detail::ResponseHead, error_code>> {
                co_return co_await s->read_head();
            });
            const bool timed_out = !head && head.error() == make_error_code(client_errc::response_head_timeout);
            check(timed_out, "response head timeout", head ? "no error" : head.error().message());
        }
    }

    {
        MultipartForm form;
        form.field("title", "hello");
        form.file("upload", "a.txt", std::string{"file-body"});
        auto r = run([&]() -> asio::awaitable<std::expected<Response, error_code>> {
            co_return co_await client.post(base + "/form-multipart").multipart(std::move(form)).send();
        });
        auto body =
            r ? run([&]() -> asio::awaitable<std::expected<std::string, error_code>> { co_return co_await r->text(); })
              : std::expected<std::string, error_code>{};
        const bool ok = body && body->find("title||hello") != std::string::npos &&
                        body->find("upload|a.txt|file-body") != std::string::npos;
        check(ok, "multipart upload", body ? *body : err(body));
    }

    {
        auto r = run([&]() -> asio::awaitable<std::expected<Response, error_code>> {
            co_return co_await client.post(base + "/json").json(JsonEcho{"n", 3}).send();
        });
        auto body =
            r ? run([&]() -> asio::awaitable<std::expected<std::string, error_code>> { co_return co_await r->text(); })
              : std::expected<std::string, error_code>{};
        const bool ok =
            body && body->find("\"name\"") != std::string::npos && body->find("\"age\"") != std::string::npos;
        check(ok, "json body", body ? *body : err(body));
    }

    std::cout << "\n" << g_checks << " checks, " << g_failed << " failed\n";
    work.reset();
    io.stop();
    runner.join();
    return g_failed == 0 ? 0 : 1;
}
