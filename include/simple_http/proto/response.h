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
        m_headers.add(std::string{name}, std::move(value));
        return *this;
    }
    Response& content_type(std::string_view ct) { return header("content-type", std::string{ct}); }

    // --- one-shot ---
    [[nodiscard]] asio::awaitable<error_code> send(std::string body = {}) {
        apply_defaults();
        return m_writer->send(m_status, std::move(m_headers), std::move(body));
    }

    // Status + headers with no body and no body framing at all (204/304 and every
    // response to HEAD): the client sees the response end at the header block.
    [[nodiscard]] asio::awaitable<error_code> send_bodyless() {
        apply_defaults();
        return m_writer->send_bodyless(m_status, std::move(m_headers));
    }

    // --- streaming ---
    [[nodiscard]] asio::awaitable<error_code> begin() {
        apply_defaults();
        return m_writer->send_headers(m_status, std::move(m_headers));
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
};

}  // namespace simple_http
