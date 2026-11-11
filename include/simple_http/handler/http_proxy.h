#pragma once

// Request-level HTTP reverse proxy (C++17 token-only port).
//
// Each proxied request builds a fresh Client for the backend route, buffers the
// request body, forwards the request (hop-by-hop and forwarding fields handled
// as Go's ReverseProxy does), and relays the backend response. The buffered
// shape keeps this first port small; streaming request/response bodies through
// the proxy is a later refinement.
//
// CONNECT is rejected with 405 (a request-level proxy is not a tunnel).

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "../client/http.h"
#include "../core/address.h"
#include "../core/http_status.h"
#include "../core/logging.h"
#include "../core/types.h"
#include "../engine/dispatcher.h"
#include "../proto/headers.h"
#include "../proto/request.h"
#include "../proto/response.h"

namespace simple_http {

namespace asio = boost::asio;

namespace detail {

inline bool proxy_drops_request_field(std::string_view name) {
    return is_hop_by_hop_field(name) || name == "forwarded" || name == "x-forwarded-for" ||
           name == "x-forwarded-host" || name == "x-forwarded-proto" || name == "host";
}

inline bool proxy_drops_response_field(std::string_view name) {
    return is_hop_by_hop_field(name) || name == "content-length" || name == "transfer-encoding";
}

} // namespace detail

inline void run_http_proxy(RequestPtr req, ResponsePtr res, bool client_is_tls, HttpProxyTarget target,
                           ClientConfig client_cfg, HandlerCompletion done) {
    auto completion = std::make_shared<HandlerCompletion>(std::move(done));
    auto finish = [completion]() mutable {
        auto callback = std::move(*completion);
        std::move(callback)();
    };
    auto send_error = [req, res, target, completion, finish](int status, error_code ec) mutable {
        if (req->cancelled() || res->started()) {
            finish();
            return;
        }
        if (target.hooks.error_handler) {
            target.hooks.error_handler(res, *req, ec, [completion]() mutable {
                auto callback = std::move(*completion);
                std::move(callback)();
            });
            return;
        }
        res->status(status).content_type("text/plain");
        res->async_send(std::string{reason_phrase(status)}, [completion](error_code) mutable {
            auto callback = std::move(*completion);
            std::move(callback)();
        });
    };

    if (req->method() == Method::Connect) {
        SIMPLE_HTTP_ERROR_LOG("http-proxy: refusing CONNECT (not a tunnel proxy)");
        send_error(status::method_not_allowed, make_error_code(asio::error::operation_not_supported));
        return;
    }

    std::string forwarded_target = target.rewrite_path.empty() ? std::string{req->target()} : target.rewrite_path;
    if (!target.rewrite_path.empty() && forwarded_target.find('?') == std::string::npos && !req->query().empty()) {
        forwarded_target.push_back('?');
        forwarded_target.append(req->query());
    }
    if (contains_ctl(req->method_token()) || contains_ctl(forwarded_target)) {
        send_error(status::bad_request, make_error_code(asio::error::invalid_argument));
        return;
    }

    auto outbound = std::make_shared<Request>(Version::Http11, req->executor());
    outbound->set_method_token(std::string{req->method_token()});
    outbound->set_target(forwarded_target);
    // RFC 9110 §7.6.1: every field nominated by Connection is hop-by-hop too.
    const auto inbound_conn_tokens = connection_tokens(req->headers());
    for (const auto &[name, value] : req->headers()) {
        if (detail::proxy_drops_request_field(name) ||
            std::find(inbound_conn_tokens.begin(), inbound_conn_tokens.end(), name) != inbound_conn_tokens.end())
            continue;
        outbound->mutable_headers().add(name, value);
    }
    std::string xff;
    if (target.preserve_inbound_forwarding) {
        if (const auto inbound = req->header("x-forwarded-for"); inbound && !inbound->empty())
            xff.assign(*inbound);
    }
    const std::string peer = req->peer_address();
    xff = xff.empty() ? peer : xff + ", " + peer;
    outbound->mutable_headers().add_lower("x-forwarded-for", std::move(xff));
    outbound->mutable_headers().add_lower("x-forwarded-proto", client_is_tls ? "https" : "http");
    if (const auto host = req->header("host")) {
        outbound->mutable_headers().add_lower("x-forwarded-host", std::string{*host});
    }
    if (!target.host_header.empty()) {
        if (contains_ctl(target.host_header)) {
            send_error(status::bad_request, make_error_code(asio::error::invalid_argument));
            return;
        }
        outbound->mutable_headers().add_lower("host", target.host_header);
    } else if (target.preserve_host) {
        if (const auto host = req->header("host"))
            outbound->mutable_headers().add_lower("host", std::string{*host});
    } else {
        outbound->mutable_headers().add_lower(
            "host", target.port == 0 ? target.host : target.host + ":" + std::to_string(target.port));
    }
    if (target.hooks.rewrite) {
        try {
            target.hooks.rewrite(*req, *outbound);
        } catch (const std::exception &e) {
            SIMPLE_HTTP_ERROR_LOG("http-proxy: rewrite hook threw: {}", e.what());
        }
    }

    auto client = std::make_shared<Client>(req->executor(), std::move(client_cfg));
    const std::string url = std::string{target.tls ? "https://" : "http://"} + target.host + ":" +
                            std::to_string(target.port == 0 ? (target.tls ? 443 : 80) : target.port) + forwarded_target;
    const std::size_t cap = 64u * 1024 * 1024;

    auto forward = [client, url, cap, req, res, target, outbound, send_error, completion](std::string body) mutable {
        if (!body.empty())
            outbound->set_body(std::move(body));
        client->async_send(outbound, url, cap,
                           [req, res, target, send_error, completion](error_code ec, Response response) mutable {
                               if (ec) {
                                   SIMPLE_HTTP_ERROR_LOG("http-proxy: upstream {}:{} failed ({})", target.host,
                                                         target.port, ec.message());
                                   send_error(status::bad_gateway, ec);
                                   return;
                               }
                               int status = response.status();
                               Headers headers = response.headers();
                               if (target.hooks.modify_response) {
                                   if (error_code hook_ec = target.hooks.modify_response(status, headers, *req)) {
                                       send_error(status::bad_gateway, hook_ec);
                                       return;
                                   }
                               }
                               res->status(status);
                               const auto outbound_conn_tokens = connection_tokens(headers);
                               for (const auto &[name, value] : headers) {
                                   if (detail::proxy_drops_response_field(name) ||
                                       std::find(outbound_conn_tokens.begin(), outbound_conn_tokens.end(), name) !=
                                           outbound_conn_tokens.end())
                                       continue;
                                   res->header(name, value);
                               }
                               res->async_send(std::string{response.text()}, [completion](error_code) mutable {
                                   auto callback = std::move(*completion);
                                   std::move(callback)();
                               });
                           });
    };

    // Always read the body to its end. HTTP/2/3 requests need not carry a
    // content-length (END_STREAM delimits them), so inferring "no body" from
    // the absence of CL silently dropped the body of every such proxied
    // request. A bodyless request completes immediately (h1 None mode, h2/h3
    // END_STREAM on the headers).
    req->body().async_read_all([forward = std::move(forward), send_error](error_code ec, std::string body) mutable {
        if (ec) {
            send_error(status::bad_request, ec);
            return;
        }
        forward(std::move(body));
    });
}

} // namespace simple_http
