#pragma once

// The OpenAPI (Swagger) document model and its JSON rendering. This file is
// glaze-free by design: the JSON Schemas for request/response bodies enter as
// raw JSON strings (produced elsewhere, e.g. openapi.h via glaze), so nothing
// here has to understand a schema — it only splices one.
//
// The document is collected by Router::route<Req, Res> (see router.h), which is
// what turns a typed registration into an OAS operation automatically. The rest
// of the OAS skeleton — info, servers, paths, per-method operations — is built
// by hand here.

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../core/http_method.h"
#include "../core/http_status.h"  // reason_phrase for the default response descriptions

namespace simple_http {
namespace openapi {

// A declared response header (OAS `responses.<code>.headers.<name>`) — the
// value type's inline schema (see openapi.h::response_header<T>).
struct ResponseHeader {
    std::string description;
    std::string schema;  // raw JSON; empty = no schema declared
};

// Free-text metadata for one operation, mirroring the fields of utoipa's
// #[utoipa::path] annotation. All optional; the schemas are automatic.
struct OperationInfo {
    std::string summary;
    std::string tag;
    // The status the handler answers on success. Defaults to 200; a POST whose
    // happy path is 201 declares that here.
    int success_status{200};
    // utoipa: description / operation_id / deprecated.
    std::string description;
    std::string operation_id;
    bool deprecated{false};
    // Whether the request body is mandatory. utoipa: request_body(required=…).
    bool request_required{true};
    // What the handler writes and reads. Defaults are JSON; a handler that
    // serves text/plain or form data says so here.
    std::string request_content_type{"application/json"};
    std::string response_content_type{"application/json"};
    // Security requirement: the names of components.securitySchemes this
    // operation is behind. Empty = no security block. utoipa: security(("x"=[])).
    std::vector<std::string> security;
    // Response headers the success response carries (OAS
    // `responses.<status>.headers`), e.g. {"X-Request-Id", ...}.
    std::vector<std::pair<std::string, ResponseHeader>> response_headers;
};

// Where a parameter travels. utoipa: `in = "query" | "path" | "header"`.
enum class ParamIn {
    Query,
    Path,
    Header,
    Cookie,
};

// One operation parameter: name, location, requiredness, and the body type's
// JSON Schema (glaze) as raw JSON. Built by openapi::query<T>() / path<T>() /
// header<T>() in openapi.h — the utoipa equivalent of an IntoParams record.
struct Param {
    std::string name;
    ParamIn in{ParamIn::Query};
    bool required{false};
    std::string description;
    std::string schema;  // raw JSON
};

// A security scheme entry for components.securitySchemes. Factories in
// openapi.h: openapi::bearer() (http bearer), openapi::api_key(...).
struct SecurityScheme {
    std::string type;       // "http" | "apiKey"
    std::string scheme;     // "bearer" (http only)
    std::string in;         // "query" | "header" | "cookie" (apiKey only)
    std::string param_name;  // the header/query/cookie name (apiKey only)
};

// A declared response: the status, a description for the document, and the body
// type's JSON Schema (glaze) as raw JSON. The primary response is built by
// add_operation from (success_status, response type); error responses arrive
// via openapi::resp<T>(status, description).
struct Response {
    int status{200};
    std::string description;
    std::string content_type{"application/json"};
    std::string schema;  // raw JSON; empty = no body declared
    std::vector<std::pair<std::string, ResponseHeader>> headers;
};

// A minimal JSON emitter for the fixed document shape. Two value kinds:
// ordinary strings (quoted and escaped here) and raw JSON fragments
// (already-serialized schemas), spliced verbatim. Compact (minified) output.
class JsonOut {
  public:
    // A member key; the following value emits with str/raw/boolean/begin_*.
    void key(std::string_view name) {
        separator();
        quote(name);
        m_out += ':';
        m_after_key = true;
    }
    void str(std::string_view value) {
        value_();
        quote(value);
    }
    void raw(std::string_view already_json) {
        value_();
        m_out.append(already_json);
    }
    void boolean(bool b) {
        value_();
        m_out.append(b ? "true" : "false");
    }
    void begin_object() {
        value_();
        m_out += '{';
        m_stack.push_back(false);
    }
    void end_object() {
        m_out += '}';
        m_stack.pop_back();
    }
    void begin_array() {
        value_();
        m_out += '[';
        m_stack.push_back(false);
    }
    void end_array() {
        m_out += ']';
        m_stack.pop_back();
    }
    std::string take() && {
        return std::move(m_out);
    }

