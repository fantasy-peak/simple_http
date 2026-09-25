#pragma once

// The client layer, aggregated.
//
// client/ is the outbound mirror of the server side, sharing its codecs instead
// of duplicating them (engine/h1/h1_parser.h parses response heads too,
// engine/h2/* does frames and HPACK for both roles, transport/* carries either).
// What is client-specific lives here:
//
//   client_config.h  where to connect, how, and the client_errc error codes
//   url.h            absolute-URL parsing
//   client_stream.h  RequestSpec / ResponseHead / ClientStream / ClientSession
//   tls_client.h     the client ssl::context and handshake (SNI, ALPN, mTLS)
//   h1_client.h      HTTP/1.1 session (one exchange at a time, keep-alive)
//   h2_client.h      HTTP/2 session (multiplexed streams, flow control, h2c)
//   client_pool.h    idle keep-alive pool, one per HttpClient
//   http_client.h    the HttpClient facade (convenience + session levels)
//
// Typical use:
//
//   simple_http::HttpClient http;                        // default policy
//   auto r = co_await http.get("https://example.com/");  // h2 via ALPN, or h1
//
//   simple_http::ClientTarget target;
//   target.host = "10.0.0.5"; target.port = 8080; target.use_tls = true;
//   auto session = co_await http.connect(target);        // expected<…>
//   auto stream  = co_await (*session)->open_stream({.method = simple_http::Method::Post,
//                                                   .target = "/upload",
//                                                   .stream_body = true});
//   co_await (*stream)->write(chunk);        // …and (*stream)->finish(last)

#include "client_config.h"
#include "url.h"
#include "client_stream.h"
#include "client_pool.h"
#include "tls_client.h"
#include "h1_client.h"
#include "h2_client.h"
#include "http_client.h"
