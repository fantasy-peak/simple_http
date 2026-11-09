#pragma once

// The client's request/response model (C++17 token-only port).
//
// The response side mirrors the server's: async_read_head() gives the status
// and headers, then async_read() yields body chunks until one reports
// end-of-body (the same ReadResult the server's Body uses). Whichever runs
// first parses and caches the head. Failures are error codes on the completion,
// so the caller's token decides whether they become values or exceptions.
//
// Version collapsing works as on the server: the caller never branches on
// HTTP/1.1 vs HTTP/2. HTTP/1.1 sessions serve one exchange at a time; HTTP/2
// sessions multiplex.

#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/async_result.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "../core/base64.h"
#include "../core/http_field.h"
#include "../core/http_method.h"
#include "../core/result.h"
#include "../core/types.h"
#include "../proto/body.h"
#include "../proto/headers.h"
#include "../proto/request.h"
#include "../proto/response_writer.h" // WriteCompletion
#include "client_config.h"

namespace simple_http::detail {

namespace asio = boost::asio;

// The response head, as async_read_head() returns it.
struct ResponseHead {
    int status{0};
    Version version{Version::Http11};
    Headers headers;
    bool bodyless{false};
    bool uncompressed{false};

    std::optional<std::string_view> header(std::string_view name) const { return headers.get(name); }
};

enum class StreamState : int {
    Open = 0,
    Eof,
    Reset,
    Disconnect,
    Failed,
};

struct StreamOutcome {
    std::atomic<int> state{static_cast<int>(StreamState::Open)};
    error_code ec;

    StreamState get() const { return static_cast<StreamState>(state.load(std::memory_order_acquire)); }

    void set(StreamState s, error_code e = {}) {
        if (state.load(std::memory_order_relaxed) != static_cast<int>(StreamState::Open))
            return;
        ec = std::move(e);
        state.store(static_cast<int>(s), std::memory_order_release);
    }

    error_code to_error() const { return ec ? ec : make_error_code(client_errc::session_closed); }
};

inline result<ReadResult> outcome_result(const StreamOutcome &outcome) {
    switch (outcome.get()) {
    case StreamState::Eof:
        return ReadResult::end();
    case StreamState::Reset:
        return make_error_code(client_errc::stream_reset);
    case StreamState::Disconnect:
        return make_error_code(asio::error::connection_reset);
    case StreamState::Failed:
        return outcome.to_error();
    case StreamState::Open:
    default:
        return make_error_code(client_errc::session_closed);
    }
}

inline error_code outcome_error(const StreamOutcome &outcome) {
    switch (outcome.get()) {
    case StreamState::Reset:
        return make_error_code(client_errc::stream_reset);
    case StreamState::Disconnect:
        return make_error_code(asio::error::connection_reset);
    case StreamState::Failed:
        return outcome.to_error();
    case StreamState::Eof:
        return make_error_code(client_errc::protocol_error);
    case StreamState::Open:
    default:
        return make_error_code(client_errc::session_closed);
    }
}

using ReadCompletion = asio::any_completion_handler<void(error_code, ReadResult)>;
using OpenCompletion = asio::any_completion_handler<void(error_code, std::shared_ptr<class ClientStream>)>;

// One request/response exchange. Completion signatures:
//   async_write / async_finish / async_finish_trailers / async_cancel -> void(error_code)
//   async_read -> void(error_code, ReadResult)
//   async_read_head -> void(error_code, ResponseHead)
class ClientStream {
  public:
    virtual ~ClientStream() { run_destroy_hook(); }

    void run_destroy_hook() {
        if (m_destroy_hook_fired.exchange(true, std::memory_order_acq_rel))
            return;
        if (m_on_destroy)
            m_on_destroy();
    }

    void set_on_destroy(std::function<void()> cb) { m_on_destroy = std::move(cb); }

    void set_gate_token(std::shared_ptr<void> tok) { m_gate_token = std::move(tok); }
    const std::shared_ptr<void> &gate_token() const { return m_gate_token; }

    // --- request side ---
    template <typename Token> auto async_write(std::string data, Token &&token) {
        auto initiation = [this, data = std::move(data)](auto handler) mutable {
            write(std::move(data), std::move(handler));
        };
        return asio::async_initiate<Token, void(error_code)>(
            detail::make_executor_initiation(std::move(initiation), head_executor()), token);
    }
    template <typename Token> auto async_finish(std::string data, Token &&token) {
        auto initiation = [this, data = std::move(data)](auto handler) mutable {
            finish(std::move(data), std::move(handler));
        };
        return asio::async_initiate<Token, void(error_code)>(
            detail::make_executor_initiation(std::move(initiation), head_executor()), token);
    }
    template <typename Token> auto async_finish_trailers(Headers trailers, Token &&token) {
        auto initiation = [this, trailers = std::move(trailers)](auto handler) mutable {
            finish_trailers(std::move(trailers), std::move(handler));
        };
        return asio::async_initiate<Token, void(error_code)>(
            detail::make_executor_initiation(std::move(initiation), head_executor()), token);
    }

