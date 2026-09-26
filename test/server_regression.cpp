// Server regression suite: the bytes a well-behaved client will not send.
//
// Every case here drives an in-process server over a raw TCP socket — malformed
// request lines, framing conflicts, oversized heads and bodies, HTTP/2 frames in
// the wrong state, WebSocket protocol violations — and asserts what comes back.
// The library's own client is deliberately well-behaved, so it cannot produce
// these; a socket can.
//
//   xmake build regression && xmake run regression

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <boost/asio.hpp>

#include "simple_http.h"

namespace asio = boost::asio;
namespace sh = simple_http;

namespace {

constexpr std::uint16_t kPlainPort = 27920;
constexpr std::uint16_t kTlsPort = 27921;
constexpr std::size_t kMaxHeaderBytes = 4096;
constexpr std::size_t kMaxBodyBytes = 64 * 1024;

// --- an in-process server ----------------------------------------------------

sh::ServerConfig make_config(std::uint16_t port, std::optional<sh::TlsConfig> tls) {
    sh::ServerConfig cfg;
    cfg.listen = sh::InetAddress{"127.0.0.1", port, false};
    cfg.worker_threads = 2;
    cfg.tls = std::move(tls);
    // Small limits, so the boundary cases stay cheap to produce.
    cfg.limits.max_header_bytes = kMaxHeaderBytes;
    cfg.limits.max_body_bytes = kMaxBodyBytes;
    cfg.limits.idle_timeout = std::chrono::seconds(2);
    return cfg;
}

void register_routes(sh::Server& server) {
    server.route("/world", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).content_type("text/plain").send("hello");
    });
    // Echoes the body length and its bytes, so framing is observable.
    server.route("/echo", [](sh::RequestPtr req, sh::ResponsePtr res) -> asio::awaitable<void> {
        auto body = co_await req->body().read_all();
        co_await res->status(200).send("len=" + std::to_string(body ? body->size() : 0) + ":" + (body ? *body : ""));
    });
    server.route("/empty", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(204).send_bodyless();
    });
    server.route("/stream", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        (void)co_await res->status(200).content_type("text/plain").begin();
        (void)co_await res->write("one-");
        (void)co_await res->finish("two");
    });
    server.route("/big", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).send(std::string(9000, 'x'));
    });
    // Answers late and never reads the body: the stream stays open with an
    // unconsumed body, so nothing replenishes the flow-control window.
    server.route("/slow", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        asio::steady_timer timer{co_await asio::this_coro::executor};
        timer.expires_after(std::chrono::milliseconds(1500));
        co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        co_await res->status(200).send("late");
    });
    // A field carrying CR/LF is response splitting; the response must not go out.
    server.route("/inject", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        (void)co_await res->status(200).header("x-bad", "a\r\ninjected: 1").send("hi");
    });
    // A handler that sets its own Content-Length: the writer owns that field and
    // must replace it, not append a second one.
    server.route("/clash", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).header("content-length", "999").send("hi");
    });
    server.ws_route("/chat", [](sh::RequestPtr, std::shared_ptr<sh::WebSocket> ws) -> asio::awaitable<void> {
        for (;;) {
            auto message = co_await ws->read();
            if (!message)
                break;
            if (!co_await ws->write(message->data, message->text))
                break;
        }
        co_return;
    });
    server.fallback([](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(404).send("not found");
    });
}

// One plaintext and one mutual-TLS listener, started once for the whole suite.
// Server is neither copyable nor movable, so it lives behind a unique_ptr (whose
// destructor stops it at exit).
sh::Server& plain_server() {
    static std::unique_ptr<sh::Server> server = [] {
        auto s = std::make_unique<sh::Server>(make_config(kPlainPort, std::nullopt));
        register_routes(*s);
        s->start();
        return s;
    }();
    return *server;
}

sh::Server& tls_server() {
    static std::unique_ptr<sh::Server> server = [] {
        sh::TlsConfig tls;
        tls.cert_chain_file = "./test/tls_certificates/server_cert.pem";
        tls.private_key_file = "./test/tls_certificates/server_key.pem";
        tls.mutual = true;
        tls.ca_file = std::string{"./test/tls_certificates/ca_cert.pem"};
        auto s = std::make_unique<sh::Server>(make_config(kTlsPort, tls));
        register_routes(*s);
        s->start();
        return s;
    }();
    return *server;
}

// --- a raw peer --------------------------------------------------------------

// A TCP peer that sends exactly the bytes it is given and accumulates everything
// it receives, with the test pumping the shared io_context. Reads run as one
// detached coroutine per client (like the server's own engines do).
class RawClient {
  public:
    // The read coroutine outlives the RawClient itself (a test may drop the client
    // while a read is pending), so everything it touches lives in a shared state.
    struct State {
        explicit State(asio::io_context& ctx) : socket(std::make_shared<asio::ip::tcp::socket>(ctx)) {
        }

        std::shared_ptr<asio::ip::tcp::socket> socket;
        std::string received;
        bool eof{false};
    };

    explicit RawClient(asio::io_context& ctx) : m_state(std::make_shared<State>(ctx)), m_ctx(ctx) {
    }

    bool connect(std::uint16_t port) {
        sh::error_code ec;
        m_state->socket->connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), ec);
        if (ec)
            return false;
        start_reading();
        return true;
    }

    void send(std::string_view bytes) {
        sh::error_code ec;
        asio::write(*m_state->socket, asio::buffer(bytes.data(), bytes.size()), ec);
    }

    // Runs the context until `done()` holds or the budget runs out.
    template <typename Pred>
    bool pump_until(Pred done, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!done() && std::chrono::steady_clock::now() < deadline) {
            m_ctx.restart();
            m_ctx.run_for(std::chrono::milliseconds(5));
        }
        return done();
    }

    bool wait_for(std::string_view needle, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        return pump_until([&] { return m_state->received.find(needle) != std::string::npos; }, timeout);
    }

    bool wait_eof(std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        return pump_until([&] { return m_state->eof; }, timeout);
    }

    bool wait_head(std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        return wait_for("\r\n\r\n", timeout);
    }

    void pump(std::chrono::milliseconds slice = std::chrono::milliseconds(50)) {
        m_ctx.restart();
        m_ctx.run_for(slice);
    }

    const std::string& received() const {
        return m_state->received;
    }

    bool eof() const {
        return m_state->eof;
    }

    void close() {
        sh::error_code ec;
        m_state->socket->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        m_state->socket->close(ec);
    }

  private:
    void start_reading() {
        auto state = m_state;
        asio::co_spawn(
            state->socket->get_executor(),
            [state]() -> asio::awaitable<void> {
                std::array<std::byte, 4096> buf{};
                for (;;) {
                    auto [ec, n] =
                        co_await state->socket->async_read_some(asio::buffer(buf), asio::as_tuple(asio::use_awaitable));
                    if (ec) {
                        state->eof = true;
                        co_return;
                    }
                    state->received.append(reinterpret_cast<const char*>(buf.data()), n);
                }
            },
            asio::detached);
    }

    std::shared_ptr<State> m_state;
    asio::io_context& m_ctx;
};

