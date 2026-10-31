#pragma once

// Unified JSON error responses and RFC 9457 "problem details".
//
//   co_await simple_http::write_error(res, status::not_found,
//                                     {"not_found", "no such pet"});
//   // {"code":"not_found","message":"no such pet"}
//
//   co_await simple_http::write_problem(res, status::unprocessable_content,
//                                       {"validation", "invalid body", {}, {}});
//   // Content-Type: application/problem+json
//
// Both are thin wrappers over write_json, so the response is one-shot and gets
// the library's default Date/Server headers. They are conveniences, not a new
// error channel: a handler that wants a different shape can keep calling
// write_json directly.

#include <optional>
#include <string>
#include <utility>

#include "../core/http_status.h"
#include "json.h"
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
// write_problem().
struct ProblemDetails {
    std::string type{"about:blank"};
    std::string title;
    int status{};
    std::string detail;
    std::string instance;
};

// Serializes `body` as JSON with `status_code`.
inline asio::awaitable<error_code> write_error(std::shared_ptr<ResponseWriter> res, int status_code, JsonError body) {
    co_return co_await write_json(std::move(res), body, status_code);
}

// Serializes RFC 9457 problem details as application/problem+json. The `status`
// member always mirrors `status_code`, as the RFC requires.
inline asio::awaitable<error_code> write_problem(std::shared_ptr<ResponseWriter> res, int status_code,
                                                 ProblemDetails problem) {
    problem.status = status_code;
    auto json = glz::write_json(problem);
    if (!json) {
        SIMPLE_HTTP_ERROR_LOG("problem: failed to serialize the response");
        (void)co_await res->status(status::internal_server_error).send("");
        co_return make_error_code(asio::error::invalid_argument);
    }
    co_return co_await res->status(status_code).content_type("application/problem+json").send(std::move(*json));
}

} // namespace simple_http
