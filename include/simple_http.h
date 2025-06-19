#ifndef _trojan_H_
#define _trojan_H_

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
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
#include <algorithm>

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
#include <boost/asio.hpp>
#include <openssl/ssl.h>

namespace trojan
{

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

enum class LogLevel : uint8_t
{
    Debug,
    Info,
    Error,
};

inline constexpr std::string_view to_string(LogLevel level) noexcept
{
    switch (level)
    {
        case LogLevel::Debug:
            return "Debug";
        case LogLevel::Info:
            return "Info";
        case LogLevel::Error:
            return "Error";
        default:
            return "Unknown";
    }
}

class ScopeExit
{
  public:
    ScopeExit(const ScopeExit &) = delete;
    ScopeExit(ScopeExit &&) = delete;
    ScopeExit &operator=(const ScopeExit &) = delete;
    ScopeExit &operator=(ScopeExit &&) = delete;

    template <typename Callable>
    explicit ScopeExit(Callable &&call) : m_call(std::forward<Callable>(call))
    {
    }

    ~ScopeExit()
    {
        if (m_call)
            m_call();
    }

    void clear()
    {
        m_call = decltype(m_call)();
    }

  private:
    std::function<void()> m_call;
};

inline std::function<void(LogLevel, std::string_view, int, std::string)>
    LOG_CB = [](auto, auto, auto, auto) {};

#define TROJAN_DEBUG_LOG(...) \
    trojan::log(trojan::LogLevel::Debug, __FILE__, __LINE__, __VA_ARGS__)
#define TROJAN_INFO_LOG(...) \
    trojan::log(trojan::LogLevel::Info, __FILE__, __LINE__, __VA_ARGS__)
#define TROJAN_ERROR_LOG(...) \
    trojan::log(trojan::LogLevel::Error, __FILE__, __LINE__, __VA_ARGS__)

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
        for (std::size_t i = 0; i < pool_size; ++i)
        {
            create();
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

    void createMainContext()
    {
        create();
    }

  private:
    void create()
    {
        auto io_context_ptr = std::make_shared<asio::io_context>();
        m_io_contexts.emplace_back(io_context_ptr);
        m_work.emplace_back(
            asio::require(io_context_ptr->get_executor(),
                          asio::execution::outstanding_work.tracked));
    }

    std::vector<std::shared_ptr<asio::io_context>> m_io_contexts;
    std::shared_ptr<asio::io_context> m_main_ioctx;
    std::list<asio::any_io_executor> m_work{};
    std::atomic_uint64_t m_next_io_context;
    std::vector<std::thread> m_threads;
    uint64_t m_pool_size;
};

asio::awaitable<void> forward(auto from_socket,
                              auto to_socket,
                              auto deadline,
                              auto &cfg)
{
    std::vector<char> buffer(32 * 1024);
    for (;;)
    {
        *deadline = std::chrono::steady_clock::now() + cfg.read_write_max_idle;
        auto [ec, length] = co_await from_socket->async_read_some(
            asio::buffer(buffer), asio::as_tuple(asio::use_awaitable));
        if (ec)
        {
            break;
        }
        // TROJAN_INFO_LOG("forward {} bytes", length);
        if (auto [ec, len] =
                co_await asio::async_write(*to_socket,
                                           asio::buffer(buffer, length),
                                           asio::as_tuple(asio::use_awaitable));
            ec)
        {
            break;
        }
    }
    co_return;
}

class Socks5Address
{
  public:
    enum AddressType
    {
        IPv4 = 1,
        DOMAINNAME = 3,
        IPv6 = 4
    } address_type;

    std::string address;
    uint16_t port;

