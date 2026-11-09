#pragma once

// Streaming uploads (C++17 token-only port): consume a request Body directly
// into a file, in bounded chunks. The whole body is never held in memory.
//
//   simple_http::async_read_body_to_file(
//       req->body(), path,
//       [res, done = std::move(done)](error_code ec, std::uint64_t written) mutable { ... });
//
// Completion signature: void(error_code, std::uint64_t). This is the low-level
// primitive; multipart/form-data framing (part names, filenames) is handled by
// read_multipart_stream in proto/multipart_stream.h.
//
// Like send_file, the file I/O is synchronous (std::ofstream): Boost 1.92's
// asio::stream_file needs an io_uring/IOCP backend that a plain epoll build does
// not have. Each chunk is bounded, so the executor is blocked only for one
// write.

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/compose.hpp>
#include <boost/system/error_code.hpp>
#include <cstdint>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#include "../core/types.h"
#include "../proto/body.h"

namespace simple_http {

namespace asio = boost::asio;

namespace detail {

struct ReadBodyToFileOp {
    Body &body;
    std::string path;
    std::uint64_t max_bytes{0};

    std::ofstream file;
    std::uint64_t written{0};
    bool started{false};

    asio::any_io_executor get_executor() const noexcept { return body.get_executor(); }

    template <typename Self> void operator()(Self &self, error_code ec = {}, ReadResult chunk = {}) {
        if (!started) {
            started = true;
            file.open(path, std::ios::binary | std::ios::trunc);
            if (!file) {
                self.complete(boost::system::errc::make_error_code(boost::system::errc::no_such_file_or_directory), 0);
                return;
            }
            body.async_read(std::move(self));
            return;
        }
        if (ec) {
            self.complete(ec, written);
            return;
        }
        if (chunk.eof) {
            file.flush();
            if (!file) {
                self.complete(boost::system::errc::make_error_code(boost::system::errc::io_error), written);
                return;
            }
            self.complete(error_code{}, written);
            return;
        }
        written += chunk.data.size();
        if (max_bytes != 0 && written > max_bytes) {
            self.complete(boost::system::errc::make_error_code(boost::system::errc::file_too_large), written);
            return;
        }
        file.write(chunk.data.data(), static_cast<std::streamsize>(chunk.data.size()));
        if (!file) {
            self.complete(boost::system::errc::make_error_code(boost::system::errc::io_error), written);
            return;
        }
        body.async_read(std::move(self));
    }
};

} // namespace detail

// Reads `body` to EOF, writing each chunk to `path` (created/truncated).
// `max_bytes` (0 = no cap) is enforced while streaming, so an oversized upload
// is rejected before the disk fills.
template <typename Token>
auto async_read_body_to_file(Body &body, std::string path, std::uint64_t max_bytes, Token &&token) {
    return asio::async_compose<Token, void(error_code, std::uint64_t)>(
        detail::ReadBodyToFileOp{body, std::move(path), max_bytes}, token, body.get_executor());
}

template <typename Token> auto async_read_body_to_file(Body &body, std::string path, Token &&token) {
    return async_read_body_to_file(body, std::move(path), 0, std::forward<Token>(token));
}

} // namespace simple_http
