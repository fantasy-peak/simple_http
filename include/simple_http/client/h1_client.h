#pragma once

// HTTP/1.1 client session (C++17 token-only port).
//
// One exchange at a time per connection: the next open_stream() on a busy
// session fails with client_errc::session_busy. Requests are framed with
// Content-Length when the length is known up front and chunked when the body is
// streamed; responses are decoded from Content-Length, chunked framing or
// close-delimited bodies, with trailer sections published on the stream.
//
// The session is deliberately narrower than the C++23 original for this first
// port: h2c upgrade and the automatic Expect: 100-continue dance are deferred,
// and an interim 100 is read through to the final response.

#include <algorithm>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "../core/http_field.h"
#include "../core/http_method.h"
#include "../core/logging.h"
#include "../core/types.h"
#include "../proto/body.h"
#include "../proto/headers.h"
#include "../proto/request.h"
#include "../transport/transport.h"
#include "client_config.h"
#include "client_stream.h"

namespace simple_http::detail {

namespace asio = boost::asio;

template <typename Transport> class Http1ClientSession;

template <typename Transport> class Http1ClientStream final : public ClientStream {
  public:
    explicit Http1ClientStream(std::shared_ptr<Http1ClientSession<Transport>> session)
        : m_session(std::move(session)), m_executor(m_session->executor()) {}

    void write(std::string data, WriteCompletion done) override;
    void finish(std::string data, WriteCompletion done) override;
    void finish_trailers(Headers trailers, WriteCompletion done) override;
    void read(ReadCompletion done) override;
    void cancel(WriteCompletion done) override;

    friend class Http1ClientSession<Transport>;

    bool finished() const override { return m_finished.load(std::memory_order_acquire); }
    Version version() const override { return Version::Http11; }
    std::uint32_t id() const override { return 0; }

    void mark_finished() { m_finished.store(true, std::memory_order_release); }

  protected:
    void await_head(WriteCompletion done) override;
    asio::any_io_executor head_executor() const override { return m_executor; }

  private:
    // The stream keeps the session alive (a caller may hold the stream after
    // dropping the Client); the session only weakly references its current
    // stream, which is what breaks the reference cycle. See release_exchange().
    std::shared_ptr<Http1ClientSession<Transport>> m_session;
    asio::any_io_executor m_executor;
    std::atomic<bool> m_finished{false};
};

template <typename Transport>
class Http1ClientSession final : public ClientSession,
                                 public std::enable_shared_from_this<Http1ClientSession<Transport>> {
  public:
    using Executor = asio::any_io_executor;

    Http1ClientSession(std::shared_ptr<Transport> transport, std::string authority, EngineLimits limits)
        : m_transport(std::move(transport)), m_executor(m_transport->get_executor()), m_authority(std::move(authority)),
          m_limits(limits) {}

    void open_stream(std::shared_ptr<Request> request, OpenCompletion done) override {
        auto self = this->shared_from_this();
        hop([self, request = std::move(request), done = std::move(done)]() mutable {
            self->open_exchange(std::move(request), std::move(done));
        });
    }

    bool alive() const override { return m_alive.load(std::memory_order_acquire); }
    bool reusable() const override {
        return alive() && !m_busy && !m_close_after && m_body_complete && !m_connection_error;
    }
    bool keepable() const override { return alive() && !m_close_after; }
    bool multiplexes() const override { return false; }
    Version version() const override { return Version::Http11; }
    std::string_view authority() const override { return m_authority; }
    asio::any_io_executor executor() const override { return m_executor; }

    void close() override {
        m_alive.store(false, std::memory_order_release);
        m_transport->close();
    }

    void set_on_exchange_done(std::function<void()> cb) override { m_exchange_done = std::move(cb); }
    std::shared_ptr<std::atomic<bool>> exchange_done_flag() const override { return m_current_done; }

    // --- token operations used by the stream ---
    //
    // `caller` identifies the exchange that issued the operation. A stream may
    // outlive its exchange (the session keeps only a weak reference after
    // release_exchange, and Response holds the stream), so without this check a
    // stale stream would write into — or read from — whatever exchange the
    // connection is running now.
    bool is_current(const Http1ClientStream<Transport> *caller) const {
        return m_current.lock().get() == caller;
    }

    void exchange_write(std::string data, bool last, WriteCompletion done,
                        const Http1ClientStream<Transport> *caller) {
        auto self = this->shared_from_this();
        hop([self, data = std::move(data), last, done = std::move(done), caller]() mutable {
            if (!self->is_current(caller)) {
                std::move(done)(make_error_code(client_errc::session_closed));
                return;
            }
            self->do_exchange_write(std::move(data), last, std::move(done));
        });
    }
    void exchange_write_trailers(Headers trailers, WriteCompletion done,
                                 const Http1ClientStream<Transport> *caller) {
        auto self = this->shared_from_this();
        hop([self, trailers = std::move(trailers), done = std::move(done), caller]() mutable {
            if (!self->is_current(caller)) {
                std::move(done)(make_error_code(client_errc::session_closed));
                return;
            }
            self->do_exchange_write_trailers(std::move(trailers), std::move(done));
        });
    }
    void exchange_read(ReadCompletion done, const Http1ClientStream<Transport> *caller) {
        auto self = this->shared_from_this();
        hop([self, done = std::move(done), caller]() mutable {
            if (!self->is_current(caller)) {
                std::move(done)(make_error_code(client_errc::session_closed), ReadResult{});
                return;
            }
            self->do_exchange_read(std::move(done));
        });
    }
    void ensure_head(WriteCompletion done, const Http1ClientStream<Transport> *caller) {
        auto self = this->shared_from_this();
        hop([self, done = std::move(done), caller]() mutable {
            if (!self->is_current(caller)) {
                std::move(done)(make_error_code(client_errc::session_closed));
                return;
            }
            self->do_ensure_head(std::move(done));
        });
    }
    void cancel_exchange(WriteCompletion done, const Http1ClientStream<Transport> *caller) {
        auto self = this->shared_from_this();
        hop([self, done = std::move(done), caller]() mutable {
            if (!self->is_current(caller)) {
                // A stale handle must not tear down the exchange that owns the
                // connection now.
                std::move(done)(make_error_code(client_errc::session_closed));
                return;
            }
            self->m_alive.store(false);
            self->m_transport->close();
            self->release_exchange();
            std::move(done)(error_code{});
        });
    }

    Transport &transport() { return *m_transport; }

  private:
    friend class Http1ClientStream<Transport>;

    enum class BodyMode { None, Fixed, Chunked, EofDelimited };

    template <typename Fn> void hop(Fn &&fn) {
        asio::dispatch(asio::bind_executor(asio::any_io_executor{m_executor}, std::forward<Fn>(fn)));
    }

    static bool method_expects_body(Method method) {
        return method != Method::Get && method != Method::Head && method != Method::Options &&
               method != Method::Trace && method != Method::Delete && method != Method::Connect;
    }

    void release_exchange() {
        m_busy = false;
        if (m_current_done)
            m_current_done->store(true, std::memory_order_release);
        if (auto current = m_current.lock()) {
            current->mark_finished();
        }
        m_current.reset();
        auto hook = m_exchange_done;
        if (hook)
            hook();
    }

    void fail_exchange(error_code ec) {
        m_connection_error = ec;
        m_alive.store(false, std::memory_order_release);
        m_transport->close();
        m_body_complete = false;
        release_exchange();
    }

    // --- request ---

    void open_exchange(std::shared_ptr<Request> request, OpenCompletion done) {
        if (!alive()) {
            std::move(done)(make_error_code(client_errc::session_closed), nullptr);
            return;
        }
        if (m_busy) {
            std::move(done)(make_error_code(client_errc::session_busy), nullptr);
            return;
        }
        std::string head;
        if (auto ec = build_request_head(*request, head); ec) {
            std::move(done)(ec, nullptr);
            return;
        }
        m_busy = true;
        m_spec = std::move(request);
        m_close_after = m_spec->close();
        // Did this request ask for an interim 100-continue? If so the first 100
        // is handed back to the caller as the head (instead of being swallowed)
        // so Stream can send the body only after the peer agreed. A final
        // response may also arrive instead -- a refusal -- and is surfaced too.
        m_expect_continue = false;
        if (auto expect = m_spec->headers().get(field::expect)) {
            m_expect_continue = icontains_ci(*expect, "100-continue");
        }
        m_awaiting_final = false;
        m_head_parsed = false;
        m_body_complete = false;
        m_connection_error = {};
        m_in.clear();
        m_trailers = Headers{};
        m_current_done = std::make_shared<std::atomic<bool>>(false);
        auto stream = std::make_shared<Http1ClientStream<Transport>>(this->shared_from_this());
        m_current = stream;

        const bool has_upfront_body = m_spec->stream_body() || m_spec->content_length() >= 0;
        if (!m_spec->stream_body() && !has_upfront_body) {
            auto bytes = std::make_shared<std::string>(std::move(head));
            auto self = this->shared_from_this();
            m_transport->async_write(ConstByteSpan{reinterpret_cast<const std::byte *>(bytes->data()), bytes->size()},
                                     [self, stream, bytes, done = std::move(done)](error_code ec, std::size_t) mutable {
                                         if (ec) {
                                             self->fail_exchange(ec);
                                             std::move(done)(ec, nullptr);
                                             return;
                                         }
                                         std::move(done)(error_code{}, stream);
                                     });
            return;
        }
        if (!m_spec->stream_body()) {
            auto self = this->shared_from_this();
            m_spec->body().async_read_all([self, stream, head = std::move(head),
                                           done = std::move(done)](error_code ec, std::string body) mutable {
                if (ec) {
                    self->release_exchange();
                    std::move(done)(ec, nullptr);
                    return;
                }
                std::string out = head;
                if (!body.empty()) {
                    if (self->m_request_chunked) {
                        out += encode_chunk(body);
                        out += "0\r\n\r\n";
                    } else {
                        out += body;
                    }
                }
                auto bytes = std::make_shared<std::string>(std::move(out));
                self->m_transport->async_write(
                    ConstByteSpan{reinterpret_cast<const std::byte *>(bytes->data()), bytes->size()},
                    [self, stream, bytes, done = std::move(done)](error_code wec, std::size_t) mutable {
                        if (wec) {
                            self->fail_exchange(wec);
                            std::move(done)(wec, nullptr);
                            return;
                        }
                        std::move(done)(error_code{}, stream);
                    });
            });
            return;
        }

        auto bytes = std::make_shared<std::string>(std::move(head));
        auto self = this->shared_from_this();
        m_transport->async_write(ConstByteSpan{reinterpret_cast<const std::byte *>(bytes->data()), bytes->size()},
                                 [self, stream, bytes, done = std::move(done)](error_code ec, std::size_t) mutable {
                                     if (ec) {
                                         self->fail_exchange(ec);
                                         std::move(done)(ec, nullptr);
                                         return;
                                     }
                                     std::move(done)(error_code{}, stream);
                                 });
    }

    error_code build_request_head(const Request &request, std::string &out) {
        if (contains_ctl(request.method_token()) || contains_ctl(request.target())) {
            return make_error_code(client_errc::invalid_spec);
        }
        out.reserve(256);
        out.append(request.method_token());
        out.push_back(' ');
        out.append(request.target());
        out.append(" HTTP/1.1\r\n");

        bool has_host = false;
        std::string host_value;
        bool has_connection = false;
        for (const auto &[name, value] : request.headers()) {
            if (name == "host") {
                has_host = true;
                host_value = value;
            }
            if (name == "connection")
                has_connection = true;
            if (name == "content-length" || name == "transfer-encoding" || is_connection_specific_field(name) ||
                name == "host") {
                continue; // framing and connection fields belong to the session
            }
            out.append(name);
            out.append(": ");
            out.append(value);
            out.append("\r\n");
        }
        out.append("host: ");
        out.append(has_host ? host_value : m_authority);
        out.append("\r\n");

        m_request_chunked = false;
        if (request.stream_body()) {
            if (request.content_length() >= 0) {
                out.append("content-length: ");
                out.append(std::to_string(request.content_length()));
                out.append("\r\n");
            } else {
                m_request_chunked = true;
                out.append("transfer-encoding: chunked\r\n");
            }
        } else if (request.content_length() >= 0) {
            out.append("content-length: ");
            out.append(std::to_string(request.content_length()));
            out.append("\r\n");
        } else if (method_expects_body(request.method())) {
            out.append("content-length: 0\r\n");
        }

        if (request.close()) {
            out.append("connection: close\r\n");
        } else if (!has_connection) {
            out.append("connection: keep-alive\r\n");
        }
        out.append("\r\n");
        return {};
    }

    static std::string encode_chunk(const std::string &data) {
        static constexpr char hex[] = "0123456789abcdef";
        char size[2 * sizeof(std::size_t) + 2];
        std::size_t i = sizeof(size);
        std::size_t value = data.size();
        do {
            size[--i] = hex[value & 0xF];
            value >>= 4;
        } while (value != 0);
        std::string out{size + i, sizeof(size) - i};
        out.append("\r\n");
        out.append(data);
        out.append("\r\n");
        return out;
    }

    void do_exchange_write(std::string data, bool last, WriteCompletion done) {
        if (!m_busy) {
            std::move(done)(make_error_code(client_errc::session_closed));
            return;
        }
        if (!m_spec || !m_spec->stream_body()) {
            std::move(done)(make_error_code(client_errc::body_not_streaming));
            return;
        }
        std::string out;
        if (m_request_chunked) {
            if (!data.empty())
                out = encode_chunk(data);
            if (last)
                out += "0\r\n\r\n";
        } else {
            out = std::move(data);
        }
        if (out.empty()) {
            std::move(done)(error_code{});
            return;
        }
        auto bytes = std::make_shared<std::string>(std::move(out));
        auto self = this->shared_from_this();
        m_transport->async_write(ConstByteSpan{reinterpret_cast<const std::byte *>(bytes->data()), bytes->size()},
                                 [self, bytes, done = std::move(done)](error_code ec, std::size_t) mutable {
                                     if (ec) {
                                         self->fail_exchange(ec);
                                     }
                                     std::move(done)(ec);
                                 });
    }

    void do_exchange_write_trailers(Headers trailers, WriteCompletion done) {
        if (!m_busy || !m_spec || !m_spec->stream_body()) {
            std::move(done)(make_error_code(client_errc::body_not_streaming));
            return;
        }
        if (!m_request_chunked) {
            std::move(done)(make_error_code(client_errc::trailers_not_supported));
            return;
        }
        std::string out = "0\r\n";
        for (const auto &[name, value] : trailers) {
            if (is_forbidden_trailer_field(name))
                continue;
            out.append(name);
            out.append(": ");
            out.append(value);
            out.append("\r\n");
        }
        out.append("\r\n");
        auto bytes = std::make_shared<std::string>(std::move(out));
        auto self = this->shared_from_this();
        m_transport->async_write(ConstByteSpan{reinterpret_cast<const std::byte *>(bytes->data()), bytes->size()},
                                 [self, bytes, done = std::move(done)](error_code ec, std::size_t) mutable {
                                     if (ec) {
                                         self->fail_exchange(ec);
                                     }
                                     std::move(done)(ec);
                                 });
    }

    // --- reading ---

    template <typename Handler> void read_more(Handler &&handler) {
        auto self = this->shared_from_this();
        // The wrapping lambda below carries no associators of its own. Forward
        // the caller's cancellation slot explicitly, or a per-operation deadline
        // (Stream::read_head_with / read_with) can never abort the socket read:
        // the slot would stop here and the read would hang until the peer moved.
        auto slot = m_read_slot.is_connected() ? m_read_slot : asio::get_associated_cancellation_slot(handler);
        auto on_read = [self, done = std::forward<Handler>(handler)](error_code ec, std::size_t n) mutable {
            if (!ec && n != 0)
                self->m_in.append(reinterpret_cast<const char *>(self->m_scratch.data()), n);
            done(ec, n);
        };
        if (slot.is_connected()) {
            m_transport->async_read_some(ByteSpan{m_scratch.data(), m_scratch.size()},
                                         asio::bind_cancellation_slot(slot, std::move(on_read)));
        } else {
            m_transport->async_read_some(ByteSpan{m_scratch.data(), m_scratch.size()}, std::move(on_read));
        }
    }

    void do_ensure_head(WriteCompletion done) {
        // Remember the caller's cancellation slot for the whole read chain.
        // The intermediate lambdas that append to m_in and re-enter read_head
        // do not carry associators, so a per-operation deadline must be applied
        // to the socket read from here rather than from the wrapped handler.
        m_read_slot = asio::get_associated_cancellation_slot(done);
        if (m_head_parsed) {
            std::move(done)(error_code{});
            return;
        }
        auto self = this->shared_from_this();
        read_head([self, done = std::move(done)](error_code ec) mutable {
            if (ec) {
                // A per-operation deadline cancels this read only. The
                // automatic 100-continue probe relies on that: on timeout the
                // caller sends the body anyway. Poisoning the exchange here
                // (closing the transport) made the fallback impossible.
                // Protocol/transport errors remain fatal.
                if (ec != asio::error::operation_aborted)
                    self->fail_exchange(ec);
                std::move(done)(ec);
                return;
            }
            std::move(done)(error_code{});
        });
    }

    // Reads status line + fields (and any body bytes already buffered),
    // transparently skipping interim 1xx responses other than a final one.
    template <typename Handler> void read_head(Handler &&handler) {
        auto done = std::forward<Handler>(handler);
        const auto marker = m_in.find("\r\n\r\n");
        if (marker == std::string::npos) {
            if (m_in.size() > m_limits.max_header_bytes) {
                done(make_error_code(client_errc::protocol_error));
                return;
            }
            read_more([this, done = std::move(done)](error_code ec, std::size_t) mutable {
                if (ec) {
                    done(ec);
                    return;
                }
                read_head(std::move(done));
            });
            return;
        }
        const std::string block = m_in.substr(0, marker + 4);
        m_in.erase(0, marker + 4);
        const auto line_end = block.find("\r\n");
        if (line_end == std::string::npos) {
            done(make_error_code(client_errc::protocol_error));
            return;
        }
        const std::string_view status_line{block.data(), line_end};
        // "HTTP/1.1 200 OK"
        const auto sp1 = status_line.find(' ');
        if (sp1 == std::string_view::npos || status_line.substr(0, sp1) != "HTTP/1.1") {
            done(make_error_code(client_errc::protocol_error));
            return;
        }
        const auto sp2 = status_line.find(' ', sp1 + 1);
        int status = 0;
        for (std::size_t i = sp1 + 1; i < (sp2 == std::string_view::npos ? status_line.size() : sp2); ++i) {
            if (status_line[i] < '0' || status_line[i] > '9') {
                done(make_error_code(client_errc::protocol_error));
                return;
            }
            status = status * 10 + (status_line[i] - '0');
        }
        ResponseHead head;
        head.status = status;
        head.version = Version::Http11;
        std::size_t pos = line_end + 2;
        while (pos + 1 < block.size()) {
            const auto end = block.find("\r\n", pos);
            if (end == std::string::npos || end == pos)
                break;
            const std::string_view line{block.data() + pos, end - pos};
            const auto colon = line.find(':');
            if (colon != std::string_view::npos) {
                head.headers.add_lower(ascii_lower_copy(trim_ows(line.substr(0, colon))),
                                       std::string{trim_ows(line.substr(colon + 1))});
            }
            pos = end + 2;
        }

        // Interim responses: a 100 that answers our own Expect: 100-continue is
        // surfaced as the head so the caller can send the body; the final
        // response is then parsed on the next read_head. Every other 1xx is not
        // a final response and is skipped.
        if (status >= 100 && status < 200 && status != 101) {
            if (status == 100 && m_expect_continue && !m_awaiting_final) {
                m_awaiting_final = true;
                // m_head_parsed stays false: this is not the final head, and the
                // caller resets the published head before asking again.
                if (auto current = m_current.lock()) {
                    current->set_head(std::move(head));
                }
                done(error_code{});
                return;
            }
            read_head(std::move(done));
            return;
        }

        m_status = status;
        m_head_parsed = true;
        m_close_after = m_close_after || header_has_token(head.headers, "connection", "close");
        ResponseHead parsed_head = std::move(head);
        if (auto current = m_current.lock()) {
            current->set_head(parsed_head);
        }

        const bool head_request = m_spec && m_spec->method() == Method::Head;
        const bool bodyless = head_request || status == 204 || status == 304;
        m_body_complete = false;
        if (bodyless) {
            m_body_mode = BodyMode::None;
            m_body_complete = true;
            done(error_code{});
            return;
        }
        if (header_has_token(parsed_head.headers, "transfer-encoding", "chunked")) {
            m_body_mode = BodyMode::Chunked;
        } else if (const auto cl = parsed_head.header("content-length")) {
            std::uint64_t value = 0;
            for (char c : *cl) {
                if (c < '0' || c > '9') {
                    done(make_error_code(client_errc::protocol_error));
                    return;
                }
                value = value * 10 + static_cast<std::uint64_t>(c - '0');
            }
            m_body_remaining = value;
            m_body_mode = value == 0 ? BodyMode::None : BodyMode::Fixed;
            m_body_complete = value == 0;
        } else {
            m_body_mode = BodyMode::EofDelimited;
        }
        done(error_code{});
    }

    void do_exchange_read(ReadCompletion done) {
        // Same as do_ensure_head: the slot must survive the read_head/read_body
        // wrappers so a body-idle deadline can abort the socket read.
        m_read_slot = asio::get_associated_cancellation_slot(done);
        if (!m_busy) {
            std::move(done)(make_error_code(client_errc::session_closed), ReadResult{});
            return;
        }
        if (!m_head_parsed) {
            auto self = this->shared_from_this();
            read_head([self, done = std::move(done)](error_code ec) mutable {
                if (ec) {
                    self->fail_exchange(ec);
                    std::move(done)(ec, ReadResult{});
                    return;
                }
                self->read_body(std::move(done));
            });
            return;
        }
        read_body(std::move(done));
    }

    void read_body(ReadCompletion done) {
        switch (m_body_mode) {
        case BodyMode::None:
            finish_body(std::move(done));
            return;
        case BodyMode::Fixed:
            read_fixed(std::move(done));
            return;
        case BodyMode::Chunked:
            read_chunked(std::move(done));
            return;
        case BodyMode::EofDelimited:
            read_eof_delimited(std::move(done));
            return;
        }
    }

    void finish_body(ReadCompletion done) {
        m_body_complete = true;
        release_exchange();
        std::move(done)(error_code{}, ReadResult::end());
    }

    void fail_read(ReadCompletion done, error_code ec) {
        fail_exchange(ec);
        std::move(done)(ec, ReadResult{});
    }

    void read_fixed(ReadCompletion done) {
        if (m_body_remaining == 0) {
            finish_body(std::move(done));
            return;
        }
        if (!m_in.empty()) {
            const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(m_body_remaining, m_in.size()));
            std::string chunk = m_in.substr(0, take);
            m_in.erase(0, take);
            m_body_remaining -= take;
            std::move(done)(error_code{}, ReadResult::chunk(std::move(chunk)));
            return;
        }
        auto self = this->shared_from_this();
        read_more([self, done = std::move(done)](error_code ec, std::size_t) mutable {
            if (ec) {
                self->fail_read(std::move(done), ec);
                return;
            }
            self->read_fixed(std::move(done));
        });
    }

