#pragma once

// The SSL half of the Asio seam. Kept apart from asio_compat.h so headers that
// never touch TLS do not pull in OpenSSL.

#include "asio_compat.h"

#if defined(SIMPLE_HTTP_USE_STANDALONE_ASIO)
#include <asio/ssl.hpp>
#else
#include <boost/asio/ssl.hpp>
#endif
