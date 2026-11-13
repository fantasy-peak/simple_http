#pragma once

// Built-in middleware (C++17 token-only port).
//
// Each is a plain Middleware: it receives the request, the response and a `next`
// continuation ending at the route, plus the completion to invoke when the whole
// chain is done. Code before calling next is the before phase; the completion
// handed to next is the after phase.
//
//   server.use(simple_http::middleware::request_id());
//   server.use(simple_http::middleware::access_log());
//   server.group("/api", {simple_http::middleware::basic_auth("svc", "s3cret")}, ...);

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/ip/address.hpp>
#include <boost/asio/steady_timer.hpp>

#include "../core/base64.h"
#include "../core/compression.h"
#include "../core/content_encoding.h"
#include "../core/http_field.h"
#include "../core/http_method.h"
#include "../core/http_status.h"
#include "../core/logging.h"
#include "../core/result.h"
#include "../core/types.h"
#include "../core/url_path.h"
#include "handler.h"

namespace simple_http {
namespace middleware {

struct RequestId {
    std::string value;
};

struct ClientIp {
    std::string value;
};

namespace detail {

inline std::string random_hex(std::size_t bytes) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes * 2);
    for (std::size_t i = 0; i < bytes; ++i) {
        const auto b = static_cast<std::uint8_t>(rng() & 0xFF);
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0xF]);
    }
    return out;
}

} // namespace detail

// Access log: one structured record per request, emitted after the chain
// completed so it reports the real answer including the router's built-in
// 404/405.
inline Middleware access_log() {
    return [](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next, HandlerCompletion done) {
#if SIMPLE_HTTP_ENABLE_LOG
        const auto start = std::chrono::steady_clock::now();
#endif
        next(req, res, ssl,
             [req, res,
#if SIMPLE_HTTP_ENABLE_LOG
              start,
#endif
              done = std::move(done)]() mutable {
#if SIMPLE_HTTP_ENABLE_LOG
                 const auto elapsed_us =
                     std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start)
                         .count();
                 const int status = res->status();
                 const std::string peer = req->peer_address();

                 std::vector<Attr> attrs;
                 attrs.reserve(12);
                 attrs.emplace_back("method", req->method_token());
                 attrs.emplace_back("path", req->path());
                 if (!req->query().empty()) {
                     attrs.emplace_back("query", req->query());
                 }
                 attrs.emplace_back("status", static_cast<std::int64_t>(status));
                 attrs.emplace_back("duration_ms", static_cast<double>(elapsed_us) / 1000.0);
                 attrs.emplace_back("peer", peer);
                 attrs.emplace_back("http_version", to_string(req->version()));
                 if (const auto *id = req->get_state<RequestId>()) {
                     attrs.emplace_back("request_id", id->value);
                 }
                 if (const auto *ip = req->get_state<ClientIp>()) {
                     attrs.emplace_back("client_ip", ip->value);
                 }
                 if (const auto *fields = req->get_state<LogFields>()) {
                     for (const auto &[key, value] : fields->kv) {
                         attrs.emplace_back(key, value);
                     }
                 }
                 const LogLevel level =
                     status >= 500 ? LogLevel::Error : (status >= 400 ? LogLevel::Warn : LogLevel::Info);
                 log_with_attrs(level, "access", source_location::current(), "request", attrs);
#endif
                 std::move(done)();
             });
    };
}

// Request id: adopts a client-provided X-Request-Id, or generates one, echoes it
// in the response and publishes it as RequestId on the request.
inline Middleware request_id() {
    return [](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next, HandlerCompletion done) {
        std::string id;
        if (const auto existing = req->header(field::x_request_id); existing && !existing->empty()) {
            id.assign(*existing);
        } else {
            id = detail::random_hex(16);
        }
        res->header(field::x_request_id, id);
        req->set_state(RequestId{std::move(id)});
        next(std::move(req), std::move(res), ssl, std::move(done));
    };
}

