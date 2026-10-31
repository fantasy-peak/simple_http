#pragma once

// Streaming uploads: consume a request Body directly into a file, in bounded
// chunks. The whole body is never held in memory.
//
//   auto written = co_await simple_http::read_body_to_file(req->body(), path);
//   if (!written) co_await res->status(400).send("upload failed");
//
// This is the low-level primitive; multipart/form-data framing (part names,
// filenames) is handled by read_multipart_stream in proto/multipart_stream.h.
//
// Like send_file, the file I/O is synchronous (std::ofstream): Boost 1.92's
// asio::stream_file needs an io_uring/IOCP backend that a plain epoll build does
// not have. Each chunk is bounded, so the executor is blocked only for one write.

#include <boost/system/error_code.hpp>
#include <cstdint>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#include "../core/types.h"
#include "../proto/body.h"

namespace simple_http {

// Reads `body` to EOF, writing each chunk to `path` (created/truncated).
// Returns the number of bytes written. `max_bytes` (0 = no cap) is enforced
// while streaming, so an oversized upload is rejected before the disk fills.
inline asio::awaitable<std::expected<std::uint64_t, error_code>> read_body_to_file(Body &body, std::string path,
                                                                                   std::uint64_t max_bytes = 0) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    if (!file) {
        co_return std::unexpected{boost::system::errc::make_error_code(boost::system::errc::no_such_file_or_directory)};
    }

    std::uint64_t written = 0;
    for (;;) {
        auto chunk = co_await body.read();
        if (!chunk) {
            co_return std::unexpected{chunk.error()};
        }
        if (chunk->eof) {
            break;
        }
        written += chunk->data.size();
        if (max_bytes != 0 && written > max_bytes) {
            co_return std::unexpected{boost::system::errc::make_error_code(boost::system::errc::file_too_large)};
        }
        file.write(chunk->data.data(), static_cast<std::streamsize>(chunk->data.size()));
        if (!file) {
            co_return std::unexpected{boost::system::errc::make_error_code(boost::system::errc::io_error)};
        }
    }
    file.flush();
    if (!file) {
        co_return std::unexpected{boost::system::errc::make_error_code(boost::system::errc::io_error)};
    }
    co_return written;
}

} // namespace simple_http
