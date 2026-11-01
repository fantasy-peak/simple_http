#pragma once

// Streaming multipart/form-data upload for the client.
//
// MultipartForm (proto/multipart.h) serializes the whole body in memory, which
// is fine for small forms but not for file uploads. MultipartStreamWriter drives
// an open Stream instead: each part is written as it is added, and file parts
// are copied from disk in fixed-size chunks. The request body is streamed
// (chunked on HTTP/1.1, DATA frames on HTTP/2/3), so the server sees the first
// bytes before the last file has been read.
//
//     auto up = co_await MultipartStreamWriter::open(
//         client, "https://host/upload", {.boundary = "B"});
//     co_await up->field("title", "hello");
//     co_await up->file("upload", "notes.txt", "./notes.txt");
//     co_await up->finish();
//     auto head = co_await up->stream().read_head();
//
// File I/O is synchronous (std::ifstream), matching file_response.h: Boost
// 1.92's asio::stream_file needs io_uring/IOCP. A slow filesystem therefore
// blocks the client's executor while a chunk is read; use an explicit Stream +
// your own async reader if that matters.
//
// The writer does not apply redirects, cookies or retries: it uses
// Client::open_stream(), which is the explicit streaming path.

#include <cstddef>
#include <expected>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

#include <boost/system/error_code.hpp>

#include "../core/mime.h"
#include "../proto/headers.h"
#include "../proto/multipart.h" // MultipartForm (boundary generation)
#include "http.h"               // Client, Stream, StreamSpec

namespace simple_http {

// The options for one streaming multipart request. `spec.method` defaults to
// POST; the writer sets Content-Type itself (a caller-provided Content-Type is
// replaced, as RequestBuilder::multipart does).
struct MultipartStreamOptions {
    StreamSpec spec{.method = Method::Post};
    std::string boundary; // empty = generate a per-request boundary
    std::size_t file_chunk_bytes{64 * 1024};
};

namespace detail {

inline std::string quote_multipart_param(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

inline bool valid_multipart_param(std::string_view value) {
    return value.find_first_of("\r\n") == std::string_view::npos;
}

} // namespace detail

// Copies a file body into an open Stream in chunks. Returns an error_code from
// the file or the stream; the caller still owns finish().
inline asio::awaitable<error_code> stream_file_body(Stream &stream, const std::string &path,
                                                    std::size_t chunk_bytes = 64 * 1024) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        co_return make_error_code(boost::system::errc::no_such_file_or_directory);
    }
    if (chunk_bytes == 0) {
        chunk_bytes = 64 * 1024;
    }
    std::string buffer(chunk_bytes, '\0');
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto read = in.gcount();
        if (read > 0) {
            if (auto ec = co_await stream.write(std::string{buffer.data(), static_cast<std::size_t>(read)}); ec) {
                co_return ec;
            }
        }
    }
    if (in.bad()) {
        co_return make_error_code(boost::system::errc::io_error);
    }
    co_return error_code{};
}

// Writes one multipart/form-data body to a Stream. The stream is opened by
// open(); field()/file()/file_data() append parts, and finish() writes the
// closing delimiter and ends the request body.
class MultipartStreamWriter {
  public:
    static asio::awaitable<std::expected<MultipartStreamWriter, error_code>> open(Client &client, std::string url,
                                                                                  MultipartStreamOptions options = {}) {
        std::string boundary = std::move(options.boundary);
        if (boundary.empty()) {
            MultipartForm generated;
            boundary = generated.boundary();
        }

        StreamSpec spec = std::move(options.spec);
        spec.headers.erase(field::content_type);
        spec.headers.add_lower(std::string{field::content_type}, "multipart/form-data; boundary=" + boundary);
        auto opened = co_await client.open_stream(std::move(url), std::move(spec));
        if (!opened) {
            co_return std::unexpected{opened.error()};
        }
        co_return MultipartStreamWriter{std::move(*opened), std::move(boundary), options.file_chunk_bytes};
    }

