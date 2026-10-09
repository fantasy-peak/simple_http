// test/loadgen.cpp — a load generator built on simple_http's own client.
//
// It drives the library's client (not a third-party loader) against any
// endpoint, for HTTP/1.1, HTTP/2 and WebSocket, and prints an h2load-shaped
// summary so the numbers are directly comparable with k6 / h2load / wrk on the
// same server:
//
//   xmake build loadgen && xmake run loadgen http://127.0.0.1:7788/world -p h1 -n 300000 -c 100 -t 4
//   xmake run loadgen http://127.0.0.1:7790/world -p h2  -n 600000 -c 20 -m 40 -t 4
//   xmake run loadgen ws://127.0.0.1:7788/echo1k -p ws -n 200000 -c 64 -W 512
//
// Protocols:
//   h1   detail::ClientEngine sessions, -c connections kept alive per worker
//        (one exchange at a time on each; connect() returns a session the
//        worker owns for the whole run) — the wrk -c C model, with no
//        per-request convenience-layer work.
//   h2   detail::ClientEngine sessions, -c connections x -m streams per
//        connection — the h2load -c/-m multiplexing model. The session stays
//        continuously busy (M in-flight streams), so the direct path is safe;
//        the public Client facade would dial one connection per concurrent
//        first hop instead (Go-style, no idle wait-queue). With --upgrade the
//        first exchange on each connection performs the h2c handshake (the
//        plain open_stream path never switches protocol), and only then do the
//        -m streams fan out — until the peer answers 101 the session is an
//        HTTP/1.1 one-exchange-at-a-time object and must not be shared.
//   pub  the public Client facade on HTTP/1.1 — what callers get by default
//        (one kept connection per origin, so concurrency past the single
//        in-flight exchange queues on the gate instead of redialing).
//   ws   public Client::open_websocket, -c connections, each echo message
//        measured as one latency sample.
//
// The target endpoint is expected to be the stress shape of the example server:
// it drains the request body and answers with `resp_bytes` bytes — /world?n=.
// The ws mode needs an echo endpoint (/echo1k).

#include <simple_http.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace asio = boost::asio;
namespace sh = simple_http;

namespace {

struct Options {
    std::string url;
    std::string protocol{"h1"};
    std::size_t requests{300000};
    std::size_t concurrency{16};
    std::size_t streams{1};       // h2: streams per connection
    std::size_t body_bytes{1024}; // POST body (the h2load -d shape)
    std::size_t resp_bytes{2048}; // ?n= tier answered by /world
    double duration{0};           // seconds; 0 = fixed request count
    int threads{1};
    std::size_t ws_bytes{512};
    bool h2_upgrade{false}; // h2c via Upgrade instead of prior knowledge
};

[[noreturn]] void usage(const char *prog) {
    std::printf("usage: %s <url> [-p h1|pub|h2|ws] [-n requests] [-c concurrency] [-m streams]\n"
                "              [-B body_bytes] [-R resp_bytes] [-d seconds] [-t threads] [-W ws_bytes] [--upgrade]\n"
                "  h1  engine sessions, -c connections reused per worker (wrk -c C model)\n"
                "  pub public Client facade, -c connections (what callers get by default)\n"
                "  h2  engine sessions, -c connections x -m streams (h2load -c/-m model; h2c)\n"
                "  ws  public Client::open_websocket, -c connections, echo messages\n"
                "  -t threads: one io_context per thread (model A — each connection binds a\n"
                "              single-threaded context; N contexts drive N cores, like the\n"
                "              server's IoCtxPool). connections are split across threads.\n",
                prog);
    std::exit(2);
}

Options parse_args(int argc, char **argv) {
    if (argc < 2 || std::string_view{argv[1]}.starts_with("-"))
        usage(argv[0]);
    Options opt;
    opt.url = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string_view a{argv[i]};
        auto next = [&]() -> std::string_view {
            if (i + 1 >= argc)
                usage(argv[0]);
            return argv[++i];
        };
        if (a == "-p" || a == "--protocol")
            opt.protocol = std::string{next()};
        else if (a == "-n" || a == "--requests")
            opt.requests = std::stoull(std::string{next()});
        else if (a == "-c" || a == "--concurrency")
            opt.concurrency = std::stoull(std::string{next()});
        else if (a == "-m" || a == "--streams")
            opt.streams = std::stoull(std::string{next()});
        else if (a == "-B" || a == "--body-bytes")
            opt.body_bytes = std::stoull(std::string{next()});
        else if (a == "-R" || a == "--resp-bytes")
            opt.resp_bytes = std::stoull(std::string{next()});
        else if (a == "-d" || a == "--duration")
            opt.duration = std::stod(std::string{next()});
        else if (a == "-t" || a == "--threads")
            opt.threads = std::stoi(std::string{next()});
        else if (a == "-W" || a == "--ws-bytes")
            opt.ws_bytes = std::stoull(std::string{next()});
        else if (a == "--upgrade")
            opt.h2_upgrade = true;
        else
            usage(argv[0]);
    }
    if (opt.protocol != "h1" && opt.protocol != "pub" && opt.protocol != "h2" && opt.protocol != "ws")
        usage(argv[0]);
    if (opt.threads < 1)
        opt.threads = 1;
    if (opt.concurrency == 0)
        opt.concurrency = 1;
    if (opt.streams == 0)
        opt.streams = 1;
    return opt;
}

