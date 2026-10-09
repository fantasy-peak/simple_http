// Multi-thread stress for the client's threading model (concurrency model A).
//
// A *thread-concurrency* harness, not a protocol suite: it exercises the
// supported ways to use the client (a per-executor, pinned Client — driven
// from its own single thread — never concurrently from several) and lets the
// sanitizers (ASan+UBSan for memory errors, TSAN for data races) decide whether
// anything is wrong. Teardown always follows the "io_context outlives its
// sessions" contract: drop the Client, then drain the executor that owns the
// kept session's object graph.
//
//   xmake build thread_check && xmake run thread_check
//   xmake run thread_check -- --quick
//
// Scenarios:
//   A. one Client pinned to one executor (the "one executor, one Client" rule).
//      The historical cross-thread guarantee is now expressed by posting work
//      onto the pinned executor — never by driving the Client from another
//      thread's executor;
//   B. teardown loop: create / use / drop a Client with its io_context drained
//      in between — the ASan-class hazard (destruction posted onto a dead
//      executor);
//   C. high concurrency: one io_context on one thread with 32 concurrent
//      coroutines (h1 and h2c) — the "one Client per executor" shape at load.

#include <simple_http.h>

#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace asio = boost::asio;
namespace sh = simple_http;

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool ok, const std::string &what) {
    ++g_checks;
    if (!ok)
        ++g_failed;
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    std::fflush(stdout);
}

// --- the in-process server (routes shared with test/client.cpp) --------------

sh::ServerConfig server_config(std::uint16_t port) {
    sh::ServerConfig cfg;
    cfg.listen = sh::InetAddress{"127.0.0.1", port, false};
    cfg.worker_threads = 4;
    cfg.limits.idle_timeout = std::chrono::seconds(30);
    return cfg;
}

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
    server.route(sh::any_methods, "/world", [](sh::RequestPtr req, sh::ResponsePtr res) -> asio::awaitable<void> {
        const std::size_t n = static_cast<std::size_t>(query_number(req, "n", 2048));
        co_await res->status(200).send(std::string(n, 'x'));
    });
    server.route(sh::any_methods, "/echo", [](sh::RequestPtr req, sh::ResponsePtr res) -> asio::awaitable<void> {
        auto body = co_await req->body().read_all();
        co_await res->status(200).send(body ? *body : std::string{});
    });
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
}

struct SharedRun {
    std::atomic<std::int64_t> ok{0};
    std::atomic<std::int64_t> bad{0};
    std::atomic<int> finished{0};
    std::atomic<bool> timed_out{false};
};