// --- small HTTP/1.x assertions ----------------------------------------------

int status_of(std::string_view response) {
    const auto space = response.find(' ');
    if (space == std::string_view::npos)
        return 0;
    return std::atoi(std::string{response.substr(space + 1, 3)}.c_str());
}

// Case-insensitive field-name search over the response head (the library writes
// canonical casing, a test should not depend on it).
bool head_has(std::string_view response, std::string_view field) {
    const auto head_end = response.find("\r\n\r\n");
    const std::string_view head = response.substr(0, head_end);
    auto lowered = [](std::string_view in) {
        std::string out{in};
        for (auto& c : out)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    };
    return lowered(head).find(lowered(field)) != std::string::npos;
}

// Everything after the response head.
std::string_view after_head(std::string_view response) {
    const auto head_end = response.find("\r\n\r\n");
    if (head_end == std::string_view::npos)
        return {};
    return response.substr(head_end + 4);
}

std::string body_of(std::string_view response) {
    return std::string{after_head(response)};
}

// For diagnostics: what actually arrived, with the unprintable bytes escaped.
std::string printable(std::string_view bytes) {
    std::string out;
    for (unsigned char c : bytes) {
        if (c >= 0x20 && c < 0x7F) {
            out.push_back(static_cast<char>(c));
        } else {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02x", c);
            out.append(buf);
        }
    }
    return out;
}

// --- small HTTP/2 helpers ----------------------------------------------------

std::string h2_frame(sh::codec::H2FrameType type,
                     std::uint8_t flags,
                     std::uint32_t stream_id,
                     std::string_view payload) {
    std::string out;
    sh::codec::serialize_frame_header(
        out, static_cast<std::uint32_t>(payload.size()), static_cast<std::uint8_t>(type), flags, stream_id);
    out.append(payload);
    return out;
}

std::string h2_request_headers(std::string_view method,
                               std::string_view path,
                               std::uint32_t stream_id = 1,
                               bool end_stream = true) {
    std::string block;
    sh::codec::hpack_append_literal(block, ":method", method);
    sh::codec::hpack_append_literal(block, ":scheme", "http");
    sh::codec::hpack_append_literal(block, ":authority", "127.0.0.1");
    sh::codec::hpack_append_literal(block, ":path", path);
    std::uint8_t flags = sh::codec::H2_FLAG_END_HEADERS;
    if (end_stream)
        flags |= sh::codec::H2_FLAG_END_STREAM;
    return h2_frame(sh::codec::H2FrameType::Headers, flags, stream_id, block);
}

// The frames received so far, in order.
struct H2Frame {
    sh::codec::H2FrameHeader header;
    std::string payload;
};

std::vector<H2Frame> parse_frames(std::string_view bytes) {
    std::vector<H2Frame> frames;
    std::size_t pos = 0;
    while (bytes.size() - pos >= sh::codec::kH2FrameHeaderSize) {
        sh::codec::H2FrameHeader header;
        sh::codec::parse_frame_header(bytes.substr(pos), header);
        const std::size_t end = pos + sh::codec::kH2FrameHeaderSize + header.length;
        if (end > bytes.size())
            break;
        frames.push_back({header, std::string{bytes.substr(pos + sh::codec::kH2FrameHeaderSize, header.length)}});
        pos = end;
    }
    return frames;
}

std::optional<H2Frame> find_frame(const std::vector<H2Frame>& frames, sh::codec::H2FrameType type) {
    for (const auto& frame : frames) {
        if (frame.header.type == static_cast<std::uint8_t>(type))
            return frame;
    }
    return std::nullopt;
}

// Decodes a HEADERS payload into fields (the block may need CONTINUATION, which
// our requests never produce; responses here are small).
std::vector<sh::codec::HpackHeader> decode_headers(std::string_view block) {
    sh::codec::HpackDecoder decoder;
    std::vector<sh::codec::HpackHeader> fields;
    decoder.decode(block, fields);
    return fields;
}

std::string header_value(const std::vector<sh::codec::HpackHeader>& fields, std::string_view name) {
    for (const auto& field : fields) {
        if (field.name == name)
            return field.value;
    }
    return {};
}

std::string h2_preface_and_settings() {
    std::string out{sh::codec::kH2ClientPreface};
    out.append(h2_frame(sh::codec::H2FrameType::Settings, 0, 0, std::string(0, '\0')));
    return out;
}

}  // namespace

// --- HTTP/1.x ----------------------------------------------------------------

TEST_CASE("regression/h1: malformed request heads are rejected with 400", "[regression][h1]") {
    plain_server();
    asio::io_context ctx;

    const std::array<std::pair<std::string_view, const char*>, 4> cases = {{
        {"GET/ HTTP/1.1\r\nHost: x\r\n\r\n", "no space after the method"},
        {"GET / HTTP/2.0\r\n\r\n", "an unknown version"},
        {"GET / HTTP/1.1\r\nHost: x\r\nBroken\r\n\r\n", "a header line without a colon"},
        {"GET / HTTP/1.1\r\nHost: x\r\n folded\r\n\r\n", "an obs-fold continuation"},
    }};

    for (const auto& [request, what] : cases) {
        RawClient client{ctx};
        REQUIRE(client.connect(kPlainPort));
        client.send(request);
        REQUIRE(client.wait_head());
        INFO("case: " << what);
        CHECK(status_of(client.received()) == 400);
        CHECK(client.wait_eof());  // a malformed request closes the connection
        client.close();
    }
}

