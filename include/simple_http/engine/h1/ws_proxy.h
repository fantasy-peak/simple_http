#pragma once

// WsProxyTunnel -- byte-level WebSocket reverse-proxy pass-through (C++17
// token-only port).
//
// When an Upgrade: websocket request matches a registered proxy route, the H1
// engine does NOT enter the WebSocket message abstraction. Instead it hands the
// connection here: we open a plain TCP connection to the backend, build the
// backend Upgrade request from the inbound fields (same header policy as the
// request-level proxy), wait for and validate the backend's 101, then splice
// the two byte streams verbatim in both directions until either side closes.
//
// Because nothing is decoded, everything passes through untouched: WebSocket
// frame boundaries, fragmentation, client-side masking, control frames and
// permessage-deflate. There is no per-message size cap and no re-framing -- this
// is a transparent tunnel, not a message-level proxy.
//
// The handshake half is shared with the HTTP/2 extended-CONNECT path
// (engine/ws_proxy_common.h); only the tunnel loops differ.
//
// Concurrency (model A): the tunnel runs on the connection's own executor. The
// backend socket is created on that same executor, so both halves of the splice
// are single-threaded and need no locks. The shared state holds strong
// references to both streams for as long as any callback is in flight; each
// direction completes on its own, and `finish` runs exactly once.

#include <algorithm>
#include <array>
#include <boost/asio.hpp>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "../../core/address.h"
#include "../../core/compat.h"
#include "../../core/http_status.h" // reason_phrase (the 502 body/status line)
#include "../../core/logging.h"
#include "../../core/types.h"
#include "../../proto/headers.h"
#include "../../proto/ws_frame.h" // ws_accept_key
#include "../../transport/transport.h"
#include "../dispatcher.h"      // WsProxyTarget
#include "../ws_proxy_common.h" // shared handshake / header policy
#include "h1_parser.h"          // ParsedHead

