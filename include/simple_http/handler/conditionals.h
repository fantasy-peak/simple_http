#pragma once

// Generic conditional-request handling for handlers (C++17 token-only port).
//
//   req/res known, then:
//     async_maybe_not_modified(req, res, etag, mtime, token);
//     // completion: void(error_code, bool) -- true means "answered 304"
//
// Implements GET/HEAD conditional requests (RFC 9110 §13): sets the response's
// ETag and Last-Modified validators, then, when the client's own validators
// match, answers 304 bodyless and reports true so the handler skips the body.

#include <cstdint>
#include <string>
#include <utility>

#include "../core/asio_compat.h"
#include "../core/cancel.h"
#include "../core/http_date.h"
#include "../core/http_field.h"
#include "../core/http_method.h"
#include "../core/http_status.h"
#include "../core/validators.h"
#include "../proto/request.h"
#include "../proto/response.h"
#include "handler.h"

namespace simple_http {

template <typename Token>
auto async_maybe_not_modified(const RequestPtr &req, const ResponsePtr &res, std::string etag,
                              std::int64_t last_modified_unix, Token &&token) {
    return asio::async_initiate<Token, void(error_code, bool)>(
        [req, res, etag = std::move(etag), last_modified_unix](auto handler) mutable {
            if (!etag.empty()) {
                res->header(field::etag, etag);
            }
            if (last_modified_unix > 0) {
                res->header(field::last_modified, http_date(last_modified_unix));
            }
            // The "not conditional" answers are decided synchronously, but an
            // Asio operation must submit its completion rather than invoke it
            // from inside the initiating function.
            auto post_false = [req](auto done) {
                auto ex = req->executor();
                asio::post(ex, [done = std::move(done)]() mutable { std::move(done)(error_code{}, false); });
            };
            const bool get_or_head = req->method() == Method::Get || req->method() == Method::Head;
            if (!get_or_head) {
                post_false(std::move(handler));
                return;
            }
            bool not_modified = false;
            if (const auto inm = req->header(field::if_none_match)) {
                not_modified = etag_matches(*inm, etag);
            } else if (const auto ims = req->header(field::if_modified_since); ims && last_modified_unix > 0) {
                const auto since = parse_http_date(*ims);
                not_modified = since.has_value() && last_modified_unix <= *since;
            }
            if (!not_modified) {
                post_false(std::move(handler));
                return;
            }
            res->status(status::not_modified);
            // Forward the caller's cancellation slot to the 304 write, so an
            // outer asio::cancel_after can abort it instead of waiting for the
            // write to drain after the deadline.
            auto slot = asio::get_associated_cancellation_slot(handler);
            res->async_send_bodyless(forward_cancel_slot(
                slot, [handler = std::move(handler)](error_code ec) mutable { std::move(handler)(ec, true); }));
        },
        token);
}

} // namespace simple_http
