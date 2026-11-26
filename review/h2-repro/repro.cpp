// Minimal, self-contained repros for two h2 server findings (see
// REVIEW-h2-server-2026-11-26.md).
//
//   P1: a malformed HPACK block that also crosses SETTINGS_MAX_HEADER_LIST_SIZE
//       is discarded (on a refused / already-closed stream) without a
//       connection-level COMPRESSION_ERROR, so the connection keeps running
//       even though the block was not decoded to the end.
//
//   P2: after the connection has begun (or finished) teardown, a successful
//       read completion is still parsed, and a new stream's handler is
//       dispatched on the dead connection.
//
// Build:  ./build.sh   (see build.sh for the xmake package include/link paths)
// Run:    ./repro p1 | ./repro p2
//
// Exit code 0 = the bug is NOT present (desired), 1 = the bug reproduced.

#include <simple_http/engine/h2/h2_engine.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace simple_http;
using namespace simple_http::codec;

namespace {

std::string frame(H2FrameType type, std::uint8_t flags, std::uint32_t stream_id, std::string_view payload) {
    std::string out;
    serialize_frame_header(out, static_cast<std::uint32_t>(payload.size()), static_cast<std::uint8_t>(type), flags,
                           stream_id);
    out.append(payload);
    return out;
}

struct Found {
    bool present = false;
    std::uint32_t error = 0;
};

// Scans the recorded output for the first frame of `type` on `stream_id`.
Found find_frame(std::string_view data, H2FrameType type, std::uint32_t stream_id) {
    for (std::size_t i = 0; i + kH2FrameHeaderSize <= data.size();) {
        H2FrameHeader hdr;
        parse_frame_header(data.substr(i), hdr);
        const std::size_t end = i + kH2FrameHeaderSize + hdr.length;
        if (end > data.size())
            break;
        if (hdr.type == static_cast<std::uint8_t>(type) && hdr.stream_id == stream_id) {
            Found f;
            f.present = true;
            if (hdr.length >= 4)
                f.error = read_u32(data, i + kH2FrameHeaderSize + (hdr.length == 8 ? 4 : 0));
            return f;
        }
        i = end;
    }
    return {};
}

// A request head that the engine accepts: :method GET, :scheme https,
// :path "/", :authority "example.com".
std::string valid_head_block() {
    std::string block;
    hpack_append_indexed(block, 2); // :method: GET
    hpack_append_indexed(block, 7); // :scheme: https
    hpack_append_literal(block, ":path", "/");
    hpack_append_literal(block, ":authority", "example.com");
    return block;
}

// Decodes to a field list larger than the 64 KiB default cap, inserts one
// dynamic-table entry, then (optionally) ends with a malformed representation
// (indexed header field with index 0).
std::string bomb_then_malformed_block(std::size_t fields = 1500, bool malformed = true) {
    std::string block;
    // A literal with incremental indexing, so a partial decode mutates the
    // dynamic table.
    block.push_back('\x40');
    hpack_append_string(block, "x-dyn");
    hpack_append_string(block, "v");
    // 12 ("content-type") + 1 + 32 = 45 bytes per field; 1500 fields = 67.5 KiB.
    for (std::size_t i = 0; i < fields; ++i)
        hpack_append_literal_indexed_name(block, 31, "a");
    if (malformed)
        block.push_back('\x80'); // 6.1 indexed field, index 0 -> invalid
    return block;
}

// ---------------------------------------------------------------------------
// Transport double: records writes, and lets the test decide when (and with
// what bytes) the pending read completes. Optionally fails every write.
// ---------------------------------------------------------------------------
class TestTransport {
  public:
    using Executor = asio::any_io_executor;
    using ReadHandler = asio::any_completion_handler<void(error_code, std::size_t)>;

    TestTransport(Executor ex, bool fail_writes) : m_ex(std::move(ex)), m_fail_writes(fail_writes) {}

    Executor get_executor() { return m_ex; }
    asio::ip::tcp::endpoint peer() const { return {}; }
    SslHandle tls_handle() const { return std::nullopt; }
    void close() { m_closed = true; }

    template <typename Token> auto async_read_some(ByteSpan buffer, Token &&token) {
        auto initiation = [this, buffer](auto handler) mutable {
            m_read_buf = buffer.data();
            m_read_cap = buffer.size();
            m_read_handler.emplace(std::move(handler));
        };
        return asio::async_initiate<Token, void(error_code, std::size_t)>(
            simple_http::detail::make_executor_initiation(std::move(initiation), m_ex), token);
    }

    template <typename Token> auto async_read(ByteSpan buffer, Token &&token) {
        return async_read_some(buffer, std::forward<Token>(token));
    }

