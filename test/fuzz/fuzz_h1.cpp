// H1 request-head parser fuzz target.
//
// Feeds arbitrary bytes both in one block and byte-by-byte (to exercise the
// NeedMore/resume path), and repeatedly drains complete pipelined heads.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "simple_http/engine/h1/h1_parser.h"

namespace {

void touch(const simple_http::ParsedHead &head) {
    volatile std::size_t sink = head.method_token.size() + head.target.size() + head.authority.size();
    for (const auto &[name, value] : head.headers) {
        sink += name.size() + value.size();
    }
    (void)sink;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size > (1u << 20)) {
        return 0;
    }

    // Whole-block parse, then every complete pipelined head.
    simple_http::H1Parser parser;
    parser.feed(std::string_view{reinterpret_cast<const char *>(data), size});
    for (int i = 0; i < 64; ++i) {
        const auto state = parser.parse_head();
        if (state != simple_http::H1Parser::State::Done) {
            break;
        }
        touch(parser.head());
        const auto consumed = parser.consumed();
        parser.reset_after_head();
        if (consumed == 0) {
            break;
        }
    }

    // Byte-at-a-time parse: exercises NeedMore and the rescan cursor.
    simple_http::H1Parser incremental;
    for (std::size_t i = 0; i < size && i < 4096; ++i) {
        incremental.feed(std::string_view{reinterpret_cast<const char *>(data + i), 1});
        const auto state = incremental.parse_head();
        if (state == simple_http::H1Parser::State::Done) {
            touch(incremental.head());
            incremental.reset_after_head();
        } else if (state == simple_http::H1Parser::State::Error) {
            break;
        }
    }
    return 0;
}
