#pragma once

// ClientEngine: the connection engine behind the public `http::Client`.
//
// This is the *internal* half of the client (http::detail): dialing sessions,
// negotiating the HTTP version (ALPN / h2c), and opening exchanges. The public
// convenience layer (redirects, cookies, retries, decompression) lives in the
// `http::Client` facade (client/http.h) on top of these primitives.
//
// **One connection per client.** An engine keeps a single TCP connection to the
// first origin it talks to (the "each client, one connection" model):
//   * requests to that origin reuse it — HTTP/1.1 queues on it (one exchange
//     at a time), HTTP/2 multiplexes over it;
//   * a transport failure on it is detected on the next exchange and re-dials
//     transparently (one free replay, then the configured retry policy);
//   * a request to a *different* origin dials a throwaway connection that
//     closes when the exchange is done (this is what keeps cross-origin
//     redirects working without a pool).
// There is no connection pool, no per-host limiter and no idle-timer
// machinery: the complexity those existed for (idle reuse arbitration,
// eviction, cross-executor lifecycles) simply does not exist when the client
// owns one connection. Sessions inherited from the pool era are gone; the
// engine holds `m_session` directly and hands back streams whose destruction
// releases the HTTP/1.1 gate (or closes a throwaway connection).
//
// Which HTTP version a connection speaks is negotiated, never guessed:
//   * https  — ALPN. Auto offers {"h2","http/1.1"} and takes what the peer
//              picks; Http2 requires "h2"; Http11 offers only "http/1.1".
//   * http   — Auto/Http2 use h2c per H2cMode: Upgrade (one request doubles as
//              the protocol switch, and a peer that ignores it just answers
//              HTTP/1.1) or PriorKnowledge (preface immediately, no round trip,
//              only for a peer known to speak h2c). Http11 stays HTTP/1.1.
// A policy that cannot be met is an error, not a silent downgrade:
// version_not_negotiated.
//
// Concurrency (model A): the engine is *pinned* to one executor — the caller
// hands it an executor at construction and guarantees that executor is driven
// by a single thread. Every public entry point hops onto it first, and dials
// bind the sessions to it, so all engine state (the kept connection, the HTTP/1.1
// gate, the dial counter) is only ever touched on that one executor: no locks,
// and no cross-thread sharing of one Client. Another thread uses this Client by
// posting work onto its pinned executor — never by driving it directly. The
// stream handles an exchange returns may be dropped from any thread: their
// destruction hooks dispatch back onto the pinned executor before touching
// session or gate state (and hold a throwaway session strongly so it dies on
// that executor, never on the caller's thread).

#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/async_result.hpp> // async_initiate
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/ssl.hpp>
#include <chrono>
#include <cmath> // std::isfinite (retry_delay overflow guard)
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <limits> // std::numeric_limits (retry_delay overflow guard)
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "../core/http_field.h"
#include "../core/http_status.h"
#include "../core/limits.h"
#include "../core/logging.h"
#include "../core/types.h"
#include "../transport/tcp_transport.h"
#include "../transport/tls_transport.h"
#include "client_config.h"
#include "client_stream.h"
#include "cookie_jar.h"
#include "decompressing_stream.h"
#include "h1_client.h"
#include "h2_client.h"
#include "tls_client.h"
#include "url.h"

namespace simple_http::detail {

namespace asio = boost::asio;

// An opened exchange.
struct OpenedStream {
    std::shared_ptr<ClientStream> stream;
    // True when the engine reused its single kept connection for this exchange
    // rather than dialing. A read-phase failure on a *reused* kept connection
    // is a "stale kept" drop (the peer may have closed the connection while it
    // sat idle): the convenience layer gives it one free re-dial, mirroring the
    // old rule that a stale pooled connection earns a replay.
    bool reused{false};
    // The HTTP/1.1 gate holder for this exchange (non-null when it ran on the
    // kept h1 connection), so the convenience layer can hand the gate back
    // when the exchange ends in a read-phase failure — before its retry/redial
    // decision, not at the session's own failure point.
    std::shared_ptr<void> gate_token;
};

// Connection counter (dials).
struct ClientStats {
    std::size_t connections_opened{0};
};

// Whether a failure came from the transport rather than from a decision the
// client itself made (a version that could not be negotiated, a protocol error,
// an oversized body, …). Only transport failures are worth replaying: the
// single connection dropped, so nothing of this request can have been acted on
// before the failure.
inline bool transport_failure(const error_code &ec) {
    return ec.category() != client_category() || ec == make_error_code(client_errc::session_closed);
}

// --- configurable retry ------------------------------------------------------

// The delay before retry number `ordinal` (1-based): the initial backoff
// multiplied by backoff_multiplier per retry, capped at max_backoff.
inline std::chrono::milliseconds retry_delay(const RetryPolicy &policy, std::size_t ordinal) {
    if (policy.initial_backoff.count() <= 0)
        return {};
    double ms = static_cast<double>(policy.initial_backoff.count());
    for (std::size_t i = 1; i < ordinal; ++i)
        ms *= policy.backoff_multiplier;
    // Guard before the cast to int64_t: a long chain with a multiplier > 1 can
    // push `ms` to +inf (or a negative multiplier can make it non-positive), and
    // converting either to int64_t is undefined behavior. NaN also fails the
    // `ms > 0.0` test, so it collapses to "no delay" like any non-positive value.
    if (!(ms > 0.0))
        return {};
    if (policy.max_backoff.count() > 0 && ms > static_cast<double>(policy.max_backoff.count()))
        ms = static_cast<double>(policy.max_backoff.count());
    // Clamp to the largest value a milliseconds count can hold. The comparison
    // is done in double and the boundary value itself is never cast, so no
    // out-of-range int64_t conversion happens.
    const auto max_count = std::chrono::milliseconds::max().count();
    if (ms >= static_cast<double>(max_count))
        return std::chrono::milliseconds{max_count};
    return std::chrono::milliseconds{static_cast<std::int64_t>(ms)};
}

// Sleeps for the delay before retry `ordinal` (a no-op for no backoff), capped
// by the request's overall deadline so the sleep cannot push a buffered send
// past its budget. Runs on the caller's executor.
inline asio::awaitable<void>
retry_sleep(const RetryPolicy &policy, std::size_t ordinal,
            std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point{}) {
    auto delay = retry_delay(policy, ordinal);
    if (deadline != std::chrono::steady_clock::time_point{}) {
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero())
            co_return;
        const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
        if (delay.count() > remaining_ms)
            delay = std::chrono::milliseconds{remaining_ms};
    }
    if (delay.count() <= 0)
        co_return;
    asio::steady_timer timer{co_await asio::this_coro::executor};
    timer.expires_after(delay);
    co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
}