    void read_chunked(ReadCompletion done) {
        const auto nl = m_in.find("\r\n");
        if (nl == std::string::npos) {
            if (m_in.size() > m_limits.max_header_bytes) {
                fail_read(std::move(done), make_error_code(client_errc::protocol_error));
                return;
            }
            auto self = this->shared_from_this();
            read_more([self, done = std::move(done)](error_code ec, std::size_t) mutable {
                if (ec) {
                    self->fail_read(std::move(done), ec);
                    return;
                }
                self->read_chunked(std::move(done));
            });
            return;
        }
        std::string_view size_line{m_in.data(), nl};
        if (auto semi = size_line.find(';'); semi != std::string_view::npos)
            size_line = size_line.substr(0, semi);
        std::uint64_t size = 0;
        bool any = false;
        for (char c : size_line) {
            unsigned d;
            if (c >= '0' && c <= '9')
                d = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                d = 10u + static_cast<unsigned>(c - 'a');
            else if (c >= 'A' && c <= 'F')
                d = 10u + static_cast<unsigned>(c - 'A');
            else {
                fail_read(std::move(done), make_error_code(client_errc::protocol_error));
                return;
            }
            size = size * 16 + d;
            any = true;
        }
        if (!any) {
            fail_read(std::move(done), make_error_code(client_errc::protocol_error));
            return;
        }
        m_in.erase(0, nl + 2);
        if (size == 0) {
            // Consume the trailer section up to the blank line.
            read_trailers(std::move(done));
            return;
        }
        if (m_in.size() < size + 2) {
            auto self = this->shared_from_this();
            read_more([self, done = std::move(done), size](error_code ec, std::size_t) mutable {
                if (ec) {
                    self->fail_read(std::move(done), ec);
                    return;
                }
                self->read_chunked_data(std::move(done), size);
            });
            return;
        }
        read_chunked_data(std::move(done), size);
    }

