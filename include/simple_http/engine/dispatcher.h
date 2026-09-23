#pragma once

// Dispatcher: the callback an engine invokes once a request is ready.
//
// It is the seam between the engine layer (which produces Request/Response) and
// the handler layer (Router, which implements a Dispatcher). Defined here so the
// engines depend only on this narrow type, not on the whole handler layer.

#include <functional>
#include <memory>
#include <optional>
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

}  // namespace simple_http