    bool parse(const std::string &data, size_t &address_len)
    {
        if (data.length() == 0 ||
            (data[0] != IPv4 && data[0] != DOMAINNAME && data[0] != IPv6))
        {
            return false;
        }
        address_type = static_cast<AddressType>(data[0]);
        switch (address_type)
        {
            case IPv4:
            {
                if (data.length() > 4 + 2)
                {
                    address = std::to_string(uint8_t(data[1])) + '.' +
                              std::to_string(uint8_t(data[2])) + '.' +
                              std::to_string(uint8_t(data[3])) + '.' +
                              std::to_string(uint8_t(data[4]));
                    port = (uint8_t(data[5]) << 8) | uint8_t(data[6]);
                    address_len = 1 + 4 + 2;
                    return true;
                }
                break;
            }
            case DOMAINNAME:
            {
                uint8_t domain_len = data[1];
                if (domain_len == 0)
                {
                    // invalid domain len
                    break;
                }
                if (data.length() > (unsigned int)(1 + domain_len + 2))
                {
                    address = data.substr(2, domain_len);
                    port = (uint8_t(data[domain_len + 2]) << 8) |
                           uint8_t(data[domain_len + 3]);
                    address_len = 1 + 1 + domain_len + 2;
                    return true;
                }
                break;
            }
            case IPv6:
            {
                if (data.length() > 16 + 2)
                {
                    char t[40];
                    sprintf(
                        t,
                        "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
                        "%02x%02x:%02x%02x",
                        uint8_t(data[1]),
                        uint8_t(data[2]),
                        uint8_t(data[3]),
                        uint8_t(data[4]),
                        uint8_t(data[5]),
                        uint8_t(data[6]),
                        uint8_t(data[7]),
                        uint8_t(data[8]),
                        uint8_t(data[9]),
                        uint8_t(data[10]),
                        uint8_t(data[11]),
                        uint8_t(data[12]),
                        uint8_t(data[13]),
                        uint8_t(data[14]),
                        uint8_t(data[15]),
                        uint8_t(data[16]));
                    address = t;
                    port = (uint8_t(data[17]) << 8) | uint8_t(data[18]);
                    address_len = 1 + 16 + 2;
                    return true;
                }
                break;
            }
        }
        return false;
    }

    static std::string generate(const boost::asio::ip::udp::endpoint &endpoint)
    {
        if (endpoint.address().is_unspecified())
        {
            return std::string("\x01\x00\x00\x00\x00\x00\x00", 7);
        }
        std::string ret;
        if (endpoint.address().is_v4())
        {
            ret += '\x01';
            auto ip = endpoint.address().to_v4().to_bytes();
            for (int i = 0; i < 4; ++i)
            {
                ret += char(ip[i]);
            }
        }
        if (endpoint.address().is_v6())
        {
            ret += '\x04';
            auto ip = endpoint.address().to_v6().to_bytes();
            for (int i = 0; i < 16; ++i)
            {
                ret += char(ip[i]);
            }
        }
        ret += char(uint8_t(endpoint.port() >> 8));
        ret += char(uint8_t(endpoint.port() & 0xFF));
        return ret;
    }
};

class UdpPacket
{
  public:
    Socks5Address address;
    uint16_t length;
    std::string payload;

    bool parse(const std::string &data, size_t &udp_packet_len)
    {
        if (data.length() <= 0)
        {
            return false;
        }
        size_t address_len;
        bool is_addr_valid = address.parse(data, address_len);
        if (!is_addr_valid || data.length() < address_len + 2)
        {
            return false;
        }
        length =
            (uint8_t(data[address_len]) << 8) | uint8_t(data[address_len + 1]);
        if (data.length() < address_len + 4 + length ||
            data.substr(address_len + 2, 2) != "\r\n")
        {
            return false;
        }
        payload = data.substr(address_len + 4, length);
        udp_packet_len = address_len + 4 + length;
        return true;
    }

    static std::string generate(const boost::asio::ip::udp::endpoint &endpoint,
                                const std::string &payload)
    {
        std::string ret = Socks5Address::generate(endpoint);
        ret += char(uint8_t(payload.length() >> 8));
        ret += char(uint8_t(payload.length() & 0xFF));
        ret += "\r\n";
        ret += payload;
        return ret;
    }

    static std::string generate(const std::string &domainname,
                                uint16_t port,
                                const std::string &payload)
    {
        std::string ret = "\x03";
        ret += char(uint8_t(domainname.length()));
        ret += domainname;
        ret += char(uint8_t(port >> 8));
        ret += char(uint8_t(port & 0xFF));
        ret += char(uint8_t(payload.length() >> 8));
        ret += char(uint8_t(payload.length() & 0xFF));
        ret += "\r\n";
        ret += payload;
        return ret;
    }
};

class TrojanRequest
{
  public:
    std::string password;

