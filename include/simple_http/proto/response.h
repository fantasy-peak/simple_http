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
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/compose.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

#include "../core/http_field.h"
#include "../core/http_status.h"
#include "../core/lifecycle.h"
#include "../core/logging.h"
#include "../core/types.h"
#include "../core/version.h"
#include "headers.h"
#include "response_writer.h"

namespace simple_http {

namespace asio = boost::asio;

class ResponseWriter : public std::enable_shared_from_this<ResponseWriter> {
  public:
    explicit ResponseWriter(std::shared_ptr<ResponseSink> writer) : m_writer(std::move(writer)) {
        // Date + Content-Type + Server are filled in by apply_defaults; a
        // handler usually adds one or two more. Reserving up front keeps the
        // common case at one vector allocation instead of log2(n) growth moves.
        m_headers.reserve(4);
    }

    ResponseWriter(const ResponseWriter &) = delete;
    ResponseWriter &operator=(const ResponseWriter &) = delete;

    // The engine binds the exchange/stream lifecycle immediately after
    // creating the writer. Writers created without one (none in-tree today)
    // keep working; only the lifecycle gate is skipped.
    void bind_lifecycle(std::weak_ptr<Lifecycle> life) {
        require_owner();
        m_life = std::move(life);
        m_life_managed = true;
    }

  private:
    // Public async entry gate. It reposts the operation onto the connection
    // executor, then acquires the writer (and, through the lifecycle, the
    // stream/connection). `Fn` runs on the owner thread with a Guard that keeps
    // the writer alive until the inner operation completes.
    template <typename Fn> struct StartWriteOp {
        StartWriteOp(std::weak_ptr<ResponseWriter> owner, ResponseWriter *raw, std::weak_ptr<Lifecycle> life,
                     bool managed, Lifecycle::Access access, asio::any_io_executor ex, Fn fn)
            : owner(std::move(owner)), raw(raw), life(std::move(life)), managed(managed), access(access),
              ex(std::move(ex)), fn(std::move(fn)) {}

        std::weak_ptr<ResponseWriter> owner;
        // Stack-constructed writers have no shared control block. The historical
        // contract (the caller keeps the writer alive until completion) still
        // applies to them, so the operation borrows a non-owning handle.
        ResponseWriter *raw{nullptr};
        std::weak_ptr<Lifecycle> life;
        bool managed{false};
        Lifecycle::Access access{Lifecycle::Access::Write};
        asio::any_io_executor ex;
        bool hopped{false};
        bool started{false};
        Guard<ResponseWriter> guard;
        Fn fn;

        asio::any_io_executor get_executor() const noexcept { return ex; }

        template <typename Self> void operator()(Self &self, error_code ec = {}) {
            if (!started) {
                // Unbound writers (standalone tests) keep the historical
                // same-thread initiation; a server-bound writer is reposted
                // onto the connection executor.
                if (managed && hop_to_owner(self, ex, hopped)) {
                    return;
                }
                if (self.cancelled() != asio::cancellation_type::none) {
                    self.complete(make_error_code(asio::error::operation_aborted));
                    return;
                }
                if (managed && access != Lifecycle::Access::Any) {
                    auto l = life.lock();
                    if (!l || !l->can(access)) {
                        self.complete(make_error_code(asio::error::not_connected));
                        return;
                    }
                }
                if (auto acquired = owner.lock()) {
                    guard = Guard<ResponseWriter>{std::move(acquired)};
                } else if (!managed && raw) {
                    // Only an unbound (stack-constructed) writer may be borrowed
                    // through `raw`: it has no control block by construction, and
                    // the historical "caller keeps it alive" contract applies.
                    // A managed writer reached here means its last strong
                    // reference was dropped while the operation was queued on a
                    // cross-thread hop, so `raw` is dangling -- not a borrow.
                    guard = Guard<ResponseWriter>{std::shared_ptr<ResponseWriter>(raw, [](ResponseWriter *) {})};
                } else {
                    self.complete(make_error_code(asio::error::not_connected));
                    return;
                }
                started = true;
                auto action = std::move(fn);
                // Evaluate the guard's target before moving `self`: the op and
                // with it `guard` are moved into WriteCompletion, and argument
                // evaluation order is unspecified.
                auto *writer = &*guard;
                action(*writer, WriteCompletion(std::move(self)));
                return;
            }
            guard = {};
            self.complete(ec);
        }
    };

