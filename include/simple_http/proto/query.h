#pragma once

// Query-string and application/x-www-form-urlencoded parsing (RFC 3986 §3.4
// query form, the `key=value&key=value...` payload of a form POST): '+' decodes
// to space, '%XX' percent-decodes a byte. This is the Go `r.URL.Query()` /
// axum `Query<T>` / `Form<T>` base — a parsed view of a request's `?query` or
// of a form body:
//
//   const QueryParams &q = req->query_params();      // the URL query, decoded
//   if (auto name = q.get("name")) { /* use *name */ }
//   for (std::string_view v : q.get_all("tag")) { /* repeated names */ }
//
//   // a urlencoded request body (Content-Type: application/x-www-form-urlencoded)
//   auto body = co_await req->body().read_all();
//   auto form = QueryParams::parse(*body);

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace simple_http {

// Parsed parameters, kept in appearance order. Values are decoded and owned; a
// getter's string_view points into this object, so the QueryParams must outlive
// the view — it does: on the Request for the URL query, or in the caller's
// frame for a parsed form body.
class QueryParams {
  public:
    // Parses `raw` ("a=1&b=2&a=3&flag"). An empty string is no pairs. A segment
    // with no '=' is a flag with an empty value. Undecodable escapes and stray
    // bytes are kept verbatim rather than refused: a query is advisory data,
    // and one malformed escape should not turn a request into a 400.
    static QueryParams parse(std::string_view raw) {
        QueryParams out;
        std::size_t pos = 0;
        for (;;) {
            const std::size_t amp = raw.find('&', pos);
            const std::size_t end = amp == std::string_view::npos ? raw.size() : amp;
            if (end > pos) {
                const std::size_t eq = raw.find('=', pos);
                if (eq == std::string_view::npos || eq > end) {
                    out.m_items.emplace_back(decode(raw.substr(pos, end - pos)), std::string{});
                } else {
                    out.m_items.emplace_back(decode(raw.substr(pos, eq - pos)),
                                             decode(raw.substr(eq + 1, end - eq - 1)));
                }
            }
            if (amp == std::string_view::npos) {
                break;
            }
            pos = amp + 1;
        }
        return out;
    }

    // The first value for `name`, or nullopt. Name comparison is exact.
    std::optional<std::string_view> get(std::string_view name) const {
        for (const auto &[key, value] : m_items) {
            if (key == name) {
                return value;
            }
        }
        return std::nullopt;
    }
    bool contains(std::string_view name) const { return get(name).has_value(); }

    // Every value for a repeated name, in order of appearance.
    std::vector<std::string_view> get_all(std::string_view name) const {
        std::vector<std::string_view> out;
        for (const auto &[key, value] : m_items) {
            if (key == name) {
                out.push_back(value);
            }
        }
        return out;
    }

    bool empty() const { return m_items.empty(); }

    // Everything, in appearance order: first = key, second = value.
    using Item = std::pair<std::string, std::string>;
    const std::vector<Item> &items() const { return m_items; }

  private:
    // '+' is a space under form encoding; '%XX' decodes a byte. A '%' that is
    // not a valid escape is kept verbatim (advisory data, not framing).
    static std::string decode(std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (std::size_t i = 0; i < s.size(); ++i) {
            const char c = s[i];
            if (c == '+') {
                out.push_back(' ');
                continue;
            }
            if (c == '%' && i + 2 < s.size()) {
                const int hi = hex(s[i + 1]);
                const int lo = hex(s[i + 2]);
                if (hi >= 0 && lo >= 0) {
                    out.push_back(static_cast<char>((hi << 4) | lo));
                    i += 2;
                    continue;
                }
            }
            out.push_back(c);
        }
        return out;
    }

    static int hex(char c) noexcept {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    }

    std::vector<Item> m_items;
};

} // namespace simple_http