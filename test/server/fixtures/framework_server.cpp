// Reference server for the framework-layer differential test against Go.
//
// It exposes every new handler-layer feature under deterministic routes so
// test/differential/framework_diff.py can send the same request to this server and
// to test/differential/go/framework_server.go and compare the observable behavior.
//
//   xmake build fixture_framework_server
//   ./build/linux/x86_64/release/framework_server 7798

#include <atomic>
#include <csignal>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <thread>

#include "simple_http.h"

namespace asio = boost::asio;
using namespace simple_http;

struct AppState {
    std::string name{"from-state"};
};

struct UserParams {
    int id{};
};

struct CreateUser {
    std::string name;
    int age{};
};

struct ListQuery {
    std::string q;
    int page{};
};

namespace simple_http {
template <> struct Validator<CreateUser> {
    static std::expected<void, std::vector<FieldError>> validate(const CreateUser &value) {
        std::vector<FieldError> errors;
        push_if(errors, required_field("name", !value.name.empty()));
        push_if(errors, range("age", value.age, 1, 150));
        return errors.empty() ? std::expected<void, std::vector<FieldError>>{} : std::unexpected(std::move(errors));
    }
};
} // namespace simple_http

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

std::string read_file(const char *path) {
    if (!path) {
        return {};
    }
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

} // namespace