// One request's retry budget, shared by the engine's exchange-open phase and
// the convenience layer's response-read phase so `max_retries` counts the
// whole request rather than each phase. `used` is bumped only when a retry is
// actually taken. `deadline` (0 = unbounded) is the hard wall-clock limit for
// the whole request including retries and backoff sleeps.
struct RetryBudget {
    const RetryPolicy *policy{nullptr};
    std::size_t used{0};
    std::chrono::steady_clock::time_point deadline{};
};

// Whether a failed attempt may be retried against `budget`. `request_sent`
// tells the default rule whether anything of this request reached the wire: a
// dial that never connected sent nothing, so even a POST may be retried; a
// failure after the request was written needs an idempotent method or a stream
// the peer provably did not process. Bumps budget->used when it allows the
// retry. (`pooled` is kept only for the caller-chosen condition's signature —
// there are no pooled connections anymore; the engine always passes false.)
inline bool retry_allowed(RetryBudget &budget, const Request &spec, const error_code &ec, bool pooled,
                          bool request_sent) {
    if (!budget.policy || budget.policy->max_retries == 0 || budget.used >= budget.policy->max_retries)
        return false;
    if (budget.deadline != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() >= budget.deadline)
        return false;
    const RetryPolicy &policy = *budget.policy;
    // A streamed body cannot be re-sent; a retry would silently send nothing.
    if (policy.require_replayable && spec.stream_body())
        return false;
    const bool want = policy.condition ? policy.condition(ec, spec.method(), pooled)
                                       : (ec == make_error_code(client_errc::stream_refused) ||
                                          is_idempotent(spec.method()) || (!request_sent && transport_failure(ec)));
    if (!want)
        return false;
    ++budget.used;
    return true;
}

// --- the single HTTP/1.1 exchange gate ----------------------------------------
//
// One h1 connection serves one exchange at a time, so a request that arrives
// while another is in flight queues here (FIFO) instead of dialing an extra
// connection or failing with session_busy. The gate is acquired before opening
// the exchange and released when the caller drops the stream (the engine wires
// it through ClientStream::set_on_destroy). HTTP/2 sessions bypass it — they
// multiplex.
//
// The queue holds *weak* liveness probes: a waiter that is cancelled while
// parked (its coroutine destroyed) drops the strong token its frame held, so
// release() reaps the dead entry instead of invoking a completion handler whose
// coroutine is gone (a use-after-free).
class ExchangeGate {
  public:
    // Acquires the gate, returning a *holder token*. Every release is scoped to
    // that token and no-ops when the gate has since been re-acquired, which is
    // what makes the release idempotent across the many places an exchange can
    // end (session finish, an open-phase failure, a read-phase failure handled
    // by the convenience layer, the caller dropping the stream). Without the
    // token, one such path — the session failing mid-exchange — released the
    // gate before its owner finished deciding whether to retry/redial, waking a
    // queued request onto a dying connection (concurrent requests on a single
    // shared HTTP/1.1 Client failed in a cascade).
    //
    // The engine is pinned to one executor (model A), so acquire()/release()
    // only ever run on it, and every external release site dispatches back
    // onto it first. No lock is needed; the FIFO is a plain single-threaded
    // queue.
    asio::awaitable<std::shared_ptr<void>> acquire() {
        auto token = std::make_shared<int>(0);
        for (;;) {
            if (!m_busy) {
                m_busy = true;
                m_holder = token;
                co_return token;
            }
            // Park in FIFO order. `token` (a local, alive across the suspension)
            // is the strong reference that keeps this waiter alive; the queued
            // weak_ptr is the liveness probe that lets release() reap a waiter
            // whose acquire() coroutine was destroyed while parked, instead of
            // invoking a completion handler that went with it.
            //
            // The completion is a type-erased handler stored in the queue entry,
            // not a per-acquire channel: the channel was allocated on every call
            // — even when the gate was free and the channel was then discarded
            // unused. The handler stores inline with no allocation beyond the
            // queue node itself.
            auto weak = std::weak_ptr<int>(token);
            auto token_token = asio::as_tuple(asio::use_awaitable);
            auto [ec] = co_await asio::async_initiate<decltype(token_token), void(error_code)>(
                [this, weak](auto handler) {
                    m_waiters.push_back(
                        Waiter{weak, asio::any_completion_handler<void(error_code)>{std::move(handler)}});
                },
                token_token);
            (void)ec;
        }
    }