namespace simple_http {

namespace asio = boost::asio;

// Reconstructs the raw HTTP/1.x request head (request line + headers + blank
// line) from a ParsedHead. Kept as the low-level spelling used by the parser
// tests; the live proxy uses build_ws_upgrade_head(), which also applies the
// Go-style forwarding-header policy.
inline std::string rebuild_request_head(const ParsedHead &head, std::string_view target_override = {}) {
    std::string out;
    out.append(head.method_token);
    out.push_back(' ');
    out.append(target_override.empty() ? std::string_view{head.target} : target_override);
    out.push_back(' ');
    out.append(head.version == Version::Http1 ? "HTTP/1.0" : "HTTP/1.1");
    out.append("\r\n");
    for (const auto &[name, value] : head.headers) {
        out.append(name);
        out.append(": ");
        out.append(value);
        out.append("\r\n");
    }
    out.append("\r\n");
    return out;
}

namespace detail {

// The state machine behind run_ws_proxy. Every asynchronous step captures
// `shared_from_this()`; the final completion runs exactly once.
template <typename Transport> class WsProxyRun : public std::enable_shared_from_this<WsProxyRun<Transport>> {
  public:
    WsProxyRun(std::shared_ptr<Transport> client, const ParsedHead &head, std::string initial, WsProxyTarget target,
               std::chrono::steady_clock::duration idle_timeout, std::size_t max_header_bytes, std::string peer_address,
               bool client_is_tls, asio::any_completion_handler<void(bool)> done)
        : m_client(std::move(client)), m_head(head), m_initial(std::move(initial)), m_target(std::move(target)),
          m_idle_timeout(idle_timeout), m_max_header_bytes(max_header_bytes), m_peer_address(std::move(peer_address)),
          m_client_is_tls(client_is_tls), m_done(std::move(done)), m_timer(m_client->get_executor()) {}

    void start() {
        auto self = this->shared_from_this();
        async_ws_proxy_connect(m_client->get_executor(), m_target,
                               [self](error_code ec, std::shared_ptr<asio::ip::tcp::socket> backend) mutable {
                                   self->on_connected(ec, std::move(backend));
                               });
    }

  private:
    void on_connected(error_code ec, std::shared_ptr<asio::ip::tcp::socket> backend) {
        if (ec) {
            SIMPLE_HTTP_ERROR_LOG("ws-proxy connect {}:{} failed (client {}): {}", m_target.host, m_target.port,
                                  address_string(m_client->peer().address()), ec.message());
            fail(status::bad_gateway);
            return;
        }
        m_backend = std::move(backend);

        const auto qmark = m_head.target.find('?');
        const std::string_view inbound_path = qmark == std::string_view::npos
                                                  ? std::string_view{m_head.target}
                                                  : std::string_view{m_head.target}.substr(0, qmark);
        const std::optional<std::string_view> inbound_query =
            qmark == std::string_view::npos
                ? std::nullopt
                : std::optional<std::string_view>{std::string_view{m_head.target}.substr(qmark + 1)};
        auto replay = ws_proxy_replay_target(m_target.rewrite_path, inbound_path, inbound_query);
        if (!replay) {
            SIMPLE_HTTP_ERROR_LOG("ws-proxy: refusing a request target with CR/LF/NUL (client {})",
                                  address_string(m_client->peer().address()));
            close_backend();
            fail(status::bad_request);
            return;
        }
        m_replay_target = std::move(*replay);

        const auto inbound_host = m_head.headers.get("host");
        if (auto inbound_key = m_head.headers.get("sec-websocket-key"); inbound_key && !inbound_key->empty()) {
            m_key.assign(*inbound_key);
        } else {
            m_key = make_ws_client_key();
        }

        m_preamble = build_ws_upgrade_head(m_head.headers, inbound_host ? *inbound_host : std::string_view{},
                                           m_replay_target, m_target, m_peer_address, m_client_is_tls, m_key);
        // `m_initial` (bytes the client sent behind its handshake) must NOT
        // travel with the replayed request: the backend has not accepted the
        // upgrade yet, so forwarding client bytes would let a client smuggle a
        // second request past the proxy whenever the backend rejects the
        // Upgrade. They are forwarded only after a validated 101.
        auto preamble = std::make_shared<std::string>(std::move(m_preamble));
        auto self = this->shared_from_this();
        asio::async_write(*m_backend, asio::buffer(preamble->data(), preamble->size()),
                          [self, preamble](error_code wec, std::size_t) mutable {
                              if (wec) {
                                  SIMPLE_HTTP_ERROR_LOG("ws-proxy write preamble failed (client {}): {}",
                                                        address_string(self->m_client->peer().address()),
                                                        wec.message());
                                  self->close_backend();
                                  self->fail(status::bad_gateway);
                                  return;
                              }
                              self->read_reply();
                          });
    }

    void read_reply() {
        auto self = this->shared_from_this();
        async_ws_proxy_read_reply(
            *m_backend, m_max_header_bytes, m_target.connect_timeout,
            [self](error_code ec, WsProxyReplyHead reply) mutable { self->on_reply(ec, std::move(reply)); });
    }

    void on_reply(error_code ec, WsProxyReplyHead reply) {
        if (ec) {
            SIMPLE_HTTP_ERROR_LOG("ws-proxy backend handshake failed (client {}): {}",
                                  address_string(m_client->peer().address()), ec.message());
            close_backend();
            fail(status::bad_gateway);
            return;
        }
        if (reply.head.status != status::switching_protocols) {
            // The backend refused the upgrade. Relay its actual response (head
            // plus whatever body bytes already arrived) instead of inventing a
            // 502; the h1 client sees the same 401/403/302 a direct connection
            // would have produced.
            auto out = std::make_shared<std::string>(std::move(reply.raw));
            out->append(reply.initial);
            auto initial = std::make_shared<std::string>(std::move(reply.initial));
            auto self = this->shared_from_this();
            async_ws_proxy_relay_refusal_body(
                *m_backend, *out, *initial, std::move(reply.head.headers), m_target.connect_timeout,
                [self, out](error_code) mutable {
                    auto keep = out;
                    self->m_client->async_write(
                        ConstByteSpan{reinterpret_cast<const std::byte *>(keep->data()), keep->size()},
                        [self, keep](error_code, std::size_t) mutable {
                            self->close_backend();
                            self->m_client->close();
                            self->finish(false);
                        });
                });
            return;
        }

        if (!ws_validate_101(reply.head, ws_accept_key(m_key))) {
            SIMPLE_HTTP_ERROR_LOG("ws-proxy backend answered 101 with a mismatched Upgrade/accept (client {})",
                                  address_string(m_client->peer().address()));
            close_backend();
            fail(status::bad_gateway);
            return;
        }

        m_opening = std::move(reply.raw);
        m_opening.append(reply.initial);

        // The backend accepted the upgrade: only now may the bytes the client
        // sent behind its handshake be forwarded.
        if (!m_initial.empty()) {
            auto initial = std::make_shared<std::string>(std::move(m_initial));
            auto self = this->shared_from_this();
            asio::async_write(*m_backend, asio::buffer(initial->data(), initial->size()),
                              [self, initial](error_code iec, std::size_t) mutable {
                                  if (iec) {
                                      SIMPLE_HTTP_ERROR_LOG(
                                          "ws-proxy write initial tunnel bytes failed (client {}): {}",
                                          address_string(self->m_client->peer().address()), iec.message());
                                      self->close_backend();
                                      self->fail(status::bad_gateway);
                                      return;
                                  }
                                  self->write_opening();
                              });
            return;
        }
        write_opening();
    }

    void write_opening() {
        auto opening = std::make_shared<std::string>(std::move(m_opening));
        auto self = this->shared_from_this();
        m_client->async_write(ConstByteSpan{reinterpret_cast<const std::byte *>(opening->data()), opening->size()},
                              [self, opening](error_code ec, std::size_t) mutable {
                                  if (ec) {
                                      self->close_backend();
                                      self->finish(false);
                                      return;
                                  }
                                  self->start_tunnel();
                              });
    }

    void start_tunnel() {
        touch();
        arm_timer();
        pump_client_to_backend();
        pump_backend_to_client();
    }

    void touch() { m_deadline = std::chrono::steady_clock::now() + m_idle_timeout; }

    void arm_timer() {
        if (m_idle_timeout.count() <= 0)
            return;
        m_timer.expires_at(m_deadline);
        auto self = this->shared_from_this();
        m_timer.async_wait([self](error_code ec) {
            if (!ec)
                self->on_idle();
        });
    }

    void on_idle() {
        if (std::chrono::steady_clock::now() < m_deadline) {
            // touch() only moves the deadline; it does not re-arm the timer.
            // Without this the idle check fires once and then disappears.
            arm_timer();
            return;
        }
        SIMPLE_HTTP_ERROR_LOG("ws-proxy tunnel idle for {}s (client {}); closing",
                              std::chrono::duration_cast<std::chrono::seconds>(m_idle_timeout).count(),
                              address_string(m_client->peer().address()));
        m_client->close();
        close_backend();
    }

    void pump_client_to_backend() {
        auto buf = std::make_shared<std::array<std::byte, 16384>>();
        auto self = this->shared_from_this();
        m_client->async_read_some(ByteSpan{buf->data(), buf->size()},
                                  [self, buf](error_code ec, std::size_t n) mutable {
                                      if (ec || n == 0) {
                                          // Half-close: the client will send no
                                          // more, but the backend may still have
                                          // its Close frame to deliver.
                                          error_code sec;
                                          self->m_backend->shutdown(asio::ip::tcp::socket::shutdown_send, sec);
                                          self->m_client_done = true;
                                          self->maybe_finish();
                                          return;
                                      }
                                      self->touch();
                                      asio::async_write(*self->m_backend, asio::buffer(buf->data(), n),
                                                        [self, buf](error_code wec, std::size_t) mutable {
                                                            if (wec) {
                                                                self->m_client_done = true;
                                                                self->maybe_finish();
                                                                return;
                                                            }
                                                            self->touch();
                                                            self->pump_client_to_backend();
                                                        });
                                  });
    }

    void pump_backend_to_client() {
        auto buf = std::make_shared<std::array<std::byte, 16384>>();
        auto self = this->shared_from_this();
        m_backend->async_read_some(asio::buffer(buf->data(), buf->size()),
                                   [self, buf](error_code ec, std::size_t n) mutable {
                                       if (ec || n == 0) {
                                           // Backend is done: closing the client
                                           // also unblocks the other direction.
                                           self->m_client->close();
                                           self->m_backend_done = true;
                                           self->maybe_finish();
                                           return;
                                       }
                                       self->touch();
                                       self->m_client->async_write(ConstByteSpan{buf->data(), n},
                                                                   [self, buf](error_code wec, std::size_t) mutable {
                                                                       if (wec) {
                                                                           self->m_backend_done = true;
                                                                           self->maybe_finish();
                                                                           return;
                                                                       }
                                                                       self->touch();
                                                                       self->pump_backend_to_client();
                                                                   });
                                   });
    }

    void maybe_finish() {
        if (!m_client_done || !m_backend_done || m_finished)
            return;
        m_finished = true;
        m_timer.cancel();
        m_client->close();
        close_backend();
        finish(true);
    }

    // Answers the client when the tunnel could not be established, then closes
    // both sides. The response is only sent before any tunnel byte reached the
    // client, so it is the response to its upgrade request and nothing else.
    void fail(int status_code) {
        const std::string resp = "HTTP/1.1 " + std::to_string(status_code) + " " +
                                 std::string{reason_phrase(status_code)} +
                                 "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        auto keep = std::make_shared<std::string>(resp);
        auto self = this->shared_from_this();
        m_client->async_write(ConstByteSpan{reinterpret_cast<const std::byte *>(keep->data()), keep->size()},
                              [self, keep](error_code, std::size_t) mutable {
                                  self->m_client->close();
                                  self->finish(false);
                              });
    }

    void close_backend() {
        if (m_backend)
            ws_proxy_close(*m_backend);
    }

    void finish(bool ok) {
        if (m_completed)
            return;
        m_completed = true;
        std::move(m_done)(ok);
    }

    std::shared_ptr<Transport> m_client;
    ParsedHead m_head;
    std::string m_initial;
    WsProxyTarget m_target;
    std::chrono::steady_clock::duration m_idle_timeout;
    std::size_t m_max_header_bytes;
    std::string m_peer_address;
    bool m_client_is_tls;

    std::shared_ptr<asio::ip::tcp::socket> m_backend;
    std::string m_replay_target;
    std::string m_key;
    std::string m_preamble;
    std::string m_opening;
    asio::steady_timer m_timer;
    std::chrono::steady_clock::time_point m_deadline{};
    bool m_client_done{false};
    bool m_backend_done{false};
    bool m_finished{false};
    bool m_completed{false};
    asio::any_completion_handler<void(bool)> m_done;
};

} // namespace detail

// Runs a byte-level WebSocket proxy tunnel between the already-upgraded client
// transport and a freshly-opened backend TCP connection. Completion signature:
// void(bool) -- true when the tunnel was established and finished, false when
// the handshake was refused (the caller treats the connection as finished
// either way).
template <typename Transport, typename Token>
auto async_run_ws_proxy(std::shared_ptr<Transport> client, const ParsedHead &head, std::string initial,
                        WsProxyTarget target, std::chrono::steady_clock::duration idle_timeout,
                        std::size_t max_header_bytes, std::string peer_address, bool client_is_tls, Token &&token) {
    auto executor = client->get_executor();
    return asio::async_initiate<Token, void(bool)>(
        [client = std::move(client), head, initial = std::move(initial), target = std::move(target), idle_timeout,
         max_header_bytes, peer_address = std::move(peer_address), client_is_tls](auto handler) mutable {
            using State = detail::WsProxyRun<Transport>;
            auto state =
                std::make_shared<State>(std::move(client), head, std::move(initial), std::move(target), idle_timeout,
                                        max_header_bytes, std::move(peer_address), client_is_tls, std::move(handler));
            state->start();
        },
        token);
}

// One-shot form for callers that only need the old awaitable-style "run it and
// forget the result" shape.
template <typename Transport>
void run_ws_proxy(std::shared_ptr<Transport> client, const ParsedHead &head, std::string initial, WsProxyTarget target,
                  std::chrono::steady_clock::duration idle_timeout = std::chrono::seconds(120),
                  std::size_t max_header_bytes = 64 * 1024, std::string peer_address = {}, bool client_is_tls = false) {
    async_run_ws_proxy(std::move(client), head, std::move(initial), std::move(target), idle_timeout, max_header_bytes,
                       std::move(peer_address), client_is_tls, [](bool) {});
}

} // namespace simple_http
