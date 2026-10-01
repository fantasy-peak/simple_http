#pragma once

// JSON helpers: read request bodies into a struct and serialize a struct into
// a response, backed by **glaze** (header-only, a regular dependency of the
// library since the JSON helpers are on by default). This is the axum
// `Json<T>` extractor / Go `json.Marshal` + `json.NewDecoder` equivalent:
//
//   struct Pet { std::int64_t id{}; std::string name; };  // glaze reflection
//
//   server.route({Method::Post}, "/pets", [](RequestPtr req, ResponsePtr res)
//                                        -> asio::awaitable<void> {
//       auto pet = co_await simple_http::read_json_body<Pet>(*req); // 400 on failure
//       if (!pet) { co_await res->status(400).send("{}"); co_return; }
//       co_await simple_http::write_json(res, *pet, 201);
//   });
//
// Like the other form readers, the body is read as a stream and the result is
// std::expected.

#include <glaze/glaze.hpp>

#include <expected>
#include <string>
#include <utility>

#include "../core/http_status.h"
#include "../core/logging.h"
#include "../core/mime.h"
#include "../core/types.h"
#include "request.h"
#include "response.h"

namespace simple_http {

namespace asio = boost::asio;

// Reads the whole request body and parses it as JSON into T (via glaze
// reflection). A body read failure surfaces as the underlying error; an
// unparseable or type-mismatched body is std::unexpected — the caller decides
// whether to answer 400. Same contract as read_urlencoded_body.
template <typename T> asio::awaitable<std::expected<T, error_code>> read_json_body(Request &req) {
    auto raw = co_await req.body().read_all();
    if (!raw) {
        co_return std::unexpected{raw.error()};
    }
    auto value = glz::read_json<T>(*raw);
    if (!value) {
        co_return std::unexpected{make_error_code(asio::error::invalid_argument)};
    }
    co_return std::move(*value);
}

// Serializes `value` (glaze reflection) and sends it as a one-shot JSON
// response with `status_code` (200 by default). A value glaze cannot represent
// is answered 500 and reported as an error — never a silent empty body.
template <typename T>
asio::awaitable<error_code> write_json(std::shared_ptr<Response> res, const T &value, int status_code = status::ok) {
    auto json = glz::write_json(value);
    if (!json) {
        SIMPLE_HTTP_ERROR_LOG("json: failed to serialize the response");
        (void)co_await res->status(status::internal_server_error).send("");
        co_return make_error_code(asio::error::invalid_argument);
    }
    co_return co_await res->status(status_code).content_type(mime::app_json).send(std::move(*json));
}

} // namespace simple_http