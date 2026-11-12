#pragma once

// Streaming multipart/form-data upload for the client (C++17 token-only port).
//
//   client.async_open_multipart(url, opts, [](error_code, shared_ptr<MultipartStreamWriter> w) {
//       w->async_field("title", "hello", ...);
//       w->async_file("upload", "notes.txt", "./notes.txt", ...);
//       w->async_finish(...);
//   });
//
// Each part is written as it is added; file parts are copied from disk in
// fixed-size chunks. File I/O is synchronous (std::ifstream), matching
// file_response.h; a slow filesystem blocks the client executor.

#include <cstddef>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/system/error_code.hpp>

#include "../core/mime.h"
#include "../proto/headers.h"
#include "../proto/multipart.h"
#include "http.h"

namespace simple_http {

// `spec.method` defaults to POST; the writer sets Content-Type itself.
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

using MultipartDone = asio::any_completion_handler<void(error_code)>;

// Reads `path` in chunks and writes each as a Stream chunk.
inline void stream_file_step(std::shared_ptr<Stream> stream, std::shared_ptr<std::ifstream> in,
                             std::shared_ptr<std::string> buffer, std::size_t chunk_bytes,
                             std::shared_ptr<MultipartDone> done) {
    in->read(buffer->data(), static_cast<std::streamsize>(chunk_bytes));
    const auto read = in->gcount();
    if (read > 0) {
        auto self = stream;
        stream->async_write(std::string{buffer->data(), static_cast<std::size_t>(read)},
                            [stream, in, buffer, chunk_bytes, done](error_code ec) mutable {
                                if (ec) {
                                    auto callback = std::move(*done);
                                    std::move(callback)(ec);
                                    return;
                                }
                                stream_file_step(std::move(stream), std::move(in), std::move(buffer), chunk_bytes,
                                                 std::move(done));
                            });
        return;
    }
    // A clean EOF sets only eofbit; fail() covers failbit too, so a read that
    // stopped for any other reason is an error rather than a silent success.
    error_code result;
    if (in->fail())
        result = make_error_code(boost::system::errc::io_error);
    auto callback = std::move(*done);
    std::move(callback)(result);
}

} // namespace detail

template <typename Token>
auto async_stream_file_body(std::shared_ptr<Stream> stream, std::string path, std::size_t chunk_bytes, Token &&token) {
    return asio::async_initiate<Token, void(error_code)>(
        [stream = std::move(stream), path = std::move(path), chunk_bytes](auto handler) mutable {
            auto done = std::make_shared<detail::MultipartDone>(std::move(handler));
            auto in = std::make_shared<std::ifstream>(path, std::ios::binary);
            if (!*in) {
                auto callback = std::move(*done);
                std::move(callback)(make_error_code(boost::system::errc::no_such_file_or_directory));
                return;
            }
            const std::size_t chunk = chunk_bytes == 0 ? 64 * 1024 : chunk_bytes;
            detail::stream_file_step(std::move(stream), std::move(in), std::make_shared<std::string>(chunk, '\0'),
                                     chunk, std::move(done));
        },
        token);
}

class MultipartStreamWriter : public std::enable_shared_from_this<MultipartStreamWriter> {
  public:
    template <typename Token>
    static auto async_open(Client &client, std::string url, MultipartStreamOptions options, Token &&token) {
        return asio::async_initiate<Token, void(error_code, std::shared_ptr<MultipartStreamWriter>)>(
            [&client, url = std::move(url), options = std::move(options)](auto handler) mutable {
                std::string boundary = std::move(options.boundary);
                if (boundary.empty()) {
                    MultipartForm generated;
                    boundary = generated.boundary();
                }
                StreamSpec spec = std::move(options.spec);
                spec.headers.erase(field::content_type);
                spec.headers.add_lower(std::string{field::content_type}, "multipart/form-data; boundary=" + boundary);
                client.async_open_stream(
                    std::move(url), std::move(spec), 0,
                    [boundary = std::move(boundary), chunk = options.file_chunk_bytes,
                     handler = std::move(handler)](error_code ec, std::shared_ptr<Stream> stream) mutable {
                        if (ec) {
                            std::move(handler)(ec, nullptr);
                            return;
                        }
                        std::move(handler)(error_code{}, create(std::move(stream), std::move(boundary), chunk));
                    });
            },
            token);
    }

    MultipartStreamWriter(MultipartStreamWriter &&) noexcept = default;
    MultipartStreamWriter &operator=(MultipartStreamWriter &&) noexcept = default;
    MultipartStreamWriter(const MultipartStreamWriter &) = delete;
    MultipartStreamWriter &operator=(const MultipartStreamWriter &) = delete;

    template <typename Token> auto async_field(std::string name, std::string value, Token &&token) {
        auto self = shared_from_this();
        return asio::async_initiate<Token, void(error_code)>(
            [self, name = std::move(name), value = std::move(value)](auto handler) mutable {
                auto done = std::make_shared<detail::MultipartDone>(std::move(handler));
                self->write_part_head(name, {}, {}, [self, value = std::move(value), done](error_code ec) mutable {
                    if (ec) {
                        auto callback = std::move(*done);
                        std::move(callback)(ec);
                        return;
                    }
                    self->m_stream->async_write(std::move(value), [done](error_code wec) mutable {
                        auto callback = std::move(*done);
                        std::move(callback)(wec);
                    });
                });
            },
            token);
    }

