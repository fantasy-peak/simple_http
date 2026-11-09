#pragma once

// Unified JSON error responses and RFC 9457 "problem details" (C++17 port).
//
//   res->status(status::not_found);
//   async_write_error(res, status::not_found, {"not_found", "no such pet"}, token);
//   // {"code":"not_found","message":"no such pet"}
//
//   async_write_problem(res, status::unprocessable_content,
//                       {"about:blank", "validation", 0, "invalid body", ""}, token);
//   // Content-Type: application/problem+json
//
// The JSON is rendered by hand: these two envelopes have a fixed shape, and the
// port deliberately does not depend on a JSON library yet. Generic JSON
// serialization (read_json_body / write_json) is a later phase.

#include <boost/asio/any_completion_handler.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "../core/http_status.h"
#include "response.h"

namespace simple_http {

// The common application error envelope. `details` is optional and omitted (null)
// when empty; a client can rely on `code` being stable and machine-readable.
struct JsonError {
    std::string code;
    std::string message;
    std::optional<std::string> details;
};

// RFC 9457 §3 members. `type` defaults to "about:blank" as the RFC specifies for
// errors with no more specific URI; `status` is filled from the HTTP status by
// async_write_problem().
struct ProblemDetails {
    std::string type{"about:blank"};
    std::string title;
    int status{};
    std::string detail;
    std::string instance;
};

namespace detail {

inline void json_escape_into(std::string &out, std::string_view text) {
    for (char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                static constexpr char hex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(hex[(static_cast<unsigned char>(c) >> 4) & 0x0f]);
                out.push_back(hex[static_cast<unsigned char>(c) & 0x0f]);
            } else {
                out.push_back(c);
            }
        }
    }
}

inline void json_string_into(std::string &out, std::string_view text) {
    out.push_back('"');
    json_escape_into(out, text);
    out.push_back('"');
}

} // namespace detail

inline std::string render_json_error(const JsonError &body) {
    std::string out = "{\"code\":";
    detail::json_string_into(out, body.code);
    out += ",\"message\":";
    detail::json_string_into(out, body.message);
    // Omit an absent details member instead of emitting null: Go's reference
    // implementation (and most JSON clients) treats the field as optional, and
    // the harness compares the rendered object field by field.
    if (body.details) {
        out += ",\"details\":";
        detail::json_string_into(out, *body.details);
    }
    out.push_back('}');
    return out;
}

inline std::string render_problem_details(const ProblemDetails &problem) {
    std::string out = "{\"type\":";
    detail::json_string_into(out, problem.type);
    out += ",\"title\":";
    detail::json_string_into(out, problem.title);
    out += ",\"status\":";
    out += std::to_string(problem.status);
    out += ",\"detail\":";
    detail::json_string_into(out, problem.detail);
    out += ",\"instance\":";
    detail::json_string_into(out, problem.instance);
    out.push_back('}');
    return out;
}

// Serializes `body` as JSON with `status_code`. Completion signature:
// void(error_code).
template <typename Token>
auto async_write_error(std::shared_ptr<ResponseWriter> res, int status_code, JsonError body, Token &&token) {
    res->status(status_code).content_type("application/json");
    return res->async_send(render_json_error(body), std::forward<Token>(token));
}

// Serializes RFC 9457 problem details as application/problem+json. The `status`
// member always mirrors `status_code`, as the RFC requires.
template <typename Token>
auto async_write_problem(std::shared_ptr<ResponseWriter> res, int status_code, ProblemDetails problem, Token &&token) {
    problem.status = status_code;
    res->status(status_code).content_type("application/problem+json");
    return res->async_send(render_problem_details(problem), std::forward<Token>(token));
}

} // namespace simple_http