    template <typename Token> auto async_write(ConstByteSpan buffer, Token &&token) {
        auto initiation = [this, buffer](auto handler) mutable {
            std::string payload(reinterpret_cast<const char *>(buffer.data()), buffer.size());
            asio::post(m_ex, [this, payload = std::move(payload), handler = std::move(handler)]() mutable {
                m_written += payload;
                if (m_fail_writes)
                    std::move(handler)(make_error_code(asio::error::connection_reset), 0);
                else
                    std::move(handler)(error_code{}, payload.size());
            });
        };
        return asio::async_initiate<Token, void(error_code, std::size_t)>(
            simple_http::detail::make_executor_initiation(std::move(initiation), m_ex), token);
    }

    template <typename Token> auto async_write_seq(span<const ConstByteSpan> buffers, Token &&token) {
        std::size_t total = 0;
        std::string payload;
        for (const auto &b : buffers) {
            payload.append(reinterpret_cast<const char *>(b.data()), b.size());
            total += b.size();
        }
        auto initiation = [this, payload = std::move(payload), total](auto handler) mutable {
            asio::post(m_ex, [this, payload = std::move(payload), total, handler = std::move(handler)]() mutable {
                m_written += payload;
                if (m_fail_writes)
                    std::move(handler)(make_error_code(asio::error::connection_reset), 0);
                else
                    std::move(handler)(error_code{}, total);
            });
        };
        return asio::async_initiate<Token, void(error_code, std::size_t)>(
            simple_http::detail::make_executor_initiation(std::move(initiation), m_ex), token);
    }

    template <typename Token> auto async_shutdown(Token &&token) {
        auto initiation = [this](auto handler) mutable {
            asio::post(m_ex, [handler = std::move(handler)]() mutable { std::move(handler)(error_code{}); });
        };
        return asio::async_initiate<Token, void(error_code)>(
            simple_http::detail::make_executor_initiation(std::move(initiation), m_ex), token);
    }

    bool pending_read() const { return m_read_handler.has_value(); }

    // Invokes the pending read completion with `bytes` (truncated to the
    // engine's buffer size). Must be called on the connection executor.
    void complete_read(std::string_view bytes) {
        auto handler = std::move(*m_read_handler);
        m_read_handler.reset();
        const std::size_t n = std::min(m_read_cap, bytes.size());
        std::memcpy(m_read_buf, bytes.data(), n);
        std::move(handler)(error_code{}, n);
    }

    const std::string &written() const { return m_written; }
    bool closed() const { return m_closed; }

