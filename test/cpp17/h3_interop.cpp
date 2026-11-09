// C++17 HTTP/3 client interop driver. Argument: the server's UDP port.
//
// Runs against fixture_conformance_server_cpp17 (or any HTTP/3 server with the
// same two routes) and checks a GET and a POST end to end over QUIC.

#include <simple_http.h>

#include <boost/asio/io_context.hpp>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <udp-port>\n", argv[0]);
        return 2;
    }
    const std::string base = "https://127.0.0.1:" + std::string{argv[1]};
    // The HTTP echo route differs by fixture: /echo here, / on the conformance
    // fixture (whose /echo is the WebSocket route).
    const std::string echo_path = argc > 2 ? std::string{argv[2]} : std::string{"/echo"};

    boost::asio::io_context context;
    simple_http::ClientConfig config;
    config.default_version = simple_http::HttpVersionPolicy::Http3;
    config.tls.ca_file = "./test/tls_certificates/ca_cert.pem";
    config.tls.verify_host = false;
    auto client = std::make_shared<simple_http::Client>(context.get_executor(), config);

    int failures = 0;
    auto check = [&failures](bool ok, const char *what) {
        std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
        std::fflush(stdout);
        if (!ok)
            ++failures;
    };

    client->get(base + "/world?n=1000").send([&](simple_http::error_code ec, simple_http::Response response) {
        check(!ec && response.status() == 200 && response.text().size() == 1000 &&
                  response.version() == simple_http::Version::Http3,
              "h3 GET /world");
        client->post(base + echo_path)
            .body("h3-roundtrip")
            .send([&](simple_http::error_code post_ec, simple_http::Response echoed) {
                if (post_ec) {
                    std::fprintf(stderr, "h3 POST error: %s\n", post_ec.message().c_str());
                } else if (echoed.text() != "h3-roundtrip") {
                    std::fprintf(stderr, "h3 POST status=%d body=[%s]\n", echoed.status(), echoed.text().c_str());
                }
                check(!post_ec && echoed.status() == 200 && echoed.text() == "h3-roundtrip",
                      "h3 POST echo");
                client->close();
            });
    });

    context.run();
    std::printf("cpp17 h3 interop: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
