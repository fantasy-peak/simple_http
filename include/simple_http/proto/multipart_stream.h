#pragma once

// Incremental multipart/form-data parser. Unlike parse_multipart (which buffers
// the whole body), this one delivers part bodies as they arrive, so a file part
// can be written straight to disk and a large upload never sits in memory.
//
//   MultipartStreamCallbacks cb;
//   cb.on_part_begin = [&](const MultipartPart &p) {
//       current = std::make_unique<std::ofstream>(dir / sanitize(p.filename));
//       return true;
//   };
//   cb.on_part_data = [&](std::string_view d) { current->write(d.data(), d.size()); return true; };
//   cb.on_part_end = [&] { current->close(); };
//   co_await read_multipart_stream(req->body(), boundary, cb);
//
// Boundaries split across reads are handled; the parser keeps only the tail
// that could be the start of a boundary.

#include <cstddef>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include "../core/types.h"
#include "body.h"
#include "multipart.h"

namespace simple_http {

struct MultipartStreamCallbacks {
    // Called once per part, after its head was parsed. Return false to abort.
    std::function<bool(const MultipartPart &part)> on_part_begin;
    // Called zero or more times with body bytes. Return false to abort.
    std::function<bool(std::string_view data)> on_part_data;
    // Called when the part's body ends.
    std::function<void()> on_part_end;
};

class MultipartStreamParser {
  public:
    explicit MultipartStreamParser(std::string boundary, std::size_t max_part_bytes = 16 * 1024 * 1024,
                                   std::size_t max_parts = 100)
        : m_delim("--" + std::move(boundary)), m_break("\r\n" + m_delim), m_max_part_bytes(max_part_bytes),
          m_max_parts(max_parts) {}

    // Feeds bytes; invokes callbacks. Returns false on malformed input or when a
    // callback aborts.
    bool feed(std::string_view bytes, MultipartStreamCallbacks &cb) {
        m_buf.append(bytes);
        return pump(cb, /*eof=*/false);
    }

    // Finalizes at end-of-body. Returns false if the closing delimiter was never
    // seen (a truncated upload).
    bool finish(MultipartStreamCallbacks &cb) {
        if (!pump(cb, /*eof=*/true)) {
            return false;
        }
        return m_phase == Phase::Done;
    }

    [[nodiscard]] std::size_t parts() const noexcept { return m_parts; }

  private:
    enum class Phase { Start, Headers, Body, AfterBody, Done, Error };

    bool pump(MultipartStreamCallbacks &cb, bool eof) {
        for (;;) {
            const std::size_t before = m_buf.size();
            switch (m_phase) {
            case Phase::Start:
                if (!pump_start(eof)) {
                    return m_phase != Phase::Error;
                }
                break;
            case Phase::Headers:
                if (!pump_headers(cb, eof)) {
                    return m_phase != Phase::Error;
                }
                break;
            case Phase::Body:
                if (!pump_body(cb, eof)) {
                    return m_phase != Phase::Error;
                }
                break;
            case Phase::AfterBody:
                if (!pump_after_body(eof)) {
                    return m_phase != Phase::Error;
                }
                break;
            case Phase::Done:
                m_buf.clear(); // ignore epilogue
                return true;
            case Phase::Error:
                return false;
            }
            if (m_phase == Phase::Error) {
                return false;
            }
            if (m_buf.size() == before && !eof) {
                break; // need more bytes
            }
            if (eof && m_buf.size() == before) {
                break;
            }
        }
        return m_phase != Phase::Error;
    }

    bool pump_start(bool eof) {
        (void)eof;
        // RFC 7578 §4.1: an optional CRLF may precede the first boundary.
        if (m_buf.size() >= 2 && m_buf.compare(0, 2, "\r\n") == 0) {
            m_buf.erase(0, 2);
        }
        if (m_buf.size() < m_delim.size()) {
            return false;
        }
        if (m_buf.compare(0, m_delim.size(), m_delim) != 0) {
            m_phase = Phase::Error;
            return false;
        }
        m_buf.erase(0, m_delim.size());
        m_phase = Phase::AfterBody; // shared delimiter-tail handling
        return true;
    }

