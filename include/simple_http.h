#pragma once

// simple_http (C++17 token-only port).
//
// This is the parallel implementation of the repository's C++23 coroutine
// library. The public surface mirrors include/simple_http.h, but every
// asynchronous operation is an Asio initiating function taking an explicit
// completion token: callbacks, asio::deferred and asio::detached always work,
// and asio::use_awaitable remains available to C++20 callers.
//
// The port covers the core/proto/transport layers, the h1 and h2 engines, the
// handler/framework layer (middleware, sessions, JWT/JWKS, extractors), static
// files, file responses/uploads, the HTTP and WebSocket reverse proxies, the
// h1/h2 client (including streaming uploads, SSE and decompression) and outbound
// proxies. HTTP/3/QUIC is not part of this port.
//
// Deliberate differences from the C++23 implementation:
//   * no coroutine keywords anywhere except the feature-guarded adapter in
//     engine/dispatcher.h;
//   * glaze-backed helpers (JSON reflection, typed params, extractors,
//     OpenAPI, JWT/JWKS) are feature-gated: a plain C++17 translation unit
//     does not see them; compiling this include root as C++23 turns them on.
//     See core/glaze_support.h;
//   * synchronous value-or-error APIs return boost::system::result<T, error_code>.

// --- core layer ---
#include "simple_http/core/accept_encoding.h"
#include "simple_http/core/address.h"
#include "simple_http/core/base64.h"
#include "simple_http/core/compat.h"
#include "simple_http/core/compression.h"
#include "simple_http/core/content_encoding.h"
#include "simple_http/core/format.h"
#include "simple_http/core/glaze_support.h"
#include "simple_http/core/http_date.h"
#include "simple_http/core/http_field.h"
#include "simple_http/core/http_method.h"
#include "simple_http/core/http_status.h"
#include "simple_http/core/io_pool.h"
#include "simple_http/core/limits.h"
#include "simple_http/core/logging.h"
#include "simple_http/core/mime.h"
#include "simple_http/core/resolve.h"
#include "simple_http/core/result.h"
#include "simple_http/core/source_location.h"
#include "simple_http/core/static_table.h"
#include "simple_http/core/tls_version.h"
#include "simple_http/core/types.h"
#include "simple_http/core/url_encoding.h"
#include "simple_http/core/url_path.h"
#include "simple_http/core/validators.h"
#include "simple_http/core/version.h"

// --- proto layer (version-agnostic HTTP model; token-based I/O) ---
#include "simple_http/proto/body.h"
#include "simple_http/proto/compressing_writer.h"
#include "simple_http/proto/error_body.h"
#include "simple_http/proto/form.h"
#include "simple_http/proto/headers.h"
#include "simple_http/proto/json.h"
#include "simple_http/proto/multipart.h"
#include "simple_http/proto/multipart_stream.h"
#include "simple_http/proto/params.h"
#include "simple_http/proto/query.h"
#include "simple_http/proto/request.h"
#include "simple_http/proto/response.h"
#include "simple_http/proto/response_writer.h"
#include "simple_http/proto/websocket.h"
#include "simple_http/proto/ws_deflate.h"
#include "simple_http/proto/ws_frame.h"

// --- transport layer (byte stream over TCP/TLS; token-based I/O) ---
#include "simple_http/transport/tcp_transport.h"
#include "simple_http/transport/tls_context.h"
#include "simple_http/transport/tls_transport.h"
#include "simple_http/transport/transport.h"

// --- engine/handler seam (handler auto-adaptation lives in dispatcher.h) ---
#include "simple_http/engine/dispatcher.h"
#include "simple_http/engine/h1/ws_proxy.h"
#include "simple_http/engine/ws_proxy_common.h"
#include "simple_http/handler/builtin_middleware.h"
#include "simple_http/handler/conditionals.h"
#include "simple_http/handler/cors.h"
#include "simple_http/handler/csrf.h"
#include "simple_http/handler/extract.h"
#include "simple_http/handler/file_response.h"
#include "simple_http/handler/handler.h"
#include "simple_http/handler/http_proxy.h"
#include "simple_http/handler/jwks.h"
#include "simple_http/handler/jwt.h"
#include "simple_http/handler/rate_limit.h"
#include "simple_http/handler/router.h"
#include "simple_http/handler/secure_headers.h"
#include "simple_http/handler/session.h"
#include "simple_http/handler/upload.h"
#include "simple_http/handler/validation.h"

// --- OpenAPI document model + glaze-backed schema generation (gated) ---
#include "simple_http/openapi/openapi.h"
#include "simple_http/openapi/openapi_doc.h"

// --- net layer (HTTP/1.x + HTTP/2 listeners) ---
#include "simple_http/net/connection.h"
#include "simple_http/net/server.h"

// --- client layer (HTTP/1.1 + HTTP/2, streaming bodies, outbound proxies) ---
#include "simple_http/client/client_config.h"
#include "simple_http/client/client_stream.h"
#include "simple_http/client/cookie_jar.h"
#include "simple_http/client/decompressing_stream.h"
#include "simple_http/client/h1_client.h"
#include "simple_http/client/h2_client.h"
#include "simple_http/client/http.h"
#include "simple_http/client/multipart_stream.h"
#include "simple_http/client/multiplexed_session.h"
#include "simple_http/client/proxy.h"
#include "simple_http/client/sse.h"
#include "simple_http/client/tls_client.h"
#include "simple_http/client/url.h"
#include "simple_http/client/ws_client.h"
