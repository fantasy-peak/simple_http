// Incremental multipart/form-data parser fuzz target.
//
// The first line of the input is used as the boundary; the rest is the body.
// The body is fed in several chunks so boundary/header splits across reads are
// exercised.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "simple_http/proto/multipart_stream.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size > (1u << 20)) {
        return 0;
    }
    const std::string_view bytes{reinterpret_cast<const char *>(data), size};
    const auto newline = bytes.find('\n');
    std::string boundary{bytes.substr(0, newline)};
    if (boundary.size() > 200) {
        boundary.resize(200);
    }
    const std::string_view body = newline == std::string_view::npos ? std::string_view{} : bytes.substr(newline + 1);

    simple_http::MultipartStreamParser parser(boundary, /*max_part_bytes=*/1u << 20, /*max_parts=*/100);
    std::size_t parts = 0;
    std::size_t bytes_seen = 0;
    simple_http::MultipartStreamCallbacks callbacks;
    callbacks.on_part_begin = [&](const simple_http::MultipartPart &part) {
        volatile auto name = part.name.size();
        volatile auto filename = part.filename.size();
        volatile auto content_type = part.content_type.size();
        (void)name;
        (void)filename;
        (void)content_type;
        return ++parts <= 100;
    };
    callbacks.on_part_data = [&](std::string_view chunk) {
        bytes_seen += chunk.size();
        return bytes_seen <= (1u << 20);
    };
    callbacks.on_part_end = [] {};

    const std::size_t chunk_size = body.empty() ? 0 : std::max<std::size_t>(1, body.size() / 3);
    for (std::size_t pos = 0; pos < body.size(); pos += chunk_size) {
        const auto chunk = body.substr(pos, chunk_size);
        if (!parser.feed(chunk, callbacks)) {
            break;
        }
    }
    (void)parser.finish(callbacks);
    return 0;
}
