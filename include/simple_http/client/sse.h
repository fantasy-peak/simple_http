#pragma once

// Server-Sent Events client (WHATWG HTML §9.2), C++17 token-only port.
//
// The parser is pure and unit-testable; SseStream ties it to a Client exchange:
//
//   client.async_open_sse(url, opts, [](error_code ec, std::shared_ptr<SseStream> sse) { ... });
//   sse->async_next([](error_code ec, SseEvent event) { ... });
//
// End-of-stream surfaces as asio::error::eof so the caller decides whether to
// reconnect (last_event_id() is exposed for that).

#include <algorithm>
#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/async_result.hpp>
#include <charconv>
#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../proto/body.h"
#include "http.h"

namespace simple_http {

struct SseEvent {
    std::string event{"message"};
    std::string data;
    std::string id;
    std::optional<int> retry_ms;
};

struct SseOptions {
    Headers headers;
    std::string last_event_id;
    bool require_event_stream{true};
};

class SseParser {
  public:
    std::vector<SseEvent> feed(std::string_view bytes) {
        m_buf.append(bytes);
        std::vector<SseEvent> out;
        std::size_t pos = 0;
        for (;;) {
            std::size_t end = std::string::npos;
            std::size_t term = 0;
            for (std::size_t i = pos; i < m_buf.size(); ++i) {
                if (m_buf[i] == '\n') {
                    end = i;
                    term = 1;
                    break;
                }
                if (m_buf[i] == '\r') {
                    if (i + 1 >= m_buf.size()) {
                        break; // a trailing CR may be the first half of CRLF: wait
                    }
                    end = i;
                    term = (m_buf[i + 1] == '\n') ? 2 : 1;
                    break;
                }
            }
            if (end == std::string::npos) {
                break;
            }
            process_line(std::string_view{m_buf.data() + pos, end - pos}, out);
            pos = end + term;
        }
        m_buf.erase(0, pos);
        return out;
    }

    std::vector<SseEvent> finish() {
        std::vector<SseEvent> out;
        if (!m_buf.empty()) {
            process_line(m_buf, out);
            m_buf.clear();
        }
        dispatch(out);
        return out;
    }

  private:
    void process_line(std::string_view line, std::vector<SseEvent> &out) {
        if (line.empty()) {
            dispatch(out);
            return;
        }
        if (line.front() == ':') {
            return;
        }
        const auto colon = line.find(':');
        const auto field = line.substr(0, colon);
        std::string_view value;
        if (colon != std::string_view::npos) {
            value = line.substr(colon + 1);
            if (!value.empty() && value.front() == ' ') {
                value.remove_prefix(1);
            }
        }
        if (field == "data") {
            m_data.append(value);
            m_data.push_back('\n');
            m_has_data = true;
        } else if (field == "event") {
            m_event.assign(value);
        } else if (field == "id") {
            if (value.find('\0') == std::string_view::npos) {
                m_id.assign(value);
            }
        } else if (field == "retry") {
            int parsed = 0;
            const auto *first = value.data();
            const auto *last = value.data() + value.size();
            if (!value.empty() && std::from_chars(first, last, parsed).ec == std::errc{}) {
                m_retry = parsed;
            }
        }
    }

    void dispatch(std::vector<SseEvent> &out) {
        if (!m_has_data) {
            m_event.clear();
            return;
        }
        SseEvent event;
        event.event = m_event.empty() ? "message" : m_event;
        event.data = std::move(m_data);
        if (!event.data.empty() && event.data.back() == '\n') {
            event.data.pop_back();
        }
        event.id = m_id;
        event.retry_ms = m_retry;
        out.push_back(std::move(event));
        m_data.clear();
        m_has_data = false;
        m_event.clear();
        m_retry.reset();
    }

    std::string m_buf;
    std::string m_data;
    std::string m_event;
    std::string m_id;
    std::optional<int> m_retry;
    bool m_has_data{false};
};

class SseStream : public std::enable_shared_from_this<SseStream> {
  public:
    explicit SseStream(std::shared_ptr<Stream> stream) : m_stream(std::move(stream)) {}

