#pragma once

// Dispatcher: the callback an engine invokes once a request is ready.
//
// It is the seam between the engine layer (which produces Request/Response) and
// the handler layer (Router, which implements a Dispatcher). Defined here so the
// engines depend only on this narrow type, not on the whole handler layer.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <boost/asio/awaitable.hpp>

#include "../proto/request.h"
#include "../proto/response.h"
#include "../proto/websocket.h"
#include "../transport/transport.h"  // SslHandle

namespace simple_http {

namespace asio = boost::asio;

using Dispatcher =
    std::function<asio::awaitable<void>(std::shared_ptr<Request>, std::shared_ptr<Response>, SslHandle)>;

// The WebSocket handler shape (mirrors handler layer's WsHandler).
using WsHandlerFn = std::function<asio::awaitable<void>(std::shared_ptr<Request>, std::shared_ptr<WebSocket>)>;

// Looks up a WebSocket handler for a request path; empty if none matches. The
// engine calls this after a websocket upgrade is requested.
using WsLookup = std::function<std::optional<WsHandlerFn>(std::string_view path)>;

// A backend a WebSocket upgrade should be transparently proxied to (byte-level
// pass-through reverse proxy). host/port name a plain TCP endpoint; the engine
// opens a TCP connection, replays the client's raw upgrade request to it, then
// copies bytes both ways verbatim (frames, fragmentation, masking and control
// frames all pass through untouched).
struct WsProxyTarget {
    std::string host;
    std::uint16_t port{0};
    // Optional request-target rewrite template. When non-empty, the upgrade
    // request line sent to the backend uses this instead of the client's
    // original target. For a regex proxy route it is a substitution template
    // that may reference capture groups: $0 = whole match, $1..$9 = groups,
    // $$ = a literal '$'. For an exact route (no groups) it is used verbatim.
    // Everything after the handshake is still spliced byte-for-byte.
    std::string rewrite_path;
};

// Looks up a proxy backend for a request path; empty if the path is not a proxy
// route. The engine consults this before the in-process ws handler lookup, so a
// path registered for proxying takes precedence over a local ws handler.
using WsProxyLookup = std::function<std::optional<WsProxyTarget>(std::string_view path)>;

// A backend a plain HTTP request should be reverse-proxied to (request-level
// proxy). Unlike WsProxyTarget this is per-request: for each matching request
// the proxy opens a TCP connection to host:port, forwards the request (with the
// standard X-Forwarded-* headers added and hop-by-hop headers stripped), then
// streams the backend's response back to the client. rewrite_path works like
// WsProxyTarget's: a substitution template ($0/$1..$9/$$) for regex routes, or
// a verbatim replacement for exact routes; empty keeps the original target.
struct HttpProxyTarget {
    std::string host;
    std::uint16_t port{0};
    std::string rewrite_path;
};

// Looks up an HTTP proxy backend for a request path; empty if the path is not a
// proxy route. The Router consults this in dispatch before matching a local
// handler, so a proxied path takes precedence.
using HttpProxyLookup = std::function<std::optional<HttpProxyTarget>(std::string_view path)>;

}  // namespace simple_http
