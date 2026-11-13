#pragma once

// SerializedWriter: one writer per connection.
//
// A transport must never have two overlapping writes in flight. Every producer
// on an HTTP/1.x connection (the response sink, the WebSocket write pump, the
// upgrade response) funnels its bytes through one SerializedWriter instead of
// calling transport->async_write_seq() itself, so that invariant lives in one
// place instead of three.
//
// Buffer ownership: a WriteEntry owns its bytes, and the queue element itself
// is the stable owner. The entry is not moved out of the queue while its write
// is pending; the scatter-gather descriptors point into the queue element. That
// is safe because std::deque does not invalidate references to elements when
// inserting or erasing at the ends: the pump only push_back()s, and only
// pop_front()s after the write completed. This is what removes the per-write
// shared_ptr (and the "keep a copy of the initiation object alive" dance that
// asio composed operations otherwise need, because async_compose moves `self`
// into the inner operation, so a descriptor built from a moved entry dangles).

#include <array>
#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/error.hpp>

#include "../core/compat.h"
#include "../core/types.h"

namespace simple_http {

namespace asio = boost::asio;

// One queued write: up to three byte ranges (framing head, body, tail). The
// head and body are owned unless the entry was built with borrowed(); the tail
// is always a non-owning view of a buffer that outlives the entry (a static
// string literal, so the usual CRLF / chunk terminator needs no allocation).
class WriteEntry {
  public:
    WriteEntry() = default;

    // `head` and `body` are moved into the entry and written after all earlier
    // entries. `tail` is borrowed; it must outlive the write.
    static WriteEntry owned(std::string head, std::string body = {}, ConstByteSpan tail = {}) {
        WriteEntry entry;
        entry.m_head = std::move(head);
        entry.m_body = std::move(body);
        entry.m_tail = tail;
        return entry;
    }

    // `body` is borrowed from the caller and must stay valid until the write
    // completes, exactly like WebSocket's write_view contract.
    static WriteEntry borrowed(std::string head, ConstByteSpan body, ConstByteSpan tail = {}) {
        WriteEntry entry;
        entry.m_head = std::move(head);
        entry.m_borrowed = body;
        entry.m_borrowed_body = true;
        entry.m_tail = tail;
        return entry;
    }

    std::size_t bytes() const noexcept {
        return m_head.size() + (m_borrowed_body ? m_borrowed.size() : m_body.size()) + m_tail.size();
    }

    bool empty() const noexcept { return m_head.empty() && m_body.empty() && m_borrowed.empty() && m_tail.empty(); }

    // Fills `out` with the non-empty ranges in write order and returns how many
    // were written. Three is the maximum, so no allocation is needed.
    std::size_t append_buffers(std::array<ConstByteSpan, 3> &out) const noexcept {
        std::size_t count = 0;
        if (!m_head.empty()) {
            out[count++] = as_bytes(m_head);
        }
        const ConstByteSpan body = m_borrowed_body ? m_borrowed : as_bytes(m_body);
        if (!body.empty()) {
            out[count++] = body;
        }
        if (!m_tail.empty()) {
            out[count++] = m_tail;
        }
        return count;
    }

  private:
    static ConstByteSpan as_bytes(std::string_view text) noexcept {
        return ConstByteSpan{reinterpret_cast<const std::byte *>(text.data()), text.size()};
    }

    std::string m_head;
    std::string m_body;
    ConstByteSpan m_borrowed{};
    ConstByteSpan m_tail{};
    bool m_borrowed_body{false};
};

// Serializes writes onto one transport: at most one transport write is in
// flight, entries are written in FIFO order, and an error fails every entry
// that has not started yet (the in-flight one completes through the transport
// callback and reports the transport error).
//
// Like every component on a connection, this is confined to the connection's
// executor: enqueue() must be called from it, and completions run there.
template <typename Transport>
class SerializedWriter final : public std::enable_shared_from_this<SerializedWriter<Transport>> {
  public:
    // The connection's idle deadline. The writer touches it when a write
    // starts, matching the per-write deadline update the sinks used to do.
    using Deadline = std::shared_ptr<std::chrono::steady_clock::time_point>;
    using Completion = asio::any_completion_handler<void(error_code)>;