// Recovery: catches a synchronous exception from the rest of the chain and
// answers 500 (or closes a started response) instead of letting the connection
// die mid-request. Exceptions thrown later, from an asynchronous continuation,
// are outside a callback middleware's reach; handlers already convert their own
// exceptions to 500 (invoke_handler).
inline Middleware recovery() {
    return [](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next, HandlerCompletion done) {
        const ResponsePtr fallback = res;
        const RequestPtr req_keep = req;
        auto completion = std::make_shared<HandlerCompletion>(std::move(done));
        auto completed = std::make_shared<bool>(false);
        // Exactly one completion, whatever the downstream chain does. It may
        // complete synchronously and then throw; the catch block must then not
        // invoke the (already moved-from) std::function — that used to escape
        // as bad_function_call out of the io_context.
        auto complete_once = [completion, completed]() {
            if (std::exchange(*completed, true))
                return;
            auto finish = std::move(*completion);
            std::move(finish)();
        };
        try {
            next(std::move(req), std::move(res), ssl, complete_once);
        } catch (const std::exception &e) {
            if (*completed)
                return; // downstream already delivered its completion
            if (req_keep && req_keep->cancelled()) {
                complete_once();
                return;
            }
            SIMPLE_HTTP_ERROR_LOG("middleware chain threw: {}", e.what());
            auto finish = finish_with(HandlerCompletion{complete_once});
            if (fallback->started()) {
                fallback->async_close(std::move(finish));
            } else {
                fallback->status(status::internal_server_error);
                fallback->async_send("", std::move(finish));
            }
        } catch (...) {
            if (*completed)
                return;
            SIMPLE_HTTP_ERROR_LOG("middleware chain threw: non-std exception");
            auto finish = finish_with(HandlerCompletion{complete_once});
            if (fallback->started()) {
                fallback->async_close(std::move(finish));
            } else {
                fallback->status(status::internal_server_error);
                fallback->async_send("", std::move(finish));
            }
        }
    };
}

namespace basic_auth_detail {

inline bool constant_time_equal(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
}

} // namespace basic_auth_detail

// HTTP Basic authentication (RFC 7617).
inline Middleware basic_auth(std::string username, std::string password) {
    const std::string expected = username + ":" + password;
    return [expected = std::move(expected)](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next,
                                            HandlerCompletion done) {
        bool ok = false;
        if (const auto auth = req->header(field::authorization)) {
            constexpr std::string_view scheme = "Basic ";
            if (auth->size() > scheme.size() && iequals_ci(auth->substr(0, scheme.size()), scheme)) {
                const std::string decoded = base64_decode(std::string{auth->substr(scheme.size())});
                ok = basic_auth_detail::constant_time_equal(decoded, expected);
            }
        }
        if (!ok) {
            res->header(field::www_authenticate, "Basic realm=\"restricted\"");
            res->status(status::unauthorized);
            res->async_send("unauthorized", finish_with(std::move(done)));
            return;
        }
        next(std::move(req), std::move(res), ssl, std::move(done));
    };
}

