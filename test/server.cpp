#include <chrono>
#include <cstdio>
#include <ostream>
#include <string>
#include <iostream>

#include <boost/url.hpp>

#include "simple_http.h"
#include <zlib.h>

#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filtering_streambuf.hpp>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

#include <zlib.h>
#include <iostream>
#include <string>

class GzipStream
{
  public:
    GzipStream()
    {
        stream_.zalloc = Z_NULL;
        stream_.zfree = Z_NULL;
        stream_.opaque = Z_NULL;
        deflateInit2(&stream_,
                     Z_DEFAULT_COMPRESSION,
                     Z_DEFLATED,
                     16 + MAX_WBITS,
                     8,
                     Z_DEFAULT_STRATEGY);
    }

    std::string compress_chunk(const std::string &input, bool flush = false)
    {
        std::string output;
        char buffer[4096];

        stream_.avail_in = input.size();
        stream_.next_in = (Bytef *)input.data();

        do
        {
            stream_.avail_out = sizeof(buffer);
            stream_.next_out = (Bytef *)buffer;

            int ret = deflate(&stream_, flush ? Z_SYNC_FLUSH : Z_NO_FLUSH);
            if (ret == Z_STREAM_ERROR)
                throw std::runtime_error("Compression error");

            output.append(buffer, sizeof(buffer) - stream_.avail_out);
        } while (stream_.avail_out == 0);

        return output;
    }

    std::string finish()
    {
        std::string output;
        char buffer[4096];
        int ret;

        do
        {
            stream_.avail_out = sizeof(buffer);
            stream_.next_out = (Bytef *)buffer;
            ret = deflate(&stream_, Z_FINISH);
            output.append(buffer, sizeof(buffer) - stream_.avail_out);
        } while (ret != Z_STREAM_END);

        deflateEnd(&stream_);
        return output;
    }

  private:
    z_stream stream_;
};

std::string compress(const std::string &data)
{
    namespace bio = boost::iostreams;
    std::istringstream origin(data);

    bio::filtering_istreambuf in;
    in.push(
        bio::gzip_compressor(bio::gzip_params(bio::gzip::best_compression)));
    in.push(origin);

    std::ostringstream compressed;
    bio::copy(in, compressed);
    return compressed.str();
}

std::string decompress(const std::string &data)
{
    namespace bio = boost::iostreams;
    std::istringstream compressed(data);

    bio::filtering_istreambuf in;
    in.push(bio::gzip_decompressor());
    in.push(compressed);

    std::ostringstream origin;
    bio::copy(in, origin);
    return origin.str();
}

asio::awaitable<void> start()
{
    simple_http::Config cfg{.ip = "0.0.0.0",
                            .port = 6666,
                            .worker_num = 8,
                            .concurrent_streams = 200};
    cfg.ssl_crt = "./v.crt";
    cfg.ssl_key = "./v.key";
    simple_http::HttpServer hs(cfg);
    simple_http::LOG_CB =
        [](simple_http::LogLevel level, auto file, auto line, std::string msg) {
            std::cout << to_string(level) << " " << file << ":" << line << " "
                      << msg << std::endl;
        };
    hs.setBefore(
        [](const auto &req, const auto &writer) -> asio::awaitable<bool> {
#if 0
        boost::urls::url_view urlv =
            boost::urls::parse_origin_form(req.target()).value();
        if (urlv.path() != "/hello")
        {
            auto res = simple_http::makeHttpResponse(http::status::bad_request);
            res->prepare_payload();
            writer->writeHttpResponse(res);
            co_return false;
        }
        for (auto const &param : urlv.params())
        {
            std::cout << param.key << " = " << param.value << "\n";
        }
#endif
            co_return true;
        });
    hs.setHttpHandler(
        "/hello", [](auto req, auto writer) -> asio::awaitable<void> {
            // std::cout << "Headers:" << std::endl;
            // // std::println("meth: {}", std::string{req.method_string()});
            // for (auto const &field : req)
            // {
            //     std::cout << field.name_string() << ": " << field.value()
            //               << "\n";
            // }
            // std::cout << req.target() << std::endl;
            // auto str = req.body();
            // std::cout << "body:" << str << std::endl;
            if (writer->version() == simple_http::Version::Http2)
            {
#if 0
                auto res = simple_http::makeHttpResponse(http::status::ok);
                res->set(http::field::content_encoding, "gzip");
                auto aa = compress(req.body());
                res->body() = aa;
                writer->writeHttpResponse(res);
#else
                writer->writeHeader("content-type", "text/plain");
                writer->writeHeader(http::field::server, "test");
                writer->writeHeader(http::field::content_encoding, "gzip");
                writer->writeHeaderEnd();
                GzipStream gs;
                writer->writeBody(gs.compress_chunk("123", true));
                writer->writeBody(gs.compress_chunk("456", true));
                writer->writeBody(gs.compress_chunk("789", true));
                writer->writeBodyEnd(gs.finish());
#endif
            }
            else
            {
#if 0
                auto res = simple_http::makeHttpResponse(http::status::ok);
                res->body() = "hello world";
                res->prepare_payload();
                writer->writeHttpResponse(res);
#else
                // curl --no-buffer  -v http://localhost:6666/hello -d "aaaa"
                http::response<http::empty_body> res{http::status::ok, 11};
                res.set(http::field::server, "simple_http_server");
                res.set(http::field::content_type, "text/plain");
                res.set(http::field::transfer_encoding, "chunked");  // 关键字段
                res.keep_alive(true);
                writer->writeChunkHeader(res);
                writer->writeChunkData("123");
                asio::steady_timer timer(co_await asio::this_coro::executor);
                timer.expires_after(std::chrono::seconds(1));
                co_await timer.async_wait(asio::use_awaitable);
                writer->writeChunkData("456");
                timer.expires_after(std::chrono::seconds(1));
                co_await timer.async_wait(asio::use_awaitable);
                writer->writeChunkEnd();
#endif
            }
            co_return;
        });
    hs.setHttpHandler("/world",
                      [](auto /* req */, auto writer) -> asio::awaitable<void> {
                          auto res =
                              simple_http::makeHttpResponse(http::status::ok);
                          res->body() =
                              writer->version() == simple_http::Version::Http2
                                  ? "/world http2 body"
                                  : "/world http1.1 body";
                          res->prepare_payload();
                          writer->writeHttpResponse(res);
                          co_return;
                      })
        .setHttpRegexHandler(
            ".*", [](auto /* req */, auto writer) -> asio::awaitable<void> {
                auto res = simple_http::makeHttpResponse(http::status::ok);
                res->body() = "regex matched";
                res->prepare_payload();
                writer->writeHttpResponse(res);
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