    template <typename Fn> struct StartConnectedOp {
        StartConnectedOp(std::weak_ptr<const ResponseWriter> owner, const ResponseWriter *raw,
                         std::weak_ptr<Lifecycle> life, bool managed, asio::any_io_executor ex, Fn fn)
            : owner(std::move(owner)), raw(raw), life(std::move(life)), managed(managed), ex(std::move(ex)),
              fn(std::move(fn)) {}

        std::weak_ptr<const ResponseWriter> owner;
        const ResponseWriter *raw{nullptr};
        std::weak_ptr<Lifecycle> life;
        bool managed{false};
        asio::any_io_executor ex;
        bool hopped{false};
        bool started{false};
        Guard<const ResponseWriter> guard;
        Fn fn;

        asio::any_io_executor get_executor() const noexcept { return ex; }

        template <typename Self> void operator()(Self &self, error_code ec = {}, bool value = false) {
            if (!started) {
                if (managed && hop_to_owner(self, ex, hopped)) {
                    return;
                }
                if (self.cancelled() != asio::cancellation_type::none) {
                    self.complete(make_error_code(asio::error::operation_aborted), false);
                    return;
                }
                // The sink reports whether the connection is reusable; it is
                // safe on a dead stream (returns success + false), so no
                // lifecycle gate is needed.
                if (auto acquired = owner.lock()) {
                    guard = Guard<const ResponseWriter>{std::move(acquired)};
                } else if (!managed && raw) {
                    // See StartWriteOp: `raw` is only a valid borrow for an
                    // unbound stack writer; a managed one whose weak_ptr expired
                    // was destroyed while this operation was queued.
                    guard =
                        Guard<const ResponseWriter>{std::shared_ptr<const ResponseWriter>(raw, [](const ResponseWriter *) {})};
                } else {
                    self.complete(make_error_code(asio::error::not_connected), false);
                    return;
                }
                started = true;
                auto action = std::move(fn);
                auto *writer = &*guard;
                action(*writer, ConnectedCompletion(std::move(self)));
                return;
            }
            guard = {};
            self.complete(ec, value);
        }
    };

    template <typename Fn, typename Token> auto start_write(Fn &&fn, Token &&token) {
        return start_io(Lifecycle::Access::Write, std::forward<Fn>(fn), std::forward<Token>(token));
    }

    // Close/connected-style operations must still run after the direction has
    // closed; they only require the lifecycle itself to be open.
    template <typename Fn, typename Token> auto start_control(Fn &&fn, Token &&token) {
        return start_io(Lifecycle::Access::Control, std::forward<Fn>(fn), std::forward<Token>(token));
    }

    // Operations whose sink is safe on a dead stream and whose historical
    // contract is "no-op after the response ended" use Any: the guard still
    // keeps the writer alive, but the lifecycle does not reject the call.
    template <typename Fn, typename Token> auto start_any(Fn &&fn, Token &&token) {
        return start_io(Lifecycle::Access::Any, std::forward<Fn>(fn), std::forward<Token>(token));
    }

    // The sink's executor (the connection's owner executor for a server-bound
    // writer) is bound to the token before composition, so the composed
    // operation's associated executor -- used by every inner step and by the
    // hop -- is the owner even if the caller bound something else to the token.
    template <typename Fn, typename Token>
    auto start_io(Lifecycle::Access access, Fn &&fn, Token &&token) {
        auto ex = m_writer->get_executor();
        auto bound = asio::bind_executor(ex, std::forward<Token>(token));
        return asio::async_compose<decltype(bound), void(error_code)>(
            StartWriteOp<std::decay_t<Fn>>{weak_from_this(), this, m_life, m_life_managed, access, ex,
                                          std::forward<Fn>(fn)},
            bound, ex);
    }

    template <typename Fn, typename Token> auto start_connected(Fn &&fn, Token &&token) const {
        auto ex = m_writer->get_executor();
        auto bound = asio::bind_executor(ex, std::forward<Token>(token));
        return asio::async_compose<decltype(bound), void(error_code, bool)>(
            StartConnectedOp<std::decay_t<Fn>>{weak_from_this(), this, m_life, m_life_managed, ex,
                                              std::forward<Fn>(fn)},
            bound, ex);
    }

