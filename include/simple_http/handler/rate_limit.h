#pragma once

// Rate limiting — a token-bucket limiter with a 429 + Retry-After response,
// the tower-http RateLimitLayer / golang.org/x/time/rate shape, in two forms:
//
//   // One bucket for the whole application:
//   server.use(simple_http::middleware::rate_limit(
//       simple_http::middleware::make_token_bucket(100, 200))); // 100 req/s, 200 burst
//
//   // Per-client buckets (key = client address by default — pair with real_ip()
//   // for the forwarded address — or any key the handler can derive):
//   server.use(simple_http::middleware::rate_limit(
//       std::make_shared<simple_http::middleware::RateLimiter>(10, 20), // per-IP: 10/s, burst 20
//       [](const RequestPtr &req) { return std::string{req->peer_address()}; }));
//
// A refused request is answered 429 (body "too many requests") with a
// Retry-After equal to the seconds until the bucket refills — a plain
// keep-alive response, like every other rejection in the router.

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "../core/http_field.h"
#include "../core/http_status.h"
#include "builtin_middleware.h" // ClientIp (detail::request_key)
#include "handler.h"

namespace simple_http {
namespace middleware {

// A token bucket, thread-safe. `rate` tokens are added per second, up to a
// burst-sized capacity; try_acquire() consumes one token or fails when empty.
class TokenBucket {
  public:
    TokenBucket(double rate, std::size_t burst)
        : m_rate(rate), m_burst(burst), m_tokens(static_cast<double>(burst)), m_last(std::chrono::steady_clock::now()) {
    }

    bool try_acquire() {
        std::lock_guard<std::mutex> lock(m_mutex);
        refill_locked();
        if (m_tokens >= 1.0) {
            m_tokens -= 1.0;
            return true;
        }
        return false;
    }

    // Whole seconds until at least one token is available (0 if one is).
    // Consults the same state as try_acquire; the caller answers 429 with this
    // as Retry-After.
    std::int64_t seconds_until_token() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        refill_locked();
        if (m_tokens >= 1.0) {
            return 0;
        }
        return static_cast<std::int64_t>(std::ceil((1.0 - m_tokens) / m_rate));
    }

  private:
    void refill_locked() const {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - m_last).count();
        m_tokens = std::min(static_cast<double>(m_burst), m_tokens + elapsed * m_rate);
        m_last = now;
    }

    mutable std::mutex m_mutex;
    double m_rate;
    std::size_t m_burst;
    mutable double m_tokens;
    mutable std::chrono::steady_clock::time_point m_last;
};

// The bucket factory the middleware shares: build one bucket, hand it to
// rate_limit(), and — when needed — to any code that wants to observe or share
// the same budget (per-connection throttling, a health metric).
inline std::shared_ptr<TokenBucket> make_token_bucket(double rate, std::size_t burst) {
    return std::make_shared<TokenBucket>(rate, burst);
}

// A per-key limiter: an independent token bucket per key, capped at max_keys so
// a hostile key space cannot grow memory without bound (new keys past the cap
// are refused, which reads as overload).
class RateLimiter {
  public:
    RateLimiter(double rate, std::size_t burst, std::size_t max_keys = 10000)
        : m_rate(rate), m_burst(burst), m_max_keys(max_keys) {}

    bool try_acquire(std::string_view key) {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::string owned{key}; // the map is keyed by std::string
        auto it = m_buckets.find(owned);
        if (it == m_buckets.end()) {
            // Check the cap *before* inserting: operator[] added the node first
            // and only then consulted max_keys, so a hostile key space grew the
            // map without bound even though new keys were "refused".
            if (m_buckets.size() >= m_max_keys) {
                return false;
            }
            it = m_buckets.emplace(owned, std::make_shared<TokenBucket>(m_rate, m_burst)).first;
        }
        return it->second->try_acquire();
    }

    // How many keys currently hold a bucket. Read-only introspection for tests
    // and metrics; never grows past max_keys once try_acquire() is fixed.
    std::size_t bucket_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_buckets.size();
    }

  private:
    double m_rate;
    std::size_t m_burst;
    std::size_t m_max_keys;
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, std::shared_ptr<TokenBucket>> m_buckets;
};

namespace detail {

// The default per-request key for RateLimiter-based limiting: the TCP peer.
// With real_ip() in the chain the field's ClientIp state is preferred, so a
// forwarded address throttles together wherever it appears.
inline std::string request_key(const RequestPtr &req) {
    if (const auto *client = req->get_state<ClientIp>()) {
        return client->value;
    }
    return req->peer_address();
}

} // namespace detail

// Global rate limit: shared `bucket` — requests beyond the bucket's rate and
// burst are answered 429 with Retry-After and never reach the rest of the
// chain.
inline Middleware rate_limit(std::shared_ptr<TokenBucket> bucket) {
    return [bucket = std::move(bucket)](RequestPtr req, ResponsePtr res, SslHandle ssl,
                                        Next next) -> asio::awaitable<void> {
        if (!bucket->try_acquire()) {
            res->header(field::retry_after, std::to_string(bucket->seconds_until_token()));
            co_await res->status(status::too_many_requests).send("too many requests");
            co_return; // answered here
        }
        co_await next(std::move(req), std::move(res), ssl);
    };
}

// Per-key rate limit: `limiter` keeps one bucket per key; `key_from` picks the
// key (default: the client address, ClientIp-aware). Requests beyond a key's
// budget are answered 429 and never reach the rest of the chain.
inline Middleware rate_limit(std::shared_ptr<RateLimiter> limiter,
                             std::function<std::string(const RequestPtr &)> key_from = detail::request_key) {
    return [limiter = std::move(limiter), key_from = std::move(key_from)](
               RequestPtr req, ResponsePtr res, SslHandle ssl, Next next) -> asio::awaitable<void> {
        if (!limiter->try_acquire(key_from(req))) {
            co_await res->status(status::too_many_requests).send("too many requests");
            co_return; // answered here — a per-key budget has no shared Retry-After
        }
        co_await next(std::move(req), std::move(res), ssl);
    };
}

} // namespace middleware
} // namespace simple_http