    enum Command : uint8_t
    {
        CONNECT = 1,
        UDP_ASSOCIATE = 3
    } command;

    Socks5Address address;
    std::string payload;

    int parse(const std::string &data)
    {
        size_t first = data.find("\r\n");
        if (first == std::string::npos)
        {
            return -1;
        }
        password = data.substr(0, first);
        payload = data.substr(first + 2);
        if (payload.length() == 0 || (payload[0] != Command::CONNECT &&
                                      payload[0] != Command::UDP_ASSOCIATE))
        {
            return -1;
        }
        command = static_cast<Command>(payload[0]);
        size_t address_len;
        bool is_addr_valid = address.parse(payload.substr(1), address_len);
        if (!is_addr_valid || payload.length() < address_len + 3 ||
            payload.substr(address_len + 1, 2) != "\r\n")
        {
            return -1;
        }
        // address_len = ATYP + DST.ADDR + DST.PORT
        payload = payload.substr(address_len + 3);
        return data.length();
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
    std::shared_ptr<std::chrono::steady_clock::time_point> deadline)
{
    asio::steady_timer timer(co_await asio::this_coro::executor);

    auto now = std::chrono::steady_clock::now();
    while (*deadline > now)
    {
        timer.expires_at(*deadline);
        co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        now = std::chrono::steady_clock::now();
    }
    co_return;
}

inline asio::awaitable<void> timeout(
    std::chrono::steady_clock::duration duration)
{
    asio::steady_timer timer(co_await asio::this_coro::executor);
    timer.expires_after(duration);
    co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
}

inline std::string SHA224(const std::string &message)
{
    uint8_t digest[EVP_MAX_MD_SIZE];
    char mdString[(EVP_MAX_MD_SIZE << 1) + 1];
    unsigned int digest_len;
    EVP_MD_CTX *ctx;
    if ((ctx = EVP_MD_CTX_new()) == nullptr)
    {
        throw std::runtime_error("could not create hash context");
    }
    if (!EVP_DigestInit_ex(ctx, EVP_sha224(), nullptr))
    {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("could not initialize hash context");
    }
    if (!EVP_DigestUpdate(ctx, message.c_str(), message.length()))
    {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("could not update hash");
    }
    if (!EVP_DigestFinal_ex(ctx, digest, &digest_len))
    {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("could not output hash");
    }

    for (unsigned int i = 0; i < digest_len; ++i)
    {
        sprintf(mdString + (i << 1), "%02x", (unsigned int)digest[i]);
    }
    mdString[digest_len << 1] = '\0';
    EVP_MD_CTX_free(ctx);
    return mdString;
}

inline asio::awaitable<void> http301(
    auto &socket,
    const std::string &url = "https://www.baidu.com")
{
    namespace http = boost::beast::http;
    http::response<http::empty_body> res{http::status::moved_permanently, 11};
    res.set(http::field::location, url);
    res.set(http::field::server, "nginx/1.20.1");
    std::stringstream ss;
    ss << res;
    auto res_str = ss.str();
    co_await asio::async_write(*socket,
                               asio::buffer(res_str.c_str(), res_str.size()),
                               asio::as_tuple(asio::use_awaitable));
}

inline long long getCurrentTimestampMs()
{
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               now.time_since_epoch())
        .count();
}

struct Config
{
    std::string ip;
    uint16_t port;
    uint16_t worker_num{4};
    std::string ssl_crt;
    std::string ssl_key;
    std::string passwd;
    std::string path;
    std::chrono::seconds timeout{10};
    std::chrono::seconds read_write_max_idle{60};
    std::chrono::minutes dns_cache_time{30};
};

class Server final
{
  public:
    Server(const Config &cfg)
        : m_cfg(cfg),
          m_ep(asio::ip::make_address(cfg.ip), cfg.port),
          m_io_ctx_pool(std::make_shared<IoCtxPool>(cfg.worker_num)),
          m_stop_ctx_pool(true)
    {
        m_io_ctx_pool->createMainContext();
        m_io_ctx_pool->start();
        m_cfg.passwd = SHA224(m_cfg.passwd);
        asio::co_spawn(*m_io_ctx_pool->getMainContext(), dns(), asio::detached);
        initSsl();
    }