TEST_CASE("regression/h1: an oversized head is refused with 431", "[regression][h1]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    // The limit is enforced while the head is still incomplete, so the padding
    // is sent without its terminating blank line.
    std::string request = "GET / HTTP/1.1\r\nHost: x\r\n";
    while (request.size() < kMaxHeaderBytes + 512) {
        request += "x-padding-header: " + std::string(200, 'p') + "\r\n";
    }
    client.send(request);

    REQUIRE(client.wait_head());
    CHECK(status_of(client.received()) == 431);
    client.close();
}

TEST_CASE("regression/h1: a body larger than the cap is refused with 413", "[regression][h1]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    client.send("POST /echo HTTP/1.1\r\nHost: x\r\ncontent-length: " + std::to_string(kMaxBodyBytes + 1) + "\r\n\r\n");
    REQUIRE(client.wait_head());
    CHECK(status_of(client.received()) == 413);
    client.close();
}

TEST_CASE("regression/h1: Content-Length with Transfer-Encoding frames as chunked", "[regression][h1]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    // RFC 9112 §6.3: when both are present, Transfer-Encoding wins (and the
    // Content-Length must not be believed — otherwise a smuggled message slips
    // past a front-end that framed it differently).
    client.send(
        "POST /echo HTTP/1.1\r\nHost: x\r\ncontent-length: 5\r\ntransfer-encoding: chunked\r\n\r\n"
        "4\r\nabcd\r\n0\r\n\r\n");
    REQUIRE(client.wait_for("len="));
    CHECK(status_of(client.received()) == 200);
    CHECK(body_of(client.received()) == "len=4:abcd");
    client.close();
}

TEST_CASE("regression/h1: a malformed chunk size is answered then dropped", "[regression][h1]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    // The handler is dispatched as soon as the head is parsed, so it answers
    // 200; the broken body then ends the connection instead of hanging.
    client.send("POST /echo HTTP/1.1\r\nHost: x\r\ntransfer-encoding: chunked\r\n\r\nzz\r\nbody\r\n0\r\n\r\n");
    REQUIRE(client.wait_head());
    CHECK(client.wait_eof());
    client.close();
}

TEST_CASE("regression/h1: chunked trailers are consumed", "[regression][h1]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    client.send(
        "POST /echo HTTP/1.1\r\nHost: x\r\ntransfer-encoding: chunked\r\n\r\n"
        "3\r\nabc\r\n0\r\nx-trailer: yes\r\n\r\n");
    REQUIRE(client.wait_for("len="));
    CHECK(body_of(client.received()) == "len=3:abc");
    client.close();
}

TEST_CASE("regression/h1: pipelined requests are answered in order", "[regression][h1]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    client.send(
        "GET /echo HTTP/1.1\r\nHost: x\r\ncontent-length: 1\r\n\r\nA"  // a body exercies the pipelining boundary
        "GET /world HTTP/1.1\r\nHost: x\r\n\r\n");
    REQUIRE(client.pump_until(
        [&] {
            const auto& r = client.received();
            return r.find("len=1:A") != std::string::npos && r.find("hello") != std::string::npos;
        },
        std::chrono::seconds(5)));

    const auto& received = client.received();
    CHECK(received.find("len=1:A") < received.find("hello"));  // first request answered first
    client.close();
}

TEST_CASE("regression/h1: HEAD and 204 carry no body", "[regression][h1]") {
    plain_server();
    asio::io_context ctx;
    {
        RawClient client{ctx};
        REQUIRE(client.connect(kPlainPort));
        client.send("HEAD /big HTTP/1.1\r\nHost: x\r\n\r\n");
        REQUIRE(client.wait_head());
        CHECK(status_of(client.received()) == 200);
        CHECK(head_has(client.received(), "content-length: 9000"));  // what a GET would produce…
        CHECK(body_of(client.received()).empty());                   // …but no body follows the head
        client.send("GET /world HTTP/1.1\r\nHost: x\r\n\r\n");                  // and the connection stays usable
        REQUIRE(client.wait_for("hello"));
        client.close();
    }
    {
        RawClient client{ctx};
        REQUIRE(client.connect(kPlainPort));
        client.send("GET /empty HTTP/1.1\r\nHost: x\r\n\r\n");
        REQUIRE(client.wait_head());
        CHECK(status_of(client.received()) == 204);
        CHECK(body_of(client.received()).empty());
        client.send("GET /world HTTP/1.1\r\nHost: x\r\n\r\n");
        REQUIRE(client.wait_for("hello"));
        client.close();
    }
}

TEST_CASE("regression/h1: Connection: close is honoured", "[regression][h1]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    client.send("GET /world HTTP/1.1\r\nHost: x\r\nconnection: close\r\n\r\n");
    REQUIRE(client.wait_for("hello"));
    CHECK(head_has(client.received(), "connection: close"));
    CHECK(client.wait_eof());
    client.close();
}

TEST_CASE("regression/h1: an idle connection is closed by the watchdog", "[regression][h1]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    client.send("GET /world HTTP/1.1\r\nHost: x\r\n\r\n");
    REQUIRE(client.wait_for("hello"));
    // limits.idle_timeout is 2s in this suite: a silent connection must be reaped.
    CHECK(client.wait_eof(std::chrono::seconds(6)));
    client.close();
}

TEST_CASE("regression/h1: Expect: 100-continue is not answered by default", "[regression][h1]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    // The engine does not send 100 Continue on its own (a handler may, via
    // send_continue()). A client that sends the body anyway must be served.
    client.send("POST /echo HTTP/1.1\r\nHost: x\r\nexpect: 100-continue\r\ncontent-length: 2\r\n\r\n");
    client.pump(std::chrono::milliseconds(100));
    CHECK(client.received().find("100 Continue") == std::string::npos);

    client.send("hi");
    REQUIRE(client.wait_for("len="));
    CHECK(body_of(client.received()) == "len=2:hi");
    client.close();
}

// --- HTTP/2 ------------------------------------------------------------------