    // Opens the exchange, finishes the (empty) request body, verifies the head,
    // and completes with the stream handle.
    template <typename Token>
    static auto async_open(Client &client, std::string url, SseOptions options, Token &&token) {
        return asio::async_initiate<Token, void(error_code, std::shared_ptr<SseStream>)>(
            [&client, url = std::move(url), options = std::move(options)](auto handler) mutable {
                StreamSpec spec;
                spec.headers = std::move(options.headers);
                if (!options.last_event_id.empty())
                    spec.headers.add("last-event-id", options.last_event_id);
                const bool require_event_stream = options.require_event_stream;
                client.async_open_stream(
                    std::move(url), std::move(spec), 0,
                    [require_event_stream, handler = std::move(handler)](error_code ec,
                                                                         std::shared_ptr<Stream> stream) mutable {
                        if (ec) {
                            std::move(handler)(ec, nullptr);
                            return;
                        }
                        stream->async_finish(std::string{}, [stream, require_event_stream,
                                                             handler = std::move(handler)](error_code fec) mutable {
                            if (fec) {
                                std::move(handler)(fec, nullptr);
                                return;
                            }
                            stream->async_read_head([stream, require_event_stream, handler = std::move(handler)](
                                                        error_code hec, detail::ResponseHead head) mutable {
                                if (hec) {
                                    std::move(handler)(hec, nullptr);
                                    return;
                                }
                                if (head.status != status::ok) {
                                    std::move(handler)(make_error_code(asio::error::invalid_argument), nullptr);
                                    return;
                                }
                                if (require_event_stream) {
                                    const auto content_type = head.header("content-type");
                                    if (!content_type ||
                                        content_type->find("text/event-stream") == std::string_view::npos) {
                                        std::move(handler)(make_error_code(asio::error::invalid_argument), nullptr);
                                        return;
                                    }
                                }
                                std::move(handler)(error_code{}, std::make_shared<SseStream>(std::move(stream)));
                            });
                        });
                    });
            },
            token);
    }

    template <typename Token> auto async_next(Token &&token) {
        return asio::async_initiate<Token, void(error_code, SseEvent)>(
            [self = this->shared_from_this()](auto handler) mutable {
                self->next_impl(
                    std::make_shared<asio::any_completion_handler<void(error_code, SseEvent)>>(std::move(handler)));
            },
            token);
    }

    // Abandons the underlying exchange (the caller does not plan to read to EOF).
    void abort() {
        if (m_stream)
            m_stream->abort();
    }

    [[nodiscard]] const std::string &last_event_id() const noexcept { return m_last_id; }
    [[nodiscard]] bool eof() const noexcept { return m_eof && m_pending.empty(); }

  private:
    void next_impl(std::shared_ptr<asio::any_completion_handler<void(error_code, SseEvent)>> done) {
        for (;;) {
            if (!m_pending.empty()) {
                SseEvent event = std::move(m_pending.front());
                m_pending.pop_front();
                if (!event.id.empty())
                    m_last_id = event.id;
                auto callback = std::move(*done);
                std::move(callback)(error_code{}, std::move(event));
                return;
            }
            if (m_eof) {
                auto callback = std::move(*done);
                std::move(callback)(make_error_code(asio::error::eof), SseEvent{});
                return;
            }
            auto self = this->shared_from_this();
            m_stream->async_read([self, done = std::move(done)](error_code ec, ReadResult chunk) mutable {
                if (ec) {
                    auto callback = std::move(*done);
                    std::move(callback)(ec, SseEvent{});
                    return;
                }
                if (chunk.eof) {
                    self->m_eof = true;
                    for (auto &event : self->m_parser.finish())
                        self->m_pending.push_back(std::move(event));
                } else {
                    for (auto &event : self->m_parser.feed(chunk.data))
                        self->m_pending.push_back(std::move(event));
                }
                self->next_impl(std::move(done));
            });
            return;
        }
    }

    std::shared_ptr<Stream> m_stream;
    SseParser m_parser;
    std::deque<SseEvent> m_pending;
    std::string m_last_id;
    bool m_eof{false};
};

} // namespace simple_http