    Server(const Config &cfg, std::shared_ptr<IoCtxPool> io_ctx_pool)
        : m_cfg(cfg),
          m_ep(asio::ip::make_address(cfg.ip), cfg.port),
          m_io_ctx_pool(std::move(io_ctx_pool)),
          m_stop_ctx_pool(false)
    {
        m_cfg.passwd = SHA224(m_cfg.passwd);
        asio::co_spawn(*m_io_ctx_pool->getMainContext(), dns(), asio::detached);
        initSsl();
    }

    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;
    Server(Server &&) = delete;
    Server &operator=(Server &&) = delete;

    asio::awaitable<void> start()
    {
        m_acceptor = std::make_unique<asio::ip::tcp::acceptor>(
            *m_io_ctx_pool->getMainContext());
        m_acceptor->open(m_ep.protocol());
        error_code ec;
        m_acceptor->set_option(asio::ip::tcp::acceptor::reuse_address(true));
        [[maybe_unused]] auto _ = m_acceptor->bind(m_ep, ec);
        if (ec)
        {
            TROJAN_ERROR_LOG("bind: {}", ec.message());
            throw std::runtime_error(ec.message());
        }
        _ = m_acceptor->listen(asio::socket_base::max_listen_connections, ec);
        if (ec)
        {
            TROJAN_ERROR_LOG("listen: {}", ec.message());
            throw std::runtime_error(ec.message());
        }
        for (;;)
        {
            auto &context = m_io_ctx_pool->getIoContextPtr();
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
                TROJAN_DEBUG_LOG("new connection from [{}:{}]",
                                 endpoint.address().to_string(),
                                 endpoint.port());
            }
            socket.set_option(asio::socket_base::keep_alive(true));

            auto stream =
                std::make_shared<asio::ssl::stream<asio::ip::tcp::socket>>(
                    std::move(socket), m_ssl_context);

            asio::co_spawn(*context,
                           startSslsession(std::move(stream), context),
                           asio::detached);
        }
    }

    void stop()
    {
        auto ctx = m_io_ctx_pool->getMainContext();
        std::promise<void> done;
        asio::post(*ctx, [&] {
            if (m_acceptor)
                m_acceptor->close();
            done.set_value();
        });
        done.get_future().wait();
        if (m_stop_ctx_pool)
        {
            m_io_ctx_pool->stop();
        }
    }

  private:
    void initSsl()
    {
        if (m_cfg.ssl_crt.empty() || m_cfg.ssl_key.empty())
            return;

        uint64_t opts = asio::ssl::context::default_workarounds |
                        asio::ssl::context::no_tlsv1 |
                        asio::ssl::context::no_tlsv1_1 |
                        asio::ssl::context::no_tlsv1_2;

        m_ssl_context.set_options(opts);

        error_code ec;
        [[maybe_unused]]
        auto ret = m_ssl_context.use_certificate_chain_file(m_cfg.ssl_crt, ec);
        if (ec)
            throw std::runtime_error(ec.message());

        [[maybe_unused]] auto _ =
            m_ssl_context.use_private_key_file(m_cfg.ssl_key,
                                               asio::ssl::context::pem,
                                               ec);
        if (ec)
            throw std::runtime_error(ec.message());

        auto native = m_ssl_context.native_handle();
        SSL_CTX_set_session_cache_mode(native, SSL_SESS_CACHE_SERVER);
    }

    enum class ParseError : int8_t
    {
        NetworkError,
        PasswordError
    };

