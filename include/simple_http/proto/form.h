#pragma once

// High-level body readers for the two classic form encodings — the axum
// `Form` / `Multipart` extractors, the Go `r.ParseForm` / `r.ParseMultipartForm`
// — as coroutines, because a request body is a stream:
//
//   auto form = co_await simple_http::read_urlencoded_body(*req);
//   if (!form) { /* 400 */ }
//   if (auto q = form->get("q")) { ... }
//
//   auto upload = co_await simple_http::read_multipart_body(*req);
//   if (!upload) { /* 400 */ }
//   for (const auto &part : *upload) { /* part.name / part.filename / part.data */ }

#include <boost/asio/awaitable.hpp>
#include <cstddef>
#include <expected>
#include <string>
#include <vector>

#include "../core/http_field.h"
#include "../core/types.h"
#include "body.h"
#include "multipart.h"
#include "query.h"
#include "request.h"

namespace simple_http {

namespace asio = boost::asio;

// Reads the whole body and parses it as application/x-www-form-urlencoded
// ("a=1&b=2"). A body read failure surfaces as the underlying error.
inline asio::awaitable<std::expected<QueryParams, error_code>> read_urlencoded_body(Request &req) {
    auto raw = co_await req.body().read_all();
    if (!raw) {
        co_return std::unexpected{raw.error()};
    }
    co_return QueryParams::parse(*raw);
}

// Reads the whole body and parses it as multipart/form-data; the boundary is
// taken from the request's Content-Type. An unparseable framing (no
// Content-Type, no boundary, a body that does not close, a part past
// max_part_bytes / max_parts) is std::unexpected. Parts are returned in order,
// fields and files mixed: a plain field has an empty `filename`.
inline asio::awaitable<std::expected<std::vector<MultipartPart>, error_code>>
read_multipart_body(Request &req, std::size_t max_part_bytes = 16 * 1024 * 1024, std::size_t max_parts = 100) {
    const auto content_type = req.header(field::content_type);
    if (!content_type) {
        co_return std::unexpected{make_error_code(asio::error::invalid_argument)};
    }
    const auto boundary = multipart_boundary(*content_type);
    if (!boundary) {
        co_return std::unexpected{make_error_code(asio::error::invalid_argument)};
    }
    auto raw = co_await req.body().read_all();
    if (!raw) {
        co_return std::unexpected{raw.error()};
    }
    auto parts = parse_multipart(*raw, *boundary, max_part_bytes, max_parts);
    if (!parts) {
        co_return std::unexpected{make_error_code(asio::error::invalid_argument)};
    }
    co_return std::move(*parts);
}

} // namespace simple_http