    // Releases the gate only if `token` still holds it (idempotent across the
    // overlapping end-of-exchange paths — the trailing ones no-op instead of
    // waking a second request onto a live exchange).
    void release(const std::shared_ptr<void> &token) {
        if (!token || m_holder != token)
            return;
        m_busy = false;
        m_holder.reset();
        // Reap waiters whose acquire() coroutine was destroyed while parked.
        while (!m_waiters.empty() && m_waiters.front().alive.expired())
            m_waiters.pop_front();
        if (!m_waiters.empty()) {
            Waiter wake = std::move(m_waiters.front());
            m_waiters.pop_front();
            wake.done(error_code{});
        }
    }

  private:
    struct Waiter {
        // The strong ref lives in the parked acquire() coroutine's frame; this
        // is only the liveness probe release() checks.
        std::weak_ptr<int> alive;
        // Resumes the parked acquire() coroutine when the gate is released.
        asio::any_completion_handler<void(error_code)> done;
    };
    bool m_busy{false};
    std::shared_ptr<void> m_holder;
    std::deque<Waiter> m_waiters;
};

// Wraps an awaitable in a deadline. The awaitable's own I/O is cancelled when
// the timer wins, which is why callers must also cancel the stream.
template <typename T>
asio::awaitable<std::expected<T, error_code>> await_with_deadline(asio::awaitable<T> op,
                                                                  std::chrono::milliseconds limit) {
    using namespace asio::experimental::awaitable_operators;
    if (limit.count() <= 0)
        co_return co_await std::move(op);

    auto deadline_op = [limit]() -> asio::awaitable<void> {
        asio::steady_timer timer{co_await asio::this_coro::executor};
        timer.expires_after(limit);
        co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
    };
    auto outcome = co_await (std::move(op) || deadline_op());
    if (auto *value = std::get_if<T>(&outcome))
        co_return std::move(*value);
    co_return std::unexpected{make_error_code(client_errc::request_timeout)};
}

// Drops a reference to a session on the session's own executor. A session's
// object graph (request Body channels, timers) is created on that executor
// (model A), so the destructor must run there even when the last reference is
// released from a foreign thread — the ClientEngine's destructor may run on
// whichever thread dropped the last Client, and it hands the kept session to
// this helper (ASan: channel_service base_destroy UAF when the graph dies
// off-executor). The reference travels in a closure *posted* to the session's
// executor (post, not dispatch: the destruction must always be deferred so it
// never happens inline on the releasing thread, e.g. inside the engine's own
// destructor); per the io_context-outlives-session contract that executor is
// still alive.
inline void release_session(std::shared_ptr<ClientSession> sess) {
    if (!sess)
        return;
    // Evaluate the executor before the closure's capture moves `sess` out:
    // function-argument evaluation order is unspecified, so `sess->executor()`
    // and the `[sess = std::move(sess)]` capture must not race each other.
    const auto ex = sess->executor();
    asio::post(ex, [sess = std::move(sess)] {});
}

class ClientEngine {
  public:
    // Pinned to `ex` for its whole life (model A): the caller guarantees that
    // executor is driven by a single thread. Building the TLS context can fail
    // (missing CA file, unusable client certificate), and it does so here
    // rather than on the first request — configuration faults should surface at
    // construction, like the server's.
    explicit ClientEngine(asio::any_io_executor ex, ClientConfig config = {})
        : m_executor(std::move(ex)), m_config(std::move(config)), m_ssl_context(make_client_ssl_context(m_config.tls)) {
    }

    ~ClientEngine() {
        // The kept session must die on its own executor (see release_session):
        // its object graph was created there, and the engine may be destroyed
        // from a thread the session never ran on. Every other member is only
        // ever touched on m_executor (model A), so nothing else needs guarding
        // — the caller must simply not destroy the engine while an operation is
        // in flight (standard object lifetime).
        release_session(std::move(m_session));
        m_origin.clear();
    }

    ClientEngine(const ClientEngine &) = delete;
    ClientEngine &operator=(const ClientEngine &) = delete;

