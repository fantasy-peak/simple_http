#pragma once

// Generic conditional-request handling for handlers — the 304 path that
// static_files already takes, exported for any handler serving a cacheable
// resource. Call it after the resource's metadata is known, before sending the
// body:
//
//   if (co_await simple_http::maybe_not_modified(req, res, etag, mtime_unix)) {
//       co_return; // answered 304, bodyless — no body was produced
//   }
//   co_await res->status(200).send(body);

#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <string>

#include "../core/http_date.h"   // parse_http_date, http_date
#include "../core/http_field.h"  // etag, last_modified, if_none_match, if_modified_since
#include "../core/http_method.h" // Method
#include "../core/http_status.h" // status::not_modified
#include "../core/validators.h"  // etag_matches
#include "../proto/request.h"
#include "../proto/response.h"

namespace simple_http {

namespace asio = boost::asio;

// Implements GET/HEAD conditional requests (RFC 9110 §13): sets the response's
// ETag and Last-Modified validators, then — when the client's own validators
// match — answers 304 bodyless and returns true, so the handler skips
// producing the body. If-None-Match is consulted first, then
// If-Modified-Since; "*" and weak comparison are handled by etag_matches.
// Non-GET/HEAD requests skip the 304 (If-None-Match does not apply to them)
// but still get their validators. An empty `etag` emits no ETag and honours
// only If-Modified-Since; `last_modified_unix` 0 emits no Last-Modified.
inline asio::awaitable<bool> maybe_not_modified(const RequestPtr &req, const ResponsePtr &res, std::string etag,
                                                std::int64_t last_modified_unix = 0) {
    if (!etag.empty()) {
        res->header(field::etag, etag);
    }
    if (last_modified_unix > 0) {
        res->header(field::last_modified, http_date(last_modified_unix));
    }
    const bool get_or_head = req->method() == Method::Get || req->method() == Method::Head;
    if (!get_or_head) {
        co_return false;
    }

    bool not_modified = false;
    if (const auto inm = req->header(field::if_none_match)) {
        not_modified = etag_matches(*inm, etag);
    } else if (const auto ims = req->header(field::if_modified_since); ims && last_modified_unix > 0) {
        const auto since = parse_http_date(*ims);
        not_modified = since.has_value() && last_modified_unix <= *since;
    }
    if (not_modified) {
        res->status(status::not_modified);
        (void)co_await res->send_bodyless();
        co_return true;
    }
    co_return false;
}

} // namespace simple_http