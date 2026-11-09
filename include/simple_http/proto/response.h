#pragma once

// ResponseWriter: the user-facing, fluent, version-agnostic reply handle
// (C++17 token-only port).
//
// ResponseWriter holds a ResponseSink and forwards to it. Every I/O operation is
// an Asio initiating function taking an explicit completion token; the state
// mutations (status/header/content_type/set_cookie/trailer) stay synchronous and
// owner-only. The caller's token decides whether a failure is an error_code
// value (callback / as_tuple / redirect_error) or an exception (use_awaitable).
//
// Two usage modes:
//   one-shot:  res.async_send("body", token);
//   streaming: res.async_begin(token); res.async_write(chunk, token); ...
//
// Multi-step operations (send with trailers, finish with trailers) are written
// as asio::async_compose state machines so the associated executor / allocator /
// cancellation slot survives every intermediate sink call.

#include <atomic>
#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/compose.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
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
    explicit ResponseWriter(std::shared_ptr<ResponseSink> writer) : m_writer(std::move(writer)) {
        // Date + Content-Type + Server are filled in by apply_defaults; a
        // handler usually adds one or two more. Reserving up front keeps the
        // common case at one vector allocation instead of log2(n) growth moves.
        m_headers.reserve(4);
    }

    // --- fluent setters (return *this for chaining) ---
    ResponseWriter &status(int code) {
        m_status = code;
        return *this;
    }
    int status() const { return m_status; }
    bool started() const { return m_started; }
    [[nodiscard]] std::uint64_t activity() const noexcept { return m_activity.load(std::memory_order_relaxed); }
    ResponseWriter &header(std::string_view name, std::string value) {
        if (contains_ctl(name) || contains_ctl(value)) {
            SIMPLE_HTTP_ERROR_LOG("response field with CR/LF/NUL rejected");
            m_field_rejected = true;
            return *this;
        }
        m_headers.add(std::string{name}, std::move(value));
        return *this;
    }

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
        if (max_age.count() < 0) {
            // Go's http.SetCookie: a negative MaxAge means "delete now" and is
            // serialized as Max-Age=0 (0 itself means "unspecified").
            field_value += "; Max-Age=0";
        } else if (max_age.count() > 0) {
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

    // Completion signature: void(error_code).
    template <typename Token> auto async_send(Token &&token) {
        return async_send(std::string{}, std::forward<Token>(token));
    }

    template <typename Token> auto async_send(std::string body, Token &&token) {
        error_code pre = prepare_one_shot();
        Mode mode = Mode::Direct;
        if (!pre) {
            if (m_status == status::no_content || m_status == status::not_modified) {
                mode = Mode::Bodyless;
            } else if (!m_trailers.empty() && !m_writer->head_request() && m_writer->version() != Version::Http1) {
                // A response carrying trailers cannot be one-shot: the fields
                // follow the body, so it is framed as a stream (chunked on
                // HTTP/1.x) and closed by the trailer section. A HEAD response
                // or an HTTP/1.0 peer keeps the one-shot framing and drops the
                // fields instead.
                mode = Mode::Stream;
            }
        }
        return compose_send(pre, mode, std::move(body), std::forward<Token>(token));
    }

    // Status + headers with no body and no body framing at all (204/304 and
    // every response to HEAD).
    template <typename Token> auto async_send_bodyless(Token &&token) {
        error_code pre = prepare_one_shot(false);
        return compose_send(pre, Mode::Bodyless, std::string{}, std::forward<Token>(token));
    }

    // --- streaming ---
    template <typename Token> auto async_begin(Token &&token) {
        error_code pre = prepare_one_shot(false);
        auto sink = m_writer;
        const int code = m_status;
        auto headers = std::move(m_headers);
        return compose_one(pre, sink->get_executor(), std::forward<Token>(token),
                           [sink, code, headers = std::move(headers)](WriteCompletion done) mutable {
                               sink->send_headers(code, std::move(headers), std::move(done));
                           });
    }

    // Sends an interim 1xx response (100 Continue).
    template <typename Token> auto async_send_continue(Token &&token) {
        note_activity();
        auto sink = m_writer;
        return compose_one(error_code{}, sink->get_executor(), std::forward<Token>(token),
                           [sink](WriteCompletion done) mutable { sink->send_continue(std::move(done)); });
    }

    template <typename Token> auto async_write(std::string data, Token &&token) {
        note_activity();
        auto sink = m_writer;
        return compose_one(error_code{}, sink->get_executor(), std::forward<Token>(token),
                           [sink, data = std::move(data)](WriteCompletion done) mutable {
                               sink->send_chunk(std::move(data), std::move(done));
                           });
    }

    template <typename Token> auto async_finish(Token &&token) {
        return async_finish(std::string{}, std::forward<Token>(token));
    }

    template <typename Token> auto async_finish(std::string data, Token &&token) {
        note_activity();
        auto sink = m_writer;
        // Trailer fields accumulated with trailer() end the response instead of
        // the final body chunk, so a finish() after them still emits them.
        if (!m_trailers.empty()) {
            const error_code pre = m_field_rejected ? make_error_code(asio::error::invalid_argument) : error_code{};
            auto trailers = std::move(m_trailers);
            return compose_finish_trailers(pre, std::move(data), std::move(trailers), sink, std::forward<Token>(token));
        }
        return compose_one(error_code{}, sink->get_executor(), std::forward<Token>(token),
                           [sink, data = std::move(data)](WriteCompletion done) mutable {
                               sink->send_last(std::move(data), std::move(done));
                           });
    }

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
    // the final finish(). Completion signature: void(error_code).
    template <typename Token> auto async_finish_trailers(Headers trailers, Token &&token) {
        for (const auto &[name, value] : trailers) {
            if (contains_ctl(name) || contains_ctl(value)) {
                SIMPLE_HTTP_ERROR_LOG("response trailer with CR/LF/NUL rejected");
                m_field_rejected = true;
                break;
            }
        }
        note_activity();
        auto sink = m_writer;
        const error_code pre = m_field_rejected ? make_error_code(asio::error::invalid_argument) : error_code{};
        return compose_finish_trailers(pre, std::string{}, std::move(trailers), sink, std::forward<Token>(token));
    }

    template <typename Token> auto async_finish_trailers(Token &&token) {
        return async_finish_trailers(std::move(m_trailers), std::forward<Token>(token));
    }

    // --- connection state ---
    template <typename Token> auto async_connected(Token &&token) const {
        auto sink = m_writer;
        return compose_connected(sink->get_executor(), std::forward<Token>(token),
                                 [sink](ConnectedCompletion done) { sink->connected(std::move(done)); });
    }

    template <typename Token> auto async_close(Token &&token) {
        auto sink = m_writer;
        return compose_one(error_code{}, sink->get_executor(), std::forward<Token>(token),
                           [sink](WriteCompletion done) mutable { sink->close(std::move(done)); });
    }

    Version version() const { return m_writer->version(); }

    // --- redirect & SSE helpers ---

    ResponseWriter &redirect(std::string location, int code = status::found) {
        m_status = code;
        return header(field::location, std::move(location));
    }

    template <typename Token> auto async_sse_begin(Token &&token) {
        header(field::content_type, "text/event-stream; charset=utf-8");
        header("cache-control", "no-cache");
        return async_begin(std::forward<Token>(token));
    }
    template <typename Token>
    auto async_sse_event(std::string data, std::string event, std::string id, std::string retry, Token &&token) {
        return async_write(render_sse(data, event, id, retry), std::forward<Token>(token));
    }
    template <typename Token> auto async_sse_event(std::string data, Token &&token) {
        return async_sse_event(std::move(data), {}, {}, {}, std::forward<Token>(token));
    }
    template <typename Token> auto async_sse_comment(std::string text, Token &&token) {
        return async_write(": " + std::move(text) + "\n\n", std::forward<Token>(token));
    }

    ResponseSink &writer() { return *m_writer; }
    const Headers &headers() const { return m_headers; }

    // A sink operation adapter used by the connect/close/one-shot helpers. The
    // token's associated properties are carried by `composed` (the async_compose
    // handler), which is wrapped into any_completion_handler at the sink edge.
    template <typename Token, typename Fn>
    static auto compose_one(error_code pre, asio::any_io_executor ex, Token &&token, Fn &&fn) {
        // Keep a copy for async_compose's I/O object: moving `ex` into the
        // composed operation would leave this argument empty, and the two
        // arguments are evaluated in an unspecified order (a moved-from
        // any_io_executor throws bad_executor when the work guard is built).
        auto executor = ex;
        return asio::async_compose<Token, void(error_code)>(
            OneOp<std::decay_t<Fn>>{pre, std::forward<Fn>(fn), std::move(ex)}, token, executor);
    }

    template <typename Token, typename Fn>
    static auto compose_connected(asio::any_io_executor ex, Token &&token, Fn &&fn) {
        auto executor = ex;
        return asio::async_compose<Token, void(error_code, bool)>(
            ConnectedOp<std::decay_t<Fn>>{std::forward<Fn>(fn), std::move(ex)}, token, executor);
    }

  private:
    enum class Mode { Direct, Bodyless, Stream };
    enum class Phase { Start, Headers, Chunk, Trailers, Final };

    // Synchronous pre-flight shared by send/send_bodyless/begin: field
    // rejection, single-head rule, defaults and the activity counter.
    error_code prepare_one_shot(bool require_first_head = true) {
        if (m_field_rejected) {
            return make_error_code(asio::error::invalid_argument);
        }
        // A response has exactly one head. Refusing a second one here is what
        // lets invoke_handler's 500 fallback check started() instead of
        // splicing a second status line onto the wire. send_bodyless()/begin()
        // keep the historical shape: they only set the flag.
        if (require_first_head && m_started) {
            return make_error_code(asio::error::invalid_argument);
        }
        note_activity();
        apply_defaults();
        m_started = true;
        return {};
    }

    template <typename Token> auto compose_send(error_code pre, Mode mode, std::string body, Token &&token) {
        auto state = std::make_shared<SendState>();
        state->pre = pre;
        state->mode = mode;
        state->sink = m_writer;
        state->status = m_status;
        state->headers = std::move(m_headers);
        state->body = std::move(body);
        if (mode == Mode::Stream) {
            state->trailers = std::move(m_trailers);
        }
        auto ex = state->sink->get_executor();
        return asio::async_compose<Token, void(error_code)>(SendOp{std::move(state)}, token, ex);
    }

    template <typename Token>
    auto compose_finish_trailers(error_code pre, std::string data, Headers trailers, std::shared_ptr<ResponseSink> sink,
                                 Token &&token) {
        auto state = std::make_shared<FinishTrailersState>();
        state->pre = pre;
        state->sink = std::move(sink);
        state->data = std::move(data);
        state->trailers = std::move(trailers);
        auto ex = state->sink->get_executor();
        return asio::async_compose<Token, void(error_code)>(FinishTrailersOp{std::move(state)}, token, ex);
    }

    // Renders one SSE event frame: optional event/id/retry fields, then one
    // "data: " line per line of `data`, closed by a blank line.
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

    // Fill in Date, Server and Content-Type if the handler did not set them.
    void apply_defaults() {
        ensure_date(m_headers);
        // 204/304 carry no content, so a default Content-Type would describe
        // something that does not exist (RFC 9110 §15.3.5/§15.4.5).
        if (m_status != status::no_content && m_status != status::not_modified && !m_headers.contains("content-type")) {
            m_headers.add_lower("content-type", "text/plain");
        }
        if (!m_headers.contains("server")) {
            m_headers.add_lower("server", std::string{server_product});
        }
    }

    template <typename Fn> struct OneOp {
        error_code pre;
        Fn fn;
        asio::any_io_executor ex;
        bool started{false};

        asio::any_io_executor get_executor() const noexcept { return ex; }

        template <typename Self> void operator()(Self &self, error_code ec = {}) {
            if (!started) {
                started = true;
                if (pre) {
                    self.complete(pre);
                    return;
                }
                auto action = std::move(fn);
                action(WriteCompletion(std::move(self)));
                return;
            }
            self.complete(ec);
        }
    };

    template <typename Fn> struct ConnectedOp {
        Fn fn;
        asio::any_io_executor ex;
        bool started{false};

        asio::any_io_executor get_executor() const noexcept { return ex; }

        template <typename Self> void operator()(Self &self, error_code ec = {}, bool value = false) {
            if (!started) {
                started = true;
                auto action = std::move(fn);
                action(ConnectedCompletion(std::move(self)));
                return;
            }
            self.complete(ec, value);
        }
    };

    // Multi-step response operations keep their state behind a shared_ptr:
    // async_compose moves the implementation into the completion handler when
    // the next operation is started, so data stored directly in the operation
    // would be moved-from exactly when the next step needs it.
    struct SendState {
        error_code pre;
        Mode mode{Mode::Direct};
        std::shared_ptr<ResponseSink> sink;
        int status{0};
        Headers headers;
        std::string body;
        Headers trailers;
        Phase phase{Phase::Start};
    };

    struct SendOp {
        std::shared_ptr<SendState> st;
        bool started{false};

        asio::any_io_executor get_executor() const noexcept { return st->sink->get_executor(); }

        template <typename Self> void operator()(Self &self, error_code ec = {}) {
            auto state = st; // keep the state alive across the move of `self`
            if (!started) {
                started = true;
                if (state->pre) {
                    self.complete(state->pre);
                    return;
                }
                switch (state->mode) {
                case Mode::Bodyless:
                    state->phase = Phase::Final;
                    state->sink->send_bodyless(state->status, std::move(state->headers),
                                               WriteCompletion(std::move(self)));
                    return;
                case Mode::Direct:
                    state->phase = Phase::Final;
                    state->sink->send(state->status, std::move(state->headers), std::move(state->body),
                                      WriteCompletion(std::move(self)));
                    return;
                case Mode::Stream:
                    state->phase = Phase::Headers;
                    state->sink->send_headers(state->status, std::move(state->headers),
                                              WriteCompletion(std::move(self)));
                    return;
                }
                return;
            }
            if (ec) {
                self.complete(ec);
                return;
            }
            switch (state->phase) {
            case Phase::Headers:
                if (!state->body.empty()) {
                    state->phase = Phase::Chunk;
                    state->sink->send_chunk(std::move(state->body), WriteCompletion(std::move(self)));
                    return;
                }
                state->phase = Phase::Trailers;
                state->sink->send_trailers(std::move(state->trailers), WriteCompletion(std::move(self)));
                return;
            case Phase::Chunk:
                state->phase = Phase::Trailers;
                state->sink->send_trailers(std::move(state->trailers), WriteCompletion(std::move(self)));
                return;
            case Phase::Final:
            case Phase::Trailers:
            case Phase::Start:
            default:
                self.complete(error_code{});
                return;
            }
        }
    };

    struct FinishTrailersState {
        error_code pre;
        std::shared_ptr<ResponseSink> sink;
        std::string data;
        Headers trailers;
        Phase phase{Phase::Start};
    };

    struct FinishTrailersOp {
        std::shared_ptr<FinishTrailersState> st;
        bool started{false};

        asio::any_io_executor get_executor() const noexcept { return st->sink->get_executor(); }

        template <typename Self> void operator()(Self &self, error_code ec = {}) {
            auto state = st;
            if (!started) {
                started = true;
                if (state->pre) {
                    self.complete(state->pre);
                    return;
                }
                if (!state->data.empty()) {
                    state->phase = Phase::Chunk;
                    state->sink->send_chunk(std::move(state->data), WriteCompletion(std::move(self)));
                    return;
                }
                state->phase = Phase::Trailers;
                state->sink->send_trailers(std::move(state->trailers), WriteCompletion(std::move(self)));
                return;
            }
            if (ec) {
                self.complete(ec);
                return;
            }
            if (state->phase == Phase::Chunk) {
                state->phase = Phase::Trailers;
                state->sink->send_trailers(std::move(state->trailers), WriteCompletion(std::move(self)));
                return;
            }
            self.complete(error_code{});
        }
    };

    std::shared_ptr<ResponseSink> m_writer;
    int m_status{status::ok};
    Headers m_headers;
    Headers m_trailers;
    bool m_field_rejected{false};
    bool m_started{false};
    std::atomic<std::uint64_t> m_activity{0};

    void note_activity() { m_activity.fetch_add(1, std::memory_order_relaxed); }
};

} // namespace simple_http
