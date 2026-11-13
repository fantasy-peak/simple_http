// transport/serialized_writer.h: the connection-level write path shared by the
// HTTP/1 response sink and the WebSocket backend.
//
// These tests drive the writer over a transport whose writes do not complete
// until the test says so. That is what makes the ownership rule observable: the
// scatter-gather descriptors point into the queue entry, so the entry must not
// move or die while the transport write is pending. The transport only reads
// the bytes when the completion runs, so a lifetime mistake is caught by ASan
// rather than passing because of a lucky copy.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "simple_http.h"
#include "simple_http/transport/serialized_writer.h"
#include "test_support.h"

using namespace simple_http;
using namespace simple_http::test;

namespace {

// Records the buffer *descriptors* at initiation and copies the bytes only
// when complete() runs. An entry that left its queue slot in between would read
// freed memory here.
class DeferredTransport {
  public:
    explicit DeferredTransport(asio::any_io_executor ex) : m_executor(std::move(ex)) {}

    template <typename Token> auto async_write_seq(boost::span<const ConstByteSpan> buffers, Token &&token) {
        ++m_starts;
        m_pending.assign(buffers.begin(), buffers.end());
        auto initiation = [this](auto handler) {
            m_handler = asio::any_completion_handler<void(error_code, std::size_t)>(std::move(handler));
        };
        return asio::async_initiate<Token, void(error_code, std::size_t)>(
            detail::make_executor_initiation(std::move(initiation), m_executor), token);
    }

    // Completes the pending write, reading its buffers at this point.
    void complete(error_code ec = {}) {
        std::string chunk;
        for (const auto &buffer : m_pending) {
            chunk.append(reinterpret_cast<const char *>(buffer.data()), buffer.size());
        }
        m_pending.clear();
        m_written += chunk;
        m_writes.push_back(std::move(chunk));
        auto handler = std::move(m_handler);
        m_handler = nullptr;
        std::move(handler)(ec, m_writes.back().size());
    }

    bool write_pending() const { return static_cast<bool>(m_handler); }
    int starts() const { return m_starts; }
    const std::string &written() const { return m_written; }
    const std::vector<std::string> &writes() const { return m_writes; }

    // Drops the pending completion without running it. The stored handler holds
    // the writer, and the writer holds this transport, so a test that ends with
    // a write in flight would otherwise leave a reference cycle behind (a real
    // asio stream keeps pending operations in the io_context, not in the
    // transport, so this cycle only exists in the test double).
    void abandon_pending() {
        m_handler = nullptr;
        m_pending.clear();
    }

    auto get_executor() { return m_executor; }

  private:
    asio::any_io_executor m_executor;
    std::vector<ConstByteSpan> m_pending;
    asio::any_completion_handler<void(error_code, std::size_t)> m_handler;
    std::vector<std::string> m_writes;
    std::string m_written;
    int m_starts{0};
};

struct Fixture {
    explicit Fixture(std::chrono::seconds idle = std::chrono::seconds(30))
        : deadline(std::make_shared<std::chrono::steady_clock::time_point>()),
          transport(std::make_shared<DeferredTransport>(ctx.get_executor())),
          writer(std::make_shared<SerializedWriter<DeferredTransport>>(transport, deadline, idle)) {}

    ~Fixture() {
        // Break the test double's writer <-> transport cycle before the members
        // are released (see DeferredTransport::abandon_pending).
        transport->abandon_pending();
    }

    asio::io_context ctx;
    SerializedWriter<DeferredTransport>::Deadline deadline;
    std::shared_ptr<DeferredTransport> transport;
    std::shared_ptr<SerializedWriter<DeferredTransport>> writer;
};

} // namespace

TEST_CASE("serialized writer: entries are written in FIFO order", "[transport][writer]") {
    Fixture f;
    std::vector<int> completed;

    f.writer->enqueue(WriteEntry::owned("head1", "body1"), [&](error_code ec) { completed.push_back(ec ? -1 : 1); });
    CHECK(f.transport->starts() == 1); // only one transport write in flight
    CHECK(f.transport->write_pending());

    // A second entry waits behind the first; it must not start a second write.
    f.writer->enqueue(WriteEntry::owned("head2", "body2"), [&](error_code ec) { completed.push_back(ec ? -1 : 2); });
    CHECK(f.transport->starts() == 1);
    CHECK(f.writer->queued() == 2);
    CHECK_FALSE(f.writer->idle());

    f.transport->complete();
    CHECK(completed == std::vector<int>{1});
    CHECK(f.transport->starts() == 2);

    f.transport->complete();
    CHECK(completed == std::vector<int>{1, 2});
    CHECK(f.transport->written() == "head1body1head2body2");
    CHECK(f.writer->idle());
}