  private:
    // A value right after a key needs no separator; an array element does.
    void value_() {
        if (m_after_key) {
            m_after_key = false;
            return;
        }
        separator();
    }
    void separator() {
        if (!m_stack.empty() && m_stack.back()) {
            m_out += ',';
        }
        if (!m_stack.empty()) {
            m_stack.back() = true;
        }
    }
    void quote(std::string_view s) {
        m_out += '"';
        for (char c : s) {
            if (c == '"' || c == '\\') {
                m_out += '\\';
                m_out += c;
            } else if (static_cast<unsigned char>(c) < 0x20) {
                static constexpr char hex[] = "0123456789abcdef";
                m_out += "\\u00";
                m_out += hex[(c >> 4) & 0xF];
                m_out += hex[c & 0xF];
            } else {
                m_out += c;
            }
        }
        m_out += '"';
    }
    std::string m_out;
    std::vector<bool> m_stack;  // per container: whether it already holds a member
    bool m_after_key{false};
};

// The OAS 3.0 document collected from typed routes. Immutable once rendered:
// the typed route overloads append operations, and render_json() reads them all.
class OpenApiSpec {
  public:
    OpenApiSpec& title(std::string v) {
        m_title = std::move(v);
        return *this;
    }
    OpenApiSpec& version(std::string v) {
        m_version = std::move(v);
        return *this;
    }
    OpenApiSpec& server(std::string url) {
        m_server = std::move(url);
        return *this;
    }

    // One typed route. `methods` become one OAS operation each under `path`; the
    // schemas are raw JSON (glaze output). request_schema empty = no request
    // body. `info.success_status` is the success response's status, the success
    // body is `response_schema`, `extras` are the declared error responses and
    // `params` the operation's parameters (utoipa's params(...)).
    void add_operation(const std::vector<Method>& methods, std::string path, OperationInfo info,
                       std::string request_schema, std::string response_schema,
                       std::vector<Param> params = {}, std::vector<Response> extras = {}) {
        auto& ops = m_paths[std::move(path)];
        ops.push_back(Operation{methods, std::move(info), std::move(request_schema),
                                std::move(response_schema), std::move(params), std::move(extras)});
    }

    // A components.securitySchemes entry — utoipa's
    // ComponentsBuilder::security_scheme(name, scheme). Operations reference it
    // by name via OperationInfo::security.
    OpenApiSpec& security_scheme(std::string name, SecurityScheme scheme) {
        m_security_schemes[std::move(name)] = std::move(scheme);
        return *this;
    }

