#pragma once

// Streaming file responses: the file is read in bounded chunks and written as
// the response body, so a large file never sits in user-space memory. This is
// the serving primitive; Range/conditional-request policy (ETag, Last-Modified,
// If-Range) stays in static_files.h, which also decides what happens for a
// missing path.
//
//   co_await simple_http::send_file(*res, "assets/app.js");
//   co_await simple_http::send_file(*res, path, {.content_type = "text/plain"});
//
// The file I/O is synchronous (std::ifstream). Boost 1.92's asio::stream_file
// only has a backend under io_uring/IOCP, so it cannot be used on a plain epoll
// build. A 64 KiB local read is short, but a slow filesystem (NFS, FUSE) will
// block the connection's executor for the duration of that read; the reverse
// proxy's upstream I/O is unaffected because it is a different connection (and
// usually a different executor in the pool).

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#include "../core/http_field.h"
#include "../core/mime.h"
#include "../core/types.h"
#include "../proto/response.h"

namespace simple_http {

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

// Streams `path` as the response body. Returns an error_code for the caller to
// map onto a status (typically 404 or 500); headers are only applied after the
// file was successfully stat'ed, so a failed call leaves the response untouched.
inline asio::awaitable<error_code> send_file(ResponseWriter &res, std::string path, SendFileOptions opts = {}) {
    std::error_code stat_ec;
    const auto size = std::filesystem::file_size(path, stat_ec);
    if (stat_ec) {
        co_return boost::system::errc::make_error_code(boost::system::errc::no_such_file_or_directory);
    }
    if (opts.max_bytes != 0 && size > opts.max_bytes) {
        co_return boost::system::errc::make_error_code(boost::system::errc::file_too_large);
    }

    const auto content_type = opts.content_type.empty() ? std::string{mime::by_extension(path)} : opts.content_type;
    res.content_type(content_type);
    if (opts.set_content_length) {
        res.header(field::content_length, std::to_string(size));
    }

    if (res.writer().head_request()) {
        co_return co_await res.send_bodyless();
    }

    std::ifstream file{path, std::ios::binary};
    if (!file) {
        co_return boost::system::errc::make_error_code(boost::system::errc::no_such_file_or_directory);
    }

    if (auto ec = co_await res.begin(); ec) {
        co_return ec;
    }

    std::array<char, 64 * 1024> buffer;
    for (;;) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto n = file.gcount();
        if (n <= 0) {
            break;
        }
        if (auto ec = co_await res.write(std::string(buffer.data(), static_cast<std::size_t>(n))); ec) {
            co_return ec;
        }
    }
    if (file.bad()) {
        co_return boost::system::errc::make_error_code(boost::system::errc::io_error);
    }
    co_return co_await res.finish();
}

} // namespace simple_http
