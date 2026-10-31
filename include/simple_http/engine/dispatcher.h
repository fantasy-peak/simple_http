#pragma once

// Dispatcher: the callback an engine invokes once a request is ready.
//
// It is the seam between the engine layer (which produces Request/Response) and
// the handler layer (Router, which implements a Dispatcher). Defined here so
// the engines depend only on this narrow type, not on the whole handler layer.

#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "../core/resolve.h" // ResolveFn (optional external DNS)
#include "../proto/request.h"
#include "../proto/response.h"
#include "../proto/websocket.h"
#include "../transport/transport.h" // SslHandle

namespace simple_http {

namespace asio = boost::asio;

using Dispatcher =
    std::function<asio::awaitable<void>(std::shared_ptr<Request>, std::shared_ptr<ResponseWriter>, SslHandle)>;

// The raw WebSocket route handler — the shape a ws_route() registration has.
// (handler.h spells the identical alias as `WsHandler`; both are the same
// function type, and this header stands alone per the layering rule, so the
// alias is repeated here and converting between the two spellings is free.)
using WsHandler = std::function<asio::awaitable<void>(std::shared_ptr<Request>, std::shared_ptr<WebSocket>)>;

// The engine's terminal step of a WebSocket upgrade: called by the handler
// layer once its middleware chain has reached it (i.e. no middleware
// short-circuited the handshake). It negotiates extensions, writes the 101
// handshake (folding in any fields middleware set on `res`), builds the
// WebSocket, runs `handler`, and closes the connection. Returns true when the
// connection was upgraded.
using WsUpgrade =
    std::function<asio::awaitable<bool>(std::shared_ptr<Request>, std::shared_ptr<ResponseWriter>, WsHandler handler)>;

// The WebSocket handler shape (mirrors handler layer's WsHandler). Rather than
// running the raw ws handler directly, the handler layer runs it through the
// middleware chain: global + group + per-route middleware (the same ones as
// HTTP routes), ending at the engine's WsUpgrade terminal. Returns true if the
// connection was upgraded; a middleware that short-circuits answers the
// handshake with a plain HTTP response via `res` and returns false. Before this
// seam, WebSocket upgrades bypassed the middleware chain entirely.
using WsHandlerFn = std::function<asio::awaitable<bool>(std::shared_ptr<Request>, std::shared_ptr<ResponseWriter>,
                                                        SslHandle, WsUpgrade up)>;

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

    // Host policy for the backend handshake. preserve_host (the default) keeps
    // the frontend's Host, which is what Go's NewSingleHostReverseProxy does
    // and what backends validating Origin against Host expect. host_header is
    // an explicit override and wins over preserve_host.
    bool preserve_host{true};
    std::string host_header;

    // Go Rewrite mode strips inbound Forwarded/X-Forwarded-* and authors the
    // three standard fields. Set this to append the inbound X-Forwarded-For
    // chain instead of replacing it with the real peer.
    bool preserve_inbound_forwarding{false};

    // Budget for establishing the backend leg: name resolution, TCP connect, and
    // the Upgrade handshake (a backend that accepts and then never answers is
    // bounded by this, not left to the idle watchdog). 0 disables the timer.
    std::chrono::milliseconds connect_timeout{10000};

    // Optional name-resolution override for the backend leg. Empty = asio's
    // system resolver. Same contract as ClientConfig::resolve: return the
    // endpoints to try, in order, from the connection executor.
    ResolveFn resolve;

    // Disable Nagle's algorithm on the backend leg, with the same default and
    // meaning as ClientConfig::tcp_nodelay (which the HTTP proxy route uses).
    // The byte tunnel is latency-sensitive and writes small frames, so leaving
    // it on would add up to a delayed-ACK worth of latency per message.
    bool tcp_nodelay{true};
};

// Looks up a proxy backend for a request path; empty if the path is not a proxy
// route. The h1 engine consults this between the local exact and the local
// regex ws handler (nginx order): a proxied path beats a local regex ws
// handler, but loses to a local exact one.
using WsProxyLookup = std::function<std::optional<WsProxyTarget>(std::string_view path)>;

