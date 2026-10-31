#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "simple_http/proto/multipart_stream.h"

namespace {

struct Collected {
    struct Part {
        std::string name;
        std::string filename;
        std::string data;
    };
    std::vector<Part> parts;

    simple_http::MultipartStreamCallbacks callbacks() {
        simple_http::MultipartStreamCallbacks cb;
        cb.on_part_begin = [this](const simple_http::MultipartPart &p) {
            parts.push_back({p.name, p.filename, {}});
            return true;
        };
        cb.on_part_data = [this](std::string_view d) {
            parts.back().data.append(d);
            return true;
        };
        cb.on_part_end = [] {};
        return cb;
    }
};

const std::string kBody = "--X\r\n"
                          "Content-Disposition: form-data; name=\"title\"\r\n"
                          "\r\n"
                          "hello\r\n"
                          "--X\r\n"
                          "Content-Disposition: form-data; name=\"upload\"; filename=\"a.txt\"\r\n"
                          "Content-Type: text/plain\r\n"
                          "\r\n"
                          "file-body\r\n"
                          "--X--\r\n";

} // namespace

TEST_CASE("multipart_stream: parts are delivered incrementally", "[proto][multipart]") {
    Collected out;
    auto cb = out.callbacks();
    simple_http::MultipartStreamParser parser("X");
    REQUIRE(parser.feed(kBody, cb));
    REQUIRE(parser.finish(cb));

    REQUIRE(out.parts.size() == 2);
    CHECK(out.parts[0].name == "title");
    CHECK(out.parts[0].filename.empty());
    CHECK(out.parts[0].data == "hello");
    CHECK(out.parts[1].name == "upload");
    CHECK(out.parts[1].filename == "a.txt");
    CHECK(out.parts[1].data == "file-body");
}

TEST_CASE("multipart_stream: byte-at-a-time boundaries are handled", "[proto][multipart]") {
    Collected out;
    auto cb = out.callbacks();
    simple_http::MultipartStreamParser parser("X");
    for (char c : kBody) {
        REQUIRE(parser.feed(std::string_view{&c, 1}, cb));
    }
    REQUIRE(parser.finish(cb));

    REQUIRE(out.parts.size() == 2);
    CHECK(out.parts[0].data == "hello");
    CHECK(out.parts[1].data == "file-body");
}

TEST_CASE("multipart_stream: a body split at every boundary byte", "[proto][multipart]") {
    Collected out;
    auto cb = out.callbacks();
    simple_http::MultipartStreamParser parser("X");
    // Split around the first boundary and the CRLF that precedes it.
    const std::size_t cut = kBody.find("\r\n--X\r\n");
    REQUIRE(cut != std::string::npos);
    for (std::size_t i = 0; i < kBody.size(); ++i) {
        const std::size_t n = (i == cut - 1 || i == cut + 2) ? 2 : 1;
        REQUIRE(parser.feed(std::string_view{kBody}.substr(i, n), cb));
        i += n - 1;
    }
    REQUIRE(parser.finish(cb));
    REQUIRE(out.parts.size() == 2);
    CHECK(out.parts[0].data == "hello");
    CHECK(out.parts[1].data == "file-body");
}

TEST_CASE("multipart_stream: a truncated body fails finish", "[proto][multipart]") {
    Collected out;
    auto cb = out.callbacks();
    simple_http::MultipartStreamParser parser("X");
    const std::string truncated = kBody.substr(0, kBody.size() - 6);
    (void)parser.feed(truncated, cb);
    CHECK_FALSE(parser.finish(cb));
}

TEST_CASE("multipart_stream: max_part_bytes aborts an oversized part", "[proto][multipart]") {
    Collected out;
    auto cb = out.callbacks();
    simple_http::MultipartStreamParser parser("X", /*max_part_bytes=*/4);
    CHECK_FALSE(parser.feed(kBody, cb));
}
