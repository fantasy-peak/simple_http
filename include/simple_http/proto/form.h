#pragma once

// High-level body readers for the two classic form encodings, as token-based
// operations (C++17 port):
//
//   req.body() -> async_read_all
//
//   read_urlencoded_body(req, token)   -> void(error_code, QueryParams)
//   read_multipart_body(req, token)    -> void(error_code, std::vector<MultipartPart>)
//
// The body is a stream, so both are asynchronous. Errors are the usual
// error_code completion argument; the caller's token decides whether they
// surface as values or exceptions.

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/compose.hpp>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "../core/http_field.h"
#include "../core/types.h"
#include "body.h"
#include "multipart.h"
#include "query.h"
#include "request.h"

namespace simple_http {

namespace asio = boost::asio;

namespace detail {

struct UrlencodedBodyOp {
    Request &request;
    bool started{false};

    asio::any_io_executor get_executor() noexcept { return request.executor(); }

    template <typename Self> void operator()(Self &self, error_code ec = {}, std::string raw = {}) {
        if (!started) {
            started = true;
            request.body().async_read_all(std::move(self));
            return;
        }
        if (ec) {
            self.complete(ec, QueryParams{});
            return;
        }
        self.complete(error_code{}, QueryParams::parse(raw));
    }
};

struct MultipartBodyOp {
    Request &request;
    error_code pre;
    std::string boundary;
    std::size_t max_part_bytes;
    std::size_t max_parts;
    bool started{false};

    asio::any_io_executor get_executor() noexcept { return request.executor(); }

    template <typename Self> void operator()(Self &self, error_code ec = {}, std::string raw = {}) {
        if (!started) {
            started = true;
            if (pre) {
                self.complete(pre, std::vector<MultipartPart>{});
                return;
            }
            request.body().async_read_all(std::move(self));
            return;
        }
        if (ec) {
            self.complete(ec, std::vector<MultipartPart>{});
            return;
        }
        auto parts = parse_multipart(raw, boundary, max_part_bytes, max_parts);
        if (!parts) {
            self.complete(boost::system::errc::make_error_code(boost::system::errc::invalid_argument),
                          std::vector<MultipartPart>{});
            return;
        }
        self.complete(error_code{}, std::move(*parts));
    }
};

} // namespace detail

// Reads the whole body and parses it as application/x-www-form-urlencoded
// ("a=1&b=2"). A body read failure surfaces as the underlying error.
template <typename Token> auto read_urlencoded_body(Request &request, Token &&token) {
    return asio::async_compose<Token, void(error_code, QueryParams)>(detail::UrlencodedBodyOp{request}, token,
                                                                     request.executor());
}

// Reads the whole body and parses it as multipart/form-data; the boundary is
// taken from the request's Content-Type. An unparseable framing (no
// Content-Type, no boundary, a body that does not close, a part past
// max_part_bytes / max_parts) is an error.
template <typename Token>
auto read_multipart_body(Request &request, std::size_t max_part_bytes, std::size_t max_parts, Token &&token) {
    error_code pre{};
    std::string boundary;
    const auto content_type = request.header(field::content_type);
    if (!content_type) {
        pre = boost::system::errc::make_error_code(boost::system::errc::invalid_argument);
    } else if (auto parsed = multipart_boundary(*content_type)) {
        boundary = std::move(*parsed);
    } else {
        pre = boost::system::errc::make_error_code(boost::system::errc::invalid_argument);
    }
    return asio::async_compose<Token, void(error_code, std::vector<MultipartPart>)>(
        detail::MultipartBodyOp{request, pre, std::move(boundary), max_part_bytes, max_parts}, token,
        request.executor());
}

template <typename Token> auto read_multipart_body(Request &request, Token &&token) {
    return read_multipart_body(request, 16 * 1024 * 1024, 100, std::forward<Token>(token));
}

} // namespace simple_http
