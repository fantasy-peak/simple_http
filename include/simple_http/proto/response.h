#pragma once

// Response: the user-facing, fluent, version-agnostic reply handle.
//
// Response holds a ResponseWriter and forwards to it. Because the writer is
// polymorphic and its write operations are awaitable, Response has zero
// protocol-version branching and its writes are safe from any thread (the
// writer hops onto the connection executor internally).
//
// Two usage modes:
//   one-shot:  co_await res.status(200).header("k","v").send("body");
//   streaming: co_await res.status(200).begin();
//              co_await res.write("chunk"); ...; co_await res.finish();

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <boost/asio/awaitable.hpp>

#include "../core/http_field.h"
#include "../core/http_status.h"
#include "../core/logging.h"
#include "../core/types.h"
#include "../core/version.h"
#include "headers.h"
#include "response_writer.h"

namespace simple_http {

namespace asio = boost::asio;

class Response {
  public:
    explicit Response(std::shared_ptr<ResponseWriter> writer) : m_writer(std::move(writer)) {}

    // --- fluent setters (return *this for chaining) ---
    Response& status(int code) {
        m_status = code;
        return *this;
    }
    Response& header(std::string_view name, std::string value) {
        // A field name or value carrying CR/LF/NUL would splice arbitrary bytes
        // into the head — response splitting, and this library's own reverse proxy
        // is exactly the kind of intermediary that turns it into a real attack.
        // HTTP/2 already refuses the whole stream for it; enforcing that only
        // there made the same handler code safe on one protocol and dangerous on
        // the other, so it is enforced once, here, at the boundary that writes.
        if (contains_ctl(name) || contains_ctl(value)) {
            SIMPLE_HTTP_ERROR_LOG("response field with CR/LF/NUL rejected");
            m_field_rejected = true;
            return *this;
        }
        m_headers.add(std::string{name}, std::move(value));
        return *this;
    }
    Response& content_type(std::string_view ct) { return header("content-type", std::string{ct}); }

    // --- one-shot ---
    [[nodiscard]] asio::awaitable<error_code> send(std::string body = {}) {
        if (m_field_rejected) {
            co_return make_error_code(asio::error::invalid_argument);
        }
        apply_defaults();
        co_return co_await m_writer->send(m_status, std::move(m_headers), std::move(body));
    }

    // Status + headers with no body and no body framing at all (204/304 and every
    // response to HEAD): the client sees the response end at the header block.
    [[nodiscard]] asio::awaitable<error_code> send_bodyless() {
        if (m_field_rejected) {
            co_return make_error_code(asio::error::invalid_argument);
        }
        apply_defaults();
        co_return co_await m_writer->send_bodyless(m_status, std::move(m_headers));
    }

    // --- streaming ---
    [[nodiscard]] asio::awaitable<error_code> begin() {
        if (m_field_rejected) {
            co_return make_error_code(asio::error::invalid_argument);
        }
        apply_defaults();
        co_return co_await m_writer->send_headers(m_status, std::move(m_headers));
    }
    [[nodiscard]] asio::awaitable<error_code> write(std::string data) { return m_writer->send_chunk(std::move(data)); }
    [[nodiscard]] asio::awaitable<error_code> finish(std::string data = {}) {
        return m_writer->send_last(std::move(data));
    }

    // --- connection state ---
    // Both hop onto the connection executor inside the writer, like the writes.
    [[nodiscard]] asio::awaitable<bool> connected() const { return m_writer->connected(); }
    [[nodiscard]] asio::awaitable<void> close() { return m_writer->close(); }
    Version version() const { return m_writer->version(); }

    ResponseWriter& writer() { return *m_writer; }

  private:
    // Fill in Server and Content-Type headers if the handler did not set them.
    void apply_defaults() {
        if (!m_headers.contains("content-type")) {
            m_headers.add_lower("content-type", "text/plain");
        }
        if (!m_headers.contains("server")) {
            m_headers.add_lower("server", std::string{server_version});
        }
    }

    std::shared_ptr<ResponseWriter> m_writer;
    int m_status{200};
    Headers m_headers;
    // Set when header() refused a field; the response is then not sent at all.
    // Emitting the rest would put a head on the wire that the handler did not
    // intend — and the whole point of rejecting at header() was to not do that.
    bool m_field_rejected{false};
};

}  // namespace simple_http