TEST_CASE("regression/h2: the preface, SETTINGS and a request", "[regression][h2]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    client.send(h2_preface_and_settings());
    client.send(h2_request_headers("GET", "/world"));
    // Wait for the SETTINGS ACK and the response DATA by *parsing* what arrived
    // (a byte pattern with a leading NUL is an empty C string to string::find).
    REQUIRE(client.pump_until(
        [&] {
            const auto frames = parse_frames(client.received());
            bool acked = false;
            for (const auto& frame : frames) {
                if (frame.header.type == static_cast<std::uint8_t>(sh::codec::H2FrameType::Settings) &&
                    (frame.header.flags & sh::codec::H2_FLAG_ACK) != 0) {
                    acked = true;
                }
            }
            return acked && find_frame(frames, sh::codec::H2FrameType::Data).has_value();
        },
        std::chrono::seconds(5)));

    const auto frames = parse_frames(client.received());
    bool acked = false;
    for (const auto& frame : frames) {
        if (frame.header.type == static_cast<std::uint8_t>(sh::codec::H2FrameType::Settings) &&
            (frame.header.flags & sh::codec::H2_FLAG_ACK) != 0) {
            acked = true;
        }
    }
    CHECK(acked);
    const auto headers = find_frame(frames, sh::codec::H2FrameType::Headers);
    REQUIRE(headers.has_value());
    CHECK(headers->header.stream_id == 1);
    const auto fields = decode_headers(headers->payload);
    CHECK(header_value(fields, ":status") == "200");
    CHECK(header_value(fields, "content-type") == "text/plain");

    const auto data = find_frame(frames, sh::codec::H2FrameType::Data);
    REQUIRE(data.has_value());
    CHECK(data->payload == "hello");
    CHECK((data->header.flags & sh::codec::H2_FLAG_END_STREAM) != 0);
    client.close();
}

TEST_CASE("regression/h2: PING is echoed", "[regression][h2]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));
    client.send(h2_preface_and_settings());

    const std::string payload = "12345678";
    client.send(h2_frame(sh::codec::H2FrameType::Ping, 0, 0, payload));
    REQUIRE(client.pump_until(
        [&] {
            for (const auto& frame : parse_frames(client.received())) {
                if (frame.header.type == static_cast<std::uint8_t>(sh::codec::H2FrameType::Ping) &&
                    (frame.header.flags & sh::codec::H2_FLAG_ACK) != 0) {
                    return true;
                }
            }
            return false;
        },
        std::chrono::seconds(5)));

    const auto ping = find_frame(parse_frames(client.received()), sh::codec::H2FrameType::Ping);
    REQUIRE(ping.has_value());
    CHECK(ping->payload == payload);  // echoed verbatim
    client.close();
}

TEST_CASE("regression/h2: protocol violations end the connection with GOAWAY", "[regression][h2]") {
    plain_server();
    asio::io_context ctx;

    auto goaway_error = [&](std::string bytes) -> std::optional<std::uint32_t> {
        RawClient client{ctx};
        REQUIRE(client.connect(kPlainPort));
        client.send(h2_preface_and_settings());
        client.send(std::move(bytes));
        client.pump_until(
            [&] {
                return find_frame(parse_frames(client.received()), sh::codec::H2FrameType::Goaway).has_value() ||
                       client.eof();
            },
            std::chrono::seconds(3));
        const auto goaway = find_frame(parse_frames(client.received()), sh::codec::H2FrameType::Goaway);
        if (!goaway) {
            std::fprintf(stderr, "no GOAWAY; received: %s\n", printable(client.received()).c_str());
            return std::nullopt;
        }
        return sh::codec::read_u32(goaway->payload, 4);
    };

    SECTION("a frame larger than the advertised MAX_FRAME_SIZE") {
        // The engine advertises 16384; a bigger frame is a FRAME_SIZE_ERROR.
        std::string oversized = h2_frame(sh::codec::H2FrameType::Data, 0, 1, std::string(20000, 'x'));
        auto error = goaway_error(std::move(oversized));
        REQUIRE(error.has_value());
        CHECK(*error == sh::codec::H2_FRAME_SIZE_ERROR);
    }
    SECTION("DATA on stream 0") {
        auto error = goaway_error(h2_frame(sh::codec::H2FrameType::Data, 0, 0, "x"));
        REQUIRE(error.has_value());
        CHECK(*error == sh::codec::H2_PROTOCOL_ERROR);
    }
    SECTION("HEADERS on a server-initiated (even) stream id") {
        std::string block;
        sh::codec::hpack_append_literal(block, ":method", "GET");
        auto error = goaway_error(h2_frame(sh::codec::H2FrameType::Headers, sh::codec::H2_FLAG_END_HEADERS, 2, block));
        REQUIRE(error.has_value());
        CHECK(*error == sh::codec::H2_PROTOCOL_ERROR);
    }
    SECTION("a SETTINGS payload that is not a multiple of six") {
        auto error = goaway_error(h2_frame(sh::codec::H2FrameType::Settings, 0, 0, "abc"));
        REQUIRE(error.has_value());
        CHECK(*error == sh::codec::H2_FRAME_SIZE_ERROR);
    }
    SECTION("a magic number in a pseudo-header value") {
        // A field value carrying a NUL (or CR/LF) must not reach a handler: the
        // stream is reset, the connection survives.
        std::string block;
        sh::codec::hpack_append_literal(block, ":method", "GET");
        sh::codec::hpack_append_literal(block, ":scheme", "http");
        sh::codec::hpack_append_literal(block, ":authority", "127.0.0.1");
        sh::codec::hpack_append_literal(block, ":path", "/world");
        sh::codec::hpack_append_literal(block, "x-bad", std::string_view{"a\0b", 3});
        RawClient client{ctx};
        REQUIRE(client.connect(kPlainPort));
        client.send(h2_preface_and_settings());
        client.send(h2_frame(
            sh::codec::H2FrameType::Headers, sh::codec::H2_FLAG_END_HEADERS | sh::codec::H2_FLAG_END_STREAM, 1, block));
        client.pump_until(
            [&] {
                return find_frame(parse_frames(client.received()), sh::codec::H2FrameType::RstStream).has_value() ||
                       client.eof();
            },
            std::chrono::seconds(3));
        const auto reset = find_frame(parse_frames(client.received()), sh::codec::H2FrameType::RstStream);
        REQUIRE(reset.has_value());  // stream error, not a connection error
        CHECK(sh::codec::read_u32(reset->payload, 0) == sh::codec::H2_PROTOCOL_ERROR);
    }
}

