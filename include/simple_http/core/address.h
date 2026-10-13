#pragma once

// IP address presentation helpers.
//
// An IPv4 peer on a dual-stack listener (bound to `::` with v6-only off) is
// reported by the OS as an IPv4-mapped IPv6 address (::ffff:a.b.c.d). Raw
// `to_string()` on that (asio's getpeername view, also what Node exposes) makes
// logs, rate-limit keys and per-IP stats treat one peer in two spellings. These
// helpers present such an address as the IPv4 it is -- the form Go's
// `net.IP.String()` and nginx produce. They are presentation-only: nothing here
// rewrites a stored endpoint.
//
// Boost 1.92 removed `address_v6::to_v4()`; the v4-mapped conversion is the
// free `make_address_v4(v4_mapped, v6)`, valid only for mapped addresses (we
// guard with `is_v4_mapped()` first). Fully qualified `boost::asio::` on purpose:
// the `asio` alias lives in each consumer's scope, not here.

#include <boost/asio.hpp>
#include <string>

namespace simple_http {

inline std::string address_string(const boost::asio::ip::address &a) {
    if (a.is_v6() && a.to_v6().is_v4_mapped())
        return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, a.to_v6()).to_string();
    return a.to_string();
}

} // namespace simple_http