  public:
    // --- fluent setters (return *this for chaining) ---
    ResponseWriter &status(int code) {
        require_owner();
        // RFC 9110 §15: a status code is exactly three digits, 100..599. A
        // handler-set 0/9999 would serialize an illegal status line (and h2
        // clients reset the stream), so refuse it like any other bad field.
        if (code < 100 || code > 599) {
            SIMPLE_HTTP_ERROR_LOG("response status {} outside 100..599 rejected", code);
            m_field_rejected = true;
            return *this;
        }
        m_status = code;
        return *this;
    }
    int status() const { return m_status; }
    bool started() const { return m_started; }

    // Drops every field the handler set — including ones rejected for syntax —
    // so the dispatcher's exception fallback can still write a clean 500. Only
    // valid before the response starts; returns false otherwise.
    bool reset_for_error() {
        require_owner();
        if (m_started)
            return false;
        m_headers.clear();
        m_trailers.clear();
        m_field_rejected = false;
        m_status = status::internal_server_error;
        return true;
    }
    [[nodiscard]] std::uint64_t activity() const noexcept { return m_activity.load(std::memory_order_relaxed); }
    ResponseWriter &header(std::string_view name, std::string value) {
        require_owner();
        // A field name is a token and a value may not contain other control
        // bytes either (RFC 9110 §5.1/§5.5); otherwise "bad name" or "\x01"
        // is serialized as-is and the peer sees a malformed field section.
        if (!is_http_token(name) || contains_invalid_field_value(value)) {
            SIMPLE_HTTP_ERROR_LOG("response field with invalid name/value rejected");
            m_field_rejected = true;
            return *this;
        }
        m_headers.add(std::string{name}, std::move(value));
        return *this;
    }

