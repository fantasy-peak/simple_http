#pragma once

// Public outbound client API (C++17 token-only port).
//
//   client.get(url).header("x", "y").send(token);       // void(error_code, Response)
//   client.post(url).body(payload).send(token);
//   client.open_stream(url, spec, token);               // void(error_code, Stream)
//
// This first port covers HTTP/1.1 (plaintext and TLS), redirects, cookies and
// the buffered convenience path. HTTP/2 multiplexing, outbound proxies,
// automatic decompression and the WebSocket client are the next phases; the
// types keep their shape so callers can migrate incrementally.

#include <algorithm>
#include <boost/asio.hpp>
#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/cancel_after.hpp>
#include <boost/asio/ssl.hpp>
#include <chrono>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "../core/base64.h"
#include "../core/content_encoding.h"
#include "../core/http_field.h"
#include "../core/http_method.h"
#include "../core/http_status.h"
#include "../core/result.h"
#include "../core/types.h"
#include "../core/url_encoding.h"
#include "../proto/headers.h"
#include "../proto/request.h"
#include "../transport/tcp_transport.h"
#include "../transport/tls_transport.h"
#include "client_config.h"
#include "client_stream.h"
#include "cookie_jar.h"
#include "decompressing_stream.h"
#include "h1_client.h"
#include "h2_client.h"
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
#include "h3_client.h"
#endif
#include "proxy.h"
#include "tls_client.h"
#include "url.h"
#include "ws_client.h"

namespace simple_http {

namespace asio = boost::asio;

namespace detail {
// Marks a request whose Accept-Encoding the client injected itself; only those
// responses are eligible for transparent decompression (Go's Transport rule).
struct AutoDecompressMark {};

// Lets the buffered send's overall request_timeout stop the exchange it is
// waiting on. Completing the caller while the inner hop still runs would let
// that hop touch a Client the caller has already destroyed (the Client may be
// a stack object; keep_alive() is only an alias for those).
struct HopCancel {
    bool cancelled{false};
    std::shared_ptr<ClientStream> stream;
};
} // namespace detail

// --- buffered response -------------------------------------------------------

class Response {
  public:
    Response() = default;
    // A buffered Response owns its bytes only. Keeping the (already finished)
    // stream here pinned the whole h1/h2/h3 session alive for as long as the
    // caller held the Response -- after Client::close() it was the last
    // reference to a closed connection.
    Response(detail::ResponseHead head, std::string body) : m_head(std::move(head)), m_body(std::move(body)) {}

    int status() const { return m_head.status; }
    Version version() const { return m_head.version; }
    const Headers &headers() const { return m_head.headers; }
    std::optional<std::string_view> header(std::string_view name) const { return m_head.header(name); }
    std::optional<std::string> header_owned(std::string_view name) const {
        auto value = m_head.header(name);
        return value ? std::optional<std::string>{*value} : std::nullopt;
    }
    bool ok() const { return m_head.status >= 200 && m_head.status < 300; }
    bool bodyless() const { return m_head.bodyless; }
    bool uncompressed() const { return m_head.uncompressed; }

    // 2xx -> empty error; anything else -> client_errc::http_status (the real
    // status stays readable through status()).
    error_code error_for_status() const { return ok() ? error_code{} : make_error_code(client_errc::http_status); }

    const std::string &text() const { return m_body; }
    const std::string &bytes() const { return m_body; }

    // A Response is only produced by the buffered send() path, which has
    // already drained the exchange into m_body. Completing from m_body here is
    // the contract; walking m_stream would ask a finished exchange for more
    // bytes (session_closed on h1, an empty EOF on h2/h3) and could even read
    // the next response when the connection has been reused.
    template <typename Token> auto async_read_all(std::size_t max_bytes, Token &&token) {
        if (max_bytes != 0 && m_body.size() > max_bytes) {
            return asio::async_initiate<Token, void(error_code, std::string)>(
                [](auto handler) { std::move(handler)(make_error_code(client_errc::body_too_large), std::string{}); },
                token);
        }
        return asio::async_initiate<Token, void(error_code, std::string)>(
            [body = m_body](auto handler) mutable { std::move(handler)(error_code{}, std::move(body)); }, token);
    }

  private:
    detail::ResponseHead m_head;
    std::string m_body;
};

// --- explicit stream ---------------------------------------------------------

struct StreamSpec {
    Method method{Method::Get};
    std::string target; // path + query; empty = the URL's own target
    Headers headers;
    // Ask the peer to acknowledge before the body is sent (Expect:
    // 100-continue). The interim 100 is delivered to the caller as the head of
    // the exchange: read_head() returns status 100 once the peer agreed, and the
    // caller then sends the body and reads again for the real response.
    bool expect_100_continue{false};
    // Request Connection: close (HTTP/1.1) / keep the connection out of the pool.
    bool close{false};
    // Per-stream budgets. 0 = the Client's defaults.
    std::chrono::milliseconds response_head_timeout{};
    std::chrono::milliseconds body_idle_timeout{};
    std::optional<HttpVersionPolicy> version{};
    // Automatic Expect: 100-continue: the request carries the header and the
    // first write()/finish() waits up to continue_timeout for the interim 100.
    bool auto_expect_continue{false};
    std::chrono::milliseconds continue_timeout{1000};
};

class Stream {
  public:
    // Constructed by the public client (open_stream). The budgets are captured
    // here so each read/head is bounded independently of later config changes.
    explicit Stream(std::shared_ptr<detail::ClientStream> stream, std::chrono::milliseconds head_timeout = {},
                    std::chrono::milliseconds idle_timeout = {}, bool auto_expect_continue = false,
                    std::chrono::milliseconds continue_timeout = std::chrono::milliseconds{1000})
        : m_stream(std::move(stream)), m_head_timeout(head_timeout), m_idle_timeout(idle_timeout),
          m_auto_expect_continue(auto_expect_continue), m_continue_timeout(continue_timeout) {}

    template <typename Token> auto async_write(std::string data, Token &&token) {
        return start_write_op(std::move(data), false, {}, std::forward<Token>(token));
    }
    template <typename Token> auto async_finish(std::string data, Token &&token) {
        return start_write_op(std::move(data), true, {}, std::forward<Token>(token));
    }
    template <typename Token> auto async_finish_trailers(Headers trailers, Token &&token) {
        return start_write_op(std::string{}, false, std::move(trailers), std::forward<Token>(token));
    }

    // Bounded by the response-head (TTFB) budget: a slow service fails here as
    // client_errc::response_head_timeout, independently of body pace.
    template <typename Token> auto async_read_head(Token &&token) {
        return read_head_with(m_head_timeout, std::forward<Token>(token));
    }
    // Each read is bounded by the body-idle budget (silence between chunks).
    template <typename Token> auto async_read(Token &&token) {
        return read_with(m_idle_timeout, std::forward<Token>(token));
    }
    template <typename Token> auto async_read_all(std::size_t max_bytes, Token &&token) {
        auto stream = m_stream;
        return asio::async_initiate<Token, void(error_code, std::string)>(
            [stream, max_bytes](auto handler) mutable { stream->async_read_all(max_bytes, std::move(handler)); },
            token);
    }
    const detail::ResponseHead &head() const { return m_stream->head(); }
    const Headers &trailers() const { return m_stream->response_trailers(); }
    bool finished() const { return m_stream->finished(); }

    // Cancels the exchange and releases the engine's single-connection gate
    // now: waiting for the destructor would block later requests to the same
    // origin on a gate nobody holds.
    void abort() {
        m_stream->async_cancel(asio::detached);
        m_stream->run_destroy_hook();
    }

    // Automatic Expect: 100-continue, performed once before the first body
    // byte. A 100 means "go"; a final response is surfaced as
    // expectation_failed; a timed-out wait sends the body anyway (Go's
    // ExpectContinueTimeout behaviour for a peer that ignores the header).
    template <typename Token> auto async_ensure_continue(Token &&token) {
        return asio::async_compose<Token, void(error_code)>(EnsureContinueOp{*this}, token, m_stream->head_executor());
    }

  private:
    template <typename Token> auto start_write_op(std::string data, bool finish, Headers trailers, Token &&token) {
        return asio::async_compose<Token, void(error_code)>(
            WriteOp{*this, std::move(data), finish, std::move(trailers)}, token, m_stream->head_executor());
    }

    template <typename Token> auto read_head_with(std::chrono::milliseconds timeout, Token &&token) {
        auto stream = m_stream;
        return asio::async_initiate<Token, void(error_code, detail::ResponseHead)>(
            [stream, timeout](auto handler) {
                struct State {
                    std::shared_ptr<detail::ClientStream> stream;
                    std::chrono::milliseconds timeout;
                    asio::any_completion_handler<void(error_code, detail::ResponseHead)> done;
                    asio::steady_timer timer;
                    asio::cancellation_signal signal;
                    bool finished{false};
                    bool timed_out{false};

                    State(std::shared_ptr<detail::ClientStream> s, std::chrono::milliseconds t)
                        : stream(std::move(s)), timeout(t), timer(stream->head_executor()) {}
                };
                auto state = std::make_shared<State>(stream, timeout);
                state->done = std::move(handler);
                auto finish = [state](error_code ec, detail::ResponseHead head) mutable {
                    if (state->finished)
                        return;
                    state->finished = true;
                    state->timer.cancel();
                    std::move(state->done)(ec, std::move(head));
                };
                if (state->timeout.count() > 0) {
                    state->timer.expires_after(state->timeout);
                    state->timer.async_wait([state](error_code ec) mutable {
                        if (ec || state->finished)
                            return;
                        state->timed_out = true;
                        state->signal.emit(asio::cancellation_type::terminal);
                    });
                }
                state->stream->async_read_head(asio::bind_cancellation_slot(
                    state->signal.slot(), [state, finish](error_code ec, detail::ResponseHead head) mutable {
                        if (ec == asio::error::operation_aborted && state->timed_out) {
                            std::move(finish)(make_error_code(client_errc::response_head_timeout),
                                              detail::ResponseHead{});
                            return;
                        }
                        std::move(finish)(ec, std::move(head));
                    }));
            },
            token);
    }

    template <typename Token> auto read_with(std::chrono::milliseconds timeout, Token &&token) {
        auto stream = m_stream;
        return asio::async_initiate<Token, void(error_code, ReadResult)>(
            [stream, timeout](auto handler) {
                struct State {
                    std::shared_ptr<detail::ClientStream> stream;
                    std::chrono::milliseconds timeout;
                    asio::any_completion_handler<void(error_code, ReadResult)> done;
                    asio::steady_timer timer;
                    asio::cancellation_signal signal;
                    bool finished{false};
                    bool timed_out{false};

                    State(std::shared_ptr<detail::ClientStream> s, std::chrono::milliseconds t)
                        : stream(std::move(s)), timeout(t), timer(stream->head_executor()) {}
                };
                auto state = std::make_shared<State>(stream, timeout);
                state->done = std::move(handler);
                auto finish = [state](error_code ec, ReadResult result) mutable {
                    if (state->finished)
                        return;
                    state->finished = true;
                    state->timer.cancel();
                    std::move(state->done)(ec, std::move(result));
                };
                if (state->timeout.count() > 0) {
                    state->timer.expires_after(state->timeout);
                    state->timer.async_wait([state](error_code ec) mutable {
                        if (ec || state->finished)
                            return;
                        state->timed_out = true;
                        state->signal.emit(asio::cancellation_type::terminal);
                    });
                }
                state->stream->async_read(asio::bind_cancellation_slot(
                    state->signal.slot(), [state, finish](error_code ec, ReadResult result) mutable {
                        if (ec == asio::error::operation_aborted && state->timed_out) {
                            std::move(finish)(make_error_code(client_errc::body_idle_timeout), ReadResult{});
                            return;
                        }
                        std::move(finish)(ec, std::move(result));
                    }));
            },
            token);
    }