    asio::awaitable<std::expected<TrojanRequest, ParseError>> recvRequest(
        auto &socket,
        auto &deadline,
        auto &request,
        auto &cfg)
    {
        constexpr size_t MAX_REQUEST_SIZE = 1024 * 4;
        char buffer[MAX_REQUEST_SIZE];
        TrojanRequest req;
        for (;;)
        {
            *deadline =
                std::chrono::steady_clock::now() + cfg.read_write_max_idle;
            auto [ec, length] = co_await socket->async_read_some(
                asio::buffer(buffer, sizeof(buffer)),
                asio::as_tuple(asio::use_awaitable));
            if (ec)
            {
                TROJAN_ERROR_LOG("async_read_some: {}", ec.message());
                co_return std::unexpected(ParseError::NetworkError);
            }
            request.append(buffer, length);
            if (request.size() >= 58 &&
                !std::ranges::equal(request.begin(),
                                    request.begin() + 56,
                                    cfg.passwd.begin(),
                                    cfg.passwd.end()))
            {
                TROJAN_ERROR_LOG("invalid password: [{}]",
                                 std::string{request.begin(),
                                             request.begin() + 56});
                co_await http301(socket);
                co_return std::unexpected(ParseError::PasswordError);
            }
            if (req.parse(request) != -1)
            {
                break;
            }
            if (request.size() > MAX_REQUEST_SIZE)
            {
                TROJAN_ERROR_LOG("request size too large: {}", request.size());
                co_await http301(socket);
                co_return std::unexpected(ParseError::PasswordError);
            }
        }
        co_return req;
    }

    asio::awaitable<std::optional<
        std::vector<asio::ip::tcp::resolver::results_type::value_type>>>
    resolve(const TrojanRequest &req)
    {
        std::vector<asio::ip::tcp::resolver::results_type::value_type> results;

        auto async_resolve = [](auto &req)
            -> asio::awaitable<std::optional<std::vector<
                asio::ip::tcp::resolver::results_type::value_type>>> {
            auto solver =
                asio::ip::tcp::resolver(co_await asio::this_coro::executor);
            auto [ec, results] =
                co_await solver.async_resolve(req.address.address,
                                              std::to_string(req.address.port),
                                              asio::as_tuple(
                                                  asio::use_awaitable));
            if (ec)
            {
                TROJAN_ERROR_LOG("resolve [{}]: {}",
                                 req.address.address,
                                 ec.message());
                co_return std::nullopt;
            }

            std::vector<asio::ip::tcp::resolver::results_type::value_type>
                ipv4_entries;
            std::vector<asio::ip::tcp::resolver::results_type::value_type>
                ipv6_entries;

            for (const auto &entry : results)
            {
                if (entry.endpoint().address().is_v4())
                {
                    ipv4_entries.push_back(entry);
                }
                else
                {
                    ipv6_entries.push_back(entry);
                }
            }
            if (ipv4_entries.empty())
            {
                TROJAN_INFO_LOG("ipv4_entries: {} ipv6_entries: {}",
                                ipv4_entries.size(),
                                ipv6_entries.size());
            }

            std::vector<asio::ip::tcp::resolver::results_type::value_type>
                prioritized_entries;

            prioritized_entries.insert(prioritized_entries.end(),
                                       ipv4_entries.begin(),
                                       ipv4_entries.end());
            prioritized_entries.insert(prioritized_entries.end(),
                                       ipv6_entries.begin(),
                                       ipv6_entries.end());

            co_return prioritized_entries;
        };

        std::unique_lock<std::mutex> lock(m_mtx);
        if (m_results.contains(req.address.address))
        {
            results = m_results[req.address.address].results;
            lock.unlock();
        }
        else
        {
            lock.unlock();
            auto opt = co_await async_resolve(req);
            if (!opt.has_value())
            {
                co_return std::nullopt;
            }
            std::unique_lock<std::mutex> lock(m_mtx);
            results = opt.value();
            m_results[req.address.address] = CachedResult{
                .results = std::move(opt.value()),
            };
        }
        co_return results;
    }