    // Renders the whole document as compact JSON (OAS 3.1.0 — the version
    // whose schema dialect is JSON Schema 2020-12, which is what glaze emits,
    // $defs/$ref included). Paths are sorted for determinism; operations keep
    // registration order.
    std::string render_json() const {
        JsonOut j;
        j.begin_object();
        j.key("openapi");
        j.str("3.1.0");
        j.key("info");
        j.begin_object();
        j.key("title");
        j.str(m_title);
        j.key("version");
        j.str(m_version);
        j.end_object();
        if (!m_server.empty()) {
            j.key("servers");
            j.begin_array();
            j.begin_object();
            j.key("url");
            j.str(m_server);
            j.end_object();
            j.end_array();
        }
        if (!m_security_schemes.empty()) {
            j.key("components");
            j.begin_object();
            j.key("securitySchemes");
            j.begin_object();
            for (const auto& [name, scheme] : m_security_schemes) {
                j.key(name);
                j.begin_object();
                j.key("type");
                j.str(scheme.type);
                if (!scheme.scheme.empty()) {
                    j.key("scheme");
                    j.str(scheme.scheme);
                }
                if (!scheme.in.empty()) {
                    j.key("in");
                    j.str(scheme.in);
                }
                if (!scheme.param_name.empty()) {
                    j.key("name");
                    j.str(scheme.param_name);
                }
                j.end_object();
            }
            j.end_object();
            j.end_object();
        }
        j.key("paths");
        j.begin_object();
        for (const auto& [path, ops] : m_paths) {
            j.key(path);
            j.begin_object();
            for (const auto& op : ops) {
                for (Method m : op.methods) {
                    const std::string_view method = method_name_lower(m);
                    if (method.empty()) {
                        continue;  // Method::Unknown is not an operation
                    }
                    j.key(method);
                    j.begin_object();
                    if (!op.info.tag.empty()) {
                        j.key("tags");
                        j.begin_array();
                        j.str(op.info.tag);
                        j.end_array();
                    }
                    if (!op.info.summary.empty()) {
                        j.key("summary");
                        j.str(op.info.summary);
                    }
                    if (!op.info.description.empty()) {
                        j.key("description");
                        j.str(op.info.description);
                    }
                    if (!op.info.operation_id.empty()) {
                        j.key("operationId");
                        j.str(op.info.operation_id);
                    }
                    if (op.info.deprecated) {
                        j.key("deprecated");
                        j.boolean(true);
                    }
                    if (!op.info.security.empty()) {
                        j.key("security");
                        j.begin_array();
                        for (const auto& scheme : op.info.security) {
                            j.begin_object();
                            j.key(scheme);
                            j.begin_array();
                            j.end_array();
                            j.end_object();
                        }
                        j.end_array();
                    }
                    // utoipa's params(...): each parameter renders name/in/required/schema.
                    if (!op.params.empty()) {
                        j.key("parameters");
                        j.begin_array();
                        for (const auto& param : op.params) {
                            j.begin_object();
                            j.key("name");
                            j.str(param.name);
                            j.key("in");
                            j.str(param_in_string(param.in));
                            if (param.required) {
                                j.key("required");
                                j.boolean(true);
                            }
                            if (!param.description.empty()) {
                                j.key("description");
                                j.str(param.description);
                            }
                            if (!param.schema.empty()) {
                                j.key("schema");
                                j.raw(param.schema);
                            }
                            j.end_object();
                        }
                        j.end_array();
                    }
                    if (!op.request_schema.empty()) {
                        j.key("requestBody");
                        j.begin_object();
                        j.key("required");
                        j.boolean(op.info.request_required);
                        j.key("content");
                        j.begin_object();
                        j.key(op.info.request_content_type);
                        j.begin_object();
                        j.key("schema");
                        j.raw(op.request_schema);
                        j.end_object();
                        j.end_object();
                        j.end_object();
                    }
                    j.key("responses");
                    j.begin_object();
                    // The success response: the declared status + the Res schema.
                    render_response(j, op.info.success_status, reason_phrase(op.info.success_status),
                                    op.info.response_content_type, op.response_schema, op.info.response_headers);
                    // The declared error responses: one entry per resp<T>().
                    for (const auto& extra : op.extras) {
                        render_response(j, extra.status,
                                        extra.description.empty() ? reason_phrase(extra.status)
                                                                  : std::string_view{extra.description},
                                        extra.content_type, extra.schema, extra.headers);
                    }
                    j.end_object();
                    j.end_object();
                }
            }
            j.end_object();
        }
        j.end_object();
        j.end_object();
        return std::move(j).take();
    }

  private:
    struct Operation {
        std::vector<Method> methods;
        OperationInfo info;
        std::string request_schema;   // raw JSON; empty = no request body
        std::string response_schema;  // raw JSON; empty = no success body declared
        std::vector<Param> params;
        std::vector<Response> extras;  // declared error / other responses
    };

    static constexpr std::string_view param_in_string(ParamIn in) noexcept {
        switch (in) {
            case ParamIn::Query:
                return "query";
            case ParamIn::Path:
                return "path";
            case ParamIn::Header:
                return "header";
            case ParamIn::Cookie:
                return "cookie";
        }
        return "query";
    }

    // The default description for a status when the caller gave none: the
    // library's reason_phrase, reused verbatim. (The document's value is the
    // schema and the status, not prose — but OAS requires a description line.)