    struct WriteOp {
        Stream &self;
        std::string data;
        bool finish;
        Headers trailers;
        int phase{0};

        asio::any_io_executor get_executor() const { return self.m_stream->head_executor(); }

        template <typename Self> void start(Self &s) {
            if (finish) {
                self.m_stream->async_finish(std::move(data), std::move(s));
            } else if (!trailers.empty()) {
                self.m_stream->async_finish_trailers(std::move(trailers), std::move(s));
            } else {
                self.m_stream->async_write(std::move(data), std::move(s));
            }
        }

        template <typename Self> void operator()(Self &s, error_code ec = {}) {
            if (phase == 0) {
                if (!self.m_auto_expect_continue || self.m_continue_checked) {
                    phase = 2;
                    start(s);
                    return;
                }
                phase = 1;
                self.async_ensure_continue(std::move(s));
                return;
            }
            if (phase == 1) {
                if (ec) {
                    s.complete(ec);
                    return;
                }
                phase = 2;
                start(s);
                return;
            }
            s.complete(ec);
        }
    };

    struct EnsureContinueOp {
        Stream &self;
        int phase{0};

        asio::any_io_executor get_executor() const { return self.m_stream->head_executor(); }

        template <typename Self> void operator()(Self &s, error_code ec = {}, detail::ResponseHead head = {}) {
            if (phase == 0) {
                phase = 1;
                self.m_continue_checked = true;
                if (!self.m_auto_expect_continue) {
                    s.complete(error_code{});
                    return;
                }
                // h1 and h2 surface the interim 100; h3 servers have no 1xx API,
                // so the client sends the body without waiting.
                if (self.m_stream->version() != Version::Http11 && self.m_stream->version() != Version::Http2) {
                    s.complete(error_code{});
                    return;
                }
                self.read_head_with(self.m_continue_timeout, std::move(s));
                return;
            }
            if (ec == make_error_code(client_errc::response_head_timeout)) {
                // No interim answer in time: proceed with the body, as Go does
                // with a peer that ignores the header.
                s.complete(error_code{});
                return;
            }
            if (ec) {
                s.complete(ec);
                return;
            }
            if (head.status == status::continue_) {
                // The interim head was handed to us; the final response still
                // follows, so the next read_head() must parse it.
                self.m_stream->clear_head_cache();
                s.complete(error_code{});
                return;
            }
            s.complete(make_error_code(client_errc::expectation_failed));
        }
    };

    struct ReadHeadOp {
        Stream &self;
        std::chrono::milliseconds timeout;
        int phase{0};

        asio::any_io_executor get_executor() const { return self.m_stream->head_executor(); }

        template <typename Self> void operator()(Self &s, error_code ec = {}, detail::ResponseHead head = {}) {
            if (phase == 0) {
                phase = 1;
                if (timeout.count() > 0) {
                    self.m_stream->async_read_head(asio::cancel_after(timeout, std::move(s)));
                } else {
                    self.m_stream->async_read_head(std::move(s));
                }
                return;
            }
            if (ec == asio::error::operation_aborted || ec == asio::error::timed_out) {
                s.complete(make_error_code(client_errc::response_head_timeout), detail::ResponseHead{});
                return;
            }
            s.complete(ec, std::move(head));
        }
    };

    struct ReadOp {
        Stream &self;
        std::chrono::milliseconds timeout;
        int phase{0};

        asio::any_io_executor get_executor() const { return self.m_stream->head_executor(); }

        template <typename Self> void operator()(Self &s, error_code ec = {}, ReadResult result = {}) {
            if (phase == 0) {
                phase = 1;
                if (timeout.count() > 0) {
                    self.m_stream->async_read(asio::cancel_after(timeout, std::move(s)));
                } else {
                    self.m_stream->async_read(std::move(s));
                }
                return;
            }
            if (ec == asio::error::operation_aborted || ec == asio::error::timed_out) {
                s.complete(make_error_code(client_errc::body_idle_timeout), ReadResult{});
                return;
            }
            s.complete(ec, std::move(result));
        }
    };

    std::shared_ptr<detail::ClientStream> m_stream;
    std::chrono::milliseconds m_head_timeout{};
    std::chrono::milliseconds m_idle_timeout{};
    bool m_auto_expect_continue{false};
    std::chrono::milliseconds m_continue_timeout{1000};
    bool m_continue_checked{false};
};

class Client;
class RequestBuilder;

// --- client ------------------------------------------------------------------

class Client : public std::enable_shared_from_this<Client> {
  public:
    Client(asio::any_io_executor executor, ClientConfig config = {})
        : m_executor(std::move(executor)), m_config(std::move(config)) {
        if (m_config.tls.verify_peer || !m_config.tls.cert_chain_file.empty())
            m_tls_context = detail::make_client_ssl_context(m_config.tls);
    }
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    RequestBuilder get(std::string url) const;
    RequestBuilder post(std::string url) const;
    RequestBuilder put(std::string url) const;
    RequestBuilder del(std::string url) const;
    RequestBuilder head(std::string url) const;
    RequestBuilder patch(std::string url) const;
    RequestBuilder request(Method method, std::string url) const;

    const ClientConfig &config() const { return m_config; }
    asio::any_io_executor executor() const { return m_executor; }

    // Closes the kept connection (if any). Call it before dropping the
    // io_context: a multiplexed session keeps a read pending for keep-alive, so
    // the context would not otherwise run out of work.
    void close() const {
        if (m_cached_session) {
            m_cached_session->close();
            m_cached_session.reset();
        }
    }

    // Buffered send: follows redirects, applies the cookie jar, reads the whole
    // body (bounded by `max_body_bytes`, 0 = the configured limit).
    template <typename Token>
    auto async_send(std::shared_ptr<Request> request, std::string url, std::size_t max_body_bytes,
                    Token &&token) const {
        return async_send(std::move(request), std::move(url), max_body_bytes, {}, {}, std::forward<Token>(token));
    }

    // Buffered send with the layered per-request budgets. `response_head_timeout`
    // / `body_idle_timeout` (0 = the Client's defaults) are the RequestBuilder
    // overrides; ClientConfig::request_timeout still bounds the whole call.
    template <typename Token>
    auto async_send(std::shared_ptr<Request> request, std::string url, std::size_t max_body_bytes,
                    std::chrono::milliseconds response_head_timeout, std::chrono::milliseconds body_idle_timeout,
                    Token &&token) const {
        return async_send(std::move(request), std::move(url), max_body_bytes, response_head_timeout, body_idle_timeout,
                          std::nullopt, std::forward<Token>(token));
    }

    template <typename Token>
    auto async_send(std::shared_ptr<Request> request, std::string url, std::size_t max_body_bytes,
                    std::chrono::milliseconds response_head_timeout, std::chrono::milliseconds body_idle_timeout,
                    std::optional<HttpVersionPolicy> version, Token &&token) const {
        const HttpVersionPolicy effective = version ? *version : m_config.default_version;
        auto keep = keep_alive();
        return asio::async_initiate<Token, void(error_code, Response)>(
            [keep, request = std::move(request), url = std::move(url), max_body_bytes, response_head_timeout,
             body_idle_timeout, effective](auto handler) mutable {
                const std::chrono::milliseconds overall = keep->m_config.request_timeout;
                if (overall.count() <= 0) {
                    keep->send_hop(std::move(request), std::move(url), max_body_bytes, 0, 0, response_head_timeout,
                                   body_idle_timeout, effective, nullptr, std::move(handler));
                    return;
                }
                // Overall budget: the per-phase deadlines are a subset, but a
                // server that dribbles bytes forever must still be cut off.
                // The caller is notified immediately. The still-running inner
                // hop must not touch this Client afterwards (a stack Client
                // dies with the awaitable frame), so it observes `cancelled`
                // through a shared flag before every use of `self`; the stream
                // handle lets the timer abort the pending read as well.
                struct State {
                    asio::any_completion_handler<void(error_code, Response)> done;
                    std::shared_ptr<asio::steady_timer> timer;
                    std::shared_ptr<detail::HopCancel> cancel{std::make_shared<detail::HopCancel>()};
                    bool finished{false};
                };
                auto state = std::make_shared<State>();
                state->done = std::move(handler);
                state->timer = std::make_shared<asio::steady_timer>(keep->m_executor);
                state->timer->expires_after(overall);
                auto finish = [state](error_code ec, Response response) {
                    if (state->finished) {
                        return;
                    }
                    state->finished = true;
                    state->timer->cancel();
                    auto completion = std::move(state->done);
                    std::move(completion)(ec, std::move(response));
                };
                state->timer->async_wait([state, finish](error_code ec) mutable {
                    if (ec || state->finished) {
                        return;
                    }
                    state->cancel->cancelled = true;
                    if (state->cancel->stream) {
                        state->cancel->stream->async_cancel(asio::detached);
                        state->cancel->stream.reset();
                    }
                    finish(make_error_code(client_errc::request_timeout), Response{});
                });
                keep->send_hop(std::move(request), std::move(url), max_body_bytes, 0, 0, response_head_timeout,
                               body_idle_timeout, effective, state->cancel,
                               [finish](error_code ec, Response response) mutable { finish(ec, std::move(response)); });
            },
            token);
    }

    // Client-side WebSocket over HTTP/1.1. Accepts ws:// and wss:// URLs.
    template <typename Token> auto async_open_websocket(std::string url, WebSocketSpec spec, Token &&token) {
        auto keep = keep_alive();
        return asio::async_initiate<Token, void(error_code, std::shared_ptr<WebSocket>)>(
            [keep, url = std::move(url), spec = std::move(spec)](auto handler) mutable {
                keep->open_ws_hop(std::move(url), std::move(spec), std::move(handler));
            },
            token);
    }

    // Explicit stream. No redirects, cookies or retries are applied (matching
    // the C++23 open_stream contract).
    template <typename Token>
    auto async_open_stream(std::string url, StreamSpec spec, std::size_t max_body_bytes, Token &&token) const {
        auto keep = keep_alive();
        return asio::async_initiate<Token, void(error_code, std::shared_ptr<Stream>)>(
            [keep, url = std::move(url), spec = std::move(spec), max_body_bytes](auto handler) mutable {
                keep->open_stream_hop(std::move(url), std::move(spec), max_body_bytes, std::move(handler));
            },
            token);
    }

  private:
    friend class RequestBuilder;
    using DialDone = asio::any_completion_handler<void(error_code, std::shared_ptr<detail::ClientSession>)>;

