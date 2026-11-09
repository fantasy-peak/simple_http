#pragma once

// Client-side WebSocket (RFC 6455), C++17 token-only port.
//
// The client sends the upgrade request, verifies the 101 and
// Sec-WebSocket-Accept, then hands the connection to the same
// WsBackendImpl/WebSocket used by the server. Completion signature:
// void(error_code, std::shared_ptr<WebSocket>).

#include <array>
#include <boost/asio.hpp>
#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/steady_timer.hpp>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <utility>

#include "../core/base64.h"
#include "../core/compat.h"
#include "../core/logging.h"
#include "../core/types.h"
#include "../core/version.h"
#include "../proto/headers.h"
#include "../proto/websocket.h"
#include "client_config.h"

namespace simple_http {

namespace asio = boost::asio;

struct WebSocketSpec {
    Headers headers;
    std::string origin;
    std::size_t max_payload{0};
    std::size_t read_ahead_bytes{0};
};

namespace detail {

inline std::string ws_client_key() {
    unsigned char random[16];
    std::random_device rd;
    for (auto &b : random)
        b = static_cast<unsigned char>(rd());
    return base64_encode(std::string_view{reinterpret_cast<const char *>(random), sizeof(random)});
}

using WsOpenCompletion = asio::any_completion_handler<void(error_code, std::shared_ptr<WebSocket>)>;

template <typename Transport>
void async_ws_upgrade_impl(std::shared_ptr<Transport> transport, std::string authority, std::string path,
                           WebSocketSpec spec, std::chrono::milliseconds idle_timeout,
                           std::chrono::milliseconds handshake_timeout, WsOpenCompletion done) {
    const std::string key = ws_client_key();
    std::string request;
    request += "GET " + (path.empty() ? std::string{"/"} : path) + " HTTP/1.1\r\n";
    request += "host: " + authority + "\r\n";
    request += "upgrade: websocket\r\n";
    request += "connection: Upgrade\r\n";
    request += "sec-websocket-key: " + key + "\r\n";
    request += "sec-websocket-version: 13\r\n";
    if (!spec.origin.empty()) {
        request += "origin: " + spec.origin + "\r\n";
    }
    for (const auto &[name, value] : spec.headers) {
        if (contains_ctl(name) || contains_ctl(value)) {
            transport->close();
            std::move(done)(make_error_code(asio::error::invalid_argument), nullptr);
            return;
        }
        request += name;
        request += ": ";
        request += value;
        request += "\r\n";
    }
    request += "user-agent: ";
    request += client_product;
    request += "\r\n\r\n";

    auto request_keep = std::make_shared<std::string>(std::move(request));
    auto response = std::make_shared<std::string>();
    auto read_buf = std::make_shared<std::array<std::byte, 512>>();
    auto step = std::make_shared<std::function<void()>>();
    auto completion = std::make_shared<WsOpenCompletion>(std::move(done));
    auto timer = std::make_shared<asio::steady_timer>(transport->get_executor());
    auto timed_out = std::make_shared<bool>(false);
    if (handshake_timeout.count() > 0) {
        timer->expires_after(handshake_timeout);
        timer->async_wait([transport, timed_out](error_code ec) {
            if (!ec) {
                *timed_out = true;
                transport->close();
            }
        });
    }
    auto finish = [completion, timer, timed_out](error_code ec, std::shared_ptr<WebSocket> ws) mutable {
        timer->cancel();
        if (ec && *timed_out)
            ec = make_error_code(client_errc::response_head_timeout);
        auto callback = std::move(*completion);
        std::move(callback)(ec, std::move(ws));
    };

    // Weak self-reference, like middleware::timeout: the function owns the
    // continuation stored in `step`, so a strong capture would cycle. The
    // pending read handler holds `keep` for as long as the step is in flight.
    std::weak_ptr<std::function<void()>> weak_step = step;
    *step = [transport, request_keep, response, read_buf, key, spec, idle_timeout, authority, finish,
             weak_step]() mutable {
        auto keep = weak_step.lock();
        if (!keep) {
            return;
        }
        // Read more of the head.
        constexpr std::size_t kMaxHead = 16384;
        transport->async_read_some(
            ByteSpan{read_buf->data(), read_buf->size()},
            [transport, request_keep, response, read_buf, key, spec, idle_timeout, authority, finish,
             keep](error_code ec, std::size_t n) mutable {
                if (ec) {
                    transport->close();
                    finish(ec, nullptr);
                    return;
                }
                response->append(reinterpret_cast<const char *>(read_buf->data()), n);
                if (response->find("\r\n\r\n") == std::string::npos) {
                    if (response->size() > kMaxHead) {
                        transport->close();
                        finish(make_error_code(client_errc::header_too_large), nullptr);
                        return;
                    }
                    (*keep)();
                    return;
                }
                const std::size_t head_end = response->find("\r\n\r\n") + 4;
                const std::string head = response->substr(0, head_end);
                const std::string extra = response->substr(head_end);
                if (!starts_with(head, "HTTP/1.1 101")) {
                    SIMPLE_HTTP_ERROR_LOG("ws client: upgrade to {} refused (not 101)", authority);
                    transport->close();
                    finish(make_error_code(client_errc::protocol_error), nullptr);
                    return;
                }
                // Accept-key check.
                std::string_view accept;
                for (std::size_t pos = 0;;) {
                    const std::size_t nl = head.find("\r\n", pos);
                    if (nl == std::string::npos || nl >= head_end)
                        break;
                    const std::string_view line = std::string_view{head}.substr(pos, nl - pos);
                    if (line.empty())
                        break;
                    const std::size_t colon = line.find(':');
                    if (colon != std::string_view::npos) {
                        std::string lower;
                        for (char c : line.substr(0, colon))
                            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
                        if (lower == "sec-websocket-accept") {
                            std::string_view value = line.substr(colon + 1);
                            while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                                value.remove_prefix(1);
                            accept = value;
                        }
                    }
                    pos = nl + 2;
                }
                if (accept != ws_accept_key(key)) {
                    transport->close();
                    finish(make_error_code(client_errc::protocol_error), nullptr);
                    return;
                }
                auto backend = std::make_shared<WsBackendImpl<Transport>>(
                    transport, spec.max_payload != 0 ? spec.max_payload : 16u * 1024 * 1024, idle_timeout,
                    /*expect_masked=*/false, spec.read_ahead_bytes);
                backend->feed(extra);
                auto ws = std::make_shared<WebSocket>(std::move(backend));
                ws->run_writer();
                finish(error_code{}, std::move(ws));
            });
    };

    transport->async_write(
        ConstByteSpan{reinterpret_cast<const std::byte *>(request_keep->data()), request_keep->size()},
        [transport, request_keep, response, read_buf, key, spec, idle_timeout, authority, finish,
         step](error_code ec, std::size_t) mutable {
            if (ec) {
                transport->close();
                finish(ec, nullptr);
                return;
            }
            (*step)();
        });
}

// The handshake read is bounded by the caller's response-head budget.
template <typename Transport, typename Token>
auto async_ws_upgrade(std::shared_ptr<Transport> transport, std::string authority, std::string path, WebSocketSpec spec,
                      std::chrono::milliseconds idle_timeout, std::chrono::milliseconds handshake_timeout,
                      Token &&token) {
    return asio::async_initiate<Token, void(error_code, std::shared_ptr<WebSocket>)>(
        [transport, authority = std::move(authority), path = std::move(path), spec = std::move(spec), idle_timeout,
         handshake_timeout](auto handler) mutable {
            async_ws_upgrade_impl(std::move(transport), std::move(authority), std::move(path), std::move(spec),
                                  idle_timeout, handshake_timeout, std::move(handler));
        },
        token);
}

} // namespace detail

} // namespace simple_http
