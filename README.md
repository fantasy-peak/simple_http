# simple_http

```
curl -v --http2-prior-knowledge http://localhost:6666/hello\?key1\=value1\&key2\=value2
curl -v --http2-prior-knowledge http://localhost:6666/hello -d "aaaa"

curl -v --http2 http://localhost:6666/hello -d "aaaa" -k
nghttp --upgrade -v http://127.0.0.1:6666/hello
nghttp --upgrade -v http://nghttp2.org 
h2load -n 60000 -c 1000 -m 200 -H 'Content-Type: application/json' --data=b.txt http://localhost:6666/hello
```

```
asio::awaitable<void> start()
{
    simple_http::Config cfg{.ip = "0.0.0.0",
                            .port = 6666,
                            .worker_num = 4,
                            .concurrent_streams = 200};
    simple_http::HttpServer hs(cfg);
    simple_http::LOG_CB =
        [](simple_http::LogLevel level, auto file, auto line, std::string msg) {
            std::osyncstream out(std::cout);
            out << to_string(level) << " " << file << ":" << line << " " << msg
                << std::endl;
        };
    hs.setBefore([](const auto &req,
                    const auto &writer) -> asio::awaitable<bool> {
        std::cout << "setBefore:" << req.target() << std::endl;
        if (req.target() != "/hello")
        {
            auto res = simple_http::makeHttpResponse(http::status::bad_request);
            res->prepare_payload();
            writer->writeHttpResponse(res);
            co_return false;
        }
        co_return true;
    });
    hs.setHttpHandler(
        "/hello", [](auto req, auto writer) -> asio::awaitable<void> {
            if (writer->version() == simple_http::Version::Http2)
            {
#if 0
                auto res = simple_http::makeHttpResponse(http::status::ok);
                res->body() = "hello h2";
                writer->writeHttpResponse(res);
#else
                writer->writeHeader("content-type", "text/plain");
                writer->writeHeader(http::field::server, "test");
                writer->writeHeaderEnd();
                writer->writeBody("123");
                writer->writeBody("456");
                writer->writeBodyEnd("789");
#endif
            }
            co_return;
        });
    co_await hs.start();
}

int main()
{
    simple_http::IoCtxPool pool{1};
    pool.start();
    asio::co_spawn(pool.getIoContext(), start(), asio::detached);
    while (true)
        sleep(1000);
    return 0;
}
```