    // The live config. Must be configured before the engine serves any request:
    // in-flight exchanges read m_config (retry policy, limits, auto_decompress,
    // …) from their own executors, so mutating it while a request is running is
    // a data race. Configure once, then only read.
    ClientConfig &config() { return m_config; }

    const ClientConfig &config() const { return m_config; }

    std::size_t opened_count() const { return m_opened.load(std::memory_order_relaxed); }

    // Every public entry point hops onto the pinned executor before touching
    // any engine/session state (model A). When already running on it — the
    // usual case — this is an inline dispatch: no suspension, no queue hop.
    asio::awaitable<void> hop() { co_await asio::dispatch(asio::bind_executor(m_executor, asio::use_awaitable)); }

    // Hands the single-connection h1 gate back after an exchange that ended in
    // a read-phase failure (the convenience layer owns that decision — see
    // OpenedStream::gate_token). Idempotent: no-ops if the gate was already
    // released (e.g. by the stream-destruction safety net). Runs on the pinned
    // executor (the convenience layer calls it after hopping).
    void release_h1_gate(const std::shared_ptr<void> &token) { m_h1_gate->release(token); }

    // Opens a request/response exchange without buffering anything: the caller
    // writes the body on the returned stream (Request::stream_body), then
    // reads the response. This is the streaming entry point the public client
    // (client/http.h) builds its Stream on.
    asio::awaitable<std::expected<std::shared_ptr<ClientStream>, error_code>> stream(ClientTarget target,
                                                                                     std::shared_ptr<Request> spec) {
        co_await hop();
        auto opened = co_await start_exchange(std::move(target), std::move(spec), {});
        if (!opened)
            co_return std::unexpected{opened.error()};
        co_return std::move(opened->stream);
    }
    asio::awaitable<std::expected<std::shared_ptr<ClientStream>, error_code>> stream(std::string_view url,
                                                                                     std::shared_ptr<Request> spec) {
        co_await hop();
        auto parsed = parse_url(url);
        if (!parsed)
            co_return std::unexpected{parsed.error()};
        if (spec->target().empty())
            spec->set_target(std::string{parsed->target});
        co_return co_await stream(target_from_url(*parsed), std::move(spec));
    }

    // Dial a fresh session and hand it to the caller — the load-generator and
    // any caller that wants to hold a connection itself. The caller owns it
    // (closes by dropping the last reference); it is not the engine's single
    // kept connection.
    asio::awaitable<std::expected<std::shared_ptr<ClientSession>, error_code>> connect(ClientTarget target) {
        co_await hop();
        co_return co_await dial(std::move(target), m_executor);
    }
    asio::awaitable<std::expected<std::shared_ptr<ClientSession>, error_code>> connect(std::string_view url) {
        co_await hop();
        auto parsed = parse_url(url);
        if (!parsed)
            co_return std::unexpected{parsed.error()};
        co_return co_await connect(target_from_url(*parsed));
    }

    // Starts one exchange on the engine's single kept connection (to the first
    // origin it talked to), dialing a throwaway connection for other origins.
    // A non-empty spec.target is used as-is; empty means the origin's "/".
    asio::awaitable<std::expected<OpenedStream, error_code>> open_stream(ClientTarget target,
                                                                         std::shared_ptr<Request> spec) {
        co_await hop();
        co_return co_await start_exchange(std::move(target), std::move(spec), /*url_target=*/{});
    }
    asio::awaitable<std::expected<OpenedStream, error_code>> open_stream(std::string_view url,
                                                                         std::shared_ptr<Request> spec) {
        co_await hop();
        auto parsed = parse_url(url);
        if (!parsed)
            co_return std::unexpected{parsed.error()};
        std::string fallback{parsed->target};
        co_return co_await start_exchange(target_from_url(*parsed), std::move(spec), std::move(fallback));
    }

