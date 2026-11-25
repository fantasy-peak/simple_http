#pragma once

// Per-Client cache of TLS 1.3 sessions for HTTP/3 resumption and 0-RTT.
//
// A session ticket and the server's 0-RTT transport parameters must travel
// together: ngtcp2 refuses to open an early stream without the saved parameters,
// and OpenSSL needs the session to build the early-data keys. Entries are
// single-use (take() removes them), matching the usual TLS ticket lifetime
// model; a failed dial simply loses the entry rather than replaying a ticket
// whose server state may have moved on.
//
// The Client is single-threaded, but the OpenSSL new-session callback and any
// future shared cache make the mutex worth keeping. It mirrors CookieJar.

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <openssl/ssl.h>

namespace simple_http::detail {

struct H3SessionEntry {
    std::shared_ptr<SSL_SESSION> session;
    std::vector<std::uint8_t> transport_params;
    std::chrono::steady_clock::time_point expires{};
};

class H3SessionCache {
  public:
    void put(std::string key, H3SessionEntry entry) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.insert_or_assign(std::move(key), std::move(entry));
    }

    // Removes and returns the entry. Expired or absent entries yield nullopt.
    std::optional<H3SessionEntry> take(const std::string &key) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_entries.find(key);
        if (it == m_entries.end())
            return std::nullopt;
        H3SessionEntry entry = std::move(it->second);
        m_entries.erase(it);
        if (entry.expires <= std::chrono::steady_clock::now())
            return std::nullopt;
        return entry;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_entries.size();
    }

  private:
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, H3SessionEntry> m_entries;
};

} // namespace simple_http::detail
