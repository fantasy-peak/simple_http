#pragma once

// Server-side sessions plus a double-submit CSRF companion (csrf.h).
//
// The session middleware loads a session id from a cookie, asks a SessionStore
// for its data, publishes a Session handle on the request, and persists it
// after the handler completes. The default MemorySessionStore is in-process and
// lock-protected; a Redis/database store implements the same three awaitable
// methods on the consumer side, so the library takes on no storage dependency.
//
//     auto sessions = std::make_shared<MemorySessionStore>();
//     server.use(simple_http::middleware::session(sessions, {
//         .cookie_name = "__Host-sid",
//         .secure = true,
//         .http_only = true,
//         .same_site = "Lax",
//     }));
//
//     server.route({Method::Post}, "/login",
//         [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
//             if (auto *s = simple_http::session(*req)) {
//                 s->regenerate_id();          // session-fixation defense
//                 s->set("uid", "42");
//             }
//             co_await res->status(status::no_content).send("");
//         });
//
// The cookie carries only an opaque id; values live in the store and are never
// trusted from the client. The id is generated with RAND_bytes (OpenSSL) and is
// base64url-encoded without padding.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <openssl/rand.h>

#include "../core/base64.h"
#include "../core/http_date.h"
#include "../core/logging.h"
#include "handler.h"

namespace simple_http {

// A cryptographically random opaque token, base64url-encoded without padding.
// Used for session ids and CSRF tokens; RAND_bytes is the primary source and a
// random_device fallback keeps a broken RNG from turning into a predictable id.
inline std::string random_token(std::size_t bytes = 32) {
    std::string raw(bytes, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char *>(raw.data()), static_cast<int>(bytes)) != 1) {
        std::random_device rd;
        for (char &c : raw) {
            c = static_cast<char>(rd() & 0xFF);
        }
    }
    return base64_url_encode(raw);
}

// Constant-time equality for cookie/header tokens (and anything else where a
// timing side channel would leak the expected value).
inline bool constant_time_equal(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}

// The data a SessionStore persists. Kept deliberately plain (string keys and
// values) so a store can serialize it however it likes.
struct SessionData {
    std::map<std::string, std::string, std::less<>> values;
    std::int64_t expires_unix{0};
};

// The storage seam. Implementations may be asynchronous (a database/Redis
// store); the middleware runs on the connection executor and awaits them.
class SessionStore {
  public:
    virtual ~SessionStore() = default;
    virtual asio::awaitable<std::optional<SessionData>> load(std::string_view id) = 0;
    virtual asio::awaitable<void> save(std::string_view id, const SessionData &data) = 0;
    virtual asio::awaitable<void> destroy(std::string_view id) = 0;
};

// In-process store. All operations are synchronous internally but expose the
// awaitable interface; the mutex is the deliberate handler-layer exception to
// model A, because one store is shared by every connection.
class MemorySessionStore final : public SessionStore {
  public:
    explicit MemorySessionStore(std::size_t max_sessions = 100000) : m_max_sessions(max_sessions) {}

    asio::awaitable<std::optional<SessionData>> load(std::string_view id) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_sessions.find(std::string{id});
        if (it == m_sessions.end()) {
            co_return std::nullopt;
        }
        if (it->second.expires_unix != 0 && it->second.expires_unix <= now_unix()) {
            m_sessions.erase(it);
            co_return std::nullopt;
        }
        co_return it->second;
    }

    asio::awaitable<void> save(std::string_view id, const SessionData &data) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        evict_expired();
        if (m_max_sessions != 0 && m_sessions.size() >= m_max_sessions) {
            // Bounded memory: drop the oldest entry we can find. Session stores
            // are not a cache; this is a safety valve against unbounded growth,
            // not an LRU policy.
            m_sessions.erase(m_sessions.begin());
        }
        m_sessions[std::string{id}] = data;
        co_return;
    }

    asio::awaitable<void> destroy(std::string_view id) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sessions.erase(std::string{id});
        co_return;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_sessions.size();
    }

  private:
    void evict_expired() {
        const std::int64_t now = now_unix();
        for (auto it = m_sessions.begin(); it != m_sessions.end();) {
            if (it->second.expires_unix != 0 && it->second.expires_unix <= now) {
                it = m_sessions.erase(it);
            } else {
                ++it;
            }
        }
    }

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, SessionData> m_sessions;
    std::size_t m_max_sessions{100000};
};

