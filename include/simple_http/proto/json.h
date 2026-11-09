#pragma once

// JSON helpers: read request bodies into a struct and serialize a struct into a
// response, backed by **glaze**. This is the axum `Json<T>` extractor / Go
// `json.Marshal` + `json.NewDecoder` equivalent:
//
//   struct Pet { std::int64_t id{}; std::string name; };  // glaze reflection
//
//   server.route({Method::Post}, "/pets",
//       [](RequestPtr req, ResponsePtr res, HandlerCompletion done) {
//           simple_http::async_read_json_body<Pet>(
//               *req, [res, done = std::move(done)](error_code ec, simple_http::result<Pet> pet) mutable {
//                   if (!pet) {
//                       res->status(simple_http::status::bad_request);
//                       res->async_send("{}", simple_http::finish_with(std::move(done)));
//                       return;
//                   }
//                   simple_http::async_write_json(std::move(res), *pet, 201,
//                                                 simple_http::finish_with(std::move(done)));
//               });
//       });
//
// Like the other form readers, the body is read as a stream and the result is
// the library's `result<T>` (error_code based, C++17). The glaze-gated surface
// is described in core/glaze_support.h: a real C++17 build does not see these
// helpers.

#include "../core/glaze_support.h"

#if SIMPLE_HTTP_HAS_GLAZE

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/compose.hpp>
#include <string>
#include <utility>

#include "../core/http_status.h"
#include "../core/logging.h"
#include "../core/mime.h"
#include "../core/result.h"
#include "../core/types.h"
#include "request.h"
#include "response.h"

namespace simple_http {

namespace asio = boost::asio;

namespace detail {

template <typename T> struct JsonReadOp {
    Request &request;
    bool started{false};

    asio::any_io_executor get_executor() const noexcept { return request.executor(); }

    template <typename Self> void operator()(Self &self, error_code ec = {}, std::string raw = {}) {
        if (!started) {
            started = true;
            request.body().async_read_all(std::move(self));
            return;
        }
        if (ec) {
            self.complete(ec, result<T>{ec});
            return;
        }
        auto value = glz::read_json<T>(raw);
        if (!value) {
            const auto error = boost::system::errc::make_error_code(boost::system::errc::invalid_argument);
            self.complete(error, result<T>{error});
            return;
        }
        self.complete(error_code{}, result<T>{std::move(*value)});
    }
};

struct JsonWriteOp {
    std::shared_ptr<ResponseWriter> response;
    std::string json;
    error_code pre;
    bool started{false};

    asio::any_io_executor get_executor() const noexcept { return response->writer().get_executor(); }

    template <typename Self> void operator()(Self &self, error_code ec = {}) {
        if (!started) {
            started = true;
            if (pre) {
                self.complete(pre);
                return;
            }
            response->async_send(std::move(json), std::move(self));
            return;
        }
        self.complete(ec);
    }
};

} // namespace detail

// Reads the whole request body and parses it as JSON into T (via glaze
// reflection). Completion signature: void(error_code, result<T>). A body read
// failure surfaces as the underlying error; an unparseable or type-mismatched
// body is invalid_argument -- the caller decides whether to answer 400.
template <typename T, typename Token> auto async_read_json_body(Request &request, Token &&token) {
    return asio::async_compose<Token, void(error_code, result<T>)>(detail::JsonReadOp<T>{request}, token,
                                                                   request.executor());
}

// Serializes `value` (glaze reflection) and sends it as a one-shot JSON
// response with `status_code`. A value glaze cannot represent is reported as
// invalid_argument and leaves the response untouched.
template <typename T, typename Token>
auto async_write_json(std::shared_ptr<ResponseWriter> response, const T &value, int status_code, Token &&token) {
    auto json = glz::write_json(value);
    error_code pre{};
    std::string body;
    if (!json) {
        SIMPLE_HTTP_ERROR_LOG("json: failed to serialize the response");
        pre = boost::system::errc::make_error_code(boost::system::errc::invalid_argument);
    } else {
        body = std::move(*json);
        response->status(status_code).content_type(mime::app_json);
    }
    auto executor = response->writer().get_executor();
    return asio::async_compose<Token, void(error_code)>(detail::JsonWriteOp{std::move(response), std::move(body), pre},
                                                        token, executor);
}

template <typename T, typename Token>
auto async_write_json(std::shared_ptr<ResponseWriter> response, const T &value, Token &&token) {
    return async_write_json(std::move(response), value, status::ok, std::forward<Token>(token));
}

} // namespace simple_http

#endif // SIMPLE_HTTP_HAS_GLAZE