    // --- response side ---
    template <typename Token> auto async_read_head(Token &&token) {
        return asio::async_compose<Token, void(error_code, ResponseHead)>(ReadHeadOp{*this}, token, head_executor());
    }

    const ResponseHead &head() const { return m_head ? *m_head : m_no_head; }
    int status() const { return head().status; }

    [[nodiscard]] virtual const Headers &response_trailers() const noexcept { return m_trailers; }

    template <typename Token> auto async_read(Token &&token) {
        auto initiation = [this](auto handler) { read(std::move(handler)); };
        return asio::async_initiate<Token, void(error_code, ReadResult)>(
            detail::make_executor_initiation(std::move(initiation), head_executor()), token);
    }

    template <typename Token> auto async_read_all(std::size_t max_bytes, Token &&token) {
        return asio::async_compose<Token, void(error_code, std::string)>(ReadAllOp{*this, max_bytes}, token,
                                                                         head_executor());
    }

    // --- state ---
    virtual bool finished() const = 0;
    virtual Version version() const = 0;
    virtual std::uint32_t id() const = 0;

    template <typename Token> auto async_cancel(Token &&token) {
        auto initiation = [this](auto handler) { cancel(std::move(handler)); };
        return asio::async_initiate<Token, void()>(
            detail::make_executor_initiation(std::move(initiation), head_executor()), token);
    }

    virtual void clear_head_cache() { reset_head(); }

    // Token-level virtuals implemented by the session-specific stream.
    virtual void write(std::string data, WriteCompletion done) = 0;
    virtual void finish(std::string data, WriteCompletion done) = 0;
    virtual void finish_trailers(Headers trailers, WriteCompletion done) {
        (void)trailers;
        std::move(done)(make_error_code(client_errc::trailers_not_supported));
    }
    virtual void read(ReadCompletion done) = 0;
    virtual void cancel(WriteCompletion done) = 0;

    // Awaits and publishes the response head; runs on the session executor.
    // Public because decorators forward it through a ClientStream pointer.
    virtual void await_head(WriteCompletion done) = 0;
    virtual asio::any_io_executor head_executor() const = 0;

  protected:
    void set_head(ResponseHead head) {
        if (!m_head)
            m_head = std::make_shared<ResponseHead>();
        *m_head = std::move(head);
        m_head_ready = true;
    }
    void reset_head() { m_head_ready = false; }
    void set_trailers(Headers trailers) { m_trailers = std::move(trailers); }

    bool head_ready() const { return m_head_ready; }
    const std::shared_ptr<ResponseHead> &head_ptr() const { return m_head; }

  private:
    struct ReadHeadOp {
        ClientStream &stream;

        asio::any_io_executor get_executor() const noexcept { return stream.head_executor(); }

        template <typename Self> void operator()(Self &self, error_code ec = {}) {
            if (!started) {
                started = true;
                if (stream.head_ready()) {
                    self.complete(error_code{}, *stream.head_ptr());
                    return;
                }
                stream.await_head(WriteCompletion(std::move(self)));
                return;
            }
            if (ec) {
                self.complete(ec, ResponseHead{});
                return;
            }
            if (!stream.head_ptr()) {
                self.complete(make_error_code(client_errc::protocol_error), ResponseHead{});
                return;
            }
            self.complete(error_code{}, *stream.head_ptr());
        }
        bool started{false};
    };

    struct ReadAllOp {
        ClientStream &stream;
        std::size_t max_bytes;
        std::string out;
        bool started{false};

        asio::any_io_executor get_executor() const noexcept { return stream.head_executor(); }

        template <typename Self> void operator()(Self &self, error_code ec = {}, ReadResult chunk = {}) {
            if (!started) {
                started = true;
                stream.read(ReadCompletion(std::move(self)));
                return;
            }
            if (ec) {
                self.complete(ec, std::string{});
                return;
            }
            if (chunk.eof) {
                self.complete(error_code{}, std::move(out));
                return;
            }
            if (max_bytes != 0 && out.size() + chunk.data.size() > max_bytes) {
                self.complete(make_error_code(client_errc::body_too_large), std::string{});
                return;
            }
            out.append(chunk.data);
            stream.read(ReadCompletion(std::move(self)));
        }
    };

    std::shared_ptr<ResponseHead> m_head;
    bool m_head_ready{false};
    ResponseHead m_no_head{};
    Headers m_trailers;
    std::function<void()> m_on_destroy;
    std::atomic<bool> m_destroy_hook_fired{false};
    std::shared_ptr<void> m_gate_token;
};

class ClientSession {
  public:
    virtual ~ClientSession() = default;

    virtual void open_stream(std::shared_ptr<Request> request, OpenCompletion done) = 0;

    virtual bool alive() const = 0;
    virtual bool reusable() const = 0;
    virtual bool keepable() const = 0;
    virtual bool multiplexes() const = 0;
    virtual Version version() const = 0;
    virtual std::string_view authority() const = 0;
    virtual asio::any_io_executor executor() const = 0;
    virtual void close() = 0;

    virtual void set_on_exchange_done(std::function<void()> cb) { (void)cb; }
    virtual std::shared_ptr<std::atomic<bool>> exchange_done_flag() const { return nullptr; }
};

} // namespace simple_http::detail
