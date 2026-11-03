// HTTP/2 frame-header and HPACK decoder fuzz target.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "simple_http/engine/h2/h2_frame.h"
#include "simple_http/engine/h2/hpack_decode.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size > (1u << 20)) {
        return 0;
    }
    const std::string_view bytes{reinterpret_cast<const char *>(data), size};

    simple_http::codec::H2FrameHeader header;
    if (simple_http::codec::parse_frame_header(bytes, header)) {
        volatile std::uint32_t sink = header.length + header.type + header.flags + header.stream_id;
        (void)sink;
        if (bytes.size() >= simple_http::codec::kH2FrameHeaderSize + header.length) {
            const auto payload = bytes.substr(simple_http::codec::kH2FrameHeaderSize, header.length);
            volatile auto n = payload.size();
            (void)n;
        }
    }

    simple_http::codec::HpackDecoder decoder;
    std::vector<simple_http::codec::HpackHeader> out;
    (void)decoder.decode(bytes, out);
    if (size >= 2) {
        // A second block on the same decoder exercises the dynamic table.
        std::vector<simple_http::codec::HpackHeader> more;
        (void)decoder.decode(bytes.substr(size / 2), more);
    }
    return 0;
}