    MultipartStreamWriter(MultipartStreamWriter &&) noexcept = default;
    MultipartStreamWriter &operator=(MultipartStreamWriter &&) noexcept = default;
    MultipartStreamWriter(const MultipartStreamWriter &) = delete;
    MultipartStreamWriter &operator=(const MultipartStreamWriter &) = delete;

    // A text field part.
    asio::awaitable<error_code> field(std::string name, std::string value) {
        if (auto ec = co_await write_part_head(name, {}, {}); ec) {
            co_return ec;
        }
        co_return co_await m_stream.write(std::move(value));
    }

    // A file part whose data is already in memory.
    asio::awaitable<error_code> file_data(std::string name, std::string filename, std::string data) {
        std::string content_type{mime::by_extension(filename)};
        return file_data(std::move(name), std::move(filename), std::move(content_type), std::move(data));
    }

    // A file part with an explicit content type. "" omits the per-part header.
    asio::awaitable<error_code> file_data(std::string name, std::string filename, std::string content_type,
                                          std::string data) {
        if (auto ec = co_await write_part_head(name, filename, content_type); ec) {
            co_return ec;
        }
        co_return co_await m_stream.write(std::move(data));
    }

    // A file part streamed from disk, with the content type inferred from the
    // filename extension.
    asio::awaitable<error_code> file(std::string name, std::string filename, std::string path) {
        std::string content_type{mime::by_extension(filename)};
        return file(std::move(name), std::move(filename), std::move(content_type), std::move(path));
    }

    // A file part streamed from disk with an explicit content type.
    asio::awaitable<error_code> file(std::string name, std::string filename, std::string content_type,
                                     std::string path) {
        if (auto ec = co_await write_part_head(name, filename, content_type); ec) {
            co_return ec;
        }
        co_return co_await stream_file_body(m_stream, path, m_chunk_bytes);
    }

    // Writes the closing delimiter and ends the request body. Idempotent: a
    // second call is a no-op.
    asio::awaitable<error_code> finish() {
        if (m_finished) {
            co_return error_code{};
        }
        std::string out;
        if (m_first) {
            out = "--" + m_boundary + "--\r\n";
        } else {
            out = "\r\n--" + m_boundary + "--\r\n";
        }
        m_finished = true;
        if (auto ec = co_await m_stream.write(std::move(out)); ec) {
            co_return ec;
        }
        co_return co_await m_stream.finish();
    }

    Stream &stream() noexcept { return m_stream; }
    const std::string &boundary() const noexcept { return m_boundary; }

  private:
    MultipartStreamWriter(Stream stream, std::string boundary, std::size_t chunk_bytes)
        : m_stream(std::move(stream)), m_boundary(std::move(boundary)), m_chunk_bytes(chunk_bytes) {}

    asio::awaitable<error_code> write_part_head(std::string_view name, std::string_view filename,
                                                std::string_view content_type) {
        if (m_finished) {
            co_return make_error_code(boost::system::errc::operation_not_permitted);
        }
        if (!detail::valid_multipart_param(name) || !detail::valid_multipart_param(filename)) {
            co_return make_error_code(boost::system::errc::invalid_argument);
        }

        std::string out;
        if (m_first) {
            out = "--" + m_boundary + "\r\n";
            m_first = false;
        } else {
            out = "\r\n--" + m_boundary + "\r\n";
        }
        out += "Content-Disposition: form-data; name=\"";
        out += detail::quote_multipart_param(name);
        out.push_back('"');
        if (!filename.empty()) {
            out += "; filename=\"";
            out += detail::quote_multipart_param(filename);
            out.push_back('"');
        }
        out += "\r\n";
        if (!content_type.empty()) {
            out += "Content-Type: ";
            out += content_type;
            out += "\r\n";
        }
        out += "\r\n";
        co_return co_await m_stream.write(std::move(out));
    }

    Stream m_stream;
    std::string m_boundary;
    std::size_t m_chunk_bytes{64 * 1024};
    bool m_first{true};
    bool m_finished{false};
};

} // namespace simple_http
