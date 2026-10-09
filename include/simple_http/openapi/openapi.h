#pragma once

// OpenAPI integration glue: JSON Schemas from C++ types via glaze's
// compile-time reflection. An aggregate struct is reflected with no metadata
// and `schema_json<T>()` returns its JSON Schema, which router.h records into
// the document model (openapi_doc.h).
//
// There is no compile-time on/off switch. glaze is a normal dependency of the
// library (it backs proto/json.h), and documentation is per-route metadata:
// every route may carry an optional `openapi::doc()...` descriptor as its
// trailing argument (see Operation below), and a route without one is not
// documented and pays nothing. Collecting is always on, serving is opt-in via
// Router::serve_openapi().

#include <algorithm>
#include <boost/asio/awaitable.hpp>
#include <glaze/glaze.hpp>
#include <glaze/json/schema.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "../proto/params.h"  // simple_http::path_params (openapi:: aliases it)
#include "../proto/request.h" // Request::param / Request::body for read_body
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

// Marker: "no request body". A route that takes no body simply omits
// `.request_body<T>()`; NoBody is kept as the explicit second type where a
// path-parameter struct is present but no body — the axum `Path(params)`
// handler:
//     server.route({Method::Get}, "/orders/{order_id}/items/{item_id}", handler,
//                  openapi::doc().path_params<OrderItemParams>().response<Order>());
struct NoBody {};

// The axum `Path<Params>` extractor, kept out of the handler signature: the
// captures of a template route are deserialized into `Params` by field name
// (glaze reflection), exactly the same "field name == {name}" contract. A
// missing or unparsable capture yields nullopt, and the handler decides (404).
// Forwards to the always-on proto/params.h version — OpenAPI just re-exports
// it so typed routes read the same as the untyped ones.
template <typename T> std::optional<T> path_params(const Request &req) { return simple_http::path_params<T>(req); }

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
inline bool params_match_template_names(const std::vector<std::string> &fields_in,
                                        const std::vector<std::string> &template_names_in) {
    if (fields_in.size() != template_names_in.size()) {
        return false;
    }
    auto fields = fields_in;
    auto template_names = template_names_in;
    std::sort(fields.begin(), fields.end());
    std::sort(template_names.begin(), template_names.end());
    return fields == template_names;
}

