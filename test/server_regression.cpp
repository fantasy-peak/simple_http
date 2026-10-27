// Server regression suite: the bytes a well-behaved client will not send.
//
// Every case here drives an in-process server over a raw TCP socket — malformed
// request lines, framing conflicts, oversized heads and bodies, HTTP/2 frames
// in the wrong state, WebSocket protocol violations — and asserts what comes
// back. The library's own client is deliberately well-behaved, so it cannot
// produce these; a socket can.
//
//   xmake build regression && xmake run regression

#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <openssl/ssl.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "simple_http.h"

namespace asio = boost::asio;
namespace sh = simple_http;

namespace {

constexpr std::uint16_t kPlainPort = 27920;
constexpr std::uint16_t kTlsPort = 27921;
constexpr std::uint16_t kShutdownPort = 27930;
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

// --- a document root for the static cases -----------------------------------
//
// Process-wide rather than per-test, because the servers below are singletons
// too: built once, alive for the whole binary. The tree is small and fixed so
// every case can state its expectation in bytes rather than in derivations.

const std::filesystem::path &static_root() {
    static const std::filesystem::path root = [] {
        const auto dir = std::filesystem::temp_directory_path() / "simple_http_regression_static";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir);
        auto write = [&](const char *rel, std::string_view body) {
            std::filesystem::create_directories(dir / std::filesystem::path{rel}.parent_path());
            std::ofstream out(dir / rel, std::ios::binary);
            out.write(body.data(), static_cast<std::streamsize>(body.size()));
        };
        write("index.html", "<html>index</html>");
        write("404.html", "<html>missing</html>");
        write("app.js", "console.log('app');");
        write("app.js.br", "BR:console.log('app');");
        write("assets/chunk-abc.js", "console.log('chunk');");
        return dir;
    }();
    return root;
}

std::shared_ptr<sh::StaticFiles> static_site() {
    static const std::shared_ptr<sh::StaticFiles> site = [] {
        sh::StaticFilesConfig cfg;
        cfg.table.root = static_root().string();
        cfg.table.immutable_prefixes = {"/assets/"};
        auto s = std::make_shared<sh::StaticFiles>(std::move(cfg));
        std::string error;
        if (!s->load(error)) {
            throw std::runtime_error("static site failed to load: " + error);
        }
        return s;
    }();
    return site;
}

void register_routes(sh::Server &server) {
    // Registered first but consulted last among the routes: the static stage sits
    // between the regex routes and the fallback, so every route below still wins.
    server.static_files(static_site());

    server.route(sh::any_methods, "/world", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).content_type("text/plain").send("hello");
    });
    // Echoes the body length and its bytes, so framing is observable.
    server.route(sh::any_methods, "/echo", [](sh::RequestPtr req, sh::ResponsePtr res) -> asio::awaitable<void> {
        auto body = co_await req->body().read_all();
        co_await res->status(200).send("len=" + std::to_string(body ? body->size() : 0) + ":" + (body ? *body : ""));
    });
    server.route(sh::any_methods, "/empty", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(204).send_bodyless();
    });
    server.route(sh::any_methods, "/stream", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        (void)co_await res->status(200).content_type("text/plain").begin();
        (void)co_await res->write("one-");
        (void)co_await res->finish("two");
    });
    server.route(sh::any_methods, "/big", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).send(std::string(9000, 'x'));
    });
    // Answers late and never reads the body: the stream stays open with an
    // unconsumed body, so nothing replenishes the flow-control window.
    server.route(sh::any_methods, "/slow", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        asio::steady_timer timer{co_await asio::this_coro::executor};
        timer.expires_after(std::chrono::milliseconds(1500));
        co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        co_await res->status(200).send("late");
    });
    // A field carrying CR/LF is response splitting; the response must not go out.
    server.route(sh::any_methods, "/inject", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        (void)co_await res->status(200).header("x-bad", "a\r\ninjected: 1").send("hi");
    });
    // A handler that sets its own Content-Length: the writer owns that field and
    // must replace it, not append a second one.
    server.route(sh::any_methods, "/clash", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).header("content-length", "999").send("hi");
    });
    // Method-aware routes: these pin the router's 405 / Allow / auto-OPTIONS
    // behaviour over real sockets, just like the static stage's 405 test does —
    // but for routes. GET implies HEAD here, so HEAD /getonly is a 200.
    server.route({sh::Method::Get}, "/method/getonly",
                 [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
                     co_await res->status(200).send("getonly");
                 });
    server.route({sh::Method::Post}, "/method/postonly",
                 [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
                     co_await res->status(200).send("postonly");
                 });
    server.route(
        {sh::Method::Put, sh::Method::Delete}, "/method/multi",
        [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> { co_await res->status(200).send("multi"); });
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
// Server is neither copyable nor movable, so it lives behind a unique_ptr
// (whose destructor stops it at exit).
sh::Server &plain_server() {
    static std::unique_ptr<sh::Server> server = [] {
        auto s = std::make_unique<sh::Server>(make_config(kPlainPort, std::nullopt));
        register_routes(*s);
        s->start();
        return s;
    }();
    return *server;
}

sh::Server &tls_server() {
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

// A TCP peer that sends exactly the bytes it is given and accumulates
// everything it receives, with the test pumping the shared io_context. Reads
// run as one detached coroutine per client (like the server's own engines do).
class RawClient {
  public:
    // The read coroutine outlives the RawClient itself (a test may drop the
    // client while a read is pending), so everything it touches lives in a shared
    // state.
    struct State {
        explicit State(asio::io_context &ctx) : socket(std::make_shared<asio::ip::tcp::socket>(ctx)) {}

        std::shared_ptr<asio::ip::tcp::socket> socket;
        std::string received;
        bool eof{false};
    };

    explicit RawClient(asio::io_context &ctx) : m_state(std::make_shared<State>(ctx)), m_ctx(ctx) {}

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
    template <typename Pred> bool pump_until(Pred done, std::chrono::milliseconds timeout) {
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

    const std::string &received() const { return m_state->received; }

    bool eof() const { return m_state->eof; }

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
                    state->received.append(reinterpret_cast<const char *>(buf.data()), n);
                }
            },
            asio::detached);
    }

    std::shared_ptr<State> m_state;
    asio::io_context &m_ctx;
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
        for (auto &c : out)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    };
    return lowered(head).find(lowered(field)) != std::string::npos;
}

// The value of a response-head field, or nullopt when it is absent. Same
// case-insensitive matching as head_has.
std::optional<std::string> head_value(std::string_view response, std::string_view name) {
    const auto head_end = response.find("\r\n\r\n");
    const std::string_view head = response.substr(0, head_end);
    auto lowered = [](std::string_view in) {
        std::string out{in};
        for (auto &c : out)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    };
    const std::string wanted = lowered(name);
    std::size_t pos = 0;
    while (pos < head.size()) {
        auto eol = head.find("\r\n", pos);
        if (eol == std::string_view::npos)
            eol = head.size();
        const std::string_view line = head.substr(pos, eol - pos);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos && lowered(line.substr(0, colon)) == wanted) {
            std::string_view value = line.substr(colon + 1);
            while (!value.empty() && value.front() == ' ')
                value.remove_prefix(1);
            return std::string{value};
        }
        pos = eol + 2;
    }
    return std::nullopt;
}

// Everything after the response head.
std::string_view after_head(std::string_view response) {
    const auto head_end = response.find("\r\n\r\n");
    if (head_end == std::string_view::npos)
        return {};
    return response.substr(head_end + 4);
}

std::string body_of(std::string_view response) { return std::string{after_head(response)}; }

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

std::string h2_frame(sh::codec::H2FrameType type, std::uint8_t flags, std::uint32_t stream_id,
                     std::string_view payload) {
    std::string out;
    sh::codec::serialize_frame_header(out, static_cast<std::uint32_t>(payload.size()), static_cast<std::uint8_t>(type),
                                      flags, stream_id);
    out.append(payload);
    return out;
}