TEST_CASE("serialized writer: buffers stay valid until the write completes", "[transport][writer]") {
    Fixture f;
    bool done = false;

    {
        // The strings live only in the entry; the enclosing scope ends before
        // the write completes.
        std::string head = "0123456789";
        std::string body(256, 'x');
        f.writer->enqueue(WriteEntry::owned(std::move(head), std::move(body)), [&](error_code ec) { done = !ec; });
    }
    CHECK(f.transport->write_pending());
    f.transport->complete();
    CHECK(done);
    CHECK(f.transport->written().size() == 10 + 256);
    CHECK(f.transport->written().substr(0, 10) == "0123456789");
}

TEST_CASE("serialized writer: borrowed entries are not copied", "[transport][writer]") {
    Fixture f;
    std::string borrowed = "borrowed-buffer";
    const auto span = ConstByteSpan{reinterpret_cast<const std::byte *>(borrowed.data()), borrowed.size()};
    bool done = false;

    f.writer->enqueue(WriteEntry::borrowed("h", span), [&](error_code ec) { done = !ec; });
    f.transport->complete();
    CHECK(done);
    CHECK(f.transport->written() == "hborrowed-buffer");
}

TEST_CASE("serialized writer: a write error fails the remaining entries", "[transport][writer]") {
    Fixture f;
    error_code first;
    error_code second;
    error_code third;

    f.writer->enqueue(WriteEntry::owned("a"), [&](error_code ec) { first = ec; });
    f.writer->enqueue(WriteEntry::owned("b"), [&](error_code ec) { second = ec; });
    f.writer->enqueue(WriteEntry::owned("c"), [&](error_code ec) { third = ec; });

    f.transport->complete(make_error_code(asio::error::connection_reset));

    CHECK(first == asio::error::connection_reset);
    // Everything queued behind the failed write reports a dead connection and
    // never reaches the transport.
    CHECK(second == asio::error::not_connected);
    CHECK(third == asio::error::not_connected);
    CHECK(f.transport->starts() == 1);
    CHECK(f.writer->closed());
}

TEST_CASE("serialized writer: close fails queued entries but not the in-flight one", "[transport][writer]") {
    Fixture f;
    error_code inflight_ec;
    error_code queued_ec;

    f.writer->enqueue(WriteEntry::owned("a"), [&](error_code ec) { inflight_ec = ec; });
    f.writer->enqueue(WriteEntry::owned("b"), [&](error_code ec) { queued_ec = ec; });
    f.writer->close(make_error_code(asio::error::operation_aborted));

    CHECK(queued_ec == asio::error::operation_aborted);
    CHECK(inflight_ec == error_code{}); // the in-flight write has not completed yet
    CHECK(f.transport->write_pending());

    // The in-flight entry completes through its own transport callback.
    f.transport->complete(make_error_code(asio::error::operation_aborted));
    CHECK(inflight_ec == asio::error::operation_aborted);
    CHECK(f.writer->closed());
    CHECK(f.writer->idle());
}

TEST_CASE("serialized writer: enqueue after close completes immediately", "[transport][writer]") {
    Fixture f;
    f.writer->close(make_error_code(asio::error::operation_aborted));

    error_code ec;
    f.writer->enqueue(WriteEntry::owned("late"), [&](error_code e) { ec = e; });
    CHECK(ec == asio::error::not_connected);
    CHECK(f.transport->starts() == 0);
}

TEST_CASE("serialized writer: completions may enqueue more work", "[transport][writer]") {
    Fixture f;
    std::vector<std::string> order;

    f.writer->enqueue(WriteEntry::owned("one"), [&](error_code) {
        order.push_back("one");
        // Re-entering while the writer is pumping must not start a competing
        // write or reorder the queue.
        f.writer->enqueue(WriteEntry::owned("three"), [&](error_code) { order.push_back("three"); });
    });
    f.writer->enqueue(WriteEntry::owned("two"), [&](error_code) { order.push_back("two"); });

    CHECK(f.transport->starts() == 1);
    f.transport->complete(); // "one" completes; "two" starts
    CHECK(order == std::vector<std::string>{"one"});
    CHECK(f.transport->starts() == 2);
    CHECK(f.transport->written() == "one");

    f.transport->complete(); // "two"
    CHECK(f.transport->starts() == 3);
    f.transport->complete(); // "three"
    CHECK(order == std::vector<std::string>{"one", "two", "three"});
    CHECK(f.transport->written() == "onetwothree");
    CHECK(f.writer->idle());
}

TEST_CASE("serialized writer: the idle deadline advances on each write", "[transport][writer]") {
    Fixture f(std::chrono::seconds(2));
    const auto before = *f.deadline;

    f.writer->enqueue(WriteEntry::owned("x"), [](error_code) {});
    CHECK(*f.deadline > before);

    const auto first = *f.deadline;
    f.transport->complete();
    f.writer->enqueue(WriteEntry::owned("y"), [](error_code) {});
    CHECK(*f.deadline >= first);
}
