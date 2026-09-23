#pragma once

// HTTP/1.x request-head parser — a beast-free, incremental, byte-level parser.
//
// The parsing approach follows paozhu's httpparse (vendor/httpserver/
// http_parse.cpp): scan the request line for method / target / version, then
// split each header line on the first ':' with leading-whitespace folding and a
// lowercased field name. paozhu wrote everything into its httppeer god-object
// and also decoded the query string; here the parser stays minimal and
// framework-free — it fills a small ParsedHead (Method + target + Version +
// Headers) and leaves body framing to the caller. It depends only on the
// standard library and simple_http core types (no Asio, no Beast).
//
// Usage: append received bytes with feed(); call parse_head(). While it returns
// NeedMore, keep reading and feeding. On Done, head() holds the parsed request
// line + headers and consumed() bytes have been taken from the buffer (the
// remainder is the start of the body). On Error, error() explains why.

#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "../../core/http_method.h"
#include "../../core/types.h"
#include "../../proto/headers.h"

namespace simple_http {

struct ParsedHead {
    Method method = Method::Unknown;
    std::string method_token;  // raw token, so unknown/extension methods survive
    std::string target;
    Version version = Version::Http11;
    Headers headers;
};

class H1Parser {
  public:
    enum class State { NeedMore, Done, Error };

    // Appends freshly-read bytes to the internal buffer.
    void feed(std::string_view bytes) { m_buf.append(bytes); }
    void feed(const std::byte* data, std::size_t n) {
        m_buf.append(reinterpret_cast<const char*>(data), n);
    }

    // Attempts to parse a complete request head (up to and including the blank
    // line CRLFCRLF). Idempotent while NeedMore.
    State parse_head() {
        // Locate the end of the header block.
        std::size_t end = m_buf.find("\r\n\r\n");
        std::size_t sep = 4;
        if (end == std::string::npos) {
            // Tolerate bare-LF line endings (some minimal clients / tests).
            end = m_buf.find("\n\n");
            sep = 2;
            if (end == std::string::npos) {
                return State::NeedMore;
            }
        }

        std::string_view block{m_buf.data(), end};
        if (!parse_block(block)) {
            return State::Error;
        }
        m_consumed = end + sep;
        return State::Done;
    }

    const ParsedHead& head() const { return m_head; }
    std::size_t consumed() const { return m_consumed; }
    unsigned int error() const { return m_error; }

    // Bytes currently buffered while still parsing the head — used by the engine
    // to bound the request head size (Slowloris / oversized-header defense).
    std::size_t buffered() const { return m_buf.size(); }

    // The bytes remaining after the parsed head (the beginning of the body).
    std::string_view remainder() const {
        return std::string_view{m_buf}.substr(m_consumed);
    }

    // Resets so the same parser can serve the next pipelined request. Drops the
    // consumed head and keeps any leftover bytes.
    void reset_after_head() {
        m_buf.erase(0, m_consumed);
        m_consumed = 0;
        m_head = ParsedHead{};
        m_error = 0;
    }

  private:
    bool parse_block(std::string_view block) {
        std::size_t pos = 0;
        // --- request line ---
        std::string_view line = next_line(block, pos);
        if (!parse_request_line(line)) {
            return false;
        }
        // --- header fields ---
        while (pos < block.size()) {
            std::string_view hline = next_line(block, pos);
            if (hline.empty()) {
                continue;  // defensive; the block excludes the terminating blank line
            }
            if (!parse_header_line(hline)) {
                return false;
            }
        }
        return true;
    }

    // Returns the next CRLF/LF-delimited line and advances pos past the newline.
    static std::string_view next_line(std::string_view block, std::size_t& pos) {
        std::size_t nl = block.find('\n', pos);
        std::size_t line_end = (nl == std::string_view::npos) ? block.size() : nl;
        std::size_t raw_end = line_end;
        if (raw_end > pos && block[raw_end - 1] == '\r') {
            raw_end -= 1;  // strip trailing CR
        }
        std::string_view line = block.substr(pos, raw_end - pos);
        pos = (nl == std::string_view::npos) ? block.size() : nl + 1;
        return line;
    }

    bool parse_request_line(std::string_view line) {
        // METHOD SP request-target SP HTTP-version
        std::size_t sp1 = line.find(' ');
        if (sp1 == std::string_view::npos) {
            m_error = 40001;
            return false;
        }
        std::size_t sp2 = line.find(' ', sp1 + 1);
        if (sp2 == std::string_view::npos) {
            m_error = 40001;
            return false;
        }
        std::string_view method_tok = line.substr(0, sp1);
        std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
        std::string_view version = line.substr(sp2 + 1);

        if (method_tok.empty() || target.empty()) {
            m_error = 40002;
            return false;
        }

        m_head.method_token.assign(method_tok);
        m_head.method = method_from_string(method_tok);
        m_head.target.assign(target);

        if (version == "HTTP/1.1") {
            m_head.version = Version::Http11;
        } else if (version == "HTTP/1.0") {
            m_head.version = Version::Http1;
        } else {
            m_error = 40005;
            return false;
        }
        return true;
    }

    // Header field parsing mirrors paozhu process_header_line: split on the
    // first ':', skip leading spaces in the value, lowercase the field name.
    bool parse_header_line(std::string_view line) {
        std::size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            m_error = 40003;
            return false;
        }
        std::string_view key = line.substr(0, colon);
        std::size_t vstart = colon + 1;
        while (vstart < line.size() && (line[vstart] == ' ' || line[vstart] == '\t')) {
            ++vstart;
        }
        std::string_view value = line.substr(vstart);
        // Trim trailing optional whitespace (OWS) from the value.
        std::size_t vend = value.size();
        while (vend > 0 && (value[vend - 1] == ' ' || value[vend - 1] == '\t')) {
            --vend;
        }
        value = value.substr(0, vend);

        if (key.size() > 200) {  // sanity bound on field-name length
            m_error = 40004;
            return false;
        }
        // Headers::add lowercases the name for us.
        m_head.headers.add(std::string{key}, std::string{value});
        return true;
    }

    std::string m_buf;
    std::size_t m_consumed = 0;
    ParsedHead m_head;
    unsigned int m_error = 0;
};

}  // namespace simple_http
