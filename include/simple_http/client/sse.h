#pragma once

// Server-Sent Events client (WHATWG HTML §9.2): an incremental parser plus a
// thin wrapper over the streaming client. The parser is deliberately pure (no
// sockets) so it is unit-testable; SseStream ties it to a Client exchange.
//
//   auto sse = co_await simple_http::SseStream::open(client, "https://host/events");
//   while (auto ev = co_await sse->next()) {
//       if (ev->event == "message") handle(ev->data);
//   }
//
// Reconnection is the caller's policy: last_event_id() is exposed so a caller
// can re-open with `SseOptions::last_event_id` and send Last-Event-ID (the
// parser keeps the id across events, as the spec requires).

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <deque>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../proto/body.h" // ReadResult
#include "http.h"          // Client, Stream, StreamSpec

namespace simple_http {

struct SseEvent {
    std::string event{"message"};
    std::string data;
    std::string id; // current Last-Event-ID, empty before any id field
    std::optional<int> retry_ms;
};

struct SseOptions {
    Headers headers;                 // extra request headers (auth, cookies)
    std::string last_event_id;       // sent as Last-Event-ID
    bool require_event_stream{true}; // reject a 200 that is not text/event-stream
};

// Incremental text/event-stream parser. feed() accepts arbitrary chunk
// boundaries (including a CR/LF split across chunks); finish() dispatches a
// final event at end-of-stream.
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
            return; // comment
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
        // An event with no data line is not dispatched, but its id/event fields
        // stay pending until the next block, matching the spec's buffer reset.
        if (!m_has_data) {
            m_event.clear();
            return;
        }
        SseEvent ev;
        ev.event = m_event.empty() ? "message" : m_event;
        ev.data = std::move(m_data);
        if (!ev.data.empty() && ev.data.back() == '\n') {
            ev.data.pop_back();
        }
        ev.id = m_id;
        ev.retry_ms = m_retry;
        out.push_back(std::move(ev));
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

// A live SSE response. next() yields one complete event per call; end-of-stream
// surfaces as the error channel (asio::error::eof) so the caller can decide
// whether to reconnect.
class SseStream {
  public:
    explicit SseStream(Stream stream) : m_stream(std::move(stream)) {}

    static asio::awaitable<std::expected<SseStream, error_code>> open(Client &client, std::string url,
                                                                      SseOptions opts = {}) {
        StreamSpec spec;
        spec.headers = std::move(opts.headers);
        if (!opts.last_event_id.empty()) {
            spec.headers.add("last-event-id", opts.last_event_id);
        }
        auto opened = co_await client.open_stream(std::move(url), std::move(spec));
        if (!opened) {
            co_return std::unexpected{opened.error()};
        }
        Stream stream = std::move(*opened);
        auto head = co_await stream.read_head();
        if (!head) {
            co_return std::unexpected{head.error()};
        }
        if (head->status != status::ok) {
            co_return std::unexpected{make_error_code(asio::error::invalid_argument)};
        }
        if (opts.require_event_stream) {
            const auto content_type = head->header("content-type");
            if (!content_type || content_type->find("text/event-stream") == std::string_view::npos) {
                co_return std::unexpected{make_error_code(asio::error::invalid_argument)};
            }
        }
        co_return SseStream(std::move(stream));
    }

    asio::awaitable<std::expected<SseEvent, error_code>> next() {
        for (;;) {
            if (!m_pending.empty()) {
                auto ev = std::move(m_pending.front());
                m_pending.pop_front();
                if (!ev.id.empty()) {
                    m_last_id = ev.id;
                }
                co_return ev;
            }
            if (m_eof) {
                co_return std::unexpected{make_error_code(asio::error::eof)};
            }
            auto chunk = co_await m_stream.read();
            if (!chunk) {
                co_return std::unexpected{chunk.error()};
            }
            if (chunk->eof) {
                m_eof = true;
                for (auto &ev : m_parser.finish()) {
                    m_pending.push_back(std::move(ev));
                }
                continue;
            }
            for (auto &ev : m_parser.feed(chunk->data)) {
                m_pending.push_back(std::move(ev));
            }
        }
    }

    [[nodiscard]] const std::string &last_event_id() const noexcept { return m_last_id; }
    [[nodiscard]] bool eof() const noexcept { return m_eof && m_pending.empty(); }

  private:
    Stream m_stream;
    SseParser m_parser;
    std::deque<SseEvent> m_pending;
    std::string m_last_id;
    bool m_eof{false};
};

} // namespace simple_http