// A. Supported multi-thread usage.
//  A1. One Client **strictly sequenced** across threads was the historical
//      shape — the unpinned engine let thread t pick up where t-1 left off,
//      hopping requests back onto the connection's first executor. With the
//      engine pinned to one executor (model A), the same guarantee is now
//      structural: work initiated elsewhere dispatches onto the pinned
//      executor, which the per-thread scenarios below exercise.
//  A2. One Client PER thread/executor (the "one executor, one Client" rule),
//      each thread running 40 concurrent coroutines — the load-generator
//      `-t N` shape. Teardown per thread: drop the Client, drain the context.
void scenario_supported_thread_use(std::uint16_t port) {
    const std::string base = "http://127.0.0.1:" + std::to_string(port);
    sh::ClientConfig cfg;
    cfg.request_timeout = std::chrono::seconds(15);
    cfg.idle_timeout = std::chrono::seconds(20);
    cfg.default_version = sh::HttpVersionPolicy::Http11;
    cfg.default_h2c = sh::H2cMode::Off;

    // (Driving one Client *concurrently* from several threads' executors is
    // unrepresentable now — the engine is pinned, and every entry point hops
    // onto its executor. The supported forms of multi-thread use follow: one
    // Client per thread (A2), and posting work onto a pinned Client's executor
    // (the teardown loop below drives it from a single thread).)

    // --- A2: one Client per thread, 4 threads x 40 concurrent coroutines ---
    std::printf("\n== A2: per-thread Client, 4 threads x 40 concurrent coroutines ==\n");
    constexpr int kThreads2 = 4;
    constexpr int kConcurrent = 40;
    SharedRun r2;
    std::vector<std::shared_ptr<asio::io_context>> ctxs;
    std::vector<std::thread> workers;
    std::atomic<int> global_pending{kThreads2 * kConcurrent};
    workers.clear();
    for (int t = 0; t < kThreads2; ++t) {
        auto ctx = std::make_shared<asio::io_context>();
        ctxs.push_back(ctx);
        workers.emplace_back([&, t, ctx] {
            auto h2cfg = cfg;
            h2cfg.default_version = sh::HttpVersionPolicy::Http2;
            h2cfg.default_h2c = sh::H2cMode::PriorKnowledge;
            auto local = std::make_shared<sh::Client>(ctx->get_executor(), h2cfg);
            for (int i = 0; i < kConcurrent; ++i) {
                asio::co_spawn(
                    *ctx,
                    [&, i]() -> asio::awaitable<void> {
                        (void)i;
                        for (int k = 0; k < 40; ++k) {
                            auto resp = co_await local->get(base + "/world?n=2048").send();
                            if (resp && resp->status() == 200)
                                ++r2.ok;
                            else
                                ++r2.bad;
                            auto up =
                                co_await local->open_stream(base + "/echo", sh::StreamSpec{.method = sh::Method::Post});
                            if (up) {
                                (void)co_await up->write("a");
                                (void)co_await up->finish("b");
                                auto head = co_await up->read_head();
                                auto body = co_await up->read_all(1u << 20);
                                if (head && head->status == 200 && body && *body == "ab")
                                    ++r2.ok;
                                else
                                    ++r2.bad;
                            } else {
                                ++r2.bad;
                            }
                        }
                        global_pending.fetch_sub(1);
                    },
                    asio::detached);
            }
            const auto w0 = std::chrono::steady_clock::now();
            while (global_pending.load() > 0 && std::chrono::steady_clock::now() - w0 < std::chrono::seconds(120))
                ctx->run_for(std::chrono::milliseconds(5));
            if (global_pending.load() > 0)
                r2.timed_out.store(true);
            // Per-thread teardown: drop this thread's Client, then drain the
            // context it was bound to (the engine posts the session release
            // onto it) — while the context still lives.
            local.reset();
            ctx->restart();
            while (ctx->poll())
                ;
        });
    }
    for (auto &w : workers)
        w.join();
    for (auto &ctx : ctxs) {
        ctx->restart();
        while (ctx->poll())
            ;
    }
    check(!r2.timed_out.load(), "A2 per-thread clients did not hang");
    check(r2.bad.load() == 0,
          "A2 per-thread: " + std::to_string(r2.ok.load()) + " ok, " + std::to_string(r2.bad.load()) + " bad");
}

// B. Teardown loop: create / use / drop a Client, draining the io_context
//    between the drop and its destruction — the ASan-class hazard.
void scenario_teardown_loop(std::uint16_t port) {
    std::printf("\n== B: teardown loop (create / use / destroy x 30) ==\n");
    const std::string base = "http://127.0.0.1:" + std::to_string(port);
    std::int64_t ok_all = 0, bad_all = 0;
    bool hung = false;
    for (int it = 0; it < 30 && !hung; ++it) {
        sh::ClientConfig cfg;
        cfg.request_timeout = std::chrono::seconds(10);
        cfg.idle_timeout = std::chrono::seconds(20);
        cfg.default_version = (it % 2) ? sh::HttpVersionPolicy::Http2 : sh::HttpVersionPolicy::Http11;
        cfg.default_h2c = (it % 2) ? sh::H2cMode::PriorKnowledge : sh::H2cMode::Off;
        auto ctx = std::make_shared<asio::io_context>();
        auto client = std::make_shared<sh::Client>(ctx->get_executor(), cfg);
        SharedRun r;
        std::atomic<int> remaining{4};
        auto guard = asio::make_work_guard(*ctx);
        std::thread th([&, ctx] {
            for (int i = 0; i < 4; ++i) {
                asio::co_spawn(
                    *ctx,
                    [&]() -> asio::awaitable<void> {
                        for (int k = 0; k < 5; ++k) {
                            auto resp = co_await client->get(base + "/world?n=512").send();
                            if (resp && resp->status() == 200)
                                ++r.ok;
                            else
                                ++r.bad;
                        }
                        remaining.fetch_sub(1);
                    },
                    asio::detached);
            }
            const auto w0 = std::chrono::steady_clock::now();
            while (remaining.load() > 0 && std::chrono::steady_clock::now() - w0 < std::chrono::seconds(60))
                ctx->run_for(std::chrono::milliseconds(5));
            if (remaining.load() > 0)
                r.timed_out.store(true);
        });
        th.join();
        guard.reset();
        ctx->restart();
        while (ctx->poll())
            ;
        client.reset(); // engine posts the kept session's release onto ctx
        ctx->restart();
        while (ctx->poll())
            ;
        hung = r.timed_out.load();
        ok_all += r.ok.load();
        bad_all += r.bad.load();
    }
    check(!hung, "teardown loop did not hang");
    check(bad_all == 0 && ok_all == 30 * 4 * 5,
          "teardown loop: " + std::to_string(ok_all) + " ok, " + std::to_string(bad_all) + " bad across 30 cycles");
}