TEST_CASE("regression/h2: an unconsumed body past the window is fatal", "[regression][h2]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));
    client.send(h2_preface_and_settings());

    // /slow never reads the body, so nothing replenishes the window. The stream
    // and connection windows are both 65535 (SETTINGS_INITIAL_WINDOW_SIZE and the
    // protocol default), so a single flooded stream trips the connection limit:
    // GOAWAY(FLOW_CONTROL_ERROR). A peer cannot enlarge our receive window for us.
    client.send(h2_request_headers("POST", "/slow", /*stream_id=*/1, /*end_stream=*/false));
    const std::string chunk(8000, 'x');
    for (int i = 0; i < 10 && !client.eof(); ++i) {
        client.send(h2_frame(sh::codec::H2FrameType::Data, 0, 1, chunk));
    }
    client.pump_until(
        [&] {
            return find_frame(parse_frames(client.received()), sh::codec::H2FrameType::Goaway).has_value() ||
                   client.eof();
        },
        std::chrono::seconds(5));
    const auto goaway = find_frame(parse_frames(client.received()), sh::codec::H2FrameType::Goaway);
    REQUIRE(goaway.has_value());
    CHECK(sh::codec::read_u32(goaway->payload, 4) == sh::codec::H2_FLOW_CONTROL_ERROR);
    client.close();
}

TEST_CASE("regression/h2: a connection-level flow-control overrun is fatal", "[regression][h2]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));
    client.send(h2_preface_and_settings());

    // Two streams with unconsumed bodies, each staying under its own window but
    // together overrunning the connection's shared 65535 octets.
    client.send(h2_request_headers("POST", "/slow", /*stream_id=*/1, /*end_stream=*/false));
    client.send(h2_request_headers("POST", "/slow", /*stream_id=*/3, /*end_stream=*/false));
    const std::string chunk(8000, 'x');
    for (int i = 0; i < 5; ++i) {
        client.send(h2_frame(sh::codec::H2FrameType::Data, 0, 1, chunk));
        client.send(h2_frame(sh::codec::H2FrameType::Data, 0, 3, chunk));
    }
    client.pump_until(
        [&] {
            return find_frame(parse_frames(client.received()), sh::codec::H2FrameType::Goaway).has_value() ||
                   client.eof();
        },
        std::chrono::seconds(5));
    const auto goaway = find_frame(parse_frames(client.received()), sh::codec::H2FrameType::Goaway);
    REQUIRE(goaway.has_value());
    CHECK(sh::codec::read_u32(goaway->payload, 4) == sh::codec::H2_FLOW_CONTROL_ERROR);
    client.close();
}

TEST_CASE("regression/h2: an h2c upgrade replays the request as stream 1", "[regression][h2]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    // RFC 9113 §3.2: the HTTP/1.1 request that carries the upgrade becomes
    // stream 1 of the new connection.
    const std::string settings = sh::base64_url_encode(std::string(0, '\0'));
    client.send(
        "GET /world HTTP/1.1\r\nhost: 127.0.0.1\r\nconnection: Upgrade, HTTP2-Settings\r\n"
        "upgrade: h2c\r\nhttp2-settings: " +
        settings + "\r\n\r\n");
    REQUIRE(client.wait_for("101"));
    CHECK(status_of(client.received()) == 101);
    CHECK(head_has(client.received(), "upgrade: h2c"));

    const std::size_t switch_end = client.received().find("\r\n\r\n") + 4;
    const std::string after_switch = client.received().substr(switch_end);
    const auto frames = parse_frames(after_switch);
    const auto headers = find_frame(frames, sh::codec::H2FrameType::Headers);
    REQUIRE(headers.has_value());
    CHECK(headers->header.stream_id == 1);  // the upgraded request's own stream
    const auto fields = decode_headers(headers->payload);
    CHECK(header_value(fields, ":status") == "200");

    // The connection continues as HTTP/2, and the client must send its own
    // connection preface now (RFC 9113 §3.2), then a request on stream 3 (ids
    // start there after the upgraded request took stream 1).
    client.send(h2_preface_and_settings());
    client.send(h2_request_headers("GET", "/empty", /*stream_id=*/3));
    REQUIRE(client.pump_until(
        [&] {
            const auto all = parse_frames(client.received().substr(switch_end));
            for (const auto& frame : all) {
                if (frame.header.type == static_cast<std::uint8_t>(sh::codec::H2FrameType::Headers) &&
                    frame.header.stream_id >= 3) {
                    return true;
                }
            }
            return false;
        },
        std::chrono::seconds(5)));
    client.close();
}

// --- WebSocket ---------------------------------------------------------------