std::string h2_request_headers(std::string_view method, std::string_view path, std::uint32_t stream_id = 1,
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

std::optional<H2Frame> find_frame(const std::vector<H2Frame> &frames, sh::codec::H2FrameType type) {
    for (const auto &frame : frames) {
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

std::string header_value(const std::vector<sh::codec::HpackHeader> &fields, std::string_view name) {
    for (const auto &field : fields) {
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

} // namespace

// --- HTTP/1.x ----------------------------------------------------------------

TEST_CASE("regression/h1: malformed request heads are rejected with 400", "[regression][h1]") {
    plain_server();
    asio::io_context ctx;

    const std::array<std::pair<std::string_view, const char *>, 4> cases = {{
        {"GET/ HTTP/1.1\r\nHost: x\r\n\r\n", "no space after the method"},
        {"GET / HTTP/2.0\r\n\r\n", "an unknown version"},
        {"GET / HTTP/1.1\r\nHost: x\r\nBroken\r\n\r\n", "a header line without a colon"},
        {"GET / HTTP/1.1\r\nHost: x\r\n folded\r\n\r\n", "an obs-fold continuation"},
    }};

    for (const auto &[request, what] : cases) {
        RawClient client{ctx};
        REQUIRE(client.connect(kPlainPort));
        client.send(request);
        REQUIRE(client.wait_head());
        INFO("case: " << what);
        CHECK(status_of(client.received()) == 400);
        CHECK(client.wait_eof()); // a malformed request closes the connection
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

TEST_CASE("regression/h1: Content-Length with Transfer-Encoding is refused", "[regression][h1]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    // RFC 9112 §6.3 allows TE to override CL, but a message carrying both is
    // the classic CL.TE request-smuggling shape: a front-end that framed by CL
    // and this server framing by TE disagree about where the next request
    // starts. Refusing the ambiguity (nginx does the same) is the only answer
    // that cannot be desynchronized.
    client.send("POST /echo HTTP/1.1\r\nHost: x\r\ncontent-length: "
                "5\r\ntransfer-encoding: chunked\r\n\r\n"
                "4\r\nabcd\r\n0\r\n\r\n");
    REQUIRE(client.wait_head());
    CHECK(status_of(client.received()) == 400);
    CHECK(client.wait_eof());
    client.close();
}

TEST_CASE("regression/h1: unsupported transfer codings are refused with 501", "[regression][h1]") {
    plain_server();
    asio::io_context ctx;

    SECTION("a token that merely contains 'chunked' is not chunked") {
        RawClient client{ctx};
        REQUIRE(client.connect(kPlainPort));
        client.send("POST /echo HTTP/1.1\r\nHost: x\r\ntransfer-encoding: notchunked\r\n"
                    "content-length: 4\r\n\r\nabcd");
        REQUIRE(client.wait_head());
        CHECK(status_of(client.received()) == 501);
        CHECK(client.wait_eof());
        client.close();
    }
    SECTION("a coding we do not decode is not silently passed through") {
        RawClient client{ctx};
        REQUIRE(client.connect(kPlainPort));
        client.send("POST /echo HTTP/1.1\r\nHost: x\r\ntransfer-encoding: gzip, chunked\r\n\r\n"
                    "4\r\nabcd\r\n0\r\n\r\n");
        REQUIRE(client.wait_head());
        CHECK(status_of(client.received()) == 501);
        CHECK(client.wait_eof());
        client.close();
    }
    SECTION("chunked must be the final coding") {
        RawClient client{ctx};
        REQUIRE(client.connect(kPlainPort));
        client.send("POST /echo HTTP/1.1\r\nHost: x\r\ntransfer-encoding: chunked, gzip\r\n\r\n"
                    "4\r\nabcd\r\n0\r\n\r\n");
        REQUIRE(client.wait_head());
        CHECK(status_of(client.received()) == 400);
        CHECK(client.wait_eof());
        client.close();
    }
}

TEST_CASE("regression/h1: Connection is parsed as a token list", "[regression][h1]") {
    plain_server();
    asio::io_context ctx;

    SECTION("a value containing 'close' is not the close token") {
        RawClient client{ctx};
        REQUIRE(client.connect(kPlainPort));
        client.send("GET /world HTTP/1.1\r\nHost: x\r\nConnection: not-close\r\n\r\n");
        REQUIRE(client.wait_for("hello"));

        // The connection must still be usable for a second request.
        client.send("GET /world HTTP/1.1\r\nHost: x\r\n\r\n");
        CHECK(client.pump_until(
            [&] {
                const auto &r = client.received();
                const auto first = r.find("hello");
                return first != std::string::npos && r.find("hello", first + 1) != std::string::npos;
            },
            std::chrono::seconds(5)));
        client.close();
    }
    SECTION("a value containing 'keep-alive' is not the keep-alive token") {
        RawClient client{ctx};
        REQUIRE(client.connect(kPlainPort));
        client.send("GET /world HTTP/1.0\r\nHost: x\r\nConnection: xkeep-alive\r\n\r\n");
        REQUIRE(client.wait_for("hello"));
        CHECK(client.wait_eof());
        client.close();
    }
}

TEST_CASE("regression/h1: a malformed chunk size is answered then dropped", "[regression][h1]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    // The handler is dispatched as soon as the head is parsed, so it answers
    // 200; the broken body then ends the connection instead of hanging.
    client.send("POST /echo HTTP/1.1\r\nHost: x\r\ntransfer-encoding: "
                "chunked\r\n\r\nzz\r\nbody\r\n0\r\n\r\n");
    REQUIRE(client.wait_head());
    CHECK(client.wait_eof());
    client.close();
}

TEST_CASE("regression/h1: a chunk terminator error consumed by the handler still closes", "[regression][h1]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    // The chunk data arrives, but the two bytes after it are not CRLF. /echo
    // reads the body, consumes the framing error itself and answers 200. The
    // engine must still close instead of parsing the pipelined GET that follows
    // as a new request (which used to yield a second response on one connection).
    client.send("POST /echo HTTP/1.1\r\nHost: x\r\ntransfer-encoding: chunked\r\n\r\n"
                "5\r\nhelloXX"
                "GET /world HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    REQUIRE(client.wait_head());
    CHECK(client.wait_eof());
    const std::string got = client.received();
    CHECK(got.find("hello") == std::string::npos); // /world's body was not served
    std::size_t status_lines = 0;
    for (std::size_t pos = 0; (pos = got.find("HTTP/1.1 ", pos)) != std::string::npos; pos += 9)
        ++status_lines;
    CHECK(status_lines == 1);
    client.close();
}

TEST_CASE("regression/h1: chunked trailers are consumed", "[regression][h1]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    client.send("POST /echo HTTP/1.1\r\nHost: x\r\ntransfer-encoding: chunked\r\n\r\n"
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

    client.send("GET /echo HTTP/1.1\r\nHost: x\r\ncontent-length: 1\r\n\r\nA" // a body
                                                                              // exercies
                                                                              // the
                                                                              // pipelining
                                                                              // boundary
                "GET /world HTTP/1.1\r\nHost: x\r\n\r\n");
    REQUIRE(client.pump_until(
        [&] {
            const auto &r = client.received();
            return r.find("len=1:A") != std::string::npos && r.find("hello") != std::string::npos;
        },
        std::chrono::seconds(5)));

    const auto &received = client.received();
    CHECK(received.find("len=1:A") < received.find("hello")); // first request answered first
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
        CHECK(head_has(client.received(),
                       "content-length: 9000"));               // what a GET would produce…
        CHECK(body_of(client.received()).empty());             // …but no body follows the head
        client.send("GET /world HTTP/1.1\r\nHost: x\r\n\r\n"); // and the connection
                                                               // stays usable
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
    // limits.idle_timeout is 2s in this suite: a silent connection must be
    // reaped.
    CHECK(client.wait_eof(std::chrono::seconds(6)));
    client.close();
}

TEST_CASE("regression/h1: idle_timeout=0 disables the watchdog", "[regression][h1]") {
    // net/connection.h treats <= 0 as "no deadline" while detecting and
    // handshaking; the engines must agree, or a config that disables the probe
    // timeout closes every connection the moment it is accepted.
    constexpr std::uint16_t port = 27931;
    sh::ServerConfig cfg = make_config(port, std::nullopt);
    cfg.limits.idle_timeout = std::chrono::seconds(0);
    sh::Server server{cfg};
    register_routes(server);
    REQUIRE(server.start().has_value());

    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(port));
    // A zero-length deadline fires as soon as the first exchange has been
    // processed (the engine is running by then); a disabled one keeps the
    // connection at a keep-alive boundary for the next request.
    client.send("GET /world HTTP/1.1\r\nHost: x\r\n\r\n");
    REQUIRE(client.wait_for("hello"));
    CHECK_FALSE(client.wait_eof(std::chrono::milliseconds(500)));
    client.send("GET /world HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    const auto hellos = [&] {
        std::size_t n = 0;
        for (std::size_t pos = 0; (pos = client.received().find("hello", pos)) != std::string::npos; pos += 5)
            ++n;
        return n;
    };
    CHECK(client.pump_until([&] { return hellos() >= 2; }, std::chrono::seconds(5)));
    client.close();
    server.stop();
}

TEST_CASE("regression/h1: Expect: 100-continue is not answered by default", "[regression][h1]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    // The engine does not send 100 Continue on its own (a handler may, via
    // send_continue()). A client that sends the body anyway must be served.
    client.send("POST /echo HTTP/1.1\r\nHost: x\r\nexpect: "
                "100-continue\r\ncontent-length: 2\r\n\r\n");
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
            for (const auto &frame : frames) {
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
    for (const auto &frame : frames) {
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
            for (const auto &frame : parse_frames(client.received())) {
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
    CHECK(ping->payload == payload); // echoed verbatim
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
        client.send(h2_frame(sh::codec::H2FrameType::Headers,
                             sh::codec::H2_FLAG_END_HEADERS | sh::codec::H2_FLAG_END_STREAM, 1, block));
        client.pump_until(
            [&] {
                return find_frame(parse_frames(client.received()), sh::codec::H2FrameType::RstStream).has_value() ||
                       client.eof();
            },
            std::chrono::seconds(3));
        const auto reset = find_frame(parse_frames(client.received()), sh::codec::H2FrameType::RstStream);
        REQUIRE(reset.has_value()); // stream error, not a connection error
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
    // GOAWAY(FLOW_CONTROL_ERROR). A peer cannot enlarge our receive window for
    // us.
    client.send(h2_request_headers("POST", "/slow", /*stream_id=*/1,
                                   /*end_stream=*/false));
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
    client.send(h2_request_headers("POST", "/slow", /*stream_id=*/1,
                                   /*end_stream=*/false));
    client.send(h2_request_headers("POST", "/slow", /*stream_id=*/3,
                                   /*end_stream=*/false));
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
    client.send("GET /world HTTP/1.1\r\nhost: 127.0.0.1\r\nconnection: Upgrade, "
                "HTTP2-Settings\r\n"
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
    CHECK(headers->header.stream_id == 1); // the upgraded request's own stream
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
            for (const auto &frame : all) {
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

    client.send("GET /chat HTTP/1.1\r\nhost: 127.0.0.1\r\nconnection: "
                "Upgrade\r\nupgrade: websocket\r\n"
                "sec-websocket-version: 13\r\nsec-websocket-key: "
                "dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
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

    auto handshake = [&](RawClient &client) {
        REQUIRE(client.connect(kPlainPort));
        client.send("GET /chat HTTP/1.1\r\nhost: 127.0.0.1\r\nconnection: "
                    "Upgrade\r\nupgrade: websocket\r\n"
                    "sec-websocket-version: 13\r\nsec-websocket-key: "
                    "dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
        REQUIRE(client.wait_head());
        REQUIRE(status_of(client.received()) == 101);
    };

    auto masked_frame = [](sh::WsOpcode opcode, std::string_view payload, bool fin = true) {
        const unsigned char mask[4] = {0x11, 0x22, 0x33, 0x44};
        std::string frame;
        frame.push_back(static_cast<char>((fin ? 0x80 : 0x00) | static_cast<unsigned char>(opcode)));
        frame.push_back(static_cast<char>(0x80 | static_cast<unsigned char>(payload.size())));
        frame.append(reinterpret_cast<const char *>(mask), 4);
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
        CHECK(static_cast<unsigned char>(frame[1]) == 8); // no mask bit
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
        CHECK(static_cast<unsigned char>(frame[0]) == 0x8A); // FIN | Pong
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
        CHECK(static_cast<unsigned char>(frame[0]) == 0x88); // FIN | Close
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
                            [&](const sh::error_code &handshake_ec) { ec = handshake_ec; });
    ctx.restart();
    ctx.run_for(std::chrono::seconds(10));

    // TLS 1.3 lets the client finish its side before the peer's verdict arrives,
    // so the rejection surfaces on the first application I/O (an alert, or a
    // close).
    if (!ec) {
        std::string probe = "GET /world HTTP/1.1\r\nHost: x\r\n\r\n";
        stream->async_write_some(asio::buffer(probe),
                                 [&](const sh::error_code &write_ec, std::size_t) { ec = write_ec; });
        ctx.restart();
        ctx.run_for(std::chrono::seconds(10));
    }
    if (!ec) {
        // Reading forces the peer's verdict (an alert, or a bare close) to land.
        std::array<char, 64> buf{};
        stream->async_read_some(asio::buffer(buf), [&](const sh::error_code &read_ec, std::size_t) { ec = read_ec; });
        ctx.restart();
        ctx.run_for(std::chrono::seconds(10));
    }
    CHECK(ec); // the server demanded a certificate we did not have
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
                            [&](const sh::error_code &handshake_ec) { ec = handshake_ec; });
    ctx.restart();
    ctx.run_for(std::chrono::seconds(10));
    CHECK(ec); // no version overlap
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

TEST_CASE("regression/server: reuse_port binds one acceptor per worker on one port", "[regression][server]") {
    sh::ServerConfig cfg;
    cfg.listen = sh::InetAddress{"127.0.0.1", 0, false}; // 0: the probe picks the port
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
    for (auto &client : clients) {
        CHECK(client->wait_for("hello"));
    }
    for (auto &client : clients) {
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
    CHECK(server.acceptor_count() == 1); // nothing to fan out to

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
// off by accident. It is applied *before* the security policy, not instead of
// it: the hook used to replace the whole policy branch, so a caller adding a
// cipher list silently got verify_none with cfg.mutual == true.
TEST_CASE("regression/tls: a setup hook does not disable mutual TLS", "[regression][tls]") {
    sh::ServerConfig cfg = make_config(
        0, sh::TlsConfig{
               .cert_chain_file = "./test/tls_certificates/server_cert.pem",
               .private_key_file = "./test/tls_certificates/server_key.pem",
               .mutual = true,
               .ca_file = "./test/tls_certificates/ca_cert.pem",
               // Something innocuous — the kind of thing this hook exists for.
               .setup = [](asio::ssl::context &ctx) { ctx.set_options(asio::ssl::context::default_workarounds); },
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
        if (ec)
            return false;
        stream->async_handshake(asio::ssl::stream_base::client, [&](const sh::error_code &e) { ec = e; });
        ctx.restart();
        ctx.run_for(std::chrono::seconds(5));
        if (ec)
            return false;

        const std::string request = "GET /world HTTP/1.1\r\nHost: x\r\n\r\n";
        asio::async_write(*stream, asio::buffer(request), [&](const sh::error_code &e, std::size_t) { ec = e; });
        ctx.restart();
        ctx.run_for(std::chrono::seconds(5));
        if (ec)
            return false;

        std::array<char, 512> buf{};
        stream->async_read_some(asio::buffer(buf), [&](const sh::error_code &e, std::size_t n) {
            ec = e;
            if (!e)
                seen.assign(buf.data(), n);
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

    const std::string &head = client.received();
    std::size_t count = 0;
    for (auto at = head.find("content-length"); at != std::string::npos; at = head.find("content-length", at + 1)) {
        ++count;
    }
    CHECK(count == 1);
    CHECK(head.find("content-length: 2\r\n") != std::string::npos); // "hi", not the handler's 999
    client.close();
}

// A response field carrying CR/LF is response splitting: it splices a field of
// the handler's choosing into the head. HTTP/2 already refuses the stream for
// it; the HTTP/1.1 writer used to write it verbatim, so the same handler was
// safe on one protocol and exploitable on the other.
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
    client.send("POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: "
                "chunked\r\n\r\n5\r\nhelloXY");
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
// deadline against the detection read made `a || b` complete at once,
// cancelling the read before a byte arrived — so every connection was accepted
// and dropped.
TEST_CASE("regression/server: idle_timeout = 0 disables the deadline instead of "
          "closing at once",
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
        if (ec || n == 0)
            break;
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

// stop() closes the listeners first, then lets a request that already reached
// the handler finish. Closing the transports immediately (the old behavior)
// turned every in-flight response into an empty reply.
TEST_CASE("regression/server: stop drains an in-flight request", "[regression][server]") {
    sh::ServerConfig cfg;
    cfg.listen = sh::InetAddress{"127.0.0.1", kShutdownPort, false};
    cfg.worker_threads = 2;
    cfg.limits.idle_timeout = std::chrono::seconds(5);
    cfg.shutdown_grace = std::chrono::seconds(2);
    sh::Server server{cfg};

    std::atomic<bool> started{false};
    server.route(sh::any_methods, "/slow-stop", [&](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        started.store(true, std::memory_order_release);
        asio::steady_timer timer{co_await asio::this_coro::executor};
        timer.expires_after(std::chrono::milliseconds(300));
        co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        co_await res->status(200).send("drained");
    });
    REQUIRE(server.start());

    std::string response;
    std::thread client([&] {
        asio::io_context io;
        asio::ip::tcp::socket socket{io};
        sh::error_code ec;
        socket.connect(asio::ip::tcp::endpoint{asio::ip::make_address("127.0.0.1"), kShutdownPort}, ec);
        if (ec)
            return;
        const std::string request = "GET /slow-stop HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        asio::write(socket, asio::buffer(request), ec);
        if (ec)
            return;
        std::array<char, 4096> buf{};
        for (;;) {
            const auto n = socket.read_some(asio::buffer(buf), ec);
            if (ec || n == 0)
                break;
            response.append(buf.data(), n);
        }
        socket.close(ec);
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!started.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(started.load(std::memory_order_acquire));
    CHECK(server.active_requests() == 1); // the handler is inside the dispatch chain

    const auto begin = std::chrono::steady_clock::now();
    server.stop();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin);
    client.join();

    CHECK(response.find("200 OK") != std::string::npos);
    CHECK(response.find("drained") != std::string::npos);
    CHECK(elapsed >= std::chrono::milliseconds(250)); // it waited for the handler
    CHECK(elapsed < std::chrono::seconds(2));         // ... and not for the full grace
    CHECK(server.active_requests() == 0);
}

// TLS over a UNIX-domain socket. The transport is templated on the socket type,
// so this is nominally the same handshake as over TCP — but "should work" is
// not "does work", and nothing else in the suite drives an encrypted AF_UNIX
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

    stream->async_handshake(asio::ssl::stream_base::client, [&](const sh::error_code &e) { ec = e; });
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
        if (ec || n == 0)
            break;
        response.append(buf.data(), n);
    }
    CHECK(response.find("200 OK") != std::string::npos);
    stream->next_layer().close(ec);

    server.stop();
    std::filesystem::remove(path, ignored);
}

// --- static files over a real socket -----------------------------------------
//
// The unit tests drive the same component through FakeResponseWriter, which
// sees headers as a collection. These see them as bytes on the wire, which is
// where the two things that component sets by hand actually matter: a length on
// a HEAD or a 416 (or the response is delimited by connection close instead),
// and the fact that HTTP/2 forwards our header block untouched while HTTP/1.1
// recomputes the length from the body.

TEST_CASE("regression/static: a GET is the file, byte for byte", "[regression][static]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    client.send("GET /app.js HTTP/1.1\r\nHost: x\r\n\r\n");
    REQUIRE(client.wait_head());
    CHECK(status_of(client.received()) == 200);
    CHECK(head_has(client.received(), "etag:"));
    CHECK(head_has(client.received(), "last-modified:"));

    const std::string body = body_of(client.received());
    CHECK(body == "console.log('app');");

    // The length the engine wrote must describe the body it wrote.
    auto cl = head_value(client.received(), "content-length");
    REQUIRE(cl.has_value());
    CHECK(*cl == std::to_string(body.size()));

    client.close();
}

TEST_CASE("regression/static: a HEAD carries the length a GET would have", "[regression][static]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    client.send("HEAD /app.js HTTP/1.1\r\nHost: x\r\n\r\n");
    REQUIRE(client.wait_head());
    CHECK(status_of(client.received()) == 200);

    auto cl = head_value(client.received(), "content-length");
    REQUIRE(cl.has_value());
    CHECK(*cl == std::to_string(std::string{"console.log('app');"}.size()));

    // And no body follows: give it a bounded window to arrive, then assert that
    // nothing did.
    client.pump(std::chrono::milliseconds(150));
    CHECK(body_of(client.received()).empty());

    client.close();
}

TEST_CASE("regression/static: a pre-compressed sibling is negotiated on the wire", "[regression][static]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    client.send("GET /app.js HTTP/1.1\r\nHost: x\r\nAccept-Encoding: br\r\n\r\n");
    REQUIRE(client.wait_head());
    CHECK(status_of(client.received()) == 200);
    CHECK(head_has(client.received(), "content-encoding: br"));

    // The bytes are the .br sibling's, delivered as-is: nothing re-compressed it.
    CHECK(body_of(client.received()) == "BR:console.log('app');");

    auto cl = head_value(client.received(), "content-length");
    REQUIRE(cl.has_value());
    CHECK(*cl == std::to_string(std::string{"BR:console.log('app');"}.size()));

    client.close();
}

TEST_CASE("regression/static: a range comes back as 206 with the right slice", "[regression][static]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    client.send("GET /app.js HTTP/1.1\r\nHost: x\r\nRange: bytes=0-4\r\n\r\n");
    REQUIRE(client.wait_head());
    CHECK(status_of(client.received()) == 206);
    CHECK(head_has(client.received(),
                   "content-range: bytes 0-4/" + std::to_string(std::string{"console.log('app');"}.size())));
    CHECK(body_of(client.received()) == "conso");
    CHECK(*head_value(client.received(), "content-length") == "5");

    client.close();
}

TEST_CASE("regression/static: a 416 does not wedge the connection", "[regression][static]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    client.send("GET /app.js HTTP/1.1\r\nHost: x\r\nRange: bytes=9999-\r\n\r\n");
    REQUIRE(client.wait_head());
    CHECK(status_of(client.received()) == 416);
    REQUIRE(head_value(client.received(), "content-length").has_value());
    CHECK(*head_value(client.received(), "content-length") == "0");
    CHECK(body_of(client.received()).empty());

    // This is the regression the length exists for. A 416 that omitted both
    // content-length and transfer-encoding is delimited by connection close in
    // HTTP/1.1, so the client would sit waiting for the idle watchdog instead of
    // reading a response that is already complete. If that were the case here,
    // this second request would never be answered.
    client.send("GET /app.js HTTP/1.1\r\nHost: x\r\n\r\n");
    REQUIRE(client.pump_until([&] { return client.received().find("console.log") != std::string::npos; },
                              std::chrono::seconds(5)));

    client.close();
}

TEST_CASE("regression/static: a conditional request is a 304 with no body", "[regression][static]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    // Learn the validator first.
    client.send("GET /app.js HTTP/1.1\r\nHost: x\r\n\r\n");
    REQUIRE(client.wait_head());
    auto etag = head_value(client.received(), "etag");
    REQUIRE(etag.has_value());
    client.close();

    RawClient second{ctx};
    REQUIRE(second.connect(kPlainPort));
    second.send("GET /app.js HTTP/1.1\r\nHost: x\r\nIf-None-Match: " + *etag + "\r\n\r\n");
    REQUIRE(second.wait_head());
    CHECK(status_of(second.received()) == 304);
    CHECK_FALSE(head_has(second.received(), "content-length:")); // RFC 9110 §15.4.5

    second.pump(std::chrono::milliseconds(150));
    CHECK(body_of(second.received()).empty());

    second.close();
}

TEST_CASE("regression/static: an encoded traversal is refused and the connection "
          "closed",
          "[regression][static]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    // Encoded, so it survives the HTTP/1.1 request-line parser intact and reaches
    // the static layer as a path rather than being rejected as a bad target.
    client.send("GET /%2e%2e/etc/passwd HTTP/1.1\r\nHost: x\r\n\r\n");
    REQUIRE(client.wait_head());
    CHECK(status_of(client.received()) == 400);
    CHECK(client.wait_eof()); // a malformed target says the peer is not a browser

    client.close();
}

TEST_CASE("regression/static: a POST to an existing file is a 405", "[regression][static]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    client.send("POST /index.html HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n");
    REQUIRE(client.wait_head());
    CHECK(status_of(client.received()) == 405);
    CHECK(head_has(client.received(), "allow: GET, HEAD"));

    client.close();
}

TEST_CASE("regression/method: a wrong method on a route is a 405 with Allow", "[regression][method]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    client.send("POST /method/getonly HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n");
    REQUIRE(client.wait_head());
    CHECK(status_of(client.received()) == 405);
    CHECK(head_has(client.received(), "allow: GET, HEAD, OPTIONS"));
    // 405 is a keep-alive response, like the mainstream routers (Go/axum/Spring):
    // the same connection must carry the next exchange.
    client.send("GET /method/getonly HTTP/1.1\r\nHost: x\r\n\r\n");
    REQUIRE(client.wait_for("getonly"));

    client.close();
}

TEST_CASE("regression/method: a method the path serves still runs", "[regression][method]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    client.send("PUT /method/multi HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n");
    REQUIRE(client.wait_for("multi"));

    client.close();
}

TEST_CASE("regression/method: GET serves HEAD with an empty body", "[regression][method]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    client.send("HEAD /method/getonly HTTP/1.1\r\nHost: x\r\n\r\n");
    REQUIRE(client.wait_head());
    CHECK(status_of(client.received()) == 200);
    CHECK(head_has(client.received(),
                   "content-length: 7"));      // what a GET would produce…
    CHECK(body_of(client.received()).empty()); // …but no body follows the head

    client.close();
}

TEST_CASE("regression/method: OPTIONS is answered with Allow, the route does not run", "[regression][method]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    client.send("OPTIONS /method/postonly HTTP/1.1\r\nHost: x\r\n\r\n");
    REQUIRE(client.wait_head());
    CHECK(status_of(client.received()) == 204);
    CHECK(head_has(client.received(), "allow: POST, OPTIONS"));

    client.close();
}

TEST_CASE("regression/method: an extension method on an any-method route is served", "[regression][method]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    // PROPFIND parses to Method::Unknown; only an any-method route serves it.
    client.send("PROPFIND /world HTTP/1.1\r\nHost: x\r\n\r\n");
    REQUIRE(client.wait_for("hello"));

    client.close();
}

TEST_CASE("regression/static: two requests on one connection both succeed", "[regression][static]") {
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));

    // Proves the stage leaves no per-request state behind — no lock held across a
    // co_await, no stream left half-written.
    client.send("GET /app.js HTTP/1.1\r\nHost: x\r\n\r\n");
    REQUIRE(client.pump_until([&] { return client.received().find("console.log") != std::string::npos; },
                              std::chrono::seconds(5)));
    client.send("GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n");
    REQUIRE(client.pump_until([&] { return client.received().find("<html>index</html>") != std::string::npos; },
                              std::chrono::seconds(5)));

    client.close();
}

TEST_CASE("regression/static: the same responses over h2c", "[regression][static][h2]") {
    // The unit tests cover these paths through a recording writer, which cannot
    // see the difference that matters here: the HTTP/2 engine forwards our header
    // block verbatim and never computes a length, so the branches that set
    // content-length by hand are only load-bearing on this side.
    plain_server();
    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(kPlainPort));
    client.send(h2_preface_and_settings());

    auto send_get = [&](const std::string &path, std::uint32_t stream_id,
                        const std::vector<std::pair<std::string, std::string>> &extra) {
        std::string block;
        sh::codec::hpack_append_literal(block, ":method", "GET");
        sh::codec::hpack_append_literal(block, ":scheme", "http");
        sh::codec::hpack_append_literal(block, ":authority", "127.0.0.1");
        sh::codec::hpack_append_literal(block, ":path", path);
        for (const auto &[name, value] : extra)
            sh::codec::hpack_append_literal(block, name, value);
        client.send(h2_frame(sh::codec::H2FrameType::Headers,
                             sh::codec::H2_FLAG_END_HEADERS | sh::codec::H2_FLAG_END_STREAM, stream_id, block));
    };

    // 200, with the length we set by hand for the encoding case.
    send_get("/app.js", 1, {{"accept-encoding", "br"}});
    REQUIRE(client.pump_until(
        [&] {
            auto frames = parse_frames(client.received());
            for (const auto &f : frames) {
                if (f.header.type == static_cast<std::uint8_t>(sh::codec::H2FrameType::Data) &&
                    f.header.stream_id == 1 && f.payload == "BR:console.log('app');") {
                    return true;
                }
            }
            return false;
        },
        std::chrono::seconds(5)));

    // 304: no DATA frame at all, and the stream is closed by the response itself.
    std::string etag_block;
    {
        auto frames = parse_frames(client.received());
        for (const auto &f : frames) {
            if (f.header.type == static_cast<std::uint8_t>(sh::codec::H2FrameType::Headers))
                break;
        }
    }
    send_get("/app.js", 3, {});
    REQUIRE(client.pump_until(
        [&] {
            for (const auto &f : parse_frames(client.received())) {
                if (f.header.stream_id == 3 &&
                    f.header.type == static_cast<std::uint8_t>(sh::codec::H2FrameType::Data)) {
                    return true;
                }
            }
            return false;
        },
        std::chrono::seconds(5)));

    // 206 with a content-range, which the h2 engine must forward untouched.
    send_get("/app.js", 5, {{"range", "bytes=0-4"}});
    REQUIRE(client.pump_until(
        [&] {
            for (const auto &f : parse_frames(client.received())) {
                if (f.header.stream_id == 5 &&
                    f.header.type == static_cast<std::uint8_t>(sh::codec::H2FrameType::Data) && f.payload == "conso") {
                    return true;
                }
            }
            return false;
        },
        std::chrono::seconds(5)));

    client.close();
}

// --- server lifecycle: connection limiting (Go netutil.LimitListener shape) --

// The limiter is backpressure, not rejection: at the limit the acceptor simply
// stops accepting, so a further connection sits in the kernel backlog — its TCP
// handshake succeeds — until an earlier one frees its slot. Asserting the negative
// (no response while the slot is held) is what distinguishes this from an
// accept-then-close design, which would answer the queued request immediately with
// a reset.
TEST_CASE("regression/server: max_connections waits for a slot instead of rejecting", "[regression][server]") {
    constexpr std::uint16_t port = 27940;
    sh::ServerConfig cfg;
    cfg.listen = sh::InetAddress{"127.0.0.1", port, false};
    cfg.worker_threads = 2;
    cfg.max_connections = 1;
    cfg.limits.idle_timeout = std::chrono::seconds(10); // generous: no watchdog interference
    sh::Server server{cfg};
    server.route({sh::Method::Get}, "/hello", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).content_type(sh::mime::text_plain).send("hello");
    });
    REQUIRE(server.start());

    asio::io_context ctx;

    // The first connection takes the only slot and keeps it (keep-alive).
    RawClient first{ctx};
    REQUIRE(first.connect(port));
    first.send("GET /hello HTTP/1.1\r\nHost: x\r\n\r\n");
    REQUIRE(first.wait_for("hello"));
    CHECK(server.active_connections() == 1);

    // The second connection's handshake completes against the backlog, but the
    // request is not served while the first holds the slot.
    RawClient second{ctx};
    REQUIRE(second.connect(port));
    second.send("GET /hello HTTP/1.1\r\nHost: x\r\n\r\n");
    second.pump(std::chrono::milliseconds(250));
    CHECK(second.received().empty());

    // Release the slot: the waiting connection is admitted and answered.
    first.close();
    REQUIRE(second.wait_for("hello"));

    second.close();
    server.stop();
}

// The listener-side counterpart of socket_setup: a hook that runs on every TCP
// acceptor after bind() and before listen(), so options that only exist on a
// listener (TCP_FASTOPEN, the listen queue's buffer sizes) can be set. It has to
// run once per acceptor — including each SO_REUSEPORT fan-out socket — and before
// any accept loop starts.
TEST_CASE("regression/server: listener_setup runs on each acceptor before accept", "[regression][server]") {
    constexpr std::uint16_t port = 27935;
    sh::ServerConfig cfg;
    cfg.listen = sh::InetAddress{"127.0.0.1", port, false};
    cfg.worker_threads = 2;
    cfg.reuse_port = true; // fan the listener out, so the hook has to run per socket

    std::atomic<int> calls{0};
    std::atomic<bool> bound_and_settable{false};
    cfg.listener_setup = [&](asio::ip::tcp::acceptor &acceptor) {
        ++calls;
        // Bound but not yet listening, and still settable — which is the window
        // the hook exists for.
        boost::system::error_code ec;
        const bool open = acceptor.is_open();
        acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
        bound_and_settable = open && !ec;
    };

    sh::Server server{cfg};
    REQUIRE(server.start());
    CHECK(calls.load() == static_cast<int>(server.acceptor_count()));
    CHECK(calls.load() >= 1);
    CHECK(bound_and_settable.load());
    server.stop();
}

// --- review regressions: listener policy, startup, and the ws proxy ----------

namespace {

// A scripted TCP backend for the reverse-proxy cases: binds an ephemeral port,
// accepts one connection (with a deadline), reads the request head, then hands
// the socket to the responder on the backend thread. Blocking calls are fine
// here — this is a test double, not a model-A engine.
class RawBackend {
  public:
    using Responder =
        std::function<void(asio::ip::tcp::socket &, std::string_view request, const std::atomic<bool> &stop)>;

    explicit RawBackend(Responder responder) {
        std::promise<std::uint16_t> ready;
        auto future = ready.get_future();
        m_thread = std::thread([this, responder = std::move(responder), ready = std::move(ready)]() mutable {
            asio::io_context io;
            asio::ip::tcp::acceptor acceptor{io};
            sh::error_code ec;
            acceptor.open(asio::ip::tcp::v4(), ec);
            if (!ec)
                acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
            if (!ec)
                acceptor.bind({asio::ip::make_address("127.0.0.1"), 0}, ec);
            if (!ec)
                acceptor.listen(64, ec);
            if (ec) {
                ready.set_value(0);
                return;
            }
            ready.set_value(acceptor.local_endpoint().port());
            acceptor.non_blocking(true, ec);
            asio::ip::tcp::socket socket{io};
            const auto accept_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            for (;;) {
                if (m_stop || std::chrono::steady_clock::now() >= accept_deadline)
                    return;
                acceptor.accept(socket, ec);
                if (!ec)
                    break;
                if (ec != asio::error::would_block && ec != asio::error::try_again)
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            std::string request;
            std::array<char, 4096> buf{};
            const auto read_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            for (;;) {
                if (m_stop || request.find("\r\n\r\n") != std::string::npos ||
                    std::chrono::steady_clock::now() >= read_deadline)
                    break;
                const auto n = socket.read_some(asio::buffer(buf), ec);
                if (!ec)
                    request.append(buf.data(), n);
                else if (ec != asio::error::would_block && ec != asio::error::try_again)
                    break;
                else
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            {
                std::lock_guard lock(m_mutex);
                m_request = request;
            }
            socket.non_blocking(false, ec);
            responder(socket, request, m_stop);
        });
        m_port = future.get();
    }

    ~RawBackend() {
        m_stop = true;
        if (m_thread.joinable())
            m_thread.join();
    }

    RawBackend(const RawBackend &) = delete;
    RawBackend &operator=(const RawBackend &) = delete;

    std::uint16_t port() const { return m_port; }
    std::string request() const {
        std::lock_guard lock(m_mutex);
        return m_request;
    }

  private:
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::uint16_t m_port{0};
    mutable std::mutex m_mutex;
    std::string m_request;
};

void backend_write(asio::ip::tcp::socket &socket, std::string_view bytes) {
    sh::error_code ec;
    asio::write(socket, asio::buffer(bytes.data(), bytes.size()), ec);
}

constexpr std::string_view kUpgradeRequest = "GET /wsproxy HTTP/1.1\r\n"
                                             "Host: front\r\n"
                                             "Connection: Upgrade\r\n"
                                             "Upgrade: websocket\r\n"
                                             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                             "Sec-WebSocket-Version: 13\r\n\r\n";

// Starts a one-listener server whose router proxies `path` to `target`.
struct ProxyServer {
    sh::Server server;
    explicit ProxyServer(std::uint16_t port, std::string path, sh::WsProxyTarget target,
                         std::chrono::seconds idle = std::chrono::seconds{0})
        : server(make_cfg(port, idle)) {
        server.router()->ws_proxy(std::move(path), std::move(target));
    }
    static sh::ServerConfig make_cfg(std::uint16_t port, std::chrono::seconds idle) {
        sh::ServerConfig cfg;
        cfg.listen = sh::InetAddress{"127.0.0.1", port, false};
        cfg.worker_threads = 1;
        cfg.reuse_port = false;
        if (idle.count() > 0)
            cfg.limits.idle_timeout = idle;
        return cfg;
    }
};

std::string negotiated_alpn(std::uint16_t port, const std::vector<std::string> &protocols) {
    asio::io_context io;
    asio::ssl::context ctx{asio::ssl::context::tls_client};
    ctx.set_verify_mode(asio::ssl::verify_none);
    asio::ssl::stream<asio::ip::tcp::socket> stream{io, ctx};
    sh::error_code ec;
    stream.next_layer().connect({asio::ip::make_address("127.0.0.1"), port}, ec);
    if (ec)
        return {"<connect failed>"};
    std::vector<unsigned char> wire;
    for (const auto &proto : protocols) {
        wire.push_back(static_cast<unsigned char>(proto.size()));
        wire.insert(wire.end(), proto.begin(), proto.end());
    }
    if (SSL_set_alpn_protos(stream.native_handle(), wire.data(), static_cast<unsigned int>(wire.size())) != 0)
        return {"<alpn setup failed>"};
    stream.handshake(asio::ssl::stream_base::client, ec);
    if (ec)
        return {"<handshake failed>"};
    const unsigned char *selected = nullptr;
    unsigned int len = 0;
    SSL_get0_alpn_selected(stream.native_handle(), &selected, &len);
    return std::string(
        reinterpret_cast<const char *>(selected ? selected : reinterpret_cast<const unsigned char *>("")), len);
}

sh::TlsConfig test_tls_config() {
    return sh::TlsConfig{"./test/tls_certificates/server_cert.pem", "./test/tls_certificates/server_key.pem"};
}

} // namespace

TEST_CASE("regression/ws-proxy: control bytes in the target never reach the backend", "[regression][wsproxy]") {
    constexpr std::uint16_t port = 27960;
    RawBackend backend([](asio::ip::tcp::socket &socket, std::string_view, const std::atomic<bool> &stop) {
        backend_write(socket, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n");
        while (!stop)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    });
    ProxyServer front{port, "/wsproxy", sh::WsProxyTarget{"127.0.0.1", backend.port(), {}}};
    REQUIRE(front.server.start());

    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(port));
    client.send("GET /wsproxy?x=\rHost:evil HTTP/1.1\r\n"
                "Host: front\r\n"
                "Connection: Upgrade\r\n"
                "Upgrade: websocket\r\n"
                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                "Sec-WebSocket-Version: 13\r\n\r\n");
    CHECK(client.wait_for("400", std::chrono::seconds(2)));
    CHECK(backend.request().find("Host:evil") == std::string::npos);
    client.close();
    front.server.stop();
}

TEST_CASE("regression/ws-proxy: a backend that never answers the handshake is reaped", "[regression][wsproxy]") {
    constexpr std::uint16_t port = 27961;
    RawBackend backend([](asio::ip::tcp::socket &, std::string_view, const std::atomic<bool> &stop) {
        while (!stop)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    });
    sh::WsProxyTarget target{"127.0.0.1", backend.port(), {}};
    target.connect_timeout = std::chrono::milliseconds(300);
    ProxyServer front{port, "/wsproxy", std::move(target)};
    REQUIRE(front.server.start());

    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(port));
    client.send(kUpgradeRequest);
    // The establishment phase must be bounded: a backend that accepts but never
    // replies cannot pin the frontend connection forever.
    CHECK(client.wait_for("502", std::chrono::milliseconds(1600)));
    client.close();
    front.server.stop();
}

TEST_CASE("regression/ws-proxy: a chunked refusal body is relayed in full", "[regression][wsproxy]") {
    constexpr std::uint16_t port = 27962;
    RawBackend backend([](asio::ip::tcp::socket &socket, std::string_view, const std::atomic<bool> &) {
        backend_write(socket, "HTTP/1.1 403 Forbidden\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
        // Separate TCP segments: the head must be read before the body arrives,
        // which is the case the old "never read past Content-Length" path lost.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        backend_write(socket, "5\r\nhello\r\n0\r\n\r\n");
        // Keep the socket open so the proxy has to stop at the terminating
        // chunk rather than wait for EOF.
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
    });
    sh::WsProxyTarget target{"127.0.0.1", backend.port(), {}};
    target.connect_timeout = std::chrono::seconds(2);
    ProxyServer front{port, "/wsproxy", std::move(target)};
    REQUIRE(front.server.start());

    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(port));
    client.send(kUpgradeRequest);
    CHECK(client.wait_for("hello", std::chrono::seconds(2)));
    CHECK(client.wait_for("0\r\n\r\n", std::chrono::seconds(1)));
    client.close();
    front.server.stop();
}

// The idle watchdog has to be refreshed by traffic in either direction: if it
// snapshots the deadline once, the "idle timeout" becomes a hard lifetime cap
// and an actively used tunnel is cut at the same moment an idle one would be.
TEST_CASE("regression/ws-proxy: an active tunnel outlives the idle timeout", "[regression][wsproxy]") {
    constexpr std::uint16_t port = 27966;
    RawBackend backend([](asio::ip::tcp::socket &socket, std::string_view request, const std::atomic<bool> &) {
        // The proxy synthesizes its own Sec-WebSocket-Key, so the Accept has to
        // be derived from what actually arrived.
        constexpr std::string_view prefix{"sec-websocket-key: "};
        std::string key;
        if (const auto at = request.find(prefix); at != std::string_view::npos) {
            const auto begin = at + prefix.size();
            const auto end = request.find("\r\n", begin);
            key.assign(request.substr(begin, end == std::string_view::npos ? end : end - begin));
        }
        backend_write(socket, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                              "Sec-WebSocket-Accept: " +
                                  sh::ws_accept_key(key) + "\r\n\r\n");
        // Byte-level echo: whatever the tunnel carries comes straight back.
        std::array<char, 512> buf{};
        for (;;) {
            sh::error_code ec;
            const auto n = socket.read_some(asio::buffer(buf), ec);
            if (ec)
                return;
            backend_write(socket, std::string_view{buf.data(), static_cast<std::size_t>(n)});
        }
    });
    sh::WsProxyTarget target{"127.0.0.1", backend.port(), {}};
    target.connect_timeout = std::chrono::seconds(2);
    ProxyServer front{port, "/wsproxy", std::move(target), std::chrono::seconds(1)};
    REQUIRE(front.server.start());

    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(port));
    client.send(kUpgradeRequest);
    REQUIRE(client.wait_for("101", std::chrono::seconds(2)));

    // Keep the tunnel busy for well over its 1s idle timeout: ten pings, each
    // followed by a 250ms slice (2.5s of traffic). Fixed counts instead of a
    // wall-clock loop keep the assertion stable on a loaded machine.
    constexpr std::size_t kPings = 10;
    std::size_t pings = 0;
    for (; pings < kPings && !client.eof(); ++pings) {
        client.send("<ping>");
        client.pump(std::chrono::milliseconds(250));
    }
    CHECK(pings == kPings);
    CHECK_FALSE(client.eof());
    std::size_t echoes = 0;
    for (auto at = client.received().find("<ping>"); at != std::string::npos;
         at = client.received().find("<ping>", at + 6))
        ++echoes;
    CHECK(echoes + 1 >= kPings); // the last echo may still be in flight
    client.close();
    front.server.stop();
}

TEST_CASE("regression/server: listener ALPN policy is applied", "[regression][server][tls]") {
    constexpr std::uint16_t port = 27963;
    sh::ServerConfig cfg;
    sh::TcpListenerConfig listener;
    listener.address = sh::InetAddress{"127.0.0.1", port, false};
    listener.reuse_port = false;
    listener.tls = test_tls_config();
    listener.alpn = sh::TlsHttp::Http1Only;
    cfg.worker_threads = 1;
    cfg.listeners = {listener};
    sh::Server server{cfg};
    REQUIRE(server.start());

    // A client offering both protocols must be steered to http/1.1; before the
    // listener policy was wired through, the context always offered h2 first.
    CHECK(negotiated_alpn(port, {"h2", "http/1.1"}) == "http/1.1");
    server.stop();
}

TEST_CASE("regression/server: start() can be retried after a bind failure", "[regression][server]") {
    constexpr std::uint16_t port = 27964;
    asio::io_context io;
    asio::ip::tcp::acceptor blocker{io};
    sh::error_code ec;
    blocker.open(asio::ip::tcp::v4(), ec);
    REQUIRE_FALSE(ec);
    blocker.set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
    REQUIRE_FALSE(ec);
    blocker.bind({asio::ip::make_address("127.0.0.1"), port}, ec);
    REQUIRE_FALSE(ec);
    blocker.listen(8, ec);
    REQUIRE_FALSE(ec);

    sh::ServerConfig cfg;
    cfg.listen = sh::InetAddress{"127.0.0.1", port, false};
    cfg.worker_threads = 1;
    cfg.reuse_port = false;
    sh::Server server{cfg};
    server.route({sh::Method::Get}, "/hello", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).send("hello");
    });

    const auto first = server.start();
    REQUIRE_FALSE(first);
    blocker.close();

    // close_listeners() documents that the caller may fix the config and start
    // again; a successful retry is the whole point.
    const auto second = server.start();
    CHECK(second.has_value());
    if (second)
        server.stop();
}

TEST_CASE("regression/server: max_connections is one budget shared by every listener", "[regression][server]") {
    constexpr std::uint16_t port_a = 27965;
    constexpr std::uint16_t port_b = 27966;
    sh::ServerConfig cfg;
    cfg.worker_threads = 2;
    cfg.max_connections = 1;
    sh::TcpListenerConfig a;
    a.name = "a";
    a.address = sh::InetAddress{"127.0.0.1", port_a, false};
    a.reuse_port = false;
    sh::TcpListenerConfig b;
    b.name = "b";
    b.address = sh::InetAddress{"127.0.0.1", port_b, false};
    b.reuse_port = false;
    cfg.listeners = {a, b};
    sh::Server server{cfg};
    server.route({sh::Method::Get}, "/hello", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).send("hello");
    });
    REQUIRE(server.start());

    asio::io_context ctx;
    RawClient first{ctx};
    REQUIRE(first.connect(port_a));
    first.send("GET /hello HTTP/1.1\r\nHost: x\r\n\r\n");
    REQUIRE(first.wait_for("hello"));

    // The connection on the other listener must wait for the one global slot,
    // not get a second budget of its own.
    RawClient second{ctx};
    REQUIRE(second.connect(port_b));
    second.send("GET /hello HTTP/1.1\r\nHost: x\r\n\r\n");
    second.pump(std::chrono::milliseconds(250));
    CHECK(second.received().empty());

    first.close();
    REQUIRE(second.wait_for("hello"));
    second.close();
    server.stop();
}

TEST_CASE("regression/server: startup failures name their cause", "[regression][server]") {
    constexpr std::uint16_t port = 27967;
    {
        sh::ServerConfig cfg;
        cfg.listen = sh::InetAddress{"127.0.0.1", port, false};
        cfg.worker_threads = 1;
        cfg.reuse_port = false;
        cfg.tls =
            sh::TlsConfig{"./test/tls_certificates/does_not_exist.pem", "./test/tls_certificates/does_not_exist.key"};
        sh::Server server{cfg};
        const auto started = server.start();
        REQUIRE_FALSE(started);
        REQUIRE(started.error().failures.size() == 1);
        CHECK(started.error().failures[0].error == sh::make_error_code(sh::server_errc::tls_setup_failed));
    }
    {
        sh::ServerConfig cfg;
        cfg.listen = sh::InetAddress{"999.999.999.999", port, false};
        cfg.worker_threads = 1;
        cfg.reuse_port = false;
        sh::Server server{cfg};
        const auto started = server.start();
        REQUIRE_FALSE(started);
        REQUIRE(started.error().failures.size() == 1);
        CHECK(started.error().failures[0].error == sh::make_error_code(sh::server_errc::invalid_config));
    }
}

TEST_CASE("regression/ws-proxy: h2 extended CONNECT keeps the backend's trailing frames", "[regression][wsproxy][h2]") {
    constexpr std::uint16_t port = 27968;
    RawBackend backend([](asio::ip::tcp::socket &socket, std::string_view request, const std::atomic<bool> &) {
        constexpr std::string_view prefix{"sec-websocket-key: "};
        std::string key;
        if (const auto at = request.find(prefix); at != std::string_view::npos) {
            const auto begin = at + prefix.size();
            const auto end = request.find("\r\n", begin);
            key.assign(request.substr(begin, end == std::string_view::npos ? end : end - begin));
        }
        backend_write(socket, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                              "Sec-WebSocket-Accept: " +
                                  sh::ws_accept_key(key) + "\r\n\r\n");
        // Wait until the frontend forwards the client's END_STREAM as a send
        // half-close.
        std::array<char, 4096> buf{};
        sh::error_code ec;
        for (;;) {
            const auto n = socket.read_some(asio::buffer(buf), ec);
            (void)n;
            if (ec)
                break;
        }
        // The backend still has a Close/final frame to deliver after the client
        // half-closes. Server-to-client frames are unmasked.
        backend_write(socket, std::string{"\x81\x03"
                                          "bye",
                                          5});
    });
    ProxyServer front{port, "/wsproxy", sh::WsProxyTarget{"127.0.0.1", backend.port(), {}}};
    REQUIRE(front.server.start());

    asio::io_context ctx;
    RawClient client{ctx};
    REQUIRE(client.connect(port));
    client.send(h2_preface_and_settings());

    std::string block;
    sh::codec::hpack_append_literal(block, ":method", "CONNECT");
    sh::codec::hpack_append_literal(block, ":scheme", "http");
    sh::codec::hpack_append_literal(block, ":authority", "127.0.0.1");
    sh::codec::hpack_append_literal(block, ":path", "/wsproxy");
    sh::codec::hpack_append_literal(block, ":protocol", "websocket");
    client.send(h2_frame(sh::codec::H2FrameType::Headers, sh::codec::H2_FLAG_END_HEADERS, 1, block));

    std::string observed_status;
    const bool established = client.pump_until(
        [&] {
            const auto frames = parse_frames(client.received());
            for (const auto &frame : frames) {
                if (frame.header.type == static_cast<std::uint8_t>(sh::codec::H2FrameType::Headers) &&
                    frame.header.stream_id == 1) {
                    observed_status = header_value(decode_headers(frame.payload), ":status");
                    return observed_status == "200";
                }
            }
            return false;
        },
        std::chrono::seconds(3));
    INFO("observed :status = " << observed_status);
    INFO("client received: " << printable(client.received()));
    INFO("backend request: " << printable(backend.request()));
    REQUIRE(established);

    // Client half-closes its tunnel; the backend's "bye" must still arrive.
    client.send(h2_frame(sh::codec::H2FrameType::Data, sh::codec::H2_FLAG_END_STREAM, 1, "x"));

    client.pump_until(
        [&] {
            std::string data;
            for (const auto &frame : parse_frames(client.received())) {
                if (frame.header.type == static_cast<std::uint8_t>(sh::codec::H2FrameType::Data) &&
                    frame.header.stream_id == 1) {
                    data.append(frame.payload);
                }
            }
            return data.find("bye") != std::string::npos;
        },
        std::chrono::seconds(2));
    std::string data;
    for (const auto &frame : parse_frames(client.received())) {
        if (frame.header.type == static_cast<std::uint8_t>(sh::codec::H2FrameType::Data) && frame.header.stream_id == 1)
            data.append(frame.payload);
    }
    CHECK(data.find("bye") != std::string::npos);
    client.close();
    front.server.stop();
}

namespace {

std::string tls_http_get(std::uint16_t port, std::string_view request, const std::vector<std::string> &protocols) {
    asio::io_context io;
    asio::ssl::context ctx{asio::ssl::context::tls_client};
    ctx.set_verify_mode(asio::ssl::verify_none);
    asio::ssl::stream<asio::ip::tcp::socket> stream{io, ctx};
    sh::error_code ec;
    stream.next_layer().connect({asio::ip::make_address("127.0.0.1"), port}, ec);
    if (ec)
        return {"<connect failed>"};
    std::vector<unsigned char> wire;
    for (const auto &proto : protocols) {
        wire.push_back(static_cast<unsigned char>(proto.size()));
        wire.insert(wire.end(), proto.begin(), proto.end());
    }
    if (SSL_set_alpn_protos(stream.native_handle(), wire.data(), static_cast<unsigned int>(wire.size())) != 0)
        return {"<alpn setup failed>"};
    stream.handshake(asio::ssl::stream_base::client, ec);
    if (ec)
        return {"<handshake failed>"};
    asio::write(stream, asio::buffer(request.data(), request.size()), ec);
    if (ec)
        return {"<write failed>"};
    std::string response;
    std::array<char, 4096> buf{};
    for (;;) {
        const auto n = stream.read_some(asio::buffer(buf), ec);
        if (ec)
            break;
        response.append(buf.data(), n);
        if (response.find("\r\n\r\n") != std::string::npos)
            break;
    }
    return response;
}

} // namespace

TEST_CASE("regression/server: a QUIC listener can opt out of Alt-Svc", "[regression][server][tls]") {
    constexpr std::uint16_t port = 27969;
    sh::ServerConfig cfg;
    cfg.worker_threads = 1;
    cfg.auto_alt_svc = true;

    sh::TcpListenerConfig tcp;
    tcp.name = "tls";
    tcp.address = sh::InetAddress{"127.0.0.1", port, false};
    tcp.reuse_port = false;
    tcp.tls = test_tls_config();
    tcp.alpn = sh::TlsHttp::Http1Only;

    sh::QuicListenerConfig quic;
    quic.name = "quic";
    quic.address = sh::QuicAddress{"127.0.0.1", port, false};
    quic.tls = test_tls_config();
    quic.reuse_port = false;
    quic.advertise_alt_svc = false;

    cfg.listeners = {tcp, quic};
    sh::Server server{cfg};
    server.route({sh::Method::Get}, "/world", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).send("hello");
    });
    REQUIRE(server.start());

    const auto response =
        tls_http_get(port, "GET /world HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", {"http/1.1"});
    INFO("response: " << printable(response));
    CHECK(response.find("hello") != std::string::npos);
    // The QUIC listener opted out, so the TLS listener must not advertise it.
    CHECK_FALSE(head_value(response, "alt-svc").has_value());
    server.stop();
}

TEST_CASE("regression/server: an h2-only TLS listener refuses a non-h2 client", "[regression][server][tls]") {
    constexpr std::uint16_t port = 27971;
    sh::ServerConfig cfg;
    cfg.worker_threads = 1;
    sh::TcpListenerConfig listener;
    listener.address = sh::InetAddress{"127.0.0.1", port, false};
    listener.reuse_port = false;
    listener.tls = test_tls_config();
    listener.alpn = sh::TlsHttp::Http2Only;
    cfg.listeners = {listener};
    sh::Server server{cfg};
    server.route({sh::Method::Get}, "/world", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).send("hello");
    });
    REQUIRE(server.start());

    // Offering h2 selects h2, as the policy says.
    CHECK(negotiated_alpn(port, {"h2"}) == "h2");

    // Offering only http/1.1 negotiates nothing. An h2-only listener must not
    // quietly fall back to serving HTTP/1.1.
    const auto response =
        tls_http_get(port, "GET /world HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", {"http/1.1"});
    INFO("response: " << printable(response));
    CHECK(response.find("hello") == std::string::npos);
    server.stop();
}

TEST_CASE("regression/server: a real fan-out bind failure fails start()", "[regression][server]") {
    constexpr std::uint16_t port = 27972;
    sh::ServerConfig cfg;
    cfg.listen = sh::InetAddress{"127.0.0.1", port, false};
    cfg.worker_threads = 4;
    cfg.reuse_port = true;
    // The hook runs per acceptor, after bind and before listen. Breaking the
    // second socket makes the fan-out fail for a real reason (not "SO_REUSEPORT
    // unavailable"), which must fail the listener rather than report success
    // with zero or reduced accept capacity.
    std::atomic<int> calls{0};
    cfg.listener_setup = [&](asio::ip::tcp::acceptor &acceptor) {
        if (calls.fetch_add(1) >= 1) {
            sh::error_code ec;
            acceptor.close(ec);
        }
    };
    sh::Server server{cfg};
    server.route({sh::Method::Get}, "/hello", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).send("hello");
    });
    const auto started = server.start();
    INFO("acceptor hook calls: " << calls.load());
    REQUIRE_FALSE(started);
    REQUIRE(started.error().failures.size() == 1);
    CHECK(started.error().failures[0].error == sh::make_error_code(sh::server_errc::bind_failed));
    server.stop();
}

TEST_CASE("regression/quic: stop tears the endpoint down before the pool grace", "[regression][quic]") {
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
    sh::TlsConfig tls;
    tls.cert_chain_file = "./test/tls_certificates/server_cert.pem";
    tls.private_key_file = "./test/tls_certificates/server_key.pem";
    sh::ServerConfig cfg = make_config(27932, tls);
    cfg.quic = sh::QuicAddress{"127.0.0.1", 27933, false};
    sh::Server server{cfg};
    server.route({sh::Method::Get}, "/hello", [](sh::RequestPtr, sh::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).send("hello");
    });
    REQUIRE(server.start().has_value());

    const auto started = std::chrono::steady_clock::now();
    server.stop();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    // receive_loop is parked on the UDP socket and send_loop on its wake
    // channel. If they are not released before IoCtxPool::stop(), the pool waits
    // out its 2s drain grace on every shutdown.
    INFO("stop() took " << elapsed.count() << " ms");
    CHECK(elapsed.count() < 1500);
#else
    SUCCEED("HTTP/3 is not compiled in");
#endif
}