    // One OAS Response member: status → { description, content if a schema was
    // declared, headers if any were }.
    static void render_response(JsonOut& j, int status, std::string_view description,
                                std::string_view content_type, const std::string& schema,
                                const std::vector<std::pair<std::string, ResponseHeader>>& headers) {
        j.key(std::to_string(status));
        j.begin_object();
        j.key("description");
        j.str(description);
        if (!schema.empty()) {
            j.key("content");
            j.begin_object();
            j.key(content_type);
            j.begin_object();
            j.key("schema");
            j.raw(schema);
            j.end_object();
            j.end_object();
        }
        render_headers(j, headers);
        j.end_object();
    }

    static void render_headers(JsonOut& j, const std::vector<std::pair<std::string, ResponseHeader>>& headers) {
        if (headers.empty()) {
            return;
        }
        j.key("headers");
        j.begin_object();
        for (const auto& [name, h] : headers) {
            j.key(name);
            j.begin_object();
            if (!h.description.empty()) {
                j.key("description");
                j.str(h.description);
            }
            if (!h.schema.empty()) {
                j.key("schema");
                j.raw(h.schema);
            }
            j.end_object();
        }
        j.end_object();
    }

    static constexpr std::string_view method_name_lower(Method m) noexcept {
        switch (m) {
            case Method::Get:
                return "get";
            case Method::Head:
                return "head";
            case Method::Post:
                return "post";
            case Method::Put:
                return "put";
            case Method::Delete:
                return "delete";
            case Method::Options:
                return "options";
            case Method::Patch:
                return "patch";
            case Method::Connect:
                return "connect";
            case Method::Trace:
                return "trace";
            case Method::Unknown:
            default:
                return {};
        }
    }

    std::string m_title{"API"};
    std::string m_version{"0.0.0"};
    std::string m_server;
    std::map<std::string, SecurityScheme> m_security_schemes;
    std::map<std::string, std::vector<Operation>> m_paths;
};

// The page Server::serve_swagger_ui serves: it loads swagger-ui-dist from a CDN
// and points it at `spec_url`. Zero bundled assets; swap in a local
// static_files site when an offline deployment needs them.
inline std::string swagger_ui_html(std::string_view spec_url) {
    return std::string{"<!DOCTYPE html><html><head><meta charset=\"utf-8\"/><title>API</title>"
                       "<link rel=\"stylesheet\" href=\"https://unpkg.com/swagger-ui-dist@5/swagger-ui.css\"/>"
                       "</head><body><div id=\"swagger-ui\"></div>"
                       "<script src=\"https://unpkg.com/swagger-ui-dist@5/swagger-ui-bundle.js\"></script>"
                       "<script>SwaggerUIBundle({url: \""}
        + std::string{spec_url} + std::string{"\", dom_id: \"#swagger-ui\"});</script></body></html>"};
}

// The path parameters a `{name}` template implies, in path order — utoipa
// derives path parameters from the route template itself. These default to a
// string schema and required=true; a user-declared openapi::path<T>("...")
// with the same name overrides one of them.
inline std::vector<Param> template_path_params(std::string_view path) {
    std::vector<Param> out;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t end = path.find('/', pos);
        const std::string_view seg =
            path.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
        if (seg.size() >= 2 && seg.front() == '{' && seg.back() == '}') {
            out.push_back(Param{std::string{seg.substr(1, seg.size() - 2)}, ParamIn::Path, true, {},
                                R"({"type":"string"})"});
        }
        if (end == std::string_view::npos) {
            break;
        }
        pos = end + 1;
    }
    return out;
}

// Just the `{name}` values of a template path, in path order — used to bind a
// template to a Params struct whose field names are the same set.
inline std::vector<std::string> template_param_names(std::string_view path) {
    std::vector<std::string> out;
    for (const Param& p : template_path_params(path)) {
        out.push_back(p.name);
    }
    return out;
}

// Appends the template-derived path parameters the caller has not already
// declared; a declared one wins, so openapi::path<T>("id") overrides the
// default string schema.
inline void merge_path_params(std::vector<Param>& params, std::string_view path) {
    for (auto& p : template_path_params(path)) {
        bool declared = false;
        for (const auto& q : params) {
            if (q.name == p.name && q.in == p.in) {
                declared = true;
                break;
            }
        }
        if (!declared) {
            params.push_back(std::move(p));
        }
    }
}

}  // namespace openapi
}  // namespace simple_http