    bool pump_headers(MultipartStreamCallbacks &cb, bool eof) {
        (void)eof; // an incomplete head is caught by finish()'s Done check
        const auto head_end = m_buf.find("\r\n\r\n");
        if (head_end == std::string::npos) {
            if (m_buf.size() > 64 * 1024) {
                m_phase = Phase::Error;
            }
            return false;
        }
        MultipartPart part;
        multipart_detail::parse_part_head(std::string_view{m_buf}.substr(0, head_end), part);
        m_buf.erase(0, head_end + 4);
        if (m_parts >= m_max_parts) {
            m_phase = Phase::Error;
            return false;
        }
        ++m_parts;
        m_part_bytes = 0;
        if (cb.on_part_begin && !cb.on_part_begin(part)) {
            m_phase = Phase::Error;
            return false;
        }
        m_phase = Phase::Body;
        return true;
    }

    bool pump_body(MultipartStreamCallbacks &cb, bool eof) {
        const auto at = m_buf.find(m_break);
        if (at != std::string::npos) {
            if (at > 0) {
                m_part_bytes += at;
                if (m_part_bytes > m_max_part_bytes) {
                    m_phase = Phase::Error;
                    return false;
                }
                if (cb.on_part_data && !cb.on_part_data(std::string_view{m_buf}.substr(0, at))) {
                    m_phase = Phase::Error;
                    return false;
                }
            }
            m_buf.erase(0, at + m_break.size());
            if (cb.on_part_end) {
                cb.on_part_end();
            }
            m_phase = Phase::AfterBody;
            return true;
        }
        // No complete boundary: emit everything that cannot be its prefix and
        // keep the tail (which may be a split "\r\n--boundary").
        const std::size_t keep = m_break.size() - 1;
        if (m_buf.size() > keep) {
            const std::size_t emit = m_buf.size() - keep;
            m_part_bytes += emit;
            if (m_part_bytes > m_max_part_bytes) {
                m_phase = Phase::Error;
                return false;
            }
            if (cb.on_part_data && !cb.on_part_data(std::string_view{m_buf}.substr(0, emit))) {
                m_phase = Phase::Error;
                return false;
            }
            m_buf.erase(0, emit);
            return true;
        }
        if (eof) {
            m_phase = Phase::Error; // truncated part
        }
        return false;
    }

    bool pump_after_body(bool eof) {
        if (m_buf.size() < 2) {
            if (eof) {
                m_phase = Phase::Error;
            }
            return false;
        }
        if (m_buf.compare(0, 2, "--") == 0) {
            m_buf.erase(0, 2);
            m_phase = Phase::Done;
            return true;
        }
        if (m_buf.compare(0, 2, "\r\n") == 0) {
            m_buf.erase(0, 2);
            m_phase = Phase::Headers;
            return true;
        }
        m_phase = Phase::Error;
        return false;
    }

    std::string m_delim; // "--boundary"
    std::string m_break; // "\r\n--boundary"
    std::string m_buf;
    std::size_t m_max_part_bytes;
    std::size_t m_max_parts;
    std::size_t m_parts{0};
    std::size_t m_part_bytes{0};
    Phase m_phase{Phase::Start};
};

// Reads a request body through an incremental parser. Returns an error when the
// body is malformed or a callback aborts.
inline asio::awaitable<std::expected<void, error_code>>
read_multipart_stream(Body &body, std::string boundary, MultipartStreamCallbacks cb,
                      std::size_t max_part_bytes = 16 * 1024 * 1024, std::size_t max_parts = 100) {
    MultipartStreamParser parser(std::move(boundary), max_part_bytes, max_parts);
    for (;;) {
        auto chunk = co_await body.read();
        if (!chunk) {
            co_return std::unexpected{chunk.error()};
        }
        if (chunk->eof) {
            break;
        }
        if (!parser.feed(chunk->data, cb)) {
            co_return std::unexpected{make_error_code(asio::error::invalid_argument)};
        }
    }
    if (!parser.finish(cb)) {
        co_return std::unexpected{make_error_code(asio::error::invalid_argument)};
    }
    co_return std::expected<void, error_code>{};
}

} // namespace simple_http
