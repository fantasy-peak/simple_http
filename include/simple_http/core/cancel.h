#pragma once

// Cancellation helpers shared by the server and client.
//
// The library follows Asio's per-operation cancellation contract: a pending
// operation that observes cancellation completes with operation_aborted.
// boost::asio::experimental::channel reports its own channel_cancelled instead,
// so composed operations normalize it at the completion boundary rather than
// leaking a second cancellation vocabulary to callers.

#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/experimental/channel_error.hpp>

#include "types.h"

namespace simple_http {

namespace asio = boost::asio;

// Request::cancel() and asio::cancel_after() both emit terminal by default.
inline bool is_terminal_cancel(asio::cancellation_type_t type) noexcept {
    return (type & asio::cancellation_type::terminal) != asio::cancellation_type::none;
}

// Map the channel's per-operation cancellation code onto Asio's conventional
// operation_aborted. Every other code (connection_reset, eof, ...) is preserved.
inline error_code normalize_cancellation(error_code ec) noexcept {
    if (ec == asio::experimental::error::channel_cancelled) {
        return make_error_code(asio::error::operation_aborted);
    }
    return ec;
}

// For code that cannot normalize at the boundary: both codes mean "cancelled".
inline bool is_cancellation_error(const error_code &ec) noexcept {
    return ec == asio::error::operation_aborted || ec == asio::experimental::error::channel_cancelled;
}

} // namespace simple_http