int main(int argc, char **argv) {
    const auto port = static_cast<std::uint16_t>(argc > 1 ? std::stoi(argv[1]) : 7798);
    const std::string rsa_public_pem = read_file(argc > 2 ? argv[2] : nullptr);
    const std::string ec_public_pem = read_file(argc > 3 ? argv[3] : nullptr);

    set_log_sink(make_stderr_sink(LogLevel::Warn));

    ServerConfig cfg;
    cfg.listen = InetAddress{"127.0.0.1", port, false};
    cfg.worker_threads = 2;
    Server server{cfg};
    server.state(std::make_shared<AppState>());

    // --- application state / extractors / validation -------------------------
    server.route({Method::Get}, "/state", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        const AppState *state = req->app_state<AppState>();
        co_await res->status(200).send(state ? state->name : "missing");
    });

    server.route({Method::Get}, "/users/{id}",
                 extract<Path<UserParams>, State<AppState>>([](const UserParams &params, AppState *state, RequestPtr,
                                                               ResponsePtr res) -> asio::awaitable<void> {
                     co_await res->status(200).send(std::to_string(params.id) + ":" + state->name);
                 }));

    server.route(
        {Method::Get}, "/list",
        extract<Query<ListQuery>>([](const ListQuery &query, RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
            co_await res->status(200).send(query.q + ":" + std::to_string(query.page));
        }));

    server.route({Method::Post}, "/json",
                 extract<Json<CreateUser>>([](CreateUser value, RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                     co_await res->status(201).send(value.name + ":" + std::to_string(value.age));
                 }));

    server.route(
        {Method::Post}, "/valid",
        extract<Valid<Json<CreateUser>>>([](CreateUser value, RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
            co_await res->status(201).send(value.name);
        }));

    server.route({Method::Post}, "/form",
                 extract<Form<CreateUser>>([](CreateUser value, RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                     co_await res->status(201).send(value.name + ":" + std::to_string(value.age));
                 }));

    server.route({Method::Get}, "/header",
                 extract<Header<int, "x-page">, Cookie<std::string, "sid">>(
                     [](int page, std::string sid, RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                         co_await res->status(200).send(std::to_string(page) + ":" + sid);
                     }));

    // --- catch-all / exact / template precedence -----------------------------
    server.route({Method::Get}, "/files/static", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).send("static");
    });
    server.route({Method::Get}, "/files/{id}", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).send("id=" + std::string{req->param("id").value_or("")});
    });
    server.route({Method::Get}, "/files/{*rest}", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).send("rest=" + std::string{req->param("rest").value_or("")});
    });

    // --- endpoints used by the C++/Go client differential probe --------------
    server.route({Method::Get, Method::Post}, "/world",
                 [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(200).send("world"); });
    server.route(any_methods, "/echo", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        auto body = co_await req->body().read_all();
        co_await res->status(200).send(body ? *body : std::string{});
    });
    server.route({Method::Post}, "/mp", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        auto parts = co_await read_multipart_body(*req);
        if (!parts) {
            co_await res->status(400).send("not multipart");
            co_return;
        }
        std::string out;
        for (const auto &part : *parts) {
            if (!out.empty())
                out.push_back('|');
            if (part.filename.empty()) {
                out += "field:" + part.name + "=" + part.data;
            } else {
                out += "file:" + part.name + ":" + part.filename + ":" + part.content_type + ":" +
                       std::to_string(part.data.size());
            }
        }
        co_await res->status(200).send(std::move(out));
    });
    server.route({Method::Post}, "/expect-ok", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->send_continue();
        auto body = co_await req->body().read_all();
        co_await res->status(200).send("got " + std::to_string(body ? body->size() : 0));
    });
    server.route({Method::Post}, "/expect-reject",
                 [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(417).send("no"); });
    server.route({Method::Post}, "/trailer-echo", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        (void)co_await req->body().read_all();
        const auto check = req->trailers().get("x-check");
        co_await res->status(200).send(check ? std::string{*check} : std::string{"missing"});
    });
    auto retry_status_hits = std::make_shared<std::atomic<int>>(0);
    server.route({Method::Get}, "/retry-status",
                 [retry_status_hits](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                     if (retry_status_hits->fetch_add(1) < 2) {
                         res->header("retry-after", "0");
                         co_await res->status(429).send("retry");
                     } else {
                         co_await res->status(200).send("ok");
                     }
                 });
    server.route({Method::Get}, "/jwks", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).content_type("application/json").send(R"({"keys":[]})");
    });

    // --- host routing --------------------------------------------------------
    server.route({Method::Get}, "/who",
                 [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(200).send("root"); });
    server.host("api.example.com", [](Router &api) {
        api.route({Method::Get}, "/who",
                  [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(200).send("api"); });
    });
    server.host("*.example.com", [](Router &sub) {
        sub.route({Method::Get}, "/who",
                  [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(200).send("sub"); });
    });
    server.host("api.example.com:8080", [](Router &port_router) {
        port_router.route({Method::Get}, "/who", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
            co_await res->status(200).send("port");
        });
    });

    // --- sessions ------------------------------------------------------------
    auto sessions = std::make_shared<MemorySessionStore>();
    auto session_mw = middleware::session(
        sessions, SessionConfig{.cookie_name = "sid", .same_site = "Lax", .secure = false, .http_only = true});
    server.route({Method::Post}, "/session/set", {session_mw},
                 [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
                     if (auto *s = session(*req)) {
                         s->set("uid", "42");
                     }
                     co_await res->status(204).send("");
                 });
    server.route({Method::Get}, "/session/get", {session_mw},
                 [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
                     auto *s = session(*req);
                     const auto uid = s ? s->get("uid") : std::nullopt;
                     co_await res->status(200).send(uid ? std::string{*uid} : "none");
                 });
    server.route({Method::Get}, "/session/rotate", {session_mw},
                 [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
                     if (auto *s = session(*req)) {
                         s->regenerate_id();
                         s->set("uid", "99");
                     }
                     co_await res->status(204).send("");
                 });

    // --- CSRF ---------------------------------------------------------------
    auto csrf_mw = middleware::csrf(CsrfConfig{
        .cookie_name = "csrf",
        .header_name = "x-csrf-token",
        .secure = false,
        .exempt_paths = {"/csrf/exempt"},
    });
    server.route({Method::Get}, "/csrf/token", {csrf_mw}, [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).send(std::string{csrf_token(*req)});
    });
    server.route({Method::Post}, "/csrf/submit", {csrf_mw},
                 [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(204).send(""); });
    server.route({Method::Post}, "/csrf/exempt", {csrf_mw},
                 [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(204).send(""); });

    // --- JWT ----------------------------------------------------------------
    auto register_jwt_routes = [&server](const std::string &prefix, Middleware jwt_mw) {
        server.route({Method::Get}, prefix + "/me", {jwt_mw},
                     [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
                         const Principal *identity = principal(*req);
                         if (!identity) {
                             co_await res->status(500).send("no principal");
                             co_return;
                         }
                         std::string scopes;
                         for (const auto &scope : identity->scopes) {
                             scopes += (scopes.empty() ? "" : ",") + scope;
                         }
                         co_await res->status(200).send(identity->subject + ":" + scopes);
                     });
        server.route({Method::Get}, prefix + "/write", {std::move(jwt_mw), middleware::require_scope("write")},
                     [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(204).send(""); });
    };

    register_jwt_routes("/jwt", middleware::jwt(JwtConfig{
                                    .algorithms = {"HS256"},
                                    .secret = "secret",
                                    .issuer = "iss",
                                    .audience = "aud",
                                }));
    register_jwt_routes("/jwt/rs256", middleware::jwt(JwtConfig{
                                          .algorithms = {"RS256"},
                                          .public_key_pem = rsa_public_pem,
                                          .issuer = "iss",
                                          .audience = "aud",
                                      }));
    register_jwt_routes("/jwt/es256", middleware::jwt(JwtConfig{
                                          .algorithms = {"ES256"},
                                          .public_key_pem = ec_public_pem,
                                          .issuer = "iss",
                                          .audience = "aud",
                                      }));

    auto started = server.start();
    if (!started) {
        for (const auto &failure : started.error().failures) {
            std::println(stderr, "{}: {}", failure.name, failure.error.message());
        }
        return 1;
    }
    std::println("framework reference server on 127.0.0.1:{}", port);
    std::fflush(stdout);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    while (g_stop == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    server.stop();
    return 0;
}
