#pragma once

// simple_http: a header-only HTTP/1.x + HTTP/2 (+ optional HTTP/3, WebSocket)
// server and client, organized as a layered architecture under
// include/simple_http/:
//
//   core/       protocol-agnostic primitives (types, logging, io pool, base64)
//   proto/      HTTP request/response model + the ResponseSink abstraction
//   transport/  byte-stream transport abstraction (TCP plain/TLS, QUIC later)
//   h1/ h2/ h3/ per-version protocol engines, each implementing a ResponseSink
//   handler/    handler type system, router and dispatch
//   net/        listeners and connection protocol detection
//   server.h    the Server facade
//   client/     outbound HTTP client (h1/h2, plaintext/TLS, pooling, h2c)
//
// This umbrella header aggregates the public API. During the ongoing rewrite it
// grows layer by layer; see the task list for progress.

// --- core layer ---
#include "simple_http/core/version.h"
#include "simple_http/core/types.h"
#include "simple_http/core/http_field.h"
#include "simple_http/core/http_status.h"
#include "simple_http/core/logging.h"
#include "simple_http/core/mime.h"
#include "simple_http/core/base64.h"
#include "simple_http/core/compression.h"
#include "simple_http/core/content_encoding.h"  // codecs need SIMPLE_HTTP_ENABLE_COMPRESSION
#include "simple_http/core/io_pool.h"

// --- proto layer (version-agnostic HTTP request/response model) ---
#include "simple_http/proto/headers.h"
#include "simple_http/proto/body.h"
#include "simple_http/proto/request.h"
#include "simple_http/proto/response_writer.h"
#include "simple_http/proto/response.h"
#include "simple_http/proto/compressing_writer.h"  // opt-in via CompressionConfig::enabled

// --- transport layer (byte-stream abstraction over TCP plain/TLS; QUIC later) ---
#include "simple_http/transport/transport.h"
#include "simple_http/transport/tcp_transport.h"
#include "simple_http/transport/tls_context.h"
#include "simple_http/transport/tls_transport.h"

// --- engine layer (per-version protocol engines implementing ResponseWriter) ---
#include "simple_http/engine/h1/h1_engine.h"
#include "simple_http/engine/h2/h2_engine.h"
#include "simple_http/engine/h3/h3_engine.h"  // no-op unless SIMPLE_HTTP_ENABLE_HTTP3

// --- handler layer (handler type system + router/dispatch) ---
#include "simple_http/engine/dispatcher.h"
#include "simple_http/handler/handler.h"
#include "simple_http/handler/router.h"

// --- net layer (protocol detection + server facade) ---
#include "simple_http/net/connection.h"
#include "simple_http/net/server.h"

// --- client layer (outbound requests; HTTP/1.1 + HTTP/2 over TCP/TLS) ---
#include "simple_http/client/client.h"