template <typename T> bool params_match_template(const std::vector<std::string> &names) {
    return params_match_template_names(param_field_names<T>(), names);
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
// Success responses attach theirs via Operation::response_header<T>.
template <typename T> ResponseHeader response_header(std::string description = {}) {
    return {std::move(description), schema_json<T>()};
}

// Declares one response of an operation beyond the success one — an error
// body, a redirect, a status the handler answers. `T` is the body type (glaze
// schema captured now), `status` and `description` are what the document says.
// Pass as many as the handler may produce, via Operation::error<T>(...) (or
// hand a customised one to Operation::response(Response)):
//     openapi::doc().error<BadRequest>(400, "invalid body")
//                   .error<Conflict>(409, "name taken"));
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
inline SecurityScheme bearer() {
    return {"http", "bearer", {}, {}};
}
inline SecurityScheme basic() {
    return {"http", "basic", {}, {}};
}
inline SecurityScheme api_key(std::string name, std::string in = "header") {
    return {"apiKey", {}, std::move(in), std::move(name)};
}

// --- per-route operation descriptor -----------------------------------------
// A route annotates its OAS operation with a value built here, passed as the
// optional trailing argument of route(). A route with no descriptor is not
// documented and pays nothing; there is no compile-time on/off switch and no
// template-arity overload set — every setter below reduces its type to runtime
// data (a schema string, capured field names) at the call site, so the router
// only ever sees one plain object.
//
//     server.route({Method::Post}, "/pets", handler,
//         openapi::doc()
//             .request_body<CreatePetReq>()
//             .response<Pet>(201)
//             .error<ErrorBody>(400, "invalid body")
//             .summary("create a pet")
//             .security({"bearer"}));
//
// The `path_params<P>()` form is the axum `Path<P>` contract: P's field names
// must equal the route template's `{name}`s (checked at registration) and each
// field's type is that path parameter's schema.

// The default "no descriptor": route()'s trailing parameter type, so a route
// that passes nothing is not documented.
struct NoDoc {};

class Operation {
  public:
    // --- typed slots: the type is reduced to its schema string right here ---
    template <typename T> Operation &request_body() {
        m_request_schema = schema_json<T>();
        return *this;
    }
    // The success response body and status (200 unless declared otherwise).
    template <typename T> Operation &response(int status = 200) {
        m_response_schema = schema_json<T>();
        m_info.success_status = status;
        return *this;
    }
    // A whole response declared by value — openapi::resp<T>() customised (e.g.
    // with headers) before being handed over. An error, a redirect, anything.
    Operation &response(Response r) {
        m_extras.push_back(std::move(r));
        return *this;
    }
    template <typename T> Operation &error(int status, std::string description = {}) {
        m_extras.push_back(resp<T>(status, std::move(description)));
        return *this;
    }

    // --- parameters ---
    template <typename T> Operation &query(std::string name, ParamOptions opts = {}) {
        m_params.push_back(openapi::query<T>(std::move(name), opts));
        return *this;
    }
    template <typename T> Operation &header(std::string name, ParamOptions opts = {}) {
        m_params.push_back(openapi::header<T>(std::move(name), opts));
        return *this;
    }
    template <typename T> Operation &cookie(std::string name, ParamOptions opts = {}) {
        m_params.push_back(openapi::cookie<T>(std::move(name), opts));
        return *this;
    }
    // Declare/override one path parameter by name (the template already implies
    // the rest).
    template <typename T> Operation &path_param(std::string name, ParamOptions opts = {}) {
        m_params.push_back(openapi::path<T>(std::move(name), opts));
        return *this;
    }
    // The path-parameter struct: one document parameter per field, and the
    // binding to the route template checked at registration.
    template <typename P> Operation &path_params() {
        for (auto &p : path_params_schema<P>()) {
            m_params.push_back(std::move(p));
        }
        m_path_field_names = param_field_names<P>();
        m_has_path_params = true;
        return *this;
    }
    // A response header on the success response.
    template <typename T> Operation &response_header(std::string name, std::string description = {}) {
        m_info.response_headers.emplace_back(std::move(name), openapi::response_header<T>(std::move(description)));
        return *this;
    }

    // --- free-text metadata (OperationInfo fields) ---
    Operation &summary(std::string v) {
        m_info.summary = std::move(v);
        return *this;
    }
    Operation &tag(std::string v) {
        m_info.tag = std::move(v);
        return *this;
    }
    Operation &description(std::string v) {
        m_info.description = std::move(v);
        return *this;
    }
    Operation &operation_id(std::string v) {
        m_info.operation_id = std::move(v);
        return *this;
    }
    Operation &deprecated(bool v = true) {
        m_info.deprecated = v;
        return *this;
    }
    // The names of components.securitySchemes this operation is behind.
    Operation &security(std::vector<std::string> names) {
        m_info.security.insert(m_info.security.end(), std::make_move_iterator(names.begin()),
                               std::make_move_iterator(names.end()));
        return *this;
    }
    Operation &request_required(bool v = true) {
        m_info.request_required = v;
        return *this;
    }
    Operation &request_content_type(std::string v) {
        m_info.request_content_type = std::move(v);
        return *this;
    }
    Operation &response_content_type(std::string v) {
        m_info.response_content_type = std::move(v);
        return *this;
    }

    // --- read-back: the router's record step and the registration check ---
    const OperationInfo &info() const { return m_info; }
    const std::string &request_schema() const { return m_request_schema; }
    const std::string &response_schema() const { return m_response_schema; }
    const std::vector<Param> &params() const { return m_params; }
    const std::vector<Response> &extras() const { return m_extras; }
    // Whether the path-parameter struct (when one was declared) matches the
    // route template's `{name}`s — the registration-time binding check.
    bool path_params_match(std::string_view path) const {
        return !m_has_path_params || params_match_template_names(m_path_field_names, template_param_names(path));
    }

  private:
    OperationInfo m_info;
    std::string m_request_schema;
    std::string m_response_schema;
    std::vector<Param> m_params;
    std::vector<Response> m_extras;
    std::vector<std::string> m_path_field_names;
    bool m_has_path_params{false};
};

// A fresh descriptor: openapi::doc().response<Pet>()...
inline Operation doc() { return {}; }

// Records a route's descriptor into the document, the router's registration
// step. Path parameters the operation did not declare are filled from the
// template (string schema), a declared one winning.
inline void record_operation(OpenApiSpec &spec, const Operation &op, const std::vector<Method> &methods,
                             const std::string &path) {
    std::vector<Param> params = op.params();
    merge_path_params(params, path);
    spec.add_operation(methods, path, op.info(), op.request_schema(), op.response_schema(), std::move(params),
                       op.extras());
}

} // namespace openapi
} // namespace simple_http