TEST_CASE("regression/ws: the handshake and the accept key", "[regression][ws]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    client.send(
        "GET /chat HTTP/1.1\r\nhost: 127.0.0.1\r\nconnection: Upgrade\r\nupgrade: websocket\r\n"
        "sec-websocket-version: 13\r\nsec-websocket-key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
    REQUIRE(client.wait_head());
    CHECK(status_of(client.received()) == 101);
    CHECK(head_has(client.received(), "upgrade: websocket"));
    // RFC 6455 §1.3's own example.
    CHECK(head_has(client.received(), "sec-websocket-accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
    client.close();
}

TEST_CASE("regression/ws: echo, fragmentation, Ping and Close", "[regression/ws]") {
    plain_server();
    asio::io_context ctx;

    auto handshake = [&](RawClient& client) {
        REQUIRE(client.connect(kPlainPort));
        client.send(
            "GET /chat HTTP/1.1\r\nhost: 127.0.0.1\r\nconnection: Upgrade\r\nupgrade: websocket\r\n"
            "sec-websocket-version: 13\r\nsec-websocket-key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
        REQUIRE(client.wait_head());
        REQUIRE(status_of(client.received()) == 101);
    };

    auto masked_frame = [](sh::WsOpcode opcode, std::string_view payload, bool fin = true) {
        const unsigned char mask[4] = {0x11, 0x22, 0x33, 0x44};
        std::string frame;
        frame.push_back(static_cast<char>((fin ? 0x80 : 0x00) | static_cast<unsigned char>(opcode)));
        frame.push_back(static_cast<char>(0x80 | static_cast<unsigned char>(payload.size())));
        frame.append(reinterpret_cast<const char*>(mask), 4);
        std::string masked{payload};
        sh::ws_unmask(masked.data(), masked.size(), mask);
        frame.append(masked);
        return frame;
    };

    SECTION("a text frame is echoed") {
        RawClient client{ctx};
        handshake(client);
        client.send(masked_frame(sh::WsOpcode::Text, "hello ws"));
        REQUIRE(client.pump_until([&] { return client.received().find("hello ws") != std::string::npos; },
                                  std::chrono::seconds(3)));
        // The server's frame is unmasked: FIN|Text then the payload length.
        const std::string_view frame = after_head(client.received());
        REQUIRE(frame.size() >= 2);
        CHECK(static_cast<unsigned char>(frame[0]) == 0x81);
        CHECK(static_cast<unsigned char>(frame[1]) == 8);  // no mask bit
        CHECK(frame.substr(2, 8) == "hello ws");
        client.close();
    }
    SECTION("fragments are reassembled into one echoed message") {
        RawClient client{ctx};
        handshake(client);
        client.send(masked_frame(sh::WsOpcode::Text, "frag-", /*fin=*/false));
        client.send(masked_frame(sh::WsOpcode::Continuation, "ments", /*fin=*/true));
        REQUIRE(client.pump_until([&] { return client.received().find("frag-ments") != std::string::npos; },
                                  std::chrono::seconds(3)));
        CHECK(client.received().find("frag-ments") != std::string::npos);
        client.close();
    }
    SECTION("a Ping is answered with a Pong") {
        RawClient client{ctx};
        handshake(client);
        client.send(masked_frame(sh::WsOpcode::Ping, "ping!"));
        REQUIRE(client.pump_until([&] { return after_head(client.received()).find("ping!") != std::string::npos; },
                                  std::chrono::seconds(3)));
        const std::string_view frame = after_head(client.received());
        REQUIRE(frame.size() >= 2);
        CHECK(static_cast<unsigned char>(frame[0]) == 0x8A);  // FIN | Pong
        client.close();
    }
    SECTION("a Close is answered with a Close") {
        RawClient client{ctx};
        handshake(client);
        client.send(masked_frame(sh::WsOpcode::Close, sh::ws_close_payload(1000)));
        const bool delivered =
            client.pump_until([&] { return !after_head(client.received()).empty(); }, std::chrono::seconds(3));
        REQUIRE(delivered);
        const std::string_view frame = after_head(client.received());
        REQUIRE(frame.size() >= 4);
        CHECK(static_cast<unsigned char>(frame[0]) == 0x88);  // FIN | Close
        client.close();
    }
}

// --- TLS ---------------------------------------------------------------------

TEST_CASE("regression/tls: mutual TLS requires a client certificate", "[regression][tls]") {
    tls_server();
    asio::io_context ctx;

    // A client that presents no certificate must be refused by the handshake.
    asio::ssl::context client_ctx{asio::ssl::context::tlsv13_client};
    client_ctx.set_verify_mode(asio::ssl::verify_none);
    auto stream = std::make_shared<asio::ssl::stream<asio::ip::tcp::socket>>(ctx, client_ctx);

    sh::error_code ec;
    stream->next_layer().connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), kTlsPort), ec);
    REQUIRE_FALSE(ec);
    stream->async_handshake(asio::ssl::stream_base::client,
                            [&](const sh::error_code& handshake_ec) { ec = handshake_ec; });
    ctx.restart();
    ctx.run_for(std::chrono::seconds(10));

    // TLS 1.3 lets the client finish its side before the peer's verdict arrives, so
    // the rejection surfaces on the first application I/O (an alert, or a close).
    if (!ec) {
        std::string probe = "GET /world HTTP/1.1\r\nHost: x\r\n\r\n";
        stream->async_write_some(asio::buffer(probe),
                                 [&](const sh::error_code& write_ec, std::size_t) { ec = write_ec; });
        ctx.restart();
        ctx.run_for(std::chrono::seconds(10));
    }
    if (!ec) {
        // Reading forces the peer's verdict (an alert, or a bare close) to land.
        std::array<char, 64> buf{};
        stream->async_read_some(asio::buffer(buf), [&](const sh::error_code& read_ec, std::size_t) { ec = read_ec; });
        ctx.restart();
        ctx.run_for(std::chrono::seconds(10));
    }
    CHECK(ec);  // the server demanded a certificate we did not have
    sh::error_code ignored;
    stream->next_layer().close(ignored);
}

TEST_CASE("regression/tls: a TLS 1.2-only client is refused", "[regression][tls]") {
    tls_server();
    asio::io_context ctx;

    // The library's server context is TLS 1.3-only, by its context flavour.
    asio::ssl::context client_ctx{asio::ssl::context::tls_client};
    client_ctx.set_options(asio::ssl::context::no_tlsv1_3);
    client_ctx.set_verify_mode(asio::ssl::verify_none);
    client_ctx.use_certificate_chain_file("./test/tls_certificates/client_cert.pem");
    client_ctx.use_private_key_file("./test/tls_certificates/client_key.pem", asio::ssl::context::pem);
    auto stream = std::make_shared<asio::ssl::stream<asio::ip::tcp::socket>>(ctx, client_ctx);

    sh::error_code ec;
    stream->next_layer().connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), kTlsPort), ec);
    REQUIRE_FALSE(ec);
    stream->async_handshake(asio::ssl::stream_base::client,
                            [&](const sh::error_code& handshake_ec) { ec = handshake_ec; });
    ctx.restart();
    ctx.run_for(std::chrono::seconds(10));
    CHECK(ec);  // no version overlap
    sh::error_code ignored;
    stream->next_layer().close(ignored);
}

// --- accept topology ---------------------------------------------------------
//
// The endpoint is a single value and reuse_port turns it into one acceptor per
// worker context. Both halves are observable from a client: every worker must
// land on the *same* port (the probe resolves it once — re-binding a configured
// port 0 would scatter the workers across different ephemeral ports, and only
// the one behind `port()` would ever be reached), and the accept loops must
// actually run.

TEST_CASE("regression/server: reuse_port binds one acceptor per worker on one port",
          "[regression][server]") {
    sh::ServerConfig cfg;
    cfg.listen = sh::InetAddress{"127.0.0.1", 0, false};  // 0: the probe picks the port
    cfg.worker_threads = 4;
    cfg.reuse_port = true;
    sh::Server server{cfg};
    register_routes(server);
    REQUIRE(server.start());

    const auto port = server.port();
    REQUIRE(port != 0);
    // This case is about the fan-out, so make sure it happened: a platform that
    // rejects SO_REUSEPORT falls back to a single acceptor, and everything
    // below would then pass for the wrong reason.
#ifdef SO_REUSEPORT
    CHECK(server.acceptor_count() == 4);
#endif

    // Four connections held open at once, so the kernel's hash has to choose
    // among the fanned-out sockets rather than reusing one. A socket bound to a
    // stray port, or an accept loop that never started, leaves its share of
    // these unanswered.
    std::vector<std::unique_ptr<asio::io_context>> contexts;
    std::vector<std::unique_ptr<RawClient>> clients;
    for (int i = 0; i < 4; ++i) {
        contexts.push_back(std::make_unique<asio::io_context>());
        clients.push_back(std::make_unique<RawClient>(*contexts.back()));
        REQUIRE(clients.back()->connect(port));
        clients.back()->send("GET /world HTTP/1.1\r\nHost: x\r\n\r\n");
    }
    for (auto& client : clients) {
        CHECK(client->wait_for("hello"));
    }
    for (auto& client : clients) {
        client->close();
    }
}