    asio::awaitable<void> session(
        std::shared_ptr<asio::ssl::stream<asio::ip::tcp::socket>> socket,
        const std::shared_ptr<asio::io_context> &ctx)
    {
        ScopeExit ssl_auto_exit([&socket] {
            asio::co_spawn(
                socket->get_executor(),
                [](auto socket) -> asio::awaitable<void> {
                    co_await socket->async_shutdown(
                        asio::as_tuple(asio::use_awaitable));
                }(socket),
                asio::detached);
        });

        auto time_point =
            std::make_shared<std::chrono::steady_clock::time_point>(
                std::chrono::steady_clock::now());
        TrojanRequest req;

        {
            beast::flat_buffer buffer;
            http::parser<true, http::empty_body> parser;
            auto result = co_await (
                http::async_read_header(*socket,
                                        buffer,
                                        parser,
                                        asio::as_tuple(asio::use_awaitable)) ||
                timeout(m_cfg.timeout));
            if (result.index() == 1)
            {
                TROJAN_ERROR_LOG("async_read_header timeout");
                co_return;
            }
            auto [ec, bytes] = std::get<0>(result);
            if (ec == http::error::end_of_stream)
            {
                co_return;
            }
            if (ec == http::error::bad_method)
            {
                // 原始协议
                auto req_str = beast::buffers_to_string(buffer.data());
                if (req.parse(req_str) != -1)
                {
                    if (req.password != m_cfg.passwd)
                    {
                        TROJAN_ERROR_LOG("password error: {}", req.password);
                        co_await http301(socket);
                        co_return;
                    }
                }
                else
                {
                    auto result = co_await (
                        recvRequest(socket, time_point, req_str, m_cfg) ||
                        watchdog(time_point));
                    if (result.index() == 1)
                    {
                        TROJAN_ERROR_LOG("recvRequest timeout");
                        co_return;
                    }
                    auto exception_req = std::get<0>(result);
                    if (!exception_req.has_value())
                    {
                        co_return;
                    }
                    req = std::move(exception_req.value());
                }
            }
            else if (ec)
            {
                TROJAN_ERROR_LOG("{}", ec.message());
                co_await http301(socket);
                co_return;
            }
            else
            {
                auto &headers = parser.get();
                if (m_cfg.path != headers.target())
                {
                    TROJAN_ERROR_LOG("not expected path: {}",
                                     headers.target().data());
                    co_await http301(socket);
                    co_return;
                }

                http::response<http::empty_body> res{http::status::ok, 11};
                res.set(http::field::server, "nginx/1.20.1");
                res.keep_alive(true);
                auto [ec, bytes] = co_await http::async_write(
                    *socket, res, asio::as_tuple(asio::use_awaitable));
                if (ec)
                {
                    TROJAN_ERROR_LOG("http write error: {}", ec.message());
                    co_return;
                }

                std::string req_str;
                auto result =
                    co_await (recvRequest(socket, time_point, req_str, m_cfg) ||
                              watchdog(time_point));
                if (result.index() == 1)
                {
                    TROJAN_ERROR_LOG("recvRequest timeout");
                    co_return;
                }
                auto exception_req = std::get<0>(result);
                if (!exception_req.has_value())
                {
                    co_return;
                }
                req = std::move(exception_req.value());
            }
        }

        if (req.command == TrojanRequest::Command::CONNECT)
        {
            auto out_socket = std::make_shared<asio::ip::tcp::socket>(
                co_await asio::this_coro::executor);

            ScopeExit auto_exit([&out_socket] {
                if (!out_socket->is_open())
                {
                    return;
                }
                boost::system::error_code ec;
                out_socket->shutdown(
                    boost::asio::ip::tcp::socket::shutdown_both, ec);
                out_socket->close(ec);
            });

            auto results_opt = co_await resolve(req);
            if (!results_opt.has_value())
            {
                co_return;
            }
            auto &results = results_opt.value();

            auto start = getCurrentTimestampMs();
            auto result = co_await (
                asio::async_connect(*out_socket,
                                    results,
                                    asio::as_tuple(asio::use_awaitable)) ||
                timeout(m_cfg.timeout));
            if (result.index() == 0)
            {
                auto [ec, ret] = std::get<0>(result);
                if (ec)
                {
                    TROJAN_ERROR_LOG("connect: {}", ec.message());
                    {
                        std::unique_lock<std::mutex> lock(m_mtx);
                        m_results.erase(req.address.address);
                    }
                    co_return;
                }
            }
            else if (result.index() == 1)
            {
                TROJAN_ERROR_LOG("connect timeout: {}", req.address.address);
                {
                    std::unique_lock<std::mutex> lock(m_mtx);
                    m_results.erase(req.address.address);
                }
                co_return;
            }
            auto end = getCurrentTimestampMs();
            if (end - start > 50)
            {
                TROJAN_INFO_LOG("req.address.address: {}:{} -> {}",
                                req.address.address,
                                req.address.port,
                                end - start);
            }

            if (!req.payload.empty())
            {
                if (auto [ec, len] = co_await asio::async_write(
                        *out_socket,
                        asio::buffer(req.payload.data(), req.payload.size()),
                        asio::as_tuple(asio::use_awaitable));
                    ec)
                {
                    TROJAN_ERROR_LOG("async_write: {}", ec.message());
                    co_return;
                }
            }

            co_await (forward(socket, out_socket, time_point, m_cfg) ||
                      forward(out_socket, socket, time_point, m_cfg) ||
                      watchdog(time_point));
            co_return;
        }
        else
        {
            auto recv_udp = [](auto self,
                               auto payload,
                               auto socket,
                               auto deadline) -> asio::awaitable<void> {
                std::unordered_map<
                    std::string,
                    std::pair<asio::ip::udp::endpoint,
                              std::shared_ptr<asio::ip::udp::socket>>>
                    udp_map;
                ScopeExit udp_auto_exit([&udp_map] {
                    for (auto &[addr, data] : udp_map)
                    {
                        auto &[endpoint, udp_socket] = data;
                        if (udp_socket->is_open())
                        {
                            boost::system::error_code ec;
                            udp_socket->cancel(ec);
                            udp_socket->close(ec);
                        }
                    }
                });
                for (;;)
                {
                    *deadline = std::chrono::steady_clock::now() +
                                self->m_cfg.read_write_max_idle;
                    UdpPacket packet;
                    size_t packet_len;
                    bool is_packet_valid = packet.parse(payload, packet_len);
                    if (!is_packet_valid)
                    {
                        char buff[4096];
                        auto [ec, length] = co_await socket->async_read_some(
                            asio::buffer(buff, sizeof(buff)),
                            asio::as_tuple(asio::use_awaitable));
                        if (ec)
                        {
                            break;
                        }
                        payload.append(buff, length);
                        if (payload.length() > 1024 * 8)
                        {
                            co_await http301(socket);
                            break;
                        }
                    }
                    else
                    {
                        payload = payload.substr(packet_len);
                        TROJAN_DEBUG_LOG("query_addr: [{}]",
                                         packet.address.address);
                        if (!udp_map.contains(packet.address.address))
                        {
                            asio::ip::udp::resolver udp_resolver(
                                co_await asio::this_coro::executor);
                            auto [err, results] =
                                co_await udp_resolver.async_resolve(
                                    packet.address.address,
                                    std::to_string(packet.address.port),
                                    asio::as_tuple(asio::use_awaitable));
                            if (err || results.empty())
                            {
                                TROJAN_ERROR_LOG("resolve error: {}",
                                                 err.message());
                                break;
                            }
                            for (const auto &entry : results)
                            {
                                auto udp_socket =
                                    std::make_shared<asio::ip::udp::socket>(
                                        co_await asio::this_coro::executor);
                                auto protocol = entry.endpoint().protocol();
                                boost::system::error_code ec;
                                udp_socket->open(protocol, ec);
                                if (ec)
                                {
                                    TROJAN_ERROR_LOG("open: {}", ec.message());
                                    co_return;
                                }
                                udp_socket->bind(
                                    asio::ip::udp::endpoint(protocol, 0), ec);
                                if (ec)
                                {
                                    TROJAN_ERROR_LOG("bind: {}", ec.message());
                                    co_return;
                                }
                                udp_map[packet.address.address] =
                                    std::make_pair(entry.endpoint(),
                                                   udp_socket);
                                asio::co_spawn(
                                    co_await asio::this_coro::executor,
                                    self->udpToTcp(
                                        socket,
                                        udp_socket,
                                        deadline,
                                        self->m_cfg.read_write_max_idle),
                                    asio::detached);
                                break;
                            }
                        }
                        auto &[endpoint, udp_socket] =
                            udp_map[packet.address.address];
                        auto [ec, len] = co_await udp_socket->async_send_to(
                            boost::asio::buffer(packet.payload.c_str(),
                                                packet.payload.size()),
                            endpoint,
                            asio::as_tuple(asio::use_awaitable));
                        if (ec)
                        {
                            TROJAN_ERROR_LOG("async_send_to: {}", ec.message());
                            break;
                        }
                    }
                }
            };
            auto deadline =
                std::make_shared<std::chrono::steady_clock::time_point>(
                    std::chrono::steady_clock::now());
            co_await (
                recv_udp(this, std::move(req.payload), socket, deadline) ||
                watchdog(deadline));
        }
        co_return;
    }