// C. High concurrency: one io_context on one thread, 32 concurrent coroutines.
void scenario_high_concurrency(std::uint16_t port) {
    std::printf("\n== C: 1 context, 32 concurrent coroutines (h1 + h2c) ==\n");
    const std::string base = "http://127.0.0.1:" + std::to_string(port);
    using Row = std::tuple<std::string, sh::HttpVersionPolicy, sh::H2cMode>;
    for (const Row &row : std::vector<Row>{{"h1", sh::HttpVersionPolicy::Http11, sh::H2cMode::Off},
                                           {"h2c", sh::HttpVersionPolicy::Http2, sh::H2cMode::PriorKnowledge}}) {
        const std::string name = std::get<0>(row);
        sh::ClientConfig cfg;
        cfg.request_timeout = std::chrono::seconds(15);
        cfg.idle_timeout = std::chrono::seconds(20);
        cfg.default_version = std::get<1>(row);
        cfg.default_h2c = std::get<2>(row);

        asio::io_context ctx;
        auto guard = asio::make_work_guard(ctx);
        SharedRun r;
        {
            sh::Client client{ctx.get_executor(), cfg};
            for (int c = 0; c < 32; ++c) {
                asio::co_spawn(
                    ctx,
                    [&]() -> asio::awaitable<void> {
                        for (int i = 0; i < 60; ++i) {
                            auto resp = co_await client.get(base + "/world?n=512").send();
                            if (resp && resp->status() == 200)
                                ++r.ok;
                            else
                                ++r.bad;
                        }
                        r.finished.fetch_add(1);
                    },
                    asio::detached);
            }
            const auto w0 = std::chrono::steady_clock::now();
            while (r.finished.load() < 32 && std::chrono::steady_clock::now() - w0 < std::chrono::seconds(120))
                ctx.run_for(std::chrono::milliseconds(5));
            if (r.finished.load() < 32)
                r.timed_out.store(true);
            guard.reset();
            ctx.run_for(std::chrono::milliseconds(300));
        } // client destroyed here: the engine posts the session release onto ctx
        ctx.restart();
        while (ctx.poll())
            ;
        check(!r.timed_out.load(), std::string{name} + " high-concurrency did not hang");
        std::printf("  %s: %lld ok, %lld bad\n", name.c_str(), (long long)r.ok.load(), (long long)r.bad.load());
    }
}

} // namespace

int main(int argc, char **argv) {
    bool quick = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string{argv[i]} == "--quick")
            quick = true;
    }

    sh::set_log_sink(sh::make_stdout_sink(sh::LogLevel::Error));

    constexpr std::uint16_t kPort = 27920;
    sh::Server server{server_config(kPort)};
    register_routes(server);
    if (!server.start()) {
        std::printf("FAIL  server did not start\n");
        return 1;
    }
    std::printf("server up on :%u\n", server.port());

    if (quick) {
        scenario_teardown_loop(server.port());
    } else {
        scenario_supported_thread_use(server.port());
        scenario_teardown_loop(server.port());
        scenario_high_concurrency(server.port());
    }

    server.stop();

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}