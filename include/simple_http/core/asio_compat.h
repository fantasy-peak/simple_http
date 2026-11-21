#pragma once

// Asio source selection.
//
// The library is written against Asio's initiating-function model, which
// Boost.Asio and standalone Asio share; only the top-level namespace and the
// include paths differ. Define SIMPLE_HTTP_USE_STANDALONE_ASIO to build
// against https://think-async.com/Asio/ instead of Boost.Asio:
//
//   * every library header names Asio types as simple_http::asio::...;
//   * the error type follows the source: core/types.h uses std::error_code
//     for standalone Asio and boost::system::error_code for Boost.Asio;
//   * SIMPLE_HTTP_HAS_CO_AWAIT mirrors ASIO_HAS_CO_AWAIT /
//     BOOST_ASIO_HAS_CO_AWAIT, so public headers do not have to test a
//     Boost-specific macro.
//
// The SSL half lives in asio_ssl.h so that headers which never touch TLS do
// not pull in OpenSSL.

#if defined(SIMPLE_HTTP_USE_STANDALONE_ASIO)

#include <asio.hpp>
#include <asio/any_completion_handler.hpp>
#include <asio/any_io_executor.hpp>
#include <asio/as_tuple.hpp>
#include <asio/associated_cancellation_slot.hpp>
#include <asio/async_result.hpp>
#include <asio/awaitable.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancel_after.hpp>
#include <asio/cancel_at.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/cancellation_type.hpp>
#include <asio/co_spawn.hpp>
#include <asio/compose.hpp>
#include <asio/dispatch.hpp>
#include <asio/error.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/experimental/channel_error.hpp>
#include <asio/experimental/concurrent_channel.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>

namespace simple_http {
namespace asio = ::asio;
} // namespace simple_http

#if defined(ASIO_HAS_CO_AWAIT)
#define SIMPLE_HTTP_HAS_CO_AWAIT 1
#endif

#if defined(ASIO_HAS_LOCAL_SOCKETS)
#define SIMPLE_HTTP_HAS_LOCAL_SOCKETS 1
#endif

#else

#include <boost/asio.hpp>
#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancel_after.hpp>
#include <boost/asio/cancel_at.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/compose.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/experimental/channel_error.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

namespace simple_http {
namespace asio = boost::asio;
} // namespace simple_http

#if defined(BOOST_ASIO_HAS_CO_AWAIT)
#define SIMPLE_HTTP_HAS_CO_AWAIT 1
#endif

#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
#define SIMPLE_HTTP_HAS_LOCAL_SOCKETS 1
#endif

#endif