    ResponseWriter &replace_header(std::string_view name, std::string value) {
        require_owner();
        if (!is_http_token(name) || contains_invalid_field_value(value)) {
            SIMPLE_HTTP_ERROR_LOG("response field with invalid name/value rejected");
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
        require_owner();
        // RFC 6265 §4.1.1: cookie-name is a token and cookie-value is
        // *cookie-octet (optionally DQUOTE-wrapped). Quoting is not an escape:
        // an embedded DQUOTE/backslash (or a space/comma/semicolon) would end
        // the value and start an attacker-chosen attribute. Refuse the field
        // instead of emitting a malformed or injected Set-Cookie.
        const bool valid = is_http_token(name) && valid_cookie_value(value) && valid_cookie_attribute(path, true) &&
                           valid_cookie_attribute(domain, false) && (same_site.empty() || is_http_token(same_site));
        if (!valid) {
            SIMPLE_HTTP_ERROR_LOG("set_cookie: invalid name/value/attribute rejected");
            m_field_rejected = true;
            return *this;
        }
        std::string field_value = std::move(name);
        field_value.push_back('=');
        field_value += value;
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
        return start_write(
            [body = std::move(body)](ResponseWriter &w, WriteCompletion done) mutable {
                error_code pre = w.prepare_one_shot();
                Mode mode = Mode::Direct;
                if (!pre) {
                    if (w.m_status == status::no_content || w.m_status == status::not_modified) {
                        mode = Mode::Bodyless;
                    } else if (!w.m_trailers.empty() && !w.m_writer->head_request() &&
                               w.m_writer->version() != Version::Http1) {
                        // A response carrying trailers cannot be one-shot: the fields
                        // follow the body, so it is framed as a stream (chunked on
                        // HTTP/1.x) and closed by the trailer section. A HEAD response
                        // or an HTTP/1.0 peer keeps the one-shot framing and drops the
                        // fields instead.
                        mode = Mode::Stream;
                    }
                }
                (void)w.compose_send(pre, mode, std::move(body), std::move(done));
            },
            std::forward<Token>(token));
    }

    // Status + headers with no body and no body framing at all (204/304 and
    // every response to HEAD).
    template <typename Token> auto async_send_bodyless(Token &&token) {
        return start_write(
            [](ResponseWriter &w, WriteCompletion done) {
                // Without a body, a response can only be framed by the status (204/304)
                // or by being the answer to HEAD. A bodyless 200 on a keep-alive h1
                // connection has no length and no terminator: the peer takes the next
                // response's bytes as this body (response smuggling).
                //
                // Validate before prepare_one_shot(): it marks the response started, and
                // invoke_handler's 500 fallback only runs while started() is false.
                error_code pre;
                if (!w.m_writer->head_request() && w.m_status != status::no_content &&
                    w.m_status != status::not_modified) {
                    pre = make_error_code(asio::error::invalid_argument);
                } else {
                    pre = w.prepare_one_shot();
                }
                (void)w.compose_send(pre, Mode::Bodyless, std::string{}, std::move(done));
            },
            std::forward<Token>(token));
    }

    // --- streaming ---
    template <typename Token> auto async_begin(Token &&token) {
        return start_write(
            [](ResponseWriter &w, WriteCompletion done) {
                // 204/304 have no body by definition; streaming one emits a chunked
                // 204 (h1) or DATA frames (h2) that a conforming peer rejects or reads
                // as the next message. Check before prepare_one_shot() so the failure
                // leaves started() false and the 500 fallback can still answer.
                error_code pre;
                if (w.m_status == status::no_content || w.m_status == status::not_modified) {
                    pre = make_error_code(asio::error::invalid_argument);
                } else {
                    pre = w.prepare_one_shot();
                }
                auto sink = w.m_writer;
                const int code = w.m_status;
                auto headers = std::move(w.m_headers);
                compose_one(pre, sink->get_executor(), std::move(done),
                            [sink, code, headers = std::move(headers)](WriteCompletion d) mutable {
                                sink->send_headers(code, std::move(headers), std::move(d));
                            });
            },
            std::forward<Token>(token));
    }

    // Sends an interim 1xx response (100 Continue).
    template <typename Token> auto async_send_continue(Token &&token) {
        return start_any(
            [](ResponseWriter &w, WriteCompletion done) {
                w.note_activity();
                auto sink = w.m_writer;
                sink->send_continue(std::move(done));
            },
            std::forward<Token>(token));
    }

    template <typename Token> auto async_write(std::string data, Token &&token) {
        // Any, not Write: after a HEAD response the stream's write side is
        // already closed, but the sink accepts these as no-ops (the same
        // contract as before the lifecycle gate). The sink also reports
        // not_connected when a non-HEAD stream is already gone.
        return start_any(
            [data = std::move(data)](ResponseWriter &w, WriteCompletion done) mutable {
                w.note_activity();
                auto sink = w.m_writer;
                sink->send_chunk(std::move(data), std::move(done));
            },
            std::forward<Token>(token));
    }

    template <typename Token> auto async_finish(Token &&token) {
        return async_finish(std::string{}, std::forward<Token>(token));
    }

    template <typename Token> auto async_finish(std::string data, Token &&token) {
        return start_any(
            [data = std::move(data)](ResponseWriter &w, WriteCompletion done) mutable {
                w.note_activity();
                auto sink = w.m_writer;
                // Trailer fields accumulated with trailer() end the response instead of
                // the final body chunk, so a finish() after them still emits them.
                if (!w.m_trailers.empty()) {
                    const error_code pre =
                        w.m_field_rejected ? make_error_code(asio::error::invalid_argument) : error_code{};
                    auto trailers = std::move(w.m_trailers);
                    w.compose_finish_trailers(pre, std::move(data), std::move(trailers), sink, std::move(done));
                    return;
                }
                sink->send_last(std::move(data), std::move(done));
            },
            std::forward<Token>(token));
    }

    ResponseWriter &trailer(std::string_view name, std::string value) {
        require_owner();
        if (!is_http_token(name) || contains_invalid_field_value(value)) {
            SIMPLE_HTTP_ERROR_LOG("response trailer with invalid name/value rejected");
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
        return start_any(
            [trailers = std::move(trailers)](ResponseWriter &w, WriteCompletion done) mutable {
                w.finish_trailers_now(std::move(trailers), std::move(done));
            },
            std::forward<Token>(token));
    }

    template <typename Token> auto async_finish_trailers(Token &&token) {
        return start_any(
            [](ResponseWriter &w, WriteCompletion done) {
                auto trailers = std::move(w.m_trailers);
                w.finish_trailers_now(std::move(trailers), std::move(done));
            },
            std::forward<Token>(token));
    }

    // --- connection state ---
    template <typename Token> auto async_connected(Token &&token) const {
        return start_connected(
            [](const ResponseWriter &w, ConnectedCompletion done) {
                auto sink = w.m_writer;
                sink->connected(std::move(done));
            },
            std::forward<Token>(token));
    }

    template <typename Token> auto async_close(Token &&token) {
        return start_any(
            [](ResponseWriter &w, WriteCompletion done) {
                auto sink = w.m_writer;
                sink->close(std::move(done));
            },
            std::forward<Token>(token));
    }

    Version version() const { return m_writer->version(); }

    // --- redirect & SSE helpers ---

    ResponseWriter &redirect(std::string location, int code = status::found) {
        require_owner();
        // Go through status() so 0/9999 are rejected like any other bad code
        // instead of serializing an illegal status line.
        status(code);
        return header(field::location, std::move(location));
    }

    template <typename Token> auto async_sse_begin(Token &&token) {
        return start_write(
            [](ResponseWriter &w, WriteCompletion done) {
                w.header(field::content_type, "text/event-stream; charset=utf-8");
                w.header("cache-control", "no-cache");
                (void)w.async_begin(std::move(done));
            },
            std::forward<Token>(token));
    }
    template <typename Token>
    auto async_sse_event(std::string data, std::string event, std::string id, std::string retry, Token &&token) {
        return async_write(render_sse(data, event, id, retry), std::forward<Token>(token));
    }
    template <typename Token> auto async_sse_event(std::string data, Token &&token) {
        return async_sse_event(std::move(data), {}, {}, {}, std::forward<Token>(token));
    }
    template <typename Token> auto async_sse_comment(std::string text, Token &&token) {
        return async_write(render_sse_comment(text), std::forward<Token>(token));
    }

    ResponseSink &writer() { return *m_writer; }
    const Headers &headers() const { return m_headers; }

    // A sink operation adapter used by the connect/close/one-shot helpers. The
    // token's associated properties are carried by `composed` (the async_compose
    // handler), which is wrapped into any_completion_handler at the sink edge.
    template <typename Token, typename Fn>
    static void compose_one(error_code pre, asio::any_io_executor ex, Token &&token, Fn &&fn) {
        // Keep a copy for async_compose's I/O object: moving `ex` into the
        // composed operation would leave this argument empty, and the two
        // arguments are evaluated in an unspecified order (a moved-from
        // any_io_executor throws bad_executor when the work guard is built).
        auto executor = ex;
        return asio::async_compose<Token, void(error_code)>(
            OneOp<std::decay_t<Fn>>{pre, std::forward<Fn>(fn), std::move(ex)}, token, executor);
    }

    template <typename Token, typename Fn>
    static void compose_connected(asio::any_io_executor ex, Token &&token, Fn &&fn) {
        auto executor = ex;
        return asio::async_compose<Token, void(error_code, bool)>(
            ConnectedOp<std::decay_t<Fn>>{std::forward<Fn>(fn), std::move(ex)}, token, executor);
    }

  private:
    // Model A: the writer is created and used on the connection thread. See
    // require_owner_thread in core/logging.h.
    void require_owner() const noexcept { require_owner_thread(m_owner_thread, "ResponseWriter"); }

    // cookie-octet = %x21 / %x23-2B / %x2D-3A / %x3C-5B / %x5D-7E
    static bool valid_cookie_value(std::string_view value) noexcept {
        for (unsigned char c : value) {
            const bool ok = c == 0x21 || (c >= 0x23 && c <= 0x2B) || (c >= 0x2D && c <= 0x3A) ||
                            (c >= 0x3C && c <= 0x5B) || (c >= 0x5D && c <= 0x7E);
            if (!ok)
                return false;
        }
        return true;
    }
    // Path-value allows SP; Domain (a host) does not. Both exclude ';', ','
    // and control bytes, which would inject another cookie attribute.
    static bool valid_cookie_attribute(std::string_view value, bool allow_space) noexcept {
        for (unsigned char c : value) {
            if (c < 0x20 || c == 0x7F || c == ';' || c == ',')
                return false;
            if (!allow_space && c == ' ')
                return false;
        }
        return true;
    }

    enum class Mode { Direct, Bodyless, Stream };
    enum class Phase { Start, Headers, Chunk, Trailers, Final };

    // Validates the trailer fields (CR/LF/NUL would inject a field line) and
    // starts the terminal chunk+trailer sequence. Runs on the owner executor,
    // after the operation has acquired the writer.
    void finish_trailers_now(Headers trailers, WriteCompletion done) {
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
        compose_finish_trailers(pre, std::string{}, std::move(trailers), std::move(sink), std::move(done));
    }

    // Synchronous pre-flight shared by send/send_bodyless/begin: field
    // rejection, single-head rule, defaults and the activity counter.
    error_code prepare_one_shot() {
        require_owner();
        if (m_field_rejected) {
            return make_error_code(asio::error::invalid_argument);
        }
        // 1xx is interim: it never ends the response, so a handler using
        // send()/begin() with status 103 would leave the client waiting for a
        // final response. The only interim API is send_continue().
        if (m_status >= 100 && m_status < 200) {
            SIMPLE_HTTP_ERROR_LOG("interim status {} cannot be a final response", m_status);
            m_field_rejected = true;
            return make_error_code(asio::error::invalid_argument);
        }
        // A response has exactly one head. Refusing a second one here is what
        // lets invoke_handler's 500 fallback check started() instead of
        // splicing a second status line onto the wire.
        if (m_started) {
            return make_error_code(asio::error::invalid_argument);
        }
        note_activity();
        apply_defaults();
        m_started = true;
        return {};
    }

    template <typename Token> void compose_send(error_code pre, Mode mode, std::string body, Token &&token) {
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
    void compose_finish_trailers(error_code pre, std::string data, Headers trailers, std::shared_ptr<ResponseSink> sink,
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
    // event/id/retry are single-line fields: a CR/LF/NUL inside one would end
    // the line and let the value inject its own "event:"/"id:" fields. Replace
    // them rather than emitting a frame a conforming parser reads differently
    // from what the caller wrote.
    static void append_sse_single_line(std::string &frame, std::string_view name, std::string_view value) {
        frame += name;
        frame += ": ";
        for (unsigned char c : value) {
            frame += (c == '\r' || c == '\n' || c == '\0') ? ' ' : static_cast<char>(c);
        }
        frame += '\n';
    }

    static std::string render_sse(std::string_view data, std::string_view event, std::string_view id,
                                  std::string_view retry) {
        std::string frame;
        if (!event.empty())
            append_sse_single_line(frame, "event", event);
        if (!id.empty())
            append_sse_single_line(frame, "id", id);
        if (!retry.empty())
            append_sse_single_line(frame, "retry", retry);
        // SSE lines end at CRLF, CR or LF. Splitting only on '\n' left a bare
        // '\r' inside a data line, which a parser turns into a new line — the
        // injection point for "data: x\rEvent: evil".
        if (data.empty()) {
            frame += "data: \n";
        } else {
            std::size_t pos = 0;
            while (pos < data.size()) {
                std::size_t end = pos;
                while (end < data.size() && data[end] != '\n' && data[end] != '\r')
                    ++end;
                frame += "data: ";
                frame.append(data.substr(pos, end - pos));
                frame += '\n';
                if (end == data.size())
                    break;
                if (data[end] == '\r' && end + 1 < data.size() && data[end + 1] == '\n')
                    ++end;
                pos = end + 1;
            }
        }
        frame += '\n';
        return frame;
    }

    static std::string render_sse_comment(std::string_view text) {
        std::string frame;
        std::size_t pos = 0;
        do {
            std::size_t end = pos;
            while (end < text.size() && text[end] != '\r' && text[end] != '\n')
                ++end;
            frame += ": ";
            frame.append(text.substr(pos, end - pos));
            frame += '\n';
            if (end == text.size())
                break;
            if (text[end] == '\r' && end + 1 < text.size() && text[end + 1] == '\n')
                ++end;
            pos = end + 1;
        } while (pos < text.size());
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
    std::weak_ptr<Lifecycle> m_life;
    bool m_life_managed{false};
    std::thread::id m_owner_thread{std::this_thread::get_id()};
    int m_status{status::ok};
    Headers m_headers;
    Headers m_trailers;
    bool m_field_rejected{false};
    bool m_started{false};
    std::atomic<std::uint64_t> m_activity{0};

    void note_activity() { m_activity.fetch_add(1, std::memory_order_relaxed); }
};

} // namespace simple_http
