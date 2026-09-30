#pragma once

// OpenAPI integration glue: JSON Schemas from C++ types via glaze's
// compile-time reflection. This is what makes `route<Req, Res>` automatic — an
// aggregate struct is reflected with no metadata and `schema_json<T>()` returns
// its JSON Schema, which router.h records into the document model
// (openapi_doc.h).
//
// Opt-in: gated by SIMPLE_HTTP_ENABLE_OPENAPI, and glaze must be on the include
// path (it is a header-only library). The library target itself does not
// depend on glaze; an OpenAPI-enabled consumer adds it.

#include <algorithm>
#include <boost/asio/awaitable.hpp>
#include <glaze/glaze.hpp>
#include <glaze/json/schema.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "../proto/request.h" // Request::param / Request::body for path_params / read_body
#include "openapi_doc.h"      // OpenApiSpec / OperationInfo, the model schema_json feeds

namespace simple_http {
namespace openapi {

// --- swagger-safe schema generation
// ------------------------------------------- glaze's write_json_schema emits
// JSON Schema 2020-12 with a `$defs` block and
// `#/$defs/...` references. Spliced into an OpenAPI document those references
// resolve against the document root, where no `$defs` exists — swagger-ui's
// resolver fails loudly on every `$ref`. So schemas are generated here by hand
// from glaze's reflection (field names, order and types): inline, ref-free,
// a narrow JSON Schema subset every Swagger UI can render. glaze still drives
// it — the types themselves need no metadata.

namespace schema_detail {
template <typename T> struct is_optional : std::false_type {};
template <typename U> struct is_optional<std::optional<U>> : std::true_type {};
template <typename T> struct is_vector : std::false_type {};
template <typename U, typename A> struct is_vector<std::vector<U, A>> : std::true_type {};

// A field's glaze_json_schema decoration: the annotation struct may name a
// subset of the data fields, so the match is by member name, not by index.
// Emits description and enum list — enough to make a field readable and to
// give swagger-ui a dropdown.
template <typename T> void render_field_annotations(JsonOut &j, std::string_view field_name) {
    if constexpr (requires { typename T::glaze_json_schema; }) {
        using Ann = typename T::glaze_json_schema;
        Ann ann{};
        std::size_t i = 0;
        glz::for_each_field(ann, [&](auto &a) {
            if (std::string_view{glz::reflect<Ann>::keys[i]} == field_name) {
                if constexpr (requires { a.description; }) {
                    if (a.description.has_value()) {
                        j.key("description");
                        j.str(a.description.value());
                    }
                }
                if constexpr (requires { a.enumeration; }) {
                    if (a.enumeration.ptr) {
                        j.key("enum");
                        j.begin_array();
                        for (const auto e : *a.enumeration.ptr) {
                            j.str(e);
                        }
                        j.end_array();
                    }
                }
            }
            ++i;
        });
    }
}

// Writes the JSON Schema *content* of an already-open object for type T: the
// "type"/"anyOf"/"items"/"properties" keys, never the enclosing braces.
// Nothing here relies on glaze's schema output — only its reflection.
template <typename T> void emit_type(JsonOut &j) {
    using D = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<D, bool>) {
        j.key("type");
        j.str("boolean");
    } else if constexpr (std::is_integral_v<D>) {
        j.key("type");
        j.str("integer");
    } else if constexpr (std::is_floating_point_v<D>) {
        j.key("type");
        j.str("number");
    } else if constexpr (std::is_same_v<D, std::string>) {
        j.key("type");
        j.str("string");
    } else if constexpr (is_optional<D>::value) {
        // optional: the value or null — swagger renders this fine.
        j.key("anyOf");
        j.begin_array();
        j.begin_object();
        emit_type<typename D::value_type>(j);
        j.end_object();
        j.begin_object();
        j.key("type");
        j.str("null");
        j.end_object();
        j.end_array();
    } else if constexpr (is_vector<D>::value) {
        j.key("type");
        j.str("array");
        j.key("items");
        j.begin_object();
        emit_type<typename D::value_type>(j);
        j.end_object();
    } else {
        // An aggregate: object with one property per reflected field.
        j.key("type");
        j.str("object");
        j.key("properties");
        j.begin_object();
        T dummy{};
        std::size_t field_index = 0;
        glz::for_each_field(dummy, [&](auto &field) {
            using F = std::remove_cvref_t<decltype(field)>;
            j.key(std::string_view{glz::reflect<T>::keys[field_index]});
            j.begin_object();
            emit_type<F>(j);
            render_field_annotations<T>(j, std::string_view{glz::reflect<T>::keys[field_index]});
            j.end_object();
            ++field_index;
        });
        j.end_object();
    }
}
} // namespace schema_detail

// The inline, ref-free JSON Schema for a serializable type as a raw JSON
// string.
template <typename T> std::string schema_json() {
    JsonOut j;
    j.begin_object();
    schema_detail::emit_type<T>(j);
    j.end_object();
    return std::move(j).take();
}

// --- request-body validation (400 / 422)
// -------------------------------------- The outcome of validating a request
// body against its declared type: the status an API should answer — 400 when
// the body is unreadable, malformed, or type-mismatched; 422 when it is valid
// JSON but violates the declared required fields — and a message the handler
// can put in its error body. The response shape (which JSON / which headers)
// stays the handler's call.
struct BodyError {
    int status{400};
    std::string message;
};

// Reads and validates the request body against the declared Req type, the
// runtime half of the document's requestBody schema — the utoipa/axum claim
// "the server rejects what the document does not allow". Returns nullopt (and
// fills `err`) so the handler answers; the handler itself decides the shape.
//
//     openapi::BodyError err;
//     auto in = co_await openapi::read_body<CreateUserReq>(*req, err);
//     if (!in) { co_await res->status(err.status).json(...); co_return; }
template <typename TReq> boost::asio::awaitable<std::optional<TReq>> read_body(Request &req, BodyError &err) {
    auto raw = co_await req.body().read_all();
    if (!raw) {
        err = {400, "request body unreadable"};
        co_return std::nullopt;
    }
    // Strict pass: required (non-nullable) fields must be present.
    TReq out{};
    if (auto ec = glz::read<glz::opts{.error_on_missing_keys = true}>(out, *raw); !ec) {
        co_return out;
    }
    // Distinguish "valid JSON, missing required field" (422) from "does not
    // parse or type-matches at all" (400) by a lenient second read.
    TReq lenient{};
    if (auto lax = glz::read<glz::opts{}>(lenient, *raw); !lax) {
        err = {422, "request body violates the declared required fields"};
    } else {
        err = {400, "request body is malformed for the declared type"};
    }
    co_return std::nullopt;
}

// Marker: "no request body". Used as the second template argument of a
// Params-typed route that takes no body — the axum `Path(params)` handler:
//     server.route<OrderItemParams, openapi::NoBody, Order>(
//         {Method::Get}, "/orders/{order_id}/items/{item_id}", handler);
struct NoBody {};

// The axum `Path<Params>` extractor, kept out of the handler signature: the
// captures of a template route are deserialized into `Params` by field name
// (glaze reflection), exactly the same "field name == {name}" contract. A
// missing or unparsable capture yields nullopt, and the handler decides (404).
template <typename T> std::optional<T> path_params(const Request &req) {
    T out{};
    bool complete = true;
    std::size_t i = 0;
    glz::for_each_field(out, [&](auto &field) {
        const std::string_view name = std::string_view{glz::reflect<T>::keys[i++]};
        const auto value = req.param(name);
        if (!value) {
            complete = false;
            return;
        }
        using Field = std::remove_cvref_t<decltype(field)>;
        if constexpr (std::is_same_v<Field, std::string>) {
            field = *value; // a path segment is already a bare string
        } else {
            // Numbers and booleans arrive bare on the wire, which is valid
            // JSON scalar input; any read error marks the parse incomplete.
            if (glz::read_json(field, *value)) {
                complete = false;
            }
        }
    });
    if (!complete) {
        return std::nullopt;
    }
    return out;
}

// The `Params` field names, from glaze reflection — the set a route template's
// `{name}` segments must equal at registration.
template <typename T> std::vector<std::string> param_field_names() {
    std::vector<std::string> out;
    T dummy{};
    std::size_t i = 0;
    glz::for_each_field(dummy, [&](auto &) { out.emplace_back(glz::reflect<T>::keys[i++]); });
    return out;
}

// Whether a template's `{name}` set equals `Params`'s field-name set (both
// compared as sorted sets, order irrelevant — matching is by name).
template <typename T> bool params_match_template(const std::vector<std::string> &names) {
    auto fields = param_field_names<T>();
    if (fields.size() != names.size()) {
        return false;
    }
    std::sort(fields.begin(), fields.end());
    auto template_names = names;
    std::sort(template_names.begin(), template_names.end());
    return fields == template_names;
}

// The path-parameter declarations for a `Params`-typed route: one per field,
// name + the field's own schema + required, in field order. utoipa/axum derive
// path parameters from the struct; this is that derivation.
template <typename T> std::vector<Param> path_params_schema() {
    std::vector<Param> out;
    T dummy{};
    std::size_t i = 0;
    glz::for_each_field(dummy, [&](auto &field) {
        const std::string_view name = std::string_view{glz::reflect<T>::keys[i++]};
        using Field = std::remove_cvref_t<decltype(field)>;
        out.push_back(Param{std::string{name}, ParamIn::Path, true, {}, schema_json<Field>()});
    });
    return out;
}

// A response-header declaration (OAS `responses.<code>.headers`), with the
// value type's inline schema:
//     openapi::Response rl = openapi::resp<ErrorBody>(429, "rate limited");
//     rl.headers = {{"Retry-After",
//     openapi::response_header<std::uint64_t>("seconds to wait")}};
// Success responses attach theirs via OperationInfo::response_headers.
template <typename T> ResponseHeader response_header(std::string description = {}) {
    return {std::move(description), schema_json<T>()};
}

// Declares one response of a typed route beyond the success one — an error
// body, a redirect, a status the handler answers. `T` is the body type (glaze
// schema captured now), `status` and `description` are what the document says.
// Pass as many as the handler may produce:
//     server.route<Req, Res>(methods, path, handler, openapi::OperationInfo{},
//                            openapi::resp<BadRequest>(400, "invalid body"),
//                            openapi::resp<Conflict>(409, "name taken"));
template <typename T>
Response resp(int status, std::string description = {}, std::string content_type = "application/json") {
    return Response{status, std::move(description), std::move(content_type), schema_json<T>()};
}

// utoipa's params(IntoParams): a parameter declaration. The name and location
// are explicit (C++23 reflection cannot derive member names into parameters),
// but the schema still comes from the type automatically:
//     openapi::query<std::string>("q", {.description = "search term"}),
//     openapi::path<std::int64_t>("id", {.required = true}),
//     openapi::header<std::string>("x-token", {.required = true})
struct ParamOptions {
    bool required{false};
    std::string description;
};

template <typename T> Param query(std::string name, ParamOptions opts = {}) {
    return {std::move(name), ParamIn::Query, opts.required, std::move(opts.description), schema_json<T>()};
}
template <typename T> Param path(std::string name, ParamOptions opts = {}) {
    return {std::move(name), ParamIn::Path, opts.required, std::move(opts.description), schema_json<T>()};
}
template <typename T> Param header(std::string name, ParamOptions opts = {}) {
    return {std::move(name), ParamIn::Header, opts.required, std::move(opts.description), schema_json<T>()};
}
template <typename T> Param cookie(std::string name, ParamOptions opts = {}) {
    return {std::move(name), ParamIn::Cookie, opts.required, std::move(opts.description), schema_json<T>()};
}

// Security scheme factories for OpenApiSpec::security_scheme(name, scheme):
//     server.openapi().security_scheme("bearer", openapi::bearer());
//     server.openapi().security_scheme("key",   openapi::api_key("X-Api-Key"));
// An operation declares it is behind one or more of these via
// OperationInfo::security = {"bearer"} — utoipa's security(("bearer" = [])).
inline SecurityScheme bearer() { return {"http", "bearer", {}, {}}; }
inline SecurityScheme basic() { return {"http", "basic", {}, {}}; }
inline SecurityScheme api_key(std::string name, std::string in = "header") {
    return {"apiKey", {}, std::move(in), std::move(name)};
}

// Folds a typed route's annotation pack (router.h): a Param goes to the
// operation's parameters, a Response to its error responses. Mirrors utoipa,
// where params(...) and responses(...) annotate one operation side by side.
inline void collect_annotation(std::vector<Param> &, std::vector<Response> &) {}

template <typename T, typename... Rest>
void collect_annotation(std::vector<Param> &params, std::vector<Response> &extras, T &&value, Rest &&...rest) {
    using D = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<D, Param>) {
        params.push_back(std::forward<T>(value));
    } else if constexpr (std::is_same_v<D, Response>) {
        extras.push_back(std::forward<T>(value));
    } else {
        static_assert(sizeof(D) == 0, "a typed route's trailing annotations must be openapi::param(...) "
                                      "or openapi::resp<T>(...)");
    }
    collect_annotation(params, extras, std::forward<Rest>(rest)...);
}

} // namespace openapi
} // namespace simple_http