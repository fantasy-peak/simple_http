// WebSocket frame-parser fuzz target.
//
// Runs the same bytes through a masked (server-side) and an unmasked
// (client-side) parser, and drains complete frames.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "simple_http/proto/ws_frame.h"

namespace {

void drain(bool expect_masked, std::string_view bytes) {
    simple_http::WsFrameParser parser(1u << 20, expect_masked, /*allow_rsv1=*/true);
    parser.append(bytes);
    simple_http::WsFrame frame;
    for (int i = 0; i < 256; ++i) {
        const auto status = parser.next(frame);
        if (status != simple_http::WsFrameParser::Status::Frame) {
            break;
        }
        volatile auto size = frame.payload.size();
        volatile auto opcode = static_cast<unsigned>(frame.opcode);
        (void)size;
        (void)opcode;
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size > (1u << 20)) {
        return 0;
    }
    const std::string_view bytes{reinterpret_cast<const char *>(data), size};
    drain(/*expect_masked=*/true, bytes);
    drain(/*expect_masked=*/false, bytes);
    return 0;
}
