#pragma once

// Headers: an ordered list of HTTP header field name/value pairs with
// case-insensitive lookup. Field names are stored lowercased.

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../core/http_date.h"  // ensure_date
#include "../core/http_field.h" // trim_ows / value_has_token
#include "../core/types.h"      // ascii_lower / iequals_ci: used below, and this header must stand alone

namespace simple_http {

class Headers {
  public:
    using value_type = std::pair<std::string, std::string>;

    Headers() = default;

    // Reserve room for fields without changing the contents. The response
    // writer uses this so Date/Content-Type/Server do not each grow the vector
    // (and move every string) on the hot path.
    void reserve(std::size_t count) { m_fields.reserve(count); }

    // Adds a field. The name is lowercased for consistent lookup.
    void add(std::string name, std::string value) {
        to_lower(name);
        m_fields.emplace_back(std::move(name), std::move(value));
    }

    // Adds a field, assuming the name is already lowercased (fast path).
    void add_lower(std::string lower_name, std::string value) {
        m_fields.emplace_back(std::move(lower_name), std::move(value));
    }

    // Case-insensitive lookup of the first matching field. Field names are
    // stored lowercased, so this compares `name` case-insensitively without
    // allocating a temporary lowercased copy (called on the request hot path).
    std::optional<std::string_view> get(std::string_view name) const {
        auto it = std::find_if(m_fields.begin(), m_fields.end(),
                               [&](const value_type &f) { return iequals_ascii(f.first, name); });
        if (it == m_fields.end()) {
            return std::nullopt;
        }
        return std::string_view{it->second};
    }

    bool contains(std::string_view name) const { return get(name).has_value(); }

    // Every value for `name`, in order of appearance — Go's r.Header.Values
    // (multiple X-Forwarded-For hops, repeated Set-Cookie, duplicate Host in a
    // smuggling test). The views point into this Headers' own storage and stay
    // valid while it does.
    std::vector<std::string_view> get_all(std::string_view name) const {
        std::vector<std::string_view> out;
        for (const auto &f : m_fields) {
            if (iequals_ascii(f.first, name)) {
                out.emplace_back(f.second);
            }
        }
        return out;
    }

    // How many fields carry `name` (case-insensitively). The request side needs
    // this for Host, where the rule is "exactly one" (RFC 9112 §3.2) and not "at
    // least one" — a second copy is a smuggling vector, not a repeatable field.
    std::size_t count(std::string_view name) const {
        return static_cast<std::size_t>(std::count_if(
            m_fields.begin(), m_fields.end(), [&](const value_type &f) { return iequals_ascii(f.first, name); }));
    }

    // Removes every field matching `name` (case-insensitively), returning true
    // if anything was removed. All matches go, not just the first: a duplicate
    // Content-Length is a request-smuggling vector on the request side and
    // ambiguous on the response side.
    bool erase(std::string_view name) {
        auto it = std::remove_if(m_fields.begin(), m_fields.end(),
                                 [&](const value_type &f) { return iequals_ascii(f.first, name); });
        if (it == m_fields.end()) {
            return false;
        }
        m_fields.erase(it, m_fields.end());
        return true;
    }

    void clear() { m_fields.clear(); }
    bool empty() const { return m_fields.empty(); }
    std::size_t size() const { return m_fields.size(); }

    auto begin() const { return m_fields.begin(); }
    auto end() const { return m_fields.end(); }

    const std::vector<value_type> &fields() const { return m_fields; }

  private:
    // Allocation-free ASCII case-insensitive equality, lowercasing only `rhs`.
    //
    // That is sound *here* and nowhere else: `lhs` is always a stored field name,
    // and this class stores them lowercased (to_lower below, and add()). It is
    // private for exactly that reason — it used to be public and static, so a
    // caller comparing two names, or passing a mixed-case `lhs`, got a silent
    // mismatch instead of a compile error. The symmetric version, for anyone who
    // needs one, is core/types.h's iequals_ci.
    static bool iequals_ascii(std::string_view lhs, std::string_view rhs) {
        if (lhs.size() != rhs.size()) {
            return false;
        }
        for (std::size_t i = 0; i < lhs.size(); ++i) {
            if (lhs[i] != ascii_lower(rhs[i])) {
                return false;
            }
        }
        return true;
    }

    static void to_lower(std::string &s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return ascii_lower(static_cast<char>(c)); });
    }

    std::vector<value_type> m_fields;
};

// Whether any occurrence of field `name` contains `token` in its
// comma-separated value list (case-insensitive, OWS-trimmed). `Connection` and
// `Upgrade` are token lists, not opaque strings, and both may legally repeat.
inline bool header_has_token(const Headers &headers, std::string_view name, std::string_view token) {
    for (auto value : headers.get_all(name)) {
        if (value_has_token(value, token))
            return true;
    }
    return false;
}

// Every value nominated by the Connection field(s), lowercased. Those fields
// are hop-by-hop for this connection (RFC 9110 §7.6.1); reading only the first
// Connection occurrence would leave a second field's nominated headers
// attached to a proxied message.
inline std::vector<std::string> connection_tokens(const Headers &headers) {
    std::vector<std::string> tokens;
    for (auto conn : headers.get_all("connection")) {
        std::size_t pos = 0;
        while (pos < conn.size()) {
            const std::size_t comma = conn.find(',', pos);
            const std::string_view item =
                trim_ows(conn.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos));
            if (!item.empty())
                tokens.push_back(ascii_lower_copy(item));
            if (comma == std::string_view::npos)
                break;
            pos = comma + 1;
        }
    }
    return tokens;
}

// Adds a Date field unless the handler already supplied one. Every response
// path in every engine needs this, so the rule lives in one place.
inline void ensure_date(Headers &headers) {
    if (!headers.contains("date"))
        headers.add_lower("date", std::string{http_date_now()});
}

} // namespace simple_http
