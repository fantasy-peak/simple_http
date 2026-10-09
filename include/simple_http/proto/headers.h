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

#include "../core/types.h" // ascii_lower / iequals_ci: used below, and this header must stand alone

namespace simple_http {

class Headers {
  public:
    using value_type = std::pair<std::string, std::string>;

    Headers() = default;

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
    // needs one, is core/content_encoding.h's iequals_ci.
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

} // namespace simple_http