struct SessionConfig {
    std::string cookie_name{"sid"};
    std::string path{"/"};
    std::string same_site{"Lax"};
    bool secure{true};    // set false for plaintext development only
    bool http_only{true}; // the session id is never read by JavaScript
    bool rolling{true};   // refresh the cookie Max-Age on each request
    std::chrono::seconds max_age{86400};
};

// The handle a handler/middleware receives for the current request. It owns the
// loaded values and the dirty flag; the middleware persists it after next().
class Session {
  public:
    Session(std::string id, SessionData data, std::function<void(std::string_view)> set_cookie)
        : m_id(std::move(id)), m_data(std::move(data)), m_set_cookie(std::move(set_cookie)) {}

    [[nodiscard]] std::string_view id() const noexcept { return m_id; }
    [[nodiscard]] bool contains(std::string_view key) const { return m_data.values.contains(key); }

    [[nodiscard]] std::optional<std::string_view> get(std::string_view key) const {
        const auto it = m_data.values.find(key);
        if (it == m_data.values.end()) {
            return std::nullopt;
        }
        return std::string_view{it->second};
    }

    void set(std::string key, std::string value) {
        m_data.values[std::move(key)] = std::move(value);
        m_dirty = true;
    }

    void erase(std::string_view key) {
        if (const auto it = m_data.values.find(key); it != m_data.values.end()) {
            m_data.values.erase(it);
            m_dirty = true;
        }
    }

    void clear() {
        if (!m_data.values.empty()) {
            m_data.values.clear();
            m_dirty = true;
        }
    }

    // Rotate the opaque id (call after a successful login) and write the new
    // cookie immediately. Must be called before the response head is sent.
    void regenerate_id() {
        m_id = random_token(32);
        m_dirty = true;
        if (m_set_cookie) {
            m_set_cookie(m_id);
        }
    }

    [[nodiscard]] bool dirty() const noexcept { return m_dirty; }
    [[nodiscard]] const SessionData &data() const noexcept { return m_data; }

  private:
    std::string m_id;
    SessionData m_data;
    bool m_dirty{false};
    std::function<void(std::string_view)> m_set_cookie;
};

// The current request's session, or nullptr when the middleware is not mounted.
inline Session *session(Request &req) {
    auto *state = req.get_state<std::shared_ptr<Session>>();
    return state && *state ? state->get() : nullptr;
}
inline const Session *session(const Request &req) {
    const auto *state = req.get_state<std::shared_ptr<Session>>();
    return state && *state ? state->get() : nullptr;
}

namespace middleware {

// Loads/creates a session, publishes it on the request, and persists changes
// after the chain. The cookie is set before next() so it rides on the response
// head; session data is saved afterwards (a failed save is logged, not turned
// into a response after the head has gone out).
inline Middleware session(std::shared_ptr<SessionStore> store, SessionConfig config = {}) {
    return [store = std::move(store), config = std::move(config)](RequestPtr req, ResponsePtr res, SslHandle ssl,
                                                                  Next next) -> asio::awaitable<void> {
        if (!store) {
            co_await next(std::move(req), std::move(res), ssl);
            co_return;
        }

        std::string id;
        SessionData data;
        bool fresh = false;
        if (const auto cookie = req->cookie(config.cookie_name); cookie && !cookie->empty()) {
            auto loaded = co_await store->load(*cookie);
            if (loaded) {
                id = std::string{*cookie};
                data = std::move(*loaded);
            }
        }
        if (id.empty()) {
            id = random_token(32);
            fresh = true;
        }

        auto set_cookie = [res, config](std::string_view value) {
            res->set_cookie(config.cookie_name, std::string{value}, config.max_age, config.path, "", config.secure,
                            config.http_only, config.same_site);
        };
        const std::string original_id = id;
        auto current = std::make_shared<Session>(id, std::move(data), set_cookie);
        req->set_state(current);
        if (fresh || config.rolling) {
            set_cookie(current->id());
        }

        co_await next(std::move(req), std::move(res), ssl);

        // Session fixation defense: if the handler rotated the id (typically on
        // login), the old id must stop working immediately.
        if (!original_id.empty() && current->id() != original_id) {
            co_await store->destroy(original_id);
        }
        if (current->dirty()) {
            SessionData out = current->data();
            // max_age == 0 means a browser-session cookie; keep the server-side
            // entry until the process/store decides otherwise rather than
            // expiring it immediately.
            out.expires_unix = config.max_age.count() > 0 ? now_unix() + config.max_age.count() : 0;
            co_await store->save(current->id(), out);
        }
        co_return;
    };
}

} // namespace middleware
} // namespace simple_http