// A single worker leaves the fan-out with nothing to do, and the one acceptor
// it bound is pinned to the only context — so no connection may be lost to a
// hand-off that no longer happens.
TEST_CASE("regression/server: reuse_port with a single worker still serves", "[regression][server]") {
    sh::ServerConfig cfg;
    cfg.listen = sh::InetAddress{"127.0.0.1", 0, false};
    cfg.worker_threads = 1;
    cfg.reuse_port = true;
    sh::Server server{cfg};
    register_routes(server);
    REQUIRE(server.start());
    CHECK(server.acceptor_count() == 1);  // nothing to fan out to

    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(server.port()));
    client.send("GET /world HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(client.wait_for("hello"));
    client.close();
}

// An IPv6 listener with v6_only off is dual-stack, so it answers IPv4 clients
// too — one endpoint covering both families instead of two listeners. Where
// IPv6 is unavailable the probe falls back to IPv4 rather than refusing to
// serve, so this expectation holds on either kind of host.
TEST_CASE("regression/server: an IPv6 listener serves IPv4 clients", "[regression][server]") {
    sh::ServerConfig cfg;
    cfg.listen = sh::InetAddress{"::", 0, false};
    cfg.worker_threads = 2;
    sh::Server server{cfg};
    register_routes(server);
    REQUIRE(server.start());
    REQUIRE(server.port() != 0);

    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(server.port()));
    client.send("GET /world HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(client.wait_for("hello"));
    client.close();
}

// A customization hook must not be able to turn client-certificate verification
// off by accident. It is applied *before* the security policy, not instead of it:
// the hook used to replace the whole policy branch, so a caller adding a cipher
// list silently got verify_none with cfg.mutual == true.
TEST_CASE("regression/tls: a setup hook does not disable mutual TLS", "[regression][tls]") {
    sh::ServerConfig cfg = make_config(
        0, sh::TlsConfig{
               .cert_chain_file = "./test/tls_certificates/server_cert.pem",
               .private_key_file = "./test/tls_certificates/server_key.pem",
               .mutual = true,
               .ca_file = "./test/tls_certificates/ca_cert.pem",
               // Something innocuous — the kind of thing this hook exists for.
               .setup = [](asio::ssl::context& ctx) { ctx.set_options(asio::ssl::context::default_workarounds); },
           });
    sh::Server server{cfg};
    register_routes(server);
    REQUIRE(server.start());

    // Sends a request and reports whether a *response* came back. The distinction
    // matters: "the read failed" is also what an idle timeout looks like, so a
    // bare error check would pass even with verification switched off — and did,
    // until this was written as a comparison.
    auto ask = [&](bool with_certificate) {
        asio::io_context ctx;
        asio::ssl::context client_ctx{asio::ssl::context::tls_client};
        client_ctx.set_verify_mode(asio::ssl::verify_none);
        if (with_certificate) {
            client_ctx.use_certificate_chain_file("./test/tls_certificates/client_cert.pem");
            client_ctx.use_private_key_file("./test/tls_certificates/client_key.pem", asio::ssl::context::pem);
        }
        auto stream = std::make_shared<asio::ssl::stream<asio::ip::tcp::socket>>(ctx, client_ctx);

        std::string seen;
        sh::error_code ec;
        stream->next_layer().connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), server.port()), ec);
        if (ec) return false;
        stream->async_handshake(asio::ssl::stream_base::client, [&](const sh::error_code& e) { ec = e; });
        ctx.restart();
        ctx.run_for(std::chrono::seconds(5));
        if (ec) return false;

        const std::string request = "GET /world HTTP/1.1\r\nHost: x\r\n\r\n";
        asio::async_write(*stream, asio::buffer(request), [&](const sh::error_code& e, std::size_t) { ec = e; });
        ctx.restart();
        ctx.run_for(std::chrono::seconds(5));
        if (ec) return false;

        std::array<char, 512> buf{};
        stream->async_read_some(asio::buffer(buf), [&](const sh::error_code& e, std::size_t n) {
            ec = e;
            if (!e) seen.assign(buf.data(), n);
        });
        ctx.restart();
        ctx.run_for(std::chrono::seconds(5));
        stream->next_layer().close(ec);
        return seen.find("hello") != std::string::npos;
    };

    // The control first: with a certificate the same path serves normally, so a
    // failure below cannot be blamed on a broken listener.
    CHECK(ask(/*with_certificate=*/true));
    // And without one it must not: the hook did not get to turn verification off.
    CHECK_FALSE(ask(/*with_certificate=*/false));

    server.stop();
}

// The writer sets Content-Length itself, so a handler that set one must have it
// *replaced*. Two conflicting Content-Length fields on one response is a
// response-splitting vector for any intermediary — and this framework is one.
TEST_CASE("regression/h1: a handler's own Content-Length is replaced, not duplicated", "[regression][h1]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));
    client.send("GET /clash HTTP/1.1\r\nHost: x\r\n\r\n");
    REQUIRE(client.wait_head());

    const std::string& head = client.received();
    std::size_t count = 0;
    for (auto at = head.find("content-length"); at != std::string::npos; at = head.find("content-length", at + 1)) {
        ++count;
    }
    CHECK(count == 1);
    CHECK(head.find("content-length: 2\r\n") != std::string::npos);  // "hi", not the handler's 999
    client.close();
}

