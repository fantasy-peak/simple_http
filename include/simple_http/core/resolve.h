#pragma once

// Name-resolution hook shared by the outbound client and the WebSocket reverse
// proxy.
//
// The default is asio::ip::tcp::resolver (getaddrinfo / the system resolver).
// An application that has its own asynchronous DNS client (a cache, c-ares,
// DoH, a private upstream, …) can provide the endpoints instead:
//
//     cfg.resolve = [](std::string host, std::string port)
//         -> asio::awaitable<std::pair<error_code,
//                                      std::vector<asio::ip::tcp::endpoint>>> {
//         auto answer = co_await my_dns.resolve(host);
//         if (!answer) co_return {answer.error(), {}};
//         std::vector<asio::ip::tcp::endpoint> endpoints;
//         for (const auto &address : answer->addresses)
//             endpoints.emplace_back(address, static_cast<std::uint16_t>(std::stoul(port)));
//         co_return {{}, std::move(endpoints)};
//     };
//
// The callback must be non-blocking, must complete on the executor of the
// connection that asked, and returns the endpoints in the order they should be
// tried. An empty list with no error is treated as host_not_found by the
// callers. IP literals need no query; the adapter is responsible for returning
// them directly.

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "types.h"

namespace simple_http {

namespace asio = boost::asio;

// (error, endpoints). Empty endpoints + empty error means "no addresses".
using ResolveFn = std::function<asio::awaitable<std::pair<error_code, std::vector<asio::ip::tcp::endpoint>>>(
    std::string host, std::string port)>;

// Resolves `host` for `port`, using `resolve` when set and the system resolver
// otherwise. The client and the WebSocket proxy both go through here, so their
// fallback behavior cannot drift.
template <typename Executor>
asio::awaitable<std::pair<error_code, std::vector<asio::ip::tcp::endpoint>>>
resolve_endpoints(const ResolveFn &resolve, std::string host, std::uint16_t port, const Executor &ex) {
    if (resolve) {
        co_return co_await resolve(std::move(host), std::to_string(port));
    }

    asio::ip::tcp::resolver resolver{ex};
    auto [ec, results] =
        co_await resolver.async_resolve(std::move(host), std::to_string(port), asio::as_tuple(asio::use_awaitable));
    if (ec) {
        co_return std::pair<error_code, std::vector<asio::ip::tcp::endpoint>>{ec, {}};
    }

    std::vector<asio::ip::tcp::endpoint> endpoints;
    endpoints.reserve(results.size());
    for (const auto &entry : results) {
        endpoints.push_back(entry.endpoint());
    }
    co_return std::pair<error_code, std::vector<asio::ip::tcp::endpoint>>{error_code{}, std::move(endpoints)};
}

} // namespace simple_http
