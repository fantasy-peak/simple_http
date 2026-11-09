#pragma once

// CSRF protection: the double-submit-cookie pattern.
//
// On a safe request (GET/HEAD/OPTIONS/TRACE) the middleware makes sure a random
// CSRF cookie exists and publishes its value on the request. The page/API can
// read it with `csrf_token(req)` (or read the non-HttpOnly cookie directly) and
// send it back in the configured header on unsafe requests. The middleware then
// compares the cookie and header in constant time; a mismatch is 403.
//
//     server.use(simple_http::middleware::csrf({
//         .cookie_name = "__Host-csrf",
//         .header_name = "x-csrf-token",
//     }));
//
// Double-submit is stateless and does not require a server-side session. It is
// weaker than a synchronizer token when subdomains are untrusted, so use
// `__Host-` cookies (Secure, Path=/, no Domain), keep the application on one
// origin, and combine it with SameSite cookies. For an HTML form, put the token
// in a hidden field and read it in the handler; the middleware can only inspect
// a header without consuming the request body, so form submissions that cannot
// set a header should validate the field themselves or use a small adapter.
//
// Exempt paths are for signature-verified webhooks. Do not exempt /login or
// any route that changes state just because it is unauthenticated: login CSRF is
// a real attack.

#include <chrono>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../core/http_method.h"
#include "../core/http_status.h"
#include "handler.h"
#include "session.h" // random_token / constant_time_equal

namespace simple_http {

struct CsrfToken {
    std::string value;
};

// The token for the current request, or empty when the middleware is not
// mounted. On a safe request it is the value of the CSRF cookie.
inline std::string_view csrf_token(const Request &req) {
    const auto *token = req.get_state<CsrfToken>();
    return token ? std::string_view{token->value} : std::string_view{};
}

struct CsrfConfig {
    std::string cookie_name{"csrf"};
    std::string header_name{"x-csrf-token"};
    std::string path{"/"};
    std::string same_site{"Lax"};
    std::chrono::seconds max_age{0}; // 0 = session cookie
    bool secure{true};
    // Exact paths, or prefixes ending in '/', that skip the check (webhooks).
    std::vector<std::string> exempt_paths;
};

namespace middleware {

inline Middleware csrf(CsrfConfig config = {}) {
    return [config = std::move(config)](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next,
                                        HandlerCompletion done) {
        const Method method = req->method();
        const bool safe =
            method == Method::Get || method == Method::Head || method == Method::Options || method == Method::Trace;

        bool exempt = false;
        for (const auto &prefix : config.exempt_paths) {
            if (req->path() == prefix || (ends_with(prefix, "/") && starts_with(req->path(), prefix))) {
                exempt = true;
                break;
            }
        }

        std::string token;
        if (const auto cookie = req->cookie(config.cookie_name); cookie && !cookie->empty()) {
            token = std::string{*cookie};
        } else if (safe) {
            token = random_token(32);
            res->set_cookie(config.cookie_name, token, config.max_age, config.path, "", config.secure,
                            /*http_only=*/false, config.same_site);
        }

        if (!token.empty()) {
            req->set_state(CsrfToken{token});
        }

        if (!safe && !exempt) {
            const auto supplied = req->header(config.header_name);
            if (token.empty() || !supplied || !constant_time_equal(token, *supplied)) {
                res->status(status::forbidden);
                res->content_type("text/plain; charset=utf-8");
                res->async_send("CSRF token invalid", finish_with(std::move(done)));
                return;
            }
        }

        next(std::move(req), std::move(res), ssl, std::move(done));
    };
}

} // namespace middleware
} // namespace simple_http
