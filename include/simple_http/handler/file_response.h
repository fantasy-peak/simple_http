#pragma once

// Streaming file responses (C++17 token-only port): the file is read in
// bounded chunks and written as the response body, so a large file never sits
// in user-space memory. This is the serving primitive; Range/conditional
// request policy (ETag, Last-Modified, If-Range) stays in static_files.h, which
// also decides what happens for a missing path.
//
//   simple_http::async_send_file(*res, "assets/app.js", simple_http::finish_with(std::move(done)));
//   simple_http::async_send_file(*res, path, {.content_type = "text/plain"}, token);
//
// Completion signature: void(error_code). The caller maps the error onto a
// status (typically 404 or 500); headers are only applied after the file was
// successfully stat'ed, so a failed call leaves the response untouched.
//
// The file I/O is synchronous (std::ifstream). Boost 1.92's asio::stream_file
// only has a backend under io_uring/IOCP, so it cannot be used on a plain epoll
// build. A 64 KiB local read is short, but a slow filesystem (NFS, FUSE) will
// block the connection's executor for the duration of that read; the reverse
// proxy's upstream I/O is unaffected because it is a different connection (and
// usually a different executor in the pool).

#include <array>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/compose.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "../core/http_field.h"
#include "../core/mime.h"
#include "../core/types.h"
#include "../proto/response.h"

namespace simple_http {

namespace asio = boost::asio;

struct SendFileOptions {
    // Empty = derive from the file extension via mime::by_extension().
    std::string content_type;
    // Refuse files larger than this (0 = no cap). Checked before opening.
    std::uint64_t max_bytes{0};
    // Send Content-Length before the body. Necessary for the one-shot framing a
    // HEAD response needs; a streamed GET could omit it, but keeping it lets
    // clients show progress and preserves the HEAD/GET symmetry.
    bool set_content_length{true};
};

namespace detail {

struct SendFileOp {
    ResponseWriter &response;
    std::string path;
    SendFileOptions options;

    std::ifstream file;
    std::array<char, 64 * 1024> buffer{};
    error_code pre;
    std::uintmax_t size{0};
    int stage{0};

    asio::any_io_executor get_executor() const noexcept { return response.writer().get_executor(); }

    // Validates the file without touching the response: headers written here
    // would survive a later open() failure and describe a body the caller is
    // about to replace with a 404/500.
    error_code prepare() {
        std::error_code stat_ec;
        size = std::filesystem::file_size(path, stat_ec);
        if (stat_ec) {
            return boost::system::errc::make_error_code(boost::system::errc::no_such_file_or_directory);
        }
        if (options.max_bytes != 0 && size > options.max_bytes) {
            return boost::system::errc::make_error_code(boost::system::errc::file_too_large);
        }
        return {};
    }

    void apply_headers() {
        const auto content_type =
            options.content_type.empty() ? std::string{mime::by_extension(path)} : options.content_type;
        response.content_type(content_type);
        if (options.set_content_length) {
            response.header(field::content_length, std::to_string(size));
        }
    }

    template <typename Self> void read_and_write(Self &self) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto n = file.gcount();
        if (n <= 0) {
            if (file.bad()) {
                self.complete(boost::system::errc::make_error_code(boost::system::errc::io_error));
                return;
            }
            stage = 3;
            response.async_finish(std::move(self));
            return;
        }
        stage = 2;
        response.async_write(std::string(buffer.data(), static_cast<std::size_t>(n)), std::move(self));
    }

    template <typename Self> void operator()(Self &self, error_code ec = {}) {
        switch (stage) {
        case 0:
            pre = prepare();
            if (pre) {
                self.complete(pre);
                return;
            }
            // Open before writing any head field: a failure here must leave the
            // response untouched so the caller can still send its own 404/500.
            file.open(path, std::ios::binary);
            if (!file) {
                self.complete(boost::system::errc::make_error_code(boost::system::errc::no_such_file_or_directory));
                return;
            }
            apply_headers();
            if (response.writer().head_request()) {
                stage = 4;
                response.async_send_bodyless(std::move(self));
                return;
            }
            stage = 1;
            response.async_begin(std::move(self));
            return;
        case 1:
            if (ec) {
                self.complete(ec);
                return;
            }
            read_and_write(self);
            return;
        case 2:
            if (ec) {
                self.complete(ec);
                return;
            }
            read_and_write(self);
            return;
        case 3:
        case 4:
            self.complete(ec);
            return;
        default:
            self.complete(boost::system::errc::make_error_code(boost::system::errc::operation_canceled));
            return;
        }
    }
};

} // namespace detail

// Streams `path` as the response body. Completion signature: void(error_code).
template <typename Token>
auto async_send_file(ResponseWriter &response, std::string path, SendFileOptions options, Token &&token) {
    auto executor = response.writer().get_executor();
    return asio::async_compose<Token, void(error_code)>(
        detail::SendFileOp{response, std::move(path), std::move(options)}, token, executor);
}

template <typename Token> auto async_send_file(ResponseWriter &response, std::string path, Token &&token) {
    return async_send_file(response, std::move(path), SendFileOptions{}, std::forward<Token>(token));
}

} // namespace simple_http