    void read_chunked_data(ReadCompletion done, std::uint64_t size) {
        if (m_in.size() < size + 2) {
            auto self = this->shared_from_this();
            read_more([self, done = std::move(done), size](error_code ec, std::size_t) mutable {
                if (ec) {
                    self->fail_read(std::move(done), ec);
                    return;
                }
                self->read_chunked_data(std::move(done), size);
            });
            return;
        }
        if (static_cast<std::size_t>(size) + 2 > m_in.size() ||
            m_in.compare(static_cast<std::size_t>(size), 2, "\r\n") != 0) {
            fail_read(std::move(done), make_error_code(client_errc::protocol_error));
            return;
        }
        std::string chunk = m_in.substr(0, static_cast<std::size_t>(size));
        m_in.erase(0, static_cast<std::size_t>(size) + 2);
        std::move(done)(error_code{}, ReadResult::chunk(std::move(chunk)));
    }

    void read_trailers(ReadCompletion done) {
        const auto nl = m_in.find("\r\n");
        if (nl == std::string::npos) {
            auto self = this->shared_from_this();
            read_more([self, done = std::move(done)](error_code ec, std::size_t) mutable {
                if (ec) {
                    self->fail_read(std::move(done), ec);
                    return;
                }
                self->read_trailers(std::move(done));
            });
            return;
        }
        if (nl == 0) {
            m_in.erase(0, 2);
            if (auto current = m_current.lock()) {
                current->set_trailers(m_trailers);
            }
            finish_body(std::move(done));
            return;
        }
        const std::string_view line{m_in.data(), nl};
        const auto colon = line.find(':');
        if (colon != std::string_view::npos) {
            const std::string name = ascii_lower_copy(trim_ows(line.substr(0, colon)));
            const std::string_view value = trim_ows(line.substr(colon + 1));
            if (!is_forbidden_trailer_field(name))
                m_trailers.add_lower(name, std::string{value});
        }
        m_in.erase(0, nl + 2);
        read_trailers(std::move(done));
    }

