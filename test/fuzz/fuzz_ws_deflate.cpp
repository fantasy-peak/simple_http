// WebSocket permessage-deflate decoder fuzz target.
//
// Feeds arbitrary bytes to WsInflater, split across two fragments and then
// finished with and without a context reset. Reaches zlib error handling, the
// zip-bomb budget in pump(), and the reset/stream-reuse path — the frame-codec
// target only parses framing and never inflates.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "simple_http/proto/ws_deflate.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size > (1u << 20)) {
        return 0;
    }

    simple_http::WsInflater inflater{15};
    if (!inflater.ok()) {
        return 0;
    }

    const std::string_view input{reinterpret_cast<const char *>(data), size};
    // An arbitrary fragment boundary exercises the "carry the inflate state
    // across frames" path.
    const std::size_t split = size == 0 ? 0 : static_cast<std::size_t>(data[0]) % (size + 1);
    constexpr std::size_t kBudget = 64 * 1024;

    std::string out;
    (void)inflater.feed(input.substr(0, split), out, kBudget);
    (void)inflater.feed(input.substr(split), out, kBudget);
    (void)inflater.finish(out, kBudget, /*reset=*/true);

    // A second message after the reset: the decoder must accept a fresh stream
    // instead of staying in the previous context.
    out.clear();
    (void)inflater.feed(input, out, kBudget);
    (void)inflater.finish(out, kBudget, /*reset=*/false);
    return 0;
}