    asio::awaitable<void> udpToTcp(auto tcp_socket,
                                   auto udp_socket,
                                   auto deadline,
                                   auto max_idle)
    {
        asio::ip::udp::endpoint udp_recv_endpoint;
        char buff[4096];
        for (;;)
        {
            *deadline = std::chrono::steady_clock::now() + max_idle;
            auto [ec, len] = co_await udp_socket->async_receive_from(
                asio::buffer(buff, sizeof(buff)),
                udp_recv_endpoint,
                asio::as_tuple(asio::use_awaitable));
            if (ec)
            {
                // TROJAN_INFO_LOG("async_receive_from: {}", ec.message());
                break;
            }
            auto data =
                UdpPacket::generate(udp_recv_endpoint, std::string(buff, len));
            if (auto [ec, len] =
                    co_await asio::async_write(*tcp_socket,
                                               asio::buffer(data),
                                               asio::as_tuple(
                                                   asio::use_awaitable));
                ec)
            {
                TROJAN_ERROR_LOG("{}", ec.message());
                break;
            }
        }
    }

    asio::awaitable<void> startSslsession(
        std::shared_ptr<asio::ssl::stream<asio::ip::tcp::socket>> socket,
        auto context)
    {
        auto result = co_await (
            socket->async_handshake(boost::asio::ssl::stream_base::server,
                                    asio::as_tuple(asio::use_awaitable)) ||
            timeout(m_cfg.timeout));
        if (result.index() == 1)
        {
            TROJAN_ERROR_LOG("async_handshake timeout");
            co_return;
        }
        auto [ec] = std::get<0>(result);
        if (ec)
        {
            TROJAN_ERROR_LOG("async_handshake: {}", ec.message());
            co_return;
        }
        asio::co_spawn(*context,
                       session(std::move(socket), context),
                       asio::detached);
    }