// Shared counters across the worker coroutines.
struct Counters {
    std::atomic<std::size_t> issued{0};    // tickets handed out
    std::atomic<std::size_t> completed{0}; // succeeded + failed + errored
    std::atomic<std::size_t> succeeded{0};
    std::atomic<std::size_t> failed{0};
    std::atomic<std::size_t> errored{0};
    std::atomic<std::size_t> timeout{0};
    std::atomic<bool> stopping{false};
    std::atomic<std::size_t> opened_connections{0};
    std::vector<double> lats_ms;
    mutable std::mutex lat_mu;
};

double ms_since(const std::chrono::steady_clock::time_point &t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// Appends ?n=<resp> to the target (keeps an existing query).
std::string n_url(const std::string &url, std::size_t resp) {
    std::string out = url;
    out += (out.find('?') == std::string::npos ? "?" : "&");
    out += "n=" + std::to_string(resp);
    return out;
}

// The origin-form path+query of an absolute URL ("http://h:p/world?n=2" -> "/world?n=2").
std::string path_query(const std::string &url) {
    const auto auth = url.find("://");
    const auto slash = url.find('/', auth == std::string::npos ? 0 : auth + 3);
    if (slash == std::string::npos)
        return "/";
    return url.substr(slash);
}

void record(Counters &ct, double ms) {
    std::lock_guard<std::mutex> lock(ct.lat_mu);
    ct.lats_ms.push_back(ms);
}

void count_status(Counters &ct, int status) {
    if (status >= 200 && status < 300)
        ++ct.succeeded;
    else
        ++ct.errored;
    ++ct.completed;
}

void classify(Counters &ct, const sh::error_code &ec) {
    if (ec == sh::make_error_code(sh::client_errc::request_timeout) ||
        ec == sh::make_error_code(sh::client_errc::response_head_timeout) ||
        ec == sh::make_error_code(sh::client_errc::body_idle_timeout) ||
        ec == sh::make_error_code(sh::client_errc::connect_timeout))
        ++ct.timeout;
    ++ct.failed;
    ++ct.completed;
}

// Builds a request in the shape the loaders use — a bodyful POST or a
// body-less GET, with an origin-form target (the session is already connected).
std::shared_ptr<sh::Request> make_lgen_request(const Options &opts, asio::any_io_executor ex, std::string_view path) {
    auto req = std::make_shared<sh::Request>(sh::Version::Http11, std::move(ex));
    req->set_method(opts.body_bytes > 0 ? sh::Method::Post : sh::Method::Get);
    req->set_target(std::string(path));
    if (opts.body_bytes > 0)
        req->set_body(std::string(opts.body_bytes, 'a'));
    return req;
}

// One exchange at a time on a session — HTTP/1.1's contract. Shared by the h1
// mode and by an h2c-upgrade connection whose peer refused the switch (which
// then stays HTTP/1.1 and must never be fanned out into concurrent streams).
asio::awaitable<void> serial_exchange(const std::shared_ptr<sh::detail::ClientSession> &session, const Options &opts,
                                      Counters &ct, std::string_view path) {
    for (;;) {
        if (ct.issued.fetch_add(1) >= opts.requests || ct.stopping.load())
            co_return;
        const auto t0 = std::chrono::steady_clock::now();
        auto stream = co_await session->open_stream(make_lgen_request(opts, co_await asio::this_coro::executor, path));
        if (!stream) {
            classify(ct, stream.error());
            continue;
        }
        auto ok = co_await (*stream)->read_all(64 * 1024);
        record(ct, ms_since(t0));
        if (!ok) {
            classify(ct, ok.error());
            continue;
        }
        count_status(ct, (*stream)->head().status);
    }
}

// --- HTTP/1.1: one engine session per worker, reused sequentially ------------
// (wrk -c C model). connect() returns a session the worker owns outright, so it
// belongs to this worker for the whole run — no engine gate, no convenience
// layer, just the session's open_stream. That is what makes the number
// comparable with h2load/wrk rather than dominated by per-request setup.

asio::awaitable<void> h1_conn(const std::shared_ptr<sh::detail::ClientEngine> &engine, const Options &opts,
                              Counters &ct) {
    auto got = co_await engine->connect(opts.url);
    if (!got) {
        classify(ct, got.error());
        co_return;
    }
    std::shared_ptr<sh::detail::ClientSession> session = std::move(*got);
    ++ct.opened_connections;
    co_await serial_exchange(session, opts, ct, path_query(n_url(opts.url, opts.resp_bytes)));
}

// --- HTTP/1.1: the public facade, one connection per worker -------------------
// What users get by default: the Client keeps a single connection per origin,
// so concurrent workers beyond the one in-flight exchange queue on the engine's
// gate (honest for the facade). -p h1 uses the session path instead so the
// number is not dominated by per-request convenience-layer work.

asio::awaitable<void> run_h1(const std::shared_ptr<sh::Client> &client, const Options &opts, Counters &ct) {
    const std::string url = n_url(opts.url, opts.resp_bytes);
    const bool bodyful = opts.body_bytes > 0;
    if (bodyful) {
        std::string body(opts.body_bytes, 'a');
        for (;;) {
            if (ct.issued.fetch_add(1) >= opts.requests || ct.stopping.load())
                co_return;
            const auto t0 = std::chrono::steady_clock::now();
            auto r = co_await client->post(url).body(body).send();
            record(ct, ms_since(t0));
            if (!r) {
                classify(ct, r.error());
                continue;
            }
            count_status(ct, r->status());
        }
    } else {
        for (;;) {
            if (ct.issued.fetch_add(1) >= opts.requests || ct.stopping.load())
                co_return;
            const auto t0 = std::chrono::steady_clock::now();
            auto r = co_await client->get(url).send();
            record(ct, ms_since(t0));
            if (!r) {
                classify(ct, r.error());
                continue;
            }
            count_status(ct, r->status());
        }
    }
}

// --- HTTP/2: engine sessions, C connections x M streams -----------------------

asio::awaitable<void> h2_stream_loop(std::shared_ptr<sh::detail::ClientSession> session, const Options &opts,
                                     Counters &ct) {
    const std::string path = path_query(n_url(opts.url, opts.resp_bytes));
    for (;;) {
        if (ct.issued.fetch_add(1) >= opts.requests || ct.stopping.load())
            co_return;
        const auto t0 = std::chrono::steady_clock::now();
        auto stream = co_await session->open_stream(make_lgen_request(opts, co_await asio::this_coro::executor, path));
        if (!stream) {
            classify(ct, stream.error());
            continue;
        }
        auto ok = co_await (*stream)->read_all(64 * 1024);
        record(ct, ms_since(t0));
        if (!ok) {
            classify(ct, ok.error());
            continue;
        }
        count_status(ct, (*stream)->head().status);
    }
}

asio::awaitable<void> h2_conn(const std::shared_ptr<sh::detail::ClientEngine> &engine, const Options &opts,
                              Counters &ct, std::string h2_settings_b64) {
    // The connection coroutine holds the session until each stream loop has
    // taken its own strong reference; the loops keep it alive past this frame.
    auto got = co_await engine->connect(opts.url);
    if (!got) {
        classify(ct, got.error());
        co_return;
    }
    std::shared_ptr<sh::detail::ClientSession> session = std::move(*got);
    ++ct.opened_connections;

    // h2c Upgrade: the first exchange *is* the handshake — the plain
    // open_stream() path never asks for a protocol switch, so the upgradeable
    // entry has to run before any other stream is opened. The ordering is not
    // just for the upgrade to happen: until the peer answers 101 the session is
    // an HTTP/1.1 one-exchange-at-a-time object, and the -m loops below would
    // all land on session_busy, retry without yielding, and blow the stack
    // (every hop() is an inline dispatch on the same executor, so a hot retry
    // loop is a synchronous recursion).
    if (opts.h2_upgrade) {
        const std::string path = path_query(n_url(opts.url, opts.resp_bytes));
        if (ct.issued.fetch_add(1) >= opts.requests || ct.stopping.load())
            co_return;
        if (auto h1 = std::dynamic_pointer_cast<sh::detail::Http1ClientSession<sh::TcpStreamTransport>>(session)) {
            const auto t0 = std::chrono::steady_clock::now();
            auto stream = co_await h1->open_stream_upgradeable(
                make_lgen_request(opts, co_await asio::this_coro::executor, path), h2_settings_b64);
            if (!stream) {
                classify(ct, stream.error());
                co_return;
            }
            auto ok = co_await (*stream)->read_all(64 * 1024);
            record(ct, ms_since(t0));
            if (!ok) {
                classify(ct, ok.error());
                co_return;
            }
            count_status(ct, (*stream)->head().status);
            // The peer refused the switch: the connection stayed HTTP/1.1, which
            // is serial by contract — serve it one exchange at a time instead of
            // fanning out -m multiplexed streams against the busy h1 session.
            if (session->version() == sh::Version::Http11) {
                co_await serial_exchange(session, opts, ct, path);
                co_return;
            }
        }
    }
    for (std::size_t s = 0; s < opts.streams; ++s) {
        asio::co_spawn(co_await asio::this_coro::executor, h2_stream_loop(session, opts, ct), asio::detached);
    }
}

// --- WebSocket: public client, echo round-trips -------------------------------

asio::awaitable<void> run_ws(const std::shared_ptr<sh::Client> &client, const Options &opts, Counters &ct) {
    sh::WebSocketSpec spec;
    auto ws = co_await client->open_websocket(opts.url, spec);
    if (!ws) {
        classify(ct, ws.error());
        co_return;
    }
    ++ct.opened_connections;
    const std::string payload(opts.ws_bytes, 'x');
    for (;;) {
        if (ct.issued.fetch_add(1) >= opts.requests || ct.stopping.load())
            break;
        const auto t0 = std::chrono::steady_clock::now();
        if (auto ec = co_await (*ws)->write_binary(payload); ec) {
            classify(ct, ec);
            continue;
        }
        auto msg = co_await (*ws)->read();
        record(ct, ms_since(t0));
        if (!msg) {
            classify(ct, msg.error());
            continue;
        }
        ++ct.completed;
        ++ct.succeeded;
    }
    (void)co_await (*ws)->close();
}

// --- report -------------------------------------------------------------------

void report(const Options &opts, const Counters &ct, double elapsed_s, std::size_t connections) {
    const std::size_t req_bytes = opts.body_bytes + opts.resp_bytes;
    const double secs = elapsed_s > 0 ? elapsed_s : 1e-9;
    const double rps = static_cast<double>(ct.succeeded) / secs;
    const double mbps = (static_cast<double>(ct.succeeded) * static_cast<double>(req_bytes)) / secs / (1024.0 * 1024.0);
    const std::size_t started = std::min(ct.issued.load(), opts.requests);
    std::printf("\nfinished in %.2fs, %.2f req/s, %.2f MB/s\n", secs, rps, mbps);
    std::printf("requests: %zu total, %zu started, %zu done, %zu succeeded, %zu failed, %zu errored, %zu timeout\n",
                opts.requests, started, ct.completed.load(), ct.succeeded.load(), ct.failed.load(), ct.errored.load(),
                ct.timeout.load());
    std::printf("connections: %zu\n", connections);

    std::vector<double> sorted;
    {
        std::lock_guard<std::mutex> lock(ct.lat_mu);
        sorted = ct.lats_ms;
    }
    if (!sorted.empty()) {
        std::sort(sorted.begin(), sorted.end());
        const auto mean = [&] {
            double sum = 0;
            for (double v : sorted)
                sum += v;
            return sum / static_cast<double>(sorted.size());
        }();
        std::printf("latency (ms): min %.3f mean %.3f p50 %.3f p95 %.3f p99 %.3f max %.3f\n", sorted.front(), mean,
                    sorted[sorted.size() / 2], *std::next(sorted.begin(), (sorted.size() - 1) * 95 / 100),
                    *std::next(sorted.begin(), (sorted.size() - 1) * 99 / 100), sorted.back());
    }
}

} // namespace