// Go httputil.ReverseProxy-shaped policy hooks. They run on the connection
// executor (model A), so they may touch the outbound Request/Headers directly
// without locking.
struct HttpProxyHooks {
    // Rewrite: after the standard outbound request has been built (hop-by-hop
    // fields removed, forwarding fields stripped), before it is sent. `out` is
    // a client-side Request: it may change the target, method, headers or body
    // framing. This is Go's ReverseProxy.Rewrite / ProxyRequest.Out.
    std::function<void(const Request &in, Request &out)> rewrite;

    // ModifyResponse: after the backend response head arrived and hop-by-hop
    // fields were removed, before it is relayed. May change the status and the
    // header section (Location, Set-Cookie, extra fields, …). Returning an
    // error invokes the error handler and aborts the exchange. This is Go's
    // ReverseProxy.ModifyResponse.
    std::function<error_code(int &status, Headers &headers, const Request &in)> modify_response;

    // ErrorHandler: an upstream exchange failed before the response started.
    // It may send its own response through `res`; if it returns without
    // starting one, the proxy falls back to 502. Mid-body failures are not
    // routed here: once bytes are flowing the only honest action is aborting
    // the frontend response (Go's ErrAbortHandler behaviour).
    std::function<asio::awaitable<void>(std::shared_ptr<ResponseWriter>, const Request &, error_code)> error_handler;
};

// A backend a plain HTTP request should be reverse-proxied to (request-level
// proxy). Unlike WsProxyTarget this is per-request: for each matching request
// the proxy takes an upstream connection to host:port (a fresh client each
// time), forwards the request (with the standard X-Forwarded-* headers added
// and hop-by-hop headers stripped), then streams the backend's response back to
// the client. rewrite_path works like WsProxyTarget's: a substitution template
// ($0/$1..$9/$$) for regex routes, or a verbatim replacement for exact routes;
// empty keeps the original target.
struct HttpProxyTarget {
    std::string host;
    std::uint16_t port{0};
    std::string rewrite_path;
    // Backend over TLS. ALPN then decides h2 vs HTTP/1.1, so an https backend
    // gets HTTP/2 for free when it offers it.
    bool tls{false};
    // Plaintext backend that speaks HTTP/2: the first request on a connection
    // carries `Upgrade: h2c` (one extra round trip); a backend that ignores it
    // answers over HTTP/1.1 as usual. Ignored when `tls` is set.
    bool h2c{false};
    // Prior-knowledge h2c: send the HTTP/2 preface directly instead of the
    // Upgrade round trip. Only for backends known to speak h2c.
    bool h2c_prior_knowledge{false};

    // Host policy. By default the client layer writes the backend's authority
    // (nginx's proxy_set_header Host $proxy_host). preserve_host keeps the
    // frontend's Host (Go's NewSingleHostReverseProxy default); host_header is
    // an explicit override and wins over preserve_host.
    bool preserve_host{false};
    std::string host_header;

    // Go's Rewrite mode strips client-supplied forwarding headers by default
    // (Forwarded and X-Forwarded-*). Set this to append the inbound
    // X-Forwarded-For chain instead of replacing it with the real peer; the
    // other X-Forwarded-* fields are always authored by the proxy.
    bool preserve_inbound_forwarding{false};

    // Upper bound on attempts for a request that is safe to replay (no body and
    // an idempotent method). 0/1 disables the proxy's own retry; the client
    // layer's RetryPolicy still applies independently.
    std::size_t max_attempts{3};

    HttpProxyHooks hooks;

    // Let the upstream Client honor HTTP_PROXY / HTTPS_PROXY / ALL_PROXY. Off by
    // default: a server's ambient proxy environment must not reroute its
    // backends (usually loopback or private addresses). A ClientConfig::proxy
    // set on the route applies regardless of this flag.
    bool use_env_proxy{false};
};

// Looks up an HTTP proxy backend for a request path; empty if the path is not a
// proxy route. The Router consults this in dispatch after the exact local match
// misses, so a proxied path beats a local regex handler but loses to an exact
// one (nginx order).
using HttpProxyLookup = std::function<std::optional<HttpProxyTarget>(std::string_view path)>;

} // namespace simple_http