    // Starts one exchange: reuse the single kept connection when the target is
    // its origin, dial (and keep) a new one on the first use of any origin,
    // and dial a throwaway for any other origin. A transport failure on the
    // kept connection drops it and re-dials once, free; further retries follow
    // the configured policy. `budget` (optional) is the caller's retry budget,
    // shared with its own retry loop so max_retries counts the whole request.
    asio::awaitable<std::expected<OpenedStream, error_code>> start_exchange(ClientTarget target,
                                                                            std::shared_ptr<Request> spec,
                                                                            std::string url_target,
                                                                            RetryBudget *budget = nullptr) {
        co_await hop(); // the whole exchange runs on the pinned executor (model A)
        if (spec->target().empty())
            spec->set_target(url_target.empty() ? "/" : url_target);

        // Every public entry point funnels through here, so this is the one
        // place that has to advertise what we can decode. A caller that set
        // accept-encoding itself keeps full control of the negotiation.
        if (m_config.auto_decompress && !spec->headers().contains("accept-encoding")) {
            std::string wanted = accept_encoding_value(m_config.accept_encodings);
            if (!wanted.empty()) {
                spec->mutable_headers().add("accept-encoding", std::move(wanted));
            }
        }

        RetryBudget owned{&m_config.retry, 0};
        if (!budget)
            budget = &owned;

        // The identity of the *connection policy* this target needs. Two
        // targets sharing an authority but wanting a different protocol
        // preference (version policy, h2c mode, a `pool_tag` for distinct TLS
        // credentials, SNI) cannot share the single kept connection, so the key
        // carries all of it — the same partition the old pool used (scheme,
        // host, port, version policy, tag).
        const std::string origin = (target.use_tls ? "https://" : "http://") + target.authority() + std::string{"|v"} +
                                   std::to_string(static_cast<int>(target.version)) + std::string{"|h"} +
                                   std::to_string(static_cast<int>(target.h2c)) + std::string{"|p"} + target.pool_tag +
                                   (target.sni.empty() ? std::string{} : std::string{"|s"} + target.sni);
        bool redialed = false; // the single free transparent re-dial
        for (;;) {
            // Phase 1: reuse the kept connection, or drop a dead one so a fresh
            // connection can take the kept slot. All of this engine state is
            // only touched on the pinned executor (model A), so there is no
            // lock — concurrent requests interleave here only at co_await
            // points, exactly like the server's per-connection handlers.
            std::shared_ptr<ClientSession> session;
            bool reused_kept = false; // took the *pre-existing* kept connection
            bool dial_needed = false;
            // Reuse when the kept connection can *serve another request* —
            // keepable(), not reusable(). A connection with an exchange in
            // flight is still a reuse: HTTP/1.1 queues the next request on
            // the one-exchange gate, HTTP/2 multiplexes. reusable() would
            // reject a busy connection and make every concurrent request
            // dial a throwaway (and drop the kept one), which is exactly the
            // pool-free model the gate exists to avoid.
            if (m_session && origin == m_origin && m_session->alive() && m_session->keepable()) {
                session = m_session;
                reused_kept = true;
            } else {
                if (m_session && origin == m_origin) {
                    // Not alive / not at a boundary: forget it before dialing
                    // so the next request does not write into a closing
                    // connection. We are on the pinned executor, so the drop
                    // destroys the session right here, where it was born.
                    m_session.reset();
                    m_origin.clear();
                }
                dial_needed = true;
            }

            if (dial_needed) {
                // Dialed while suspended on I/O: another coroutine on this same
                // executor may bind this origin while we wait.
                auto dialed = co_await dial(target, m_executor);
                if (!dialed) {
                    // Dialing failed before anything reached the wire: retry per
                    // the policy (any method — nothing was sent).
                    if (retry_allowed(*budget, *spec, dialed.error(), /*pooled=*/false, /*request_sent=*/false)) {
                        SIMPLE_HTTP_ERROR_LOG("client: dialing {} failed ({}), retry {} of {}", target.authority(),
                                              dialed.error().message(), budget->used, m_config.retry.max_retries);
                        co_await retry_sleep(m_config.retry, budget->used, budget->deadline);
                        continue;
                    }
                    co_return std::unexpected{dialed.error()};
                }
                session = std::move(*dialed);
                // Bind as the kept connection if the slot is still free — a
                // concurrent coroutine on this executor may have bound this
                // origin while we dialed, in which case our session stays a
                // throwaway (closed after this exchange).
                if (m_session == nullptr && (m_origin.empty() || m_origin == origin)) {
                    m_origin = origin;
                    m_session = session;
                }
            }

            // HTTP/1.1: one exchange on the *kept* connection at a time — wait
            // for it. A throwaway (other-origin) exchange runs on its own
            // connection and never touches the gate.
            const bool h1 = session->version() != Version::Http2;
            const bool keep_exchange = session == m_session && origin == m_origin;
            std::shared_ptr<void> gate_token;
            if (h1 && keep_exchange) {
                gate_token = co_await m_h1_gate->acquire();
                // The gate is handed back exactly when this exchange completes —
                // the session's exchange-done hook, re-armed per attempt with
                // this attempt's holder token (idempotent), NOT on a session
                // failure or the caller dropping the stream. A caller may keep
                // the Stream handle alive while the next request proceeds.
                session->set_on_exchange_done([gate = m_h1_gate, token = gate_token] { gate->release(token); });
            }
            auto usable = co_await negotiate_and_open(session, target, spec->clone_for_replay(m_executor));
            if (usable) {
                auto stream = maybe_decompressing_stream(*usable, m_config.auto_decompress);
                stream->set_gate_token(gate_token);
                if (keep_exchange) {
                    if (h1) {
                        // A successful upgrade made the connection HTTP/2: the
                        // gate is no longer used. Otherwise, only drop the
                        // connection if the request asked to close (or the body
                        // did not end at a boundary) when the caller is done with
                        // the stream; the gate itself frees via the exchange-done
                        // hook above.
                        if (stream->version() == Version::Http2)
                            m_h1_gate->release(gate_token);
                        else
                            stream->set_on_destroy([ex = m_executor, gate = m_h1_gate, token = gate_token,
                                                    weak = std::weak_ptr<ClientSession>(session),
                                                    done = session->exchange_done_flag()] {
                                // The caller may drop the stream from any thread:
                                // the session and the gate only ever live on the
                                // pinned executor, so the cleanup dispatches back
                                // onto it first. The session is held *weakly* here
                                // — the engine's own m_session (or the stream's
                                // member) already keeps it alive, and a strong
                                // capture would let a stray Stream handle keep a
                                // dead connection alive indefinitely.
                                asio::dispatch(ex, [gate, token, done, weak] {
                                    auto sess = weak.lock();
                                    if (sess && done && !done->load(std::memory_order_acquire))
                                        sess->close();
                                    gate->release(token); // idempotent safety net
                                });
                            });
                    }
                    // kept HTTP/2: multiplex, nothing to release.
                } else {
                    // A throwaway connection (different origin or a policy that
                    // lost the kept slot race): close it when the caller is done
                    // with this exchange. The session is captured *strongly* on
                    // purpose: it has no other owner — by the time the destructor
                    // fires, the stream's own session member is already gone, and
                    // this dispatch is what moves the last reference onto the
                    // pinned executor. A weak capture here would let the session
                    // die on the caller's (possibly foreign) thread, destroying
                    // its executor-bound object graph there — the exact lifetime
                    // bug the strong capture + dispatch exist to prevent.
                    stream->set_on_destroy([ex = m_executor, sess = std::move(session)] {
                        asio::dispatch(ex, [sess = std::move(sess)] { sess->close(); });
                    });
                }
                co_return OpenedStream{std::move(stream), /*reused=*/reused_kept, std::move(gate_token)};
            }

            const error_code ec = usable.error();
            if (h1 && keep_exchange && gate_token)
                m_h1_gate->release(gate_token);

            // A throwaway connection (not the kept one) is discarded on any
            // failure.
            const bool throwaway = session != m_session;
            if (throwaway) {
                session->close();
            } else if ((transport_failure(ec) || ec == make_error_code(client_errc::session_closed)) && !redialed) {
                // The single kept connection dropped: forget it and re-dial
                // once, free (this request provably got nothing back). A
                // session_closed here means the kept connection was dropped
                // between the reuse decision and the open (a concurrent request
                // on the same shared connection failed and closed it) — that is
                // the same stale-kept case and must re-dial just as freely,
                // otherwise every queued request would fail in a cascade.
                redialed = true;
                session->close();
                m_session.reset();
                m_origin.clear();
                SIMPLE_HTTP_ERROR_LOG("client: the kept connection to {} failed ({}), re-dialing", target.authority(),
                                      ec.message());
                continue;
            }
            if (retry_allowed(*budget, *spec, ec, /*pooled=*/false, /*request_sent=*/true)) {
                SIMPLE_HTTP_ERROR_LOG("client: exchange on {} failed ({}), retry {} of {}", target.authority(),
                                      ec.message(), budget->used, m_config.retry.max_retries);
                co_await retry_sleep(m_config.retry, budget->used, budget->deadline);
                continue;
            }
            co_return std::unexpected{ec};
        }
    }

