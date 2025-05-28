#ifndef _SIMPLE_HTTP_H_
#define _SIMPLE_HTTP_H_

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <list>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <nghttp2/nghttp2.h>

#include <boost/asio.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/message_fwd.hpp>
#include <boost/beast/http/string_body_fwd.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

namespace simple_http
{

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

enum class LogLevel
{
    Info,
    Error,
};

enum class Version : uint8_t
{
    Http1 = 0,
    Http11 = 1,
    Http2 = 2,
};

inline std::shared_ptr<http::response<http::string_body>> makeHttpResponse(
    http::status status = http::status::ok,
    std::string_view content_type = "text/plain")
{
    auto res = std::make_shared<http::response<http::string_body>>();
    res->version(11);
    res->result(status);
    res->set(http::field::server, "simple_http_server");
    res->set(http::field::content_type, content_type);
    return res;
}

inline std::shared_ptr<http::request<http::string_body>> makeHttpRequest(
    const std::string &path,
    http::verb method = http::verb::post,
    Version http_version = Version::Http11)
{
    auto req = std::make_shared<http::request<http::string_body>>(
        method, path, http_version == Version::Http11 ? 11 : 10);
    req->set(http::field::user_agent, "simpe_http_client");
    return req;
}

constexpr int32_t CHANNEL_SIZE = 100000;

inline constexpr std::string_view to_string(LogLevel level) noexcept
{
    switch (level)
    {
        case LogLevel::Info:
            return "Info";
        case LogLevel::Error:
            return "Error";
        default:
            return "Unknown";
    }
}

inline std::function<void(LogLevel, std::string_view, int, std::string)>
    LOG_CB = [](auto, auto, auto, auto) {};

#define INFO(...) log(LogLevel::Info, __FILE__, __LINE__, __VA_ARGS__)
#define ERROR(...) log(LogLevel::Error, __FILE__, __LINE__, __VA_ARGS__)

template <typename... Args>
inline void log(LogLevel level,
                std::string_view file,
                int line,
                std::format_string<Args...> fmt,
                Args &&...args)
{
    LOG_CB(level, file, line, std::format(fmt, std::forward<Args>(args)...));
}

using error_code = boost::system::error_code;
using namespace boost::asio::experimental::awaitable_operators;

class IoCtxPool final
{
  public:
    IoCtxPool(std::size_t pool_size)
        : m_next_io_context(0), m_pool_size(pool_size)
    {
        if (pool_size == 0)
            throw std::runtime_error("ContextPool size is 0");
        for (std::size_t i = 0; i < pool_size + 1; ++i)
        {
            auto io_context_ptr = std::make_shared<asio::io_context>();
            m_io_contexts.emplace_back(io_context_ptr);
            m_work.emplace_back(
                asio::require(io_context_ptr->get_executor(),
                              asio::execution::outstanding_work.tracked));
        }
    }

    void start()
    {
        for (auto &context : m_io_contexts)
            m_threads.emplace_back([&] { context->run(); });
    }

    void stop()
    {
        for (auto &context_ptr : m_io_contexts)
            context_ptr->stop();
        for (auto &thread : m_threads)
        {
            if (thread.joinable())
                thread.join();
        }
    }

    auto &getIoContext()
    {
        size_t index =
            m_next_io_context.fetch_add(1, std::memory_order_relaxed);
        return *m_io_contexts[index % m_pool_size];
    }

    auto &getIoContextPtr()
    {
        size_t index =
            m_next_io_context.fetch_add(1, std::memory_order_relaxed);
        return m_io_contexts[index % m_pool_size];
    }

    auto &getMainContext()
    {
        return m_io_contexts.back();
    }

  private:
    std::vector<std::shared_ptr<asio::io_context>> m_io_contexts;
    std::shared_ptr<asio::io_context> m_main_ioctx;
    std::list<asio::any_io_executor> m_work{};
    std::atomic_uint64_t m_next_io_context;
    std::vector<std::thread> m_threads;
    uint64_t m_pool_size;
};

struct DataContext
{
    const char *data;
    size_t total_len;
    size_t offset;
    std::shared_ptr<void> input_data{nullptr};
};

inline ssize_t dataReadCallback(nghttp2_session * /* session */,
                                int32_t /* stream_id */,
                                uint8_t *buf,
                                size_t length,
                                uint32_t *data_flags,
                                nghttp2_data_source *source,
                                void * /* user_data */)
{
    auto *ctx = static_cast<DataContext *>(source->ptr);

    size_t remaining = ctx->total_len - ctx->offset;
    size_t to_copy = remaining < length ? remaining : length;

    memcpy(buf, ctx->data + ctx->offset, to_copy);
    ctx->offset += to_copy;

    if (ctx->offset >= ctx->total_len)
    {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        delete ctx;
    }

    return to_copy;
}

const char base64_url_alphabet[] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
                                    'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
                                    'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
                                    'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
                                    'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
                                    'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
                                    'w', 'x', 'y', 'z', '0', '1', '2', '3',
                                    '4', '5', '6', '7', '8', '9', '-', '_'};