    template <typename Token>
    auto async_file_data(std::string name, std::string filename, std::string content_type, std::string data,
                         Token &&token) {
        auto self = shared_from_this();
        return asio::async_initiate<Token, void(error_code)>(
            [self, name = std::move(name), filename = std::move(filename), content_type = std::move(content_type),
             data = std::move(data)](auto handler) mutable {
                auto done = std::make_shared<detail::MultipartDone>(std::move(handler));
                self->write_part_head(name, filename, content_type,
                                      [self, data = std::move(data), done](error_code ec) mutable {
                                          if (ec) {
                                              auto callback = std::move(*done);
                                              std::move(callback)(ec);
                                              return;
                                          }
                                          self->m_stream->async_write(std::move(data), [done](error_code wec) mutable {
                                              auto callback = std::move(*done);
                                              std::move(callback)(wec);
                                          });
                                      });
            },
            token);
    }

    template <typename Token>
    auto async_file_data(std::string name, std::string filename, std::string data, Token &&token) {
        std::string content_type{mime::by_extension(filename)};
        return async_file_data(std::move(name), std::move(filename), std::move(content_type), std::move(data),
                               std::forward<Token>(token));
    }

    template <typename Token>
    auto async_file(std::string name, std::string filename, std::string content_type, std::string path, Token &&token) {
        auto self = shared_from_this();
        return asio::async_initiate<Token, void(error_code)>(
            [self, name = std::move(name), filename = std::move(filename), content_type = std::move(content_type),
             path = std::move(path)](auto handler) mutable {
                auto done = std::make_shared<detail::MultipartDone>(std::move(handler));
                self->write_part_head(name, filename, content_type,
                                      [self, path = std::move(path), done](error_code ec) mutable {
                                          if (ec) {
                                              auto callback = std::move(*done);
                                              std::move(callback)(ec);
                                              return;
                                          }
                                          auto stream = self->m_stream;
                                          async_stream_file_body(stream, std::move(path), self->m_chunk_bytes,
                                                                 [done](error_code fec) mutable {
                                                                     auto callback = std::move(*done);
                                                                     std::move(callback)(fec);
                                                                 });
                                      });
            },
            token);
    }

    template <typename Token> auto async_file(std::string name, std::string filename, std::string path, Token &&token) {
        std::string content_type{mime::by_extension(filename)};
        return async_file(std::move(name), std::move(filename), std::move(content_type), std::move(path),
                          std::forward<Token>(token));
    }

    template <typename Token> auto async_finish(Token &&token) {
        auto self = shared_from_this();
        return asio::async_initiate<Token, void(error_code)>(
            [self](auto handler) mutable {
                if (self->m_finished) {
                    std::move(handler)(error_code{});
                    return;
                }
                std::string out;
                if (self->m_first) {
                    out = "--" + self->m_boundary + "--\r\n";
                } else {
                    out = "\r\n--" + self->m_boundary + "--\r\n";
                }
                self->m_finished = true;
                auto done = std::make_shared<detail::MultipartDone>(std::move(handler));
                auto stream = self->m_stream;
                stream->async_write(std::move(out), [stream, done](error_code ec) mutable {
                    if (ec) {
                        auto callback = std::move(*done);
                        std::move(callback)(ec);
                        return;
                    }
                    stream->async_finish(std::string{}, [done](error_code fec) mutable {
                        auto callback = std::move(*done);
                        std::move(callback)(fec);
                    });
                });
            },
            token);
    }

    std::shared_ptr<Stream> stream() noexcept { return m_stream; }
    const std::string &boundary() const noexcept { return m_boundary; }

  private:
    static std::shared_ptr<MultipartStreamWriter> create(std::shared_ptr<Stream> stream, std::string boundary,
                                                         std::size_t chunk_bytes) {
        return std::shared_ptr<MultipartStreamWriter>(
            new MultipartStreamWriter(std::move(stream), std::move(boundary), chunk_bytes));
    }

    MultipartStreamWriter(std::shared_ptr<Stream> stream, std::string boundary, std::size_t chunk_bytes)
        : m_stream(std::move(stream)), m_boundary(std::move(boundary)), m_chunk_bytes(chunk_bytes) {}

    void write_part_head(std::string_view name, std::string_view filename, std::string_view content_type,
                         std::function<void(error_code)> done) {
        if (m_finished) {
            done(make_error_code(boost::system::errc::operation_not_permitted));
            return;
        }
        if (!detail::valid_multipart_param(name) || !detail::valid_multipart_param(filename) ||
            !detail::valid_multipart_param(content_type)) {
            done(make_error_code(boost::system::errc::invalid_argument));
            return;
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
        m_stream->async_write(std::move(out), std::move(done));
    }

    std::shared_ptr<Stream> m_stream;
    std::string m_boundary;
    std::size_t m_chunk_bytes{64 * 1024};
    bool m_first{true};
    bool m_finished{false};
};

} // namespace simple_http