    // Resolves, connects and TLS-handshakes to `target`, returning the raw
    // transport — no HTTP session is started. The caller (the WebSocket
    // client) speaks its own protocol over it.
    // Public: the WebSocket client (client/ws_client.h) dials through here.
    using DialedTransport = std::variant<std::shared_ptr<TcpStreamTransport>, std::shared_ptr<TlsStreamTransport>>;
    asio::awaitable<std::expected<DialedTransport, error_code>> dial_transport(ClientTarget target,
                                                                               asio::any_io_executor ex) {
        co_await hop(); // the socket and every timer below bind to the pinned executor
        error_code ec;
        auto endpoints = co_await resolve(target, ex);
        if (!endpoints)
            co_return std::unexpected{endpoints.error()};

        auto socket = std::make_shared<asio::ip::tcp::socket>(ex);
        apply_socket_options(*socket);
        if (auto cec = co_await connect_with_deadline(target.authority(), *socket, *endpoints)) {
            socket->close();
            co_return std::unexpected{cec};
        }
        error_code pe;
        auto peer = socket->remote_endpoint(pe);
        ++m_opened;

        if (!target.use_tls) {
            co_return DialedTransport{std::make_shared<TcpStreamTransport>(std::move(socket), peer)};
        }

        auto stream = std::make_shared<asio::ssl::stream<asio::ip::tcp::socket>>(std::move(*socket), *m_ssl_context);
        auto transport = std::make_shared<TlsStreamTransport>(stream, peer);

        ClientTlsHandshake hs;
        hs.sni = target.sni.empty() && !m_config.tls.sni_override.empty() ? m_config.tls.sni_override
                                                                          : std::string{target.sni_host()};
        hs.verify_host = m_config.tls.verify_host && m_config.tls.verify_peer;
        // The caller's version policy decides the ALPN list (dial_transport is
        // also the WebSocket client's path, which pins http/1.1 itself — see
        // ws_client.h). A TLS WebSocket must not negotiate h2, or the upgrade
        // would be impossible.
        hs.alpn_wire = alpn_wire_list(target, m_config.tls);

        auto outcome = co_await await_with_deadline(tls_client_handshake(*transport, hs), m_config.connect_timeout);
        if (!outcome) {
            transport->close();
            co_return std::unexpected{outcome.error()};
        }
        if (error_code e = *outcome) {
            SIMPLE_HTTP_ERROR_LOG("client: TLS handshake with {} failed: {}", target.sni_host(), e.message());
            transport->close();
            co_return std::unexpected{e};
        }
        co_return DialedTransport{std::move(transport)};
    }

