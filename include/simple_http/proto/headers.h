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

    // Case-insensitive lookup of the first matching field.
    std::optional<std::string_view> get(std::string_view name) const {
        std::string lowered{name};
        to_lower(lowered);
        auto it = std::find_if(m_fields.begin(), m_fields.end(),
                               [&](const value_type& f) { return f.first == lowered; });
        if (it == m_fields.end()) {
            return std::nullopt;
        }
        return std::string_view{it->second};
    }

    bool contains(std::string_view name) const { return get(name).has_value(); }

    void clear() { m_fields.clear(); }
    bool empty() const { return m_fields.empty(); }
    std::size_t size() const { return m_fields.size(); }

    auto begin() const { return m_fields.begin(); }
    auto end() const { return m_fields.end(); }

    const std::vector<value_type>& fields() const { return m_fields; }

  private:
    static void to_lower(std::string& s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    }

    std::vector<value_type> m_fields;
};

}  // namespace simple_http
