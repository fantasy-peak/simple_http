#pragma once

// ResponseWriter: the user-facing, fluent, version-agnostic reply handle.
//
// ResponseWriter holds a ResponseSink and forwards to it. Because the sink is
// polymorphic and its write operations are awaitable, ResponseWriter has zero
// protocol-version branching and its writes are safe from any thread (the
// sink hops onto the connection executor internally).
//
// Two usage modes:
//   one-shot:  co_await res.status(200).header("k","v").send("body");
//   streaming: co_await res.status(200).begin();
//              co_await res.write("chunk"); ...; co_await res.finish();

#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "../core/http_field.h"
#include "../core/http_status.h"
#include "../core/logging.h"
#include "../core/types.h"
#include "../core/version.h"
#include "headers.h"
#include "response_writer.h"

namespace simple_http {

namespace asio = boost::asio;

class ResponseWriter {
  public:
    explicit ResponseWriter(std::shared_ptr<ResponseSink> writer) : m_writer(std::move(writer)) {}

    // --- fluent setters (return *this for chaining) ---
    ResponseWriter &status(int code) {
        m_status = code;
        return *this;
    }
    // The status code this response will be — or was — sent with. A middleware
    // reads it in its after-phase (after `co_await next(...)` resumed) to see
    // what the handler or the router's built-in replies ended on.
    int status() const { return m_status; }
    // Whether the response head has already gone out (send()/begin()/
    // send_bodyless() was called). A middleware uses this to decide whether it
    // can still substitute its own reply — before it starts it can, after it
    // starts the only honest action is close(). It is the same ResponseWriter
    // object all the way down the chain, so the flag the handler sets is what a
    // middleware observes.
    bool started() const { return m_started; }
    ResponseWriter &header(std::string_view name, std::string value) {
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

    // Replaces every field with `name` (the last set wins) instead of appending
    // a duplicate — the override a middleware-provided default (security
    // headers, CORS Vary) needs from a handler: `res->replace_header(field::x_frame_options, "SAMEORIGIN")`.
    // Like header(), a CR/LF/NUL in either part refuses the whole field.
    ResponseWriter &replace_header(std::string_view name, std::string value) {
        if (contains_ctl(name) || contains_ctl(value)) {
            SIMPLE_HTTP_ERROR_LOG("response field with CR/LF/NUL rejected");
            m_field_rejected = true;
            return *this;
        }
        m_headers.erase(name);
        m_headers.add(std::string{name}, std::move(value));
        return *this;
    }
    ResponseWriter &content_type(std::string_view ct) { return header("content-type", std::string{ct}); }

    // Builds and adds one Set-Cookie header (RFC 6265), returning *this for
    // chaining. A value containing characters the cookie grammar does not allow
    // (;, ,, space, quote, backslash) is quoted with "...", mirroring Go's
    // http.SetCookie. SameSite takes "Lax" / "Strict" / "None".
    ResponseWriter &set_cookie(std::string name, std::string value, std::chrono::seconds max_age = {},
                               std::string path = {}, std::string domain = {}, bool secure = false,
                               bool http_only = false, std::string same_site = {}) {
        std::string field_value = std::move(name);
        field_value.push_back('=');
        if (value.find_first_of(";, \"\\") != std::string::npos) {
            field_value.push_back('"');
            field_value += value;
            field_value.push_back('"');
        } else {
            field_value += value;
        }
        if (!domain.empty()) {
            field_value += "; Domain=";
            field_value += domain;
        }
        if (!path.empty()) {
            field_value += "; Path=";
            field_value += path;
        }
        if (max_age.count() > 0) {
            field_value += "; Max-Age=";
            field_value += std::to_string(max_age.count());
        }
        if (secure) {
            field_value += "; Secure";
        }
        if (http_only) {
            field_value += "; HttpOnly";
        }
        if (!same_site.empty()) {
            field_value += "; SameSite=";
            field_value += same_site;
        }
        return header(field::set_cookie, std::move(field_value));
    }

    // --- one-shot ---
    [[nodiscard]] asio::awaitable<error_code> send(std::string body = {}) {
        if (m_field_rejected) {
            co_return make_error_code(asio::error::invalid_argument);
        }
        apply_defaults();
        m_started = true;
        // A response carrying trailers cannot be one-shot: the fields follow the
        // body, so it is framed as a stream (chunked on HTTP/1.x) and closed by
        // the trailer section. `send()` on a trailer-less response keeps the
        // one-shot framing (a known Content-Length on HTTP/1.x).
        if (!m_trailers.empty()) {
            co_return co_await send_stream(std::move(body), std::move(m_trailers));
        }
        co_return co_await m_writer->send(m_status, std::move(m_headers), std::move(body));
    }

    // Status + headers with no body and no body framing at all (204/304 and every
    // response to HEAD): the client sees the response end at the header block.
    [[nodiscard]] asio::awaitable<error_code> send_bodyless() {
        if (m_field_rejected) {
            co_return make_error_code(asio::error::invalid_argument);
        }
        apply_defaults();
        m_started = true;
        co_return co_await m_writer->send_bodyless(m_status, std::move(m_headers));
    }

    // --- streaming ---
    [[nodiscard]] asio::awaitable<error_code> begin() {
        if (m_field_rejected) {
            co_return make_error_code(asio::error::invalid_argument);
        }
        apply_defaults();
        m_started = true;
        co_return co_await m_writer->send_headers(m_status, std::move(m_headers));
    }
    [[nodiscard]] asio::awaitable<error_code> write(std::string data) { return m_writer->send_chunk(std::move(data)); }
    [[nodiscard]] asio::awaitable<error_code> finish(std::string data = {}) {
        // Trailer fields accumulated with trailer() end the response instead of
        // the final body chunk, so a `finish()` after them still emits them. The
        // response head already went out in begin(), so any final body bytes go
        // out as an ordinary chunk and the trailer section closes the stream.
        if (!m_trailers.empty()) {
            if (!data.empty()) {
                if (auto ec = co_await m_writer->send_chunk(std::move(data)); ec) {
                    co_return ec;
                }
            }
            co_return co_await m_writer->send_trailers(std::move(m_trailers));
        }
        co_return co_await m_writer->send_last(std::move(data));
    }

    // Accumulates one trailer field (RFC 9110 §6.5), sent when the response ends
    // via finish() or finish_trailers(). Trailer fields must not be pseudo-headers
    // or framing headers; a CR/LF/NUL is refused exactly as it is for header().
    //   co_await res->status(200).begin();
    //   co_await res->write(chunk);
    //   res->trailer("server-timing", "db;dur=53");
    //   co_await res->finish();
    ResponseWriter &trailer(std::string_view name, std::string value) {
        if (contains_ctl(name) || contains_ctl(value)) {
            SIMPLE_HTTP_ERROR_LOG("response trailer with CR/LF/NUL rejected");
            m_field_rejected = true;
            return *this;
        }
        m_trailers.add(std::string{name}, std::move(value));
        return *this;
    }

    bool has_trailers() const { return !m_trailers.empty(); }

    // Ends a streamed response with the given trailing field section, replacing
    // the final finish(). Available even when no trailer() was accumulated.
    [[nodiscard]] asio::awaitable<error_code> finish_trailers(Headers trailers) {
        if (m_field_rejected) {
            co_return make_error_code(asio::error::invalid_argument);
        }
        co_return co_await m_writer->send_trailers(std::move(trailers));
    }
    // Ends a streamed response with the fields accumulated by trailer().
    [[nodiscard]] asio::awaitable<error_code> finish_trailers() { return finish_trailers(std::move(m_trailers)); }

    // --- connection state ---
    // Both hop onto the connection executor inside the writer, like the writes.
    [[nodiscard]] asio::awaitable<bool> connected() const { return m_writer->connected(); }
    [[nodiscard]] asio::awaitable<void> close() { return m_writer->close(); }
    Version version() const { return m_writer->version(); }

    // --- redirect & SSE helpers ---

    // Fluent redirect: sets `Location` and the status (302 Found by default —
    // status::see_other for a POST→GET transition, status::permanent_redirect
    // for a moved-forever). The Location header goes through header(), so a
    // CR/LF/NUL injection is refused there, not emitted. Use with .send(""):
    //   co_await res->redirect("/login").send("");
    ResponseWriter &redirect(std::string location, int code = status::found) {
        m_status = code;
        return header(field::location, std::move(location));
    }

    // Server-Sent Events (SSE, text/event-stream): a thin stream wrapper.
    //   co_await res->status(200).sse_begin();
    //   co_await res->sse_event("hello");               // data: hello\n\n
    //   co_await res->sse_event("l1\nl2");              // two data: lines, one event
    //   co_await res->sse_event("payload", "update", "42", ""); // + event: and id:
    //   co_await res->sse_comment("keepalive");         // : keepalive\n\n
    // The connection stays open (h1 chunked / h2 DATA frames) until the handler
    // returns or the client goes away; returning without finish() leaves the
    // stream half-open, which the engine closes as part of cleanup.
    [[nodiscard]] asio::awaitable<error_code> sse_begin() {
        header(field::content_type, "text/event-stream; charset=utf-8");
        header("cache-control", "no-cache");
        return begin();
    }
    [[nodiscard]] asio::awaitable<error_code> sse_event(std::string data, std::string event = {}, std::string id = {},
                                                        std::string retry = {}) {
        return write(render_sse(data, event, id, retry));
    }
    [[nodiscard]] asio::awaitable<error_code> sse_comment(std::string text) {
        return write(": " + std::move(text) + "\n\n");
    }

    ResponseSink &writer() { return *m_writer; }

    // The accumulated response fields. The WebSocket upgrade path reads this to
    // fold middleware-set headers into the 101 handshake.
    const Headers &headers() const { return m_headers; }

  private:
    // Renders one SSE event frame: optional event/id/retry fields, then one
    // "data: " line per line of `data` (so multi-line payloads stay one event
    // for the client), closed by a blank line.
    static std::string render_sse(std::string_view data, std::string_view event, std::string_view id,
                                  std::string_view retry) {
        std::string frame;
        if (!event.empty()) {
            frame += "event: ";
            frame += event;
            frame += '\n';
        }
        if (!id.empty()) {
            frame += "id: ";
            frame += id;
            frame += '\n';
        }
        if (!retry.empty()) {
            frame += "retry: ";
            frame += retry;
            frame += '\n';
        }
        std::size_t pos = 0;
        do {
            const std::size_t nl = data.find('\n', pos);
            frame += "data: ";
            frame.append(data.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos));
            frame += '\n';
            if (nl == std::string_view::npos) {
                break;
            }
            pos = nl + 1;
        } while (pos < data.size());
        frame += '\n';
        return frame;
    }
    // Frames a one-shot response whose end is a trailer section: headers
    // (chunked on HTTP/1.x), an optional body chunk, then the trailers. Only for
    // send(), where the head has not gone out yet; the streaming finish() path
    // has already sent its head in begin().
    [[nodiscard]] asio::awaitable<error_code> send_stream(std::string body, Headers trailers) {
        if (auto ec = co_await m_writer->send_headers(m_status, std::move(m_headers)); ec) {
            co_return ec;
        }
        if (!body.empty()) {
            if (auto ec = co_await m_writer->send_chunk(std::move(body)); ec) {
                co_return ec;
            }
        }
        co_return co_await m_writer->send_trailers(std::move(trailers));
    }

    // Fill in Server and Content-Type headers if the handler did not set them.
    void apply_defaults() {
        if (!m_headers.contains("content-type")) {
            m_headers.add_lower("content-type", "text/plain");
        }
        if (!m_headers.contains("server")) {
            m_headers.add_lower("server", std::string{server_version});
        }
    }

    std::shared_ptr<ResponseSink> m_writer;
    int m_status{200};
    Headers m_headers;
    // Trailer fields accumulated with trailer(); sent by finish()/finish_trailers().
    Headers m_trailers;
    // Set when header() refused a field; the response is then not sent at all.
    // Emitting the rest would put a head on the wire that the handler did not
    // intend — and the whole point of rejecting at header() was to not do that.
    bool m_field_rejected{false};
    // Set once the head has gone out (see started()).
    bool m_started{false};
};

} // namespace simple_http