// Per-thread state: each worker thread runs its own io_context and its own
// Client/ClientEngine, so the connections it opens bind to *its* context (model
// A — one connection, one single-threaded context; with `-t N` there are N such
// contexts, exactly the shape of the server's IoCtxPool). This is what lets the
// loader scale across cores instead of feeding every connection through one
// thread. A watchdog coroutine lives on every context and stops it when the run
// is done / the duration budget is spent / a hard deadline hits.
struct ThreadState {
    asio::io_context ctx;
    std::shared_ptr<sh::Client> client;               // pub / ws modes
    std::shared_ptr<sh::detail::ClientEngine> engine; // h1 / h2 modes
    std::string h2_settings_b64;                      // h2 mode: HTTP2-Settings for the h2c upgrade
};

// Detects completion / the duration budget / a hang and stops *this thread's*
// own context. The counters are shared, so every watchdog reaches the same
// verdict and every context winds down together.
asio::awaitable<void> watchdog(asio::io_context &ctx, const std::chrono::steady_clock::time_point &start,
                               const Options &opts, Counters &ct) {
    const auto hard = start + std::chrono::seconds{opts.duration > 0 ? static_cast<long>(opts.duration) + 60 : 120};
    std::chrono::steady_clock::time_point stopped_at{};
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (opts.duration == 0 && ct.completed.load() >= opts.requests)
            break;
        if (opts.duration > 0 && std::chrono::duration<double>(now - start).count() >= opts.duration) {
            ct.stopping.store(true);
            if (stopped_at == std::chrono::steady_clock::time_point{})
                stopped_at = now;
            // 2s grace for the in-flight tail, then report.
            if (now - stopped_at >= std::chrono::seconds(2))
                break;
        }
        if (now >= hard) {
            ct.stopping.store(true);
            std::fprintf(stderr, "loadgen: hard deadline reached, stopping\n");
            break;
        }
        asio::steady_timer tick{co_await asio::this_coro::executor};
        tick.expires_after(std::chrono::milliseconds(10));
        co_await tick.async_wait(asio::as_tuple(asio::use_awaitable));
    }
    ctx.stop();
}