    void send_hop(std::shared_ptr<Request> request, std::string url, std::size_t max_body_bytes, std::size_t redirects,
                  std::size_t retries, std::chrono::milliseconds response_head_timeout,
                  std::chrono::milliseconds body_idle_timeout, HttpVersionPolicy version,
                  std::shared_ptr<detail::HopCancel> cancel,
                  asio::any_completion_handler<void(error_code, Response)> done) const;
    void open_stream_hop(std::string url, StreamSpec spec, std::size_t max_body_bytes,
                         asio::any_completion_handler<void(error_code, std::shared_ptr<Stream>)> done) const;
    void dial(const detail::Url &url, HttpVersionPolicy version, DialDone done) const;
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
    void dial_h3(const detail::Url &url, DialDone done) const;
#endif
    void dial_via_proxy(const detail::Url &url, const ProxyConfig &proxy, HttpVersionPolicy version,
                        DialDone done) const;
    // Builds the h1/h2 session for an already-connected byte stream. `version`
    // is the per-request policy (already resolved against the Client default).
    template <typename Transport>
    void start_session(std::shared_ptr<Transport> transport, const detail::Url &url, HttpVersionPolicy version,
                       DialDone done) const;
    // Builds the session for a TLS origin from the ALPN result (h2 or h1).
    template <typename Transport>
    void start_negotiated_session(std::shared_ptr<Transport> transport, const detail::Url &url,
                                  HttpVersionPolicy version, DialDone done) const;
    // Runs the origin TLS handshake over an already-connected (possibly nested)
    // transport, then starts the negotiated session.
    template <typename Transport>
    void tls_handshake_and_start(std::shared_ptr<Transport> transport, const detail::Url &url,
                                 HttpVersionPolicy version, DialDone done) const;
    void open_ws_hop(std::string url, WebSocketSpec spec,
                     asio::any_completion_handler<void(error_code, std::shared_ptr<WebSocket>)> done) const;
    void open_ws_via_proxy(const detail::Url &url, const ProxyConfig &proxy, std::string path, std::string authority,
                           WebSocketSpec spec,
                           asio::any_completion_handler<void(error_code, std::shared_ptr<WebSocket>)> done) const;
    template <typename Transport>
    void begin_ws_upgrade(std::shared_ptr<Transport> transport, std::string authority, std::string path,
                          WebSocketSpec spec,
                          asio::any_completion_handler<void(error_code, std::shared_ptr<WebSocket>)> done) const;
    template <typename Transport>
    void
    ws_tls_handshake_and_start(std::shared_ptr<Transport> transport, std::string authority, std::string path,
                               WebSocketSpec spec, std::string sni,
                               asio::any_completion_handler<void(error_code, std::shared_ptr<WebSocket>)> done) const;
    // An owning reference to this Client when it is owned by a shared_ptr, or a
    // non-owning alias when it lives on the stack. Every asynchronous callback
    // captures this instead of a raw `this`, so a heap-allocated Client cannot
    // be destroyed while a dial/send/WebSocket operation is still in flight
    // (ASan found a UAF in dial_via_proxy's DNS completion). A stack Client
    // relies on the awaitable contract: it must outlive the operation.
    std::shared_ptr<Client> keep_alive() const {
        // The non-const overload is used deliberately: the const overload of
        // weak_from_this() yields weak_ptr<const Client>, and every call site
        // is a const member on an object that is not actually const.
        if (auto keep = const_cast<Client *>(this)->weak_from_this().lock()) {
            return keep;
        }
        return std::shared_ptr<Client>(std::shared_ptr<Client>{}, const_cast<Client *>(this));
    }

    std::shared_ptr<detail::ClientSession> take_cached(std::string_view origin, HttpVersionPolicy version) const;
    void cache(std::string origin, std::shared_ptr<detail::ClientSession> session) const;

    asio::any_io_executor m_executor;
    ClientConfig m_config;
    std::shared_ptr<asio::ssl::context> m_tls_context;
    mutable std::string m_cached_origin;
    mutable std::shared_ptr<detail::ClientSession> m_cached_session;
};

class RequestBuilder {
  public:
    RequestBuilder(const Client *client, Method method, std::string url)
        : m_client(client), m_method(method), m_url(std::move(url)) {}

    RequestBuilder &query(std::initializer_list<std::pair<std::string_view, std::string_view>> kv) {
        if (kv.size() != 0) {
            std::string addition;
            for (const auto &[key, value] : kv) {
                if (!addition.empty())
                    addition.push_back('&');
                addition += query_escape(key);
                addition.push_back('=');
                addition += query_escape(value);
            }
            m_url += (m_url.find('?') == std::string::npos ? '?' : '&');
            m_url += addition;
        }
        return *this;
    }
    RequestBuilder &header(std::string_view name, std::string value) {
        m_headers.add(std::string{name}, std::move(value));
        return *this;
    }
    RequestBuilder &headers(Headers h) {
        m_headers = std::move(h);
        return *this;
    }
    RequestBuilder &content_type(std::string_view ct) { return header("content-type", std::string{ct}); }
    RequestBuilder &basic_auth(std::string user, std::string pass) {
        return header(field::authorization, "Basic " + base64_encode(user + ":" + pass));
    }
    RequestBuilder &bearer_auth(std::string token) {
        return header(field::authorization, "Bearer " + std::move(token));
    }
    RequestBuilder &body(std::string value) {
        m_body = std::move(value);
        return *this;
    }
    RequestBuilder &max_body_bytes(std::size_t value) {
        m_max_body_bytes = value;
        return *this;
    }
    RequestBuilder &timeout(std::chrono::milliseconds head, std::chrono::milliseconds idle = {}) {
        m_head_timeout = head;
        m_idle_timeout = idle;
        return *this;
    }
    RequestBuilder &version(HttpVersionPolicy policy) {
        m_version = policy;
        return *this;
    }

    template <typename Token> auto send(Token &&token) {
        auto request = std::make_shared<Request>(Version::Http11, m_client->executor());
        request->set_method(m_method);
        request->mutable_headers() = m_headers;
        const auto &config = m_client->config();
        // Advertise only codings this build can actually decode; advertising an
        // unsupported one makes the server compress a body we would then have to
        // pass through raw (or fail to decode).
        const auto encodings = detail::normalize_accept_encodings(config.accept_encodings);
        if (config.auto_decompress && m_method != Method::Head && !request->header(field::accept_encoding) &&
            !request->header(field::range) && !encodings.empty()) {
            request->mutable_headers().add(std::string{field::accept_encoding}, detail::join_encodings(encodings));
            request->set_state(detail::AutoDecompressMark{});
        }
        if (!m_body.empty())
            request->set_body(m_body);
        if (m_method == Method::Head)
            request->set_close(false);
        const std::size_t cap = m_max_body_bytes != 0 ? m_max_body_bytes : m_client->config().limits.max_body_bytes;
        return m_client->async_send(std::move(request), m_url, cap, m_head_timeout, m_idle_timeout, m_version,
                                    std::forward<Token>(token));
    }