namespace real_ip_detail {

inline bool ip_covered(std::string_view entry, std::string_view ip) {
    const std::size_t slash = entry.find('/');
    if (slash == std::string_view::npos) {
        return iequals_ci(entry, ip);
    }
    const std::string address_part{entry.substr(0, slash)};
    // An empty prefix ("10.0.0.0/") parses no digits and would otherwise keep
    // the default 0, silently trusting every IPv4 peer. An explicit "/0" is
    // still honoured.
    if (slash + 1 == entry.size()) {
        return false;
    }
    unsigned prefix = 0;
    for (std::size_t i = slash + 1; i < entry.size(); ++i) {
        if (entry[i] < '0' || entry[i] > '9') {
            return false;
        }
        // 128 is the longest IPv6 prefix; rejecting here also keeps the
        // accumulate from wrapping a huge configured value back into a small
        // (and therefore over-trusting) prefix such as /0.
        if (prefix > 128) {
            return false;
        }
        prefix = prefix * 10 + static_cast<unsigned>(entry[i] - '0');
    }
    if (prefix > 128) {
        return false; // the last digit may have pushed it past the bound
    }
    // Separate error codes: reusing one lets a malformed network part stay at
    // its default 0.0.0.0 when the host parses successfully, silently treating
    // "junk/8" as 0.0.0.0/8 and trusting 0.x.x.x peers.
    error_code net_ec;
    const auto net = asio::ip::make_address(address_part, net_ec);
    if (net_ec) {
        return false;
    }
    error_code host_ec;
    const auto host = asio::ip::make_address(std::string{ip}, host_ec);
    if (host_ec) {
        return false;
    }
    if (net.is_v4() != host.is_v4()) {
        return false;
    }
    std::array<unsigned char, 16> bytes{};
    std::array<unsigned char, 16> want{};
    const unsigned max_bits = net.is_v4() ? 32 : 128;
    if (net.is_v4()) {
        const auto n = net.to_v4().to_bytes();
        const auto h = host.to_v4().to_bytes();
        for (std::size_t i = 0; i < 4; ++i) {
            bytes[i] = n[i];
            want[i] = h[i];
        }
    } else {
        bytes = net.to_v6().to_bytes();
        want = host.to_v6().to_bytes();
    }
    if (prefix > max_bits) {
        return false;
    }
    unsigned bits = prefix;
    for (std::size_t i = 0; i < bytes.size() && bits > 0; ++i) {
        const unsigned take = std::min(bits, 8u);
        const auto mask = static_cast<unsigned char>(0xFFu << (8 - take));
        if ((bytes[i] & mask) != (want[i] & mask)) {
            return false;
        }
        bits -= take;
    }
    return true;
}

inline bool peer_is_trusted(std::string_view peer, const std::vector<std::string> &trusted) {
    for (const auto &entry : trusted) {
        if (ip_covered(entry, peer)) {
            return true;
        }
    }
    return false;
}

} // namespace real_ip_detail

// real_ip: resolves the client's real address behind a trusted proxy. The
// forwarded fields are consulted only when the direct peer is trusted.
inline Middleware real_ip(std::vector<std::string> trusted_proxies = {}) {
    return [trusted = std::move(trusted_proxies)](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next,
                                                  HandlerCompletion done) {
        if (real_ip_detail::peer_is_trusted(req->peer_address(), trusted)) {
            // The header value becomes a trusted client identity downstream, so
            // it has to parse as an address; a proxy that forwarded junk (or an
            // attacker controlling a trusted hop) must not mint "evil" as an IP.
            bool have_real = false;
            if (const auto x_real = req->header("x-real-ip"); x_real && !x_real->empty()) {
                error_code ip_ec;
                asio::ip::make_address(std::string{*x_real}, ip_ec);
                if (!ip_ec) {
                    req->set_state(ClientIp{std::string{*x_real}});
                    have_real = true;
                }
            }
            if (!have_real) {
                if (const auto xff = req->header("x-forwarded-for")) {
                    std::vector<std::string_view> hops;
                    std::size_t pos = 0;
                    while (pos <= xff->size()) {
                        const std::size_t comma = xff->find(',', pos);
                        std::string_view hop =
                            xff->substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
                        while (!hop.empty() && hop.front() == ' ') {
                            hop.remove_prefix(1);
                        }
                        while (!hop.empty() && hop.back() == ' ') {
                            hop.remove_suffix(1);
                        }
                        if (!hop.empty()) {
                            hops.push_back(hop);
                        }
                        if (comma == std::string_view::npos) {
                            break;
                        }
                        pos = comma + 1;
                    }
                    if (!hops.empty()) {
                        std::string_view chosen = hops.front();
                        for (auto it = hops.rbegin(); it != hops.rend(); ++it) {
                            bool is_trusted = false;
                            for (const auto &t : trusted) {
                                if (real_ip_detail::ip_covered(t, *it)) {
                                    is_trusted = true;
                                    break;
                                }
                            }
                            if (!is_trusted) {
                                chosen = *it;
                                break;
                            }
                        }
                        error_code chosen_ec;
                        asio::ip::make_address(std::string{chosen}, chosen_ec);
                        if (!chosen_ec) {
                            req->set_state(ClientIp{std::string{chosen}});
                        }
                    }
                }
            }
        }
        next(std::move(req), std::move(res), ssl, std::move(done));
    };
}