    SerializedWriter(std::shared_ptr<Transport> transport, Deadline deadline,
                     std::chrono::steady_clock::duration idle_timeout)
        : m_transport(std::move(transport)), m_deadline(std::move(deadline)), m_idle_timeout(idle_timeout) {}

    SerializedWriter(const SerializedWriter &) = delete;
    SerializedWriter &operator=(const SerializedWriter &) = delete;

    // Queues `entry`. `done` runs when those bytes have been written, or with
    // the transport error / not_connected once the writer is closed.
    void enqueue(WriteEntry entry, Completion done) {
        if (m_closed) {
            if (done)
                std::move(done)(make_error_code(asio::error::not_connected));
            return;
        }
        m_queue.push_back(Item{std::move(entry), std::move(done)});
        pump();
    }

    // Stops accepting writes and fails everything that has not started. An
    // in-flight entry stays in place (its buffers are referenced by the
    // transport operation) and completes through that operation's callback.
    void close(error_code ec) {
        if (m_closed)
            return;
        m_closed = true;
        fail_queued(ec);
    }

    // True when every enqueued write has reached the transport. The HTTP/1
    // engine uses this to decide whether the connection may be reused.
    bool idle() const noexcept { return !m_in_flight && m_queue.empty(); }
    bool closed() const noexcept { return m_closed; }

    // Entries queued (including the in-flight one). Backpressure/limits use it.
    std::size_t queued() const noexcept { return m_queue.size(); }

  private:
    struct Item {
        WriteEntry entry;
        Completion done;
    };

    void touch_deadline() {
        if (m_deadline)
            *m_deadline = std::chrono::steady_clock::now() + m_idle_timeout;
    }

    void pump() {
        if (m_pumping)
            return;
        m_pumping = true;
        // Load-bearing: if the transport completes synchronously, the handler
        // that holds the inner `self` is destroyed before async_write_seq()
        // returns, so without this reference the loop below could touch a
        // destroyed writer when `done` released the last external owner.
        auto self = this->shared_from_this();
        for (;;) {
            if (m_closed || m_in_flight || m_queue.empty())
                break;
            m_in_flight = true;
            touch_deadline();
            // The front element is the buffer owner: it stays in the queue
            // until the write completes (see the file comment).
            const Item &item = m_queue.front();
            std::array<ConstByteSpan, 3> buffers{};
            const std::size_t count = item.entry.append_buffers(buffers);
            m_transport->async_write_seq(boost::span<const ConstByteSpan>{buffers.data(), count},
                                         [self](error_code ec, std::size_t) { self->on_written(ec); });
            // A synchronous completion cleared m_in_flight and popped the
            // entry; loop for the next one. An asynchronous one owns the
            // transport until it calls back.
        }
        m_pumping = false;
    }

    void on_written(error_code ec) {
        // No self reference needed: the transport completion handler that calls
        // this holds a strong reference for the whole call.
        if (m_queue.empty())
            return;
        m_in_flight = false;
        Item item = std::move(m_queue.front());
        m_queue.pop_front();
        if (ec)
            m_closed = true;
        if (item.done)
            std::move(item.done)(ec);
        if (ec) {
            fail_queued(make_error_code(asio::error::not_connected));
            return;
        }
        pump();
    }

    // Fails every entry except a still-in-flight front. Entries are removed
    // from the back: std::deque keeps references to the remaining elements
    // valid, so the in-flight front's buffers stay put while its completion is
    // pending. The collected entries are then completed in FIFO order.
    void fail_queued(error_code ec) {
        // Load-bearing: the loop runs completions, and a completion may release
        // the last external owner of this writer before the next iteration.
        auto self = this->shared_from_this();
        const std::size_t keep = m_in_flight ? 1 : 0;
        std::vector<Item> failed;
        failed.reserve(m_queue.size() > keep ? m_queue.size() - keep : 0);
        while (m_queue.size() > keep) {
            failed.push_back(std::move(m_queue.back()));
            m_queue.pop_back();
        }
        for (auto it = failed.rbegin(); it != failed.rend(); ++it) {
            if (it->done)
                std::move(it->done)(ec);
        }
    }

    std::shared_ptr<Transport> m_transport;
    Deadline m_deadline;
    std::chrono::steady_clock::duration m_idle_timeout{};
    std::deque<Item> m_queue;
    bool m_in_flight{false};
    bool m_pumping{false};
    bool m_closed{false};
};

} // namespace simple_http