  private:
    const Client *m_client;
    Method m_method;
    std::string m_url;
    Headers m_headers;
    std::string m_body;
    std::size_t m_max_body_bytes{0};
    std::chrono::milliseconds m_head_timeout{};
    std::chrono::milliseconds m_idle_timeout{};
    std::optional<HttpVersionPolicy> m_version;
};

inline RequestBuilder Client::get(std::string url) const { return RequestBuilder(this, Method::Get, std::move(url)); }
inline RequestBuilder Client::post(std::string url) const { return RequestBuilder(this, Method::Post, std::move(url)); }
inline RequestBuilder Client::put(std::string url) const { return RequestBuilder(this, Method::Put, std::move(url)); }
inline RequestBuilder Client::del(std::string url) const {
    return RequestBuilder(this, Method::Delete, std::move(url));
}
inline RequestBuilder Client::head(std::string url) const { return RequestBuilder(this, Method::Head, std::move(url)); }
inline RequestBuilder Client::patch(std::string url) const {
    return RequestBuilder(this, Method::Patch, std::move(url));
}
inline RequestBuilder Client::request(Method method, std::string url) const {
    return RequestBuilder(this, method, std::move(url));
}

// --- implementation ----------------------------------------------------------

inline std::shared_ptr<detail::ClientSession> Client::take_cached(std::string_view origin,
                                                                  HttpVersionPolicy version) const {
    if (m_cached_session && m_cached_origin == origin && m_cached_session->alive()) {
        auto session = m_cached_session;
        if (version != HttpVersionPolicy::Auto) {
            const Version wanted = version == HttpVersionPolicy::Http11  ? Version::Http11
                                   : version == HttpVersionPolicy::Http2 ? Version::Http2
                                                                         : Version::Http3;
            if (session->version() != wanted) {
                // Do not reuse a connection of the wrong version. Keep it in
                // the cache: a failed dial then leaves the previous (still
                // valid) connection available for a later Auto request.
                return nullptr;
            }
        }
        // A multiplexed session (h2/h3) can take another stream as long as it is
        // keepable; a single-stream session (h1) needs its previous exchange
        // finished before it can be reused. The old check refused multiplexed
        // sessions outright, so every h2/h3 request redialed and abandoned the
        // previous connection (which then sat on its QUIC idle timer).
        const bool usable = session->multiplexes() ? session->keepable() : session->reusable();
        if (usable) {
            return session;
        }
        // Do NOT close a session that still has an exchange in flight: a second
        // concurrent request on the same Client used to tear down the first
        // one's connection (connection_reset). Dial a throwaway connection
        // instead and leave the cached session for its owner to finish. A
        // draining h2 session (GOAWAY) is the same case: it cannot take new
        // streams but must be allowed to finish the ones the peer promised.
        if (session->has_active_exchange()) {
            return nullptr;
        }
        session->close();
        m_cached_session.reset();
    }
    return nullptr;
}

inline void Client::cache(std::string origin, std::shared_ptr<detail::ClientSession> session) const {
    m_cached_origin = std::move(origin);
    m_cached_session = std::move(session);
}

// --- session construction -----------------------------------------------------

template <typename Transport>
inline void Client::start_session(std::shared_ptr<Transport> transport, const detail::Url &url,
                                  HttpVersionPolicy version, DialDone done) const {
    if (version == HttpVersionPolicy::Http2) {
        auto h2_target = url.to_target();
        h2_target.version = HttpVersionPolicy::Http2;
        auto session = std::make_shared<detail::Http2ClientSession<Transport>>(transport, h2_target, m_config.limits,
                                                                               m_config.idle_timeout);
        session->start([session, done = std::move(done)](error_code sec) mutable {
            if (sec) {
                std::move(done)(sec, nullptr);
                return;
            }
            std::move(done)(error_code{}, session);
        });
        return;
    }
    if (version == HttpVersionPolicy::Http3) {
        std::move(done)(make_error_code(client_errc::version_not_negotiated), nullptr);
        return;
    }
    auto session = std::make_shared<detail::Http1ClientSession<Transport>>(transport, url.authority(), m_config.limits);
    std::move(done)(error_code{}, session);
}

template <typename Transport>
inline void Client::start_negotiated_session(std::shared_ptr<Transport> transport, const detail::Url &url,
                                             HttpVersionPolicy version, DialDone done) const {
    if (version == HttpVersionPolicy::Http11 && transport->alpn_selected() == "h2") {
        std::move(done)(make_error_code(client_errc::version_not_negotiated), nullptr);
        return;
    }
    if (version == HttpVersionPolicy::Http2 && transport->alpn_selected() != "h2") {
        std::move(done)(make_error_code(client_errc::version_not_negotiated), nullptr);
        return;
    }
    if (transport->alpn_selected() == "h2") {
        auto h2_target = url.to_target();
        h2_target.version = HttpVersionPolicy::Http2;
        auto session = std::make_shared<detail::Http2ClientSession<Transport>>(transport, h2_target, m_config.limits,
                                                                               m_config.idle_timeout);
        session->start([session, done = std::move(done)](error_code sec) mutable {
            if (sec) {
                std::move(done)(sec, nullptr);
                return;
            }
            std::move(done)(error_code{}, session);
        });
        return;
    }
    auto session = std::make_shared<detail::Http1ClientSession<Transport>>(transport, url.authority(), m_config.limits);
    std::move(done)(error_code{}, session);
}

template <typename Transport>
inline void Client::tls_handshake_and_start(std::shared_ptr<Transport> transport, const detail::Url &url,
                                            HttpVersionPolicy version, DialDone done) const {
    detail::ClientTlsHandshake hs;
    // SNI override first: it is what a VIP/LB deployment needs, and the
    // certificate is verified against the same name.
    hs.sni = m_config.tls.sni_override.empty() ? url.host : m_config.tls.sni_override;
    hs.verify_host = m_config.tls.verify_host;
    // TlsClientConfig::alpn overrides the policy-derived list. With no
    // override, the caller's version policy decides (offering http/1.1 to an
    // explicit h2 request would let the peer pick a refused version).
    auto alpn_target = url.to_target();
    alpn_target.version = version;
    hs.alpn_wire = detail::alpn_wire_list(alpn_target, m_config.tls);
    auto self = keep_alive();
    detail::async_tls_client_handshake_bounded(
        transport, hs, m_config.connect_timeout,
        [self, transport, url, version, done = std::move(done)](error_code hec) mutable {
            if (hec) {
                transport->close();
                std::move(done)(hec, nullptr);
                return;
            }
            self->start_negotiated_session(transport, url, version, std::move(done));
        });
}

namespace detail {

// Connects to `endpoints`, bounding the attempt by `limit` (0 = unbounded).
// On timeout the socket is closed, which makes the composed async_connect
// finish with operation_aborted; that completion is swallowed and the caller
// sees client_errc::connect_timeout instead. The endpoint list is kept alive by
// the shared state because async_connect only stores iterators into it.
inline void async_connect_bounded(std::shared_ptr<asio::ip::tcp::socket> socket,
                                  std::shared_ptr<std::vector<asio::ip::tcp::endpoint>> endpoints,
                                  std::chrono::milliseconds limit, asio::any_io_executor ex,
                                  asio::any_completion_handler<void(error_code, asio::ip::tcp::endpoint)> done) {
    struct State {
        std::shared_ptr<asio::ip::tcp::socket> socket;
        std::shared_ptr<std::vector<asio::ip::tcp::endpoint>> endpoints;
        std::shared_ptr<asio::steady_timer> timer;
        asio::any_completion_handler<void(error_code, asio::ip::tcp::endpoint)> done;
        bool finished{false};
    };

    auto state = std::make_shared<State>();
    state->socket = std::move(socket);
    state->endpoints = std::move(endpoints);
    state->done = std::move(done);

    auto finish = [state](error_code ec, asio::ip::tcp::endpoint peer) {
        if (state->finished) {
            return;
        }
        state->finished = true;
        if (state->timer) {
            state->timer->cancel();
        }
        auto completion = std::move(state->done);
        std::move(completion)(ec, std::move(peer));
    };

    if (limit.count() > 0) {
        state->timer = std::make_shared<asio::steady_timer>(ex);
        state->timer->expires_after(limit);
        state->timer->async_wait([state, finish](error_code ec) mutable {
            if (ec) {
                return; // the connect finished first and cancelled the timer
            }
            error_code ignored;
            state->socket->cancel(ignored);
            state->socket->close(ignored);
            finish(make_error_code(client_errc::connect_timeout), asio::ip::tcp::endpoint{});
        });
    }

    asio::async_connect(
        *state->socket, *state->endpoints,
        [state, finish](error_code ec, const asio::ip::tcp::endpoint &peer) mutable { finish(ec, peer); });
}

// Resolves with the whole-dial connect budget. The underlying resolver is not
// cancellable, but the one-shot completion means a late answer is ignored and
// the caller stops waiting. Without this, a black-holed DNS server (or a custom
// ResolveFn that never calls back) hung dial() forever.
struct ResolveBoundedState {
    std::shared_ptr<asio::steady_timer> timer;
    asio::any_completion_handler<void(error_code, std::vector<asio::ip::tcp::endpoint>)> done;
    bool finished{false};
};

inline void
async_resolve_bounded(ResolveFn resolve, std::string host, std::uint16_t port, asio::any_io_executor ex,
                      std::chrono::milliseconds limit,
                      asio::any_completion_handler<void(error_code, std::vector<asio::ip::tcp::endpoint>)> done) {
    auto state = std::make_shared<ResolveBoundedState>();
    state->done = std::move(done);
    auto finish = [state](error_code ec, std::vector<asio::ip::tcp::endpoint> endpoints) {
        if (state->finished)
            return;
        state->finished = true;
        if (state->timer)
            state->timer->cancel();
        auto completion = std::move(state->done);
        std::move(completion)(ec, std::move(endpoints));
    };
    if (limit.count() > 0) {
        state->timer = std::make_shared<asio::steady_timer>(ex);
        state->timer->expires_after(limit);
        state->timer->async_wait([state](error_code ec) {
            if (ec || state->finished)
                return;
            state->finished = true;
            auto completion = std::move(state->done);
            std::move(completion)(make_error_code(client_errc::connect_timeout),
                                  std::vector<asio::ip::tcp::endpoint>{});
        });
    }
    async_resolve_endpoints(std::move(resolve), std::move(host), port, ex,
                            [finish](error_code ec, std::vector<asio::ip::tcp::endpoint> endpoints) mutable {
                                finish(ec, std::move(endpoints));
                            });
}

// Bounds a CONNECT/SOCKS5 handshake. The tunnel operations touch a socket the
// caller owns, so timeout means "close that socket, report connect_timeout";
// their late completion is ignored. IPv6-only/WSL2 paths can otherwise drop a
// SYN or a proxy reply and hang before TLS even starts.
struct TunnelBoundedState {
    std::shared_ptr<asio::steady_timer> timer;
    asio::any_completion_handler<void(error_code)> done;
    bool finished{false};
};

inline void async_proxy_tunnel_bounded(asio::any_io_executor ex, std::chrono::milliseconds limit,
                                       std::function<void()> cancel,
                                       asio::any_completion_handler<void(error_code)> done,
                                       std::function<void(asio::any_completion_handler<void(error_code)>)> start) {
    auto state = std::make_shared<TunnelBoundedState>();
    state->done = std::move(done);
    auto finish = [state](error_code ec) {
        if (state->finished)
            return;
        state->finished = true;
        if (state->timer)
            state->timer->cancel();
        auto completion = std::move(state->done);
        std::move(completion)(ec);
    };
    if (limit.count() > 0) {
        state->timer = std::make_shared<asio::steady_timer>(ex);
        state->timer->expires_after(limit);
        state->timer->async_wait([state, cancel = std::move(cancel)](error_code ec) mutable {
            if (ec || state->finished)
                return;
            state->finished = true;
            cancel();
            auto completion = std::move(state->done);
            std::move(completion)(make_error_code(client_errc::connect_timeout));
        });
    }
    start(asio::any_completion_handler<void(error_code)>([finish](error_code ec) mutable { finish(ec); }));
}

// Reads a response head under a deadline. The timer emits into a cancellation
// signal bound to the read, so the pending socket read is aborted and the
// caller sees `reason` instead of the inner operation_aborted. A zero limit
// disables the deadline.
inline void async_read_head_bounded(std::shared_ptr<ClientStream> stream, std::chrono::milliseconds limit,
                                    client_errc reason,
                                    asio::any_completion_handler<void(error_code, ResponseHead)> done) {
    struct State {
        std::shared_ptr<ClientStream> stream;
        client_errc reason;
        asio::any_completion_handler<void(error_code, ResponseHead)> done;
        asio::steady_timer timer;
        asio::cancellation_signal signal;
        bool finished{false};
        bool timed_out{false};

        State(std::shared_ptr<ClientStream> s, client_errc r)
            : stream(std::move(s)), reason(r), timer(stream->head_executor()) {}
    };

    auto state = std::make_shared<State>(std::move(stream), reason);
    state->done = std::move(done);
    auto finish = [state](error_code ec, ResponseHead head) {
        if (state->finished) {
            return;
        }
        state->finished = true;
        state->timer.cancel();
        auto completion = std::move(state->done);
        std::move(completion)(ec, std::move(head));
    };
    if (limit.count() > 0) {
        state->timer.expires_after(limit);
        state->timer.async_wait([state](error_code ec) {
            if (ec || state->finished) {
                return;
            }
            state->timed_out = true;
            state->signal.emit(asio::cancellation_type::terminal);
        });
    }
    state->stream->async_read_head(
        asio::bind_cancellation_slot(state->signal.slot(), [state, finish](error_code ec, ResponseHead head) mutable {
            if (ec == asio::error::operation_aborted && state->timed_out) {
                finish(make_error_code(state->reason), ResponseHead{});
                return;
            }
            finish(ec, std::move(head));
        }));
}

// Reads the final (non-informational) response head. The h1/h2 sessions surface
// an interim 100 that answers Expect: 100-continue as the exchange head; the
// buffered convenience path must discard it, clear the stream's head cache and
// read again, or it would return status 100 with the real response's body.
struct FinalHeadState {
    std::shared_ptr<ClientStream> stream;
    std::chrono::milliseconds limit;
    client_errc reason;
    asio::any_completion_handler<void(error_code, ResponseHead)> done;
};

inline void read_final_head_next(const std::shared_ptr<FinalHeadState> &state);

inline void async_read_final_head_bounded(std::shared_ptr<ClientStream> stream, std::chrono::milliseconds limit,
                                          client_errc reason,
                                          asio::any_completion_handler<void(error_code, ResponseHead)> done) {
    auto state = std::make_shared<FinalHeadState>();
    state->stream = std::move(stream);
    state->limit = limit;
    state->reason = reason;
    state->done = std::move(done);
    read_final_head_next(state);
}

inline void read_final_head_next(const std::shared_ptr<FinalHeadState> &state) {
    async_read_head_bounded(state->stream, state->limit, state->reason,
                            [state](error_code ec, ResponseHead head) mutable {
                                if (ec) {
                                    auto completion = std::move(state->done);
                                    std::move(completion)(ec, ResponseHead{});
                                    return;
                                }
                                if (head.status >= 100 && head.status < 200) {
                                    state->stream->clear_head_cache();
                                    read_final_head_next(state);
                                    return;
                                }
                                auto completion = std::move(state->done);
                                std::move(completion)(error_code{}, std::move(head));
                            });
}

// Same shape for a single body read (per-read idle deadline).
inline void async_read_bounded(std::shared_ptr<ClientStream> stream, std::chrono::milliseconds limit,
                               client_errc reason, asio::any_completion_handler<void(error_code, ReadResult)> done) {
    struct State {
        std::shared_ptr<ClientStream> stream;
        client_errc reason;
        asio::any_completion_handler<void(error_code, ReadResult)> done;
        asio::steady_timer timer;
        asio::cancellation_signal signal;
        bool finished{false};
        bool timed_out{false};

        State(std::shared_ptr<ClientStream> s, client_errc r)
            : stream(std::move(s)), reason(r), timer(stream->head_executor()) {}
    };

    auto state = std::make_shared<State>(std::move(stream), reason);
    state->done = std::move(done);
    auto finish = [state](error_code ec, ReadResult result) {
        if (state->finished) {
            return;
        }
        state->finished = true;
        state->timer.cancel();
        auto completion = std::move(state->done);
        std::move(completion)(ec, std::move(result));
    };
    if (limit.count() > 0) {
        state->timer.expires_after(limit);
        state->timer.async_wait([state](error_code ec) {
            if (ec || state->finished) {
                return;
            }
            state->timed_out = true;
            state->signal.emit(asio::cancellation_type::terminal);
        });
    }
    state->stream->async_read(
        asio::bind_cancellation_slot(state->signal.slot(), [state, finish](error_code ec, ReadResult result) mutable {
            if (ec == asio::error::operation_aborted && state->timed_out) {
                finish(make_error_code(state->reason), ReadResult{});
                return;
            }
            finish(ec, std::move(result));
        }));
}

// Read the whole body with a per-read idle deadline and an optional cap.
// The pending read owns the state; the continuation is an ordinary function
// rather than a shared std::function, which would capture the state it is
// stored in and never be released.
struct ReadAllBoundedState {
    std::shared_ptr<ClientStream> stream;
    std::size_t max_bytes{0};
    std::chrono::milliseconds idle_limit;
    asio::any_completion_handler<void(error_code, std::string)> done;
    std::string out;
    bool finished{false};
};

inline void read_all_bounded_next(const std::shared_ptr<ReadAllBoundedState> &state);

inline void async_read_all_bounded(std::shared_ptr<ClientStream> stream, std::size_t max_bytes,
                                   std::chrono::milliseconds idle_limit,
                                   asio::any_completion_handler<void(error_code, std::string)> done) {
    auto state = std::make_shared<ReadAllBoundedState>();
    state->stream = std::move(stream);
    state->max_bytes = max_bytes;
    state->idle_limit = idle_limit;
    state->done = std::move(done);
    read_all_bounded_next(state);
}

inline void read_all_bounded_next(const std::shared_ptr<ReadAllBoundedState> &state) {
    async_read_bounded(state->stream, state->idle_limit, client_errc::body_idle_timeout,
                       [state](error_code ec, ReadResult chunk) {
                           if (state->finished) {
                               return;
                           }
                           if (ec) {
                               state->finished = true;
                               auto completion = std::move(state->done);
                               std::move(completion)(ec, std::string{});
                               return;
                           }
                           if (chunk.eof) {
                               state->finished = true;
                               auto completion = std::move(state->done);
                               std::move(completion)(error_code{}, std::move(state->out));
                               return;
                           }
                           if (state->max_bytes != 0 && state->out.size() + chunk.data.size() > state->max_bytes) {
                               state->finished = true;
                               auto completion = std::move(state->done);
                               std::move(completion)(make_error_code(client_errc::body_too_large), std::string{});
                               return;
                           }
                           state->out.append(chunk.data);
                           read_all_bounded_next(state);
                       });
}

// Discards (rather than buffers) a response body up to `max_bytes`, then
// aborts the exchange. Used for redirect responses, whose body is not needed
// and must never be accumulated: a 3xx with a multi-gigabyte body used to be
// read into a std::string with no cap at all.
struct DiscardBoundedState {
    std::shared_ptr<ClientStream> stream;
    std::size_t max_bytes{0};
    std::size_t seen{0};
    std::chrono::milliseconds idle_limit;
    asio::any_completion_handler<void(error_code)> done;
    bool finished{false};
};

inline void discard_bounded_next(const std::shared_ptr<DiscardBoundedState> &state);

inline void async_discard_bounded(std::shared_ptr<ClientStream> stream, std::size_t max_bytes,
                                  std::chrono::milliseconds idle_limit,
                                  asio::any_completion_handler<void(error_code)> done) {
    auto state = std::make_shared<DiscardBoundedState>();
    state->stream = std::move(stream);
    state->max_bytes = max_bytes;
    state->idle_limit = idle_limit;
    state->done = std::move(done);
    discard_bounded_next(state);
}

inline void discard_bounded_next(const std::shared_ptr<DiscardBoundedState> &state) {
    async_read_bounded(state->stream, state->idle_limit, client_errc::body_idle_timeout,
                       [state](error_code ec, ReadResult chunk) {
                           if (state->finished) {
                               return;
                           }
                           auto finish = [state](error_code result) {
                               state->finished = true;
                               auto completion = std::move(state->done);
                               std::move(completion)(result);
                           };
                           if (ec) {
                               finish(ec);
                               return;
                           }
                           if (chunk.eof) {
                               finish(error_code{});
                               return;
                           }
                           state->seen += chunk.data.size();
                           if (state->max_bytes != 0 && state->seen > state->max_bytes) {
                               // Too much to drain: close/reset this exchange and
                               // follow the redirect on a fresh connection.
                               state->stream->async_cancel(asio::detached);
                               finish(error_code{});
                               return;
                           }
                           discard_bounded_next(state);
                       });
}

// RFC 3986 §5.2.4. Removes "." and ".." segments from a path, preserving a
// leading slash and a trailing slash. Redirecting through "../" is legitimate;
// refusing traversal belongs to the static-file key, not to URL resolution.
inline bool has_prefix(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

inline std::string remove_dot_segments(std::string_view input) {
    std::string output;
    while (!input.empty()) {
        if (has_prefix(input, "../")) {
            input.remove_prefix(3);
        } else if (has_prefix(input, "./")) {
            input.remove_prefix(2);
        } else if (has_prefix(input, "/./")) {
            input.remove_prefix(2); // keep the leading '/'
        } else if (input == "/.") {
            input = "/";
        } else if (has_prefix(input, "/../")) {
            input.remove_prefix(3);
            const auto slash = output.rfind('/');
            output.erase(slash == std::string::npos ? 0 : slash);
        } else if (input == "/..") {
            input = "/";
            const auto slash = output.rfind('/');
            output.erase(slash == std::string::npos ? 0 : slash);
        } else if (input == "." || input == "..") {
            input = {};
        } else {
            const std::size_t start = input.front() == '/' ? 1 : 0;
            const auto slash = input.find('/', start);
            const std::size_t take = slash == std::string_view::npos ? input.size() : slash;
            output.append(input.substr(0, take));
            input.remove_prefix(take);
        }
    }
    return output;
}

// Resolves a redirect Location against the URL of the request that produced it
// (RFC 3986 §5.2). The result is an absolute URL; parse_url() validates it.
// Absolute, scheme-relative, root-relative, query-only and path-relative
// references are all covered.
inline std::string resolve_redirect(const Url &base, std::string_view location) {
    // Absolute URI: a valid scheme followed by ':'. A "://" substring alone is
    // not enough — a relative path may contain one.
    const auto colon = location.find(':');
    if (colon != std::string_view::npos && colon > 0) {
        bool scheme = (location[0] >= 'a' && location[0] <= 'z') || (location[0] >= 'A' && location[0] <= 'Z');
        for (std::size_t i = 1; scheme && i < colon; ++i) {
            const char c = location[i];
            scheme = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' ||
                     c == '-' || c == '.';
        }
        if (scheme)
            return std::string{location};
    }

    const std::string origin = base.scheme + "://" + base.authority();
    if (has_prefix(location, "//"))
        return base.scheme + ":" + std::string{location};

    std::string target;
    if (!location.empty() && location.front() == '/') {
        target = remove_dot_segments(location);
    } else if (!location.empty() && location.front() == '?') {
        target = std::string{base.path()} + std::string{location};
    } else if (!location.empty() && location.front() == '#') {
        target = std::string{base.target}; // fragment never goes on the wire
    } else {
        // Merge with the base path's directory, then remove dot segments.
        const std::string_view base_path = base.path();
        const auto slash = base_path.rfind('/');
        std::string merged =
            slash == std::string_view::npos ? std::string{"/"} : std::string{base_path.substr(0, slash + 1)};
        merged.append(location);
        target = remove_dot_segments(merged);
    }
    if (auto hash = target.find('#'); hash != std::string::npos)
        target.resize(hash);
    if (target.empty())
        target = "/";
    return origin + target;
}

} // namespace detail

inline void Client::dial(const detail::Url &url, HttpVersionPolicy version, DialDone done) const {
    if (version == HttpVersionPolicy::Http3) {
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
        dial_h3(url, std::move(done));
        return;
#else
        std::move(done)(make_error_code(client_errc::version_not_negotiated), nullptr);
        return;
#endif
    }
    auto selection = detail::select_proxy(m_config, url.to_target());
    if (!selection) {
        std::move(done)(selection.error(), nullptr);
        return;
    }
    if (*selection) {
        dial_via_proxy(url, **selection, version, std::move(done));
        return;
    }

    auto self = keep_alive();
    detail::async_resolve_bounded(
        m_config.resolve, url.host, url.effective_port(), m_executor, m_config.connect_timeout,
        [self, url, version, done = std::move(done)](error_code ec,
                                                     std::vector<asio::ip::tcp::endpoint> endpoints) mutable {
            if (ec) {
                std::move(done)(ec, nullptr);
                return;
            }
            if (endpoints.empty()) {
                std::move(done)(make_error_code(asio::error::host_not_found), nullptr);
                return;
            }
            auto socket = std::make_shared<asio::ip::tcp::socket>(self->m_executor);
            error_code option_ec;
            if (self->m_config.tcp_nodelay) {
                socket->set_option(asio::ip::tcp::no_delay(true), option_ec);
            }
            if (self->m_config.tcp_keepalive) {
                socket->set_option(asio::socket_base::keep_alive(true), option_ec);
            }
            if (self->m_config.socket_setup) {
                try {
                    self->m_config.socket_setup(*socket);
                } catch (const std::exception &) {
                }
            }
            detail::async_connect_bounded(
                socket, std::make_shared<std::vector<asio::ip::tcp::endpoint>>(std::move(endpoints)),
                self->m_config.connect_timeout, self->m_executor,
                [self, url, socket, version, done = std::move(done)](error_code cec,
                                                                     asio::ip::tcp::endpoint peer) mutable {
                    if (cec) {
                        std::move(done)(cec, nullptr);
                        return;
                    }
                    if (!url.use_tls()) {
                        self->start_session(std::make_shared<TcpStreamTransport>(socket, peer), url, version,
                                            std::move(done));
                        return;
                    }
                    auto context = self->m_tls_context;
                    if (!context) {
                        try {
                            context = detail::make_client_ssl_context(self->m_config.tls);
                        } catch (const std::exception &) {
                            std::move(done)(make_error_code(client_errc::invalid_spec), nullptr);
                            return;
                        }
                    }
                    auto stream =
                        std::make_shared<asio::ssl::stream<asio::ip::tcp::socket>>(std::move(*socket), *context);
                    self->tls_handshake_and_start(std::make_shared<TlsStreamTransport>(stream, peer), url, version,
                                                  std::move(done));
                });
        });
}

#ifdef SIMPLE_HTTP_ENABLE_HTTP3
inline void Client::dial_h3(const detail::Url &url, DialDone done) const {
    if (!url.use_tls()) {
        // QUIC carries TLS 1.3 itself; there is no plain http:// form.
        std::move(done)(make_error_code(client_errc::version_not_negotiated), nullptr);
        return;
    }
    auto context = m_tls_context;
    if (!context) {
        try {
            context = detail::make_client_ssl_context(m_config.tls);
        } catch (const std::exception &) {
            std::move(done)(make_error_code(client_errc::invalid_spec), nullptr);
            return;
        }
    }
    detail::ClientTlsHandshake hs;
    hs.sni = m_config.tls.sni_override.empty() ? url.host : m_config.tls.sni_override;
    hs.verify_host = m_config.tls.verify_host && m_config.tls.verify_peer;
    // QUIC mandates ALPN; HTTP/3 offers exactly h3 (TlsClientConfig::alpn is a
    // TCP-side override and does not apply here).
    hs.alpn_wire.assign(quic::kHttp3AlpnWire.begin(), quic::kHttp3AlpnWire.end());

    auto target = url.to_target();
    target.version = HttpVersionPolicy::Http3;
    detail::Http3ClientSession<asio::any_io_executor>::async_connect(
        m_executor, context->native_handle(), target, hs, m_config.limits, m_config.quic_max_udp_payload_size,
        m_config.connect_timeout, m_config.idle_timeout, m_config.resolve,
        // Keep the SSL_CTX alive until the connection (and therefore SSL_new,
        // which takes its own reference) has been created. When the Client had
        // no context of its own, `context` above is a local that would otherwise
        // die as soon as dial_h3 returns; ConnectState only stores a raw pointer.
        [context, done = std::move(done)](
            error_code ec, std::shared_ptr<detail::Http3ClientSession<asio::any_io_executor>> session) mutable {
            if (ec) {
                std::move(done)(ec, nullptr);
                return;
            }
            std::move(done)(error_code{}, std::shared_ptr<detail::ClientSession>(std::move(session)));
        });
}
#endif

inline void Client::dial_via_proxy(const detail::Url &url, const ProxyConfig &proxy, HttpVersionPolicy version,
                                   DialDone done) const {
    auto self = keep_alive();
    detail::async_resolve_bounded(
        m_config.resolve, proxy.host, proxy.effective_port(), m_executor, m_config.connect_timeout,
        [self, url, proxy, version, done = std::move(done)](error_code ec,
                                                            std::vector<asio::ip::tcp::endpoint> endpoints) mutable {
            if (ec) {
                std::move(done)(ec, nullptr);
                return;
            }
            if (endpoints.empty()) {
                std::move(done)(make_error_code(asio::error::host_not_found), nullptr);
                return;
            }
            auto socket = std::make_shared<asio::ip::tcp::socket>(self->m_executor);
            detail::async_connect_bounded(
                socket, std::make_shared<std::vector<asio::ip::tcp::endpoint>>(std::move(endpoints)),
                self->m_config.connect_timeout, self->m_executor,
                [self, url, proxy, socket, version, done = std::move(done)](error_code cec,
                                                                            asio::ip::tcp::endpoint peer) mutable {
                    if (cec) {
                        std::move(done)(cec, nullptr);
                        return;
                    }
                    SIMPLE_HTTP_DEBUG_LOG("client: tunnelling {} through proxy {}", url.authority(), proxy.authority());
                    if (proxy.kind != ProxyKind::Https) {
                        // Plaintext proxy leg: CONNECT/SOCKS5 on the socket, then
                        // either the transport itself or the origin TLS inside it.
                        auto after_tunnel = [self, url, socket, peer, version,
                                             done = std::move(done)](error_code tec) mutable {
                            if (tec) {
                                error_code ignored;
                                socket->close(ignored);
                                std::move(done)(tec, nullptr);
                                return;
                            }
                            if (!url.use_tls()) {
                                self->start_session(std::make_shared<TcpStreamTransport>(socket, peer), url, version,
                                                    std::move(done));
                                return;
                            }
                            auto context = self->m_tls_context;
                            if (!context) {
                                try {
                                    context = detail::make_client_ssl_context(self->m_config.tls);
                                } catch (const std::exception &) {
                                    std::move(done)(make_error_code(client_errc::invalid_spec), nullptr);
                                    return;
                                }
                            }
                            auto stream = std::make_shared<asio::ssl::stream<asio::ip::tcp::socket>>(std::move(*socket),
                                                                                                     *context);
                            self->tls_handshake_and_start(std::make_shared<TlsStreamTransport>(stream, peer), url,
                                                          version, std::move(done));
                        };
                        detail::async_proxy_tunnel_bounded(
                            self->m_executor, self->m_config.connect_timeout,
                            [socket] {
                                error_code ce;
                                socket->close(ce);
                            },
                            asio::any_completion_handler<void(error_code)>(std::move(after_tunnel)),
                            [socket, proxy, url](asio::any_completion_handler<void(error_code)> handler) mutable {
                                if (proxy.kind == ProxyKind::Socks5)
                                    detail::async_socks5_connect(*socket, proxy, url.host, url.effective_port(),
                                                                 std::move(handler));
                                else
                                    detail::async_http_connect(*socket, proxy, url.authority(), std::move(handler));
                            });
                        return;
                    }

                    // HTTPS proxy: TLS to the proxy, CONNECT inside it, then the
                    // origin's own stream (plaintext inside the proxy TLS, or
                    // TLS-over-TLS).
                    std::shared_ptr<asio::ssl::context> proxy_context;
                    try {
                        proxy_context = detail::make_proxy_ssl_context(proxy, self->m_config.tls);
                    } catch (const std::exception &e) {
                        SIMPLE_HTTP_ERROR_LOG("client proxy: ssl context for {}: {}", proxy.authority(), e.what());
                        error_code ignored;
                        socket->close(ignored);
                        std::move(done)(make_error_code(client_errc::proxy_error), nullptr);
                        return;
                    }
                    auto inner =
                        std::make_shared<asio::ssl::stream<asio::ip::tcp::socket>>(std::move(*socket), *proxy_context);
                    auto proxy_transport = std::make_shared<TlsStreamTransport>(inner, peer);
                    detail::ClientTlsHandshake hs;
                    hs.sni = proxy.server_name.empty() ? proxy.host : proxy.server_name;
                    hs.verify_host = proxy.verify_peer && proxy.verify_host;
                    hs.alpn_wire = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
                    detail::async_tls_client_handshake_bounded(
                        proxy_transport, hs, self->m_config.connect_timeout,
                        [self, url, proxy, proxy_transport, inner, peer, version,
                         done = std::move(done)](error_code hec) mutable {
                            if (hec) {
                                proxy_transport->close();
                                std::move(done)(hec, nullptr);
                                return;
                            }
                            auto after_tunnel = [self, url, inner, peer, version,
                                                 done = std::move(done)](error_code tec) mutable {
                                if (tec) {
                                    error_code ignored;
                                    inner->lowest_layer().close(ignored);
                                    std::move(done)(tec, nullptr);
                                    return;
                                }
                                if (!url.use_tls()) {
                                    // Plaintext origin inside the proxy's TLS
                                    // connection: the proxy TLS stream is just the
                                    // tunnel's byte stream.
                                    self->start_session(std::make_shared<TlsStreamTransport>(inner, peer), url, version,
                                                        std::move(done));
                                    return;
                                }
                                auto context = self->m_tls_context;
                                if (!context) {
                                    try {
                                        context = detail::make_client_ssl_context(self->m_config.tls);
                                    } catch (const std::exception &) {
                                        std::move(done)(make_error_code(client_errc::invalid_spec), nullptr);
                                        return;
                                    }
                                }
                                auto outer =
                                    std::make_shared<asio::ssl::stream<asio::ssl::stream<asio::ip::tcp::socket>>>(
                                        std::move(*inner), *context);
                                self->tls_handshake_and_start(std::make_shared<NestedTlsTransport>(outer, peer), url,
                                                              version, std::move(done));
                            };
                            detail::async_proxy_tunnel_bounded(
                                self->m_executor, self->m_config.connect_timeout,
                                [inner] {
                                    error_code ce;
                                    inner->lowest_layer().close(ce);
                                },
                                asio::any_completion_handler<void(error_code)>(std::move(after_tunnel)),
                                [inner, proxy, url](asio::any_completion_handler<void(error_code)> handler) mutable {
                                    if (proxy.kind == ProxyKind::Socks5)
                                        detail::async_socks5_connect(*inner, proxy, url.host, url.effective_port(),
                                                                     std::move(handler));
                                    else
                                        detail::async_http_connect(*inner, proxy, url.authority(), std::move(handler));
                                });
                        });
                });
        });
}

inline void Client::send_hop(std::shared_ptr<Request> request, std::string url, std::size_t max_body_bytes,
                             std::size_t redirects, std::size_t retries,
                             std::chrono::milliseconds response_head_timeout,
                             std::chrono::milliseconds body_idle_timeout, HttpVersionPolicy version,
                             std::shared_ptr<detail::HopCancel> cancel,
                             asio::any_completion_handler<void(error_code, Response)> done) const {
    // The overall request_timeout may already have completed the caller and
    // destroyed a stack Client; do not touch `this` past this point.
    if (cancel && cancel->cancelled) {
        std::move(done)(make_error_code(asio::error::operation_aborted), Response{});
        return;
    }
    // Per-request budgets override the Client's defaults (0 = use the default).
    const std::chrono::milliseconds head_budget =
        response_head_timeout.count() > 0 ? response_head_timeout : m_config.response_head_timeout;
    const std::chrono::milliseconds idle_budget =
        body_idle_timeout.count() > 0 ? body_idle_timeout : m_config.body_idle_timeout;

    auto parsed = detail::parse_url(url);
    if (!parsed) {
        std::move(done)(parsed.error(), Response{});
        return;
    }
    const detail::Url &target = *parsed;
    request->set_target(std::string{target.target});
    if (m_config.cookie_jar && !request->header(field::cookie)) {
        const std::string cookies = m_config.cookie_jar->cookie_header(url);
        if (!cookies.empty())
            request->mutable_headers().add(std::string{field::cookie}, cookies);
    }
    const std::string origin = target.scheme + "://" + target.authority();
    auto session = take_cached(origin, version);
    auto self = keep_alive();
    auto start = [self, request, url, target, origin, max_body_bytes, redirects, retries, head_budget, idle_budget,
                  version, cancel, done = std::move(done)](std::shared_ptr<detail::ClientSession> ready) mutable {
        auto open_done = [self, request, url, target, origin, max_body_bytes, redirects, retries, head_budget,
                          idle_budget, version, cancel, done = std::move(done)](
                             error_code oec, std::shared_ptr<detail::ClientStream> stream) mutable {
            if (oec) {
                std::move(done)(oec, Response{});
                return;
            }
            if (cancel) {
                // The overall budget may have expired while this hop was
                // dialing/opening; do not start an exchange the caller is no
                // longer waiting for.
                if (cancel->cancelled) {
                    stream->async_cancel(asio::detached);
                    std::move(done)(make_error_code(asio::error::operation_aborted), Response{});
                    return;
                }
                cancel->stream = stream;
            }
            detail::async_read_final_head_bounded(
                stream, head_budget, client_errc::response_head_timeout,
                [self, request, url, target, origin, max_body_bytes, redirects, retries, head_budget, idle_budget,
                 stream, version, cancel, done = std::move(done)](error_code hec, detail::ResponseHead head) mutable {
                    if (cancel && cancel->cancelled) {
                        std::move(done)(make_error_code(asio::error::operation_aborted), Response{});
                        return;
                    }
                    if (hec) {
                        std::move(done)(hec, Response{});
                        return;
                    }
                    const int status = head.status;
                    const bool is_redirect =
                        (status == 301 || status == 302 || status == 303 || status == 307 || status == 308);
                    const auto location = head.header(field::location);
                    if (is_redirect && location && self->m_config.max_redirects > 0) {
                        if (redirects >= self->m_config.max_redirects) {
                            std::move(done)(make_error_code(client_errc::too_many_redirects), Response{});
                            return;
                        }
                        const std::string location_value{*location};
                        // A redirect response's Set-Cookie applies before the
                        // next hop, exactly like any other response.
                        if (self->m_config.cookie_jar)
                            self->m_config.cookie_jar->store(url, head.headers);
                        // Discard, don't buffer. A redirect body is irrelevant,
                        // and a peer must not be able to make the client hold an
                        // arbitrarily large one; past the drain budget the
                        // exchange is aborted and the redirect is followed on a
                        // fresh connection.
                        detail::async_discard_bounded(
                            stream, 256 * 1024, idle_budget,
                            [stream, self, request, url, target, origin, max_body_bytes, redirects, retries,
                             head_budget, idle_budget, status, location_value, version, cancel,
                             done = std::move(done)](error_code bec) mutable {
                                if (cancel && cancel->cancelled) {
                                    std::move(done)(make_error_code(asio::error::operation_aborted), Response{});
                                    return;
                                }
                                if (bec) {
                                    stream->async_cancel(asio::detached);
                                    if (bec == asio::error::operation_aborted) {
                                        std::move(done)(bec, Response{});
                                        return;
                                    }
                                }
                                // RFC 3986 §5.2 reference resolution.
                                std::string next_url = detail::resolve_redirect(target, location_value);
                                auto next = detail::parse_url(next_url);
                                if (!next) {
                                    std::move(done)(next.error(), Response{});
                                    return;
                                }
                                // A downgrade would put the next request (and any
                                // credentials it carries) on the wire in clear.
                                if (target.scheme == "https" && next->scheme == "http") {
                                    std::move(done)(make_error_code(client_errc::redirect_to_insecure), Response{});
                                    return;
                                }
                                // Scheme and host are case-insensitive (RFC 3986
                                // §3.1/§3.2.2); comparing raw bytes cleared
                                // credentials on a mere spelling difference.
                                const bool same_origin = iequals_ci(next->scheme, target.scheme) &&
                                                         iequals_ci(next->host, target.host) &&
                                                         next->effective_port() == target.effective_port();
                                // Replay from the saved body: the first hop
                                // consumed the live Body, and a 307/308 must
                                // resend the same entity. clone_for_replay keeps
                                // the method token, target and headers.
                                auto replay = request->clone_for_replay(self->m_executor);
                                if (self->m_config.cookie_jar) {
                                    // Re-derive from the jar for the new URL/path;
                                    // the copied header describes the old path.
                                    replay->mutable_headers().erase(field::cookie);
                                }
                                if (!same_origin) {
                                    replay->mutable_headers().erase(field::authorization);
                                    replay->mutable_headers().erase(field::cookie);
                                }
                                if (status == 303 ||
                                    ((status == 301 || status == 302) && replay->method() != Method::Get &&
                                     replay->method() != Method::Head)) {
                                    replay->set_method(Method::Get);
                                    // The entity is dropped on this hop. Mark
                                    // the replay bodyless (unknown length) so
                                    // the HTTP/1.1 writer does not advertise
                                    // "content-length: 0" -- Go sends none.
                                    replay->set_body(std::string{});
                                    replay->set_stream_body(false);
                                    replay->set_content_length(-1);
                                }
                                self->send_hop(std::move(replay), std::move(next_url), max_body_bytes, redirects + 1,
                                               retries, head_budget, idle_budget, version, cancel, std::move(done));
                            });
                        return;
                    }
                    // max_redirects == 0 (or no Location): the 3xx is final.
                    detail::async_read_all_bounded(
                        stream, max_body_bytes, idle_budget,
                        [self, url, request, head, stream, max_body_bytes, retries, head_budget, idle_budget, version,
                         cancel, done = std::move(done)](error_code bec, std::string body) mutable {
                            if (cancel && cancel->cancelled) {
                                std::move(done)(make_error_code(asio::error::operation_aborted), Response{});
                                return;
                            }
                            if (bec) {
                                std::move(done)(bec, Response{});
                                return;
                            }
                            if (self->m_config.cookie_jar)
                                self->m_config.cookie_jar->store(url, head.headers);
                            // Opt-in status retry (default off, matching Go and
                            // reqwest): the body is consumed, so the retry is a
                            // fresh exchange from the saved request source.
                            const auto &policy = self->m_config.retry;
                            const bool retry_status =
                                retries < policy.max_retries && !policy.retry_statuses.empty() &&
                                std::find(policy.retry_statuses.begin(), policy.retry_statuses.end(), head.status) !=
                                    policy.retry_statuses.end() &&
                                (is_idempotent(request->method()) || policy.retry_status_on_non_idempotent) &&
                                !request->stream_body();
                            if (retry_status) {
                                const auto retry_after = head.header(field::retry_after);
                                const auto delay =
                                    retry_after ? retry_after_delay(*retry_after) : retry_delay(policy, retries + 1);
                                SIMPLE_HTTP_WARN_LOG("client: {} returned {}, retry {} of {}", url, head.status,
                                                     retries + 1, policy.max_retries);
                                auto timer = std::make_shared<asio::steady_timer>(self->m_executor);
                                timer->expires_after(delay);
                                timer->async_wait([self, request, url, max_body_bytes, head_budget, idle_budget,
                                                   retries, timer, version, cancel,
                                                   done = std::move(done)](error_code tec) mutable {
                                    if (tec) {
                                        return;
                                    }
                                    if (cancel && cancel->cancelled) {
                                        std::move(done)(make_error_code(asio::error::operation_aborted), Response{});
                                        return;
                                    }
                                    self->send_hop(request->clone_for_replay(self->m_executor), url, max_body_bytes,
                                                   /*redirects=*/0, retries + 1, head_budget, idle_budget, version,
                                                   cancel, std::move(done));
                                });
                                return;
                            }
                            // An empty body has nothing to decode; feeding it to
                            // gzip/brotli reports a truncated stream and turns a
                            // legitimate empty response into body_decode_failed.
                            if (!body.empty() && self->m_config.auto_decompress &&
                                request->get_state<detail::AutoDecompressMark>() && request->method() != Method::Head &&
                                !request->header(field::range) && !head.bodyless && head.status != status::no_content &&
                                head.status != status::not_modified) {
                                if (const auto encoding = head.header(field::content_encoding)) {
                                    const ContentCoding coding = parse_content_coding(*encoding);
                                    const bool wanted = (coding == ContentCoding::Gzip &&
                                                         std::find(self->m_config.accept_encodings.begin(),
                                                                   self->m_config.accept_encodings.end(),
                                                                   "gzip") != self->m_config.accept_encodings.end()) ||
                                                        (coding == ContentCoding::Brotli &&
                                                         std::find(self->m_config.accept_encodings.begin(),
                                                                   self->m_config.accept_encodings.end(),
                                                                   "br") != self->m_config.accept_encodings.end());
                                    if (wanted) {
                                        if (auto decoder = make_decoder(*encoding)) {
                                            // The wire body is already capped, but
                                            // decompression is where a bomb does
                                            // its damage. Use the same budget the
                                            // streaming decorator uses and fail
                                            // before allocating the expansion.
                                            const std::size_t budget = max_body_bytes != 0
                                                                           ? max_body_bytes
                                                                           : self->m_config.limits.max_body_bytes;
                                            const std::size_t decoded_cap =
                                                budget == 0 ? std::numeric_limits<std::size_t>::max() : budget;
                                            bool limited = false;
                                            std::string decoded = decoder->write_bounded(body, decoded_cap, limited);
                                            if (!limited) {
                                                const std::size_t room =
                                                    decoded_cap > decoded.size() ? decoded_cap - decoded.size() : 0;
                                                decoded += decoder->finish_bounded(room, limited);
                                            }
                                            if (limited) {
                                                std::move(done)(make_error_code(client_errc::body_too_large),
                                                                Response{});
                                                return;
                                            }
                                            if (decoder->failed()) {
                                                std::move(done)(make_error_code(client_errc::body_decode_failed),
                                                                Response{});
                                                return;
                                            }
                                            body = std::move(decoded);
                                            head.headers.erase(field::content_encoding);
                                            head.headers.erase(field::content_length);
                                            head.uncompressed = true;
                                        }
                                    }
                                }
                            }
                            Response response{head, std::move(body)};
                            std::move(done)(error_code{}, std::move(response));
                        });
                });
        };
        if (ready) {
            ready->open_stream(request, std::move(open_done));
            return;
        }
        self->dial(target, version,
                   [self, request, origin, cancel, open_done = std::move(open_done)](
                       error_code dec, std::shared_ptr<detail::ClientSession> session) mutable {
                       if (cancel && cancel->cancelled) {
                           open_done(make_error_code(asio::error::operation_aborted), nullptr);
                           return;
                       }
                       if (dec) {
                           open_done(dec, nullptr);
                           return;
                       }
                       self->cache(origin, session);
                       session->open_stream(request, std::move(open_done));
                   });
    };
    if (session) {
        start(std::move(session));
    } else {
        start(nullptr);
    }
}

inline void
Client::open_stream_hop(std::string url, StreamSpec spec, std::size_t max_body_bytes,
                        asio::any_completion_handler<void(error_code, std::shared_ptr<Stream>)> done) const {
    auto parsed = detail::parse_url(url);
    if (!parsed) {
        std::move(done)(parsed.error(), nullptr);
        return;
    }
    const detail::Url target = *parsed;
    detail::ResponseDecoding decoding;
    if (m_config.auto_decompress && spec.method != Method::Head && !spec.headers.get(field::accept_encoding) &&
        !spec.headers.get(field::range) && !m_config.accept_encodings.empty()) {
        const auto encodings = detail::normalize_accept_encodings(m_config.accept_encodings);
        if (!encodings.empty()) {
            spec.headers.add(std::string{field::accept_encoding}, detail::join_encodings(encodings));
            decoding.enabled = true;
            decoding.encodings = encodings;
        }
    }
    auto request = std::make_shared<Request>(Version::Http11, m_executor);
    request->set_method(spec.method);
    request->set_target(spec.target.empty() ? std::string{target.target} : spec.target);
    request->mutable_headers() = std::move(spec.headers);
    if (spec.expect_100_continue || spec.auto_expect_continue) {
        request->mutable_headers().add(std::string{field::expect}, "100-continue");
    }
    request->set_close(spec.close);
    request->set_stream_body(true);
    const std::chrono::milliseconds head_timeout =
        spec.response_head_timeout.count() > 0 ? spec.response_head_timeout : m_config.response_head_timeout;
    const std::chrono::milliseconds idle_timeout =
        spec.body_idle_timeout.count() > 0 ? spec.body_idle_timeout : m_config.body_idle_timeout;
    const bool auto_continue = spec.auto_expect_continue;
    const std::chrono::milliseconds continue_timeout = spec.continue_timeout;
    const HttpVersionPolicy version = spec.version ? *spec.version : m_config.default_version;
    auto self = keep_alive();
    dial(target, version,
         [self, request, max_body_bytes, decoding, head_timeout, idle_timeout, auto_continue, continue_timeout,
          done = std::move(done)](error_code ec, std::shared_ptr<detail::ClientSession> session) mutable {
             if (ec) {
                 std::move(done)(ec, nullptr);
                 return;
             }
             session->open_stream(request, [max_body_bytes, decoding, head_timeout, idle_timeout, auto_continue,
                                            continue_timeout, done = std::move(done)](
                                               error_code oec, std::shared_ptr<detail::ClientStream> stream) mutable {
                 if (oec) {
                     std::move(done)(oec, nullptr);
                     return;
                 }
                 auto wrapped = detail::maybe_decompressing_stream(std::move(stream), decoding, max_body_bytes);
                 std::move(done)(error_code{}, std::make_shared<Stream>(std::move(wrapped), head_timeout, idle_timeout,
                                                                        auto_continue, continue_timeout));
             });
         });
}

template <typename Transport>
inline void
Client::begin_ws_upgrade(std::shared_ptr<Transport> transport, std::string authority, std::string path,
                         WebSocketSpec spec,
                         asio::any_completion_handler<void(error_code, std::shared_ptr<WebSocket>)> done) const {
    detail::async_ws_upgrade(std::move(transport), std::move(authority), std::move(path), std::move(spec),
                             m_config.idle_timeout, m_config.response_head_timeout, std::move(done));
}

template <typename Transport>
inline void Client::ws_tls_handshake_and_start(
    std::shared_ptr<Transport> transport, std::string authority, std::string path, WebSocketSpec spec, std::string sni,
    asio::any_completion_handler<void(error_code, std::shared_ptr<WebSocket>)> done) const {
    detail::ClientTlsHandshake hs;
    hs.sni = std::move(sni);
    hs.verify_host = m_config.tls.verify_host;
    // A WebSocket must not negotiate h2, or the Upgrade would be impossible.
    hs.alpn_wire = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    auto self = keep_alive();
    detail::async_tls_client_handshake_bounded(
        transport, hs, m_config.connect_timeout,
        [self, transport, authority = std::move(authority), path = std::move(path), spec = std::move(spec),
         done = std::move(done)](error_code hec) mutable {
            if (hec) {
                transport->close();
                std::move(done)(hec, nullptr);
                return;
            }
            self->begin_ws_upgrade(std::move(transport), std::move(authority), std::move(path), std::move(spec),
                                   std::move(done));
        });
}

inline void
Client::open_ws_via_proxy(const detail::Url &url, const ProxyConfig &proxy, std::string path, std::string authority,
                          WebSocketSpec spec,
                          asio::any_completion_handler<void(error_code, std::shared_ptr<WebSocket>)> done) const {
    auto resolver = std::make_shared<asio::ip::tcp::resolver>(m_executor);
    auto self = keep_alive();
    resolver->async_resolve(
        proxy.host, std::to_string(proxy.effective_port()),
        [self, url, proxy, path, authority, spec, resolver, done = std::move(done)](error_code ec,
                                                                                    auto results) mutable {
            if (ec) {
                std::move(done)(ec, nullptr);
                return;
            }
            auto socket = std::make_shared<asio::ip::tcp::socket>(self->m_executor);
            asio::async_connect(
                *socket, results,
                [self, url, proxy, path, authority, spec, socket,
                 done = std::move(done)](error_code cec, const asio::ip::tcp::endpoint &peer) mutable {
                    if (cec) {
                        std::move(done)(cec, nullptr);
                        return;
                    }
                    if (proxy.kind != ProxyKind::Https) {
                        auto after_tunnel = [self, url, socket, peer, path, authority, spec,
                                             done = std::move(done)](error_code tec) mutable {
                            if (tec) {
                                error_code ignored;
                                socket->close(ignored);
                                std::move(done)(tec, nullptr);
                                return;
                            }
                            if (!url.use_tls()) {
                                self->begin_ws_upgrade(std::make_shared<TcpStreamTransport>(socket, peer),
                                                       std::move(authority), std::move(path), std::move(spec),
                                                       std::move(done));
                                return;
                            }
                            auto context = self->m_tls_context;
                            if (!context) {
                                try {
                                    context = detail::make_client_ssl_context(self->m_config.tls);
                                } catch (const std::exception &) {
                                    std::move(done)(make_error_code(client_errc::invalid_spec), nullptr);
                                    return;
                                }
                            }
                            auto stream = std::make_shared<asio::ssl::stream<asio::ip::tcp::socket>>(std::move(*socket),
                                                                                                     *context);
                            self->ws_tls_handshake_and_start(std::make_shared<TlsStreamTransport>(stream, peer),
                                                             std::move(authority), std::move(path), std::move(spec),
                                                             url.host, std::move(done));
                        };
                        detail::async_proxy_tunnel_bounded(
                            self->m_executor, self->m_config.connect_timeout,
                            [socket] {
                                error_code ce;
                                socket->close(ce);
                            },
                            asio::any_completion_handler<void(error_code)>(std::move(after_tunnel)),
                            [socket, proxy, url](asio::any_completion_handler<void(error_code)> handler) mutable {
                                if (proxy.kind == ProxyKind::Socks5)
                                    detail::async_socks5_connect(*socket, proxy, url.host, url.effective_port(),
                                                                 std::move(handler));
                                else
                                    detail::async_http_connect(*socket, proxy, url.authority(), std::move(handler));
                            });
                        return;
                    }

                    std::shared_ptr<asio::ssl::context> proxy_context;
                    try {
                        proxy_context = detail::make_proxy_ssl_context(proxy, self->m_config.tls);
                    } catch (const std::exception &e) {
                        SIMPLE_HTTP_ERROR_LOG("client proxy: ssl context for {}: {}", proxy.authority(), e.what());
                        error_code ignored;
                        socket->close(ignored);
                        std::move(done)(make_error_code(client_errc::proxy_error), nullptr);
                        return;
                    }
                    auto inner =
                        std::make_shared<asio::ssl::stream<asio::ip::tcp::socket>>(std::move(*socket), *proxy_context);
                    auto proxy_transport = std::make_shared<TlsStreamTransport>(inner, peer);
                    detail::ClientTlsHandshake hs;
                    hs.sni = proxy.server_name.empty() ? proxy.host : proxy.server_name;
                    hs.verify_host = proxy.verify_peer && proxy.verify_host;
                    hs.alpn_wire = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
                    detail::async_tls_client_handshake_bounded(
                        proxy_transport, hs, self->m_config.connect_timeout,
                        [self, url, proxy, proxy_transport, inner, peer, path, authority, spec,
                         done = std::move(done)](error_code hec) mutable {
                            if (hec) {
                                proxy_transport->close();
                                std::move(done)(hec, nullptr);
                                return;
                            }
                            auto after_tunnel = [self, url, inner, peer, path, authority, spec,
                                                 done = std::move(done)](error_code tec) mutable {
                                if (tec) {
                                    error_code ignored;
                                    inner->lowest_layer().close(ignored);
                                    std::move(done)(tec, nullptr);
                                    return;
                                }
                                if (!url.use_tls()) {
                                    self->begin_ws_upgrade(std::make_shared<TlsStreamTransport>(inner, peer),
                                                           std::move(authority), std::move(path), std::move(spec),
                                                           std::move(done));
                                    return;
                                }
                                auto context = self->m_tls_context;
                                if (!context) {
                                    try {
                                        context = detail::make_client_ssl_context(self->m_config.tls);
                                    } catch (const std::exception &) {
                                        std::move(done)(make_error_code(client_errc::invalid_spec), nullptr);
                                        return;
                                    }
                                }
                                auto outer =
                                    std::make_shared<asio::ssl::stream<asio::ssl::stream<asio::ip::tcp::socket>>>(
                                        std::move(*inner), *context);
                                self->ws_tls_handshake_and_start(std::make_shared<NestedTlsTransport>(outer, peer),
                                                                 std::move(authority), std::move(path), std::move(spec),
                                                                 url.host, std::move(done));
                            };
                            detail::async_proxy_tunnel_bounded(
                                self->m_executor, self->m_config.connect_timeout,
                                [inner] {
                                    error_code ce;
                                    inner->lowest_layer().close(ce);
                                },
                                asio::any_completion_handler<void(error_code)>(std::move(after_tunnel)),
                                [inner, proxy, url](asio::any_completion_handler<void(error_code)> handler) mutable {
                                    if (proxy.kind == ProxyKind::Socks5)
                                        detail::async_socks5_connect(*inner, proxy, url.host, url.effective_port(),
                                                                     std::move(handler));
                                    else
                                        detail::async_http_connect(*inner, proxy, url.authority(), std::move(handler));
                                });
                        });
                });
        });
}

inline void Client::open_ws_hop(std::string url, WebSocketSpec spec,
                                asio::any_completion_handler<void(error_code, std::shared_ptr<WebSocket>)> done) const {
    std::string http_url = url;
    if (starts_with(url, "wss://")) {
        http_url = "https://" + url.substr(6);
    } else if (starts_with(url, "ws://")) {
        http_url = "http://" + url.substr(5);
    } else {
        std::move(done)(make_error_code(client_errc::unsupported_scheme), nullptr);
        return;
    }
    auto parsed = detail::parse_url(http_url);
    if (!parsed) {
        std::move(done)(parsed.error(), nullptr);
        return;
    }
    const detail::Url target = *parsed;
    const std::string path = std::string{target.target};
    const std::string authority = target.authority();

    auto selection = detail::select_proxy(m_config, target.to_target());
    if (!selection) {
        std::move(done)(selection.error(), nullptr);
        return;
    }
    if (*selection) {
        open_ws_via_proxy(target, **selection, path, authority, std::move(spec), std::move(done));
        return;
    }

    auto resolver = std::make_shared<asio::ip::tcp::resolver>(m_executor);
    auto self = keep_alive();
    resolver->async_resolve(
        target.host, std::to_string(target.effective_port()),
        [self, target, resolver, path, authority, spec, done = std::move(done)](error_code ec, auto results) mutable {
            if (ec) {
                std::move(done)(ec, nullptr);
                return;
            }
            auto socket = std::make_shared<asio::ip::tcp::socket>(self->m_executor);
            asio::async_connect(
                *socket, results,
                [self, target, path, authority, spec, socket,
                 done = std::move(done)](error_code cec, const asio::ip::tcp::endpoint &peer) mutable {
                    if (cec) {
                        std::move(done)(cec, nullptr);
                        return;
                    }
                    if (!target.use_tls()) {
                        self->begin_ws_upgrade(std::make_shared<TcpStreamTransport>(socket, peer), std::move(authority),
                                               std::move(path), std::move(spec), std::move(done));
                        return;
                    }
                    auto context = self->m_tls_context;
                    if (!context) {
                        try {
                            context = detail::make_client_ssl_context(self->m_config.tls);
                        } catch (const std::exception &) {
                            std::move(done)(make_error_code(client_errc::invalid_spec), nullptr);
                            return;
                        }
                    }
                    auto stream =
                        std::make_shared<asio::ssl::stream<asio::ip::tcp::socket>>(std::move(*socket), *context);
                    error_code ep_ec;
                    const auto endpoint = stream->lowest_layer().remote_endpoint(ep_ec);
                    auto transport = std::make_shared<TlsStreamTransport>(stream, endpoint);
                    self->ws_tls_handshake_and_start(std::move(transport), std::move(authority), std::move(path),
                                                     std::move(spec), target.host, std::move(done));
                });
        });
}

} // namespace simple_http