  private:
    Executor m_ex;
    bool m_fail_writes = false;
    bool m_closed = false;
    std::byte *m_read_buf = nullptr;
    std::size_t m_read_cap = 0;
    std::optional<ReadHandler> m_read_handler;
    std::string m_written;
};

EngineLimits repro_limits() {
    EngineLimits limits;
    limits.idle_timeout = std::chrono::seconds(0); // no watchdog: keep the harness deterministic
    return limits;
}

void pump(asio::io_context &ctx, std::chrono::milliseconds ms = std::chrono::milliseconds(200)) {
    ctx.restart();
    ctx.run_for(ms);
}

// ---------------------------------------------------------------------------
// P1
// ---------------------------------------------------------------------------
int run_p1() {
    // Evidence from the decoder itself: both flags are set by this block.
    {
        HpackDecoder decoder;
        decoder.set_max_list_size(64 * 1024);
        std::vector<HpackHeader> fields;
        const std::string block = bomb_then_malformed_block();
        const bool ok = decoder.decode(block, fields);
        std::printf("[P1] decoder: decode=%d compression_error=%d header_list_too_large=%d "
                    "dynamic_table_bytes=%zu (block=%zu bytes)\n",
                    ok ? 1 : 0, decoder.compression_error() ? 1 : 0, decoder.header_list_too_large() ? 1 : 0,
                    decoder.dynamic_table_size(), block.size());
        if (ok || !decoder.compression_error() || !decoder.header_list_too_large() ||
            decoder.dynamic_table_size() == 0) {
            std::printf("[P1] repro precondition failed\n");
            return 2;
        }
    }

    asio::io_context ctx;
    auto transport = std::make_shared<TestTransport>(ctx.get_executor(), /*fail_writes=*/false);
    EngineLimits limits = repro_limits();
    limits.h2_max_concurrent_streams = 1; // stream 1 fills the table; stream 3 is refused

    int dispatched = 0;
    std::vector<RequestPtr> keep_alive;
    CallbackHandler dispatch = [&](RequestPtr req, ResponsePtr, SslHandle, HandlerCompletion) {
        ++dispatched;
        keep_alive.push_back(std::move(req)); // never completes: stream 1 stays open
    };
    bool done = false;
    auto engine = std::make_shared<Http2Engine<TestTransport>>(transport, limits);
    engine->run(dispatch, std::string{}, {}, {}, {}, [&] { done = true; });

    // Preface + client SETTINGS + stream 1 (valid, END_STREAM) + stream 3
    // (refused, END_HEADERS) carrying the malicious block.
    std::string in{kH2ClientPreface};
    in += frame(H2FrameType::Settings, 0, 0, {});
    in += frame(H2FrameType::Headers, H2_FLAG_END_HEADERS | H2_FLAG_END_STREAM, 1, valid_head_block());
    in += frame(H2FrameType::Headers, H2_FLAG_END_HEADERS | H2_FLAG_END_STREAM, 3, bomb_then_malformed_block());

    if (!transport->pending_read()) {
        std::printf("[P1] engine did not start a read\n");
        return 2;
    }
    transport->complete_read(in);
    pump(ctx);

    const Found goaway = find_frame(transport->written(), H2FrameType::Goaway, 0);
    const Found rst = find_frame(transport->written(), H2FrameType::RstStream, 3);
    std::printf("[P1] dispatched=%d goaway=%d goaway_error=0x%x rst3=%d rst3_error=0x%x written=%zu bytes\n",
                dispatched, goaway.present ? 1 : 0, goaway.error, rst.present ? 1 : 0, rst.error,
                transport->written().size());

    if (goaway.present && goaway.error == H2_COMPRESSION_ERROR) {
        std::printf("[P1] OK: malformed discard-block is a connection-level COMPRESSION_ERROR\n");
        return 0;
    }
    std::printf("[P1] BUG REPRODUCED: no GOAWAY(COMPRESSION_ERROR); the connection was kept alive after a "
                "block that was not decoded to the end\n");
    return 1;
}

// ---------------------------------------------------------------------------
// P2
// ---------------------------------------------------------------------------
int run_p2() {
    asio::io_context ctx;
    // Every write fails: the engine starts a read, then the SETTINGS write
    // completion reports an error, which drives finish_connection() while the
    // read is still outstanding.
    auto transport = std::make_shared<TestTransport>(ctx.get_executor(), /*fail_writes=*/true);
    EngineLimits limits = repro_limits();

    int dispatched = 0;
    std::vector<RequestPtr> keep_alive;
    CallbackHandler dispatch = [&](RequestPtr req, ResponsePtr, SslHandle, HandlerCompletion) {
        ++dispatched;
        keep_alive.push_back(std::move(req));
    };
    bool done = false;
    auto engine = std::make_shared<Http2Engine<TestTransport>>(transport, limits);
    engine->run(dispatch, std::string{}, {}, {}, {}, [&] { done = true; });
    pump(ctx);

    std::printf("[P2] after teardown: done=%d dispatched=%d pending_read=%d closed=%d\n", done ? 1 : 0, dispatched,
                transport->pending_read() ? 1 : 0, transport->closed() ? 1 : 0);
    if (!done || dispatched != 0 || !transport->pending_read()) {
        std::printf("[P2] repro precondition failed (done=%d dispatched=%d pending_read=%d)\n", done ? 1 : 0,
                    dispatched, transport->pending_read() ? 1 : 0);
        return 2;
    }

    // A late successful read: the peer's request is already in flight. A real
    // transport would normally complete this with an error; a socket/SSL read
    // can also complete successfully with data before close takes effect.
    std::string in{kH2ClientPreface};
    in += frame(H2FrameType::Headers, H2_FLAG_END_HEADERS | H2_FLAG_END_STREAM, 1, valid_head_block());
    transport->complete_read(in);
    pump(ctx, std::chrono::milliseconds(50));

    std::printf("[P2] after late read: dispatched=%d (handler runs after on_done=%d)\n", dispatched, done ? 1 : 0);
    if (dispatched == 0) {
        std::printf("[P2] OK: no handler dispatched after teardown\n");
        return 0;
    }
    std::printf("[P2] BUG REPRODUCED: a handler was dispatched on a connection that already finished teardown\n");
    return 1;
}

} // namespace

int main(int argc, char **argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "p1")
        return run_p1();
    if (mode == "p2")
        return run_p2();
    if (mode == "dump") {
        // dump <path> [fields] [malformed|wellformed]
        if (argc < 3)
            return 2;
        const std::size_t fields = argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 1500;
        const bool malformed = argc <= 4 || std::string_view(argv[4]) != "wellformed";
        const std::string block = bomb_then_malformed_block(fields, malformed);
        std::FILE *f = std::fopen(argv[2], "wb");
        if (!f)
            return 2;
        const std::size_t wrote = std::fwrite(block.data(), 1, block.size(), f);
        std::fclose(f);
        std::printf("wrote %zu/%zu bytes to %s\n", wrote, block.size(), argv[2]);
        return wrote == block.size() ? 0 : 2;
    }
    std::printf("usage: %s p1|p2|dump <path> [fields] [malformed|wellformed]\n", argv[0]);
    return 2;
}