  private:
    // The ClientTarget a URL describes, with the client-wide defaults for the
    // knobs a URL cannot express.
    ClientTarget target_from_url(const Url &url) const {
        ClientTarget target = url.to_target();
        target.version = m_config.default_version;
        target.h2c = m_config.default_h2c;
        return target;
    }

    // Opens a stream, performing the h2c upgrade when the target asks for it.
    asio::awaitable<std::expected<std::shared_ptr<ClientStream>, error_code>>
    negotiate_and_open(const std::shared_ptr<ClientSession> &session, const ClientTarget &target,
                       std::shared_ptr<Request> spec) {
        const bool want_upgrade = !target.use_tls && target.h2c == H2cMode::Upgrade &&
                                  target.version != HttpVersionPolicy::Http11 && !spec->stream_body();
        if (want_upgrade) {
            auto h1 = std::dynamic_pointer_cast<Http1ClientSession<TcpStreamTransport>>(session);
            if (h1 && h1->upgrade_available(spec->stream_body())) {
                // The upgrading request is consumed by the h2c handshake; the
                // plain path below would need it whole, so each path gets its
                // own shared copy (the body rebuilds from the saved source).
                auto stream = co_await h1->open_stream_upgradeable(
                    spec->clone_for_replay(co_await asio::this_coro::executor), h2_settings_base64url(m_config.limits));
                if (!stream)
                    co_return std::unexpected{stream.error()};
                // A pinned HTTP/2 policy is not satisfied by an ignored upgrade.
                if (target.version == HttpVersionPolicy::Http2 && (*stream)->version() != Version::Http2) {
                    (void)co_await (*stream)->cancel();
                    co_return std::unexpected{make_error_code(client_errc::version_not_negotiated)};
                }
                co_return *stream;
            }
        }
        co_return co_await session->open_stream(std::move(spec));
    }

    // Dials (resolving, connecting, handshaking, negotiating) and starts a
    // session. The session is handed straight to the caller: the engine either
    // keeps it as its single connection or the caller closes it.
    asio::awaitable<std::expected<std::shared_ptr<ClientSession>, error_code>> dial(ClientTarget target,
                                                                                    asio::any_io_executor ex) {
        auto transport = co_await dial_transport(target, ex);
        if (!transport)
            co_return std::unexpected{transport.error()};
        auto session = co_await std::visit(
            [&](auto &typed) -> asio::awaitable<std::expected<std::shared_ptr<ClientSession>, error_code>> {
                using Transport = std::decay_t<decltype(*typed)>;
                // An HTTP/1.1 session, or an HTTP/2 one when the policy says so
                // (h2c prior-knowledge) / ALPN selected h2, and the wire allows
                // it. WebSockets never come through here — see dial_transport.
                if constexpr (std::is_same_v<Transport, TlsStreamTransport>) {
                    const std::string_view alpn = typed->alpn_selected();
                    if (target.version == HttpVersionPolicy::Http2 && alpn != "h2") {
                        typed->close();
                        co_return std::unexpected{make_error_code(client_errc::version_not_negotiated)};
                    }
                    if (alpn == "h2") {
                        auto session = std::make_shared<Http2ClientSession<TlsStreamTransport>>(
                            typed, target, m_config.limits, m_config.idle_timeout);
                        if (auto ec = co_await session->start(); ec) {
                            session->close();
                            co_return std::unexpected{ec};
                        }
                        co_return session;
                    }
                    auto session = std::make_shared<Http1ClientSession<TlsStreamTransport>>(
                        typed, target.authority(), m_config.limits, m_config.idle_timeout);
                    wire_h1_session(session, target);
                    co_return session;
                } else {
                    const bool speak_h2_at_once =
                        target.h2c == H2cMode::PriorKnowledge && target.version != HttpVersionPolicy::Http11;
                    if (speak_h2_at_once) {
                        auto session = std::make_shared<Http2ClientSession<TcpStreamTransport>>(
                            typed, target, m_config.limits, m_config.idle_timeout);
                        if (auto ec = co_await session->start(); ec) {
                            session->close();
                            co_return std::unexpected{ec};
                        }
                        co_return session;
                    }
                    auto session = std::make_shared<Http1ClientSession<TcpStreamTransport>>(
                        typed, target.authority(), m_config.limits, m_config.idle_timeout);
                    wire_h1_session(session, target);
                    co_return session;
                }
            },
            *transport);
        co_return session;
    }

