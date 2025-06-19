#include <cstdio>
#include <ostream>
#include <string>
#include <iostream>
#include <cstdlib>
#include <iostream>
#include <string>

#include "simple_http.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <nlohmann/json.hpp>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

bool set_log_level(auto logger, const std::string &level)
{
    static const std::unordered_map<std::string, spdlog::level::level_enum>
        level_map = {{"trace", spdlog::level::trace},
                     {"debug", spdlog::level::debug},
                     {"info", spdlog::level::info},
                     {"warn", spdlog::level::warn},
                     {"error", spdlog::level::err},
                     {"critical", spdlog::level::critical},
                     {"off", spdlog::level::off}};

    auto it = level_map.find(level);
    if (it != level_map.end())
    {
        logger->set_level(it->second);
        return true;
    }
    return false;
}

trojan::Config load_config_from_env()
{
    trojan::Config cfg;

    const char *ip = std::getenv("TROJAN_IP");
    const char *passwd = std::getenv("TROJAN_PASSWD");
    const char *path = std::getenv("TROJAN_PATH");
    const char *ssl_crt = std::getenv("TROJAN_SSL_CRT");
    const char *ssl_key = std::getenv("TROJAN_SSL_KEY");
    const char *port_str = std::getenv("TROJAN_PORT");
    const char *worker_str = std::getenv("TROJAN_WORKERS");

    cfg.ip = ip ? ip : "0.0.0.0";
    cfg.passwd = passwd ? passwd : "";
    cfg.path = path ? path : "/";
    cfg.ssl_crt = ssl_crt ? ssl_crt : "";
    cfg.ssl_key = ssl_key ? ssl_key : "";

    cfg.port = port_str ? std::stoi(port_str) : 5555;
    cfg.worker_num = worker_str ? std::stoi(worker_str) : 4;

    return cfg;
}

namespace trojan
{
std::ostream &operator<<(std::ostream &os, const Config &cfg)
{
    os << "Trojan Config:\n";
    os << "  IP         : " << cfg.ip << "\n";
    os << "  Port       : " << cfg.port << "\n";
    os << "  Workers    : " << cfg.worker_num << "\n";
    os << "  Password   : " << cfg.passwd << "\n";  // 注意：敏感信息
    os << "  Path       : " << cfg.path << "\n";
    os << "  SSL Cert   : " << cfg.ssl_crt << "\n";
    os << "  SSL Key    : " << cfg.ssl_key << "\n";
    return os;
}
}  // namespace trojan

asio::awaitable<void> start()
{
    trojan::Config cfg = load_config_from_env();
    std::cout << cfg << std::endl;

    trojan::Server hs(cfg);
    trojan::LOG_CB =
        [](trojan::LogLevel level, auto file, auto line, std::string msg) {
            if (level == trojan::LogLevel::Error)
                spdlog::error(" [{}:{}] {}", file, line, msg);
            else if (level == trojan::LogLevel::Info)
            {
                spdlog::info(" [{}:{}] {}", file, line, msg);
            }
            else if (level == trojan::LogLevel::Debug)
            {
                spdlog::debug(" [{}:{}] {}", file, line, msg);
            }
        };

    co_await hs.start();
}

int main()
{
    size_t queue_size = 8192;
    size_t thread_count = 1;
    spdlog::init_thread_pool(queue_size, thread_count);

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    std::vector<spdlog::sink_ptr> sinks{console_sink};

    auto logger_ = std::make_shared<spdlog::async_logger>(
        "async_logger",
        sinks.begin(),
        sinks.end(),
        spdlog::thread_pool(),
        spdlog::async_overflow_policy::block);

    spdlog::set_default_logger(logger_);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%t] [%^%l%$] %v");
    spdlog::set_level(spdlog::level::info);

    trojan::IoCtxPool pool{1};
    pool.start();
    asio::co_spawn(pool.getIoContext(), start(), asio::detached);

    // curl -X POST http://localhost:7777/loglevel -H "Content-Type:
    // application/json" -d '{"level": "debug"}'
    const char *port_str = std::getenv("TROJAN_LOG_PORT");
    int port = port_str ? std::stoi(port_str) : 7777;
    asio::ip::tcp::acceptor acceptor(
        pool.getIoContext(),
        asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port));

    for (;;)
    {
        try
        {
            asio::ip::tcp::socket socket(pool.getIoContext());
            acceptor.accept(socket);

            beast::flat_buffer buffer;
            http::request<http::string_body> req;
            http::read(socket, buffer, req);

            http::response<http::string_body> res;
            if (req.method() == http::verb::post && req.target() == "/loglevel")
            {
                try
                {
                    auto j = nlohmann::json::parse(req.body());
                    std::string level = j.at("level").get<std::string>();
                    if (set_log_level(logger_, level))
                    {
                        res.result(http::status::ok);
                        res.body() = "Log level set to: " + level;
                    }
                    else
                    {
                        res.result(http::status::bad_request);
                        res.body() = "Invalid log level";
                    }
                }
                catch (...)
                {
                    res.result(http::status::bad_request);
                    res.body() = "Invalid JSON";
                }
            }
            else
            {
                res.result(http::status::not_found);
                res.body() = "Not found";
            }

            res.prepare_payload();

            http::write(socket, res);
        }
        catch (std::exception const &e)
        {
            TROJAN_ERROR_LOG("Error: {}", e.what());
        }
    }

    return 0;
}