    void read_eof_delimited(ReadCompletion done) {
        if (!m_in.empty()) {
            std::string chunk = std::move(m_in);
            m_in.clear();
            std::move(done)(error_code{}, ReadResult::chunk(std::move(chunk)));
            return;
        }
        auto self = this->shared_from_this();
        read_more([self, done = std::move(done)](error_code ec, std::size_t) mutable {
            if (ec == asio::error::eof || ec == asio::error::connection_reset) {
                self->m_close_after = true;
                self->finish_body(std::move(done));
                return;
            }
            if (ec) {
                self->fail_read(std::move(done), ec);
                return;
            }
            self->read_eof_delimited(std::move(done));
        });
    }

    std::shared_ptr<Transport> m_transport;
    asio::any_io_executor m_executor;
    std::string m_authority;
    EngineLimits m_limits;

    std::shared_ptr<Request> m_spec;
    // Weak: the stream owns a strong reference to this session, so the session
    // must not own the stream back. A caller may keep the Stream handle after
    // dropping the Client; that handle keeps the session and transport alive.
    std::weak_ptr<Http1ClientStream<Transport>> m_current;
    std::shared_ptr<std::atomic<bool>> m_current_done;
    std::function<void()> m_exchange_done;

    std::atomic<bool> m_alive{true};
    bool m_busy{false};
    bool m_close_after{false};
    error_code m_connection_error;
    bool m_request_chunked{false};

