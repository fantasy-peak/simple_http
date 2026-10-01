#pragma once

// Typed request-parameter extraction — the axum `Path<T>` / `Query<T>`
// extractors (and Go 1.22's typed `r.PathValue`): a struct's field names are
// matched against the route template's `{name}`s (path) or the query-string
// keys (query), and each value is parsed into its field's type by glaze
// reflection:
//
//   struct Params { std::string id; std::int64_t page; };
//   server.route({Method::Get}, "/orders/{id}?page={page}"… // actually path:
//   server.route({Method::Get}, "/orders/{id}", handler);       // template
//   const auto p = simple_http::path_params<Params>(*req);      // captures
//   const auto q = simple_http::query_params<Params>(*req);     // ?page=…
//
// A missing or unparsable value yields nullopt — the handler decides the 404
// /400 — matching openapi::path_params (which now aliases here). Values are
// read as JSON scalars, so numeric and boolean fields parse exactly like their
// JSON spellings; std::string fields take the raw text.

#include <glaze/glaze.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include "query.h"
#include "request.h"

namespace simple_http {

namespace params_detail {

// Fills `out` field by field from `get(name)`: glaze reflection names the
// fields, the getter supplies the values. Any missing or unparsable field
// marks the parse incomplete (nullopt upstream).
template <typename T, typename Getter> bool params_from(T &out, Getter &&get) {
    bool complete = true;
    std::size_t i = 0;
    glz::for_each_field(out, [&](auto &field) {
        const std::string_view name = std::string_view{glz::reflect<T>::keys[i++]};
        const auto value = get(name);
        if (!value) {
            complete = false;
            return;
        }
        using Field = std::remove_cvref_t<decltype(field)>;
        if constexpr (std::is_same_v<Field, std::string>) {
            field = std::string{*value}; // a segment / query value is a bare string
        } else {
            // Numbers and booleans arrive bare on the wire, which is valid JSON
            // scalar input; any read error marks the parse incomplete.
            if (glz::read_json(field, *value)) {
                complete = false;
            }
        }
    });
    return complete;
}

} // namespace params_detail

// The captures of a template route, parsed into T by field name (the axum
// `Path<T>` contract: field name == `{name}`).
template <typename T> std::optional<T> path_params(const Request &req) {
    T out{};
    if (!params_detail::params_from<T>(out, [&](std::string_view name) { return req.param(name); })) {
        return std::nullopt;
    }
    return out;
}

// The URL query, parsed into T by field name (the axum `Query<T>` contract:
// field name == query key). Also the shape for an application/x-www-form-
// urlencoded body: read it with read_urlencoded_body, then parse_params<T>.
template <typename T> std::optional<T> query_params(const Request &req) {
    T out{};
    if (!params_detail::params_from<T>(out, [&](std::string_view name) { return req.query_params().get(name); })) {
        return std::nullopt;
    }
    return out;
}

// Parses any QueryParams into T — the shared engine behind query_params, so a
// parsed urlencoded form body parses the same way. The returned T owns its
// strings; `q` only needs to outlive the call.
template <typename T> std::optional<T> parse_params(const QueryParams &q) {
    T out{};
    if (!params_detail::params_from<T>(out, [&](std::string_view name) { return q.get(name); })) {
        return std::nullopt;
    }
    return out;
}

} // namespace simple_http