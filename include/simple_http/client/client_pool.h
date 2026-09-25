#pragma once

// Idle keep-alive pool for client sessions.
//
// One pool belongs to one HttpClient. That ownership is the point: a session's
// identity includes the TLS settings its connection was established under
// (verification, CA, client certificate, SNI), and those live in ClientConfig,
// not in the target — so a pool shared between clients could hand a connection
// authenticated one way to a client configured another way. Per-client pooling
// removes that class of bug instead of trying to hash it into the key.
//
// Keys still carry what varies *within* a client: the executor the session is
// bound to (an asio requirement, and the reason a session is only ever taken
// from the executor that created it), the origin, the version policy it was
// negotiated under, and an optional caller-supplied tag.
//
// Ownership is a strong reference, because a live HTTP/2 session keeps its read
// loop running by holding itself: nothing else would ever close an idle one. So
// the pool also arms the session's idle-close timer when it puts it back (and
// cancels it when the session is taken out), which bounds how long an unused
// connection can hold a socket.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "client_config.h"
#include "client_stream.h"

namespace simple_http {

struct PoolKey {
    const void* executor{nullptr};  // executor identity (its io_context)
    std::string origin;             // "https://host:port"
    int version_policy{0};          // HttpVersionPolicy, as negotiated
    std::string tag;                // ClientTarget::pool_tag

    bool operator==(const PoolKey& other) const {
        return executor == other.executor && version_policy == other.version_policy && tag == other.tag &&
               origin == other.origin;
    }
};

struct PoolKeyHash {
    std::size_t operator()(const PoolKey& k) const {
        std::size_t h = std::hash<const void*>{}(k.executor);
        h = h * 31u + std::hash<std::string>{}(k.origin);
        h = h * 31u + static_cast<std::size_t>(k.version_policy);
        h = h * 31u + std::hash<std::string>{}(k.tag);
        return h;
    }
};

class ClientPool {
  public:
    ClientPool(std::size_t max_idle_per_key, std::chrono::milliseconds idle_ttl)
        : m_max_idle_per_key(std::max<std::size_t>(1, max_idle_per_key)), m_idle_ttl(idle_ttl) {
    }

    ~ClientPool() {
        clear();
    }

    ClientPool(const ClientPool&) = delete;
    ClientPool& operator=(const ClientPool&) = delete;

    // Takes an idle session for `key`, or nullptr. Sessions that are no longer
    // alive, no longer reusable, or older than the idle TTL are dropped on the
    // way past rather than handed out.
    std::shared_ptr<ClientSession> take(const PoolKey& key) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_idle.find(key);
        if (it == m_idle.end())
            return nullptr;
        auto& entries = it->second;
        const auto now = std::chrono::steady_clock::now();
        while (!entries.empty()) {
            Entry entry = std::move(entries.back());
            entries.pop_back();
            if (now - entry.since > m_idle_ttl) {
                entry.session->close();  // too old to trust: the peer may have closed it
                continue;
            }
            if (!entry.session->alive() || !entry.session->reusable()) {
                entry.session->close();
                continue;
            }
            entry.session->disarm_idle_close();
            ++m_reused;
            return entry.session;
        }
        return nullptr;
    }

    // Returns a session to the pool. A session that is not reusable is closed
    // instead of stored: pooling it would only hand the next request a
    // connection in an unknown position.
    void put(const PoolKey& key, std::shared_ptr<ClientSession> session) {
        if (!session)
            return;
        if (!session->alive() || !session->reusable()) {
            session->close();
            return;
        }
        session->arm_idle_close(m_idle_ttl);
        std::lock_guard<std::mutex> lock(m_mutex);
        auto& entries = m_idle[key];
        entries.push_back(Entry{std::move(session), std::chrono::steady_clock::now()});
        while (entries.size() > m_max_idle_per_key) {
            entries.front().session->close();
            entries.erase(entries.begin());
        }
    }

    // Closes and forgets every idle session (the caller is shutting down).
    void clear() {
        std::unordered_map<PoolKey, std::vector<Entry>, PoolKeyHash> drained;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            drained.swap(m_idle);
        }
        for (auto& [key, entries] : drained) {
            for (auto& entry : entries)
                entry.session->close();
        }
    }

    std::size_t idle_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::size_t n = 0;
        for (const auto& [key, entries] : m_idle)
            n += entries.size();
        return n;
    }

    // Sessions handed out of the pool so far (diagnostics / tests).
    std::size_t reused_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_reused;
    }

  private:
    struct Entry {
        std::shared_ptr<ClientSession> session;
        std::chrono::steady_clock::time_point since;
    };

    mutable std::mutex m_mutex;
    std::unordered_map<PoolKey, std::vector<Entry>, PoolKeyHash> m_idle;
    std::size_t m_max_idle_per_key;
    std::chrono::milliseconds m_idle_ttl;
    std::size_t m_reused{0};
};

}  // namespace simple_http