    // Gives an HTTP/1.1 session its h2c-upgrade machinery: a successful upgrade
    // hands the connection to an HTTP/2 session that is started with the
    // upgrading request as stream 1 (RFC 9113 §3.2). The successor reports
    // through this session (it fronts the connection), and the successor
    // self-holds via its resident loops, so nothing else needs to keep it alive.
    template <typename Session> void wire_h1_session(std::shared_ptr<Session> session, const ClientTarget &target) {
        using Transport = std::decay_t<decltype(*session->transport())>;
        auto config = m_config;
        std::weak_ptr<ClientSession> weak_h1 = session;
        session->set_h2_upgrade_factory(
            [config, target, weak_h1](std::shared_ptr<Transport> transport, std::shared_ptr<Request> seed,
                                      std::string initial)
                -> asio::awaitable<std::expected<
                    std::pair<std::shared_ptr<ClientSession>, std::shared_ptr<ClientStream>>, error_code>> {
                (void)seed; // the peer already has the request: only stream 1 is
                            // recorded
                auto h2 = std::make_shared<Http2ClientSession<Transport>>(transport, target, config.limits,
                                                                          config.idle_timeout);
                auto stream = co_await h2->start_with_stream(std::shared_ptr<Request>{}, std::move(initial));
                if (!stream) {
                    h2->close();
                    co_return std::unexpected{stream.error()};
                }
                co_return std::make_pair(std::shared_ptr<ClientSession>{h2}, *stream);
            });
    }

    asio::awaitable<std::expected<std::vector<asio::ip::tcp::endpoint>, error_code>>
    resolve(const ClientTarget &target, const asio::any_io_executor &ex) {
        if (m_config.resolve) {
            auto [ec, endpoints] = co_await m_config.resolve(target.host, std::to_string(target.effective_port()));
            if (ec)
                co_return std::unexpected{ec};
            if (endpoints.empty())
                co_return std::unexpected{make_error_code(asio::error::host_not_found)};
            co_return endpoints;
        }
        auto resolver = std::make_shared<asio::ip::tcp::resolver>(ex);
        auto [ec, results] = co_await resolver->async_resolve(target.host, std::to_string(target.effective_port()),
                                                              asio::as_tuple(asio::use_awaitable));
        if (ec)
            co_return std::unexpected{ec};
        std::vector<asio::ip::tcp::endpoint> endpoints;
        endpoints.reserve(results.size());
        for (const auto &entry : results)
            endpoints.push_back(entry.endpoint());
        co_return endpoints;
    }

    asio::awaitable<error_code> connect_with_deadline(const std::string &authority, asio::ip::tcp::socket &socket,
                                                      const std::vector<asio::ip::tcp::endpoint> &endpoints) {
        using namespace asio::experimental::awaitable_operators;
        auto connect_op = [&socket, &endpoints]() -> asio::awaitable<std::tuple<error_code, asio::ip::tcp::endpoint>> {
            co_return co_await asio::async_connect(socket, endpoints, asio::as_tuple(asio::use_awaitable));
        };
        if (m_config.connect_timeout.count() <= 0) {
            auto [ec, endpoint] = co_await connect_op();
            (void)endpoint;
            co_return ec;
        }
        auto deadline_op = [this]() -> asio::awaitable<void> {
            asio::steady_timer timer{co_await asio::this_coro::executor};
            timer.expires_after(m_config.connect_timeout);
            co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        };
        auto outcome = co_await (connect_op() || deadline_op());
        if (auto *result = std::get_if<std::tuple<error_code, asio::ip::tcp::endpoint>>(&outcome)) {
            co_return std::get<0>(*result);
        }
        SIMPLE_HTTP_ERROR_LOG("client: connecting to {} timed out after {}ms", authority,
                              m_config.connect_timeout.count());
        (void)authority; // only reported through the log, which may be compiled out
        co_return make_error_code(client_errc::connect_timeout);
    }

    void apply_socket_options(asio::ip::tcp::socket &socket) {
        error_code ec;
        if (m_config.tcp_nodelay)
            socket.set_option(asio::ip::tcp::no_delay(true), ec);
        if (m_config.tcp_keepalive)
            socket.set_option(asio::socket_base::keep_alive(true), ec);
        if (m_config.socket_setup) {
            try {
                m_config.socket_setup(socket);
            } catch (const std::exception &e) {
                SIMPLE_HTTP_ERROR_LOG("client: socket_setup threw: {}", e.what());
            }
        }
    }

    // The executor this engine is pinned to (model A): every entry point hops
    // here first, and dials bind the sessions to it, so all of the state below
    // is only ever touched on this one executor — no locks.
    asio::any_io_executor m_executor;
    ClientConfig m_config;
    std::shared_ptr<asio::ssl::context> m_ssl_context; // outlives every stream using it
    std::shared_ptr<ExchangeGate> m_h1_gate{std::make_shared<ExchangeGate>()};
    // The single kept connection and its identity, touched only on m_executor
    // (start_exchange runs there; the destructor moves the session out for
    // release_session before this member dies).
    std::string m_origin;                     // the kept connection's origin
    std::shared_ptr<ClientSession> m_session; // the single kept connection
    std::atomic<std::size_t> m_opened{0};     // dial counter; read by stats() from any thread
};

} // namespace simple_http::detail