// A response field carrying CR/LF is response splitting: it splices a field of the
// handler's choosing into the head. HTTP/2 already refuses the stream for it; the
// HTTP/1.1 writer used to write it verbatim, so the same handler was safe on one
// protocol and exploitable on the other.
TEST_CASE("regression/h1: a response field with CR/LF is refused, not spliced", "[regression][h1]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));
    client.send("GET /inject HTTP/1.1\r\nHost: x\r\n\r\n");
    client.pump(std::chrono::milliseconds(200));
    // Neither the injected field nor a well-formed head: the response is withheld
    // rather than emitted with a head the handler did not intend.
    CHECK(client.received().find("injected") == std::string::npos);
    CHECK(client.received().find("200 OK") == std::string::npos);
    client.close();
}

// RFC 9112 §7.1 requires CRLF after each chunk's data. Accepting any two bytes
// there is the lenient half of a request-smuggling split with a CRLF-strict
// front-end, and a proxy is exactly where that disagreement gets exploited.
TEST_CASE("regression/h1: a chunk trailer that is not CRLF is not accepted", "[regression][h1]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));
    client.send("POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhelloXY");
    REQUIRE(client.wait_head());
    // /echo reports what it could read, so this is what separates the two
    // behaviours: treating "XY" as the trailer yields "len=5:hello", while
    // rejecting it leaves the body unreadable. The engine's own response is a 200
    // either way — the route decides that — so the body is the observable.
    CHECK(client.received().find("len=5:hello") == std::string::npos);

    // A malformed chunk leaves the stream position unknown, so the connection
    // must not be reused for a following request on it.
    client.send("GET /world HTTP/1.1\r\nHost: x\r\n\r\n");
    client.pump(std::chrono::milliseconds(200));
    CHECK(client.received().find("hello from") == std::string::npos);
    client.close();
}

// "Disabled" has to mean "no deadline", not "close immediately". Racing a no-op
// deadline against the detection read made `a || b` complete at once, cancelling
// the read before a byte arrived — so every connection was accepted and dropped.
TEST_CASE("regression/server: idle_timeout = 0 disables the deadline instead of closing at once",
          "[regression][server]") {
    auto cfg = make_config(0, std::nullopt);
    cfg.limits.idle_timeout = std::chrono::seconds(0);
    sh::Server server{cfg};
    register_routes(server);
    REQUIRE(server.start());

    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(server.port()));
    client.send("GET /world HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(client.wait_for("hello"));
    client.close();
    server.stop();
}

// The UNIX-domain listener shares everything with the TCP path except the
// endpoint, so what is worth checking is what actually differs: it binds to a
// path, answers over it, and can be restarted on the same path.
TEST_CASE("regression/server: a UNIX-domain listener serves over its socket file", "[regression][server]") {
    const std::string path = "/tmp/simple_http_regression.sock";
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    sh::ServerConfig cfg;
    cfg.listen = sh::UnixAddress{path};
    cfg.worker_threads = 2;
    sh::Server server{cfg};
    register_routes(server);
    REQUIRE(server.start());

    asio::io_context ctx;
    asio::local::stream_protocol::socket socket{ctx};
    sh::error_code ec;
    socket.connect(asio::local::stream_protocol::endpoint{path}, ec);
    REQUIRE_FALSE(ec);

    const std::string request = "GET /world HTTP/1.1\r\nHost: x\r\n\r\n";
    asio::write(socket, asio::buffer(request), ec);
    REQUIRE_FALSE(ec);

    std::string response;
    std::array<char, 4096> buf{};
    while (response.find("hello") == std::string::npos) {
        const auto n = socket.read_some(asio::buffer(buf), ec);
        if (ec || n == 0) break;
        response.append(buf.data(), n);
    }
    CHECK(response.find("200 OK") != std::string::npos);
    CHECK(response.find("hello") != std::string::npos);
    socket.close(ec);

    // Restarting on the same path has to work: the socket file the first
    // listener left behind is unlinked before the second binds. Without that
    // step every restart fails with EADDRINUSE.
    server.stop();
    sh::Server restarted{cfg};
    register_routes(restarted);
    CHECK(restarted.start());
    restarted.stop();
    std::filesystem::remove(path, ignored);
}

// TLS over a UNIX-domain socket. The transport is templated on the socket type,
// so this is nominally the same handshake as over TCP — but "should work" is not
// "does work", and nothing else in the suite drives an encrypted AF_UNIX
// connection (the plaintext UNIX case above is the only other one).
TEST_CASE("regression/server: TLS over a UNIX-domain socket", "[regression][server][tls]") {
    const std::string path = "/tmp/simple_http_regression_tls.sock";
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    sh::ServerConfig cfg;
    cfg.listen = sh::UnixAddress{path};
    cfg.worker_threads = 2;
    cfg.tls = sh::TlsConfig{
        .cert_chain_file = "./test/tls_certificates/server_cert.pem",
        .private_key_file = "./test/tls_certificates/server_key.pem",
        .mutual = true,
        .ca_file = "./test/tls_certificates/ca_cert.pem",
    };
    sh::Server server{cfg};
    register_routes(server);
    REQUIRE(server.start());

    asio::io_context ctx;
    asio::ssl::context client_ctx{asio::ssl::context::tls_client};
    client_ctx.set_verify_mode(asio::ssl::verify_none);
    client_ctx.use_certificate_chain_file("./test/tls_certificates/client_cert.pem");
    client_ctx.use_private_key_file("./test/tls_certificates/client_key.pem", asio::ssl::context::pem);

    auto stream = std::make_shared<asio::ssl::stream<asio::local::stream_protocol::socket>>(ctx, client_ctx);
    sh::error_code ec;
    stream->next_layer().connect(asio::local::stream_protocol::endpoint{path}, ec);
    REQUIRE_FALSE(ec);

    stream->async_handshake(asio::ssl::stream_base::client, [&](const sh::error_code& e) { ec = e; });
    ctx.restart();
    ctx.run_for(std::chrono::seconds(10));
    REQUIRE_FALSE(ec);

    const std::string request = "GET /world HTTP/1.1\r\nHost: x\r\n\r\n";
    asio::write(*stream, asio::buffer(request), ec);
    REQUIRE_FALSE(ec);

    std::string response;
    std::array<char, 4096> buf{};
    while (response.find("hello") == std::string::npos) {
        const auto n = stream->read_some(asio::buffer(buf), ec);
        if (ec || n == 0) break;
        response.append(buf.data(), n);
    }
    CHECK(response.find("200 OK") != std::string::npos);
    stream->next_layer().close(ec);

    server.stop();
    std::filesystem::remove(path, ignored);
}