    asio::awaitable<void> dns()
    {
        m_timer = std::make_unique<asio::steady_timer>(
            *m_io_ctx_pool->getMainContext());
        for (;;)
        {
            m_timer->expires_after(std::chrono::minutes(2));
            auto [ec] = co_await m_timer->async_wait(
                asio::as_tuple(asio::use_awaitable));
            if (ec)
            {
                break;
            }
            std::unique_lock<std::mutex> l(m_mtx);
            std::erase_if(m_results, [&](auto &p) {
                auto &[addr, cached_result] = p;
                return std::chrono::steady_clock::now() -
                           cached_result.expire_time >
                       m_cfg.dns_cache_time;
            });
            l.unlock();
        }
    }

    Config m_cfg;
    asio::ip::tcp::endpoint m_ep;
    std::shared_ptr<IoCtxPool> m_io_ctx_pool;
    bool m_stop_ctx_pool;
    asio::ssl::context m_ssl_context{asio::ssl::context::tlsv13_server};
    std::unique_ptr<asio::ip::tcp::acceptor> m_acceptor;
    std::mutex m_mtx;
    std::unique_ptr<asio::steady_timer> m_timer;

    struct CachedResult
    {
        std::vector<asio::ip::tcp::resolver::results_type::value_type> results;
        std::chrono::steady_clock::time_point expire_time{
            std::chrono::steady_clock::now()};
    };

    std::unordered_map<std::string, CachedResult> m_results;
};

}  // namespace trojan

#endif  // _trojan_H_