inline std::string reencode_clean_path(std::string_view normalized) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(normalized.size());
    for (const unsigned char c : normalized) {
        if (c == '%' || c == '?' || c == '#' || c == ' ' || c < 0x21 || c == 0x7F) {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

inline Middleware clean_path() {
    return [](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next, HandlerCompletion done) {
        const std::string_view raw = req->target();
        const std::size_t qmark = raw.find('?');
        const std::string_view path = raw.substr(0, qmark);
        const std::string_view query = qmark == std::string_view::npos ? std::string_view{} : raw.substr(qmark);
        PathError err = PathError::None;
        auto normalized = decode_and_normalize(path, err);
        if (!normalized) {
            res->status(status::bad_request);
            res->async_send("", finish_with(std::move(done)));
            return;
        }
        if (*normalized != path) {
            req->set_target(reencode_clean_path(*normalized) + std::string{query});
        }
        next(std::move(req), std::move(res), ssl, std::move(done));
    };
}

inline Middleware strip_prefix(std::string prefix) {
    return [prefix = std::move(prefix)](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next,
                                        HandlerCompletion done) {
        std::string target;
        const std::string_view path = req->path();
        if (path == prefix) {
            target = "/";
        } else if (path.size() > prefix.size() && starts_with(path, prefix) && path[prefix.size()] == '/') {
            target.assign(path.substr(prefix.size()));
        } else {
            next(std::move(req), std::move(res), ssl, std::move(done));
            return;
        }
        if (!req->query().empty()) {
            target.push_back('?');
            target.append(req->query());
        }
        req->set_target(std::move(target));
        next(std::move(req), std::move(res), ssl, std::move(done));
    };
}

inline Middleware redirect_slashes() {
    return [](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next, HandlerCompletion done) {
        const std::string_view path = req->path();
        if (path.size() > 1 && path.back() == '/') {
            std::string location{path.substr(0, path.size() - 1)};
            if (!req->query().empty()) {
                location.push_back('?');
                location.append(req->query());
            }
            res->redirect(std::move(location), status::moved_permanently);
            res->async_send("", finish_with(std::move(done)));
            return;
        }
        next(std::move(req), std::move(res), ssl, std::move(done));
    };
}

// Rejects WebSocket upgrades whose Origin is not on the allow list.
inline Middleware ws_origin(std::vector<std::string> allowed_origins, bool allow_missing = false) {
    return [allowed = std::move(allowed_origins), allow_missing](RequestPtr req, ResponsePtr res, SslHandle ssl,
                                                                 Next next, HandlerCompletion done) {
        const auto upgrade = req->header(field::upgrade);
        const bool is_h1_ws =
            upgrade && icontains_ci(*upgrade, "websocket") && req->header("sec-websocket-key").has_value();
        const bool is_h2_ws = req->method() == Method::Connect;
        if (!is_h1_ws && !is_h2_ws) {
            next(std::move(req), std::move(res), ssl, std::move(done));
            return;
        }
        const auto origin = req->header(field::origin);
        if (!origin) {
            if (allow_missing) {
                next(std::move(req), std::move(res), ssl, std::move(done));
                return;
            }
            res->status(status::forbidden);
            res->async_send("origin required", finish_with(std::move(done)));
            return;
        }
        std::string_view o = *origin;
        while (!o.empty() && o.back() == '/') {
            o.remove_suffix(1);
        }
        bool ok = false;
        for (const std::string &entry : allowed) {
            if (entry == "*" || iequals_ci(o, entry)) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            res->status(status::forbidden);
            res->async_send("origin not allowed", finish_with(std::move(done)));
            return;
        }
        next(std::move(req), std::move(res), ssl, std::move(done));
    };
}

// Per-request deadline, streaming-aware: the handler must start responding
// within `deadline`; after that it must not go silent for `deadline`. The poll
// watches ResponseWriter::activity(), so a streaming response that keeps
// producing is never killed.
inline Middleware timeout(std::chrono::milliseconds deadline) {
    return [deadline](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next, HandlerCompletion done) {
        const auto upgrade = req->header(field::upgrade);
        const bool ws =
            (upgrade && icontains_ci(*upgrade, "websocket") && req->header("sec-websocket-key").has_value()) ||
            req->method() == Method::Connect;
        if (ws) {
            next(std::move(req), std::move(res), ssl, std::move(done));
            return;
        }

        using clock = std::chrono::steady_clock;
        auto tick = deadline / 4;
        if (tick < std::chrono::milliseconds(10))
            tick = std::chrono::milliseconds(10);
        if (tick > std::chrono::seconds(1))
            tick = std::chrono::seconds(1);
        if (tick > deadline && deadline > std::chrono::milliseconds(0))
            tick = deadline;

        auto timer = std::make_shared<asio::steady_timer>(req->executor());
        auto last_change = std::make_shared<clock::time_point>(clock::now());
        auto last_seq = std::make_shared<std::uint64_t>(res->activity());
        auto completion = std::make_shared<HandlerCompletion>(std::move(done));
        auto finished = std::make_shared<bool>(false);

        auto finish_chain = [completion, finished, timer]() mutable {
            if (*finished) {
                return;
            }
            *finished = true;
            timer->cancel();
            auto finish = std::move(*completion);
            std::move(finish)();
        };

        auto poll = std::make_shared<std::function<void()>>();
        // The poll loop re-arms itself from the timer completion. Capturing the
        // shared_ptr inside the function it owns would make every timeout()
        // installation a permanent cycle holding the request, response,
        // completion and timer. The function holds a weak self-reference; the
        // pending timer handler holds the strong reference that keeps it alive.
        std::weak_ptr<std::function<void()>> weak_poll = poll;
        *poll = [timer, req, res, last_change, last_seq, deadline, tick, finished, completion, weak_poll] {
            auto keep = weak_poll.lock();
            if (!keep) {
                return;
            }
            timer->expires_after(tick);
            timer->async_wait(
                [timer, req, res, last_change, last_seq, deadline, tick, finished, completion, keep](error_code ec) {
                    if (ec || *finished) {
                        return;
                    }
                    const std::uint64_t seq = res->activity();
                    if (seq != *last_seq) {
                        *last_seq = seq;
                        *last_change = clock::now();
                        (*keep)();
                        return;
                    }
                    if (clock::now() - *last_change >= deadline) {
                        *finished = true;
                        req->cancel();
                        auto finish = finish_with(std::move(*completion));
                        if (!res->started()) {
                            res->status(status::gateway_timeout).content_type("text/plain");
                            res->async_send("Gateway Timeout", std::move(finish));
                        } else {
                            res->async_close(std::move(finish));
                        }
                        return;
                    }
                    (*keep)();
                });
        };
        (*poll)();

        try {
            next(std::move(req), std::move(res), ssl, finish_chain);
        } catch (...) {
            finish_chain();
        }
    };
}

// Request-body decompression: nothing is decoded unless this middleware is
// mounted. Only a single gzip/br coding is supported; a list is refused with
// 415 because guessing the layer order corrupts bytes.
inline Middleware decompress_request(RequestDecompressionConfig config = {},
                                     std::function<bool(const Request &)> skip = {}) {
    return [config = std::move(config), skip = std::move(skip)](RequestPtr req, ResponsePtr res, SslHandle ssl,
                                                                Next next, HandlerCompletion done) {
        bool covered = true;
        const std::string_view path = req->path();
        for (const std::string &entry : config.excluded_paths) {
            if (entry.empty()) {
                continue;
            }
            if (entry.back() == '/') {
                // A directory entry must stop at a path-segment boundary: with a
                // bare prefix, "/api/" also excluded "/apix". Match the bare
                // prefix and its children, but nothing that merely shares the
                // characters.
                const std::string_view prefix{entry.data(), entry.size() - 1};
                if (path == prefix || starts_with(path, entry)) {
                    covered = false;
                    break;
                }
            } else if (path == entry) {
                covered = false;
                break;
            }
        }
        if (covered && skip && skip(*req)) {
            covered = false;
        }
        if (!covered) {
            next(std::move(req), std::move(res), ssl, std::move(done));
            return;
        }

        const auto values = req->headers().get_all(field::content_encoding);
        if (values.empty()) {
            next(std::move(req), std::move(res), ssl, std::move(done));
            return;
        }
        const ContentCoding coding =
            values.size() == 1 ? parse_content_coding(values.front()) : ContentCoding::Unsupported;
        if (coding == ContentCoding::Identity) {
            next(std::move(req), std::move(res), ssl, std::move(done));
            return;
        }
        const bool allowed =
            (coding == ContentCoding::Gzip && config.gzip) || (coding == ContentCoding::Brotli && config.brotli);
        if (!allowed) {
            std::string accept;
            if (config.gzip) {
                accept = "gzip";
            }
            if (config.brotli) {
                if (!accept.empty()) {
                    accept += ", ";
                }
                accept += "br";
            }
            res->status(status::unsupported_media_type);
            if (!accept.empty()) {
                res->header(field::accept_encoding, std::move(accept));
            }
            res->async_send("unsupported content-encoding", finish_with(std::move(done)));
            return;
        }

        auto decoder = make_decoder(coding == ContentCoding::Gzip ? kEncodingGzip : kEncodingBrotli);
        if (!decoder || !req->body().enable_decoding(std::move(decoder), config.max_decoded_bytes)) {
            SIMPLE_HTTP_ERROR_LOG("request decompression: body was already read before the middleware");
            res->status(status::internal_server_error);
            res->async_send("request body already consumed", finish_with(std::move(done)));
            return;
        }
        req->mutable_headers().erase(field::content_encoding);
        req->mutable_headers().erase(field::content_length);
        req->mutable_headers().erase("content-md5");
        req->mutable_headers().erase("content-digest");
        next(std::move(req), std::move(res), ssl, std::move(done));
    };
}

// Response compression: mounting this enables compression for the scope it is
// attached to. The response writer reads the policy from Request state.
inline Middleware compress(CompressionConfig config = {}, std::function<bool(const Request &)> skip = {}) {
    config.enabled = true;
    return [config = std::move(config), skip = std::move(skip)](RequestPtr req, ResponsePtr res, SslHandle ssl,
                                                                Next next, HandlerCompletion done) {
        bool covered = true;
        const std::string_view path = req->path();
        for (const std::string &entry : config.excluded_paths) {
            if (entry.empty()) {
                continue;
            }
            if (entry.back() == '/') {
                // A directory entry must stop at a path-segment boundary: with a
                // bare prefix, "/api/" also excluded "/apix". Match the bare
                // prefix and its children, but nothing that merely shares the
                // characters.
                const std::string_view prefix{entry.data(), entry.size() - 1};
                if (path == prefix || starts_with(path, entry)) {
                    covered = false;
                    break;
                }
            } else if (path == entry) {
                covered = false;
                break;
            }
        }
        if (covered && skip && skip(*req)) {
            covered = false;
        }
        if (covered) {
            req->set_state(config);
        }
        next(std::move(req), std::move(res), ssl, std::move(done));
    };
}

} // namespace middleware
} // namespace simple_http