inline std::string base64_decode(const std::string &in)
{
    std::string out;
    std::vector<int> T(256, -1);
    unsigned int i;
    for (i = 0; i < 64; i++)
        T[base64_url_alphabet[i]] = i;

    int val = 0, valb = -8;
    for (i = 0; i < in.length(); i++)
    {
        unsigned char c = in[i];
        if (T[c] == -1)
            break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0)
        {
            out.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

inline bool isHttp2(const std::string &cache_data)
{
    if (cache_data.empty() || cache_data.size() < 6)
        return false;
    if (cache_data[0] == 0x50 && cache_data[1] == 0x52 &&
        cache_data[2] == 0x49 && cache_data[3] == 0x20 &&
        cache_data[4] == 0x2A && cache_data[5] == 0x20)
    {
        return true;
    }
    else
    {
        return false;
    }
}

struct Disconnect
{
};

class HttpResponseWriter;

using Http1Channel = asio::experimental::concurrent_channel<
    void(error_code,
         std::variant<std::shared_ptr<http::response<http::string_body>>,
                      std::string,
                      Disconnect>)>;

using Http2Channel = asio::experimental::concurrent_channel<
    void(error_code, std::variant<std::shared_ptr<std::string>, Disconnect>)>;

inline bool callHandler(auto &map_proc,
                        auto &io_dispatch,
                        auto req,
                        auto writer)
{
    std::string path = req.target();
    if (auto pos = path.find('?'); pos != std::string::npos)
    {
        path = path.substr(0, pos);
    }
    if (map_proc.contains(path))
    {
        asio::co_spawn(io_dispatch,
                       map_proc[path](std::move(req), std::move(writer)),
                       asio::detached);
        return true;
    }
    else
    {
        asio::co_spawn(io_dispatch,
                       map_proc["*"](std::move(req), std::move(writer)),
                       asio::detached);
        return false;
    }
}

class Http2Parse final : public std::enable_shared_from_this<Http2Parse>
{
  public:
    struct Config
    {
        bool is_h2c_upgrade;
        std::string h2_setting;
        int32_t concurrent_streams{200};
        std::optional<int32_t> window_size;
        std::optional<int32_t> max_frame_size;
        http::verb method;
    };

    Http2Parse(const std::shared_ptr<Http2Channel> &ch2,
               const std::shared_ptr<Http1Channel> &ch1,
               auto io_context,
               auto &map_proc)
        : m_h2_channel(ch2),
          m_h1_channel(ch1),
          m_io_dispatch(std::move(io_context)),
          m_map_proc(map_proc)
    {
    }

    ~Http2Parse()
    {
        if (m_session)
        {
            nghttp2_session_callbacks_del(m_cbs);
            nghttp2_session_del(m_session);
        }
    }

    int init(const Config &cfg)
    {
        nghttp2_session_callbacks_new(&m_cbs);
        nghttp2_session_callbacks_set_on_header_callback(m_cbs,
                                                         onHeaderCallback);
        nghttp2_session_callbacks_set_send_callback(m_cbs, sendCallback);
        nghttp2_session_callbacks_set_on_frame_recv_callback(
            m_cbs, onFrameRecvCallback);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
            m_cbs, onDataChunkRecvCallback);

        nghttp2_session_server_new(&m_session, m_cbs, this);

        if (cfg.is_h2c_upgrade)
        {
            auto http2_settings_base64 = base64_decode(cfg.h2_setting);
            auto ret = nghttp2_session_upgrade2(
                m_session,
                (uint8_t *)http2_settings_base64.data(),
                http2_settings_base64.size(),
                cfg.method == http::verb::head ? 1 : 0,
                nullptr);
            if (ret)
            {
                INFO("nghttp2_session_upgrade2 error: {}",
                     nghttp2_strerror(ret));
                return ret;
            }
        }

        std::vector<nghttp2_settings_entry> iv;
        iv.emplace_back(NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS,
                        cfg.concurrent_streams);
        if (cfg.window_size.has_value())
        {
            iv.emplace_back(NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE,
                            cfg.window_size.value());
        }
        if (cfg.max_frame_size.has_value())
        {
            iv.emplace_back(NGHTTP2_SETTINGS_MAX_FRAME_SIZE,
                            cfg.max_frame_size.value());
        }
        nghttp2_submit_settings(m_session,
                                NGHTTP2_FLAG_NONE,
                                iv.data(),
                                iv.size());
        nghttp2_session_send(m_session);

        return 0;
    }

    bool writeHeaderEnd(std::unordered_map<std::string, std::string> headers,
                        int32_t stream_id,
                        std::string http_status)
    {
        if (auto sp = m_h2_channel.lock())
        {
            std::weak_ptr<Http2Parse> self = shared_from_this();
            asio::post(sp->get_executor(),
                       [this,
                        self = std::move(self),
                        headers = std::move(headers),
                        stream_id,
                        http_status = std::move(http_status)]() mutable {
                           if (auto sp = self.lock())
                           {
                               std::vector<nghttp2_nv> hdrs;
                               auto fill = [](const auto &name,
                                              const auto &value,
                                              auto &hdrs) {
                                   nghttp2_nv nv;
                                   nv.name = (uint8_t *)name.c_str();
                                   nv.namelen = name.size();
                                   nv.value = (uint8_t *)value.c_str();
                                   nv.valuelen = value.size();
                                   nv.flags = NGHTTP2_NV_FLAG_NONE;
                                   hdrs.push_back(nv);
                               };
                               static std::string status{":status"};
                               fill(status, http_status, hdrs);
                               for (auto &[name, value] : headers)
                               {
                                   fill(name, value, hdrs);
                               }
                               nghttp2_submit_headers(m_session,
                                                      NGHTTP2_FLAG_END_HEADERS,
                                                      stream_id,
                                                      nullptr,
                                                      hdrs.data(),
                                                      hdrs.size(),
                                                      nullptr);
                               nghttp2_session_send(m_session);
                           }
                       });
            return true;
        }
        return false;
    }

    bool writeBody(std::string data,
                   int32_t stream_id,
                   nghttp2_flag flag = NGHTTP2_FLAG_NONE)
    {
        if (auto sp = m_h2_channel.lock())
        {
            std::weak_ptr<Http2Parse> self = shared_from_this();
            asio::post(sp->get_executor(),
                       [this,
                        self = std::move(self),
                        data = std::make_shared<std::string>(std::move(data)),
                        flag,
                        stream_id] {
                           if (auto sp = self.lock())
                           {
                               nghttp2_data_provider data_prd;
                               data_prd.read_callback = dataReadCallback;
                               auto *ctx =
                                   new DataContext{.data = data->c_str(),
                                                   .total_len = data->size(),
                                                   .offset = 0,
                                                   .input_data = data};
                               nghttp2_data_source source;
                               source.ptr = ctx;
                               data_prd.source = source;
                               nghttp2_submit_data(m_session,
                                                   flag,
                                                   stream_id,
                                                   &data_prd);
                               nghttp2_session_send(m_session);
                           }
                       });
            return true;
        }
        return false;
    }

    bool writeChunkData(std::string header)
    {
        if (auto sp = m_h1_channel.lock())
        {
            if (!sp->try_send(error_code{}, std::move(header)))
            {
                ERROR("writeChunkData error");
                return false;
            }
            return true;
        }
        return false;
    }

    bool writeHttp1Response(
        const std::shared_ptr<http::response<http::string_body>>
            &http_1_response)
    {
        if (auto sp = m_h1_channel.lock())
        {
            if (!sp->try_send(error_code{}, http_1_response))
                return false;
            return true;
        }
        return false;
    }

    static int onHeaderCallback(nghttp2_session * /* session */,
                                const nghttp2_frame *frame,
                                const uint8_t *_name,
                                size_t namelen,
                                const uint8_t *_value,
                                size_t valuelen,
                                uint8_t /* flags */,
                                void *userdata)
    {
        int32_t stream_id = frame->hd.stream_id;
        auto h2p = static_cast<Http2Parse *>(userdata);
        auto req = h2p->getStreamCtx(stream_id);
        std::string name{(char *)_name, namelen};
        std::string_view value{(char *)_value, valuelen};
        std::ranges::transform(name, name.begin(), [](unsigned char c) {
            return std::tolower(c);
        });
        if (name == ":method")
        {
            req->method(http::string_to_verb(value));
        }
        if (name == ":path")
        {
            req->target(value);
        }
        req->set(name, value);
        return 0;
    }

    static ssize_t sendCallback(nghttp2_session * /* session */,
                                const uint8_t *data,
                                size_t length,
                                int /* flags */,
                                void *userdata)
    {
        auto h2p = static_cast<Http2Parse *>(userdata);
        auto sp = h2p->m_h2_channel.lock();
        if (sp == nullptr)
            return length;
        if (!sp->try_send(error_code{},
                          std::make_shared<std::string>((char *)data, length)))
        {
            ERROR("sendCallback send error!!!!");
        }
        return length;
    }

    static int onFrameRecvCallback(nghttp2_session * /* session */,
                                   const nghttp2_frame *frame,
                                   void *userdata)
    {
        auto call_handler = [&] {
            int32_t stream_id = frame->hd.stream_id;
            auto h2p = static_cast<Http2Parse *>(userdata);
            auto req = h2p->getStreamCtx(stream_id);
            req->prepare_payload();
            auto writer =
                std::make_shared<HttpResponseWriter>(h2p->shared_from_this(),
                                                     stream_id,
                                                     Version::Http2);
            if (auto sp = h2p->m_h2_channel.lock())
            {
                callHandler(h2p->m_map_proc,
                            *h2p->m_io_dispatch,
                            std::move(*req),
                            std::move(writer));
            }
            h2p->erase(stream_id);
        };

        if (frame->hd.type == NGHTTP2_HEADERS &&
            frame->headers.cat == NGHTTP2_HCAT_REQUEST)
        {
            if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
            {
                call_handler();
            }
        }

        if (frame->hd.type == NGHTTP2_DATA &&
            (frame->hd.flags & NGHTTP2_FLAG_END_STREAM))
        {
            call_handler();
        }

        return 0;
    }

    static int onDataChunkRecvCallback(nghttp2_session * /* session */,
                                       uint8_t /* flags */,
                                       int32_t stream_id,
                                       const uint8_t *data,
                                       size_t len,
                                       void *userdata)
    {
        auto h2p = static_cast<Http2Parse *>(userdata);
        auto req = h2p->getStreamCtx(stream_id);
        req->body().append((char *)data, len);
        return 0;
    }

    int feedRecvData(const char *data, size_t len)
    {
        size_t ret =
            nghttp2_session_mem_recv(m_session, (const uint8_t *)data, len);
        if (ret != len)
        {
            ERROR("nghttp2 error: {}", nghttp2_strerror(ret));
            return -1;
        }
        return (int)ret;
    }

    std::shared_ptr<http::request<http::string_body>> &getStreamCtx(
        int32_t stream_id)
    {
        if (!m_streams.contains(stream_id))
        {
            m_streams[stream_id] =
                std::make_shared<http::request<http::string_body>>();
        }
        return m_streams[stream_id];
    }

    void erase(int32_t stream_id)
    {
        m_streams.erase(stream_id);
    }

    std::weak_ptr<Http2Channel> m_h2_channel;
    std::weak_ptr<Http1Channel> m_h1_channel;
    std::shared_ptr<asio::io_context> m_io_dispatch;
    nghttp2_session_callbacks *m_cbs{};
    nghttp2_session *m_session{};
    std::unordered_map<int32_t,
                       std::shared_ptr<http::request<http::string_body>>>
        m_streams;
    std::unordered_map<std::string,
                       std::function<asio::awaitable<void>(
                           http::request<http::string_body>,
                           std::shared_ptr<HttpResponseWriter>)>> &m_map_proc;
};

class HttpResponseWriter
{
  public:
    HttpResponseWriter(const std::shared_ptr<Http2Parse> &http2_helper,
                       int32_t stream_id,
                       Version version)
        : m_http2_parse(http2_helper),
          m_stream_id(stream_id),
          m_version(version)
    {
    }

    ~HttpResponseWriter()
    {
        if (m_version == Version::Http2 && !m_write_h2_header_done)
        {
            writeHeaderEnd();
        }
        if (m_version == Version::Http2 && !m_write_h2_body_done)
        {
            writeBodyEnd("");
        }
    }

    void writeStatus(int32_t http_status)
    {
        m_http_status = std::to_string(http_status);
    }

    void writeStatus(http::status http_status)
    {
        writeStatus(static_cast<int32_t>(http_status));
    }

    template <typename Key, typename Value>
    void writeHeader(Key &&key, Value &&value)
    {
        if constexpr (std::is_same_v<std::decay_t<Key>, http::field>)
        {
            m_headers.emplace(http::to_string(std::forward<Key>(key)),
                              std::forward<Value>(value));
        }
        else
        {
            m_headers.emplace(std::forward<Key>(key),
                              std::forward<Value>(value));
        }
    }

    void writeHeader(std::unordered_map<std::string, std::string> headers)
    {
        m_headers.merge(headers);
    }

    bool writeHeaderEnd()
    {
        if (m_version != Version::Http2)
            return false;
        static std::string server = http::to_string(http::field::server);
        if (!m_headers.contains(server))
        {
            writeHeader(http::field::server, "simple_http_server");
        }
        m_write_h2_header_done = true;
        return m_http2_parse->writeHeaderEnd(std::move(m_headers),
                                             m_stream_id,
                                             std::move(m_http_status));
    }

    template <typename T>
    bool writeBody(T &&data, nghttp2_flag flag = NGHTTP2_FLAG_NONE)
    {
        if (m_version != Version::Http2)
            return false;
        static_assert(std::is_constructible_v<std::string, T &&>,
                      "T must be convertible to std::string");
        return m_http2_parse->writeBody(std::forward<T>(data),
                                        m_stream_id,
                                        flag);
    }

    template <typename T>
    bool writeBodyEnd(T &&data)
    {
        if (m_version != Version::Http2)
            return false;
        m_write_h2_body_done = true;
        return writeBody(std::forward<T>(data), NGHTTP2_FLAG_END_STREAM);
    }

    // for http1.1 chunk
    bool writeChunkHeader(const http::response<http::empty_body> &res)
    {
        std::stringstream ss;
        ss << res.base();
        return m_http2_parse->writeChunkData(ss.str());
    }

    bool writeChunkData(const std::string &data)
    {
        std::ostringstream oss;
        oss << std::hex << data.length() << "\r\n";
        oss << data << "\r\n";
        return m_http2_parse->writeChunkData(oss.str());
    }

    bool writeChunkEnd()
    {
        static std::string close_stream{"0\r\n\r\n"};
        return m_http2_parse->writeChunkData(close_stream);
    }

    bool connected()
    {
        if (m_version == Version::Http2)
        {
            return m_http2_parse->m_h2_channel.lock() ? true : false;
        }
        else
        {
            return m_http2_parse->m_h1_channel.lock() ? true : false;
        }
    }

    void forceClose()
    {
        if (m_version == Version::Http2)
        {
            if (auto sp = m_http2_parse->m_h2_channel.lock())
            {
                asio::post(sp->get_executor(),
                           [sp] { sp->try_send(error_code{}, Disconnect{}); });
            }
        }
        else
        {
            if (auto sp = m_http2_parse->m_h1_channel.lock())
            {
                asio::post(sp->get_executor(),
                           [sp] { sp->try_send(error_code{}, Disconnect{}); });
            }
        }
    }

    bool writeHttpResponse(
        const std::shared_ptr<http::response<http::string_body>> &http_response)
    {
        if (m_version == Version::Http2)
        {
            m_http_status = std::to_string(http_response->result_int());
            for (const auto &field : http_response->base())
            {
                writeHeader(field.name_string(), field.value());
            }
            writeHeaderEnd();
            writeBodyEnd(http_response->body());
            return true;
        }
        else
        {
            return m_http2_parse->writeHttp1Response(http_response);
        }
    }

    auto version()
    {
        return m_version;
    }

  private:
    std::shared_ptr<Http2Parse> m_http2_parse;
    int32_t m_stream_id;
    Version m_version;
    std::string m_http_status{"200"};
    std::unordered_map<std::string, std::string> m_headers;
    bool m_write_h2_header_done{false};
    bool m_write_h2_body_done{false};
};

inline asio::awaitable<void> toSocket(
    auto socket,
    std::shared_ptr<Http2Channel> ch,
    std::shared_ptr<std::chrono::steady_clock::time_point> deadline)
{
    for (;;)
    {
        auto [ec, data] =
            co_await ch->async_receive(asio::as_tuple(asio::use_awaitable));
        if (ec)
        {
            break;
        }
        *deadline = std::chrono::steady_clock::now();
        if (std::holds_alternative<Disconnect>(data))
        {
            // forceClose
            break;
        }
        auto &info_ptr = std::get<std::shared_ptr<std::string>>(data);
        if (auto [ec, nwritten] =
                co_await async_write(*socket,
                                     asio::buffer(info_ptr->c_str(),
                                                  info_ptr->size()),
                                     asio::as_tuple(asio::use_awaitable));
            ec)
        {
            break;
        }
    }
    co_return;
}

inline asio::awaitable<void> toH2Parse(
    auto socket,
    auto h2p,
    std::shared_ptr<std::chrono::steady_clock::time_point> deadline)
{
    char buffer[4096];
    for (;;)
    {
        auto [ec, nread] = co_await socket->async_read_some(
            asio::buffer(buffer, sizeof(buffer)),
            asio::as_tuple(asio::use_awaitable));
        if (ec)
        {
            break;
        }
        *deadline = std::chrono::steady_clock::now();
        auto ret = h2p->feedRecvData(buffer, nread);
        if (ret == -1)
        {
            break;
        }
    }
};

void shutdown(const auto &socket)
{
    if constexpr (std::is_same_v<std::shared_ptr<asio::ip::tcp::socket>,
                                 std::decay_t<decltype(socket)>>)
    {
        if (socket->is_open())
        {
            error_code ec;
            socket->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            socket->close(ec);
        }
    }
    else
    {
        error_code ec;
        socket->shutdown(ec);
        socket->next_layer().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket->next_layer().close(ec);
    }
}

inline asio::awaitable<void> watchdog(
    std::shared_ptr<std::chrono::steady_clock::time_point> deadline,
    std::chrono::seconds interval)
{
    asio::steady_timer timer(co_await asio::this_coro::executor);
    while (true)
    {
        timer.expires_at(std::chrono::steady_clock::now() + interval);
        co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        auto now = std::chrono::steady_clock::now();
        if (now - *deadline >= interval)
        {
            INFO("timeout");
            break;
        }
    }
    co_return;
}

struct Config
{
    std::string ip;
    uint16_t port;
    uint16_t worker_num{4};
    int32_t concurrent_streams{200};
    std::optional<int32_t> window_size;
    std::optional<int32_t> max_frame_size;
    std::chrono::seconds max_idle_time{120};
    std::string ssl_crt;
    std::string ssl_key;
};

class HttpServer final
{
  public:
    HttpServer(const Config &cfg)
        : m_cfg(cfg),
          m_ep(asio::ip::make_address(cfg.ip), cfg.port),
          m_ssl_context(asio::ssl::context::tlsv13_server),
          m_io_ctx_pool(cfg.worker_num),
          m_io_dispatch(std::make_shared<IoCtxPool>(cfg.worker_num))
    {
        m_io_ctx_pool.start();
        m_io_dispatch->start();

        if (cfg.ssl_crt.empty() || cfg.ssl_key.empty())
            return;
        m_ssl_context.set_options(asio::ssl::context::default_workarounds |
                                  asio::ssl::context::no_tlsv1 |
                                  asio::ssl::context::no_tlsv1_1 |
                                  asio::ssl::context::no_tlsv1_2);
        error_code ec;
        [[maybe_unused]]
        auto ret = m_ssl_context.use_certificate_chain_file(cfg.ssl_crt, ec);
        if (ec)
            throw std::runtime_error(ec.message());

        [[maybe_unused]] auto _ =
            m_ssl_context.use_private_key_file(cfg.ssl_key,
                                               asio::ssl::context::pem,
                                               ec);
        if (ec)
            throw std::runtime_error(ec.message());

        SSL_CTX_set_alpn_select_cb(
            m_ssl_context.native_handle(),
            [](SSL * /* ssl */,
               const unsigned char **out,
               unsigned char *outlen,
               const unsigned char *in,
               unsigned int inlen,
               void * /* arg */) {
                static const unsigned char alpn_proto_list[] = {
                    0x02, 'h', '2'  // length-prefixed: "\x02h2"
                };
                if (SSL_select_next_proto((unsigned char **)out,
                                          outlen,
                                          alpn_proto_list,
                                          sizeof(alpn_proto_list),
                                          in,
                                          inlen) != OPENSSL_NPN_NEGOTIATED)
                {
                    return SSL_TLSEXT_ERR_NOACK;
                }
                return SSL_TLSEXT_ERR_OK;
            },
            nullptr);
    }

    void stop()
    {
        auto ctx = m_io_ctx_pool.getMainContext();
        std::promise<void> done;
        asio::post(*ctx, [&] {
            if (m_acceptor)
                m_acceptor->close();
            done.set_value();
        });
        done.get_future().wait();
        m_io_ctx_pool.stop();
        m_io_dispatch->stop();
    }

    asio::awaitable<void> upgradeH2c(
        auto socket,
        const std::shared_ptr<asio::io_context> & /* ctx */,
        http::request<http::string_body> req,
        std::string settings)
    {
        // nghttp --upgrade  http://127.0.0.1:6666/hello --data ./a.txt
        // curl -v --http2 http://localhost:6666/hello -d "aaaa" -k
        http::response<http::empty_body> res{http::status::switching_protocols,
                                             11};
        res.set(http::field::connection, "Upgrade");
        res.set(http::field::upgrade, "h2c");
        co_await http::async_write(*socket,
                                   res,
                                   asio::as_tuple(asio::use_awaitable));

        auto &io_dispatch = m_io_dispatch->getIoContextPtr();
        auto ch =
            std::make_shared<Http2Channel>(co_await asio::this_coro::executor,
                                           CHANNEL_SIZE);
        auto h2p =
            std::make_shared<Http2Parse>(ch, nullptr, io_dispatch, m_map_proc);
        if (auto ret = h2p->init(Http2Parse::Config{
                .is_h2c_upgrade = true,
                .h2_setting = std::move(settings),
                .concurrent_streams = m_cfg.concurrent_streams,
                .window_size = m_cfg.window_size,
                .max_frame_size = m_cfg.max_frame_size,
                .method = req.method(),
            });
            ret)
        {
            ERROR("init error: {}", ret);
            co_return;
        }

        if (req.method() != http::verb::options)
        {
            callHandler(m_map_proc,
                        *io_dispatch,
                        std::move(req),
                        std::make_shared<HttpResponseWriter>(h2p,
                                                             1,
                                                             Version::Http2));
        }
        auto deadline = std::make_shared<std::chrono::steady_clock::time_point>(
            std::chrono::steady_clock::now());
        co_await (toH2Parse(socket, h2p, deadline) ||
                  toSocket(socket, ch, deadline) ||
                  watchdog(deadline, m_cfg.max_idle_time));
        shutdown(socket);
    }

    asio::awaitable<void> switchH2c(
        auto socket,
        const std::shared_ptr<asio::io_context> & /* ctx */,
        const std::string &buffer)
    {
        // curl -v --http2-prior-knowledge http://localhost:6666/hello
        // curl -v --http2-prior-knowledge http://localhost:6666/hello -d "aaaa"
        auto &io_dispatch = m_io_dispatch->getIoContextPtr();
        // start proc http2
        auto ch =
            std::make_shared<Http2Channel>(co_await asio::this_coro::executor,
                                           CHANNEL_SIZE);
        auto h2p =
            std::make_shared<Http2Parse>(ch, nullptr, io_dispatch, m_map_proc);
        if (auto ret = h2p->init(Http2Parse::Config{
                .is_h2c_upgrade = false,
                .h2_setting = "",
                .concurrent_streams = m_cfg.concurrent_streams,
                .window_size = m_cfg.window_size,
                .max_frame_size = m_cfg.max_frame_size,
                .method = http::verb::get,  // not use
            });
            ret)
        {
            ERROR("init error: {}", ret);
            co_return;
        }
        auto ret = h2p->feedRecvData(buffer.c_str(), buffer.size());
        if (ret == -1)
        {
            co_return;
        }
        auto deadline = std::make_shared<std::chrono::steady_clock::time_point>(
            std::chrono::steady_clock::now());
        co_await (toH2Parse(socket, h2p, deadline) ||
                  toSocket(socket, ch, deadline) ||
                  watchdog(deadline, m_cfg.max_idle_time));
        shutdown(socket);
    }

    asio::awaitable<void> switchHttp1(auto socket,
                                      std::shared_ptr<Http1Channel> http1_ch,
                                      std::shared_ptr<Http2Parse> h2p,
                                      Version version)
    {
        auto recv_request = [this](auto socket,
                                   auto h2p,
                                   auto deadline) -> asio::awaitable<void> {
            for (;;)
            {
                beast::flat_buffer buffer;
                http::request<http::string_body> req;
                auto [ec, count] = co_await http::async_read(
                    *socket, buffer, req, asio::as_tuple(asio::use_awaitable));
                if (ec)
                {
                    co_return;
                }
                *deadline = std::chrono::steady_clock::now();
                auto version =
                    (req.version() == 11 ? Version::Http11 : Version::Http1);
                callHandler(m_map_proc,
                            m_io_dispatch->getIoContext(),
                            std::move(req),
                            std::make_shared<HttpResponseWriter>(h2p,
                                                                 0,
                                                                 version));
            }
        };
        auto send_response = [](auto socket,
                                auto http1_ch,
                                Version version,
                                auto deadline) -> asio::awaitable<void> {
            for (;;)
            {
                auto [ec, h1_rsp] = co_await http1_ch->async_receive(
                    asio::as_tuple(asio::use_awaitable));
                if (ec)
                {
                    break;
                }
                *deadline = std::chrono::steady_clock::now();
                if (std::holds_alternative<std::string>(h1_rsp))
                {
                    auto &body = std::get<std::string>(h1_rsp);
                    if (auto [ec, count] = co_await asio::async_write(
                            *socket,
                            asio::buffer(body.data(), body.size()),
                            asio::as_tuple(asio::use_awaitable));
                        ec)
                    {
                        break;
                    }
                }
                else if (std::holds_alternative<std::shared_ptr<
                             http::response<http::string_body>>>(h1_rsp))
                {
                    auto &body = std::get<
                        std::shared_ptr<http::response<http::string_body>>>(
                        h1_rsp);
                    if (auto [ec, count] = co_await http::async_write(
                            *socket,
                            *body,
                            asio::as_tuple(asio::use_awaitable));
                        ec)
                    {
                        break;
                    }
                }
                else
                {
                    // forceClose
                    break;
                }
                if (version == Version::Http1)
                    break;
            }
        };
        auto deadline = std::make_shared<std::chrono::steady_clock::time_point>(
            std::chrono::steady_clock::now());
        co_await (recv_request(socket, h2p, deadline) ||
                  send_response(socket, http1_ch, version, deadline) ||
                  watchdog(deadline, m_cfg.max_idle_time));
        shutdown(socket);
        co_return;
    }

    asio::awaitable<void> session(auto socket,
                                  const std::shared_ptr<asio::io_context> &ctx)
    {
        auto http1_ch = std::make_shared<Http1Channel>(*ctx, CHANNEL_SIZE);
        auto h2p = std::make_shared<Http2Parse>(nullptr,
                                                http1_ch,
                                                nullptr,
                                                m_map_proc);
        beast::flat_buffer buffer;
        http::parser<true, http::string_body> parser;
        auto [ec, bytes] = co_await http::async_read_header(
            *socket, buffer, parser, asio::as_tuple(asio::use_awaitable));
        if (ec == http::error::end_of_stream)
        {
            co_return;
        }
        if (ec == http::error::bad_version)
        {
            auto req_str = beast::buffers_to_string(buffer.data());
            if (isHttp2(req_str))
            {
                co_await switchH2c(std::move(socket), ctx, req_str);
            }
            else
            {
                ERROR("not http2 request");
            }
            co_return;
        }
        if (ec)
        {
            co_return;
        }
        auto &headers = parser.get();

        std::string h2_setting;
        if (headers.find(http::field::upgrade) != headers.end() &&
            headers[http::field::upgrade] == "h2c")
        {
            h2_setting = headers[http::field::http2_settings];
        }

        std::tie(ec, bytes) = co_await http::async_read(
            *socket, buffer, parser, asio::as_tuple(asio::use_awaitable));
        if (ec)
        {
            ERROR("body read error: {}", ec.message());
            co_return;
        }

        http::request<http::string_body> full_req = parser.get();

        if (!h2_setting.empty())
        {
            co_await upgradeH2c(std::move(socket),
                                ctx,
                                std::move(full_req),
                                std::move(h2_setting));
            co_return;
        }

        // this is http1 or 1.1
        auto version =
            (full_req.version() == 11 ? Version::Http11 : Version::Http1);
        callHandler(m_map_proc,
                    m_io_dispatch->getIoContext(),
                    std::move(full_req),
                    std::make_shared<HttpResponseWriter>(h2p, 0, version));
        co_await switchHttp1(std::move(socket), http1_ch, h2p, version);

        co_return;
    }

    // https server
    asio::awaitable<void> startSslsession(
        std::shared_ptr<asio::ssl::stream<asio::ip::tcp::socket>> socket,
        auto context)
    {
        if (auto [ec] = co_await socket->async_handshake(
                boost::asio::ssl::stream_base::server,
                asio::as_tuple(asio::use_awaitable));
            ec)
        {
            ERROR("async_handshake: {}", ec.message());
            co_return;
        }
        asio::co_spawn(*context,
                       session(std::move(socket), context),
                       asio::detached);
    }

    asio::awaitable<void> start()
    {
        m_acceptor = std::make_unique<asio::ip::tcp::acceptor>(
            *m_io_ctx_pool.getMainContext());
        m_acceptor->open(m_ep.protocol());
        error_code ec;
        m_acceptor->set_option(asio::ip::tcp::acceptor::reuse_address(true));
        m_acceptor->bind(m_ep);
        m_acceptor->listen(asio::socket_base::max_listen_connections, ec);
        if (ec)
        {
            ERROR("listen: {}", ec.message());
            throw std::runtime_error(ec.message());
        }
        for (;;)
        {
            auto &context = m_io_ctx_pool.getIoContextPtr();
            asio::ip::tcp::socket socket(*context);
            auto [ec] = co_await m_acceptor->async_accept(
                socket, asio::as_tuple(asio::use_awaitable));
            if (ec)
            {
                if (ec == asio::error::operation_aborted)
                    break;
                continue;
            }
            auto endpoint = socket.remote_endpoint(ec);
            if (!ec)
            {
                std::stringstream ss;
                ss << endpoint;
                INFO("new connection from:[{}]", ss.str());
            }
            socket.set_option(asio::socket_base::keep_alive(true));
            socket.set_option(asio::ip::tcp::no_delay(true));
            if (m_cfg.ssl_crt.empty())
            {
                asio::co_spawn(*context,
                               session(std::make_shared<asio::ip::tcp::socket>(
                                           std::move(socket)),
                                       context),
                               asio::detached);
            }
            else
            {
                auto stream =
                    std::make_shared<asio::ssl::stream<asio::ip::tcp::socket>>(
                        std::move(socket), m_ssl_context);
                asio::co_spawn(*context,
                               startSslsession(std::move(stream), context),
                               asio::detached);
            }
        }
    }

    void setHttpHandler(const std::string &path,
                        std::function<asio::awaitable<void>(
                            http::request<http::string_body>,
                            std::shared_ptr<HttpResponseWriter>)> _cb)
    {
        m_map_proc[path] = std::move(_cb);
    }

    void setUnhandled(std::function<asio::awaitable<void>(
                          http::request<http::string_body>,
                          std::shared_ptr<HttpResponseWriter>)> _cb)
    {
        m_map_proc["*"] = std::move(_cb);
    }

    Config m_cfg;
    asio::ip::tcp::endpoint m_ep;
    asio::ssl::context m_ssl_context;
    IoCtxPool m_io_ctx_pool;
    std::shared_ptr<IoCtxPool> m_io_dispatch;
    std::unique_ptr<asio::ip::tcp::acceptor> m_acceptor;
    std::unordered_map<std::string,
                       std::function<asio::awaitable<void>(
                           http::request<http::string_body>,
                           std::shared_ptr<HttpResponseWriter>)>>
        m_map_proc{
            {"*", [](auto, auto writer) -> asio::awaitable<void> {
                 if (writer->version() == simple_http::Version::Http2)
                 {
                     writer->writeStatus(404);
                     writer->writeHeader("content-type", "text/plain");
                     writer->writeHeader(http::field::server, "simple_http");
                     writer->writeHeaderEnd();
                     writer->writeBodyEnd("");
                 }
                 else
                 {
                     http::response<http::string_body> res{
                         http::status::not_found, 11};
                     res.set(http::field::content_type, "text/plain");
                     res.set(http::field::server, "simple_http");
                     res.body() = "";
                     res.prepare_payload();
                     writer->writeHttpResponse(
                         std::make_shared<http::response<http::string_body>>(
                             res));
                 }
                 co_return;
             }}};
};

#ifdef _EXPERIMENT_HTTP_CLIENT_

struct HttpClient
{
    using Channel = asio::experimental::concurrent_channel<
        void(error_code,
             std::variant<http::response<http::empty_body>,
                          std::string,
                          Disconnect>)>;

    using ReqChannel = asio::experimental::concurrent_channel<
        void(error_code,
             std::tuple<std::shared_ptr<http::request<http::string_body>>,
                        std::shared_ptr<Channel>>)>;

    HttpClient() = default;
    virtual ~HttpClient() = default;

    virtual asio::awaitable<bool> start() = 0;
    virtual asio::awaitable<void> stop() = 0;
    virtual asio::awaitable<std::shared_ptr<Channel>> sendRequest(
        std::shared_ptr<http::request<http::string_body>> req) = 0;
};

class HttpsClient final : public HttpClient,
                          public std::enable_shared_from_this<HttpsClient>
{
  public:
    HttpsClient(std::string host,
                uint16_t port,
                std::shared_ptr<asio::io_context> ctx,
                int32_t timeout = 60)
        : m_host(std::move(host)),
          m_port(port),
          m_ctx(std::move(ctx)),
          m_timeout(timeout),
          m_ssl_context(asio::ssl::context::tlsv13_client)
    {
        m_h2_channel = std::make_shared<Http2Channel>(*m_ctx, CHANNEL_SIZE);
        m_req_channel = std::make_shared<ReqChannel>(*m_ctx, CHANNEL_SIZE);

        m_ssl_context.set_verify_mode(SSL_VERIFY_PEER);
        m_ssl_context.set_default_verify_paths();
        const unsigned char alpn_protos[] = {0x02, 'h', '2'};
        SSL_CTX_set_alpn_protos(m_ssl_context.native_handle(),
                                alpn_protos,
                                sizeof(alpn_protos));
    }

    asio::awaitable<bool> start() override
    {
        co_await asio::dispatch(
            asio::bind_executor(m_ctx->get_executor(), asio::use_awaitable));

        if (m_connected)
            co_return true;

        auto solver = asio::ip::tcp::resolver(*m_ctx);
        auto [ec, results] =
            co_await solver.async_resolve(m_host,
                                          std::to_string(m_port),
                                          asio::as_tuple(asio::use_awaitable));
        if (ec)
        {
            ERROR("async_resolve: {}", ec.message());
            co_return false;
        }

        asio::ip::tcp::socket socket(*m_ctx);
        asio::steady_timer timer(*m_ctx);
        timer.expires_after(std::chrono::seconds(10));
        auto result = co_await (
            socket.async_connect(*(results.begin()),
                                 asio::as_tuple(asio::use_awaitable)) ||
            timer.async_wait(asio::as_tuple(asio::use_awaitable)));
        if (result.index() == 0)
        {
            auto [ec] = std::get<0>(result);
            if (ec)
            {
                ERROR("async_connect: {}", ec.message());
                co_return false;
            }
        }
        else if (result.index() == 1)
        {
            ERROR("async_connect timeout");
            co_return false;
        }

        m_socket = std::make_unique<asio::ssl::stream<asio::ip::tcp::socket>>(
            std::move(socket), m_ssl_context);

        if (!SSL_set_tlsext_host_name(m_socket->native_handle(),
                                      m_host.c_str()))
        {
            ec = boost::system::error_code(static_cast<int>(::ERR_get_error()),
                                           asio::error::get_ssl_category());
            ERROR("SSL_set_tlsext_host_name: {}", ec.message());
            co_return false;
        }

        if (auto [ec] = co_await m_socket->async_handshake(
                asio::ssl::stream_base::client,
                asio::as_tuple(asio::use_awaitable));
            ec)
        {
            ERROR("async_handshake: {}", ec.message());
            co_return false;
        }
        const unsigned char *protocol = nullptr;
        unsigned int length = 0;

        SSL_get0_alpn_selected(m_socket->native_handle(), &protocol, &length);

        if (length == 2 && std::memcmp(protocol, "h2", 2) == 0)
        {
            INFO("Negotiated ALPN: h2");
            m_h2 = true;
            initNghttp2();
            co_await startHttp2Cleint();
            m_connected = true;
        }
        else
        {
            INFO("ALPN negotiation failed or not h2.");
            m_h2 = false;
        }

        co_return true;
    }

    asio::awaitable<void> stop() override
    {
        co_await asio::dispatch(
            asio::bind_executor(m_ctx->get_executor(), asio::use_awaitable));
        if (m_socket)
        {
            shutdown(m_socket);
        }
        if (m_session)
        {
            nghttp2_session_callbacks_del(m_cbs);
            nghttp2_session_del(m_session);
            m_session = nullptr;
        }
        co_return;
    }

    int initNghttp2()
    {
        if (m_session)
        {
            nghttp2_session_callbacks_del(m_cbs);
            nghttp2_session_del(m_session);
            m_session = nullptr;
        }
        nghttp2_session_callbacks_new(&m_cbs);
        nghttp2_session_callbacks_set_on_header_callback(m_cbs,
                                                         onHeaderCallback);
        nghttp2_session_callbacks_set_send_callback(m_cbs, sendCallback);
        nghttp2_session_callbacks_set_on_frame_recv_callback(
            m_cbs, onFrameRecvCallback);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
            m_cbs, onDataChunkRecvCallback);

        nghttp2_session_client_new(&m_session, m_cbs, this);

        std::vector<nghttp2_settings_entry> iv;
        iv.emplace_back(NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 200);

        nghttp2_submit_settings(m_session,
                                NGHTTP2_FLAG_NONE,
                                iv.data(),
                                iv.size());
        nghttp2_session_send(m_session);

        return 0;
    }

    static int onHeaderCallback(nghttp2_session * /* session */,
                                const nghttp2_frame *frame,
                                const uint8_t *_name,
                                size_t namelen,
                                const uint8_t *_value,
                                size_t valuelen,
                                uint8_t /* flags */,
                                void *userdata)
    {
        int32_t stream_id = frame->hd.stream_id;
        auto cli = static_cast<HttpsClient *>(userdata);
        if (cli->m_streams.contains(stream_id))
        {
            std::string name{(char *)_name, namelen};
            std::string_view value{(char *)_value, valuelen};
            std::ranges::transform(name, name.begin(), [](unsigned char c) {
                return std::tolower(c);
            });
            if (name == ":status")
            {
                cli->m_streams[stream_id]->header.result(
                    std::stoul(value.data()));
            }
            else
            {
                cli->m_streams[stream_id]->header.set(name, value);
            }
        }
        else
        {
            ERROR("not found : {}", stream_id);
        }
        return 0;
    }

    static ssize_t sendCallback(nghttp2_session * /* session */,
                                const uint8_t *data,
                                size_t length,
                                int /* flags */,
                                void *userdata)
    {
        auto h2_cli = static_cast<HttpsClient *>(userdata);
        if (!h2_cli->m_h2_channel->try_send(
                error_code{},
                std::make_shared<std::string>((char *)data, length)))
        {
            ERROR("sendCallback send error!!!!");
        }
        return length;
    }

    static int onFrameRecvCallback(nghttp2_session * /* session */,
                                   const nghttp2_frame *frame,
                                   void *userdata)
    {
        auto call_handler = [&] {
            int32_t stream_id = frame->hd.stream_id;
            auto h2_cli = static_cast<HttpsClient *>(userdata);
            if (h2_cli->m_streams.contains(stream_id))
            {
                h2_cli->m_streams[stream_id]->close();
            }
            else
            {
                ERROR("not found : {}", stream_id);
            }
            h2_cli->m_streams.erase(stream_id);
        };

        if (frame->hd.type == NGHTTP2_HEADERS &&
            frame->headers.cat == NGHTTP2_HCAT_RESPONSE)
        {
            int32_t stream_id = frame->hd.stream_id;
            auto h2_cli = static_cast<HttpsClient *>(userdata);
            if (h2_cli->m_streams.contains(stream_id))
            {
                h2_cli->m_streams[stream_id]->sendHttpHeader();
            }

            if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
            {
                call_handler();
            }
        }

        if (frame->hd.type == NGHTTP2_DATA &&
            (frame->hd.flags & NGHTTP2_FLAG_END_STREAM))
        {
            call_handler();
        }

        return 0;
    }

    static int onDataChunkRecvCallback(nghttp2_session * /* session */,
                                       uint8_t /* flags */,
                                       int32_t stream_id,
                                       const uint8_t *data,
                                       size_t len,
                                       void *userdata)
    {
        auto h2_cli = static_cast<HttpsClient *>(userdata);
        if (h2_cli->m_streams.contains(stream_id))
        {
            h2_cli->m_streams[stream_id]->sendHttpBody(
                std::string{(char *)data, len});
        }
        else
        {
            ERROR("not found : {}", stream_id);
        }
        return 0;
    }

    asio::awaitable<std::shared_ptr<Channel>> sendRequest(
        std::shared_ptr<http::request<http::string_body>> req) override
    {
        auto channel = std::make_shared<Channel>(*m_ctx, CHANNEL_SIZE);
        auto tp = std::make_tuple(std::move(req), channel);
        if (!m_req_channel->try_send(error_code{}, tp))
        {
            auto [ec] = co_await m_req_channel->async_send(
                error_code{}, tp, asio::as_tuple(asio::use_awaitable));
            if (ec)
            {
                co_return nullptr;
            }
        }
        co_return channel;
    }

    int feedRecvData(const char *data, size_t len)
    {
        size_t ret =
            nghttp2_session_mem_recv2(m_session, (const uint8_t *)data, len);
        if (ret != len)
        {
            ERROR("nghttp2 error: {}", nghttp2_strerror(ret));
            return -1;
        }
        return (int)ret;
    }

    bool connected()
    {
        return m_connected.load(std::memory_order_relaxed);
    }

  private:
    asio::awaitable<void> forwardRequest()
    {
        auto submit_request = [this](auto tp) mutable {
            auto &[req, ch] = tp;
            std::vector<nghttp2_nv> hdrs;
            auto fill =
                [](std::string_view name, std::string_view value, auto &hdrs) {
                    nghttp2_nv nv;
                    nv.name = (uint8_t *)name.data();
                    nv.namelen = name.size();
                    nv.value = (uint8_t *)value.data();
                    nv.valuelen = value.size();
                    nv.flags = NGHTTP2_NV_FLAG_NONE;
                    hdrs.push_back(nv);
                };

            fill(":path", req->target(), hdrs);
            fill(":scheme", "https", hdrs);
            fill(":authority", m_host, hdrs);
            std::string method_str = http::to_string(req->method());
            fill(":method", method_str, hdrs);

            for (const auto &field : *req)
            {
                fill(field.name_string(), field.value(), hdrs);
            }

            const auto &post_data = req->body();

            nghttp2_data_provider data_prd;
            auto *ctx = new DataContext{.data = post_data.data(),
                                        .total_len = post_data.size(),
                                        .offset = 0,
                                        .input_data = req};
            data_prd.source.ptr = ctx;
            data_prd.read_callback = dataReadCallback;
            int stream_id = nghttp2_submit_request(m_session,
                                                   nullptr,
                                                   hdrs.data(),
                                                   hdrs.size(),
                                                   &data_prd,
                                                   nullptr);

            nghttp2_session_send(m_session);
            if (stream_id < 0)
            {
                ERROR("Failed to submit POST request: {}",
                      nghttp2_strerror(stream_id));
                ch->close();
                return -1;
            }
            m_streams.emplace(stream_id,
                              std::make_shared<Response>(std::move(ch)));
            return 0;
        };
        for (;;)
        {
            auto [ec, tp] = co_await m_req_channel->async_receive(
                asio::as_tuple(asio::use_awaitable));
            if (ec)
            {
                break;
            }
            auto [req, ch] = tp;
            submit_request(std::move(tp));
        }
    }

    asio::awaitable<void> startHttp2Cleint()
    {
        auto func = [](auto socket,
                       auto h2_channel,
                       std::weak_ptr<HttpsClient> self,
                       auto timeout) -> asio::awaitable<void> {
            auto sp = self.lock();
            auto deadline =
                std::make_shared<std::chrono::steady_clock::time_point>(
                    std::chrono::steady_clock::now());
            co_await (toH2Parse(socket, sp, deadline) ||
                      toSocket(socket, h2_channel, deadline) ||
                      sp->forwardRequest() ||
                      watchdog(deadline, std::chrono::seconds(timeout)));
            sp->m_connected = false;
            // disconnect
            for (auto &[id, rsp] : sp->m_streams)
            {
                rsp->m_channel->try_send(error_code{}, Disconnect{});
            }
            sp->m_streams.clear();
        };
        asio::co_spawn(
            *m_ctx,
            func(m_socket, m_h2_channel, shared_from_this(), m_timeout),
            asio::detached);
        co_return;
    }

    std::string m_host;
    uint16_t m_port;
    std::shared_ptr<asio::io_context> m_ctx;
    int32_t m_timeout;

    asio::ssl::context m_ssl_context;
    std::shared_ptr<asio::ssl::stream<asio::ip::tcp::socket>> m_socket;
    bool m_h2;
    nghttp2_session_callbacks *m_cbs{};
    nghttp2_session *m_session{};
    std::shared_ptr<Http2Channel> m_h2_channel;
    std::shared_ptr<ReqChannel> m_req_channel;
    std::atomic_bool m_connected{false};

    struct Response
    {
        Response(std::shared_ptr<Channel> ch) : m_channel(std::move(ch))
        {
        }

        bool sendHttpHeader()
        {
            return m_channel->try_send(error_code{}, std::move(header));
        }

        bool sendHttpBody(std::string body)
        {
            return m_channel->try_send(error_code{}, std::move(body));
        }

        void close()
        {
            if (m_channel)
                m_channel->close();
        }

        http::response<http::empty_body> header;
        std::shared_ptr<Channel> m_channel;
    };

    std::unordered_map<int32_t, std::shared_ptr<Response>> m_streams;
};

#endif

}  // namespace simple_http

#endif  // _SIMPLE_HTTP_H_
