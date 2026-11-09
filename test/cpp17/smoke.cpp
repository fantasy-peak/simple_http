// C++17 port smoke test.
//
// This target is deliberately built with -std=c++17 and linked against
// simple_http_cpp17 (not the C++23 simple_http target). It is the compile,
// linkage and behaviour gate for the ported layers: core utilities, the
// token-based Body, the handler/dispatcher adaptation and the Router. Later
// phases extend it to start a real server.

#include <simple_http.h>

#include <array>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>

static_assert(__cplusplus >= 201703L && __cplusplus < 202002L, "cpp17 smoke must build as C++17");

namespace {

int failures = 0;

void check(bool condition, const char *what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void test_base64() {
    const std::string encoded = simple_http::base64_encode("hello");
    check(encoded == "aGVsbG8=", "base64_encode");
    check(simple_http::base64_decode(encoded) == "hello", "base64_decode round trip");
    check(simple_http::base64_url_encode("\xfb\xff") == "-_8", "base64_url_encode");
}

void test_http_date() {
    // 2026-11-06T00:00:00Z
    const std::int64_t stamp = 1793923200;
    const std::string formatted = simple_http::http_date(stamp);
    const auto parsed = simple_http::parse_http_date(formatted);
    check(parsed && *parsed == stamp, "http_date round trip");
}

void test_url_path() {
    simple_http::PathError error{};
    const auto decoded = simple_http::decode_and_normalize("/a%20b", error);
    check(decoded && *decoded == "/a b", "decode_and_normalize");
}

void test_result() {
    const simple_http::result<int> good{42};
    check(good && *good == 42, "boost::system::result value");
    const simple_http::result<int> bad{boost::system::errc::make_error_code(boost::system::errc::invalid_argument)};
    check(!bad && bad.error(), "boost::system::result error");
}

void test_body_tokens() {
    boost::asio::io_context context;
    simple_http::Body body{context.get_executor()};
    body.feed("abc");
    body.finish();

    std::string got;
    bool eof = false;
    std::function<void()> read_next;
    read_next = [&] {
        body.async_read([&](simple_http::error_code ec, simple_http::ReadResult result) {
            if (ec) {
                check(false, "Body::async_read error");
                return;
            }
            if (result.eof) {
                eof = true;
                return;
            }
            got += result.data;
            read_next();
        });
    };
    read_next();
    context.run();
    check(got == "abc" && eof, "Body::async_read delivers data then eof");
}

class CaptureSink final : public simple_http::ResponseSink {
  public:
    explicit CaptureSink(boost::asio::any_io_executor executor) : m_executor(std::move(executor)) {}

    void send(int code, simple_http::Headers headers, std::string body, simple_http::WriteCompletion done) override {
        status = code;
        m_headers = std::move(headers);
        m_body = std::move(body);
        std::move(done)(simple_http::error_code{});
    }
    void send_headers(int code, simple_http::Headers headers, simple_http::WriteCompletion done) override {
        status = code;
        m_headers = std::move(headers);
        std::move(done)(simple_http::error_code{});
    }
    void send_chunk(std::string data, simple_http::WriteCompletion done) override {
        m_body += data;
        std::move(done)(simple_http::error_code{});
    }
    void send_last(std::string data, simple_http::WriteCompletion done) override {
        m_body += data;
        std::move(done)(simple_http::error_code{});
    }
    void send_bodyless(int code, simple_http::Headers headers, simple_http::WriteCompletion done) override {
        status = code;
        m_headers = std::move(headers);
        std::move(done)(simple_http::error_code{});
    }
    void connected(simple_http::ConnectedCompletion done) const override {
        std::move(done)(simple_http::error_code{}, true);
    }
    void close(simple_http::WriteCompletion done) override { std::move(done)(simple_http::error_code{}); }
    boost::asio::any_io_executor get_executor() const override { return m_executor; }
    simple_http::Version version() const override { return simple_http::Version::Http11; }

    int status{0};
    std::string body() const { return m_body; }

  private:
    boost::asio::any_io_executor m_executor;
    simple_http::Headers m_headers;
    std::string m_body;
};

void test_router() {
    boost::asio::io_context context;
    simple_http::Router router;
    router.route({simple_http::Method::Get}, "/hello",
                 [](simple_http::RequestPtr, simple_http::ResponsePtr res, simple_http::HandlerCompletion done) {
                     res->status(200).content_type("text/plain");
                     res->async_send("hi", simple_http::finish_with(std::move(done)));
                 });
    router.route({simple_http::Method::Get}, "/users/{id}",
                 [](simple_http::RequestPtr req, simple_http::ResponsePtr res, simple_http::HandlerCompletion done) {
                     const auto id = req->param("id");
                     res->status(200);
                     res->async_send(id ? std::string{*id} : std::string{"missing"},
                                     simple_http::finish_with(std::move(done)));
                 });

    auto call = [&](std::string target) {
        auto sink = std::make_shared<CaptureSink>(context.get_executor());
        auto res = std::make_shared<simple_http::ResponseWriter>(sink);
        auto req = std::make_shared<simple_http::Request>(simple_http::Version::Http11, context.get_executor());
        req->set_method(simple_http::Method::Get);
        req->set_target(std::move(target));
        bool finished = false;
        router.dispatch(req, res, std::nullopt, [&finished] { finished = true; });
        context.run();
        context.restart();
        return std::make_pair(finished, std::make_pair(sink->status, sink->body()));
    };

    const auto exact = call("/hello");
    check(exact.first && exact.second.first == 200 && exact.second.second == "hi", "Router exact route");
    const auto templated = call("/users/42");
    check(templated.first && templated.second.first == 200 && templated.second.second == "42",
          "Router template capture");
    const auto missing = call("/nope");
    check(missing.first && missing.second.first == 404, "Router built-in 404");
}

void test_server() {
    simple_http::ServerConfig cfg;
    cfg.listen = simple_http::InetAddress{"127.0.0.1", 0, false};
    cfg.worker_threads = 2;
    simple_http::Server server{cfg};
    server.route({simple_http::Method::Get}, "/",
                 [](simple_http::RequestPtr, simple_http::ResponsePtr res, simple_http::HandlerCompletion done) {
                     res->status(200).content_type("text/plain");
                     res->async_send("hello", simple_http::finish_with(std::move(done)));
                 });
    server.route({simple_http::Method::Post}, "/echo",
                 [](simple_http::RequestPtr req, simple_http::ResponsePtr res, simple_http::HandlerCompletion done) {
                     req->body().async_read_all(
                         [res, done = std::move(done)](simple_http::error_code ec, std::string body) mutable {
                             if (ec) {
                                 res->status(400);
                                 res->async_send("", simple_http::finish_with(std::move(done)));
                                 return;
                             }
                             res->status(200).content_type("text/plain");
                             res->async_send(std::move(body), simple_http::finish_with(std::move(done)));
                         });
                 });
    const auto started = server.start();
    check(started.has_value(), "Server::start");
    if (!started) {
        return;
    }

    boost::asio::io_context context;
    boost::asio::ip::tcp::socket socket{context};
    boost::system::error_code ec;
    socket.connect(boost::asio::ip::tcp::endpoint{boost::asio::ip::make_address("127.0.0.1"), server.port()}, ec);
    if (ec) {
        check(false, "Server connect");
        server.stop();
        return;
    }
    auto round_trip = [&](const std::string &request) {
        boost::asio::ip::tcp::socket client{context};
        boost::system::error_code error;
        client.connect(boost::asio::ip::tcp::endpoint{boost::asio::ip::make_address("127.0.0.1"), server.port()},
                       error);
        if (error) {
            return std::string{};
        }
        boost::asio::write(client, boost::asio::buffer(request), error);
        boost::asio::streambuf response;
        boost::asio::read(client, response, error);
        return std::string{boost::asio::buffers_begin(response.data()), boost::asio::buffers_end(response.data())};
    };

    const auto text = round_trip("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    check(text.find("200") != std::string::npos && text.find("hello") != std::string::npos,
          "Server serves an HTTP/1.1 request");

    const auto missing = round_trip("GET /missing HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    check(missing.find("404") != std::string::npos, "Server built-in 404");

    const auto echoed = round_trip("POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n"
                                   "Connection: close\r\n\r\nhello");
    check(echoed.find("200") != std::string::npos && echoed.find("hello") != std::string::npos,
          "Server reads a Content-Length request body");

    server.stop();
}

std::string read_one_response(boost::asio::ip::tcp::socket &socket) {
    boost::system::error_code ec;
    boost::asio::streambuf buffer;
    boost::asio::read_until(socket, buffer, "\r\n\r\n", ec);
    if (ec) {
        return {};
    }
    const std::string all{boost::asio::buffers_begin(buffer.data()), boost::asio::buffers_end(buffer.data())};
    const auto header_end = all.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return {};
    }
    const std::string head = all.substr(0, header_end + 4);
    std::string body = all.substr(head.size());
    std::size_t content_length = 0;
    if (const auto pos = head.find("content-length: "); pos != std::string::npos) {
        content_length = static_cast<std::size_t>(std::stoul(head.substr(pos + 16)));
    }
    buffer.consume(all.size());
    if (body.size() < content_length) {
        boost::asio::read(socket, buffer, boost::asio::transfer_exactly(content_length - body.size()), ec);
        if (ec) {
            return {};
        }
        body += std::string{boost::asio::buffers_begin(buffer.data()), boost::asio::buffers_end(buffer.data())};
    }
    return head + body;
}

void test_keep_alive() {
    simple_http::ServerConfig cfg;
    cfg.listen = simple_http::InetAddress{"127.0.0.1", 0, false};
    cfg.worker_threads = 2;
    simple_http::Server server{cfg};
    server.route({simple_http::Method::Get}, "/",
                 [](simple_http::RequestPtr, simple_http::ResponsePtr res, simple_http::HandlerCompletion done) {
                     res->status(200);
                     res->async_send("one", simple_http::finish_with(std::move(done)));
                 });
    check(server.start().has_value(), "keep-alive server start");

    boost::asio::io_context context;
    boost::asio::ip::tcp::socket socket{context};
    boost::system::error_code ec;
    socket.connect(boost::asio::ip::tcp::endpoint{boost::asio::ip::make_address("127.0.0.1"), server.port()}, ec);
    if (!ec) {
        const std::string first = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
        boost::asio::write(socket, boost::asio::buffer(first), ec);
        const auto response = read_one_response(socket);
        check(response.find("200") != std::string::npos && response.find("one") != std::string::npos,
              "keep-alive first response");

        const std::string second = "GET /missing HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        boost::asio::write(socket, boost::asio::buffer(second), ec);
        const auto second_response = read_one_response(socket);
        check(second_response.find("404") != std::string::npos, "keep-alive second request on the same connection");
    } else {
        check(false, "keep-alive connect");
    }
    server.stop();
}

void test_websocket() {
    simple_http::ServerConfig cfg;
    cfg.listen = simple_http::InetAddress{"127.0.0.1", 0, false};
    cfg.worker_threads = 2;
    simple_http::Server server{cfg};
    server.ws_route("/echo", [](simple_http::RequestPtr, std::shared_ptr<simple_http::WebSocket> ws,
                                simple_http::HandlerCompletion done) {
        // any_completion_handler is move-only, so the echo loop stores it
        // behind a shared_ptr instead of capturing it by value.
        auto completion = std::make_shared<simple_http::HandlerCompletion>(std::move(done));
        auto loop = std::make_shared<std::function<void()>>();
        *loop = [ws, loop, completion] {
            ws->async_read([ws, loop, completion](simple_http::error_code ec, simple_http::WsMessage msg) {
                if (ec || !msg.text) {
                    auto finish = std::move(*completion);
                    std::move(finish)();
                    return;
                }
                ws->async_write(std::move(msg.data), true, [loop, completion](simple_http::error_code write_ec) {
                    if (write_ec) {
                        auto finish = std::move(*completion);
                        std::move(finish)();
                        return;
                    }
                    (*loop)();
                });
            });
        };
        (*loop)();
    });
    check(server.start().has_value(), "websocket server start");

    boost::asio::io_context context;
    boost::asio::ip::tcp::socket socket{context};
    boost::system::error_code ec;
    socket.connect(boost::asio::ip::tcp::endpoint{boost::asio::ip::make_address("127.0.0.1"), server.port()}, ec);
    if (!ec) {
        const std::string handshake = "GET /echo HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
                                      "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                      "Sec-WebSocket-Version: 13\r\n\r\n";
        boost::asio::write(socket, boost::asio::buffer(handshake), ec);
        boost::asio::streambuf reply;
        boost::asio::read_until(socket, reply, "\r\n\r\n", ec);
        const std::string head{boost::asio::buffers_begin(reply.data()), boost::asio::buffers_end(reply.data())};
        check(head.find("101") != std::string::npos, "websocket handshake");

        // One masked text frame: FIN|text, len=5, key, "hello" XOR key.
        std::string frame;
        frame.push_back(static_cast<char>(0x81));
        frame.push_back(static_cast<char>(0x80 | 5));
        const char key[4] = {1, 2, 3, 4};
        frame.append(key, 4);
        const std::string payload = "hello";
        for (std::size_t i = 0; i < payload.size(); ++i) {
            frame.push_back(static_cast<char>(payload[i] ^ key[i % 4]));
        }
        boost::asio::write(socket, boost::asio::buffer(frame), ec);

        std::array<unsigned char, 2> header{};
        boost::asio::read(socket, boost::asio::buffer(header), ec);
        std::string echoed;
        if (!ec && header[0] == 0x81 && (header[1] & 0x7f) == 5) {
            echoed.resize(5);
            boost::asio::read(socket, boost::asio::buffer(echoed), ec);
        }
        check(echoed == "hello", "websocket echo");
    } else {
        check(false, "websocket connect");
    }
    server.stop();
}

void test_middleware() {
    simple_http::ServerConfig cfg;
    cfg.listen = simple_http::InetAddress{"127.0.0.1", 0, false};
    cfg.worker_threads = 2;
    simple_http::Server server{cfg};
    server.use(simple_http::middleware::request_id());
    server.use(simple_http::middleware::basic_auth("u", "p"));
    server.route({simple_http::Method::Get}, "/",
                 [](simple_http::RequestPtr, simple_http::ResponsePtr res, simple_http::HandlerCompletion done) {
                     res->status(200);
                     res->async_send("ok", simple_http::finish_with(std::move(done)));
                 });
    check(server.start().has_value(), "middleware server start");

    boost::asio::io_context context;
    auto round_trip = [&](const std::string &request) {
        boost::asio::ip::tcp::socket client{context};
        boost::system::error_code error;
        client.connect(boost::asio::ip::tcp::endpoint{boost::asio::ip::make_address("127.0.0.1"), server.port()},
                       error);
        if (error) {
            return std::string{};
        }
        boost::asio::write(client, boost::asio::buffer(request), error);
        boost::asio::streambuf response;
        boost::asio::read(client, response, error);
        return std::string{boost::asio::buffers_begin(response.data()), boost::asio::buffers_end(response.data())};
    };

    const auto unauthorized = round_trip("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    check(unauthorized.find("401") != std::string::npos, "basic_auth rejects a missing credential");
    check(unauthorized.find("x-request-id:") != std::string::npos, "request_id is stamped on the 401");

    const auto authorized = round_trip("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
                                       "Authorization: Basic dTpw\r\n\r\n");
    check(authorized.find("200") != std::string::npos && authorized.find("ok") != std::string::npos,
          "basic_auth accepts the configured credentials");
    server.stop();
}

void test_cors() {
    simple_http::ServerConfig cfg;
    cfg.listen = simple_http::InetAddress{"127.0.0.1", 0, false};
    cfg.worker_threads = 2;
    simple_http::Server server{cfg};
    server.cors(simple_http::CorsConfig{});
    server.route({simple_http::Method::Get}, "/",
                 [](simple_http::RequestPtr, simple_http::ResponsePtr res, simple_http::HandlerCompletion done) {
                     res->status(200);
                     res->async_send("ok", simple_http::finish_with(std::move(done)));
                 });
    check(server.start().has_value(), "cors server start");

    boost::asio::io_context context;
    auto round_trip = [&](const std::string &request) {
        boost::asio::ip::tcp::socket client{context};
        boost::system::error_code error;
        client.connect(boost::asio::ip::tcp::endpoint{boost::asio::ip::make_address("127.0.0.1"), server.port()},
                       error);
        if (error) {
            return std::string{};
        }
        boost::asio::write(client, boost::asio::buffer(request), error);
        boost::asio::streambuf response;
        boost::asio::read(client, response, error);
        return std::string{boost::asio::buffers_begin(response.data()), boost::asio::buffers_end(response.data())};
    };

    const auto preflight = round_trip("OPTIONS / HTTP/1.1\r\nHost: localhost\r\nOrigin: https://app.example\r\n"
                                      "Access-Control-Request-Method: GET\r\nConnection: close\r\n\r\n");
    check(preflight.find("204") != std::string::npos &&
              preflight.find("access-control-allow-origin: *") != std::string::npos,
          "CORS preflight is answered without reaching the route");

    const auto actual = round_trip("GET / HTTP/1.1\r\nHost: localhost\r\nOrigin: https://app.example\r\n"
                                   "Connection: close\r\n\r\n");
    check(actual.find("200") != std::string::npos && actual.find("access-control-allow-origin: *") != std::string::npos,
          "CORS headers on the actual request");
    server.stop();
}

void test_static_files() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("simple_http_cpp17_static_" + std::to_string(::getpid()));
    std::error_code fs_ec;
    fs::create_directories(root, fs_ec);
    {
        std::ofstream out{root / "index.html", std::ios::binary};
        out << "static-index";
    }
    {
        std::ofstream out{root / "hello.txt", std::ios::binary};
        out << "hello-static";
    }

    simple_http::StaticFilesConfig config;
    config.table.root = root.string();
    auto site = std::make_shared<simple_http::StaticFiles>(config);
    std::string error;
    const bool loaded = site->load(error);
    check(loaded, "StaticFiles::load");

    simple_http::ServerConfig cfg;
    cfg.listen = simple_http::InetAddress{"127.0.0.1", 0, false};
    cfg.worker_threads = 2;
    simple_http::Server server{cfg};
    server.static_files(site);
    check(server.start().has_value(), "static server start");

    boost::asio::io_context context;
    auto round_trip = [&](const std::string &request) {
        boost::asio::ip::tcp::socket client{context};
        boost::system::error_code error_code;
        client.connect(boost::asio::ip::tcp::endpoint{boost::asio::ip::make_address("127.0.0.1"), server.port()},
                       error_code);
        if (error_code) {
            return std::string{};
        }
        boost::asio::write(client, boost::asio::buffer(request), error_code);
        boost::asio::streambuf response;
        boost::asio::read(client, response, error_code);
        return std::string{boost::asio::buffers_begin(response.data()), boost::asio::buffers_end(response.data())};
    };

    const auto file = round_trip("GET /hello.txt HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    check(file.find("200") != std::string::npos && file.find("hello-static") != std::string::npos,
          "static file served");
    const auto index = round_trip("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    check(index.find("200") != std::string::npos && index.find("static-index") != std::string::npos,
          "static directory index");
    const auto missing_file = round_trip("GET /missing.txt HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    check(missing_file.find("404") != std::string::npos, "static miss falls through to 404");

    server.stop();
    fs::remove_all(root, fs_ec);
}

std::string cookie_value(const std::string &response, std::string_view name) {
    const std::string needle = "set-cookie: " + std::string{name} + "=";
    const auto pos = response.find(needle);
    if (pos == std::string::npos) {
        return {};
    }
    const auto start = pos + needle.size();
    const auto end = response.find(';', start);
    return response.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

void test_session_csrf() {
    simple_http::ServerConfig cfg;
    cfg.listen = simple_http::InetAddress{"127.0.0.1", 0, false};
    cfg.worker_threads = 2;
    simple_http::Server server{cfg};
    auto store = std::make_shared<simple_http::MemorySessionStore>();
    simple_http::SessionConfig session_cfg;
    session_cfg.secure = false;
    simple_http::CsrfConfig csrf_cfg;
    csrf_cfg.secure = false;
    server.use(simple_http::middleware::secure_headers({}));
    server.use(simple_http::middleware::session(store, session_cfg));
    server.use(simple_http::middleware::csrf(csrf_cfg));
    server.route({simple_http::Method::Get}, "/me",
                 [](simple_http::RequestPtr req, simple_http::ResponsePtr res, simple_http::HandlerCompletion done) {
                     std::string uid = "none";
                     if (auto *s = simple_http::session(*req)) {
                         if (auto value = s->get("uid")) {
                             uid = std::string{*value};
                         }
                     }
                     res->status(200);
                     res->async_send(std::move(uid), simple_http::finish_with(std::move(done)));
                 });
    server.route({simple_http::Method::Post}, "/login",
                 [](simple_http::RequestPtr req, simple_http::ResponsePtr res, simple_http::HandlerCompletion done) {
                     if (auto *s = simple_http::session(*req))
                         s->set("uid", "42");
                     res->status(204);
                     res->async_send_bodyless(simple_http::finish_with(std::move(done)));
                 });
    check(server.start().has_value(), "session server start");

    boost::asio::io_context context;
    auto round_trip = [&](const std::string &request) {
        boost::asio::ip::tcp::socket client{context};
        boost::system::error_code error;
        client.connect(boost::asio::ip::tcp::endpoint{boost::asio::ip::make_address("127.0.0.1"), server.port()},
                       error);
        if (error) {
            return std::string{};
        }
        boost::asio::write(client, boost::asio::buffer(request), error);
        boost::asio::streambuf response;
        boost::asio::read(client, response, error);
        return std::string{boost::asio::buffers_begin(response.data()), boost::asio::buffers_end(response.data())};
    };

    const auto first = round_trip("GET /me HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    check(first.find("x-content-type-options: nosniff") != std::string::npos, "secure_headers adds nosniff");
    check(first.find("none") != std::string::npos, "session starts empty");
    const std::string sid = cookie_value(first, "sid");
    const std::string csrf = cookie_value(first, "csrf");
    check(!sid.empty() && !csrf.empty(), "session and CSRF cookies issued");

    const auto rejected = round_trip("POST /login HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
                                     "Cookie: sid=" +
                                     sid + "; csrf=" + csrf + "\r\n\r\n");
    check(rejected.find("403") != std::string::npos, "csrf rejects a POST without the header");

    const auto posted = round_trip("POST /login HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
                                   "Cookie: sid=" +
                                   sid + "; csrf=" + csrf +
                                   "\r\n"
                                   "x-csrf-token: " +
                                   csrf + "\r\n\r\n");
    check(posted.find("204") != std::string::npos, "csrf accepts the matching token");

    const std::string sid_after = cookie_value(posted, "sid");
    const std::string session_cookie = sid_after.empty() ? sid : sid_after;
    const auto second = round_trip("GET /me HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
                                   "Cookie: sid=" +
                                   session_cookie + "\r\n\r\n");
    check(second.find("42") != std::string::npos, "session value persisted across requests");
    server.stop();
}

void test_http_proxy() {
    simple_http::ServerConfig backend_cfg;
    backend_cfg.listen = simple_http::InetAddress{"127.0.0.1", 0, false};
    backend_cfg.worker_threads = 2;
    simple_http::Server backend{backend_cfg};
    backend.route({simple_http::Method::Get}, "/hello",
                  [](simple_http::RequestPtr, simple_http::ResponsePtr res, simple_http::HandlerCompletion done) {
                      res->status(200);
                      res->async_send("hello", simple_http::finish_with(std::move(done)));
                  });
    backend.route({simple_http::Method::Post}, "/echo",
                  [](simple_http::RequestPtr req, simple_http::ResponsePtr res, simple_http::HandlerCompletion done) {
                      req->body().async_read_all(
                          [res, done = std::move(done)](simple_http::error_code ec, std::string body) mutable {
                              if (ec) {
                                  res->status(400);
                                  res->async_send("", simple_http::finish_with(std::move(done)));
                                  return;
                              }
                              res->status(200);
                              res->async_send(std::move(body), simple_http::finish_with(std::move(done)));
                          });
                  });
    check(backend.start().has_value(), "proxy backend start");

    simple_http::ServerConfig proxy_cfg;
    proxy_cfg.listen = simple_http::InetAddress{"127.0.0.1", 0, false};
    proxy_cfg.worker_threads = 2;
    simple_http::Server proxy{proxy_cfg};
    simple_http::HttpProxyTarget target;
    target.host = "127.0.0.1";
    target.port = backend.port();
    target.preserve_host = true;
    proxy.http_proxy("/hello", target);
    proxy.http_proxy("/echo", target);
    check(proxy.start().has_value(), "proxy server start");

    boost::asio::io_context context;
    auto round_trip = [&](const std::string &request) {
        boost::asio::ip::tcp::socket client{context};
        boost::system::error_code error;
        client.connect(boost::asio::ip::tcp::endpoint{boost::asio::ip::make_address("127.0.0.1"), proxy.port()}, error);
        if (error) {
            return std::string{};
        }
        boost::asio::write(client, boost::asio::buffer(request), error);
        boost::asio::streambuf response;
        boost::asio::read(client, response, error);
        return std::string{boost::asio::buffers_begin(response.data()), boost::asio::buffers_end(response.data())};
    };

    const auto get = round_trip("GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    check(get.find("200") != std::string::npos && get.find("hello") != std::string::npos, "proxy forwards GET");
    const auto post = round_trip("POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 9\r\n"
                                 "Connection: close\r\n\r\nvia-proxy");
    check(post.find("200") != std::string::npos && post.find("via-proxy") != std::string::npos,
          "proxy forwards POST body");
    proxy.stop();
    backend.stop();
}

// Overwrites the stack region a finished helper frame occupied. The scatter
// write regression is a use-after-return of buffer descriptors, so the test
// must actually reuse that stack before the continuation runs.
// Keep the scatter helper's frame (and the buffer-descriptor array it builds)
// out of the caller's frame: the regression is a use-after-return, so inlining
// would extend the array's lifetime past the point where the continuation runs.
template <typename Handler>
#if defined(__GNUC__)
__attribute__((noinline))
#endif
void scatter_write_noinline(boost::asio::ip::tcp::socket &socket, boost::span<const simple_http::ConstByteSpan> buffers,
                            Handler &&handler) {
    simple_http::detail::async_stream_write_seq(socket, buffers, std::forward<Handler>(handler));
}

void poison_stack(unsigned depth) {
    std::array<std::byte, 4096> junk{};
    volatile std::byte *p = junk.data();
    for (std::size_t i = 0; i < junk.size(); ++i)
        p[i] = std::byte{0xCD};
    if (depth > 0)
        poison_stack(depth - 1);
}

// A scatter-gather write larger than the socket buffers must survive several
// async_write_some rounds. Regression guard: passing a span over a stack-local
// asio::const_buffer array let Asio keep only the view, so the continuation
// read buffer descriptors out of a dead frame (observed as a WebSocket echo
// stalling at 128 KiB).
void test_scatter_write() {
    boost::asio::io_context context;
    using boost::asio::ip::tcp;
    tcp::acceptor acceptor{context, tcp::endpoint{boost::asio::ip::make_address("127.0.0.1"), 0}};
    tcp::socket server{context};
    tcp::socket client{context};
    bool accepted = false;
    bool connected = false;
    acceptor.async_accept(server, [&](simple_http::error_code ec) { accepted = !ec; });
    client.async_connect(acceptor.local_endpoint(), [&](simple_http::error_code ec) { connected = !ec; });
    context.run();
    check(accepted && connected, "scatter write: sockets connected");

    // Tiny kernel buffers plus a read started only after the write is
    // initiated force the composed write to resume across callbacks.
    boost::system::error_code ignored;
    server.set_option(boost::asio::socket_base::send_buffer_size{4096}, ignored);
    client.set_option(boost::asio::socket_base::receive_buffer_size{4096}, ignored);

    const std::string first(128 * 1024, 'a');
    const std::string second(128 * 1024, 'b');
    const std::size_t total = first.size() + second.size();
    const std::array<simple_http::ConstByteSpan, 2> buffers{
        simple_http::ConstByteSpan{reinterpret_cast<const std::byte *>(first.data()), first.size()},
        simple_http::ConstByteSpan{reinterpret_cast<const std::byte *>(second.data()), second.size()}};
    auto received = std::make_shared<std::string>(total, '\0');
    bool wrote = false;
    bool read = false;
    // Never let a broken write path hang the whole suite: close both sockets
    // after five seconds, which completes the pending operations with
    // operation_aborted and turns the check below into a normal failure.
    boost::asio::steady_timer watchdog{context};
    auto cancel_watchdog = [&] { watchdog.cancel(); };
    scatter_write_noinline(server, boost::span<const simple_http::ConstByteSpan>{buffers.data(), buffers.size()},
                           [&](simple_http::error_code ec, std::size_t n) {
                               wrote = !ec && n == total;
                               if (wrote && read)
                                   cancel_watchdog();
                           });
    boost::asio::async_read(client, boost::asio::buffer(received->data(), received->size()),
                            [&](simple_http::error_code ec, std::size_t n) {
                                read = !ec && n == total;
                                if (wrote && read)
                                    cancel_watchdog();
                            });
    watchdog.expires_after(std::chrono::seconds(5));
    watchdog.async_wait([&](simple_http::error_code ec) {
        if (!ec) {
            boost::system::error_code close_ec;
            server.close(close_ec);
            client.close(close_ec);
        }
    });
    poison_stack(64);
    context.restart();
    context.run();
    check(wrote && read, "scatter write: full 256 KiB transfer");
    check(*received == first + second, "scatter write: payload intact");
}

void test_logging() {
    simple_http::set_log_sink(simple_http::make_stdout_sink(simple_http::LogLevel::Info));
    SIMPLE_HTTP_INFO_LOG("cpp17 smoke {} {}", 1, "ok");
    simple_http::set_log_sink(nullptr);
}

} // namespace

int main() {
    test_base64();
    test_http_date();
    test_url_path();
    test_result();
    test_body_tokens();
    test_router();
    test_server();
    test_keep_alive();
    test_websocket();
    test_middleware();
    test_cors();
    test_static_files();
    test_session_csrf();
    test_http_proxy();
    test_scatter_write();
    test_logging();
    if (failures == 0) {
        std::puts("cpp17 smoke: PASS");
    }
    return failures == 0 ? 0 : 1;
}