    std::string m_in;
    std::array<std::byte, 16 * 1024> m_scratch{};

    bool m_head_parsed{false};
    bool m_expect_continue{false}; // the request asked for an interim 100
    bool m_awaiting_final{false};  // a 100 was surfaced; the final still follows
    // Cancellation slot of the pending head/body read, captured at the entry
    // points so per-operation deadlines survive the intermediate wrappers.
    asio::cancellation_slot m_read_slot;
    int m_status{0};
    BodyMode m_body_mode{BodyMode::None};
    std::uint64_t m_body_remaining{0};
    bool m_body_complete{false};
    Headers m_trailers;
};

template <typename Transport> void Http1ClientStream<Transport>::write(std::string data, WriteCompletion done) {
    m_session->exchange_write(std::move(data), /*last=*/false, std::move(done), this);
}

template <typename Transport> void Http1ClientStream<Transport>::finish(std::string data, WriteCompletion done) {
    m_session->exchange_write(std::move(data), /*last=*/true, std::move(done), this);
}

template <typename Transport>
void Http1ClientStream<Transport>::finish_trailers(Headers trailers, WriteCompletion done) {
    m_session->exchange_write_trailers(std::move(trailers), std::move(done), this);
}

template <typename Transport> void Http1ClientStream<Transport>::read(ReadCompletion done) {
    m_session->exchange_read(std::move(done), this);
}

template <typename Transport> void Http1ClientStream<Transport>::cancel(WriteCompletion done) {
    m_session->cancel_exchange(std::move(done), this);
}

template <typename Transport> void Http1ClientStream<Transport>::await_head(WriteCompletion done) {
    m_session->ensure_head(std::move(done), this);
}

} // namespace simple_http::detail