int main(int argc, char **argv) {
    const Options opts = parse_args(argc, argv);
    if (opts.protocol == "ws" && !(opts.url.rfind("ws://", 0) == 0 || opts.url.rfind("wss://", 0) == 0)) {
        std::fprintf(stderr, "ws mode needs a ws:// or wss:// url\n");
        return 2;
    }

    sh::set_log_sink(sh::make_stderr_sink(sh::LogLevel::Critical));

    Counters ct;

    // One context + one Client/Engine per thread (see ThreadState). Each thread
    // owns its connections and its kept single connection; nothing is shared
    // across contexts except the counters.
    std::vector<ThreadState> states(opts.threads);
    for (auto &ts : states) {
        sh::ClientConfig cfg;
        cfg.default_version = sh::HttpVersionPolicy::Http11;
        cfg.default_h2c = sh::H2cMode::Off;
        if (opts.protocol == "h2") {
            cfg.default_version = sh::HttpVersionPolicy::Http2;
            cfg.default_h2c = opts.h2_upgrade ? sh::H2cMode::Upgrade : sh::H2cMode::PriorKnowledge;
        }
        if (opts.protocol == "h1" || opts.protocol == "h2")
            ts.engine = std::make_shared<sh::detail::ClientEngine>(ts.ctx.get_executor(), cfg);
        else
            ts.client = std::make_shared<sh::Client>(ts.ctx.get_executor(), cfg);
        // The h2c-upgrade handshake advertises the same SETTINGS the engine's
        // HTTP/2 session will open with, so the upgrade request and the h2
        // connection agree on the window/frame limits.
        if (opts.protocol == "h2")
            ts.h2_settings_b64 = sh::detail::h2_settings_base64url(cfg.limits);
    }

    const auto start = std::chrono::steady_clock::now();
    const std::size_t per_thread = opts.concurrency / opts.threads;
    const std::size_t remainder = opts.concurrency % opts.threads;
    for (std::size_t t = 0; t < states.size(); ++t) {
        auto &ts = states[t];
        const std::size_t c_t = per_thread + (t < remainder ? 1 : 0);
        const auto ex = ts.ctx.get_executor();
        if (opts.protocol == "h1") {
            for (std::size_t c = 0; c < c_t; ++c)
                asio::co_spawn(ex, h1_conn(ts.engine, opts, ct), asio::detached);
        } else if (opts.protocol == "h2") {
            for (std::size_t c = 0; c < c_t; ++c)
                asio::co_spawn(ex, h2_conn(ts.engine, opts, ct, ts.h2_settings_b64), asio::detached);
        } else if (opts.protocol == "pub") {
            for (std::size_t c = 0; c < c_t; ++c)
                asio::co_spawn(ex, run_h1(ts.client, opts, ct), asio::detached);
        } else {
            for (std::size_t c = 0; c < c_t; ++c)
                asio::co_spawn(ex, run_ws(ts.client, opts, ct), asio::detached);
        }
        asio::co_spawn(ex, watchdog(ts.ctx, start, opts, ct), asio::detached);
    }

    std::vector<std::thread> workers;
    workers.reserve(states.size());
    for (auto &ts : states)
        workers.emplace_back([&ts] { ts.ctx.run(); });
    for (auto &w : workers)
        w.join();

    const double elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::size_t connections = ct.opened_connections.load();
    for (auto &ts : states)
        if (ts.client)
            connections = std::max(connections, ts.client->stats().connections_opened);
    report(opts, ct, elapsed_s, connections);
    return 0;
}